/** @file
  Audit the three subsystems an ARM64 OS depends on at the moment we hand over:
  the GIC interrupt path, the architected timer, and the MTK clock tree.

  Nothing in this firmware has ever proven that an interrupt is actually
  delivered -- UFS is polled, and every console timestamp comes from a
  free-running counter.  So ask the hardware instead of reading code:

  1. does the Timer Architectural Protocol's tick counter advance on its own
     (it only moves inside the timer ISR, so this alone proves GIC delivery);
  2. does a software-generated interrupt reach a handler we install;
  3. in which security group do the SGI/PPI sources sit, and is the CPU
     interface enabled at all;
  4. how much of the MT6789 clock tree the driver can actually enumerate.

  All of it runs during DXE dispatch, long before ExitBootServices, so it
  cannot change what the OS inherits.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/ArmLib.h>
#include <Library/ArmGenericTimerCounterLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>
#include <Protocol/HardwareInterrupt.h>
#include <Protocol/MtkClock.h>
#include <Protocol/Timer.h>
#include <Guid/Acpi.h>

#define GICD_CTLR      0x000
#define GICD_TYPER     0x004
#define GICD_IGROUPR   0x080
#define GICD_ISENABLER 0x100

#define GICR_TYPER      0x008
#define GICR_WAKER      0x014
// IGROUPR0/ISENABLER0 are not in the RD_base frame: they live in the 64 KiB
// SGI_base frame that follows it (EDK2: ARM_GICR_CTLR_FRAME_SIZE).
#define GICR_SGI_FRAME  0x10000
#define GICR_IGROUPR0   (GICR_SGI_FRAME + 0x080)
#define GICR_ISENABLER0 (GICR_SGI_FRAME + 0x100)

// System registers, encoded by hand because ArmLib exposes no readers for them
#define SYS_ICC_PMR_EL1       "s3_0_c4_c6_0"
#define SYS_ICC_SRE_EL1       "s3_0_c12_c12_5"
#define SYS_ICC_IGRPEN0_EL1   "s3_0_c12_c12_6"
#define SYS_ICC_IGRPEN1_EL1   "s3_0_c12_c12_7"
#define SYS_ICC_SGI1R_EL1     "s3_0_c12_c11_5"

// INTID 1 is a free SGI on this platform (0 is what EDK2 would use for IPIs)
#define AUDIT_SGI_INTID  1
#define AUDIT_SGI_VALUE  ((AUDIT_SGI_INTID << 24) | 0x1)

STATIC volatile UINTN               mAuditSgiHits;

STATIC EFI_HARDWARE_INTERRUPT_PROTOCOL  *mAuditInterrupt;

STATIC
UINT64
ReadSysReg64 (
  VOID
  )
{
  UINT64  Value;

  Value = 0;
  __asm__ volatile ("mrs %0, " SYS_ICC_IGRPEN1_EL1 : "=r" (Value));

  return Value;
}

STATIC
UINT64
ReadPmr (
  VOID
  )
{
  UINT64  Value;

  Value = 0;
  __asm__ volatile ("mrs %0, " SYS_ICC_PMR_EL1 : "=r" (Value));

  return Value;
}

STATIC
UINT64
ReadSre (
  VOID
  )
{
  UINT64  Value;

  Value = 0;
  __asm__ volatile ("mrs %0, " SYS_ICC_SRE_EL1 : "=r" (Value));

  return Value;
}

STATIC
VOID
FireSgiToSelf (
  VOID
  )
{
  UINT64  Value;

  Value = AUDIT_SGI_VALUE;
  __asm__ volatile ("msr " SYS_ICC_SGI1R_EL1 ", %0\n isb" :: "r" (Value));
}

STATIC
VOID
EFIAPI
AuditSgiHandler (
  IN  HARDWARE_INTERRUPT_SOURCE  Source,
  IN  EFI_SYSTEM_CONTEXT         SystemContext
  )
{
  mAuditSgiHits++;
  mAuditInterrupt->EndOfInterrupt (mAuditInterrupt, Source);
}


STATIC
UINT64
ReadSys (
  IN CONST CHAR8  *Name
  )
{
  UINT64  Value;

  Value = 0;
  if (Name[0] == 'c') {
    __asm__ volatile ("mrs %0, cpacr_el1" : "=r" (Value));
  } else if (Name[0] == 's') {
    __asm__ volatile ("mrs %0, sctlr_el1" : "=r" (Value));
  } else if (Name[0] == 'p') {
    __asm__ volatile ("mrs %0, id_aa64pfr0_el1" : "=r" (Value));
  } else {
    __asm__ volatile ("mrs %0, CurrentEL" : "=r" (Value));
  }

  return Value;
}

STATIC
UINTN
RegionBase (
  IN CHAR8  *Name
  )
{
  EFI_MEMORY_REGION_DESCRIPTOR  Region;

  if (EFI_ERROR (LocateMemoryRegionByName (Name, &Region))) {
    return 0;
  }

  return (UINTN)Region.Address;
}

STATIC
VOID
AuditInterrupts (
  IN EFI_HANDLE  ImageHandle
  )
{
  EFI_STATUS                    Status;
  EFI_TIMER_ARCH_PROTOCOL       *Timer;
  EFI_HARDWARE_INTERRUPT_PROTOCOL   *Interrupt;
  UINT64                        TimerPeriod;
  UINTN                         Gicd;
  UINTN                         Gicr;

  Status = gBS->LocateProtocol (&gEfiTimerArchProtocolGuid, NULL, (VOID **)&Timer);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "AUDIT: no timer proto %r\n", Status));
    return;
  }

  Status = gBS->LocateProtocol (&gHardwareInterruptProtocolGuid, NULL, (VOID **)&Interrupt);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "AUDIT: no int proto %r\n", Status));
    return;
  }

  TimerPeriod = 0;
  Timer->GetTimerPeriod (Timer, &TimerPeriod);

  //
  // A DXE timer event is only signalled from the core's tick, and the core's
  // tick only advances inside the timer ISR.  So a periodic event that never
  // fires means the timer interrupt never reached the CPU.
  //
  {
    EFI_EVENT  TickEvent;
    UINT32     Fired;
    UINT32     Poll;

    Fired     = 0;
    TickEvent = NULL;
    if (!EFI_ERROR (gBS->CreateEvent (EVT_TIMER, TPL_APPLICATION, NULL, NULL, &TickEvent))) {
      gBS->SetTimer (TickEvent, TimerPeriodic, 10 * 10000);   // 10 ms, in 100 ns units
      for (Poll = 0; Poll < 30; Poll++) {
        gBS->Stall (10000);
        if (!EFI_ERROR (gBS->CheckEvent (TickEvent))) {
          Fired++;
        }
      }

      gBS->SetTimer (TickEvent, TimerCancel, 0);
      gBS->CloseEvent (TickEvent);
    }

    DEBUG ((
      DEBUG_ERROR,
      "AUDIT: dxe tick polls=%u/30 IRQ=%a period=%ld\n",
      Fired,
      (Fired > 0) ? "ALIVE" : "DEAD",
      TimerPeriod
      ));
  }

  Gicd = RegionBase ("GIC Distributor");
  Gicr = RegionBase ("GIC Redistributors");

  DEBUG ((DEBUG_ERROR, "AUDIT: GICD ctl=%08X typ=%08X grp=%08X,%08X en=%08X\n",
          MmioRead32 (Gicd + GICD_CTLR),
          MmioRead32 (Gicd + GICD_TYPER),
          MmioRead32 (Gicd + GICD_IGROUPR),
          MmioRead32 (Gicd + GICD_IGROUPR + 4),
          MmioRead32 (Gicd + GICD_ISENABLER)));
  DEBUG ((DEBUG_ERROR, "AUDIT: GICR typ=%016lX wak=%08X grp=%08X en=%08X\n",
          MmioRead64 (Gicr + GICR_TYPER),
          MmioRead32 (Gicr + GICR_WAKER),
          MmioRead32 (Gicr + GICR_IGROUPR0),
          MmioRead32 (Gicr + GICR_ISENABLER0)));
  DEBUG ((DEBUG_ERROR, "AUDIT: ICC sre=%08X pmr=%02X igrpen1=%08X mpidr=%016lX\n",
          (UINT32)ReadSre (),
          (UINT32)ReadPmr (),
          (UINT32)ReadSysReg64 (),
          ArmReadMpidr ()));

  //
  // Independent of the timer: generate an SGI and see if our handler runs.
  //
  mAuditSgiHits = 0;
  mAuditInterrupt = Interrupt;
  Status = Interrupt->RegisterInterruptSource (Interrupt, AUDIT_SGI_INTID, AuditSgiHandler);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "AUDIT: sgi register %r\n", Status));
    return;
  }

  Interrupt->EnableInterruptSource (Interrupt, AUDIT_SGI_INTID);
  FireSgiToSelf ();
  gBS->Stall (200000);
  DEBUG ((DEBUG_ERROR, "AUDIT: sgi hits=%u %a\n",
          (UINT32)mAuditSgiHits,
          (mAuditSgiHits > 0) ? "DELIVERED" : "NOT-DELIVERED"));
  Interrupt->DisableInterruptSource (Interrupt, AUDIT_SGI_INTID);
}

STATIC
VOID
AuditTimer (
  VOID
  )
{
  UINT64  Freq;
  UINT64  Count1;
  UINT64  Count2;

  Freq = ArmGenericTimerGetTimerFreq ();
  gBS->Stall (100000);
  Count1  = ArmGenericTimerGetSystemCount ();
  gBS->Stall (100000);
  Count2  = ArmGenericTimerGetSystemCount ();

  DEBUG ((DEBUG_ERROR, "AUDIT: CNTFRQ=%ld Hz count %ld -> %ld (d=%ld)\n",
          Freq, Count1, Count2, Count2 - Count1));
  DEBUG ((DEBUG_ERROR, "AUDIT: CNTP_CTL=%08X CVAL=%016lX\n",
          (UINT32)ArmGenericTimerGetTimerCtrlReg (),
          ArmGenericTimerGetCompareVal ()));
  DEBUG ((DEBUG_ERROR, "AUDIT: curEL=%ld cpacr=%08lX sctlr=%016lX pfr0=%016lX\n",
          ReadSys ("e") >> 2,
          ReadSys ("c"),
          ReadSys ("s"),
          ReadSys ("p")));
}


//
// LK boots this firmware as an Android "kernel" from a v4 boot image plus the
// stock vendor_boot, so the DTB it passes is the *vendor* one -- the authoritative
// list of reserved regions on this board.  Anything that list reserves but our
// EFI memory map hands to the OS as free RAM can be written by the OS while
// TEE/SCP/SSPM/DRAMC firmware still owns it.
//
#define FDT_MAGIC        0xD00DFEEDU
#define DTB_SCAN_BASE    0x47C00000U
#define DTB_SCAN_END     0x47D00000U

STATIC
UINT32
FdtBe32 (
  IN CONST UINT8  *P
  )
{
  return ((UINT32)P[0] << 24) | ((UINT32)P[1] << 16) | ((UINT32)P[2] << 8) | P[3];
}

STATIC
BOOLEAN
MapHasReserved (
  IN EFI_MEMORY_DESCRIPTOR  *Map,
  IN UINTN                  Count,
  IN UINTN                  DescSize,
  IN UINT64                 Base,
  IN UINT64                 Size
  )
{
  EFI_MEMORY_DESCRIPTOR  *Desc;
  UINTN                  Index;

  for (Index = 0, Desc = Map; Index < Count; Index++, Desc = NEXT_MEMORY_DESCRIPTOR (Desc, DescSize)) {
    UINT64 Start = Desc->PhysicalStart;
    UINT64 End   = Start + LShiftU64 (Desc->NumberOfPages, EFI_PAGE_SHIFT);

    if (Desc->Type != EfiReservedMemoryType) {
      continue;
    }

    if ((Base >= Start) && (Base + Size <= End)) {
      return TRUE;
    }
  }

  return FALSE;
}

STATIC
VOID
AuditDtbReserved (
  VOID
  )
{
  EFI_STATUS             Status;
  EFI_MEMORY_DESCRIPTOR  *Map;
  UINTN                  Size;
  UINTN                  MapKey;
  UINTN                  DescSize;
  UINT32                 DescVersion;
  UINTN                  Count;
  UINT8                  *Fdt;
  UINT32                 Total;
  UINT32                 OffStruct;
  UINT32                 P;
  UINT32                 Depth;
  BOOLEAN                InRsv;
  UINT32                 Nodes;
  UINT32                 Miss;
  UINT32                 RsvDepth;
  UINTN                  Scan;

  Fdt = NULL;
  for (Scan = DTB_SCAN_BASE; Scan < DTB_SCAN_END; Scan += 8) {
    if (FdtBe32 ((UINT8 *)Scan) == FDT_MAGIC) {
      UINT32 T = FdtBe32 ((UINT8 *)Scan + 4);
      UINT32 OS = FdtBe32 ((UINT8 *)Scan + 8);
      UINT32 SS = FdtBe32 ((UINT8 *)Scan + 12);
      if ((T > 0x100) && (T < 0x400000) && (OS < T) && (SS < T)) {
        Fdt = (UINT8 *)Scan;
        break;
      }
    }
  }

  if (Fdt == NULL) {
    DEBUG ((DEBUG_ERROR, "AUDIT: no dtb\n"));
    return;
  }

  Total     = FdtBe32 (Fdt + 4);
  OffStruct = FdtBe32 (Fdt + 8);
  DEBUG ((DEBUG_ERROR, "AUDIT: dtb=0x%lx size=0x%x\n", (UINT64)(UINTN)Fdt, Total));

  Size   = 0;
  MapKey = 0;
  Status = gBS->GetMemoryMap (&Size, NULL, &MapKey, &DescSize, &DescVersion);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    return;
  }

  Map = AllocatePool (Size + (2 * DescSize));
  if (Map == NULL) {
    return;
  }

  if (EFI_ERROR (gBS->GetMemoryMap (&Size, Map, &MapKey, &DescSize, &DescVersion))) {
    FreePool (Map);
    return;
  }

  Count = Size / DescSize;
  P     = OffStruct;
  Depth = 0;
  InRsv = FALSE;
  Nodes    = 0;
  Miss     = 0;
  RsvDepth = 0;

  while (P + 8 < Total) {
    UINT32 Tag = FdtBe32 (Fdt + P);

    if (Tag == 1) {                 // FDT_BEGIN_NODE
      CONST CHAR8  *Name = (CONST CHAR8 *)(Fdt + P + 4);
      UINT32       Len   = 0;

      while (Name[Len] != 0) {
        Len++;
      }

      Depth++;
      if ((Len == 15) && (AsciiStrnCmp (Name, "reserved-memory", 15) == 0)) {
        InRsv    = TRUE;
        RsvDepth = Depth;
      }

      P += 4 + Len + 1;
      P  = (P + 3) & ~3U;
      continue;
    }

    if (Tag == 3) {                 // FDT_PROP
      UINT32 Len  = FdtBe32 (Fdt + P + 4);
      UINT32 Name = FdtBe32 (Fdt + P + 8);

      if (InRsv && (Depth == RsvDepth + 1) && (Len == 16) && (Name != 0)) {
        CONST CHAR8  *Prop = (CONST CHAR8 *)(Fdt + FdtBe32 (Fdt + 12) + Name);

        if (AsciiStrCmp (Prop, "reg") == 0) {
          CONST UINT8  *R = Fdt + P + 12;
          UINT32       AHi = FdtBe32 (R);
          UINT32       ALo = FdtBe32 (R + 4);
          UINT32       SHi = FdtBe32 (R + 8);
          UINT32       SLo = FdtBe32 (R + 12);

          if ((SHi == 0) && (SLo != 0) && ((AHi == 0) || (AHi == 1) || (AHi == 2))) {
            UINT64 Base = LShiftU64 ((UINT64)AHi, 32) | ALo;
            UINT64 Sz   = SLo;

            Nodes++;
            if (!MapHasReserved (Map, Count, DescSize, Base, Sz)) {
              Miss++;
              if (Miss <= 6) {
                DEBUG ((DEBUG_ERROR, "AUDIT: M 0x%lx+0x%lx\n", Base, Sz));
              }
            }
          }
        }
      }

      P += 12 + Len;
      P  = (P + 3) & ~3U;
      continue;
    }

    if (Tag == 2) {                 // FDT_END_NODE
      if (InRsv && (Depth == RsvDepth)) {
        InRsv = FALSE;
      }

      if (Depth > 0) {
        Depth--;
      }

      P += 4;
      continue;
    }

    if (Tag == 4) {                 // FDT_NOP
      P += 4;
      continue;
    }

    if (Tag == 9) {                 // FDT_END
      break;
    }

    break;
  }

  DEBUG ((DEBUG_ERROR, "AUDIT: dtb rsv=%u missing=%u\n", Nodes, Miss));
  FreePool (Map);
}

STATIC
VOID
AuditClocks (
  VOID
  )
{
  EFI_STATUS           Status;
  MTK_CLOCK_PROTOCOL   *Clock;
  CONST CHAR8          *Name;
  UINT32               Id;
  UINT32               Total;
  UINT32               Enabled;
  UINT32               Failed;
  BOOLEAN              On;

  Status = gBS->LocateProtocol (&gMediaTekClockProtocolGuid, NULL, (VOID **)&Clock);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "AUDIT: no clock proto %r\n", Status));
    return;
  }

  Total   = 0;
  Enabled = 0;
  Failed  = 0;
  for (Id = 0; Id < 4096; Id++) {
    Name = NULL;
    if (EFI_ERROR (Clock->GetName (Id, &Name)) || (Name == NULL)) {
      break;
    }

    Total++;
    On     = FALSE;
    Status = Clock->IsEnabled (Id, &On);
    if (EFI_ERROR (Status)) {
      Failed++;
    } else if (On) {
      Enabled++;
    }
  }

  DEBUG ((DEBUG_ERROR, "AUDIT: clocks total=%u enabled=%u readfail=%u\n",
          Total, Enabled, Failed));
}


STATIC
VOID
AuditMemoryMap (
  VOID
  )
{
  EFI_STATUS             Status;
  EFI_MEMORY_DESCRIPTOR  *Map;
  EFI_MEMORY_DESCRIPTOR  *Desc;
  UINTN                  Size;
  UINTN                  MapKey;
  UINTN                  DescSize;
  UINT32                 DescVersion;
  UINTN                  Count;
  UINTN                  Index;
  UINT64                 Top;
  UINT64                 End;

  Size   = 0;
  MapKey = 0;
  Status = gBS->GetMemoryMap (&Size, NULL, &MapKey, &DescSize, &DescVersion);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    DEBUG ((DEBUG_ERROR, "AUDIT: map probe %r\n", Status));
    return;
  }

  Map = AllocatePool (Size + (2 * DescSize));
  if (Map == NULL) {
    return;
  }

  Status = gBS->GetMemoryMap (&Size, Map, &MapKey, &DescSize, &DescVersion);
  if (EFI_ERROR (Status)) {
    FreePool (Map);
    return;
  }

  Count = Size / DescSize;
  Top   = 0;
  for (Index = 0, Desc = Map; Index < Count; Index++, Desc = NEXT_MEMORY_DESCRIPTOR (Desc, DescSize)) {
    End = Desc->PhysicalStart + LShiftU64 (Desc->NumberOfPages, EFI_PAGE_SHIFT);
    if (End > Top) {
      Top = End;
    }
  }

  DEBUG ((DEBUG_ERROR, "AUDIT: map n=%u top=0x%lx\n", (UINT32)Count, Top));
  FreePool (Map);
}


STATIC
VOID
AuditAcpiLocation (
  VOID
  )
{
  EFI_STATUS             Status;
  EFI_MEMORY_DESCRIPTOR  *Map;
  EFI_MEMORY_DESCRIPTOR  *Desc;
  UINTN                  Size;
  UINTN                  MapKey;
  UINTN                  DescSize;
  UINT32                 DescVersion;
  UINTN                  Count;
  UINTN                  Index;
  UINTN                  Table;
  UINT64                 Rsdp;

  Rsdp = 0;
  for (Table = 0; Table < gST->NumberOfTableEntries; Table++) {
    if (CompareGuid (&gST->ConfigurationTable[Table].VendorGuid, &gEfiAcpi20TableGuid) ||
        CompareGuid (&gST->ConfigurationTable[Table].VendorGuid, &gEfiAcpi10TableGuid))
    {
      Rsdp = (UINT64)(UINTN)gST->ConfigurationTable[Table].VendorTable;
      break;
    }
  }

  if (Rsdp == 0) {
    DEBUG ((DEBUG_ERROR, "AUDIT: no rsdp\n"));
    return;
  }

  Size   = 0;
  MapKey = 0;
  Status = gBS->GetMemoryMap (&Size, NULL, &MapKey, &DescSize, &DescVersion);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    return;
  }

  Map = AllocatePool (Size + (2 * DescSize));
  if (Map == NULL) {
    return;
  }

  Status = gBS->GetMemoryMap (&Size, Map, &MapKey, &DescSize, &DescVersion);
  if (EFI_ERROR (Status)) {
    FreePool (Map);
    return;
  }

  Count = Size / DescSize;
  for (Index = 0, Desc = Map; Index < Count; Index++, Desc = NEXT_MEMORY_DESCRIPTOR (Desc, DescSize)) {
    if ((Rsdp >= Desc->PhysicalStart) &&
        (Rsdp < Desc->PhysicalStart + LShiftU64 (Desc->NumberOfPages, EFI_PAGE_SHIFT)))
    {
      DEBUG ((
        DEBUG_ERROR,
        "AUDIT: rsdp=0x%lx type=%u attr=0x%lx base=0x%lx\n",
        Rsdp,
        Desc->Type,
        Desc->Attribute,
        Desc->PhysicalStart
        ));
      break;
    }
  }

  FreePool (Map);
}

EFI_STATUS
EFIAPI
HandoffAuditEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  AuditMemoryMap ();
  AuditAcpiLocation ();
  AuditDtbReserved ();
  AuditInterrupts (ImageHandle);
  AuditTimer ();
  AuditClocks ();

  return EFI_SUCCESS;
}
