#include <Uefi.h>
#include <PiDxe.h>
#include <Protocol/DevicePath.h>
#include <Protocol/UfsHostController.h>
#include <Protocol/UfsHostControllerPlatform.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/ArmSmcLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/MtkPmic.h>
#include <IndustryStandard/UfsHci.h>

#define YUNLUO_UFS_HCI_BASE       0x11270000U
#define YUNLUO_UFS_PHY_BASE       0x01FA0000U
#define YUNLUO_INFRA_BASE         0x10001000U

#define INFRA_AO2_CG_CLR          0xA8U
#define INFRA_AO2_UNIPRO_SYS      BIT11
#define INFRA_AO2_UNIPRO_TICK     BIT12
#define INFRA_AO2_UFS_SAP_BCLK    BIT13
#define INFRA_AO2_UFS             BIT28
#define INFRA_AO2_UFS_AES         BIT29

#define TOPCKGEN_BASE             0x10000000U
#define TOPCKGEN_UFS_CFG          0xD0U
#define TOPCKGEN_UFS_CFG_SET      0xD4U
#define TOPCKGEN_UFS_CFG_CLR      0xD8U
#define TOPCKGEN_UFS_SEL_SHIFT    24U
#define TOPCKGEN_UFS_SEL_MASK     (0x7U << TOPCKGEN_UFS_SEL_SHIFT)
#define TOPCKGEN_UFS_SEL_PARENT   (1U << TOPCKGEN_UFS_SEL_SHIFT)
#define TOPCKGEN_UFS_GATE_BIT     BIT31
#define TOPCKGEN_AES_SEL_SHIFT    16U
#define TOPCKGEN_AES_SEL_MASK     (0x7U << TOPCKGEN_AES_SEL_SHIFT)
#define TOPCKGEN_AES_SEL_PARENT   (1U << TOPCKGEN_AES_SEL_SHIFT)
#define TOPCKGEN_AES_GATE_BIT     BIT23
#define TOPCKGEN_UFS_UPDATE       BIT20

#define MTK_SIP_UFS_CONTROL       0xC2000276U
#define UFS_MTK_SIP_DEVICE_RESET  BIT1
#define UFS_MTK_SIP_CRYPTO_CTRL   BIT2
#define UFS_MTK_SIP_REFCLK        BIT3
#define UFS_SMC_OK                0U

#define UFS_UIC_DME_GET           0x01U
#define UFS_UIC_DME_SET           0x02U
#define UFS_PA_LOCALVERINFO       0x15A9U
#define UFS_PA_LOCAL_TX_LCC       0x155EU
#define UFS_PA_MAXTXSPEEDFAST     0x1521U
#define UFS_PA_ACTIVETXDATALANES  0x1560U
#define UFS_PA_TXPWRSTATUS        0x1567U
#define UFS_PA_TXGEAR             0x1568U
#define UFS_PA_PWRMODE            0x1571U
#define UFS_PA_TXTERMINATION      0x1569U
#define UFS_PA_MAXTXSPEEDSLOW     0x1522U
#define UFS_PA_HSSERIES           0x156AU
#define UFS_PA_RXTERMINATION      0x1584U
#define UFS_PA_ACTIVERXDATALANES  0x1580U
#define UFS_PA_RXPWRSTATUS        0x1582U
#define UFS_PA_RXGEAR             0x1583U
#define UFS_VS_DEBUGCLOCKENABLE   0xD0A1U
#define UFS_VS_SAVEPOWERCONTROL   0xD0A6U
#define UFS_VS_UNIPRO_POWERDOWN   0xD0A8U
#define UFS_SAVEPOWER_RX_SYMBOL_CLK_GATE  BIT0
#define UFS_SAVEPOWER_SYS_CLK_GATE        BIT2
#define UFS_SAVEPOWER_TX_CLK_GATE         BIT3
#define UFS_SAVEPOWER_DEEP_STALL          BIT6
#define UFS_DEBUGCLK_TX_SYMBOL_CLK_REQ_FORCE BIT5

#define INFRA_RST1_SET            0x130U
#define INFRA_RST2_SET            0x140U
#define INFRA_RST3_SET            0x150U
#define UFS_HCI_RST_BIT           BIT15
#define UFS_UNIPRO_RST_BIT        BIT7
#define UFS_CRYPTO_RST_BIT        BIT21

#define UFS_XOUFS_CTRL            0x140U
#define UFS_REFCLK_CTRL           0x144U
#define UFS_REFCLK_REQUEST        BIT0
#define UFS_REFCLK_ACK            BIT1
#define UFS_REFCLK_TIMEOUT_US     3000U

#define MP_GLB_DIG_8C             0x008CU
#define FRC_PLL_ISO_EN            BIT8
#define PLL_ISO_EN                BIT9
#define FRC_PLL_PWR_ON            BIT10
#define PLL_PWR_ON                BIT11
#define MP_LN_DIG_RX_9C           0xA09CU
#define FSM_DIFZ_FRC              BIT18
#define MP_LN_DIG_RX_AC           0xA0ACU
#define FRC_RX_SQ_EN              BIT0
#define RX_SQ_EN                  BIT1
#define MP_LN_RX_44               0xB044U
#define FRC_CDR_PWR_ON            BIT17
#define CDR_PWR_ON                BIT18
#define FRC_CDR_ISO_EN            BIT19
#define CDR_ISO_EN                BIT20

#define UFS_HC_PRIVATE_SIGNATURE  SIGNATURE_32 ('Y','U','F','S')

/* Hold each diagnostic milestone on screen long enough to photograph it. */
#define MTK_UFS_PHOTO_STALL_MS    4000U
#define MTK_UFS_DUMP_PERIOD_100NS 40000000ULL /* 4 s */
#define MTK_UFS_DUMP_COUNT        5U

/*
 * AUTO_HIBERNATE_IDLE_TIMER: AUTO_HIBERNATE_TIMER [23:0] scaled by
 * 10^AUTO_HIBERNATE_SCALE [27:24].  Zero does not disable auto-hibernate, it
 * asks the link to hibernate the moment it goes idle, so every 4KB data
 * segment of a transfer pays a hibernate/exit handshake and throughput
 * collapses to ~0.6MB/s.  This is the value the kernel installs when the host
 * supports auto-hibernate (ufshcd_probe_hba(): timer 150, scale 3).
 */
#define YUNLUO_UFS_AHIT_DEFAULT   0x03000096U

/* UFSHCI register offsets and Interrupt Status fields used below. */
#define MTK_UFS_REG_HCS           0x04U
#define MTK_UFS_REG_IS            0x20U
#define MTK_UFS_IS_UPMCS          0x10U   /* UIC Power Mode Change Status */

/* UniPro power mode encoding: FAST_MODE is 1, PA_PWRMODE = RX << 4 | TX. */
#define MTK_UFS_PWRMODE_HS        0x11U
#define MTK_UFS_HSSERIES_HS_G1    0x10U

typedef struct {
  VENDOR_DEVICE_PATH Vendor;
  EFI_DEVICE_PATH_PROTOCOL End;
} YUNLUO_UFS_DEVICE_PATH;

typedef struct {
  UINT32 Signature;
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL Hc;
  EDKII_UFS_HC_PLATFORM_PROTOCOL Platform;
  EFI_HANDLE Handle;
  YUNLUO_UFS_DEVICE_PATH DevicePath;
} YUNLUO_UFS_PRIVATE;

#define PRIVATE_FROM_HC(a) CR (a, YUNLUO_UFS_PRIVATE, Hc, UFS_HC_PRIVATE_SIGNATURE)

#define YUNLUO_UFS_MAPPING_SIGNATURE SIGNATURE_32 ('Y','U','M','P')

typedef struct {
  UINT32 Signature;
  VOID   *HostAddress;
  UINTN  Length;
} YUNLUO_UFS_MAPPING;

STATIC YUNLUO_UFS_PRIVATE *mPrivate;

STATIC
EFI_STATUS
EFIAPI
MtkUfsGetMmioBar (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL *This,
  OUT UINTN *MmioBar
  )
{
  if (This == NULL || MmioBar == NULL) return EFI_INVALID_PARAMETER;
  *MmioBar = YUNLUO_UFS_HCI_BASE;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MtkUfsAllocateBuffer (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL *This,
  IN EFI_ALLOCATE_TYPE Type,
  IN EFI_MEMORY_TYPE MemoryType,
  IN UINTN Pages,
  OUT VOID **HostAddress,
  IN UINT64 Attributes
  )
{
  EFI_PHYSICAL_ADDRESS Address;
  EFI_STATUS           Status;
  if (This == NULL || HostAddress == NULL || Pages == 0) return EFI_INVALID_PARAMETER;
  Address = 0xFFFFFFFFULL;
  Status = gBS->AllocatePages (AllocateMaxAddress, MemoryType, Pages, &Address);
  if (EFI_ERROR (Status)) {
    return EFI_OUT_OF_RESOURCES;
  }
  /*
   * MTK UFS is not cache-coherent, and the descriptor rings are shared with
   * the controller.  Match Linux dma_alloc_coherent() and map them Normal
   * Non-Cacheable (EFI_MEMORY_WC), not Device memory: Normal memory permits
   * the unaligned accesses the packed UPIU/UTRD structures generate, while
   * Device memory faults on them.
   */
  Status = gDS->SetMemorySpaceAttributes (
                Address,
                EFI_PAGES_TO_SIZE (Pages),
                EFI_MEMORY_WC
                );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "UFS MTK: failed to mark descriptor memory uncached: %r\n", Status));
  }
  *HostAddress = (VOID *)(UINTN)Address;
  ZeroMem (*HostAddress, EFI_PAGES_TO_SIZE (Pages));
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MtkUfsFreeBuffer (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL *This,
  IN UINTN Pages,
  IN VOID *HostAddress
  )
{
  if (This == NULL || HostAddress == NULL || Pages == 0) return EFI_INVALID_PARAMETER;
  return gBS->FreePages ((EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress, Pages);
}

STATIC
EFI_STATUS
EFIAPI
MtkUfsMap (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL *This,
  IN EDKII_UFS_HOST_CONTROLLER_OPERATION Operation,
  IN VOID *HostAddress,
  IN OUT UINTN *NumberOfBytes,
  OUT EFI_PHYSICAL_ADDRESS *DeviceAddress,
  OUT VOID **Mapping
  )
{
  YUNLUO_UFS_MAPPING *MapInfo;

  if (This == NULL || HostAddress == NULL || NumberOfBytes == NULL ||
      DeviceAddress == NULL || Mapping == NULL) return EFI_INVALID_PARAMETER;

  MapInfo = AllocatePool (sizeof (*MapInfo));
  if (MapInfo == NULL) return EFI_OUT_OF_RESOURCES;
  MapInfo->Signature   = YUNLUO_UFS_MAPPING_SIGNATURE;
  MapInfo->HostAddress = HostAddress;
  MapInfo->Length      = *NumberOfBytes;

  if (Operation == EdkiiUfsHcOperationBusMasterWrite) {
    /* Drop stale/dirty lines before the device fills the buffer. */
    InvalidateDataCacheRange (HostAddress, *NumberOfBytes);
  } else {
    WriteBackDataCacheRange (HostAddress, *NumberOfBytes);
  }

  *DeviceAddress = (EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress;
  *Mapping = MapInfo;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MtkUfsUnmap (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL *This,
  IN VOID *Mapping
  )
{
  YUNLUO_UFS_MAPPING *MapInfo;

  if (This == NULL) return EFI_INVALID_PARAMETER;
  MapInfo = (YUNLUO_UFS_MAPPING *)Mapping;
  if ((MapInfo == NULL) || (MapInfo->Signature != YUNLUO_UFS_MAPPING_SIGNATURE)) {
    return EFI_SUCCESS;
  }

  /* Make device DMA writes visible to the processor. */
  InvalidateDataCacheRange (MapInfo->HostAddress, MapInfo->Length);
  MapInfo->Signature = 0;
  FreePool (MapInfo);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MtkUfsAllocateCommonBuffer (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL *This,
  IN EFI_ALLOCATE_TYPE Type,
  IN EFI_MEMORY_TYPE MemoryType,
  IN UINTN Pages,
  OUT VOID **HostAddress,
  IN UINT64 Attributes
  )
{
  return MtkUfsAllocateBuffer (This, Type, MemoryType, Pages, HostAddress, Attributes);
}

STATIC
EFI_STATUS
EFIAPI
MtkUfsFlush (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL *This
  )
{
  MemoryFence (); 
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MtkUfsRead (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL *This,
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH Width,
  IN UINT64 Offset,
  IN UINTN Count,
  IN OUT VOID *Buffer
  )
{
  UINTN Index;
  if (This == NULL || Buffer == NULL || Offset >= 0x2300) return EFI_INVALID_PARAMETER;
  for (Index = 0; Index < Count; Index++) {
    switch (Width) {
      case EfiUfsHcWidthUint8:  ((UINT8 *)Buffer)[Index] = MmioRead8 (YUNLUO_UFS_HCI_BASE + (UINT32)Offset + Index); break;
      case EfiUfsHcWidthUint16: ((UINT16 *)Buffer)[Index] = MmioRead16 (YUNLUO_UFS_HCI_BASE + (UINT32)Offset + Index * 2); break;
      case EfiUfsHcWidthUint32: ((UINT32 *)Buffer)[Index] = MmioRead32 (YUNLUO_UFS_HCI_BASE + (UINT32)Offset + Index * 4); break;
      case EfiUfsHcWidthUint64: ((UINT64 *)Buffer)[Index] = MmioRead64 (YUNLUO_UFS_HCI_BASE + (UINT32)Offset + Index * 8); break;
      default: return EFI_INVALID_PARAMETER;
    }
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MtkUfsWrite (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL *This,
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH Width,
  IN UINT64 Offset,
  IN UINTN Count,
  IN OUT VOID *Buffer
  )
{
  UINTN Index;
  if (This == NULL || Buffer == NULL || Offset >= 0x2300) return EFI_INVALID_PARAMETER;
  for (Index = 0; Index < Count; Index++) {
    switch (Width) {
      case EfiUfsHcWidthUint8:  MmioWrite8 (YUNLUO_UFS_HCI_BASE + (UINT32)Offset + Index, ((UINT8 *)Buffer)[Index]); break;
      case EfiUfsHcWidthUint16: MmioWrite16 (YUNLUO_UFS_HCI_BASE + (UINT32)Offset + Index * 2, ((UINT16 *)Buffer)[Index]); break;
      case EfiUfsHcWidthUint32: MmioWrite32 (YUNLUO_UFS_HCI_BASE + (UINT32)Offset + Index * 4, ((UINT32 *)Buffer)[Index]); break;
      case EfiUfsHcWidthUint64: MmioWrite64 (YUNLUO_UFS_HCI_BASE + (UINT32)Offset + Index * 8, ((UINT64 *)Buffer)[Index]); break;
      default: return EFI_INVALID_PARAMETER;
    }
  }
  MemoryFence ();
  return EFI_SUCCESS;
}

STATIC
UINTN
MtkUfsSmc (
  IN UINTN Command,
  IN UINTN Arg1,
  IN UINTN Arg2,
  OUT ARM_SMC_ARGS *Args OPTIONAL
  );

STATIC
VOID
MtkUfsStatusText (
  IN CONST CHAR16 *Text
  );

STATIC
VOID
MtkUfsClearHciErrors (
  VOID
  );

STATIC
VOID
MtkUfsPhyPowerOn (VOID)
{
  EFI_STATUS Status;
  MTK_PMIC_PROTOCOL *Pmic;

  Status = gBS->LocateProtocol (&gMediaTekPmicProtocolGuid,
                                NULL, (VOID **)&Pmic);
  if (!EFI_ERROR (Status) && (Pmic != NULL) && (Pmic->RegulatorSetEnable != NULL)) {
    Pmic->RegulatorSetEnable ("ldo_vemc", TRUE);
    MtkUfsStatusText (L"UFS MTK: VEMC=ON (ldo_vemc)\r\n");
  } else {
    MtkUfsStatusText (L"UFS MTK: VEMC protocol unavailable\r\n");
  }
  MmioWrite32 (YUNLUO_INFRA_BASE + INFRA_AO2_CG_CLR,
               INFRA_AO2_UNIPRO_SYS | INFRA_AO2_UNIPRO_TICK |
               INFRA_AO2_UFS_SAP_BCLK | INFRA_AO2_UFS | INFRA_AO2_UFS_AES);
  /* Linux selects mainpll_d4_d4 (parent index 1) for ufs_sel and
     ungates the mux before enabling the downstream UFS gates. */
  MmioWrite32 (TOPCKGEN_BASE + TOPCKGEN_UFS_CFG_CLR,
               TOPCKGEN_UFS_SEL_MASK | TOPCKGEN_AES_SEL_MASK);
  MmioWrite32 (TOPCKGEN_BASE + TOPCKGEN_UFS_CFG_SET,
               TOPCKGEN_UFS_SEL_PARENT | TOPCKGEN_AES_SEL_PARENT);
  MmioWrite32 (TOPCKGEN_BASE + TOPCKGEN_UFS_CFG_CLR,
               TOPCKGEN_UFS_GATE_BIT);
  /* clk-mt6789.c gates both UFS muxes at bit31 (ufs_sel) and bit23
     (aes_ufsfde_sel) of CLK_CFG_12. */
  MmioWrite32 (TOPCKGEN_BASE + TOPCKGEN_UFS_CFG_CLR,
               TOPCKGEN_AES_GATE_BIT);
  MmioWrite32 (TOPCKGEN_BASE + 0x08U,
               TOPCKGEN_UFS_UPDATE | (1U << 19));
  MmioOr32 (YUNLUO_UFS_PHY_BASE + MP_GLB_DIG_8C, PLL_PWR_ON);
  MmioAnd32 (YUNLUO_UFS_PHY_BASE + MP_GLB_DIG_8C, (UINT32)~FRC_PLL_PWR_ON);
  MmioAnd32 (YUNLUO_UFS_PHY_BASE + MP_GLB_DIG_8C,
             (UINT32)~(PLL_ISO_EN | FRC_PLL_ISO_EN));
  MmioOr32 (YUNLUO_UFS_PHY_BASE + MP_LN_RX_44, CDR_PWR_ON);
  MmioAnd32 (YUNLUO_UFS_PHY_BASE + MP_LN_RX_44, (UINT32)~FRC_CDR_PWR_ON);
  MmioAnd32 (YUNLUO_UFS_PHY_BASE + MP_LN_RX_44,
             (UINT32)~(CDR_ISO_EN | FRC_CDR_ISO_EN));
  MmioOr32 (YUNLUO_UFS_PHY_BASE + MP_LN_DIG_RX_AC, RX_SQ_EN);
  MmioAnd32 (YUNLUO_UFS_PHY_BASE + MP_LN_DIG_RX_AC, (UINT32)~FRC_RX_SQ_EN);
  MicroSecondDelay (1);
  MmioAnd32 (YUNLUO_UFS_PHY_BASE + MP_LN_DIG_RX_9C, (UINT32)~FSM_DIFZ_FRC);

  /* Baseline enables unipro_clk/mp_clk before the PHY active sequence. */
  MtkUfsStatusText (L"UFS MTK: clocks/gates enabled\r\n");
}

STATIC
VOID
MtkUfsResetAndEnable (VOID)
{
  MmioWrite32 (YUNLUO_INFRA_BASE + INFRA_RST1_SET, UFS_HCI_RST_BIT);
  MmioWrite32 (YUNLUO_INFRA_BASE + INFRA_RST2_SET, UFS_UNIPRO_RST_BIT);
  MmioWrite32 (YUNLUO_INFRA_BASE + INFRA_RST3_SET, UFS_CRYPTO_RST_BIT);
  MicroSecondDelay (100);
  MmioWrite32 (YUNLUO_INFRA_BASE + INFRA_RST2_SET + 4, UFS_UNIPRO_RST_BIT);
  MmioWrite32 (YUNLUO_INFRA_BASE + INFRA_RST3_SET + 4, UFS_CRYPTO_RST_BIT);
  MmioWrite32 (YUNLUO_INFRA_BASE + INFRA_RST1_SET + 4, UFS_HCI_RST_BIT);
}

STATIC
UINTN
MtkUfsSmc (
  IN UINTN Command,
  IN UINTN Arg1,
  IN UINTN Arg2,
  OUT ARM_SMC_ARGS *Args OPTIONAL
  );

STATIC
VOID
MtkUfsStatusText (
  IN CONST CHAR16 *Text
  );

STATIC
EFI_STATUS
MtkUfsRefClkOn (
  VOID
  )
{
  UINT32 Value;
  UINTN Elapsed;
  UINTN SmcStatus;

  /* Baseline calls TFA before requesting the controller refclk. */
  SmcStatus = MtkUfsSmc (UFS_MTK_SIP_REFCLK, 1, 0, NULL);
  if (SmcStatus != UFS_SMC_OK) {
    MtkUfsStatusText (L"UFS MTK: refclk SMC=FAIL\r\n");
    return EFI_DEVICE_ERROR;
  }

  MmioOr32 (YUNLUO_UFS_HCI_BASE + UFS_XOUFS_CTRL, BIT7);
  MmioOr32 (YUNLUO_UFS_HCI_BASE + UFS_REFCLK_CTRL, UFS_REFCLK_REQUEST);
  for (Elapsed = 0; Elapsed < UFS_REFCLK_TIMEOUT_US; Elapsed += 10) {
    Value = MmioRead32 (YUNLUO_UFS_HCI_BASE + UFS_REFCLK_CTRL);
    if (((Value & UFS_REFCLK_ACK) >> 1) ==
        (Value & UFS_REFCLK_REQUEST)) {
      /* Baseline ungating wait: 32-100 us depending on the device. */
      MicroSecondDelay (100);
      return EFI_SUCCESS;
    }
    MicroSecondDelay (10);
  }
  return EFI_TIMEOUT;
}

STATIC
VOID
MtkUfsDeviceReset (
  VOID
  )
{
  UINTN SmcStatus;

  /*
   * Baseline ufshcd_init() calls ufshcd_vops_device_reset() before
   * ufshcd_hba_enable().  MTK pulses RST_n low (active low) for >=10us,
   * then high, then waits 10-15ms.
   */
  SmcStatus = MtkUfsSmc (UFS_MTK_SIP_DEVICE_RESET, 0, 0, NULL);
  MtkUfsStatusText (SmcStatus == UFS_SMC_OK ?
                    L"UFS MTK: device reset low OK\r\n" :
                    L"UFS MTK: device reset low FAIL\r\n");
  MicroSecondDelay (10);

  SmcStatus = MtkUfsSmc (UFS_MTK_SIP_DEVICE_RESET, 1, 0, NULL);
  MtkUfsStatusText (SmcStatus == UFS_SMC_OK ?
                    L"UFS MTK: device reset high OK\r\n" :
                    L"UFS MTK: device reset high FAIL\r\n");
  MicroSecondDelay (10000);
}

STATIC
EFI_STATUS
EFIAPI
MtkUfsOverrideInfo (
  IN EFI_HANDLE Controller,
  IN OUT EDKII_UFS_HC_INFO *Info
  )
{
  if (Info == NULL) return EFI_INVALID_PARAMETER;
  DEBUG ((
    DEBUG_ERROR,
    "UFS MTK: HCI info version=0x%x capabilities=0x%x\n",
    Info->Version,
    Info->Capabilities
    ));

  /*
   * The old 32-bit EDK2 UfsPassThru worked on this HCI; the new generic
   * driver follows UFS_HC_CAP_64ADDR and programs 64-bit UTRL/UTRD fields.
   * All our buffers are below 4 GiB, so force the legacy 32-bit path.
   */
  Info->Capabilities &= ~UFS_HC_CAP_64ADDR;
  DEBUG ((DEBUG_ERROR, "UFS MTK: HCI capabilities -> 0x%x (64ADDR cleared)\n", Info->Capabilities));

  /* EDK2 UfsPassThru uses legacy UTP lists and never enables MCQ. */
  return EFI_SUCCESS;
}

STATIC
UINTN
MtkUfsSmc (
  IN UINTN Command,
  IN UINTN Arg1,
  IN UINTN Arg2,
  OUT ARM_SMC_ARGS *Args OPTIONAL
  )
{
  ARM_SMC_ARGS Local;

  ZeroMem (&Local, sizeof (Local));
  Local.Arg0 = MTK_SIP_UFS_CONTROL;
  Local.Arg1 = Command;
  Local.Arg2 = Arg1;
  Local.Arg3 = Arg2;
  ArmCallSmc (&Local);
  if (Args != NULL) {
    CopyMem (Args, &Local, sizeof (Local));
  }
  return Local.Arg0;
}

/*
 * The first link bring-up prints the whole sequence.  Every later cycle comes
 * from the pass-thru driver's controller recovery, where the bring-up detail
 * only pushes the transfer diagnostics off the console.
 */
STATIC BOOLEAN  mMtkUfsQuiet = FALSE;

STATIC
VOID
MtkUfsStatusText (
  IN CONST CHAR16 *Text
  )
{
  if ((Text == NULL) || mMtkUfsQuiet) {
    return;
  }

  /* ConOut may not exist yet during DXE dispatch; DEBUG_ERROR reaches the
     FrameBuffer serial console directly. */
  DEBUG ((DEBUG_ERROR, "%s", Text));
}

STATIC
EFI_STATUS
MtkUfsExecUic (
  IN EDKII_UFS_HC_DRIVER_INTERFACE *Interface,
  IN UINT32 Opcode,
  IN UINT32 Arg1,
  IN UINT32 Arg2,
  OUT UINT32 *Result
  )
{
  EDKII_UIC_COMMAND Command;
  EFI_STATUS Status;

  if ((Interface == NULL) || (Interface->UfsExecUicCommand == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  Command.Opcode = Opcode;
  Command.Arg1 = Arg1;
  Command.Arg2 = Arg2;
  Command.Arg3 = 0;
  Status = Interface->UfsExecUicCommand (Interface, &Command);
  if (Result != NULL) {
    *Result = Command.Arg2;
  }
  return Status;
}

STATIC
EFI_STATUS
MtkUfsDmeGet (
  IN EDKII_UFS_HC_DRIVER_INTERFACE *Interface,
  IN UINT32 Mib,
  OUT UINT32 *Value
  )
{
  EDKII_UIC_COMMAND Command;
  EFI_STATUS Status;

  if ((Interface == NULL) || (Interface->UfsExecUicCommand == NULL) ||
      (Value == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  Command.Opcode = UFS_UIC_DME_GET;
  Command.Arg1   = Mib << 16;
  Command.Arg2   = 0;
  Command.Arg3   = 0;
  Status = Interface->UfsExecUicCommand (Interface, &Command);
  /* UFS HCI returns the DME GET value in UICCMDARG3, not ARG2. */
  *Value = Command.Arg3;
  return Status;
}

/**
  Print a UniPro attribute as a bit bar.  Hex digits in a photograph of a
  scrolling console are unreadable; the count of '#' is not.
**/
STATIC
VOID
MtkUfsDumpAttr (
  IN EDKII_UFS_HC_DRIVER_INTERFACE *Interface,
  IN CONST CHAR8                   *Name,
  IN UINT32                        Mib
  )
{
  UINT32  Value;
  CHAR8   Bar[9];
  UINTN   Index;

  Value = 0;
  MtkUfsDmeGet (Interface, Mib, &Value);
  for (Index = 0; Index < 8; Index++) {
    Bar[Index] = (CHAR8)(((Value >> Index) & 1) != 0 ? '#' : '.');
  }

  Bar[8] = '\0';
  DEBUG ((DEBUG_ERROR, "UFS MTK: %a=%u [%a]\n", Name, Value, Bar));
}

/**
  DME_SET of a local UniPro attribute.

  The value travels in UICCMDARG3, exactly like the result of DME_GET.  The
  generic MtkUfsExecUic() helper passes its argument through as ARG2, which
  the controller ignores, so every DME_SET routed through it has been writing
  zero.
**/
STATIC
EFI_STATUS
MtkUfsDmeSet (
  IN EDKII_UFS_HC_DRIVER_INTERFACE *Interface,
  IN CONST CHAR8                   *Name,
  IN UINT32                        Mib,
  IN UINT32                        Value
  )
{
  EDKII_UIC_COMMAND  Command;
  EFI_STATUS         Status;

  if ((Interface == NULL) || (Interface->UfsExecUicCommand == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Command.Opcode = UFS_UIC_DME_SET;
  Command.Arg1   = Mib << 16;
  Command.Arg2   = 0;
  Command.Arg3   = Value;
  Status = Interface->UfsExecUicCommand (Interface, &Command);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "UFS MTK: set %a=%u rejected code=%u %r\n",
      Name,
      Value,
      Command.Arg2 & 0xFF,
      Status
      ));
  }

  return Status;
}

/**
  Move the link from the PWM mode it comes up in after LINK_STARTUP to HS-G1.

  Nothing in the generic pass-thru driver does this, so the data path stays at
  roughly a megabyte per second and the Windows loader times out reading its
  image.  Mirrors ufshcd_change_power_mode(): the capability attributes are
  best-effort (several are read-only and rejected, which the kernel ignores
  too) and only the PA_PWRMODE change decides the outcome.
**/
STATIC
EFI_STATUS
MtkUfsChangePwrModeHs (
  IN EDKII_UFS_HC_DRIVER_INTERFACE *Interface
  )
{
  EFI_STATUS  Status;
  UINT32      Value;
  UINTN       Wait;

  MtkUfsDmeSet (Interface, "txgear", UFS_PA_TXGEAR, 1);
  MtkUfsDmeSet (Interface, "rxgear", UFS_PA_RXGEAR, 1);
  MtkUfsDmeSet (Interface, "txlanes", UFS_PA_ACTIVETXDATALANES, 1);
  MtkUfsDmeSet (Interface, "rxlanes", UFS_PA_ACTIVERXDATALANES, 1);
  MtkUfsDmeSet (Interface, "txterm", UFS_PA_TXTERMINATION, 1);
  MtkUfsDmeSet (Interface, "rxterm", UFS_PA_RXTERMINATION, 1);
  MtkUfsDmeSet (Interface, "hsseries", UFS_PA_HSSERIES, MTK_UFS_HSSERIES_HS_G1);

  /* FAST_MODE (1) in both nibbles: RX << 4 | TX. */
  MmioWrite32 (YUNLUO_UFS_HCI_BASE + MTK_UFS_REG_IS, MTK_UFS_IS_UPMCS);
  Status = MtkUfsDmeSet (Interface, "pwrmode", UFS_PA_PWRMODE, MTK_UFS_PWRMODE_HS);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Wait = 0;
  while (Wait < 500) {
    Value = MmioRead32 (YUNLUO_UFS_HCI_BASE + MTK_UFS_REG_IS);
    if ((Value & MTK_UFS_IS_UPMCS) != 0) {
      MmioWrite32 (YUNLUO_UFS_HCI_BASE + MTK_UFS_REG_IS, MTK_UFS_IS_UPMCS);
      break;
    }

    MicroSecondDelay (1000);
    Wait++;
  }

  if (Wait >= 500) {
    DEBUG ((DEBUG_ERROR, "UFS MTK: pwr mode change timed out\n"));
    return EFI_TIMEOUT;
  }

  Value = (MmioRead32 (YUNLUO_UFS_HCI_BASE + MTK_UFS_REG_HCS) >> 8) & 0x7;
  DEBUG ((DEBUG_ERROR, "UFS MTK: pwr mode done upmcrs=%u\n", Value));
  return EFI_SUCCESS;
}

STATIC
VOID
MtkUfsPause (
  IN UINTN Milliseconds
  )
{
  gBS->Stall ((UINTN)Milliseconds * 1000U);
}

#if 0
/**
  Baseline ufs_mtk_post_link(): enable the UniPro clock-gating features.
**/
STATIC
EFI_STATUS
MtkUfsPostLink (
  IN EDKII_UFS_HC_DRIVER_INTERFACE *Interface
  )
{
  EFI_STATUS Status;
  UINT32     Value;

  Status = MtkUfsDmeGet (Interface, UFS_VS_SAVEPOWERCONTROL, &Value);
  if (EFI_ERROR (Status)) return Status;
  Value |= UFS_SAVEPOWER_RX_SYMBOL_CLK_GATE |
           UFS_SAVEPOWER_SYS_CLK_GATE |
           UFS_SAVEPOWER_TX_CLK_GATE;
  Status = MtkUfsExecUic (Interface, UFS_UIC_DME_SET,
                          (UFS_VS_SAVEPOWERCONTROL << 16), Value, NULL);
  if (EFI_ERROR (Status)) return Status;

  Status = MtkUfsDmeGet (Interface, UFS_VS_DEBUGCLOCKENABLE, &Value);
  if (EFI_ERROR (Status)) return Status;
  Value &= ~UFS_DEBUGCLK_TX_SYMBOL_CLK_REQ_FORCE;
  return MtkUfsExecUic (Interface, UFS_UIC_DME_SET,
                        (UFS_VS_DEBUGCLOCKENABLE << 16), Value, NULL);
}
#endif

STATIC
EFI_STATUS
EFIAPI
MtkUfsCallback (
  IN EFI_HANDLE Controller,
  IN EDKII_UFS_HC_PLATFORM_CALLBACK_PHASE Phase,
  IN OUT VOID *Data
  )
{
  EDKII_UFS_HC_DRIVER_INTERFACE *Interface;
  EFI_STATUS Status;
  UINT32 Value;

  if (Data == NULL) return EFI_INVALID_PARAMETER;
  Interface = (EDKII_UFS_HC_DRIVER_INTERFACE *)Data;
  if (Phase == EdkiiUfsHcPostHce) {
    /* The HCE reset clears AHIT, so restore the idle timer after enabling. */
    MmioWrite32 (YUNLUO_UFS_HCI_BASE + 0x18, YUNLUO_UFS_AHIT_DEFAULT);
    if (!mMtkUfsQuiet) {
      DEBUG ((DEBUG_ERROR, "UFS MTK: PostHce AHIT=0x%x\n", MmioRead32 (YUNLUO_UFS_HCI_BASE + 0x18)));
    }

    return EFI_SUCCESS;
  }
  if (Phase == EdkiiUfsHcPreHce) {
    /*
     * Baseline order: device reset first (HCE still off), then the
     * HCE-notify host reset, then crypto and refclk.
     */
    MtkUfsDeviceReset ();
    MtkUfsResetAndEnable ();

    MmioWrite32 (YUNLUO_UFS_HCI_BASE + 0x18, YUNLUO_UFS_AHIT_DEFAULT);
    if (!mMtkUfsQuiet) {
      DEBUG ((DEBUG_ERROR, "UFS MTK: AHIT -> 0x%x\n", MmioRead32 (YUNLUO_UFS_HCI_BASE + 0x18)));
    }

    /*
     * UEFI has no inline-crypto key setup.  Keep the MTK crypto engine off;
     * enabling it via SMC without configuring keys leaves the HCI in a host
     * error state and UTP query requests never complete.
     */
    MtkUfsStatusText (L"UFS MTK: crypto engine left off (no keys)\r\n");

    Status = MtkUfsRefClkOn ();
    MtkUfsStatusText (EFI_ERROR (Status) ? L"UFS MTK: PreHce refclk=FAIL\r\n" : L"UFS MTK: PreHce refclk=OK\r\n");
    if (!mMtkUfsQuiet) {
      MtkUfsPause (MTK_UFS_PHOTO_STALL_MS / 2);
    }

    return Status;
  }
  if (Phase == EdkiiUfsHcPreLinkStartup) {
    /*
     * Baseline ufs_mtk_pre_link() order:
     *   PA_LOCALVERINFO -> exit UniPro LPM -> disable host TX LCC ->
     *   clear VS_SAVEPOWERCONTROL deep stall.
     */
    Value = 0;
    Status = MtkUfsDmeGet (Interface, UFS_PA_LOCALVERINFO, &Value);
    if (!mMtkUfsQuiet) {
      DEBUG ((
        DEBUG_ERROR,
        "UFS MTK: PA_LOCALVERINFO status=%r value=0x%x\n",
        Status,
        Value
        ));
    }

    /* The baseline keeps its default version when the DME get fails. */

    Status = MtkUfsExecUic (Interface, UFS_UIC_DME_SET,
                            (UFS_VS_UNIPRO_POWERDOWN << 16), 0, NULL);
    MtkUfsStatusText (EFI_ERROR (Status) ? L"UFS MTK: UNIPRO_LPM=FAIL\r\n" : L"UFS MTK: UNIPRO_LPM=OK\r\n");
    if (EFI_ERROR (Status)) return Status;

    Status = MtkUfsExecUic (Interface, UFS_UIC_DME_SET,
                            (UFS_PA_LOCAL_TX_LCC << 16), 0, NULL);
    MtkUfsStatusText (EFI_ERROR (Status) ? L"UFS MTK: TX_LCC=FAIL\r\n" : L"UFS MTK: TX_LCC=OK\r\n");
    if (EFI_ERROR (Status)) return Status;

    Status = MtkUfsDmeGet (Interface, UFS_VS_SAVEPOWERCONTROL, &Value);
    MtkUfsStatusText (EFI_ERROR (Status) ? L"UFS MTK: SAVEPOWER get=FAIL\r\n" : L"UFS MTK: SAVEPOWER get=OK\r\n");
    if (EFI_ERROR (Status)) return Status;
    if (!mMtkUfsQuiet) {
      DEBUG ((DEBUG_ERROR, "UFS MTK: SAVEPOWER=0x%x\n", Value));
    }

    Value &= ~UFS_SAVEPOWER_DEEP_STALL;
    Status = MtkUfsExecUic (Interface, UFS_UIC_DME_SET,
                            (UFS_VS_SAVEPOWERCONTROL << 16), Value, NULL);
    MtkUfsStatusText (EFI_ERROR (Status) ? L"UFS MTK: SAVEPOWER set=FAIL\r\n" : L"UFS MTK: SAVEPOWER set=OK\r\n");
    if (EFI_ERROR (Status)) return Status;

    /* UFS_LINK_STARTUP is performed by the generic UfsDeviceDetection path. */
    return EFI_SUCCESS;
  }
  if (Phase == EdkiiUfsHcPostLinkStartup) {
    MtkUfsClearHciErrors ();

    /*
     * Throughput is flat at roughly a megabyte per second no matter the
     * request size, which is the order of magnitude of a PWM link rather
     * than an HS one.  Record what the negotiation settled on.  Bit 0 of
     * hsseries is PWM-G1; bit 4 is HS-G1.
     */
    if (!mMtkUfsQuiet) {
      MtkUfsDumpAttr (Interface, "pwrmode", UFS_PA_PWRMODE);
      MtkUfsDumpAttr (Interface, "txgear", UFS_PA_TXGEAR);
      MtkUfsDumpAttr (Interface, "rxgear", UFS_PA_RXGEAR);
      MtkUfsDumpAttr (Interface, "hsseries", UFS_PA_HSSERIES);
      MtkUfsDumpAttr (Interface, "txpwr", UFS_PA_TXPWRSTATUS);
      MtkUfsDumpAttr (Interface, "rxpwr", UFS_PA_RXPWRSTATUS);
      MtkUfsDumpAttr (Interface, "txlanes", UFS_PA_ACTIVETXDATALANES);
      MtkUfsDumpAttr (Interface, "maxtxfast", UFS_PA_MAXTXSPEEDFAST);
      MtkUfsDumpAttr (Interface, "maxtxslow", UFS_PA_MAXTXSPEEDSLOW);
      MtkUfsDumpAttr (Interface, "txterm", UFS_PA_TXTERMINATION);
      DEBUG ((DEBUG_ERROR, "UFS MTK: hcs=0x%x ahit=0x%x\n",
              MmioRead32 (YUNLUO_UFS_HCI_BASE + 0x04),
              MmioRead32 (YUNLUO_UFS_HCI_BASE + 0x18)));
    }

    MtkUfsChangePwrModeHs (Interface);
    if (!mMtkUfsQuiet) {
      MtkUfsDumpAttr (Interface, "HS pwrmode", UFS_PA_PWRMODE);
      MtkUfsDumpAttr (Interface, "HS txgear", UFS_PA_TXGEAR);
      MtkUfsDumpAttr (Interface, "HS txpwr", UFS_PA_TXPWRSTATUS);
      MtkUfsDumpAttr (Interface, "HS txterm", UFS_PA_TXTERMINATION);
    }

    /* CG-off experiment: do not enable UniPro clock gating. */
    Status = EFI_SUCCESS;
    MtkUfsStatusText (L"UFS MTK: post-link CG off\r\n");
    if (!mMtkUfsQuiet) {
      MtkUfsPause (MTK_UFS_PHOTO_STALL_MS / 2);
      DEBUG ((DEBUG_ERROR, "UFS MTK: link up, later cycles go quiet\n"));
      mMtkUfsQuiet = TRUE;
    }

    return EFI_SUCCESS;
  }
  return EFI_SUCCESS;
}

STATIC
VOID
MtkUfsClearHciErrors (
  VOID
  )
{
  /* UFS HCI IS and UEC* are write-1-to-clear.  The generic UfsPassThru
     path clears UCCS only, leaving UE/UECPA set and blocking UTRL work. */
  MmioWrite32 (YUNLUO_UFS_HCI_BASE + 0x20, 0xFFFFFFFF); // IS
  MmioWrite32 (YUNLUO_UFS_HCI_BASE + 0x38, 0xFFFFFFFF); // UECPA
  MmioWrite32 (YUNLUO_UFS_HCI_BASE + 0x3C, 0xFFFFFFFF); // UECDL
  MmioWrite32 (YUNLUO_UFS_HCI_BASE + 0x40, 0xFFFFFFFF); // UECN
  MmioWrite32 (YUNLUO_UFS_HCI_BASE + 0x44, 0xFFFFFFFF); // UECT
  MmioWrite32 (YUNLUO_UFS_HCI_BASE + 0x48, 0xFFFFFFFF); // UECDME
}

STATIC
VOID
EFIAPI
MtkUfsDumpEvent (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  STATIC UINTN DumpCount;

  DEBUG ((
    DEBUG_ERROR,
    "UFS MTK: post-init[%u] HCS=0x%x IS=0x%x UECPA=0x%x UECDL=0x%x UECN=0x%x UECT=0x%x UECD=0x%x\n",
    (UINT32)DumpCount,
    MmioRead32 (YUNLUO_UFS_HCI_BASE + 0x30),
    MmioRead32 (YUNLUO_UFS_HCI_BASE + 0x20),
    MmioRead32 (YUNLUO_UFS_HCI_BASE + 0x38),
    MmioRead32 (YUNLUO_UFS_HCI_BASE + 0x3C),
    MmioRead32 (YUNLUO_UFS_HCI_BASE + 0x40),
    MmioRead32 (YUNLUO_UFS_HCI_BASE + 0x44),
    MmioRead32 (YUNLUO_UFS_HCI_BASE + 0x48)
    ));

  /* Repost after the Shell clears the screen, then stop. */
  DumpCount++;
  if (DumpCount < MTK_UFS_DUMP_COUNT) {
    gBS->SetTimer (Event, TimerRelative, MTK_UFS_DUMP_PERIOD_100NS);
  } else {
    gBS->CloseEvent (Event);
  }
}

EFI_STATUS EFIAPI MtkUfsEntryPoint (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;
  EFI_HANDLE Handle;
  EFI_EVENT DumpEvent;
  mPrivate = AllocateZeroPool (sizeof (*mPrivate));
  if (mPrivate == NULL) return EFI_OUT_OF_RESOURCES;
  mPrivate->Signature = UFS_HC_PRIVATE_SIGNATURE;
  mPrivate->Hc.GetUfsHcMmioBar = MtkUfsGetMmioBar;
  mPrivate->Hc.AllocateBuffer = MtkUfsAllocateCommonBuffer;
  mPrivate->Hc.FreeBuffer = MtkUfsFreeBuffer;
  mPrivate->Hc.Map = MtkUfsMap;
  mPrivate->Hc.Unmap = MtkUfsUnmap;
  mPrivate->Hc.Flush = MtkUfsFlush;
  mPrivate->Hc.Read = MtkUfsRead;
  mPrivate->Hc.Write = MtkUfsWrite;
  mPrivate->Platform.Version = EDKII_UFS_HC_PLATFORM_PROTOCOL_VERSION;
  mPrivate->Platform.OverrideHcInfo = MtkUfsOverrideInfo;
  mPrivate->Platform.Callback = MtkUfsCallback;
  mPrivate->Platform.RefClkFreq = EdkiiUfsCardRefClkFreqObsolete;
  mPrivate->Platform.SkipLinkStartup = FALSE;
  MtkUfsPhyPowerOn ();
  MtkUfsResetAndEnable ();
  SetMem (&mPrivate->DevicePath, sizeof (mPrivate->DevicePath), 0);
  mPrivate->DevicePath.Vendor.Header.Type = HARDWARE_DEVICE_PATH;
  mPrivate->DevicePath.Vendor.Header.SubType = HW_VENDOR_DP;
  SetDevicePathNodeLength (&mPrivate->DevicePath.Vendor.Header, sizeof (VENDOR_DEVICE_PATH));
  mPrivate->DevicePath.End.Type = END_DEVICE_PATH_TYPE;
  mPrivate->DevicePath.End.SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
  SetDevicePathNodeLength (&mPrivate->DevicePath.End, sizeof (EFI_DEVICE_PATH));
  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (&Handle,
          &gEdkiiUfsHostControllerProtocolGuid, &mPrivate->Hc,
          &gEdkiiUfsHcPlatformProtocolGuid, &mPrivate->Platform,
          &gEfiDevicePathProtocolGuid, &mPrivate->DevicePath,
          NULL);
  if (EFI_ERROR (Status)) {
    MtkUfsStatusText (L"\r\nUFS HC install failed\r\n");
    FreePool (mPrivate);
    return Status;
  }

  mPrivate->Handle = Handle;
  /* ConOut is not available during DXE dispatch. BDS connects this handle
     after the console is ready from PlatformBootManagerAfterConsole(). */
  DumpEvent = NULL;
  if (!EFI_ERROR (gBS->CreateEvent (EVT_TIMER, TPL_CALLBACK, MtkUfsDumpEvent, NULL, &DumpEvent))) {
    gBS->SetTimer (DumpEvent, TimerRelative, MTK_UFS_DUMP_PERIOD_100NS);
  }
  return EFI_SUCCESS;
}
