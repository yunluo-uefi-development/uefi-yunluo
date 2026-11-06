/** @file
  UFS dependency bring-up self test for yunluo.

  Ported from baselines/kernel_xiaomi-yunluo:
    - 7 UFS clocks (infracfg_ao gates + topckgen muxes); parents set to
      MAINPLL_D4_D4 (ufs_sel, index 1) and MAINPLL_D4 (aes_ufsfde_sel, index 1)
    - 3 infracfg resets (hci/crypto/unipro)
    - VEMC (ldo_vemc) via the Mu PMIC protocol
    - TF-A UFS SMC: crypto enable + ref-clk notification + VCC info
    - UFS refclk request/ack handshake at HCI+0x144
    - read-only HCI/MPHY state dump

  It deliberately does NOT start the UFS link or install a UFS host
  controller.  The goal is to verify every dependency on screen first.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/ArmSmcLib.h>

#include <Protocol/MtkClock.h>
#include <Protocol/MtkPmic.h>

#define MTK_SIP_UFS_CONTROL       0xC2000276U
#define UFS_MTK_SIP_DEVICE_RESET  BIT1
#define UFS_MTK_SIP_CRYPTO_CTRL   BIT2
#define UFS_MTK_SIP_REFCLK_NOTIFY BIT3
#define UFS_MTK_SIP_GET_VCC_INFO  BIT6

#define INFRA_AO_BASE             0x10001000U
#define INFRA_RST_HCI_SET         0x130U
#define INFRA_RST_HCI_CLR         0x134U
#define INFRA_RST_UNIPRO_SET      0x140U
#define INFRA_RST_UNIPRO_CLR      0x144U
#define INFRA_RST_CRYPTO_SET      0x150U
#define INFRA_RST_CRYPTO_CLR      0x154U
#define RST_HCI_BIT               BIT15
#define RST_UNIPRO_BIT            BIT7
#define RST_CRYPTO_BIT            BIT21

#define UFS_HCI_BASE              0x11270000U
#define UFS_HCI_VERSION           0x0000U
#define UFS_XOUFS_CTRL            0x0140U
#define UFS_REFCLK_CTRL           0x0144U
#define UFS_MTK_HW_VER            0x2240U
#define REFCLK_REQUEST            BIT0
#define REFCLK_ACK                BIT1
#define REFCLK_TIMEOUT_US         3000U

#define UFS_PHY_BASE              0x01FA0000U
#define PHY_MP_GLB_DIG_8C         0x008CU
#define PHY_MP_LN_RX_44           0xB044U
#define PHY_MP_LN_DIG_RX_AC       0xA0ACU
#define PHY_MP_LN_DIG_RX_9C       0xA09CU

#define UFS_VEMC_NAME             "ldo_vemc"

STATIC CONST CHAR8 *mUfsEnableClocks[] = {
  "TOP_UFS_SEL",
  "TOP_AES_UFSFDE_SEL",
  "TOP_UFS",
  "TOP_AES_UFSFDE",
  "IFRAO_UFS",
  "IFRAO_UFS_SAP_BCLK",
  "IFRAO_UNIPRO_TICK",
  "IFRAO_UNIPRO_SYSCLK",
  "IFRAO_UFS_AES"
};

STATIC CONST CHAR8 *mUfsMuxClocks[] = {
  "TOP_UFS_SEL",
  "TOP_AES_UFSFDE_SEL"
};

STATIC
UINTN
MtkUfsSmc (
  IN  UINTN         Command,
  IN  UINTN         Arg1,
  IN  UINTN         Arg2,
  OUT ARM_SMC_ARGS  *Result OPTIONAL
  )
{
  ARM_SMC_ARGS  Args;

  ZeroMem (&Args, sizeof (Args));
  Args.Arg0 = MTK_SIP_UFS_CONTROL;
  Args.Arg1 = Command;
  Args.Arg2 = Arg1;
  Args.Arg3 = Arg2;
  ArmCallSmc (&Args);

  if (Result != NULL) {
    CopyMem (Result, &Args, sizeof (Args));
  }

  return Args.Arg0;
}

STATIC
BOOLEAN
UfsClockIsMux (
  IN CONST CHAR8  *ClockName
  )
{
  UINTN  Index;

  for (Index = 0; Index < ARRAY_SIZE (mUfsMuxClocks); Index++) {
    if (AsciiStrCmp (ClockName, mUfsMuxClocks[Index]) == 0) {
      return TRUE;
    }
  }

  return FALSE;
}

STATIC
EFI_STATUS
UfsSetClockParent (
  IN MTK_CLOCK_PROTOCOL  *Clock,
  IN CONST CHAR8         *ClockName,
  IN CONST CHAR8         *ParentName
  )
{
  EFI_STATUS  Status;
  UINT32      ClockId;
  UINT32      ParentId;

  Status = Clock->GetId (ClockName, &ClockId);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "UFSDEPS: clock %a not found: %r\n", ClockName, Status));
    return Status;
  }

  Status = Clock->GetId (ParentName, &ParentId);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "UFSDEPS: parent %a not found: %r\n", ParentName, Status));
    return Status;
  }

  Status = Clock->SetParent (ClockId, ParentId);
  DEBUG ((DEBUG_ERROR, "UFSDEPS: %a parent <- %a: %r\n", ClockName, ParentName, Status));
  return Status;
}

STATIC
VOID
UfsReportClock (
  IN MTK_CLOCK_PROTOCOL  *Clock,
  IN CONST CHAR8         *ClockName
  )
{
  EFI_STATUS  Status;
  UINT32      ClockId;
  UINT32      ParentId;
  UINT64      Frequency;
  BOOLEAN     Enabled;

  ParentId  = 0;
  Frequency = 0;
  Enabled   = FALSE;

  Status = Clock->GetId (ClockName, &ClockId);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "UFSDEPS: clock %a id: %r\n", ClockName, Status));
    return;
  }

  if (UfsClockIsMux (ClockName)) {
    Status = Clock->GetParent (ClockId, &ParentId);
    if (EFI_ERROR (Status)) {
      ParentId = 0xFFFFFFFFU;
    }
  }

  Status = Clock->GetFrequency (ClockId, &Frequency);
  if (EFI_ERROR (Status)) {
    Frequency = 0;
  }

  Status = Clock->IsEnabled (ClockId, &Enabled);
  if (EFI_ERROR (Status)) {
    Enabled = FALSE;
  }

  DEBUG ((
    DEBUG_ERROR,
    "UFSDEPS: clk %a id=%u parent=%u freq=%uMHz en=%u\n",
    ClockName,
    ClockId,
    ParentId,
    (UINT32)(Frequency / 1000000U),
    Enabled
    ));
}

STATIC
VOID
UfsPhyPowerOn (
  VOID
  )
{
  //
  // drivers/phy/mediatek/phy-mtk-ufs.c: ufs_mtk_phy_set_active().
  // The unipro/mp clocks are already enabled above.
  //
  MmioOr32  (UFS_PHY_BASE + PHY_MP_GLB_DIG_8C, BIT11);              // PLL_PWR_ON
  MmioAnd32 (UFS_PHY_BASE + PHY_MP_GLB_DIG_8C, ~BIT10);             // FRC_PLL_PWR_ON
  MmioAnd32 (UFS_PHY_BASE + PHY_MP_GLB_DIG_8C, ~(BIT9 | BIT8));     // PLL_ISO / FRC
  MmioOr32  (UFS_PHY_BASE + PHY_MP_LN_RX_44, BIT18);                // CDR_PWR_ON
  MmioAnd32 (UFS_PHY_BASE + PHY_MP_LN_RX_44, ~BIT17);               // FRC_CDR_PWR_ON
  MmioAnd32 (UFS_PHY_BASE + PHY_MP_LN_RX_44, ~(BIT20 | BIT19));     // CDR_ISO / FRC
  MmioOr32  (UFS_PHY_BASE + PHY_MP_LN_DIG_RX_AC, BIT1);             // RX_SQ_EN
  MmioAnd32 (UFS_PHY_BASE + PHY_MP_LN_DIG_RX_AC, ~BIT0);            // FRC_RX_SQ_EN
  gBS->Stall (1);
  MmioAnd32 (UFS_PHY_BASE + PHY_MP_LN_DIG_RX_9C, ~BIT18);           // DIFZ

  DEBUG ((DEBUG_ERROR, "UFSDEPS: mphy power-on sequence written\n"));
}

EFI_STATUS
EFIAPI
UfsDepsDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS          Status;
  MTK_CLOCK_PROTOCOL  *Clock;
  MTK_PMIC_PROTOCOL   *Pmic;
  ARM_SMC_ARGS        Smc;
  UINT32              ClockId;
  UINT32              Value;
  UINT32              Voltage;
  BOOLEAN             Enabled;
  UINTN               Index;
  UINTN               Elapsed;
  UINTN               SmcStatus;

  Clock = NULL;
  Pmic  = NULL;
  Voltage = 0;

  DEBUG ((DEBUG_ERROR, "\nUFSDEPS: begin (baseline dependency self test)\n"));

  Status = gBS->LocateProtocol (&gMediaTekClockProtocolGuid, NULL, (VOID **)&Clock);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "UFSDEPS: clock protocol missing: %r\n", Status));
    goto Pause;
  }

  //
  // Baseline clk-mt6789: ufs_sel and aes_ufsfde_sel default to parent 1
  // (TOP_MAINPLL_D4_D4 and TOP_MAINPLL_D4 respectively).
  //
  UfsSetClockParent (Clock, "TOP_UFS_SEL", "TOP_MAINPLL_D4_D4");
  UfsSetClockParent (Clock, "TOP_AES_UFSFDE_SEL", "TOP_MAINPLL_D4");

  for (Index = 0; Index < ARRAY_SIZE (mUfsEnableClocks); Index++) {
    Status = Clock->GetId (mUfsEnableClocks[Index], &ClockId);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "UFSDEPS: clock %a id: %r\n", mUfsEnableClocks[Index], Status));
      continue;
    }

    Status = Clock->SetEnable (ClockId, TRUE);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "UFSDEPS: clock %a enable: %r\n", mUfsEnableClocks[Index], Status));
    }
  }

  gBS->Stall (100);

  for (Index = 0; Index < ARRAY_SIZE (mUfsEnableClocks); Index++) {
    UfsReportClock (Clock, mUfsEnableClocks[Index]);
  }

  Status = gBS->LocateProtocol (&gMediaTekPmicProtocolGuid, NULL, (VOID **)&Pmic);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "UFSDEPS: pmic protocol missing: %r\n", Status));
    goto Pause;
  }

  Status = Pmic->RegulatorSetEnable (UFS_VEMC_NAME, TRUE);
  DEBUG ((DEBUG_ERROR, "UFSDEPS: ldo_vemc enable: %r\n", Status));
  gBS->Stall (100);

  Enabled = FALSE;
  Status  = Pmic->RegulatorIsEnabled (UFS_VEMC_NAME, &Enabled);
  DEBUG ((DEBUG_ERROR, "UFSDEPS: ldo_vemc is_enabled: %r en=%u\n", Status, Enabled));

  Status = Pmic->RegulatorGetVoltage (UFS_VEMC_NAME, &Voltage);
  DEBUG ((DEBUG_ERROR, "UFSDEPS: ldo_vemc voltage: %r uv=%u\n", Status, Voltage));

  UfsPhyPowerOn ();

  //
  // infracfg_ao resets: hci(0x130/15), unipro(0x140/7), crypto(0x150/21).
  // Assert all, wait, then deassert unipro -> crypto -> hci (vendor order).
  //
  MmioOr32 (INFRA_AO_BASE + INFRA_RST_HCI_SET,    RST_HCI_BIT);
  MmioOr32 (INFRA_AO_BASE + INFRA_RST_CRYPTO_SET, RST_CRYPTO_BIT);
  MmioOr32 (INFRA_AO_BASE + INFRA_RST_UNIPRO_SET, RST_UNIPRO_BIT);
  gBS->Stall (100);
  MmioOr32 (INFRA_AO_BASE + INFRA_RST_UNIPRO_CLR, RST_UNIPRO_BIT);
  MmioOr32 (INFRA_AO_BASE + INFRA_RST_CRYPTO_CLR, RST_CRYPTO_BIT);
  MmioOr32 (INFRA_AO_BASE + INFRA_RST_HCI_CLR,    RST_HCI_BIT);
  gBS->Stall (100);
  DEBUG ((DEBUG_ERROR, "UFSDEPS: resets assert/deassert done\n"));

  //
  // TF-A UFS SMC: crypto enable, then VCC info.
  //
  ZeroMem (&Smc, sizeof (Smc));
  SmcStatus = MtkUfsSmc (UFS_MTK_SIP_CRYPTO_CTRL, 1, 0, &Smc);
  DEBUG ((DEBUG_ERROR, "UFSDEPS: crypto smc=%u ret=0x%x\n", (UINT32)SmcStatus, (UINT32)Smc.Arg0));

  ZeroMem (&Smc, sizeof (Smc));
  SmcStatus = MtkUfsSmc (UFS_MTK_SIP_GET_VCC_INFO, 0, 0, &Smc);
  DEBUG ((DEBUG_ERROR, "UFSDEPS: vcc info smc=%u a1=%u\n", (UINT32)SmcStatus, (UINT32)Smc.Arg1));

  //
  // Ref clock: notify TF-A, release the controller refclk request and wait
  // for the ACK bit.  Baseline: 3 ms timeout, then 32-100 us settle.
  //
  ZeroMem (&Smc, sizeof (Smc));
  SmcStatus = MtkUfsSmc (UFS_MTK_SIP_REFCLK_NOTIFY, 1, 0, &Smc);
  DEBUG ((DEBUG_ERROR, "UFSDEPS: refclk smc=%u ret=0x%x\n", (UINT32)SmcStatus, (UINT32)Smc.Arg0));

  MmioOr32 (UFS_HCI_BASE + UFS_XOUFS_CTRL, BIT7);
  MmioOr32 (UFS_HCI_BASE + UFS_REFCLK_CTRL, REFCLK_REQUEST);

  Value = 0;
  for (Elapsed = 0; Elapsed < REFCLK_TIMEOUT_US; Elapsed += 10) {
    Value = MmioRead32 (UFS_HCI_BASE + UFS_REFCLK_CTRL);
    if (((Value & REFCLK_ACK) >> 1) == (Value & REFCLK_REQUEST)) {
      break;
    }

    gBS->Stall (10);
  }

  gBS->Stall (100);
  DEBUG ((
    DEBUG_ERROR,
    "UFSDEPS: refclk ctrl=0x%x %a (waited %uus)\n",
    Value,
    (Elapsed < REFCLK_TIMEOUT_US) ? "ACK" : "TIMEOUT",
    (UINT32)Elapsed
    ));

  //
  // Baseline ufshcd_init(): ufshcd_vops_device_reset() runs BEFORE
  // ufshcd_hba_enable().  The MTK vop pulses RST_n low (active low) for
  // >=10us, then high, and waits 10-15ms.  The old UEFI driver skipped this.
  //
  ZeroMem (&Smc, sizeof (Smc));
  SmcStatus = MtkUfsSmc (UFS_MTK_SIP_DEVICE_RESET, 0, 0, &Smc);
  DEBUG ((DEBUG_ERROR, "UFSDEPS: device reset low  smc=%u ret=0x%x\n", (UINT32)SmcStatus, (UINT32)Smc.Arg0));
  gBS->Stall (10);

  ZeroMem (&Smc, sizeof (Smc));
  SmcStatus = MtkUfsSmc (UFS_MTK_SIP_DEVICE_RESET, 1, 0, &Smc);
  DEBUG ((DEBUG_ERROR, "UFSDEPS: device reset high smc=%u ret=0x%x\n", (UINT32)SmcStatus, (UINT32)Smc.Arg0));
  gBS->Stall (10000);

  //
  // Read-only state dump.  The baseline does not drive the MPHY from Linux
  // (no phys phandle); LK/TF-A leaves it on.  If these look dead, the PHY
  // power sequence is the next dependency to port.
  //
  DEBUG ((
    DEBUG_ERROR,
    "UFSDEPS: hci ver=0x%x mtk_hw=0x%x\n",
    MmioRead32 (UFS_HCI_BASE + UFS_HCI_VERSION),
    MmioRead32 (UFS_HCI_BASE + UFS_MTK_HW_VER)
    ));

  DEBUG ((
    DEBUG_ERROR,
    "UFSDEPS: phy 8c=0x%x rx44=0x%x rxac=0x%x rx9c=0x%x\n",
    MmioRead32 (UFS_PHY_BASE + PHY_MP_GLB_DIG_8C),
    MmioRead32 (UFS_PHY_BASE + PHY_MP_LN_RX_44),
    MmioRead32 (UFS_PHY_BASE + PHY_MP_LN_DIG_RX_AC),
    MmioRead32 (UFS_PHY_BASE + PHY_MP_LN_DIG_RX_9C)
    ));

Pause:
  DEBUG ((DEBUG_ERROR, "UFSDEPS: DONE, pause 8s for photos\n"));
  gBS->Stall (8000000);

  return EFI_SUCCESS;
}
