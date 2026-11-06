# MTK UFS 在 UEFI（Mu-Silicium / EDK2）上的移植说明

> 参考实现：Xiaomi Redmi Pad（`yunluo`，MT8781 / MT6789，Helio G99）
> 固件：Project Silicium（Mu-Silicium），EDK2 通用 UFS 栈 + 平台驱动 `MtkUfsDxe`
> 结果：UFS 正常初始化，3 个 LUN 全部枚举，GPT 分区在 UEFI Shell 里显示为 `blk0`–`blk71`

这份文档按“依赖 → 平台驱动 → 通用栈修补 → 调试”的顺序说明，最后附完整代码。别的 MTK 设备可以照第 8 节的检查表替换寄存器/时钟数据。

---

## 1. 先确认硬件事实（全部从 DTS / 时钟驱动反查）

| 项 | yunluo 的值 | 从哪里查 |
|---|---|---|
| UFS HCI | `0x11270000`，size `0x2300`，IRQ 137 | 原厂 `mt6789.dts` 的 `ufshci` 节点 |
| UFS PHY | `0x01FA0000`，size `0xc000` | **mainline** `mt6789.dtsi` 的 `ufs-phy` 节点（节点名可能写成 `11fa0000`，以 `reg` 为准） |
| 时钟 | `ufs`、`ufs_mp`、`unipro_tick`、`unipro_sys`、`ufs_aes`（infracfg gate）+ `ufs_sel`、`aes_ufsfde_sel`（topckgen mux+gate） | DTS `clocks`；bit 位置查 `drivers/clk/mediatek/clk-mtXXXX.c` |
| 复位 | `hci_rst`、`unipro_rst`、`crypto_rst`（3 个，没有 `mphy_rst`） | DTS `resets` |
| 供电 | `mt6358_vemc_reg` / `ldo_vemc` 3.0 V | DTS `vcc-supply` |
| SMC ABI | `0xC2000276`：BIT1 device reset、BIT2 crypto、BIT3 refclk | 原厂 `ufs-mediatek.c` 的 `ufs_mtk_smc()` |
| 设备 | SKhynix `HN8T062EHKX039`，4K 逻辑块，4 个 WLUN | 运行时读描述符 |

**权威顺序**：原厂 kernel（`kernel_xiaomi-*/drivers/scsi/ufs/vendor/...`）> mainline（只用来解释 PHY/寄存器）。
不要从别的 SoC 抄设备级数据。

在本平台上实测的关键寄存器：

| 名称 | 地址/偏移 | 说明 |
|---|---|---|
| `HCS` | HCI + `0x30` | 正常值 `0xF`（DP/UTRLRdy/UTMRLRdy/UCRdy） |
| `IS` | HCI + `0x20` | `BIT0`=UTRCS，`BIT8`=ULSS，`BIT10`=UCCS |
| `UECPA` | HCI + `0x38` | link startup 期间出现 `0x80000010`（PA LINERESET）属正常 |
| `XOUFS_CTRL` | HCI + `0x140` | bit7 需要在 refclk 前置位 |
| refclk 握手 | HCI + `0x144` | bit0=REQUEST，bit1=ACK |
| `PA_LOCALVERINFO` | MIB `0x15A9` | DME GET |
| `PA_Local_TX_LCC` | MIB `0x155E` | link startup 前必须置 0 |
| `VS_SAVEPOWERCONTROL` | MIB `0xD0A6` | bit6=deep stall；bit0/2/3=UniPro clock gating |
| `VS_UNIPROPOWERDOWNCONTROL` | MIB `0xD0A8` | 退出 UniPro LPM 时写 0 |
| `VS_DEBUGCLOCKENABLE` | MIB `0xD0A1` | bit5=`TX_SYMBOL_CLK_REQ_FORCE` |

---

## 2. 软件栈与组件注册

UEFI 侧完整的 UFS 读写链是四层，**少一层都枚举不出磁盘**：

```text
MtkUfsDxe            平台 Host 协议 + 平台回调（我们的驱动）
  ↓ gEdkiiUfsHostControllerProtocol / gEdkiiUfsHcPlatformProtocol
UfsPassThruDxe       ExtScsiPassThru
  ↓
ScsiBusDxe           ScsiIo
  ↓
ScsiDiskDxe          BlockIo / BlockIo2
  ↓
DiskIoDxe → PartitionDxe → FatPkg
```

> 最容易漏的是 **`ScsiBusDxe`**：`ScsiDiskDxe` 绑的是 `ScsiIo`，不是 `ExtScsiPassThru`。缺它时日志会停在 `UFS: ExtScsiPassThru installed`，看起来像“UFS 没枚举”。

DSC 注册（`[Components]` 和 `[LibraryClasses]`）：

```ini
[LibraryClasses]
  UefiScsiLib|MdePkg/Library/UefiScsiLib/UefiScsiLib.inf

[Components]
  yunluoPkg/Drivers/MtkUfsDxe/MtkUfsDxe.inf
  MdeModulePkg/Bus/Ufs/UfsPassThruDxe/UfsPassThruDxe.inf
  MdeModulePkg/Bus/Scsi/ScsiBusDxe/ScsiBusDxe.inf
  MdeModulePkg/Bus/Scsi/ScsiDiskDxe/ScsiDiskDxe.inf
```

APRIORI / DXE 的 FDF include 里同样按这个顺序列出：

```text
INF yunluoPkg/Drivers/MtkUfsDxe/MtkUfsDxe.inf
INF MdeModulePkg/Bus/Ufs/UfsPassThruDxe/UfsPassThruDxe.inf
INF MdeModulePkg/Bus/Scsi/ScsiBusDxe/ScsiBusDxe.inf
INF MdeModulePkg/Bus/Scsi/ScsiDiskDxe/ScsiDiskDxe.inf
INF MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf
INF MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf
INF FatPkg/EnhancedFatDxe/Fat.inf
```

`MtkUfsDxe.inf`：

```ini
[Defines]
  INF_VERSION   = 0x00010005
  BASE_NAME     = MtkUfsDxe
  MODULE_TYPE   = UEFI_DRIVER
  ENTRY_POINT   = MtkUfsEntryPoint

[LibraryClasses]
  BaseLib
  BaseMemoryLib
  ArmSmcLib
  DebugLib
  CacheMaintenanceLib
  DevicePathLib
  DxeServicesTableLib
  IoLib
  TimerLib
  MemoryAllocationLib
  UefiBootServicesTableLib
  UefiDriverEntryPoint
  UefiLib

[Protocols]
  gEdkiiUfsHostControllerProtocolGuid ## PRODUCES
  gEdkiiUfsHcPlatformProtocolGuid     ## PRODUCES
  gEfiDevicePathProtocolGuid          ## PRODUCES
  gMediaTekPmicProtocolGuid           ## CONSUMES

[Depex]
  TRUE
```

---

## 3. 平台驱动 `MtkUfsDxe` 的实现顺序

通用 `UfsPassThruDxe` 会按下面的阶段回调平台驱动：

```c
EdkiiUfsHcPreHce            // 打开 HCE 之前
EdkiiUfsHcPostHce           // 打开 HCE 之后
EdkiiUfsHcPreLinkStartup    // DME_LINK_STARTUP 之前
EdkiiUfsHcPostLinkStartup   // 设备初始化完成之后
```

我们让平台驱动接管 link startup（`SkipLinkStartup = TRUE`），这样才能拿到 `DME_LINK_STARTUP` 的 UniPro 返回值。

### 3.1 入口：供电、时钟、PHY、复位

`MtkUfsEntryPoint()` 里先做：

1. `Pmic->RegulatorSetEnable("ldo_vemc", TRUE)`（VEMC 3.0 V）。
2. 打开 5 个 Infra gate：`UNIPRO_SYS|UNIPRO_TICK|UFS_SAP_BCLK|UFS|UFS_AES`（`INFRA_AO2_CG_CLR`，`0x10001000 + 0xA8`）。
3. topckgen `CLK_CFG_12`（`0xD0/0xD4/0xD8`）：
   - 清 `ufs_sel`（bit24..26）和 `aes_ufsfde_sel`（bit16..18）的 mux，再选 parent；
   - **ungate bit31（`ufs_sel` gate）和 bit23（`aes_ufsfde_sel` gate）**；
   - `CLK_CFG_UPDATE`（`0x08`）置 bit20（ufs）和 bit19（aes）。
4. PHY active 序列（PHY base `0x01FA0000`）：
   - `0x008C`：`PLL_PWR_ON`，清 `FRC_PLL_PWR_ON`、`PLL_ISO_EN`
   - `0xB044`：`CDR_PWR_ON`，清 force/iso
   - `0xA0AC`：`RX_SQ_EN`，清 force
   - delay 1 µs；`0xA09C` 清 `FSM_DIFZ_FRC`
5. 三段复位：`rst1 bit15(hci)`、`rst2 bit7(unipro)`、`rst3 bit21(crypto)`，100 µs 后按相反顺序释放。
6. 安装 `gEdkiiUfsHostControllerProtocol`、`gEdkiiUfsHcPlatformProtocol`、`gEfiDevicePathProtocol`。

### 3.2 `EdkiiUfsHcPreHce`

顺序直接照抄原厂 `ufshcd_init()`：

```text
device reset（SMC BIT1：拉低 10 µs → 拉高 → 等 10 ms）
→ 三段 host reset
→ crypto 保持关闭（UEFI 没有密钥，开了会让 UTP query 永远不完成）
→ refclk：SMC BIT3 → XOUFS_CTRL bit7 → 0x144 REQUEST/ACK 握手 → 等 100 µs
```

### 3.3 `EdkiiUfsHcPreLinkStartup`

必须和原厂 `ufs_mtk_pre_link()` 的顺序一致：

```text
DME GET  PA_LOCALVERINFO (0x15A9)
DME SET  VS_UNIPROPOWERDOWNCONTROL (0xD0A8) = 0   // 退出 UniPro LPM
DME SET  PA_Local_TX_LCC (0x155E) = 0             // 主机关闭 TX LCC
DME GET  VS_SAVEPOWERCONTROL (0xD0A6)
DME SET  VS_SAVEPOWERCONTROL = 清 bit6            // 关 deep stall
DME_LINK_STARTUP（UIC opcode 0x16），最多重试 3 次，检查 Arg2 == 0
→ 清 HCI 错误寄存器（IS / UECPA / UECDL / UECN / UECT / UECDME 写 1 清）
```

两个容易错的地方：

- **link startup 的 UIC opcode 是 `0x16`**（`UfsUicDmeLinkStartup`），不是自定义的 `0x08`。
- **DME GET 的返回值在 `UICCMDARG3`**，不是 `ARG2`。`EDKII_UIC_COMMAND.Arg2` 回读的是结果码（低 8 位非 0 即失败），`Arg3` 才是数据。取错会看到 `PA_LOCALVERINFO=0`、`SAVEPOWER=0`，而且把 0 写回去会清掉 `SAVEPOWER` 里其它有效位。

### 3.4 `EdkiiUfsHcPostLinkStartup`

对应原厂 `ufs_mtk_post_link()`，做 UniPro clock gating：

```text
DME GET/SET VS_SAVEPOWERCONTROL：置 bit0（RX symbol）/bit2（SYS）/bit3（TX）
DME GET/SET VS_DEBUGCLOCKENABLE (0xD0A1)：清 bit5（TX_SYMBOL_CLK_REQ_FORCE）
```

这只是省电优化，失败不要阻断启动。

### 3.5 描述符内存：Normal Non-Cacheable

MTK UFS **不是 cache-coherent**。通用栈会直接轮询 UTRD 里的 `Ocs` 和 response UPIU，如果描述符在可缓存内存里，CPU 会读到旧值（表现为 `OCS=0xF`、`QueryResp=0`）。

正确做法是给描述符环用 **Normal Non-Cacheable**，等价于 Linux 的 `dma_alloc_coherent()`：

```c
gBS->AllocatePages (AllocateMaxAddress, MemoryType, Pages, &Address);   // 4 GiB 以下
gDS->SetMemorySpaceAttributes (Address, EFI_PAGES_TO_SIZE (Pages), EFI_MEMORY_WC);
```

**不要用 `EFI_MEMORY_UC`**：它映射成 Device 内存，Device 内存禁止非对齐访问，而 UTRD/UPIU 是 packed 结构，会触发：

```text
Data abort: Alignment fault
ESR 0x96000021  (EC=0x25, DFSC=0x21)
FAR 0x...1DE    (只有 2 字节对齐)
```

数据缓冲区（调用者的读写 buffer）仍然是普通可缓存内存，需要在 `Map/Unmap` 里做 cache 维护：

```c
Map (BusMasterWrite，设备→内存) : InvalidateDataCacheRange (addr, len);
Map (BusMasterRead，内存→设备)  : WriteBackDataCacheRange (addr, len);
Unmap                            : InvalidateDataCacheRange (addr, len);
```

### 3.6 `OverrideHcInfo`

Mu/新版 EDK2 的 `UfsPassThruDxe` 会按 `UFS_HC_CAP_64ADDR` 决定 UTRL/UTRD 用 64 位还是 32 位地址。我们的缓冲区都在 4 GiB 以下，直接把该能力位清掉，走 32 位路径：

```c
Info->Capabilities &= ~UFS_HC_CAP_64ADDR;
```

---

## 4. 通用栈必须修的三处

### 4.1 完成标志是 `IS.UTRCS`，不是 `UTRLDBR`

`UTRLDBR` 在 HCI **取走**请求时就清零，不代表命令完成。社区版 `UfsPassThruDxe` 用它判断完成，会过早读 OCS（读到初始化值 `0xF`）。正确做法：

`MdePkg/Include/IndustryStandard/UfsHci.h` 增加：

```c
#define UFS_HC_IS_UTRCS    BIT0
```

`UfsPassThruHci.c`：

```c
/* UfsStartExecCmd()：敲门铃前先清掉陈旧的 UTRCS */
Status = UfsMmioWrite32 (Private, UFS_HC_IS_OFFSET, UFS_HC_IS_UTRCS);
if (EFI_ERROR (Status)) return Status;
Status = UfsMmioWrite32 (Private, UFS_HC_UTRLDBR_OFFSET, BIT0 << Slot);

/* 同步 query / NOP / SCSI：等 IS.UTRCS */
Status = UfsWaitMemSet (Private, UFS_HC_IS_OFFSET, UFS_HC_IS_UTRCS,
                        UFS_HC_IS_UTRCS, Packet->Timeout);

/* 异步 ProcessAsyncTaskList()：看每个槽位的 OCS 是否还在初始值 */
if (TransReq->Trd->Ocs == UFS_HC_TRD_OCS_INIT_VALUE) {
  /* 未完成，继续等或超时 */
} else {
  UfsMmioWrite32 (Private, UFS_HC_IS_OFFSET, UFS_HC_IS_UTRCS); /* 应答共享位 */
  /* 处理完成：sense data / TargetStatus / InTransferLength */
}
```

### 4.2 DME GET 结果在 `ARG3`

见 3.3。平台驱动里封装：

```c
Command.Opcode = UFS_UIC_DME_GET;
Command.Arg1   = Mib << 16;
Command.Arg2   = 0;
Command.Arg3   = 0;
Status = Interface->UfsExecUicCommand (Interface, &Command);
*Value = Command.Arg3;   // 不是 Arg2
```

### 4.3 `ScsiBusDxe` 不能漏

见第 2 节。这是“UFS 初始化成功但没有盘”的最常见原因。

---

## 5. 调试顺序（按这个顺序看日志）

1. **HCI 是否可访问**：`HCS`（`0x30`）应为 `0xF`；`IS`（`0x20`）能清。
2. **UIC 通路**：`DME SET` 的 Arg2 低 8 位为 0；`PA_LOCALVERINFO` 能读到非 0。
3. **链路**：`DME_LINK_STARTUP` 返回 `arg2=0`；失败时立刻 dump
   `IS/UECPA/UECDL/UECN/UECT/UECDME`（`0x20/0x38/0x3C/0x40/0x44/0x48`），
   再查 PHY 的 `XOUFS_CTRL(0x140)`、`PA_Local_TX_LCC(0x155E)`、
   `VS_SAVEPOWERCONTROL(0xD0A6)`、`VS_UNIPROPOWERDOWNCONTROL(0xD0A8)`。
4. **query/描述符**：能进 `EdkiiUfsHcPostLinkStartup`，说明 NOP IN 和 `fDeviceInit` 都成功了（否则会提前失败）。
5. **LUN**：打印 unit descriptor 的 `LunEn` 位图，再打印 device descriptor。
6. **磁盘栈**：确认 `ScsiDisk: attached`、`Partition: detection ...`。
7. **分区**：Shell 里能看到 `blk*` 即 GPT 解析成功。

本平台成功日志：

```text
UFS MTK: PA_LOCALVERINFO status=Success value=0x3
UFS MTK: UNIPRO_LPM=OK  TX_LCC=OK  SAVEPOWER=0x90 → set OK
UFS MTK: LINK_STARTUP try 0: status=Success arg2=0x0
UFS MTK: link done status=Success arg2=0x0 HCS=0xf IS=0x404
         UECPA=0x80000010 UECDL=0x0 UECN=0x0 UECT=0x0 UECD=0x0
UFS MTK: after clear HCS=0xf IS=0x0 ...
UFS MTK: post-link CG=OK
UFS LUN 0/1/2 is enabled
UFS: unit descriptor scan done, LUN mask = 0x7
UFS: device descriptor manufacturer=0x1D1 product_rev=0x5 spec=0x200 num_wlun=4 security_lun=1
UFS: ExtScsiPassThru installed, LUN mask = 0x807
ScsiDisk: attached vendor='SKhynix' product='HN8T062EHKX039', block=0x1000
  LUN0 last_lba=0x1DCAFFF (~31 GiB)，LUN1/2 last_lba=0x3FF (4 MiB)
```

---

## 6. 踩坑清单

| 现象 | 根因 | 解决 |
|---|---|---|
| `DME_LINK_STARTUP` 超时、`arg2=0` | 用了 opcode `0x08` | 改 `UfsUicDmeLinkStartup`（`0x16`） |
| link 成功但 query `OCS=0xF` | 等的是 `UTRLDBR`；且描述符可缓存 | 等 `IS.UTRCS`；描述符改 Normal-NC |
| `PA_LOCALVERINFO=0`、`SAVEPOWER=0` | DME GET 取了 `ARG2` | 取 `UICCMDARG3` |
| 查询全部失败、日志显示 HCE 异常 | 无密钥却打开 crypto 引擎 | UEFI 阶段保持 crypto 关闭 |
| `Synchronous Exception / Data abort: Alignment fault` | 描述符用了 `EFI_MEMORY_UC`（Device 内存） | 改 `EFI_MEMORY_WC`（Normal-NC） |
| UFS 初始化完成但 Shell 没有盘 | 缺 `ScsiBusDxe` | 补 `ScsiBusDxe.inf` 到 DSC/APRIORI/DXE |
| UTRL 地址被截断/异常 | 64ADDR 能力与缓冲区位置不匹配 | `OverrideHcInfo` 清 `UFS_HC_CAP_64ADDR` |
| link 前就失败 | 时钟/供电没起来 | 先确认 VEMC、7 个时钟（含 AES gate）和 PHY 上电 |
| 开机日志看不到 | BDS 日志被 Shell 清屏 | DEBUG 版跳过 Shell 的 `ClearScreen`，或在关键阶段留延时 |

---

## 7. 移植到其它 MTK 设备的检查表

1. 从原厂 DTS 拿到 HCI/PHY 的 `reg`、`interrupts`、`clocks`、`resets`、`vcc-supply`。
2. 从 `clk-mtXXXX.c` 确认 `ufs_sel` / `aes_ufsfde_sel` 的 mux 位、gate 位和 update 位（不同 SoC 位号不同）。
3. 从原厂 `ufs-mediatek.c` 确认 SMC 命令号与各 BIT 的含义（`MTK_SIP_UFS_CONTROL` 的编号也可能不同）。
4. PHY 寄存器偏移以 mainline `phy-mtk-ufs.c` 为准；如果 PHY `reg` 是 `0x11fa0000` 而不是 `0x01fa0000`，按实际 `reg` 访问。
5. 确认设备 reset 的极性/时序（我们这里是 RST_n 低有效，低 10 µs、高后等 10 ms）。
6. 确认 crypto 是否必须开启；没有密钥就保持关闭。
7. 先跑通 link + query，再谈磁盘栈；每一步都打印寄存器，避免“黑盒重试”。
8. 如果设备是 4K 逻辑块，ScsiDisk 报 `block=0x1000` 是正常的；Android 的 LUN 是 ext4/f2fs，`Fat: no volume` 不代表失败。

---

## 附录 A：平台驱动完整源码

文件：`Platforms/Xiaomi/yunluoPkg/Drivers/MtkUfsDxe/MtkUfsDxe.c`

> 下面是实机调试版。`MTK_UFS_PHOTO_STALL_MS`、`MtkUfsPause()` 和 `MtkUfsDumpEvent` 只是为了让没有串口/键盘的设备能拍照记录日志，正式固件可以删掉；其余逻辑与验证版本一致。

```c
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

STATIC
VOID
MtkUfsStatusText (
  IN CONST CHAR16 *Text
  )
{
  if (Text == NULL) {
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

STATIC
VOID
MtkUfsPause (
  IN UINTN Milliseconds
  )
{
  gBS->Stall ((UINTN)Milliseconds * 1000U);
}

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
  UINT32 LinkResult;
  UINTN Retry;

  if (Data == NULL) return EFI_INVALID_PARAMETER;
  Interface = (EDKII_UFS_HC_DRIVER_INTERFACE *)Data;
  if (Phase == EdkiiUfsHcPreHce) {
    /*
     * Baseline order: device reset first (HCE still off), then the
     * HCE-notify host reset, then crypto and refclk.
     */
    MtkUfsDeviceReset ();
    MtkUfsResetAndEnable ();

    /*
     * UEFI has no inline-crypto key setup.  Keep the MTK crypto engine off;
     * enabling it via SMC without configuring keys leaves the HCI in a host
     * error state and UTP query requests never complete.
     */
    MtkUfsStatusText (L"UFS MTK: crypto engine left off (no keys)\r\n");

    Status = MtkUfsRefClkOn ();
    MtkUfsStatusText (EFI_ERROR (Status) ? L"UFS MTK: PreHce refclk=FAIL\r\n" : L"UFS MTK: PreHce refclk=OK\r\n");
    MtkUfsPause (MTK_UFS_PHOTO_STALL_MS / 2);
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
    DEBUG ((
      DEBUG_ERROR,
      "UFS MTK: PA_LOCALVERINFO status=%r value=0x%x\n",
      Status,
      Value
      ));
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
    DEBUG ((DEBUG_ERROR, "UFS MTK: SAVEPOWER=0x%x\n", Value));
    Value &= ~UFS_SAVEPOWER_DEEP_STALL;
    Status = MtkUfsExecUic (Interface, UFS_UIC_DME_SET,
                            (UFS_VS_SAVEPOWERCONTROL << 16), Value, NULL);
    MtkUfsStatusText (EFI_ERROR (Status) ? L"UFS MTK: SAVEPOWER set=FAIL\r\n" : L"UFS MTK: SAVEPOWER set=OK\r\n");
    if (EFI_ERROR (Status)) return Status;

    /*
     * Do the link startup here, with the UIC result visible.  The generic
     * UfsPassThru path checks only HCS.DP and can hide DME_LINK_STARTUP's
     * arg2 (UniPro result).  SkipLinkStartup is set so the generic driver
     * does not run it a second time.
     */
    LinkResult = 0;
    for (Retry = 0; Retry < 3; Retry++) {
      Status = MtkUfsExecUic (Interface, UfsUicDmeLinkStartup, 0, 0, &LinkResult);
      DEBUG ((
        DEBUG_ERROR,
        "UFS MTK: LINK_STARTUP try %u: status=%r arg2=0x%x\n",
        (UINT32)Retry,
        Status,
        LinkResult
        ));
      if (!EFI_ERROR (Status) && (LinkResult == 0)) {
        break;
      }
      MicroSecondDelay (1000);
    }

    DEBUG ((
      DEBUG_ERROR,
      "UFS MTK: link done status=%r arg2=0x%x HCS=0x%x IS=0x%x UECPA=0x%x UECDL=0x%x UECN=0x%x UECT=0x%x UECD=0x%x\n",
      Status,
      LinkResult,
      MmioRead32 (YUNLUO_UFS_HCI_BASE + 0x30),
      MmioRead32 (YUNLUO_UFS_HCI_BASE + 0x20),
      MmioRead32 (YUNLUO_UFS_HCI_BASE + 0x38),
      MmioRead32 (YUNLUO_UFS_HCI_BASE + 0x3C),
      MmioRead32 (YUNLUO_UFS_HCI_BASE + 0x40),
      MmioRead32 (YUNLUO_UFS_HCI_BASE + 0x44),
      MmioRead32 (YUNLUO_UFS_HCI_BASE + 0x48)
      ));
    MtkUfsPause (MTK_UFS_PHOTO_STALL_MS);

    if (EFI_ERROR (Status) || (LinkResult != 0)) {
      return EFI_DEVICE_ERROR;
    }

    MtkUfsStatusText (L"UFS MTK: clearing HCI UIC errors\r\n");
    MtkUfsClearHciErrors ();
    DEBUG ((
      DEBUG_ERROR,
      "UFS MTK: after clear HCS=0x%x IS=0x%x UECPA=0x%x UECDL=0x%x UECN=0x%x UECT=0x%x UECD=0x%x\n",
      MmioRead32 (YUNLUO_UFS_HCI_BASE + 0x30),
      MmioRead32 (YUNLUO_UFS_HCI_BASE + 0x20),
      MmioRead32 (YUNLUO_UFS_HCI_BASE + 0x38),
      MmioRead32 (YUNLUO_UFS_HCI_BASE + 0x3C),
      MmioRead32 (YUNLUO_UFS_HCI_BASE + 0x40),
      MmioRead32 (YUNLUO_UFS_HCI_BASE + 0x44),
      MmioRead32 (YUNLUO_UFS_HCI_BASE + 0x48)
      ));

    /*
     * Baseline ufs_mtk_post_link() runs at POST_CHANGE, which the generic
     * driver reports as EdkiiUfsHcPostLinkStartup.
     */
    MtkUfsPause (MTK_UFS_PHOTO_STALL_MS / 2);
    return EFI_SUCCESS;
  }
  if (Phase == EdkiiUfsHcPostLinkStartup) {
    /* Baseline ufs_mtk_post_link(): enable UniPro clock gating. */
    Status = MtkUfsPostLink (Interface);
    MtkUfsStatusText (EFI_ERROR (Status) ? L"UFS MTK: post-link CG=FAIL\r\n" : L"UFS MTK: post-link CG=OK\r\n");
    MtkUfsPause (MTK_UFS_PHOTO_STALL_MS / 2);
    /* Clock gating is an optimisation; never fail the boot for it. */
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
  mPrivate->Platform.SkipLinkStartup = TRUE;
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
```
