/** @file
  Board boot options for yunluo.

  Registers the UEFI Shell as an active boot option.  Without an OS payload
  (this firmware has none), BDS would otherwise fall through to
  PlatformBootManagerUnableToBoot(), which shuts the device down after 10 s.
  With the Shell registered, the firmware stops at a usable prompt.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PlatformBootOptionsLib.h>

#include <Guid/GlobalVariable.h>

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

  Options = AllocateZeroPool (sizeof (EFI_PLATFORM_BOOT_OPTION));
  if (Options == NULL) {
    DEBUG ((DEBUG_ERROR, "%a: out of memory\n", __FUNCTION__));
    return;
  }

  Options[0].AppName       = L"UEFI Shell";
  Options[0].AppGuid       = &gUefiShellFileGuid;
  Options[0].LoadAttribute = LOAD_OPTION_ACTIVE;

  *BootOption      = Options;
  *BootOptionCount = 1;

  DEBUG ((DEBUG_ERROR, "%a: registering %u platform boot option(s)\n", __FUNCTION__, *BootOptionCount));
}
