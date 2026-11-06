##
#  yunluo (Xiaomi Redmi Pad, MT6789) — Project Silicium / Mu-Silicium platform.
#
#  Boot chain is the verified edk2-yunluo one: LK loads the boot image at
#  0x40000000, our BootShim copies the 7 MiB FV to 0x50000000 and jumps.
#  Memory layout is the static edk2-yunluo map, not the DT-based scheme.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

[Defines]
  PLATFORM_NAME                  = yunluo
  PLATFORM_GUID                  = 7C4E1B92-3F5A-4D8E-9A21-6B0F2C7D4E31
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/yunluoPkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = yunluoPkg/yunluo.fdf
  USE_CUSTOM_DISPLAY_DRIVER      = 1

!include MT6789Pkg/MT6789Pkg.dsc.inc

[Packages]
  yunluoPkg/yunluoPkg.dec

[PcdsFixedAtBuild]
  #
  # DDR / UEFI stack, matching Library/MemoryMapLib/MemoryMapLib.c
  #
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x40000000
  gArmTokenSpaceGuid.PcdSystemMemorySize|0x0000000200000000
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0x40C00000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x00040000

  #
  # Boot menu timeout.  The keypad is release-to-arm, so give the user a little
  # more time to open the menu (SiliciumPkg.dsc.inc defaults this to 3).
  #
  gEfiMdePkgTokenSpaceGuid.PcdPlatformBootTimeOut|5

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"Xiaomi"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"Redmi Pad"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"yunluo"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"Redmi_Pad_yunluo"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"Redmi Pad"

  #
  # FrameBufferSerialPortLib layout.  LK's simplefb on this panel is
  # 1200x2000, line length 4800 bytes (1200 px).
  #
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|1200
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|2000
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32

  #
  # Our GOP driver: pitch and visible width are separate.
  #
  gYunluoPkgTokenSpaceGuid.PcdYunluoFrameBufferAddress|0x00000000FE2C3000
  gYunluoPkgTokenSpaceGuid.PcdYunluoFrameBufferStride|1200
  gYunluoPkgTokenSpaceGuid.PcdYunluoFrameBufferVisibleWidth|1200
  gYunluoPkgTokenSpaceGuid.PcdYunluoFrameBufferHeight|2000
  gYunluoPkgTokenSpaceGuid.PcdYunluoFrameBufferLayers|3
  gYunluoPkgTokenSpaceGuid.PcdYunluoFrameBufferPixelFormat|1
  #
  # The OEM mounted this panel upside-down, so the OVL scanout has to be
  # rotated 180 degrees (CON BIT9|BIT10 + scanout from the last visible byte).
  #
  gYunluoPkgTokenSpaceGuid.PcdYunluoFrameBufferRotation|180

  #
  # The Shell app initialises its own shell protocol; library constructors
  # must not try to locate it before the app runs.
  #
  gEfiShellPkgTokenSpaceGuid.PcdShellLibAutoInitialize|FALSE

[LibraryClasses]
  #
  # Board libraries
  #
  #
  # Memory map, selected per SKU (they are NOT interchangeable the other way):
  #   ./build-yunluo-mu.sh DEBUG       -> 8 GiB map  (default; 8 GiB units)
  #   ./build-yunluo-mu.sh DEBUG 6g    -> 6 GiB map  (6 GiB units; top 32 MiB
  #                                       withheld as a guard)
  # Describing RAM that does not exist makes Windows die in Phase 0, so a
  # 6 GiB unit must never run the 8 GiB image.  The reverse is safe but wastes
  # 2 GiB.  The build script always passes MEMORY_MAP_6G, so no DEFINE here.
  #
!if $(MEMORY_MAP_6G) == TRUE
  MemoryMapLib|yunluoPkg/Library/MemoryMap6GLib/MemoryMapLib.inf
!else
  MemoryMapLib|yunluoPkg/Library/MemoryMapLib/MemoryMapLib.inf
!endif
  KeypadDeviceLib|yunluoPkg/Library/KeypadDeviceLib/KeypadDeviceLib.inf
  PlatformBootOptionsLib|yunluoPkg/Library/PlatformBootOptionsLib/PlatformBootOptionsLib.inf

  #
  # SCSI disk layer on top of UfsPassThruDxe
  #
  UefiScsiLib|MdePkg/Library/UefiScsiLib/UefiScsiLib.inf

  #
  # Setup UI: DisplayEngineDxe draws the forms with this helper; nothing else
  # in this platform pulls it in (SiliciumPkg.dsc.inc does not map it).
  #
  CustomizedDisplayLib|MdeModulePkg/Library/CustomizedDisplayLib/CustomizedDisplayLib.inf

  #
  # Shell libraries (nothing in the upstream platform needed them)
  #
  ShellLib|ShellPkg/Library/UefiShellLib/UefiShellLib.inf
  ShellCommandLib|ShellPkg/Library/UefiShellCommandLib/UefiShellCommandLib.inf
  HandleParsingLib|ShellPkg/Library/UefiHandleParsingLib/UefiHandleParsingLib.inf
  BcfgCommandLib|ShellPkg/Library/UefiShellBcfgCommandLib/UefiShellBcfgCommandLib.inf

[Components]
  #
  # ACPI tables (FACP/APIC/GTDT/DSDT).  AcpiTableDxe and AcpiPlatformDxe
  # are already pulled in by SiliciumPkg.dsc.inc.
  #
  yunluoPkg/AcpiTables/AcpiTables.inf

  #
  # Post-boot status dump (ACPI/GOP) for on-device diagnosis.
  #
  yunluoPkg/Drivers/PlatformStatusDxe/PlatformStatusDxe.inf

  #
  # Mask all GIC sources and disarm the EL1 architected timers at before-EBS.
  #
  yunluoPkg/Drivers/HandoffQuiesceDxe/HandoffQuiesceDxe.inf

  #
  # Ask the hardware whether the GIC, the architected timer and the clock tree
  # actually work, rather than inferring it from code.
  #
  yunluoPkg/Drivers/HandoffAuditDxe/HandoffAuditDxe.inf

  #
  # Touch (merged from yunluo-uefi-development/uefi-yunluo):
  #   YunluoTouchAcpiDxe     - installs the SSDT describing the OS-owned
  #                            NT36523 node (_HID "YUNT0001").
  #   YunluoTouchPlatformDxe - publishes the platform resource facts
  #                            (GPIO 0x10005000, SPI0 0x1100A000, GPIO30-33,
  #                            touch GPIO8, SPI mode 0) on a private protocol.
  # Neither driver touches hardware (no MMIO/GPIO/reset/firmware upload), so
  # they are inert until a Windows-side consumer exists.
  #
  yunluoPkg/Drivers/YunluoTouchAcpiDxe/YunluoTouchAcpiDxe.inf
  yunluoPkg/Drivers/YunluoTouchPlatformDxe/YunluoTouchPlatformDxe.inf

  #
  # Input: PMIC power/home (volume up) and GPIO 66 (volume down)
  #
  SiliciumPkg/Drivers/KeypadDxe/KeypadDxe.inf
  SiliciumPkg/Drivers/KeypadDeviceDxe/KeypadDeviceDxe.inf

  #
  # GOP on the LK framebuffer, including the OVL handoff.
  #
  yunluoPkg/Drivers/SimpleFbDxe/SimpleFbDxe.inf

  #
  # UFS host controller + generic pass-thru.
  #
  yunluoPkg/Drivers/MtkUfsDxe/MtkUfsDxe.inf
  MdeModulePkg/Bus/Ufs/UfsPassThruDxe/UfsPassThruDxe.inf
  MdeModulePkg/Bus/Scsi/ScsiBusDxe/ScsiBusDxe.inf
  MdeModulePkg/Bus/Scsi/ScsiDiskDxe/ScsiDiskDxe.inf

  #
  # UEFI setup front end.  UiApp is the graphical settings UI (Device Manager /
  # Boot Manager / Boot Maintenance Manager) and is registered as the
  # "UEFI Settings" boot option by PlatformBootOptionsLib.
  #
  # UiApp needs three pieces that nothing else in this platform pulls in:
  #   UiApp              - the menu front end itself
  #   DisplayEngineDxe   - produces EDKII_FORM_DISPLAY_ENGINE_PROTOCOL, which is
  #                        what actually draws the forms ("Fatal Error!
  #                        EDKII_FORM_DISPLAY_ENGINE_PROTOCOL not found!" when absent)
  #   (SetupBrowserDxe / BootManagerMenuApp come from SiliciumPkg.dsc.inc +
  #    SiliciumPkg/Extra.fdf.inc)
  #
  MdeModulePkg/Application/UiApp/UiApp.inf
  MdeModulePkg/Universal/DisplayEngineDxe/DisplayEngineDxe.inf

  #
  # UEFI Shell, registered as a boot option by PlatformBootOptionsLib so a
  # firmware with no OS payload stops at a prompt instead of shutting down.
  #
  ShellPkg/Application/Shell/Shell.inf {
    <PcdsFixedAtBuild>
      gEfiShellPkgTokenSpaceGuid.PcdShellLibAutoInitialize|FALSE
    <LibraryClasses>
      NULL|ShellPkg/Library/UefiShellCommandLib/UefiShellCommandLib.inf
      NULL|ShellPkg/Library/UefiShellLevel1CommandsLib/UefiShellLevel1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellLevel2CommandsLib/UefiShellLevel2CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellLevel3CommandsLib/UefiShellLevel3CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellDriver1CommandsLib/UefiShellDriver1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellDebug1CommandsLib/UefiShellDebug1CommandsLib.inf
  }
