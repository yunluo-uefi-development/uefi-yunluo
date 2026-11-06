#include <Library/MemoryMapLib.h>

//
// yunluo (Redmi Pad, MT6789) memory map, ported from the verified
// edk2-yunluo platform map.  The DDR regions are carved from the mainline
// mt8781-yunluo.dts reserved-memory nodes so the DXE allocator never hands
// out LK/ATF/TEE/SCP/SSPM/consys/framebuffer memory.
//
// The framebuffer region keeps the name "Display Reserved": SiliciumPkg's
// SEC clears it at boot and FrameBufferSerialPortLib uses it for the
// on-screen console before the GOP exists.
//
// Register regions are listed individually (not as one 1 GiB "Periphs"
// block) because Mu's RamManagerLib looks up the "EMI" region by name and
// the MMU table is built from this list.
//
STATIC
EFI_MEMORY_REGION_DESCRIPTOR
gMemoryDescriptor[] = {
  // Name, Address, Length, HobOption, ResourceType, ResourceAttribute, MemoryType, ArmAttribute

  // Register Regions
  {"UFS PHY",            0x01FA0000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"GIC Distributor",    0x0C000000, 0x00040000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"GIC Redistributors", 0x0C040000, 0x00200000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"Top Ck Gen",         0x10000000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"Infra Cfg AO",       0x10001000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"Pinctrl",            0x10005000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"WatchDog Timer",     0x10007000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"AP Mixed",           0x1000C000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"PMIC Wrapper",       0x10026000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"EMI",                0x10219000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"UFS HCI",            0x11270000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"MSDC-0",             0x11230000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"MSDC-1",             0x11240000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"MSDC Top-0",         0x11F50000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"MSDC Top-1",         0x11EF0000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},
  {"Display OVL",        0x14005000, 0x00003000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},

  // DDR Regions
  {"UEFI DDR Low",       0x40000000, 0x00C00000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
  {"UEFI Stack",         0x40C00000, 0x00040000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"CPU Vectors",        0x40C40000, 0x00010000, AddMem, SYS_MEM, SYS_MEM_CAP, BsCode, WRITE_BACK},
  {"DXE Heap",           0x40C50000, 0x07430000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
  {"Res kinfo-pstore",   0x48080000, 0x00100000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"DDR 0x48180000",     0x48180000, 0x00080000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
  {"BL31",               0x48200000, 0x00200000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"DDR 0x48400000",     0x48400000, 0x00001000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
  {"Res crypto",         0x48401000, 0x00001000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"DDR 0x48402000",     0x48402000, 0x07BFE000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
  {"UEFI FD",            0x50000000, 0x00700000, AddMem, SYS_MEM, SYS_MEM_CAP, BsCode, WRITE_BACK},
  {"Res aee_lk",         0x50700000, 0x00800000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"DDR 0x50F00000",     0x50F00000, 0x1F100000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
  {"TEE Mem",            0x70000000, 0x03200000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"DDR 0x73200000",     0x73200000, 0x09E00000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
  {"Consys EMI",         0x7D000000, 0x00500000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"DDR 0x7D500000",     0x7D500000, 0x00D00000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
  {"TEE SecMem",         0x7E200000, 0x00800000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"GZ/Unmap/ISU/MCUPM", 0x7EA00000, 0x01600000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"DDR 0x80000000",     0x80000000, 0x0F000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
  {"SCP Share",          0x8F000000, 0x005A0000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"DDR 0x8F5A0000",     0x8F5A0000, 0x0FFC0000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
  {"SSPM",               0x9F560000, 0x00910000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  // mainline mt8781-yunluo.dts reserves a further 1.5 MiB right after the block
  // above (sspm@9fe70000); leaving it Conventional hands live SSPM memory to
  // the OS as free RAM.
  {"SSPM log",           0x9FE70000, 0x00180000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"DDR 0x9FFF0000",     0x9FFF0000, 0x1F010000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
  {"WIFI",               0xBF000000, 0x00600000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"DDR 0xBF600000",     0xBF600000, 0x00400000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
  {"SCP/ATF",            0xBFA00000, 0x00600000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"DDR 0xC0000000",     0xC0000000, 0x3E000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
  {"Res dfdmcu",         0xFE000000, 0x00100000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"DDR 0xFE100000",     0xFE100000, 0x001C3000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
  {"Display Reserved",   0xFE2C3000, 0x01D3C000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH_XN},
  {"DDR 0xFFFFF000",     0xFFFFF000, 0x40000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
  // DRAMC rank-0 repair page, reserved by mainline as dramc-rk0@13ffff000.
  {"Res dramc-rk0",      0x13FFFF000, 0x00001000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  {"DDR 0x140000000",    0x140000000, 0xFF200000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
  {"TEE High",           0x23F200000, 0x00E00000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK}
};

VOID
GetMemoryMap (
  OUT EFI_MEMORY_REGION_DESCRIPTOR **MemoryDescriptor,
  OUT UINT8                         *MemoryDescriptorCount
  )
{
  *MemoryDescriptor      = gMemoryDescriptor;
  *MemoryDescriptorCount = ARRAY_SIZE (gMemoryDescriptor);
}
