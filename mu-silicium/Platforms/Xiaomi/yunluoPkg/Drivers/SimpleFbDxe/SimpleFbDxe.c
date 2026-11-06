/** @file
  Simple framebuffer GOP driver for the Xiaomi Redmi Pad (yunluo, MT6789).

  LK hands over a live MIPI panel scanout buffer at 0xFE2C3000: 2000x1200
  visible, 2048 pixels per line, a8b8g8r8 in memory order.  Three queued
  buffers are allocated for the display engine and the OVL layers are already
  enabled, but still point at LK's splash buffer.  This driver

    * publishes the GOP at the pixel format the OVL actually scans out,
    * keeps the 2048-pixel pitch separate from the 2000-pixel visible width,
    * re-points only the layers that already belong to the framebuffer
      carveout, preserving every other CON bit,
    * forces the reserved/alpha byte to 0xFF after BufferToVideo blits, since
      the OVL blends on alpha and FrameBufferBltLib leaves that byte alone,
    * clears all three queue slots so no LK splash remains.

  The register sequence and the alpha repair match the verified
  edk2-yunluo SimpleFbDxe for this board.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <PiDxe.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FrameBufferBltLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Protocol/GraphicsOutput.h>

#define YUNLUO_OVL0_BASE       0x14005000U
#define YUNLUO_OVL0_2L_BASE    0x14006000U
#define YUNLUO_OVL_LAYER_COUNT 4U
#define YUNLUO_OVL_EN          0x0CU
#define YUNLUO_OVL_ROI_SIZE    0x20U
#define YUNLUO_OVL_ROI_BGCLR   0x28U
#define YUNLUO_OVL_SRC_CON     0x2CU
#define YUNLUO_OVL_CON(n)      (0x30U + (0x20U * (n)))
#define YUNLUO_OVL_SRC_SIZE(n) (0x38U + (0x20U * (n)))
#define YUNLUO_OVL_OFFSET(n)   (0x3CU + (0x20U * (n)))
#define YUNLUO_OVL_PITCH_MSB(n) (0x40U + (0x20U * (n)))
#define YUNLUO_OVL_PITCH(n)    (0x44U + (0x20U * (n)))
#define YUNLUO_OVL_ADDR(n)     (0xF40U + (0x20U * (n)))
#define YUNLUO_OVL_FORMAT_MASK 0x010FF000U
#define YUNLUO_OVL_RGBA8888    (2U << 12)
#define YUNLUO_OVL_BYTE_SWAP   (1U << 24)
#define YUNLUO_OVL_BG_BLACK    0xFF000000U

#define FB_BYTES_PER_PIXEL     4U

typedef struct {
  VENDOR_DEVICE_PATH  Vendor;
  EFI_DEVICE_PATH     End;
} YUNLUO_DISPLAY_DEVICE_PATH;

STATIC YUNLUO_DISPLAY_DEVICE_PATH mDisplayDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP,
      { (UINT8)sizeof (VENDOR_DEVICE_PATH), (UINT8)(sizeof (VENDOR_DEVICE_PATH) >> 8) } },
    EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE,
    { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC FRAME_BUFFER_CONFIGURE *mFrameBufferConfigure;
STATIC UINTN                   mFrameBufferConfigureSize;

STATIC
EFI_STATUS
EFIAPI
YunluoQueryMode (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL          *This,
  IN  UINT32                                 ModeNumber,
  OUT UINTN                                 *SizeOfInfo,
  OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  **Info
  )
{
  EFI_STATUS  Status;

  if ((SizeOfInfo == NULL) || (Info == NULL) || (ModeNumber >= This->Mode->MaxMode)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = gBS->AllocatePool (
                  EfiBootServicesData,
                  sizeof (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION),
                  (VOID **)Info
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *SizeOfInfo = sizeof (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION);
  CopyMem (*Info, This->Mode->Info, sizeof (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION));

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
YunluoSetMode (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL  *This,
  IN UINT32                         ModeNumber
  )
{
  return (ModeNumber == 0) ? EFI_SUCCESS : EFI_UNSUPPORTED;
}

/**
  Point every enabled overlay layer that already targets the framebuffer
  carveout at the handed-over buffer, with the format and pitch the panel
  uses.  Layers belonging to anything else are left untouched.
**/
STATIC
UINT32
YunluoAdoptOverlay (
  IN UINT32                OverlayBase,
  IN EFI_PHYSICAL_ADDRESS  FrameBufferAddress,
  IN UINT32                FrameBufferSize,
  IN UINT32                FrameBufferStride,
  IN UINT32                VisibleWidth,
  IN UINT32                Height
  )
{
  UINT32  Enabled;
  UINT32  Layer;
  UINT32  Address;
  UINT32  Con;
  UINT32  Adopted;
  UINT32  RoiSize;
  UINT32  SrcSize;
  UINT32  Width;
  UINT32  Height2;
  UINT32  Area;
  UINT32  BestArea;
  UINT32  PrimaryLayer;

  Adopted      = 0;
  PrimaryLayer = 0xFFFFFFFFU;
  BestArea     = 0;
  RoiSize      = (Height << 16) | VisibleWidth;
  Enabled      = MmioRead32 (OverlayBase + YUNLUO_OVL_SRC_CON);

  DEBUG ((
    DEBUG_ERROR,
    "YUNLUOFB: OVL 0x%08x EN=0x%x ROI=0x%x SRC_CON=0x%x\n",
    OverlayBase,
    MmioRead32 (OverlayBase + YUNLUO_OVL_EN),
    MmioRead32 (OverlayBase + YUNLUO_OVL_ROI_SIZE),
    Enabled
    ));

  //
  // LK may have left the OVL ROI at the splash geometry.  Force the full
  // panel rectangle; otherwise the firmware's 2000x1200 framebuffer is
  // cropped into a corner.
  //
  MmioWrite32 (OverlayBase + YUNLUO_OVL_ROI_SIZE, RoiSize);
  MmioWrite32 (OverlayBase + YUNLUO_OVL_ROI_BGCLR, YUNLUO_OVL_BG_BLACK);

  //
  // Pick the primary layer: the enabled layer in the framebuffer carveout
  // with the largest source rectangle.  Only this layer gets the full
  // geometry.  Forcing every enabled layer full-screen turns LK's small
  // overlay/cursor layers into extra copies of the framebuffer.
  //
  for (Layer = 0; Layer < YUNLUO_OVL_LAYER_COUNT; Layer++) {
    if ((Enabled & (1U << Layer)) == 0) {
      continue;
    }

    Address = MmioRead32 (OverlayBase + YUNLUO_OVL_ADDR (Layer));
    if ((Address < FrameBufferAddress) ||
        (Address >= FrameBufferAddress + (3ULL * FrameBufferSize)))
    {
      continue;
    }

    SrcSize = MmioRead32 (OverlayBase + YUNLUO_OVL_SRC_SIZE (Layer));
    Width   = SrcSize & 0xFFFFU;
    Height2 = SrcSize >> 16;
    Area    = Width * Height2;
    if (Area > BestArea) {
      BestArea     = Area;
      PrimaryLayer = Layer;
    }
  }

  for (Layer = 0; Layer < YUNLUO_OVL_LAYER_COUNT; Layer++) {
    if ((Enabled & (1U << Layer)) == 0) {
      continue;
    }

    Address = MmioRead32 (OverlayBase + YUNLUO_OVL_ADDR (Layer));
    if ((Address < FrameBufferAddress) ||
        (Address >= FrameBufferAddress + (3ULL * FrameBufferSize)))
    {
      continue;
    }

    DEBUG ((
      DEBUG_ERROR,
      "YUNLUOFB: OVL 0x%x L%u CON=0x%x SRC=0x%x OFF=0x%x PITCH=0x%x ADDR=0x%x\n",
      OverlayBase,
      Layer,
      MmioRead32 (OverlayBase + YUNLUO_OVL_CON (Layer)),
      MmioRead32 (OverlayBase + YUNLUO_OVL_SRC_SIZE (Layer)),
      MmioRead32 (OverlayBase + YUNLUO_OVL_OFFSET (Layer)),
      MmioRead32 (OverlayBase + YUNLUO_OVL_PITCH (Layer)),
      Address
      ));

    Con = MmioRead32 (OverlayBase + YUNLUO_OVL_CON (Layer));
    /*
     * GOP is BGRR: memory bytes B,G,R,A.  Linux maps DRM_FORMAT_ARGB8888
     * (little-endian 0xAARRGGBB, same byte order) to CLRFMT_RGBA8888 with
     * no byte swap.
     */
    Con = (Con & ~YUNLUO_OVL_FORMAT_MASK) | YUNLUO_OVL_RGBA8888;
    // Full-screen layer alpha; AEN and other CON bits stay as LK left them.
    Con = (Con & ~0xFFU) | 0xFFU;
    // OVL_CON_AEN: disable per-pixel alpha blending.  Windows bootmgr leaves
    // the reserved/alpha byte at 0 for many glyphs; blending those pixels
    // makes text and UI elements disappear or turn into black blocks.
    Con &= ~BIT8;
    MmioWrite32 (OverlayBase + YUNLUO_OVL_CON (Layer), Con);
    MmioWrite32 (OverlayBase + YUNLUO_OVL_PITCH (Layer), FrameBufferStride);
    MmioWrite32 (OverlayBase + YUNLUO_OVL_ADDR (Layer), (UINT32)FrameBufferAddress);

    if (Layer == PrimaryLayer) {
      MmioWrite32 (OverlayBase + YUNLUO_OVL_PITCH_MSB (Layer), 0);
      MmioWrite32 (OverlayBase + YUNLUO_OVL_SRC_SIZE (Layer), RoiSize);
      MmioWrite32 (OverlayBase + YUNLUO_OVL_OFFSET (Layer), 0);
    }

    Adopted++;
  }

  DEBUG ((
    DEBUG_ERROR,
    "YUNLUOFB: OVL 0x%08x ROI->0x%x primary L%u adopted %u\n",
    OverlayBase,
    RoiSize,
    PrimaryLayer,
    Adopted
    ));

  //
  // LK can leave several layers enabled for its splash.  They all now point
  // at the framebuffer but keep their own OFFSET/SRC_SIZE, which tiles the
  // console several times across the panel.  Keep only the primary layer.
  //
  if (PrimaryLayer < YUNLUO_OVL_LAYER_COUNT) {
    UINT32 NewEnabled = (Enabled & ~0xFU) | (1U << PrimaryLayer);
    MmioWrite32 (OverlayBase + YUNLUO_OVL_SRC_CON, NewEnabled);
    DEBUG ((
      DEBUG_ERROR,
      "YUNLUOFB: OVL 0x%08x SRC_CON 0x%x -> 0x%x\n",
      OverlayBase,
      Enabled,
      NewEnabled
      ));
  }

  return Adopted;
}

STATIC
EFI_STATUS
EFIAPI
YunluoBlt (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL       *This,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL      *BltBuffer OPTIONAL,
  IN EFI_GRAPHICS_OUTPUT_BLT_OPERATION   BltOperation,
  IN UINTN                               SourceX,
  IN UINTN                               SourceY,
  IN UINTN                               DestinationX,
  IN UINTN                               DestinationY,
  IN UINTN                               Width,
  IN UINTN                               Height,
  IN UINTN                               Delta OPTIONAL
  )
{
  EFI_STATUS  Status;
  EFI_TPL     Tpl;
  UINT32      *FrameBuffer;
  UINTN       Row;
  UINTN       Column;

  Tpl = gBS->RaiseTPL (TPL_NOTIFY);

  Status = FrameBufferBlt (
             mFrameBufferConfigure,
             BltBuffer,
             BltOperation,
             SourceX,
             SourceY,
             DestinationX,
             DestinationY,
             Width,
             Height,
             Delta
             );

  gBS->RestoreTPL (Tpl);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // 32-bit GOP formats keep the alpha byte at bits 31:24.  FrameBufferBltLib
  // does not define it, and the MTK OVL blends on it, so an unset byte makes
  // the text invisible.  Only patch the region that was written to video; for
  // VideoToBuffer DestinationX/Y refer to the caller's buffer instead.
  //
  if ((BltOperation == EfiBltBufferToVideo) &&
      ((This->Mode->Info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor) ||
       (This->Mode->Info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor)))
  {
    FrameBuffer = (UINT32 *)(UINTN)This->Mode->FrameBufferBase;
    for (Row = 0; Row < Height; Row++) {
      for (Column = 0; Column < Width; Column++) {
        FrameBuffer[(DestinationY + Row) * This->Mode->Info->PixelsPerScanLine +
                    DestinationX + Column] |= 0xFF000000U;
      }
    }
  }

  return EFI_SUCCESS;
}

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL mGraphicsOutput = {
  YunluoQueryMode,
  YunluoSetMode,
  YunluoBlt,
  NULL
};

EFI_STATUS
EFIAPI
YunluoSimpleFbInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS           Status;
  EFI_HANDLE           Handle;
  EFI_PHYSICAL_ADDRESS FrameBufferAddress;
  UINT32               Stride;
  UINT32               VisibleWidth;
  UINT32               Height;
  UINT32               Layers;
  UINT32               PixelFormat;
  UINT32               FrameBufferSize;
  UINT32               Layer;
  UINT32               Adopted0;
  UINT32               Adopted2L;

  FrameBufferAddress = FixedPcdGet64 (PcdYunluoFrameBufferAddress);
  Stride             = FixedPcdGet32 (PcdYunluoFrameBufferStride);
  VisibleWidth       = FixedPcdGet32 (PcdYunluoFrameBufferVisibleWidth);
  Height             = FixedPcdGet32 (PcdYunluoFrameBufferHeight);
  Layers             = FixedPcdGet32 (PcdYunluoFrameBufferLayers);
  PixelFormat        = FixedPcdGet32 (PcdYunluoFrameBufferPixelFormat);

  if ((FrameBufferAddress == 0) || (Stride == 0) || (Height == 0)) {
    DEBUG ((DEBUG_ERROR, "YUNLUOFB: invalid framebuffer PCDs\n"));
    return EFI_DEVICE_ERROR;
  }

  if ((VisibleWidth == 0) || (VisibleWidth > Stride)) {
    VisibleWidth = Stride;
  }

  FrameBufferSize = Stride * Height * FB_BYTES_PER_PIXEL;

  DEBUG ((
    DEBUG_ERROR,
    "YUNLUOFB: base 0x%lx stride %u visible %u height %u layers %u format %u\n",
    FrameBufferAddress,
    Stride,
    VisibleWidth,
    Height,
    Layers,
    PixelFormat
    ));

  Status = gBS->AllocatePool (
                  EfiBootServicesData,
                  sizeof (EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE),
                  (VOID **)&mGraphicsOutput.Mode
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ZeroMem (mGraphicsOutput.Mode, sizeof (EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE));

  Status = gBS->AllocatePool (
                  EfiBootServicesData,
                  sizeof (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION),
                  (VOID **)&mGraphicsOutput.Mode->Info
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ZeroMem (mGraphicsOutput.Mode->Info, sizeof (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION));

  mGraphicsOutput.Mode->MaxMode       = 1;
  mGraphicsOutput.Mode->Mode          = 0;
  mGraphicsOutput.Mode->SizeOfInfo    = sizeof (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION);
  mGraphicsOutput.Mode->FrameBufferBase = FrameBufferAddress;
  mGraphicsOutput.Mode->FrameBufferSize = FrameBufferSize;

  mGraphicsOutput.Mode->Info->Version              = 0;
  mGraphicsOutput.Mode->Info->HorizontalResolution = VisibleWidth;
  mGraphicsOutput.Mode->Info->VerticalResolution   = Height;
  mGraphicsOutput.Mode->Info->PixelsPerScanLine    = Stride;
  mGraphicsOutput.Mode->Info->PixelFormat          =
    (PixelFormat == 0) ? PixelRedGreenBlueReserved8BitPerColor
                       : PixelBlueGreenRedReserved8BitPerColor;

  Status = FrameBufferBltConfigure (
             (VOID *)(UINTN)mGraphicsOutput.Mode->FrameBufferBase,
             mGraphicsOutput.Mode->Info,
             mFrameBufferConfigure,
             &mFrameBufferConfigureSize
             );
  if (Status == RETURN_BUFFER_TOO_SMALL) {
    mFrameBufferConfigure = AllocatePool (mFrameBufferConfigureSize);
    if (mFrameBufferConfigure == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }

    Status = FrameBufferBltConfigure (
               (VOID *)(UINTN)mGraphicsOutput.Mode->FrameBufferBase,
               mGraphicsOutput.Mode->Info,
               mFrameBufferConfigure,
               &mFrameBufferConfigureSize
               );
  }

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "YUNLUOFB: FrameBufferBltConfigure %r\n", Status));
    return Status;
  }

  //
  // Clear every queued buffer first: the adoption dump below is written to
  // the framebuffer and must survive.
  //
  for (Layer = 0; Layer < Layers; Layer++) {
    ZeroMem ((VOID *)(UINTN)(FrameBufferAddress + (UINT64)FrameBufferSize * Layer), FrameBufferSize);
  }

  //
  // LK leaves the OVL layers enabled but aimed at its own splash buffer.
  // Adopt the layers that already sit in the framebuffer carveout.
  //
  Adopted0   = YunluoAdoptOverlay (YUNLUO_OVL0_BASE, FrameBufferAddress, FrameBufferSize, Stride * FB_BYTES_PER_PIXEL, VisibleWidth, Height);
  Adopted2L  = YunluoAdoptOverlay (YUNLUO_OVL0_2L_BASE, FrameBufferAddress, FrameBufferSize, Stride * FB_BYTES_PER_PIXEL, VisibleWidth, Height);

  DEBUG ((
    DEBUG_ERROR,
    "YUNLUOFB: ready %ux%u pitch %u, OVL0 %u OVL0_2L %u\n",
    VisibleWidth,
    Height,
    Stride,
    Adopted0,
    Adopted2L
    ));

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiDevicePathProtocolGuid,
                  &mDisplayDevicePath,
                  &gEfiGraphicsOutputProtocolGuid,
                  &mGraphicsOutput,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "YUNLUOFB: install GOP %r\n", Status));
  }

  return Status;
}
