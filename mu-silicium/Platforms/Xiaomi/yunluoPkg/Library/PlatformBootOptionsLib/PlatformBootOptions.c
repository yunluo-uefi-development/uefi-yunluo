/** @file
  Board boot options for yunluo.

  Registers two active boot options:

    "UEFI Shell"    - the ShellApp, so a firmware with no OS payload stops at a
                      usable prompt instead of PlatformBootManagerUnableToBoot().
    "UEFI Settings" - UiApp, which is the graphical setup front end (Device
                      Manager / Boot Manager / Boot Maintenance Manager).  UiApp
                      renders on ConOut, so with GraphicsConsoleDxe bound to our
                      GOP it comes up as a graphical UI on the panel.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PlatformBootOptionsLib.h>

#include <Guid/GlobalVariable.h>

//
// UiApp.inf FILE_GUID.  MdeModulePkg.dec only publishes the BootManagerMenuApp
// GUID (PcdBootManagerMenuFile), so the UiApp file GUID is spelled out here.
//
STATIC EFI_GUID  mUiAppFileGuid = {
  0x462CAA21, 0x7614, 0x4503, { 0x83, 0x6E, 0x8A, 0xB6, 0xF4, 0x66, 0x23, 0x31 }
};

VOID
GetPlatformBootOptions (
  OUT EFI_PLATFORM_BOOT_OPTION **BootOption,
  OUT UINT8                     *BootOptionCount
  )
{
  EFI_PLATFORM_BOOT_OPTION  *Options;

  if ((BootOption == NULL) || (BootOptionCount == NULL)) {
    return;
  }

  *BootOption      = NULL;
  *BootOptionCount = 0;

  Options = AllocateZeroPool (sizeof (EFI_PLATFORM_BOOT_OPTION) * 2);
  if (Options == NULL) {
    DEBUG ((DEBUG_ERROR, "%a: out of memory\n", __FUNCTION__));
    return;
  }

  Options[0].AppName       = L"UEFI Shell";
  Options[0].AppGuid       = &gUefiShellFileGuid;
  Options[0].LoadAttribute = LOAD_OPTION_ACTIVE;

  Options[1].AppName       = L"UEFI Settings";
  Options[1].AppGuid       = &mUiAppFileGuid;
  Options[1].LoadAttribute = LOAD_OPTION_ACTIVE;

  *BootOption      = Options;
  *BootOptionCount = 2;

  DEBUG ((DEBUG_ERROR, "%a: registering %u platform boot option(s)\n", __FUNCTION__, *BootOptionCount));
}
