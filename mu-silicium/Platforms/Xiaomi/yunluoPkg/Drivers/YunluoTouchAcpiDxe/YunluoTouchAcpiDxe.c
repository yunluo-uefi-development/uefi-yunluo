// SPDX-License-Identifier: BSD-2-Clause-Patent
// Enumerate the OS-owned touch consumer. No pinmux/reset/SRAM operation in DXE.
#include <Uefi.h>
#include <Protocol/AcpiTable.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/DebugLib.h>
#include "YunluoTouchSsdt.h"

EFI_STATUS EFIAPI
YunluoTouchAcpiEntryPoint (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_ACPI_TABLE_PROTOCOL *Acpi;
  EFI_STATUS Status;
  UINTN Key;
  (VOID)ImageHandle;
  (VOID)SystemTable;
  Status = gBS->LocateProtocol (&gEfiAcpiTableProtocolGuid, NULL, (VOID **)&Acpi);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = Acpi->InstallAcpiTable (Acpi, YunluoTouchSsdt, sizeof (YunluoTouchSsdt), &Key);
  DEBUG ((DEBUG_INFO, "YUNLUO-TOUCH: ACPI YUNT0001 install=%r; runtime GPIO is OS-owned\n", Status));
  return Status;
}
