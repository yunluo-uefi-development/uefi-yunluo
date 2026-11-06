// SPDX-License-Identifier: BSD-2-Clause-Patent
// Platform-only UEFI contract for the Windows NT36523 path.
// This driver publishes verified board resources; it deliberately does not
// reset the controller, upload firmware, claim GPIOs or generate touch reports.
#include "YunluoTouchPlatform.h"
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

STATIC EFI_STATUS EFIAPI
GetInfo (
  IN  YUNLUO_TOUCH_PLATFORM_PROTOCOL  *This,
  OUT YUNLUO_TOUCH_PLATFORM_INFO      *Info
  )
{
  if ((This == NULL) || (Info == NULL) || (This->Revision != 1)) {
    return EFI_INVALID_PARAMETER;
  }
  ZeroMem (Info, sizeof (*Info));
  Info->GpioBase      = 0x10005000;
  Info->GpioRegionSize = 0x1000;
  Info->SpiMmioBase   = 0x1100A000;
  Info->SpiMmioSize   = 0x100;
  Info->SpiPins[0]    = 30; // CLK
  Info->SpiPins[1]    = 31; // CS
  Info->SpiPins[2]    = 32; // MOSI
  Info->SpiPins[3]    = 33; // MISO
  Info->TouchIrqGpio  = 8;
  Info->Mode          = 0;  // SPI mode 0; private NT36523 protocol remains OS-owned.
  return EFI_SUCCESS;
}

STATIC YUNLUO_TOUCH_PLATFORM_PROTOCOL mProtocol = {
  1,
  GetInfo
};

EFI_STATUS
EFIAPI
YunluoTouchPlatformEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS Status;
  (VOID)ImageHandle;
  (VOID)SystemTable;
  Status = gBS->InstallProtocolInterface (
                  &ImageHandle,
                  &gYunluoTouchPlatformProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mProtocol
                  );
  if (!EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "YUNLUO-TOUCH-PLATFORM: resources published; no hardware claimed\n"));
  }
  return Status;
}
