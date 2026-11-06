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
  # The Shell app initialises its own shell protocol; library constructors
  # must not try to locate it before the app runs.
  #
  gEfiShellPkgTokenSpaceGuid.PcdShellLibAutoInitialize|FALSE

[LibraryClasses]
  #
  # Board libraries
  #
  MemoryMapLib|yunluoPkg/Library/MemoryMapLib/MemoryMapLib.inf
  KeypadDeviceLib|yunluoPkg/Library/KeypadDeviceLib/KeypadDeviceLib.inf
  PlatformBootOptionsLib|yunluoPkg/Library/PlatformBootOptionsLib/PlatformBootOptionsLib.inf

  #
  # SCSI disk layer on top of UfsPassThruDxe
  #
  UefiScsiLib|MdePkg/Library/UefiScsiLib/UefiScsiLib.inf

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
