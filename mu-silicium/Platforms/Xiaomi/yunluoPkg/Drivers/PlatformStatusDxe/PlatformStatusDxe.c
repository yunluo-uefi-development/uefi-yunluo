/** @file
  Periodic platform status dump for on-device (photo) diagnostics.

  Prints the ACPI RSDP/XSDT contents and the GOP mode after the Shell has
  started, so the information is not scrolled away by the UFS partition
  enumeration.  Debug aid only.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <PiDxe.h>
#include <Guid/Acpi.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/Shell.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>

STATIC VOID PrintAcpiStatus (VOID);
STATIC VOID PrintGopStatus (VOID);

STATIC
EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER *
FindRsdp (
  VOID
  )
{
  UINTN  Index;

  for (Index = 0; Index < gST->NumberOfTableEntries; Index++) {
    if (CompareGuid (&gST->ConfigurationTable[Index].VendorGuid, &gEfiAcpi20TableGuid)) {
      return (EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER *)gST->ConfigurationTable[Index].VendorTable;
    }
  }

  return NULL;
}

STATIC
VOID
PrintStatusConOut (
  VOID
  )
{
  EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER  *Rsdp;
  EFI_ACPI_DESCRIPTION_HEADER                   *Xsdt;
  UINT64                                        *Entry;
  UINTN                                         Index;
  UINTN                                         Count;
  CHAR8                                         Signature[5];
  CHAR16                                        Line[128];
  EFI_GRAPHICS_OUTPUT_PROTOCOL                  *Gop;

  if (gST->ConOut == NULL) {
    return;
  }

  Rsdp = FindRsdp ();
  if (Rsdp == NULL) {
    gST->ConOut->OutputString (gST->ConOut, L"STATUS: ACPI RSDP not published\r\n");
  } else {
    Xsdt  = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)Rsdp->XsdtAddress;
    Count = 0;
    if ((Xsdt != NULL) &&
        (Xsdt->Signature == EFI_ACPI_6_0_EXTENDED_SYSTEM_DESCRIPTION_TABLE_SIGNATURE))
    {
      Count = (Xsdt->Length - sizeof (EFI_ACPI_DESCRIPTION_HEADER)) / sizeof (UINT64);
    }

    UnicodeSPrint (
      Line,
      sizeof (Line),
      L"STATUS: ACPI RSDP=0x%lx rev=%u tables=%u\r\n",
      (UINT64)(UINTN)Rsdp,
      (UINT32)Rsdp->Revision,
      (UINT32)Count
      );
    gST->ConOut->OutputString (gST->ConOut, Line);

    if (Count != 0) {
      Entry = (UINT64 *)(Xsdt + 1);
      for (Index = 0; Index < Count; Index++) {
        if (Entry[Index] == 0) {
          continue;
        }

        CopyMem (Signature, (VOID *)(UINTN)Entry[Index], 4);
        Signature[4] = 0;
        UnicodeSPrint (Line, sizeof (Line), L"STATUS: ACPI table %a\r\n", Signature);
        gST->ConOut->OutputString (gST->ConOut, Line);
      }
    }
  }

  Gop = NULL;
  if (!EFI_ERROR (gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop)) &&
      (Gop != NULL) && (Gop->Mode != NULL) && (Gop->Mode->Info != NULL))
  {
    UnicodeSPrint (
      Line,
      sizeof (Line),
      L"STATUS: GOP %ux%u pitch=%u fmt=%u\r\n",
      Gop->Mode->Info->HorizontalResolution,
      Gop->Mode->Info->VerticalResolution,
      Gop->Mode->Info->PixelsPerScanLine,
      (UINT32)Gop->Mode->Info->PixelFormat
      );
    gST->ConOut->OutputString (gST->ConOut, Line);
  }
}

STATIC
VOID
EFIAPI
ShellReadyNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  PrintAcpiStatus ();
  PrintGopStatus ();
  PrintStatusConOut ();
  gBS->CloseEvent (Event);
}

STATIC
VOID
PrintAcpiStatus (
  VOID
  )
{
  EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER  *Rsdp;
  EFI_ACPI_DESCRIPTION_HEADER                   *Xsdt;
  UINT64                                        *Entry;
  UINTN                                         Index;
  UINTN                                         Count;
  CHAR8                                         Signature[5];

  Rsdp = NULL;
  for (Index = 0; Index < gST->NumberOfTableEntries; Index++) {
    if (CompareGuid (&gST->ConfigurationTable[Index].VendorGuid, &gEfiAcpi20TableGuid)) {
      Rsdp = (EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER *)gST->ConfigurationTable[Index].VendorTable;
      break;
    }
  }

  if (Rsdp == NULL) {
    DEBUG ((DEBUG_ERROR, "STATUS: ACPI RSDP not published\n"));
    return;
  }

  DEBUG ((
    DEBUG_ERROR,
    "STATUS: ACPI RSDP=0x%lx rev=%u XSDT=0x%lx\n",
    (UINT64)(UINTN)Rsdp,
    (UINT32)Rsdp->Revision,
    (UINT64)Rsdp->XsdtAddress
    ));

  if (Rsdp->XsdtAddress == 0) {
    return;
  }

  Xsdt = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)Rsdp->XsdtAddress;
  if (Xsdt->Signature != EFI_ACPI_6_0_EXTENDED_SYSTEM_DESCRIPTION_TABLE_SIGNATURE) {
    DEBUG ((DEBUG_ERROR, "STATUS: bad XSDT signature\n"));
    return;
  }

  Count = (Xsdt->Length - sizeof (EFI_ACPI_DESCRIPTION_HEADER)) / sizeof (UINT64);
  Entry = (UINT64 *)(Xsdt + 1);
  for (Index = 0; Index < Count; Index++) {
    if (Entry[Index] == 0) {
      continue;
    }

    CopyMem (Signature, (VOID *)(UINTN)Entry[Index], 4);
    Signature[4] = 0;
    DEBUG ((DEBUG_ERROR, "STATUS: ACPI table[%u] %a\n", (UINT32)Index, Signature));
  }
}

STATIC
VOID
PrintGopStatus (
  VOID
  )
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;

  Gop = NULL;
  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  if (EFI_ERROR (Status) || (Gop == NULL) || (Gop->Mode == NULL) || (Gop->Mode->Info == NULL)) {
    DEBUG ((DEBUG_ERROR, "STATUS: GOP unavailable (%r)\n", Status));
    return;
  }

  DEBUG ((
    DEBUG_ERROR,
    "STATUS: GOP %ux%u pitch=%u format=%u fb=0x%lx\n",
    Gop->Mode->Info->HorizontalResolution,
    Gop->Mode->Info->VerticalResolution,
    Gop->Mode->Info->PixelsPerScanLine,
    (UINT32)Gop->Mode->Info->PixelFormat,
    (UINT64)Gop->Mode->FrameBufferBase
    ));
}

EFI_STATUS
EFIAPI
PlatformStatusEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_EVENT  Event;
  VOID       *Registration;

  /*
   * Timer events do not fire on this platform (no visible callback), so wait
   * for the Shell protocol to be installed instead: protocol notify events
   * are signalled directly by the DXE core and need no timer interrupt.
   */
  Event        = NULL;
  Registration = NULL;
  Event = EfiCreateProtocolNotifyEvent (
            &gEfiShellProtocolGuid,
            TPL_CALLBACK,
            ShellReadyNotify,
            NULL,
            &Registration
            );
  return (Event == NULL) ? EFI_OUT_OF_RESOURCES : EFI_SUCCESS;
}
