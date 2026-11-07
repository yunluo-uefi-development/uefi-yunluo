# yunluo UEFI — 跨会话跟踪

**最后更新：2026-11-07** · 上一版（UFS 还是 PWM 慢速路径、WinPE 读 `boot.wim` 超时）已归档到 [archive/TRACKING-2026-10-24-pre-hs-fix.md](archive/TRACKING-2026-10-24-pre-hs-fix.md)，只在查阅旧调试过程时使用。

> **权威优先级（用户指令，务必遵守）**：
> `baselines/kernel_xiaomi-yunluo`（原厂最完整实现）> `yunluo-mainline`（最小实现，只用来解释寄存器/补最小链路）。
> 不要从其它 SoC 平台（emerald、MT6765、MT6853 等）抄设备级数据。

---

## 1. 一句话状态

Mu-Silicium 固件**实机验证通过**：显示正常、能进 `Shell>`、UFS 已跑在 **HS-G1**（256 MiB 读约 2.2 s，零超时）。**最终目标是基于这套 UEFI 运行 Windows（ARM64）**；当前不包含 UniXNU/DarwinBoot，也不要再加。

**当前状态（2026-10-25 12:xx）：Windows PE 已经在这台机器上跑起来了 —— 根因是内存表漏标了两个保留区。** 镜像 `ff43a778…` 实机进入 **WinPE 22631 桌面**（Everything / Dism++ / DiskGenius 全在，右下角 `Build 22631` 水印）。根因：`MemoryMapLib.c` 把两段 vendor 保留内存当成了普通空闲 RAM 交给 Windows，Windows 在 Phase0 建页帧/清零时踩进去，破坏常驻协处理器（SSPM/DRAMC）的内存 ⇒ 无 thread 阶段的异常 ⇒ `PANIC_STACK_SWITCH (0x2B)`。补上后立刻能开机，**这是整条链上唯一真正致命的缺失**：
  - `sspm@0x9FE70000` size `0x180000`（1.5 MiB，`compatible = "mediatek,SSPM-reserved"`, `no-map`）—— 原来落在 `"DDR 0x9FE70000"`（Conv）里；
  - `dramc-rk0@0x13FFFF000` size `0x1000`（4 KiB，`mediatek,dramc-rk0`, `no-map`）—— 原来落在 `"DDR 0xFFFFF000"`（Conv）里。
  修法：`SSPM` 后新增 `{"SSPM log", 0x9FE70000, 0x180000, …, Reserv}` 并把后续 Conv 起点改到 `0x9FFF0000`；`0xFFFFF000` 的 5 GiB Conv 段拆成 `Conv 1 GiB` + `{"Res dramc-rk0", 0x13FFFF000, 0x1000, …Reserv}` + `Conv 0xFF200000`，`TEE High` 不变。改后 36 段、仍是 **8.00 GiB 连续无空洞**（`AUDIT: map n=72`）。
  **对照方法（可复用）**：反编译主线 `mt8781-yunluo.dtb`（29 个 `reserved-memory` 节点）→ 逐条做"是否被 EFI 表的 `Reserv` 描述符完整覆盖"的程序化比对 ⇒ 只剩上面两个洞。**原厂 vendor DTB 不能当基线**：它只有 1 个 `ssmr-reserved-cma_memory`（只有 `size` 没有 `reg`），MTK vendor 靠 SSMR/固定布局，不写进 DT（`dtb_addr=0x47C80000`，从 `vendor_boot` 里可抽出）。
  **另外**：随镜像发布的 BCD 里 `BcdOSLoaderInteger_SafeBoot (0x25000080)` 一直是 **1（Minimal）**——早先排查第三方驱动留下的，已清零（元素 blob 前 4 字节 `01000000`→`00000000`，BCD hive 原地改、不动包长），刷 `cust` 生效。





**回退之上叠加的新观测驱动（2026-10-24 22:36:37 构建成功）：`HandoffAuditDxe`**（`yunluoPkg/Drivers/HandoffAuditDxe/`，FDF 第 60 行注册，Depex = Timer+GIC+Clock 三协议）。它就是"重新设计观测手段"的第一步——**不再从代码推断，直接问硬件**。在 DXE dispatch 阶段（远早于 EBS，不改变交接给 OS 的状态）实测五件事，全部输出 `AUDIT:` 前缀（DEBUG_ERROR 级，屏上可见）：
1. **DXE tick 死活**：建 10 ms 周期定时器事件，30 次轮询统计走动次数——tick 只在定时器 ISR 里推进，走动 = 定时器中断真的送达过 CPU（此前整个固件从未证明过任何一次中断交付：UFS 是轮询的，时间戳来自自由计数器）；
2. **SGI 自发自收**：`ICC_SGI1R_EL1` 向自己发 SGI 1（INTID 0 留给 EDK2 IPI），看 handler 是否运行；
3. **GIC/ICC 寄存器快照**：GICD `CTLR/TYPER/IGROUPR/ISENABLER`、GICR `TYPER/WAKER/IGROUPR0/ISENABLER0`、`ICC_SRE/PMR/IGRPEN1` + `MPIDR`；
4. **CNTFRQ 与系统计数器**两次 100 ms 间隔采样 + `CNTP_CTL/CVAL`；
5. **MTK 时钟树**：时钟协议枚举总数/使能数/读失败数。
所有等待都是 `Stall` 忙等，不会挂死。构建修复记录（三次失败后成功）：INF 补 `EmbeddedPkg/EmbeddedPkg.dec`（`gHardwareInterruptProtocolGuid` 定义在那里，**不在 ArmPkg.dec**）；`.c` 里不存在的 `Timer->GetTickCount` 成员改成 `CreateEvent/SetTimer/CheckEvent` 轮询。

**为什么要回退——两条硬结论。** ① **`ArmReadMpidr() = 0x81000000` 解码出来亲和号是 0**（位域 `[7:0]Aff0 / [15:8]Aff1 / [23:16]Aff2 / [24]U / [31]MT / [39:32]Aff3` ⇒ 只有 MT 和 U 两个标志位被置，四个 Aff 全 0；U=1 时 Aff1/Aff2 架构上就是 RAZ），厂商 DTS 独立佐证：`mt6789.dts` 的 `cpus` 是 `cpu0 reg=<0x0000>`、`cpu1 reg=<0x0100>` … `cpu7 reg=<0x0700>` —— **我们从 MT6768 抄的那组 MPIDR 本来就是对的**，7.4.5 那条"硬件是 Aff2=1 ⇒ 该写 0x10000"是把 `0x81000000` 读成了 `0x1<<16`。② 于是因果整个反了：MPIDR=`0x0` 时 Windows 认出自己、进内核、用 bootvid **画出整屏 BSOD**（能画 ⇒ EBS 之后的页表、bootvid、framebuffer 写这条路全通）；MPIDR=`0x10000`（不匹配任何核）时 **EBS 之后什么都不发生** ⇒ 静默挂死比 `0x2B` **更早**，是倒退不是进展。之后那 7 个单变量（DSDT `_STA`、runtime 向量、单条 GICC、KEP、DBG2、GICR 扫描、颜色阶梯）读数全部一致"停在白"，很可能只是在同一处死水里换输入。

**下一步（2026-11-06 现状）：`0x2B` 已关闭，WinPE 22631 能进桌面（`ff43a778…`，10-25）。触摸工作已整体撤回。** 用户 11-06 指令：先"把触摸相关的都给撤回"，再"要的是与 10:47 逐字节一致" ⇒ `mu-silicium` 已从 `/home/zhaoqing412/yunluo-uefi-src-20261025.tar.zst`（2026-10-25 10:47）整树恢复，`diff -rq --exclude=Build` 返回 0。撤回内容：`Applications/TouchTest` 及其 dsc/fdf 行、`MemoryMapLib.c` 里 SPI0/I2C0 的 `AddDev` 区、keypad 边沿触发补丁、`cust` 镜像里的 `TouchTest.efi`/`startup.nsh`。触摸的 bus/pin/协议事实已备份到 `archive/touch-2026-11-06/TOUCH-WINDOWS-PLAN.md`，将来重启时再取。**当前镜像 `949fe2fa…`（= 钉住的 10-25 10:47 树，2026-11-06 18:51:35 构建），已刷入 `boot_a`；屏上仍会有 `AUDIT:` 行（脚手架还在），收尾动作是 WinPE 桌面。真正的待办见第 9 节。**

**2026-11-07 从 [yunluo-uefi-development/uefi-yunluo](https://github.com/yunluo-uefi-development/uefi-yunluo) 合并修复。** 该仓库是从我们 `yunluo-uefi-src-20261025.tar.zst`（SHA-256 `46de770e…`）派生的公开源码 overlay（243 文件，**197 与我们逐字节相同**），基于 **6 GiB 内存**变体测试；用户确认它与我们设备的差别只有内存大小。**已合并（现在 210 文件与其一致）**：
- **UfsPassThru**（`Mu_Basecore/.../UfsPassThruDxe/UfsPassThru.c`）—— 真 bug：`UfsRwDeviceDesc` 是 IN OUT，出错会把 `UnitDescriptorSize` 改成 0，导致后续 LU 全部读不到；改为每轮重置 + `ZeroMem` + 描述符校验，`LunEn != 0` 才算使能，并加 `YUNLUO-LU:` 逐 LU 打印。
- **SimpleFb + OVL 旋转**（`SimpleFbDxe.c/.inf`、新 `OvlScanout.h`、DEC 新 PCD `PcdYunluoFrameBufferRotation`）—— **按他们原样合并，未做任何改造**；DSC 里设 **180**，因为 OEM 把屏装反了（`CON` 置 `BIT9|BIT10`，扫描从最后一个可见字节起算）。
- **BootShim —— 已合并但又【还原】**：他们那版 128 B 把 `FD_BASE`/`FD_SIZE` 写进伪 Linux 头的 Text Offset/Image Size（并去掉 `EDK2-MTK` 标记），实机刷入后 **设备不进 UEFI**（LK 跳转后无任何输出）⇒ 已回退到我们原来的 144 B 版本（两个字段保持 0，magic 在 `0x38`，`EDK2-MTK` 标记保留），`BootShim.S` 与 `Makefile` 与原版逐字节一致。**根因推测**：旧版注释写着 `has to be 0 on some devices` —— MTK 这条路径会校验这两个字段，非 0 即被拒。以后若要再试他们的 shim，先试 `REQUIRES_KERNEL_HEADER=0`（整段伪头都不要）而不是只改字段值。
- **触摸全部** —— `Drivers/YunluoTouchAcpiDxe/`（`Touch.asl` + 生成的 `YunluoTouchSsdt.h`，发布 `_HID="YUNT0001"` 的 SSDT）与 `Drivers/YunluoTouchPlatformDxe/`（私有 DEC+protocol，只发布资源事实 GPIO `0x10005000`／SPI0 `0x1100A000`／GPIO30-33／GPIO8）；**两者都不碰硬件**（无 MMIO/GPIO/复位/固件下载）。已接进 `yunluo.dsc` `[Components]` 与 `yunluo.fdf`。
- **按键** —— 用他们的 `KeypadReleaseToArmLib` 内容覆盖我们的 `Library/KeypadDeviceLib/KeypadDeviceLib.c`（路径不变，DSC 无需改）：① `KEY_CONTEXT_PRIVATE.Armed` —— 必须先看到一次"松开"才允许发按键，治 PMIC 闩锁导致的菜单自动滚动；② 真 bug 修复：`BitmapSet/Clear/Test(gBitmapUnicodeChars, …)` 原来拿 **ScanCode** 索引 UnicodeChar 位图，改为 `UnicodeChar`。

**2026-11-07（第二批）6 GiB SKU 支持 + UFS 只暴露 LU2。**
- **6 GiB / 8 GiB 两套表分开构建**（用户决定不做"一份镜像通吃"）：他们的 `Library/MemoryMap6GLib`（6 GiB 上限，顶部 32 MiB 作 `Top guard 6G`）已原样并入，与我们的 8 GiB `MemoryMapLib` 并存。**刷错方向不可逆**：6 GiB 机器刷 8 GiB 版 = 描述不存在的内存 ⇒ Windows Phase0 崩；8 GiB 机器刷 6 GiB 版 = 安全但只用得到 6 GiB。
- 构建开关（`yunluo.dsc` 里 `!if $(MEMORY_MAP_6G) == TRUE` + 构建脚本传 `-D MEMORY_MAP_6G`，**条件编译已实测生效**，两次构建分别编到 `MemoryMap6GLib` / `MemoryMapLib`）：
  `./build-yunluo-mu.sh DEBUG` → **8 GiB 版（默认）** → `Build/yunluo-mu/boot-yunluo-mu.img`
  `./build-yunluo-mu.sh DEBUG 6g` → **6 GiB 版** → `Build/yunluo-mu/boot-yunluo-mu-6g.img`
- **UFS 只暴露 LU2**（`UfsPassThru.c`）：新增 `YUNLUO_EXPOSED_LU=2`，枚举循环里非 2 的 LU 只打印 `YUNLUO-LU: LU n hidden` 不置位；RPMB 那条（`SecurityLun==1 → BIT11`）改成只打印 `RPMB WLUN present but hidden`，不再暴露。**LUN0/LUN1/RPMB 都不会再出现为块设备**，系统里只剩 `cust` 所在的那一个盘。
- 注意：EDK2 DEBUG 构建**不是逐位可复现**的（同一份源码两次构建 sha 不同，例如通用版先后得到 `4b124490…` / `97465a8d…`），所以 sha 只对那一次产物有效。

**2026-11-07（第三批）UEFI 设置入口进引导菜单 + 图形化。**
- 引导菜单里新增 **"UEFI Settings"** 一项（`PlatformBootOptionsLib` 里注册，`AppGuid` = UiApp 的 `462CAA21-…`，`MdeModulePkg.dec` 没有为它发布 `gUiAppFileGuid`，所以在库里直接写了 GUID）。进 UiApp 后就是标准的 **Device Manager / Boot Manager / Boot Maintenance Manager**。
- FV 里只补 **UiApp** 一个模块（`yunluo.dsc` + `yunluo.fdf`）：`SetupBrowserDxe` 与 `BootManagerMenuApp` 早已由 `SiliciumPkg.dsc.inc` 与 `SiliciumPkg/Extra.fdf.inc` 引入（我一开始重复加，`GenFv` 报 "the 53th file and 57th file have the same file GUID: ebf342fe-…" 才发现）。
- **图形化**：UiApp 走 `gST->ConOut`，而我们的 console 链是 `ConPlatform → ConSplitter → GraphicsConsoleDxe`（绑在 SimpleFbDxe 提供的 GOP 上），`PcdConOutGopSupport=TRUE`，所以设置界面直接在屏上以图形方式渲染，不依赖串口。
- 引导等待时间从 3 s 提到 **5 s**（`PcdPlatformBootTimeOut|5`），因为按键是"松手才武装"，3 s 太紧。开机默认仍启动 "Internal Storage"，按任意键进菜单。
- **实机报错 `Fatal Error! EDKII_FORM_DISPLAY_ENGINE_PROTOCOL not found!` ⇒ 缺 `DisplayEngineDxe`**：`SetupBrowserDxe` 只是表单浏览器，真正画表单、提供 `EDKII_FORM_DISPLAY_ENGINE_PROTOCOL` 的是 `MdeModulePkg/Universal/DisplayEngineDxe`，而 `SiliciumPkg.dsc.inc` / `Extra.fdf.inc` 都没有引它。补进 DSC+FDF 后还缺库 `CustomizedDisplayLib`（`MdeModulePkg/Library/CustomizedDisplayLib`，`SiliciumPkg.dsc.inc` 也没映射），一并加上。**设置 UI 需要四件套**：`UiApp`（前端）+ `SetupBrowserDxe`（浏览器，已有）+ `DisplayEngineDxe`（表单显示引擎，本次补）+ `HiiDatabaseDxe`（HII 数据库，DXE.inc 里已有）；外加 `BootManagerMenuApp`（菜单 App，已有）。完整 FV 校验：`E660EA85…DisplayEngine`、`462CAA21…UiApp`、`EBF342FE…SetupBrowser`、`EEC25BDC…BootManagerMenuApp` 四个 FFS 都在。

**2026-11-07（第四批）仓库改为自包含，并用冷克隆验证。** 仓库迁到 `yunluo-uefi-development/uefi-yunluo`（历史被我们这条真实历史取代，旧 243 文件状态备份在本地分支 `remote-main-backup`）。为了"`git clone` 就能构建"，把 Mu 基座里**实际被编译**的部分一并入库：`Mu_Basecore`、`Common/Mu`、`Common/Mu_OEM_Sample`、`Silicon/Silicium/{SiliciumPkg,SiliciumNullPkg}`、`Silicon/MediaTek`、`Platforms/Xiaomi`、根构建脚本 —— 入库判据不是猜，是对成功构建日志做 grep（只有这些路径出现在编译行里）。**排除**：`Silicon/Silicium/OpensslPkg`(165M，只在 TLS/SecureBoot 时才需要)、`Silicium-ACPI`(47M，仅注释提及)、`Resources`(190M)、`Build`、其它厂商的 Silicon/Platforms、`.github`、`bootchain/vendor_boot.img`(原厂镜像，不入库)。
**冷克隆测出两个只有干净检出才暴露的坑（已修）**：① `mu-silicium/.gitignore` 里有个裸 `Conf` 规则，把 `BaseTools/Conf/target|tools_def|build_rule.template` 一起忽略了 → 必须 `git add -f`；② `BaseTools/.gitignore` 忽略 `Source/C/bin`（上游 Mu 把预编译工具当生成物），冷克隆里根本没有 `GenFfs/GenFw` → 强行入库那 13 个二进制（6.8 MiB），并让脚本在缺失时自己 `make -C BaseTools`。另外脚本的 `PACKAGES_PATH` 改成只挂存在的目录（否则 EDK2 报 `error 000E: One Path in PACKAGES_PATH doesn't exist`），`vendor_boot.img` 缺失时只打包 boot.img 并提示。
**验证**：全新 `git clone` → `./build-yunluo-mu.sh DEBUG` → `build rc=0`，产出 FD 7 MiB + boot.img（kernel 851542 B，sha `2ea852e5…`）。DEBUG 构建不可复现，冷构建 sha 与本地不同属正常。

**未合并（用户明确不需要）**：`CustBootPolicyLib`、`CustAutoBootManagerLib`、`PlatformWindowsBootOptionsLib`、`YunluoBootOptionsLib`、`WindowsBootManager`、`YunluoDualBootManager`、`Test/CustPolicyHostTests.c`、`yunluo-6g{,-cust,-winboot,-dualboot}.dsc/fdf`，以及他们的 **`MemoryMap6GLib`（6 GiB）—— 我们保留自己的 8 GiB `MemoryMapLib`**。他们也未发布 ACPI `.aml` 与 boot/vendor_boot 镜像。合并前备份在 `archive/pre-merge-2026-11-07/`。

**合并后镜像 `da13da65…`**（DEBUG，kernel 813163 B，FD 7 MiB 占用 841800 B），**尚未刷入**。预期变化：屏上画面按 180° 正确显示（此前靠保留 LK 的 flip 位，现在显式设置）、`_SB.YTCH` 会作为未知设备出现在 Windows 设备树里、按键需先松手才响应。

**安全启动这条线已排除，不要再查**（7.4.7）：我们链的是 `SecurityStubDxe`，没有 PK/KEK/db/dbx，`SecureBoot` 变量不存在 ⇒ Windows 视 SB 为关闭；SB 的强制点全在 EBS 之前的 winload，我们已经过了 EBS。

**上一阶段（重新回到台面上）**：WinPE 完整加载 `boot.wim` 并进内核后蓝屏 `Stop code: PANIC_STACK_SWITCH` = **bugcheck 代码 `0x2B`**，官方定义"内核模式堆栈已溢出"、参数 1 = 陷阱帧；反汇编证据与四条已排除理论（EL2→EL1、armed 定时器/GIC 线、`CNTFRQ_EL0`、pending SError）见 7.4 与 6.4 实测表。**注意它能画出 BSOD，说明 EBS 之后的交接、页表、bootvid 写 framebuffer 这条路本来是通的。**

**ACPI 的表集与中断号不要再动**（7.1/7.2/7.3）。已确认**真的错并且已改**的只有一条：7.4.11 DSDT 把 8 个 CPU 全报 present 而 MADT 只使能 1 个（已改成 `_STA=0`）。GICC MPIDR 那条是**我改错了**，正在回退（见上）。

---

## 2. 新会话从这里开始

```bash
cd /home/zhaoqing412/pt/uefi
./build-yunluo-mu.sh DEBUG        # 或 RELEASE
```

产物：

```text
mu-silicium/Build/yunluoPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd   7340032 B
Build/yunluo-mu/boot-yunluo-mu.img                                67108864 B (boot v4)
Build/yunluo-mu/vendor_boot-yunluo-mu.img                         67108864 B（原厂 stock）
```

最新构建（8 GiB 版**已刷入**）：`a64af02333ac529ff2bcde0839d432b24b62b0c3ada27521c418f2f7a3842ce6`（`boot-yunluo-mu.img`，= 设置入口 + DisplayEngineDxe + CustomizedDisplayLib，FV 占用 880984 B）；6 GiB 版仍是 `47c55f1e07e5684969c1135a569976ef95dc96e01d76fda68479d9d3377c299b`（**尚未重编入设置入口**，重编命令 `./build-yunluo-mu.sh DEBUG 6g`）。上一版 `369c2797379cc0009e1f2602933a9ba33366f8f7d2073f1b0f2c291fc8dd0b90`（8 GiB 表、已实机刷入并进系统）证明 shim 还原后链是通的。被它取代的 `da13da65e82967d34056d1e0ecdec3ffff6be9d9fd7fc570a35696e13595fc08` 用的是他们的 128 B shim，**实机不进 UEFI，已弃用**。

最近一次实机镜像：`949fe2fa12a7ed350f3b6e5ddf11d711580e8227792b97aae4c18fb4d541a283`（= **钉回 10-25 10:47 的树**，2026-11-06 18:51:35 构建，触摸已撤回，含内存表修复；见第 1 节。已刷入 `boot_a`）；再往前 `affe973d31c6f9d157653ef99411bac3b295f21aedeb956810e91e4616bc7cb2`（= **回退基线 + HandoffAuditDxe（GICR 偏移修正）+ 真正生效的 quiesce**，7.4.30/31；上一版 `013bafa18643e397c79ec59d28eae9c82f2da5063b5483658fdcac127444ecbe`（= **回退基线 + HandoffAuditDxe 硬件审计**，kernel 810032 B，见第 1 节；纯回退版是 `90cdcc7d847ad2d71bf984e962917c959817d80e5543f3e171d11fd02ac9cde7`，已被取代。被回退掉的探针版依次是 `a10a3e24…`（MPIDR 改回 0x0 但保留全部探针）、`691ac1e7…`（GICR 逐帧扫描，7.4.25）、`cb2fb3bb…`（不发布 DBG2，7.4.21）、`4a2558e9…`（关 KEP，7.4.18）、`cfa20128…`（单条 GICC，7.4.15）、`b2ba1f77…`（runtime 向量，7.4.13）、`fb2d7b05…`（颜色阶梯，7.4.9）、`80a48e75…`（**MPIDR 改成 0x10000 —— 事后证明是误读，7.4.26**）。中间版本存档在 `archive/post-1547-probes-2026-10-24/`。更早一段的阶梯：`cb2fb3bbe56738de411bd92f98b0c3409bb6c4aa3ebcb571b032212624a92ba2`（= 不发布 DBG2，7.4.21；上一版 `4a2558e9bdead18d2d84aecebe92f38ae9fc45eb32f4960690d812f60b13afba`（= 关掉 KEP，已刷入等读数，7.4.17；上一版 `cfa2012858d6ef60f7bab3428486c8c7f2b8de293d8d5f81a42e72c3ba04cad4`（= 单条 GICC 的 MADT，7.4.15；上一版 `b2ba1f773f90413ed1059562cffd8bb6692efa81ea8397481df273408b7cac12`（= **颜色阶梯 + runtime 异常向量**，7.4.13；上一版 `fb2d7b05ac9f4112d55f72a19e8173731387ec2f964a5edaa1392aacefd5fa1b`（= **颜色阶梯**，7.4.9：DXE core + RuntimeDxe 内部各打 3 个上色点，外加 before-EBS 的 `XMAP:` 内存映射 X 光；`SimpleFbDxe`/`ProbeRtDxe` 的上色只涂上半 60%）。里程碑版本：`e1862270…` = 探针搬进 runtime driver，**仍纯蓝**；`3cd03938…` = RuntimeDxe 的 `REPORT_STATUS_CODE` 换成 Null + RT 服务染色探针（`GetVariable`=青等），**仍纯蓝、文本不变**；`0dd3b38a…` = 两色探针第一次读到整屏蓝；`80a48e75…` = **APIC 里 CPU0 的 GICC MPIDR 改成 `0x10000`，0x2B 蓝屏消失、变成 EBS 后静默挂死**（原表备份 `archive/apic-before-mpidr-2026-10-24/`）；`b210284b…` = before-EBS X 光探针（读数已抄进 6.4，代码已撤）；`4fe3b58a…` = 只静默不打印；`e8aebd82…` = MT6789 SEC 补 EL2→EL1（**无变化 ⇒ 本来就是 EL1**，原版在 `archive/PlatformHelper.S.mt6789-before-el1-demote-2026-10-24`）；`37df6874…` = ACPI 回退 + 补 `DBG2`/`SSDT`。

刷机（设备**不支持 `fastboot boot`**，只能刷 `boot_a`；vendor_boot 已刷过原厂 stock）：

```bash
fastboot flash boot_a Build/yunluo-mu/boot-yunluo-mu.img && fastboot reboot
```

> 设备会很快自动退出 fastboot，把上面这条**放后台跑**，它会阻塞等待设备上线。

**不要再回头排查**（结论在第 5、6、7 节）：UFS link startup、UIC DME 前置、HCE、时钟 gate、SMC ABI、UTRCS 完成标志、描述符 cache 属性、ScsiBus 依赖、显示几何、PRDT/UTRD 布局与 256 KiB 分段、AHIT、内存表、门铃/槽位恢复逻辑、**ACPI 中断号按 GSIV 改**、ACPI 表数量/表集（6 张已等同 lancelot）、CPU 数量、BCD SafeBoot（排除了第三方驱动）、**MADT/DSDT 里的在场 CPU 数**（8→1 两边都改过，两次都毫无变化，7.4.12/7.4.16）、**EL2→EL1 降级**、**before-EBS 静默定时器+屏蔽清除全部 GIC 线**、**`CNTFRQ_EL0`（实测 13 MHz）**、**pending SError（实测无）**、**EBS 之后的异常（runtime 向量表实机无 dump ⇒ 无故障空转，7.4.14）**、**Windows 安全启动（`SecurityStubDxe` ⇒ SB 变量都不存在，且 SB 强制点在 EBS 之前）**、**MAT/NX 被 Windows 强制（SB=off 时 Windows 不遵守 `EFI_MEMORY_ATTRIBUTES_TABLE`）**。全部已实机定论。**但 GICC MPIDR 是例外：那个必须改，改了蓝屏就没了（7.4.5）。**

---

## 3. 路径与产物

| 项 | 值 |
|---|---|
| 工作区 | `/home/zhaoqing412/pt/uefi` |
| 固件树 | `/home/zhaoqing412/pt/uefi/mu-silicium`（Mu-Silicium main + 固定子模块，**无 git 元数据**） |
| 板级包 | `mu-silicium/Platforms/Xiaomi/yunluoPkg` |
| 同级参考平台 | `mu-silicium/Platforms/Xiaomi/lancelotPkg`（MT6833，`Status.md` 里 Windows Boot ✅，**唯一已验证能起 WinPE 的 MTK 平台**，对比差异先看它） |
| 引导打包 | `bootchain/`（`BootShim/` 源码、`Tools/mkbootimg.py`、原厂 `vendor_boot.img`） |
| 构建脚本 | `build-yunluo-mu.sh`（classic `build`，CLANGPDB，LLVM 22） |
| 基线参考 | `/home/zhaoqing412/pt/kernel/yunluo/baselines/kernel_xiaomi-yunluo`、`/home/zhaoqing412/pt/kernel/yunluo/yunluo-mainline` |
| 历史归档 | `archive/`（旧 TRACKING、旧 ENVIRONMENT、edk2-yunluo 平台/UFS 参考源码、Mu 上游 tar、Pearl 之前的 UFS 驱动备份） |
| UFS 移植说明 | `UFS-PORTING.md`（面向其它 MTK 设备开发者的实现说明 + 完整平台驱动源码） |
| ACPI 表 | `mu-silicium/Platforms/Xiaomi/yunluoPkg/AcpiTables/`（FACP/APIC/GTDT/DSDT，`.before-gsiv` 是修正前备份） |

> 固件树不是 git 仓库，改动没有版本历史。重要结论写进本文档。

---

## 4. 启动链与内存布局

- LK 把 boot v4 的 kernel 解压到 `0x40000000` 并跳转；kernel = gzip(我们的 BootShim + 7 MiB FD)。
- BootShim 把 FD 拷到 `0x50000000` 后跳转；FD 范围 `0x50000000–0x50700000`。
- 栈 `0x40C00000`，DXE heap `0x40C50000`。
- RAM **8 GB** `0x40000000–0x240000000`（已用 mainline `mt8781-yunluo.dts` 的 `reg = <0 0x40000000 0x2 0x00>` 复核），静态内存表按 yunluo DTS 保留区雕刻。
- **`MemoryMapLib.c` 已逐条核算过：`AddMem` 项从 `0x40000000` 到 `0x240000000` 完全连续、无重叠**。`{"DDR 0xFFFFF000", …, 0x13F201000}` 的末地址正好落在 `TEE High`（`0x23F200000`）上，看着像越界其实是错觉——别再怀疑内存表。
- **刷机只认 `boot_a`**；`fastboot boot` 不可用。

---

## 5. 显示（已实机验证）

- 竖屏 **1200×2000**，pitch **4800 B**（1200 px × 4），FB `0xFE2C3000`，三缓冲。
- `SimpleFbDxe` PCD：Address `0xFE2C3000`、Stride/VisibleWidth `1200`、Height `2000`、Layers `3`、PixelFormat `1`（`PixelBlueGreenRedReserved8BitPerColor`，微软要求）。
- OVL 交接：`ROI/SRC_SIZE = (2000<<16)|1200 = 0x07D004B0`，`CON = (CON & ~0x010F000) | RGBA8888 (2<<12)`、无 byte swap（对应 BGRR 内存序），`PITCH=4800`，只改指向 FB carveout 的启用层。
- `FrameBufferSerialPortLib` 的 `FB_STRIDE=1200`。
- **旧记录 2000×1200 / 2048 是错的**，不要再改回去。

---

## 6. UFS（已实机跑通，HS-G1）

### 6.1 组件栈

```text
MtkUfsDxe (平台 Host 协议 + baseline 钩子, yunluoPkg/Drivers)
  → UfsPassThruDxe (ExtScsiPassThru, = Pearl/MT6895 版整体采纳)
  → ScsiBusDxe     (ScsiIo)
  → ScsiDiskDxe    (BlockIo)
  → DiskIoDxe / PartitionDxe / Fat
```

`UefiScsiLib`、`ScsiDiskDxe`、`ScsiBusDxe` 都必须在 DSC + APRIORI + DXE 中注册。**缺 `ScsiBusDxe` 时 `ScsiDiskDxe` 不会启动**（它绑 `ScsiIo`）。

### 6.2 实机结果

```text
UFS MTK: LINK_STARTUP try 0: status=Success arg2=0x0
UFS MTK: pwrmode=85 [#.##.#.] txgear=1 hsseries=1 txterm=0     ← 改之前：停在 PWM
UFS MTK: link up, later cycles go quiet
UFS MTK: HS pwrmode=17 [#...#..] HS txgear=1 HS txpwr=3 HS txterm=1   ← HS/HS 生效
UFS op=0x28 lun=2 lba=0x273D10 len=0x10000 i d=17852 -> Success
UFS op=0x28 lun=2 lba=0x2861CE len=0xFFFFF000 i d=28825731 -> Success   ← 256 MiB，约 2.2 s
[KEP] Winload_text Memory Range = 0x40703000 - 0x408BB000
```

`ScsiDisk`：SKhynix HN8T062EHKX039，`block=0x1000`；装 GPT/`cust` 的是 **LUN2**（31 GiB），LUN0/1 各 4 MiB。Shell 枚举 `blk0`–`blk71`。

### 6.3 关键结论（踩过的坑）

1. **整条存储失败的唯一根因：UniPro 链路从未离开 PWM-G1。** EDK2 和 Pearl 驱动都不做 Linux 在 link startup 之后的 `DME_SET(PA_PWRMODE)`，所以只有 1–4 MB/s；`ScsiDiskReadSectors` 的 `MaxBlock=0xFFFF`（4K 块 = **256 MiB 单条 READ(10)**）必然超过 30 s 的 `SCSI_DISK_TIMEOUT`，看起来像"卡死/超时"。修法：`MtkUfsChangePwrModeHs()` —— 先 best-effort 设 `PA_TXGEAR/RXGEAR/ACTIVERXDATALANES/TXTERMINATION/RXTERMINATION/HSSERIES`（**忽略每个返回值，和 `ufshcd_change_power_mode()` 一致**），W1C `IS.UPMCS`，`DME_SET(PA_PWRMODE, 0x11)`，轮询 `IS` bit4（≤500 ms）。`set hsseries=16 rejected code=2` 是无害的。
2. **`DME_SET` 的值放在 `UICCMDARG3`**（和 DME_GET 的返回同一个寄存器），`ATTR_SET_NOR=0` ⇒ `ARG2=0`。我们的 `MtkUfsExecUic()` 历史上把值塞在 ARG2，等于所有 DME_SET 都静默写 0。
3. **`PA_PWRMODE = 0x1571`**（`0x1569` 是 `PA_TXTERMINATION`）。原厂 `unipro.h`：`FAST=1 SLOW=2 FASTAUTO=4 SLOWAUTO=5 UNCHANGED=7`，值 = `pwr_rx<<4 | pwr_tx`，HS/HS = `0x11`；改前读回 `85 (0x55)`。
4. **完成标志是 `IS.UTRCS` / UTRD 的 OCS**，不是门铃 `UTRLDBR`（取走请求时就清零，`DBR=1` 不能证明"没取走"）。`OCS == 0x0F` 是初始哨兵值，不是错误码。
5. **描述符环用 `EFI_MEMORY_WC`（Normal Non-Cacheable）**，不能用 `EFI_MEMORY_UC`（packed UPIU 非对齐访问会 Alignment fault）。数据缓冲区仍由 `Map/Unmap` 做 cache 维护。
6. **UIC DME GET 的返回值在 `UICCMDARG3`**，不是 `ARG2`。UIC `DmeLinkStartup` 是 **`0x16`**（`0x08` 会静默超时）。
7. **设备 `RST_n` 之后必须重跑 `UfsFinishDeviceInitialization()`**，否则 LUN 回 `sense 0x02 / ASC 0x3A`；重跑后第一条命令回一次性的 **UNIT ATTENTION（`0x06 / 0x29 / 0x02`）**，EDK2 SCSI 栈不会重试 UA，所以 `UfsExecScsiCmds` 里做了**一次性重试**（保存/恢复 `In/OutTransferLength`）。UA 是**按逻辑单元**各自布防的，枚举阶段每个 LUN 的第一条 TEST UNIT READY 都会命中。
8. **不要在健康路径里加门铃卡死检测 / UIC 唤醒 / `UTRLCLR` 重挂**。实机试过，把正常在途命令误判为卡死，结果所有读都失败、Shell 只剩 4 个 LUN，已回滚。
9. `AHIT`（HCI+`0x18`，不是 `0x64`，那是 UTRLCLR）与本次超时无关：`0x03000096` 和 0 都一样。HCI 关闭时写 AHIT 不锁存。
10. **`EFI_BLOCK_IO_MEDIA.MediaId` 不是 LUN**（`ScsiDiskDxe` 拿它存 removable 标志 0/1），按它过滤的诊断永远不执行。

已对齐 baseline `ufs-mediatek.c`：PreHce 顺序（device reset → 三段 reset → crypto 关 → refclk）、PHY/时钟、PreLink 顺序（`PA_LOCALVERINFO` → `UNIPRO_POWERDOWN=0` → `TX_LCC=0` → `SAVEPOWER` 清 bit6）。

### 6.4 硬件事实

| 项 | 值 |
|---|---|
| UFS HCI | `0x11270000`，大小 `0x2300`，IRQ 137 |
| UFS PHY | `0x01FA0000`（DTS 节点名 `ufs-phy@11fa0000` 的 unit-address 是误导，reg 才是准的） |
| HCI 版本 / 能力 | `0x200` / raw `0x3587071F`（64ADDR 置位；清掉行为不变） |
| HCS / IS | HCS `0x30`：Dp=0 UtrlRdy=1 UtmrlRdy=2 UcRdy=3（`0xF` 正常）；IS `0x20`：UTRCS=0 UPMCS=4 UCCS=10 |
| UECPA | `0x38` = Ec[4:0] + Err bit31；`0x80000010` 是 link startup 期间正常的 PHY LINERESET |
| 时钟 | 5 个 Infra gate（`ufs`/`ufs_mp`/`unipro_tick`/`unipro_sys`/`ufs_aes`）+ `ufs_sel` + `aes_ufsfde_sel` |
| 复位 | `infracfg_rst 1/2/3` = `hci_rst`/`unipro_rst`/`crypto_rst`；**没有 `mphy_rst`** |
| 供电 | `mt6358_vemc_reg`（`ldo_vemc` 3.0 V） |
| SMC | `MTK_SIP_UFS_CONTROL = 0xC2000276`（BIT1 device reset / BIT2 crypto / BIT3 refclk / BIT7 device pwr） |
| 设备 | SKhynix HN8T062EHKX039，4K 逻辑块，4 个 WLUN |
| 性能 | HS-G1 单通道：64 KiB ≈ 18k ticks，256 MiB ≈ 28.8M ticks（计数器约 13 MHz ⇒ ≈115 MB/s） |

**CPU / GIC / 定时器实测（2026-10-24 的 before-EBS X 光探针，照片放大逐字确认）**

| 项 | 值 |
|---|---|
| 运行 EL | **EL1**（`el=1`）⇒ MT6789 SEC 补 EL2→EL1 降级是纯 no-op，EL 这条排除 |
| 启动核 MIDR | `0x412FD050` = ARM implementer 0x41、**Primary Part Number 0xD05 = Cortex-A55**、r2p0 |
| 启动核 MPIDR | **`0x81000000`** ⇒ U=1、**Aff2=1、Aff1=0、Aff0=0**，线性硬件 ID = **`0x10000`** |
| 与 MADT 的矛盾 | 我们的 APIC（照抄 `MT6768/APIC.UniCore`）8 条 GICC 的 MPIDR 是 `0x0,0x100,…,0x700`（Aff1=0..7、Aff2=0），**没有一个等于 `0x10000`**；原厂 `mt6789.dts` 的 `cpu reg` 也是 `0x0000,0x0100…0x0700`（Android 八核正常，说明 Linux 侧不在乎或 MTK PSCI 自己做了映射，但 **Windows HAL 是拿 MPIDR 去匹配 GICC 的**） |
| SCTLR_EL1 | `0x30D0180D` ⇒ M=1 C=1 SA=1 I=1，**MMU 与 D-cache 都是开的** |
| `CNTFRQ_EL0` | **`13000000`**（13 MHz，非 0）⇒ Samsung 那条 "CNTFRQ_EL0==0 要打 ntoskrnl 补丁" 的路不适用 |
| 交接时 GIC | `GICD ctlr=0x12 typer=0x790413`，`GICD_ISENABLER0/1 = 0/0`；`RDB0 ctlr=0x1 typer=0x0`，`ISENABLER/ISPENDR/ISACTIVER = 0/0/0` ⇒ **没有任何线使能、无 pending、无 active**（之前那个 `sgi=0x6C000000` 读数不是 `GICR_ISENABLER0`，别再拿它当依据） |
| 交接时定时器 | `CNTP_CTL_EL0=0x1`（ENABLE=1、IMASK=0）、`CNTP_CVAL=0x17C80F40` 而 `CNTPCT=0x17C69A88` ⇒ EDK2 确实留了一个约 **7 ms 后到期的 armed 物理定时器**；`CNTV_CTL=0x0`、`CNTV_CVAL=0xEFE4B00656AEE754`（垃圾值但已 ENABLE=0） |
| pending SError | **无**：before-EBS 里 `ArmEnableAsynchronousAbort()+ArmEnableInterrupts()` 后停 300 ms 打出了 `survived, nothing pending` ⇒ EDK2 全程屏蔽 SError 攒下外部 abort 这条也排除 |

### 6.5 尚未移植（需要时再加）

`PA_TXHSADAPTTYPE`、`DEVICE_PWR_CTRL`（必须在读描述符之后）、inline crypto 密钥、gear/lane 上限协商、`ufs_mtk` 其它 quirk。

---

## 7. ACPI 与 Windows 启动（进行中）

### 7.1 表集现状

`mu-silicium/Platforms/Xiaomi/yunluoPkg/AcpiTables/`：`FACP` `APIC` `GTDT` `DSDT` `SSDT` `DBG2` 六张，与 lancelot 的表集一致。

- `FACP` `GTDT` `DSDT` `DBG2` 与 `Silicium-ACPI/Silicon/MediaTek/MT6768/` 逐字节相同；`SSDT` 来自 `Silicium-ACPI/Common/SSDT.aml`（只有一个 `_HID("SSDE")`）；`APIC` 现在只差 OEM 字段。
- `DSDT`：8×`ACPI0007` CPU（`_STA=0x0F`）。`APIC`：8×GICC + GICD `0x0C000000` + GICR `0x0C040000`/`0x200000`，**只有 CPU0 `flags=1`**，Efficiency Class `0,0,0,0,1,1,1,2`（照抄 `APIC.UniCore.aml`）。
- `DBG2` 指向 uart `0x11002000`，与 MT6789 DTS 的 `serial@11002000`（`GIC_SPI 141`）一致，可原样使用。
- 平台不做任何 ACPI 运行时改写：MTK 链 `AcpiTableUpdateLibNull`，高通版也只改 DSDT 命名对象。`.aml` 就是 Windows 看到的。

### 7.2 已定论：ACPI 中断号本来就是对的，别再动

**ACPI 的 GSIV 就是 GIC INTID，而 PPI 占 INTID 16–31。坑在于误以为 PPI 基址是 32——32 是 SPI 的基址。** 原厂驱动 `drivers/irqchip/irq-gic-v3.c` 的 `gic_irq_domain_translate()`：

```c
case 0:  /* SPI */  *hwirq = fwspec->param[1] + 32;
case 1:  /* PPI */  *hwirq = fwspec->param[1] + 16;
```

| 字段 | 表里的值 | DTS 来源 | 曾被误改成 |
|---|---|---|---|
| GTDT secEL1 / NSEL1 / virt / NSEL2 | `0x1D 0x1E 0x1B 0x1A` | `GIC_PPI 13/14/11/10` + 16 | 45/46/43/42 |
| GICC Performance Interrupt | `0x17` (23) | PMU `GIC_PPI 7` + 16 | 39 |
| GICC Virtual GIC Interrupt | `0x19` (25) | `arm,gic-v3` `GIC_PPI 9` + 16 | 41 |

**MTK 参考表和全部 17 个高通 WoA SoC 的 ACPI 都是这组正确值。** 实机反证：改成 `32+n` 后 Windows 拿到的是一组合法但不存在的 SPI 号，它照单全收去武装，现象从"能正常显示的 `PANIC_STACK_SWITCH` 蓝屏"退化成 `ExitBootServices` 之后的**静默挂死**。已全部回退，备份在 `archive/acpi-before-gsiv-2026-10-24/`。

**表数量也已排除**：补齐 lancelot 缺的两张（`DBG2` + `Common/SSDT.aml`）后实机毫无变化，仍是 `PANIC_STACK_SWITCH`，说明 Windows 根本没在用那个 DBG2 串口。FACP 与高通真机表在所有 ARM 相关字段上逐字节相同（`profile=8`、`flags=0x00300000` HW_REDUCED+LPS0、`armflags=0x1`、rev 6）。

与高通剩下的真实差异（**尚未验证**，属后备候选）：高通 GTDT 四个 timer flags 是 `0x4`（Always On）、`PlatformTimerCount=1` 且带 watchdog（表长 156），我们是 `0x0`/`0`（表长 96）；高通 MADT 8 核全开并填了 per-CPU `Redistributor Base Address`；高通还带 `PPTT`/`IORT`/`MCFG`/`CSRT`。

### 7.3 WinPE 现状与已排除项

- `BYDJ_PE_V1.5.0.iso`（ARM64，底座 build **22631**、`EDITIONID=Enterprise`、9269 个文件，第三方深度改版）已做成 1 GiB FAT32 `Build/winpe/cust-winpe.img`，`fastboot flash cust` 刷入；GPT 第 68 项 `cust`，LUN2，LBA `0x273900–0x2B38FF`。
- 基线现象：BDS 挂载 FAT → `bootmgfw.efi` → 完整读完 `boot.wim` → `[KEP] Winload_text Memory Range = 0x40703000 - 0x408BB000` → 蓝屏 `Stop code: PANIC_STACK_SWITCH`。
- **BCD 安全模式实验已做完，无结论性变化**：往对象 `{7619dcc9-fafe-11d9-b411-000476eba25f}` 写入 `BcdOSLoaderInteger_SafeBoot (0x25000080) = 1` 后蓝屏一模一样，说明凶手不是 BYDJ 注入的第三方驱动（SafeBoot 只跳过非微软驱动，内核/HAL 照跑）。方法见第 8.1 节。
- **屏上探针实测到的事实**（临时构建，代码已回退，方法记在第 8 节）：`EfiExitBootServices` **成功返回**；Windows 继承的内存表健康——`n=92 sz=48 ver=1 usable=7975 MB top=0x23F200000`、`rtcode=4 rtdat=1 other=53 rtNoRunAttr=0`；GIC 交接状态干净——`spi enabled=0`、`redist0 sgi=0x6C000000 ppi=0x00000000`。
- **已排除**：内存表（见第 4 节，逐条核算连续）；`ConvertPages: failed to find range 10000000-102C5FFF` / `102000-102FFF`（`MdeModulePkg/Core/Dxe/Mem/Page.c:688`，Windows 加载器请求 1 MB 以下优先地址失败后重定位，lancelot 同样有）；UFS 杂散中断（驱动从不写 `IE` `0x24`，中断是屏蔽的）；CPU 数量（单核 MADT 蓝屏不变；MPIDR 修好后又裁成单条 GICC，仍无变化，7.4.16）；~~KEP 勘误补丁器~~ **（这条已重新打开，见 7.4.17：`ErrataPatchesLibNull` 只说明不注入 shell code，但页保护那两趟和 `gBS` 钩子照旧，而且 MPIDR 修好之前那个更早的异常死法把所有后置问题都遮住了）**。
- **蓝屏 QR 码没有诊断价值**：解出来只有 `https://www.windows.com/stopcode`，不含 bugcheck 参数。要参数只能上内核调试（uart0 `0x11002000` + 改 PE 的 BCD）。解图用 `pip install --user opencv-contrib-python-headless` + `cv2.wechat_qrcode.WeChatQRCode()`。
- 已知待办：变量仍是 RAM 模拟；RTC 是 Null；设备无 USB 输入，PE 能启动也无法交互。

---

### 7.4 定论：`PANIC_STACK_SWITCH` = bugcheck `0x2B`，抛点是内核"无 thread"VBAR 的 SP_ELx 四个槽

之前把它当成 `0x139 KERNEL_SECURITY_CHECK_FAILURE` 去搜，方向整个是错的。证据是从设备上真正在跑的那份 build 22631 里挖出来的，方法可复用：

```bash
cd Build/winpe/root/sources
7z x -y boot.wim 'Windows/System32/ntoskrnl.exe' 'Windows/System32/drivers/*' 'Windows/System32/hal*.dll' 'Windows/System32/securekernel.exe'
llvm-objdump-22 -d --no-show-raw-insn ntoskrnl.exe > /tmp/nto.asm
```

- `PANIC_STACK_SWITCH` 串在文件偏移 `0x9b7ebc`（属 `.data`，VA `0xC1F4BC`）那张"码→名字"表里。表项 = `名字 + \r\n + 补零到 4 字节对齐`，紧随其后的 4 字节是**本项总长**，所以能顺序走表；用已知锚点 `MEMORY_MANAGEMENT = 0x1A` 校验索引即代码 ⇒ **`PANIC_STACK_SWITCH = 0x2B`**。
- 对全部 31 个节扫 `bl` 到导出 RVA `0x4fcd70`（`KeBugCheckEx`）的调用点，共 **2025 个**；其中唯一真实的 `mov x0, #0x2b` 站点在 **VA `0x140203c1c`**，参数 `x1 = sp`（trap frame，里面存着 ELR/SPSR/ESR），`x2..x4 = 0`。
- 跳进这段 panic 桩的是 **VA `0x140202a28 / 0x140202a80 / 0x140202b00 / 0x140202b80`** —— 一张 VBAR 表里 `Current EL with SP_ELx` 的 **Sync / IRQ / FIQ / SError** 四个槽。同一张表（基址 `0x140202800`）的 `SP_EL0` 四个槽是真 handler；`0x140203bc8` 的选择逻辑是 `[KPCR+0xf8] == 0` 用这张、否则用 `0x140207000`。
- 所以含义很硬：**异常发生时内核还没有 current thread，且运行在 SP_ELx 上**（Phase0 极早期）。四个槽里只有 Sync 留了 0x28 字节的白名单（`ELR - 0x14020b6e8 <= 0x28` → 走 fixup），其余无条件 panic。真 handler 的入口是 `mrs x18, SP_EL0 / and sp, x18, #~0xf / sub sp, sp, #0x370`，即**它拿 SP_EL0 当建 trap frame 的栈**。
- 拿不到参数：QR 码只含 `https://www.windows.com/stopcode`，`boot.wim` 只读，无串口。所以只能做固件侧单变量实验。

由"交接给 OS 时的 CPU 状态"推出的候选（都与 ACPI 内容无关，正好解释为什么补表/改中断号/改核数全无变化）：

0. **官方文档已确认语义**（[Bug check 0x2B PANIC_STACK_SWITCH](https://learn.microsoft.com/zh-cn/windows-hardware/drivers/debugger/bug-check-0x2b--panic-stack-switch)）：**"内核模式堆栈已溢出"**，参数 1 = **陷阱帧**，参数 2–4 = Reserved，原因写的是"驱动用了过多堆栈空间 / 内核严重数据损坏"。与上面反汇编完全吻合：栈没了 → 取异常 → 建不了 trap frame → 无条件 panic。**"内核栈被打爆"最典型的成因是一个清不掉、在反复递归的中断。**
1. **异常等级 EL —— 已实机验证，无变化（`e8aebd82…`）。** `Silicon/MediaTek/MT6768Pkg/Library/PlatformSecLib/AArch64/PlatformHelper.S:6-20`（lancelot）会 `HCR_EL2 = 1<<31(HCD)`、`ELR_EL2 = x30`、`SPSR_EL2 = 0x3C5`(EL1h) 再 `eret` 把 EL2 降到 EL1，MT6789 原来是空 stub；照抄后蓝屏一模一样。该分支在 EL1 入口时是纯 no-op，所以这个结果同时说明**我们本来就是 EL1 启动**，EL 这条排除。
2. **`CNTFRQ_EL0`。** GTDT 里根本没有计数器频率字段（`0x24` 是 `0xFFFFFFFF` 的 Counter Block 基址），频率只能来自 `CNTFRQ_EL0`。手机 SoC 上它常年是 0，Silicium 树里**只有 Samsung 平台**为此给 ntoskrnl 打补丁：`Silicon/Samsung/SamsungPkg/Library/ErrataPatchesLib/ErrataPatchesLib.c:18-42`（把 `mrs x?, CNTFRQ_EL0` 原地换成 `mov x?, #imm`，头注释明写 "not cause a bugcheck due to CNTFRQ_EL0 value being 0"，DuoWoA 作者 2022）。MTK 链的是 `ErrataPatchesLibNull`，不补。**先验**：我们屏上时间戳和 UFS 的 tick 测量数值都是合理的，所以 `CNTFRQ_EL0` 大概率非 0。
3. **架构定时器 / GIC 线在交接时仍然 armed。当前实验（X 光探针 `b210284b…`）。** 之前屏上探针读到 `redist0 sgi=0x6C000000 ppi=0x00000000`：标签写 sgi 的那个读数 bit26/27/29/30 其实落在 **PPI 区间**，最可能是 **`GICR_ISENABLER0`（它同时覆盖 SGI 0–15 和 PPI 16–31）**，而 `ppi=0` 那路大概是 GICv3 下 RAZ 的 `GICD_ISENABLER0`（探针代码已回退，无法复算，所以按"很可能"处理）。若成立，则 EBS 时 GTDT 点名的四个架构定时器中断（INTID 26/27/29/30）**全是使能的**。EDK2 唯一的静默点在 `Mu_Basecore/ArmPkg/Drivers/TimerDxe/TimerDxe.c:438`（`EVT_SIGNAL_EXIT_BOOT_SERVICES` → `ArmGenericTimerDisableTimer()`），只清 `CNTP_CTL_EL0.ENABLE`（我们链的是 PhyCounterLib，**虚拟定时器 CNTV 根本没人管**），也**不清 pending/active**。新增 `Platforms/Xiaomi/yunluoPkg/Drivers/HandoffQuiesceDxe/`：注册 `gEfiEventBeforeExitBootServicesGuid`（在 `MdeModulePkg/Core/Dxe/DxeMain/DxeMain.c:790` 于内存表终止之前同步触发），做 `CNTP/CNTV_CTL_EL0 = IMASK` + `CVAL = 0x7FFF…`，再对全部 `GICD_TYPER` 数出的 SPI bank 和 RDB0 的 SGI/PPI bank 写 `ICENABLER/ICPENDR/ICACTIVER = 0xFFFFFFFF`。GIC 基址取自 `MemoryMapLib` 的 `"GIC Distributor"` / `"GIC Redistributors"`，不硬编码。
   **注意**：`4fe3b58a…`（只静默、不打印）蓝屏不变，但屏上无法证明驱动加载过、事件触发过，**所以那次不算结论**。`b210284b…` 在同一回调里打 pre/post 状态、然后故意清 SError 屏蔽（EDK2 全程 `DAIF.A=1`，攒下来的异步外部 abort 只会在 Windows 清掩码时炸）、最后 `WFI` 死循环停住，保证文字留在屏上可拍。读数：`pre` 块没出现 ⇒ 事件没触发；停在 `unmasking` 而没打 `survived` ⇒ 有 pending SError。
4. 挂起的 SError（EDK2 全程屏蔽 SError，`DAIF.A` 一直是 1，所以一个 DXE 期间攒下来的外部.abort 只会在 Windows 第一次清 `A` 时炸）。**已排除**：before-EBS 清掩码后停 300 ms 打出了 `survived, nothing pending`。
5. **MADT 的 MPIDR 与硬件不符 —— 已实机验证有效（`80a48e75…`）：0x2B 蓝屏消失了。** 启动核 `MPIDR_EL1 = 0x81000000`（Aff2=1 ⇒ 线性 `0x10000`），而 APIC 里 8 条 GICC 的 MPIDR 是 `0x0,0x100…0x700`（从 `MT6768/APIC.UniCore` 照抄），启动核没有任何匹配项。只把**唯一使能的 CPU0** 那条 GICC 的 MPIDR（文件偏移 `0x2C+0x44 = 0x70`，u64）从 `0x0` 改成 `0x10000` 并重算 checksum（原表备份 `archive/apic-before-mpidr-2026-10-24/APIC.aml.orig`）之后：**不再蓝屏，改成 `ExitBootServices` 之后的静默挂死**，屏幕停在 winload 期的 UEFI 控制台文本 + `SecurityLock::LockType: SOFTWARE_LOCK ... Lock Capsule Interface`（该打印来自 `LockCapsuleInterface`，注册在 `gEfiEventExitBootServicesGuid`，见 `Mu_Basecore/MdeModulePkg/Universal/CapsuleRuntimeDxe/CapsuleService.c:476`）⇒ **EBS 已进入并跑完事件表**。剩下的 7 条 GICC 的 MPIDR 还没填对（多核暂时无从验证，先不动）。
   **坑**：改 `.aml` 后 build 日志里搜不到 `AcpiTables`，别以为没重建——直接查 `Build/yunluoPkg/DEBUG_CLANGPDB/FV/Ffs/*AcpiTables/*SEC2.*.raw`（RAW 段前有 4 字节长度前缀，所以表内 `0x70` 在 raw 的 `0x74`）；FD 里搜不到 `APIC` 明文是因为 FV 被压缩了。
6. **定界挂死位置：两色阶段探针。实机读数 = 整屏蓝（`0dd3b38a…`）⇒ EBS 事件表跑完、`SetVirtualAddressMap` 没有到达我们。** 探针在 `SimpleFbDxe` 里注册两个事件，把 scanout 缓冲整屏刷色：`gEfiEventExitBootServicesGuid` → **蓝**（`0x000000FF` BGRX），`gEfiEventVirtualAddressChangeGuid` → **绿**（`0x0000FF00`）。照片里蓝色之上还有两行 winload 期的控制台文本（capsule 锁那行），说明文字是在我们刷色**之后**打上去的，即 EBS 事件表还在往下走。
   **这个读数的唯一漏洞**：VAC 事件确实会进 Runtime AP 的事件表（`CoreCreateEventEx` 在 `Mu_Basecore/MdeModulePkg/Core/Dxe/Event/Event.c:495` 把 `EVT_RUNTIME` 事件挂到 `gRuntime->EventHead`，事件结构体本身是 `AllocateRuntimeZeroPool`），但 `RuntimeDriverSetVirtualAddressMap` 是在**搬移任何镜像之前**、用**物理地址**直接调 `RuntimeEvent->NotifyFunction`（`Mu_Basecore/MdeModulePkg/Core/RuntimeDxe/Runtime.c:297`）。`SimpleFbDxe` 是 `DXE_DRIVER`，那个函数体在 `EfiBootServicesCode` 里——Windows 只有顺手把全部内存 1:1 映射了才进得去。所以"没变绿"既可能是"没调 VAC"，也可能是"调了但没映射我们的页"。
7. **安全启动 —— 排除，三条硬理由。** ①我们链的是 `MdeModulePkg/Universal/SecurityStubDxe`（所有 Xiaomi 平台都一样），没有 PK/KEK/db/dbx，`SecureBoot` 变量根本不存在 ⇒ `GetVariable(L"SecureBoot")` 返回 NOT_FOUND ⇒ Windows 认为 SB **关闭**。②SB 的强制点全在 **EBS 之前的 winload**（`LoadImage`/`HandoffTables`/`EFI_IMAGE_EXECUTION_INFO`），真失败会直接报 `EFI_SECURITY_VIOLATION`/"安全启动策略违反"，走不到 EBS；而蓝色已经证明过了 EBS。③EBS 之后唯一与 SB 有关的流量是内核 CI 用 `gRT->GetVariable` 读 `dbx`，变量不存在时是空操作。
   **顺带堵掉一条相关但不同的**：Windows **只在 Secure Boot 开启时**才遵守 `EFI_MEMORY_ATTRIBUTES_TABLE` 的 NX 属性。SB=off ⇒ "我们的 RT 代码被 MAT 标成不可执行、跳进 `SetVirtualAddressMap` 时炸掉"这条自动不成立。屏上最后那行 `SecurityLock::LockType: SOFTWARE_LOCK` 来自 Mu 的 `SecurityLockAudit`，是 EBS 时锁架构协议的正常动作，不是原因。
8. **当前实验（`e1862270…`，已刷入）：把探针搬进真正的 runtime driver。** 新增 `Platforms/Xiaomi/yunluoPkg/Drivers/ProbeRtDxe/`（`MODULE_TYPE = DXE_RUNTIME_DRIVER`，`[Depex] gEfiRuntimeArchProtocolGuid AND gEfiGraphicsOutputProtocolGuid`）：入口用 `LocateProtocol(gEfiGraphicsOutputProtocolGuid)` 取 `Mode->FrameBufferBase/PixelsPerScanLine/分辨率` 存进 static（runtime data），然后注册同样的两个事件，蓝=EBS、绿=VAC。它的 notify 函数体在 `EfiRuntimeServicesCode`，Windows **必须**在调 VAC 期间恒等映射，所以读数不再有歧义：停在蓝 ⇒ VAC 真的没被调用；变绿 ⇒ 挂在内核里更后面。`SimpleFbDxe` 里的旧探针（含 `[Guids]`）已全部撤掉。
   **下一步（按停在蓝的分支）**：查 winload 在 EBS 之后、VAC 之前还会碰什么——它必须先 `GetMemoryMap` 的结果建自己的页表，然后才会调 VAC。重点：①静态 34 区内存映射里 `EfiRuntimeServicesCode/Data` 是否都带了 `EFI_MEMORY_RUNTIME` 位；②`VariableRuntimeDxe` 是纯 RAM 模拟，winload/内核早期会 `GetVariable`（`BootInformation`/`ErrCode`）；③`EmbeddedMonotonicCounter`/`RealTimeClockRuntimeDxe` 都是 `DXE_RUNTIME_DRIVER`（已核对，这条没问题），但 `WatchdogTimerDxe` 是 `DXE_DRIVER` ⇒ `gRT` 里没有它，无碍。
9. **颜色阶梯（`fb2d7b05…`，当前实验）—— 一次定位交接的哪一步。** `e1862270…` 实机仍是**纯蓝、控制台文本不变**，而且这次 RuntimeDxe 的 `REPORT_STATUS_CODE` 已换成 `BaseReportStatusCodeLibNull`（已核对 `InternalGetReportStatusCode` 从 RuntimeDxe.map 里消失），所以"没变绿"要么真是 Windows 没调 VAC，要么是我们的 VAC 事件处理器排在本该执行的处理器之后、被前面某个驱动的死循环挡住。**不再猜**：直接把探针打进 DXE core 和 RuntimeDxe 内部（两份 `YunluoProbePaint` 副本，原文件备份在 `archive/probe-before-2026-10-24/`）：

   | 颜色 | 值 | 抛出点 |
   |---|---|---|
   | 蓝 | `0x000000FF` | `ProbeRtDxe` 的 EBS 处理器（事件表进行中） |
   | 橙 | `0x00FF8000` | `CoreExitBootServices`：整个 EBS 事件列表已返回（`DxeMain.c`） |
   | 白 | `0x00FFFFFF` | `CoreExitBootServices` 末尾，`gRuntime->AtRuntime=TRUE` 之后、即将交回 winload（`DxeMain.c`） |
   | 绿 | `0x0000FF00` | `RuntimeDriverSetVirtualAddressMap` 第一行（`Runtime.c`） |
   | 品红 | `0x00FF00FF` | VAC 的两道校验都过了，即将进事件循环（`Runtime.c`） |
   | 青 | `0x0000FFFF` | VAC 全部做完，即将 `return EFI_SUCCESS`（`Runtime.c`） |

   **最后一色即最后越过的门槛**：只有蓝 ⇒ 挂在某个排在我们之后的 EBS 处理器里；橙 ⇒ 挂在 EBS 事件表之后的 DXE core 收尾（`REPORT_STATUS_CODE`/`MemoryProtectionExitBootServicesCallback`/关中断）；白 ⇒ winload 拿到控制权但没调 VAC ⇒ 挂在 Windows 自己用内存映射建页表那一段；绿 ⇒ 进了 VAC 但被 `AtRuntime`/`VirtualMode`/描述符版本校验挡回错误；品红 ⇒ 挂在 VAC 事件循环或镜像重定位里；青 ⇒ 虚拟地址切换成功，挂在内核更后面。
   **同一版还带内存映射 X 光**：`HandoffQuiesceDxe` 在 before-EBS 里 `GetMemoryMap` 并打 `XMAP:` 行（描述符数/大小/key/最高地址、RT/MMIO/Reserved 计数、缺 `EFI_MEMORY_RUNTIME` 位的 RT 区、前 14 条 RT 区明细、配置表数量与 RSDP 地址）。**所有上色只涂屏幕上半 60%（前 1200 行）**，好让最底下十来行 `XMAP:` 文本在同一张照片里读得到。
10. **阶梯实机读数 = 白（`fb2d7b05…`，2026-10-24 20:0x）。** 屏幕上半是白色、控制台文本仍是 capsule 锁那两行 ⇒ **`ExitBootServices` 完整跑完并把控制权交回 winload，但 Windows 从头到尾没有调用 `SetVirtualAddressMap`，也没有调用过任何一个 runtime service**。挂死点完全在 Windows 自己的代码里（winload 交接 + 内核 Phase0 早段），固件侧只剩"改交给它的输入"这一条路。
   **同次 `XMAP:` 实测**（逐条抄录）：
   ```text
   XMAP: rt0 t=5 s=0x46EC3000 p=0x50  a=0x800000000000010F
   XMAP: rt1 t=5 s=0x47020000 p=0x50  a=0x800000000000010F
   XMAP: rt2 t=5 s=0x470A2000 p=0x1A0 a=0x800000000000010F
   XMAP: rt3 t=5 s=0x47323000 p=0xD0  a=0x800000000000010F
   XMAP: rt4 t=5 s=0x4761B000 p=0x96  a=0x800000000000010F
   XMAP: rt5 t=6 s=0x4761B000 p=0x12C a=0x800000000000010F
   XMAP: n=37 dsz=48 ver=1 key=0xE48 top=0x240000000
   XMAP: rt=6 miss=0 mmio=0 rsrv=14 zero=0
   XMAP: tables=8 rsdp=0x47248018
   ```
   健康项：6 个 RT 区全部带 `EFI_MEMORY_RUNTIME`（`miss=0`）、无零长度描述符、`top` 正好是 9 GiB 边界。**一条异常：`mmio=0`** —— `Platforms/Xiaomi/yunluoPkg/Library/MemoryMapLib/MemoryMapLib.c` 里那 16 个 `AddDev/MMAP_IO/MmIO` 寄存器区（GICD/GICR/UFS/OVL…）**一个都没进 EFI 内存映射**，0x0–0x40000000 整段设备空间在交给 winload 的表里是空的。写法与能跑 Windows 的参照平台逐字段相同（对比 `citrusPkg`/`fogPkg` 的同名文件），所以是**我们这条链路上被丢掉了**，原因未查；Windows 靠 ACPI 自己映射设备，所以先不动它。
11. **当前实验（`8559575c…`）：DSDT 只报一个"在场"的 CPU。** 读 `DSDT.aml` 发现一条具体矛盾：**8 个 `ACPI0007` 处理器对象（`CPU0`…`CPU7`，`_UID` 0–7）的 `_STA` 全部 `Return(0x0F)`**（present+enabled+functional+doing I/O），而 MADT 只有 CPU0 的 GICC `flags=1`、其余 7 条的 MPIDR 还是照抄 MT6768 的 `0x100…0x700`（这些亲和号在 MT6789 上不存在；而且我们把 CPU0 那条从 `0x0` 改成 `0x10000` 之后，表里再没有任何一条是 `0x0`）。HAL 在 Phase0 枚举处理器时看到的是"8 个在线"，只有 1 个能对上真实 GICC/MPIDR —— 正好是"以前抛异常、现在静默死循环"的形状。**之前那次"单核 MADT"实验没动 `_STA`，所以这是一条全新单变量。** 做法：把 `CPU1…CPU7` 的 `_STA` 返回值 `0x0F`→`0x00`（7 个单字节，偏移 `0x76/0x9D/0xC4/0xEB/0x112/0x139/0x160`，长度不变），重算 checksum=`0x85`（全字节和 `&0xFF==0`）；原件备份 `archive/probe-before-2026-10-24/DSDT.aml.orig`；已在 `Build/.../Ffs/*AcpiTables/*SEC2.3.raw`（RAW 段前 4 字节是长度前缀）里复核过落进 FV 的值确实是 `0F 00 00 00 00 00 00 00`。阶梯探针原样保留，所以这一版仍然能读出走到第几级。
12. **DSDT 单核 `_STA` 实验：无变化（`8559575c…`，实机照片与 `fb2d7b05…` 逐条相同）。** 上半屏仍是白，`XMAP:` 九行一字不差，控制台文本仍是 capsule 锁那两行 ⇒ **“8 个 ACPI0007 全报 present” 不是挂死原因**。改动保留（MADT 只使能 CPU0，DSDT 就该只报 1 个在场，更自洽），但这条假设关闭。
13. **当前实验（`b2ba1f77…`，已刷入）：把异常向量表搬进 runtime 代码，让 Windows 早期那次异常自己把寄存器打在屏幕上。** 推理：`VBAR_EL1` 由 `Silicon/Silicium/SiliciumPkg/Sec/Sec.c:184` 与 `CpuExceptionHandlerLib` 设置，指向的是**链接进 CpuDxe（`DXE_DRIVER`）镜像里的 `.text.ExceptionHandlersStart` 节**，即 `EfiBootServicesCode`；内存映射里那 64 KiB 的 `"CPU Vectors"`（`0x40C40000`）也是 `BsCode`（`MemoryMapLib.c:43`）。winload 换上自己的页表之后这段就不再被映射，**于是 Windows 在自己装 VBAR 之前发生的任何异常都会跳进一张没映射的向量表 → 立刻再次异常 → 静默死循环**，既不会 BSOD 也不会再调固件——和阶梯读数“停在白、之后什么都没有”完全吻合。做法：给 `ProbeRtDxe`（`DXE_RUNTIME_DRIVER`）链上 `CpuExceptionHandlerLib`，在 EBS 事件里先刷蓝、再调 `InitializeCpuExceptionHandlers (NULL)`，把 VBAR 指向**本驱动镜像里**那张表（map 确认 `.text.ExceptionHandlersStart` 在 RVA `0x13800`、长 `0x800`、对齐 2048 ✓）；Windows 必须 1:1 映射 `EfiRuntimeServicesCode`，所以处理器活着，且 DEBUG 版 `DebugLib = BaseDebugLibSerialPort` + `SerialPortLib = FrameBufferSerialPortLib`，寄存器 dump 会直接写进 framebuffer。为此放宽了两个 basecore 库的模块类型白名单（原件在 `archive/probe-before-2026-10-24/`）：`DxeCpuExceptionHandlerLib.inf` 的 `LIBRARY_CLASS` 加 `DXE_RUNTIME_DRIVER`，`DxeMemoryProtectionHobLib.inf` 同理（异常库会连带拉进它）。
   **读数**：开机文本里应出现 `PROBE: vectors Success -> 0x...`（证明 VBAR 真的换了）；之后若屏上出现 EDK2 的 `CPU Exception` 寄存器表 ⇒ 直接拿到 ESR/ELR/FAR，根因即定；若仍“停在白、无 dump” ⇒ 不是异常，而是 Windows 自己真的在死循环，改攻交给它的输入（下一个单变量：补 `mmio=0`）。
14. **runtime 向量实验：VBAR 换成功了，但屏上没有 dump ⇒ EBS 之后没有异常（`b2ba1f77…`，2026-10-24 20:19）。** 照片里 `PROBE: vectors Success -> 0x46ED3800`，该地址正落在 `rt0 s=0x46EBE000 t=5`（`EfiRuntimeServicesCode`）里 ⇒ 向量表确实搬进了 Windows 必须映射的代码；上半屏仍是白、仍无 `CPU Exception` 表。又因为**改 MPIDR 之前 Windows 能画整屏 BSOD**（证明它 EBS 后确实映射并写入这块 framebuffer），可以定死：**`ExitBootServices` 之后没有任何异常发生，Windows 是在无故障地空转**，不是在跳进死向量表。顺带：这一版的 `XMAP: n=88 key=0xE35`、RT 区起点整体位移（`rt0 s=0x46EBE000`），说明描述符数会随固件自身分配变化，`mmio=0` 依旧。
15. **当前实验（`cfa20128…`，正在刷入）：MADT 裁到只剩一条 GICC。** 新形状：**`HAL` 按 MADT 的 GICC 列表建处理器表并等第二核心上线**。表里原本 8 条 GICC，后 7 条的 MPIDR 是照抄 MT6768 的 `0x100…0x700` —— 这些亲和号在 MT6789 上不存在，PSCI `CPU_ON` 永远等不到人，BSP 就死等。之前那次"单核 MADT"是在 CPU0 的 MPIDR 还错着的时候做的（那时更早就死了），所以**裁到只剩一条 GICC** 仍是全新单变量。做法：`APIC.aml` 由 `44 + 8×0x52 + GICD(0x18) + GICR(0x10)` = 740 字节裁成 `44 + 1×0x52 + 0x18 + 0x10` = **166** 字节，改 `Length`（偏移 4）与 checksum（偏移 9 → `0x28`，全字节和 `&0xFF==0`），保留的那条 GICC 仍是 `flags=1 @+0x0C`、`MPIDR=0x10000 @+0x44`；8 条版备份 `archive/probe-before-2026-10-24/APIC.aml.8cpu`；已在 `Build/.../Ffs/*AcpiTables/*SEC2.1.raw` 里复核过 FV 内确实是 3 条记录（GICC/GICD/GICR）。DSDT 那边 `CPU1…CPU7` 的 `_STA` 已经是 0，两边一致。
   **注意这张 GICC 的字段布局比 ACPI 6.x 规范短 2 字节**（`flags` 在 `+0x0C`、`MPIDR` 在 `+0x44`、记录长 `0x52`、`+0x50` 处还有个 u16）——它是从真实能跑 Windows 的 MT6768 设备上 dump 下来的，所以按它抄就对了，别拿规范偏移去"修正"。
16. **单条 GICC 的 MADT：无变化（`cfa20128…`，2026-10-24 20:3x）。** 照片与上一版一字不差（上半屏白、`PROBE: vectors Success -> 0x46ED3800`、capsule 锁那两行、`XMAP:` 只有地址随固件大小平移）。**连同 7.4.12 的 DSDT `_STA`，两次把"在场 CPU 数"从 8 改成 1（MADT 与 DSDT 两边都改）没有产生任何可观察差异** ⇒ 推论：**Windows 挂死时还没走到消费处理器表的那一步**，"HAL 等第二核心"这条关闭。
17. **当前实验（`4a2558e9…`，已刷入，等实机读数）：彻底关掉 Kernel Errata Patcher。** 上两条 ACPI 实验都毫无反应，把嫌疑推向"更早"——winload 自己的交接段。而 `Silicon/Silicium/SiliciumPkg/Library/KernelErrataPatcherLib/KernelErrataPatcherLib.c` 是**我们对 Windows 镜像做的最后一次内存改动**：它在 ReadyToBoot 钩住 `gBS->AllocatePages` 与 `gBS->ExitBootServices`（并重算 `gBS->Hdr.CRC32`），靠 `KepParseAllocatedMemory` 猜出 winload 的 `.text`，然后 `SetWinloadProtection(FALSE)` → `ApplyPlatformErrataPatches` → `SetWinloadProtection(TRUE)`，即**通过 Memory Attributes 协议把 winload 代码段的页属性翻两趟**。MT6789 链的是 `ErrataPatchesLibNull`，`GetPlatformShellCode` 返回 NULL，所以 EBS 那次确实是空转，但**页保护那两趟照翻**。之前把它判"已排除"的理由只有"不注入 shell code"，没覆盖这条；更关键的是**改 MPIDR 之前 Windows 在更早就异常死了，那个早死把所有更后面的问题全遮住了**——KEP 完全可以一直是坏的，只是以前没机会表现。做法：给 `KernelErrataPatcherLibConstructor` 开头加 `return EFI_SUCCESS;`（原件备份 `archive/probe-before-2026-10-24/KernelErrataPatcherLib.c.orig`），链接器 dead-strip 后已在 `Build/.../Ffs/*BdsDxe/*SEC2.1.pe32` 里复核 `strings | grep -c KEP` = **0**。**读数**：开机文本里 `[KEP] Winload .text Memory Range = ...` 那行应当消失（证明生效）；若上半屏越过白变绿/品红/青 ⇒ 就是它；仍停在白 ⇒ 下一个单变量补 `mmio=0`。
18. **KEP 判死，但这次有生效凭据（`4a2558e9…`，2026-10-24 20:5x）。** 照片里 `[KEP] Winload .text Memory Range = ...` 那行**确实消失了**（UFS 行之后直接是 `XMAP:`），而 `PROBE: vectors Success -> 0x46ED3800` 还在、上半屏仍停在白 ⇒ 关闭生效了，但没改变挂死。另外这次照片也再次确认 `mmio=0`、`n=98`、`rsdp=0x4723F018`。
19. **修正一条我推过头的结论。** 7.4.12/7.4.16 那两个"改 CPU 数毫无反应"**并不能**证明"Windows 还没消费处理器表"：删掉的全是**禁用**的 GICC 和**次级**处理器的 `_STA`，一个只看使能项的 HAL 本来就不会有反应。真正硬的约束是 7.4.14：**装了 runtime 向量表之后仍然没有任何异常 dump ⇒ 这不是缺映射的取指/数据异常。**
20. **由此换成的新模型：总线挂死（bus hang）。** ARM 上访问一个时钟未使能的外设，互连永远不返回，CPU 就地停住——**不产生异常、不调用固件、不 BSOD**，与全部观测吻合；而 MTK 恰好以"外设时钟没开就访问会挂总线"出名。在这个模型下 **DBG2 从猜测升级成头号嫌疑**：它宣告 `0x11002000` 上一个 16550 类调试端口（`/home/zhaoqing412/pt/kernel/yunluo/baselines/kernel_xiaomi-yunluo/arch/arm64/boot/dts/mediatek/mt6789.dts:3423` 确认 MT6789 的 `uart0: serial@11002000` 就在那儿，地址没错），但 ① 它的 `compatible` 是 `mediatek,mt6577-uart`，**不是 16550**，子类型是照抄 MT6768 的；② 端口设备路径写的是 `\_SB_.UAR2`，**我们的 DSDT 只有 `CPU0…CPU7`，没有 `UAR2`**；③ 我们的 UEFI 控制台走 framebuffer，**从头到尾没使能过 UART 时钟**。而 `KdInitSystem` 在 Phase0 很早期就会去**读** 16550 寄存器探测调试器。
21. **当前实验（`cb2fb3bb…`，正在刷入）：不发布 DBG2。** `AcpiTables.inf` 的 `[Binaries]` 里把 `ASL|DBG2.aml` 注释掉（文件保留在目录里）。**生效凭据**：`XMAP: tables=` 应从 8 变成 7；也已在 `Build/.../Ffs/*AcpiTables/*SEC2.*.raw` 里复核过 FV 内表集只剩 `APIC DSDT FACP GTDT SSDT SSDT`，`DBG2` 不在其中。若上半屏越过白 ⇒ 就是 UART 挂总线，之后要么在 UEFI 里开 UART 时钟+修好 DBG2，要么继续不发布；若仍停在白 ⇒ 下一个单变量按挂总线模型继续排（`mmio=0` 与 FACP/GTDT 里指向设备区的地址）。
22. **DBG2 判死（`cb2fb3bb…`，2026-10-24 21:0x）。** 照片上半屏仍停在白，其余一字不差 ⇒ UART/DBG2 不是挂死点，保持不发布。
23. **`mmio=0` 的成因查清，且它不是我们的表写错。** 链路：`MemoryInitPei.c:92` 对 `AddDev` 调 `BuildResourceDescriptorHob(MMAP_IO,…)` ⇒ `Gcd.c:2691` 把 `EFI_RESOURCE_MEMORY_MAPPED_IO` 变成 `EfiGcdMemoryTypeMemoryMappedIo` 的**memory space**（只 `CoreInternalAddMemorySpace`，不建描述符）；而 `CoreGetMemoryMap` 只对**已分配的 GCD map 条目**产出描述符，且 `Page.c:2163-2164` 要求 MMIO 条目必须带 `EFI_MEMORY_RUNTIME` 才写进表。`CorePreAllocateMemory` 只给 SystemMemory/HOB/FV 建描述符，没有任何人给设备区建。⇒ **这是 Mu-Silicium core 的通用行为，全平台一致**（`sweet_k6a/lime/nabu/venus…` 的 `MemoryMapLib.c` 逐字节同构），不是 yunluo 的移植缺陷。另外 Mu 的内存保护在这条链上是**全关**的：没有 memory-protection HOB ⇒ `DxeMemoryProtectionHobLibConstructor` 走失败分支、`gDxeMps` 保持全 0 ⇒ 无 NX / 无 image protection / 无 heap guard，`MemoryProtectionExitBootServicesCallback()` 因 `gDxeMps.ImageProtectionPolicy.Data==0` 直接空转 —— "EBS 时把 BS 代码设成 NX 打断 winload 取指"这条也判死。
24. **当前实验（`691ac1e7…`，正在刷入）：GICR 逐帧扫描，把"挂总线"搬到我们看得见、能打印的时刻。** 关键不对称：**改 MPIDR 之前 Windows 已经能进内核并画完整 BSOD** ⇒ EBS 之后的交接、页表、bootvid 写 framebuffer 这条路本来是通的；修对 MPIDR 之后反而静默，说明 HAL 在 Phase0 **匹配上自己的 GICC 之后走得更深了**，死在第一次真正碰设备的地方。而 `ArmGicV3Dxe.c:87-102` 的 `GicGetCpuRedistributorBase()` 是 do-while：** affinity 对上就 return，从不读匹配帧之后的那些帧**——也就是说 UEFI 全程只证明过 `0x0C040000` 起、到本核那一帧为止是可读的；操作系统的 GIC 初始化通常要按 MADT GICR discovery range（`0x0C040000` + **2 MiB** = 16 帧 ×128 KiB）枚举全部帧、还要读/写每帧的 `GICR_WAKER` 去唤醒重分配器。**A55 簇此刻是断电的，它的重分配器帧若落在电源域里，读它就是不返回（挂互连），既不异常也不打印**——与"停在白、之后什么都没有"完全同形，而且解释了为什么裁 MADT 到一条 GICC 毫无反应：那 16 帧的 discovery range 我一行都没动。做法：`HandoffQuiesceDxe` 在 before-EBS 里 `QuiesceGic()`+`XrayMemoryMap()` 之后跑 `XrayGicrScan()`，打一行 `GICR: dtyper=… mpidr=…`，然后每帧一行 `GICR: fN @0xADDR t=<TYPER 64bit hex> w=<WAKER 32bit hex>`，最后一行 `GICR: scan done N frames`。
   **读数规则**：某 `fN` 那行**消失** ⇒ 挂死就在 `上一行地址 + 0x20000` 那一帧（开机直接冻在半屏，需要长按断电）——根因即定，随后把 MADT 的 GICR discovery range 长度缩到第一具死帧之前；16 行全出且 `scan done` ⇒ 所有帧可读，本条判死，按 TYPER 的 `LAST(bit43)/VLPIS(bit40)/ProcessorAffinity` 反推真实的帧布局，再决定 GICC 的 `Redistributor base(@+0x3C)` 要不要填真值（现在全是 0）。
25. **GICR 扫描实机读数（`691ac1e7…`，2026-10-24 21:4x）：16 帧全部可读，没有挂总线，阶梯仍停在白 ⇒ 7.4.24 这条判死。** 逐字抄录（`GICR_TYPER=0x08`、`GICR_WAKER=0x14`，与厂商内核 `include/linux/irqchip/arm-gic-v3.h` 一致）：
   ```text
   GICR: dtyper=00790413 mpidr=0000000081000000
   GICR: f0  @0x0C040000 t=0000000000000020 w=00000000
   GICR: f1  @0x0C060000 t=0000010000000120 w=00000000
   GICR: f2  @0x0C080000 t=0000020000000220 w=00000000
   GICR: f3  @0x0C0A0000 t=0000030000000320 w=00000000
   GICR: f4  @0x0C0C0000 t=0000040000000420 w=00000000
   GICR: f5  @0x0C0E0000 t=0000050000000520 w=00000000
   GICR: f6  @0x0C100000 t=0000060000000620 w=00000000
   GICR: f7  @0x0C120000 t=0000070000000730 w=00000000
   GICR: f8  @0x0C140000 t=000000000201743B w=00000000
   GICR: f9  @0x0C160000 t=0000000000000000 w=00000000     (f10…f15 同样全 0)
   ```
   读法：帧 `fN`（N=0…7）真实存在，`ProcessorNumber(bits[23:8]) = N`，`bits[47:40] = N`，`f7` 比别的多一个 `bit4`；**`f8` 起是空洞**（读到的是总线残值 `0x201743B`，再往后恒 0）⇒ 真实 GICR 只有 8 帧 = 1 MiB，而 MADT 的 discovery range 写的是 2 MiB（与厂商 DTS `reg = <0 0x0c040000 0 0x200000>` 一致，所以照抄不算错）。**两个可疑点记下来但都不再当假设用**：① **没有任何一帧置 `LAST(bit43)`**，靠"扫到 LAST 为止"的 OS 驱动会走出范围；② 所有帧 `bits[39:32]`（架构上的 Processor Affinity）= **0**。`WAKER` 全 0 ⇒ 八帧全醒、`ProcessorSleep=0`，读它们绝对安全。
26. **决定性更正：`mpidr=0000000081000000` 解码出来亲和号是 0，不是 `0x10000`。** `MPIDR_EL1` 的位域是 `[7:0]Aff0 / [15:8]Aff1 / [23:16]Aff2 / [24]U / [31]MT / [39:32]Aff3`，`0x81000000` = **bit31 MT=1 + bit24 U=1，四个 Aff 全 0**（U=1 时 Aff1/Aff2 架构上就是 RAZ）。厂商 DTS 独立佐证：`mt6789.dts` 的 `cpus` 节点 `cpu0 reg=<0x0000>`、`cpu1 reg=<0x0100>` … `cpu7 reg=<0x0700>` —— **正是我们从 MT6768 抄来的那组值，它本来就是对的**。7.4.5 里"硬件是 Aff2=1 ⇒ 该写 0x10000"是我把 `0x81000000` 读成了 `0x1<<16`，整条"MPIDR 修复"建立在一次误读上。
27. **由 26 反推出整晚的因果是倒的，当前实验（`a10a3e24…`，正在刷入）：MPIDR 改回 `0x0`。** 硬事实对比：**MPIDR=`0x0` 时 Windows 进到了内核并画出整屏 BSOD**（能画 ⇒ EBS 后页表、bootvid、framebuffer 写这条路全通）；**MPIDR=`0x10000` 时 EBS 之后什么都不发生**。所以 `0x10000` 不是"修好了 0x2B"，而是让 Windows 在更早的地方（HAL 找不到代表自己的那条 GICC）就不动了 —— 后面 7 个单变量（DSDT `_STA`、runtime 向量、单条 GICC、KEP、DBG2、GICR 扫描）全部读数一致"停在白"，很可能都只是在**同一处死水里换输入**。做法：`APIC.aml` 第一条 GICC 的 `MPIDR@+0x44` 由 `0x10000` 改回 `0x0`，checksum → `0x29`（全字节和 `&0xFF==0`），表长仍 166 字节、仍只有一条 GICC、`flags` 仍 `0x1`；已在 `Build/yunluoPkg/DEBUG_CLANGPDB/FV/Ffs/*AcpiTables/*SEC2.1.raw` 里复核 `flags=0x1 MPIDR=0x0`。
   **读数规则**：BSOD `0x2B` 回来 ⇒ 假设成立，回到"能看见错误"的状态，下一步用 runtime 向量表 + BSOD 四个参数攻真正的异常（这次是带着能用的观测手段去打）；若仍停在白 ⇒ 说明 `0x10000` 那次"见效"另有原因，得回头查同批改动（checksum、或 GICC 记录数）。
28. **顺带记两条本机观测。** ① 屏上在"加载 winload"前后有两条我们自己 DXE core 的 `ConvertPages: failed to find range 10000000 - 102C9FFF` / `1020000 - 102FFF` —— 就是 7.4.23 里 `mmio=0` 的下游症状：Mu 的内存保护想给设备区打属性，但那些 GCD MMIO 区没有描述符，转换找不到区间。② `XMAP: tables=8` 在删掉 DBG2 之后**仍然是 8**（配置表条目是 RSDP/XSDT/SMBIOS 这类，不是 XSDT 成员数），所以它不能当"DBG2 已删"的凭据；真正的凭据是 FFS 里 `*AcpiTables/*SEC2.*.raw` 只剩 6 张表（`APIC DSDT FACP GTDT SSDT SSDT`），本版复核过仍是 6 张。

29. **中断 / timer / clock 三条实机判活（`013bafa1…`，2026-10-25 09:2x 照片，逐字抄录）**：
   ```text
   AUDIT: dxe tick polls=30/30 IRQ=ALIVE period=100000
   AUDIT: GICD ctl=00000012 typ=00790413 grp=00000000,00000000 en=00000000
   AUDIT: GICR typ=0000000000000020 wak=00000000 grp=00000000 en=00000000
   AUDIT: ICC sre=00000007 pmr=F0 igrpen1=00000001 mpidr=0000000081000000
   AUDIT: sgi hits=1 DELIVERED
   AUDIT: CNTFRQ=13000000 Hz count 119580140 -> 120880140 (d=1300000)
   AUDIT: CNTP_CTL=00000001 CVAL=00000000073BBAD0
   AUDIT: clocks total=226 enabled=222 readfail=0
   ```
   判读：**中断**——DXE tick 只在定时器 ISR 里推进，30/30 次轮询全中 ⇒ 定时器中断确实送达过 CPU；自发 `ICC_SGI1R_EL1` 的 SGI 1 也被我们自己的 handler 接住（`hits=1`）⇒ GIC→CPU 端到端可用；`ICC_SRE=7 / PMR=0xF0 / IGRPEN1=1` 说明系统寄存器接口与 Group1 都开着。**timer**——`CNTFRQ=13 MHz` 与 `mt6789.dts` 的 `clock-frequency = <13000000>` 一致，100 ms 精确走 `1,300,000` tick，`CNTP_CTL=1`（ENABLE，IMASK=0）且 CVAL 已装弹。**clock**——226 个时钟可枚举、222 个已开、0 个读失败（`Invalid clock id 226 is provided` 是探测循环终点的正常输出）。⇒ **"这三条还没移植/没移植完全"判死**，问题不在它们身上。
   **同时修正我自己的两次位域误读**：`GICD_CTLR` 的 `DS` 是 **bit6**、`ARE_NS` 是 **bit4**（厂商内核 `include/linux/irqchip/arm-gic-v3.h`），所以 `ctl=0x12` 是 `ARE_NS + EnableGrp1NS`、`DS=0` —— 标准 Non-secure 配置，**不是**我中途怀疑的"固件跑在 Secure 态"。`GICD_TYPER=0x00790413`：SecurityExtn(bit10)=1、SPI 数 (0x13+1)×32=640。
30. **审计抓出的真 bug：`HandoffQuiesceDxe` 的 redistributor 屏蔽从来没生效。** SGI/PPI 寄存器不在 `RD_base`，而是在其后 64 KiB 的 **`SGI_base` 帧**（EDK2 自己也是 `mGicRedistributorBase + ARM_GICR_CTLR_FRAME_SIZE + off`，`ArmGicV3Dxe.c:208`）。我们的代码写的是 `Gicr + 0x180/0x280/0x380`，落在 `RD_base` 的保留区 ⇒ **三行全是空操作**。这直接推翻了 7.4.3 里"before-EBS 清空全部 GIC 线"那次实验的"无变化"结论——**它从未真正执行过**。已改为 `Gicr + 0x10000 + off`（`HandoffQuiesceDxe.c:24-27, 72-74`）；同一处偏移错误也让 7.4.29 里 `GICR grp=/en=` 两个读数无效（读的是保留区，恒 0），已一并修正。**教训**：凡 GICR 的 SGI/PPI 寄存器（IGROUPR0/ISENABLER0/ICENABLER0/IPRIORITYR/ICFGR）都要加 0x10000；`GICR_TYPER/WAKER/CTLR` 才在 `RD_base`。
31. **基线闭环并已换镜像**：`013bafa1…` 实机**蓝屏回来、stop code 与 MPIDR 改动前完全相同（`0x2B`）** ⇒ "MPIDR=0x10000 修好了 0x2B"确系误读造成的倒退，7.4 开头的反汇编证据重新可用。当前镜像 `affe973d…`（已 `flash rc=0` / `reboot rc=0`）= 蓝屏基线 + 审计偏移修正 + **真正生效的 quiesce**，一次开机同时读"`GICR grp=/en=` 真值"和"真清了 PPI 之后 `0x2B` 是否变化"。

30b. **`GICR_ISENABLER0 = 0x6C000000` ⇒ 交接到 OS 那一刻，四个架构定时器 PPI 全是开着的（`affe973d…` 实机，2026-10-25 09:5x）。** `0x6C000000` = INTID `26 / 27 / 29 / 30`，正好是 GTDT 点名、`ArmPkg/Drivers/TimerDxe/TimerDxe.c:401-416` 逐条 `RegisterInterruptSource` 的那四条（hyp / virtual / secure-phys / non-secure-phys）。**这也把我此前的一个错误判断改回来**：早先那次屏上读到 `sgi=0x6C000000` 被我当成"读错地址"而废弃，其实它就是正确偏移下的 `GICR_ISENABLER0`（7.4.30 的偏移 bug 让当时的复核读数恒 0，才误判为"GIC 已经静默"）。所以"交接时定时器中断线仍 armed"这条**从来没有被证伪**，只是被一次空操作实验和一次错偏移读数掩盖了。当前 `affe973d…` 的 quiesce 已真正生效，正好是这条假设的第一次有效检验。

---
---



## 8. Shell 与屏上诊断（DEBUG）

- **DEBUG 版 Shell 不清屏**（`Shell.c` 里 `#ifndef MDEPKG_NDEBUG` 跳过 `ClearScreen`），启动日志和 `blk` 列表可直接拍照；RELEASE 保持正常清屏。
- 屏上控制台走 `FrameBufferSerialPortLib`（无物理串口），**屏幕只装得下约 25 行**，所以每个事件只打一行、并且把恢复周期的 `UFS MTK:` 噪声用 `mMtkUfsQuiet` 静音。
- `UfsExecScsiCmds` 的统一诊断行：只在**失败**或**传输 ≥64 KiB** 时打印
  `UFS op=… lun=… lba=… len=… io|ua|retry t=<GetPerformanceCounter> d=<ticks> -> <status>`。
  `SCSI_DISK_TIMEOUT` 打成 `300000000`（100 ns 单位 = 30 s）。
- UniPro 属性用 **`#`/`.` 位条**打印（照片里十六进制小字根本读不出来，位条是唯一读得清的形式）。
- 归一化照片读数：`magick -rotate 180 -deskew 40% -normalize -contrast-stretch 1%x1% -resize 1600x`。
- **Shell 无输入通道**（无键盘、无串口输入），所有验证只能靠开机自动打印。

### 8.1 离线改 WinPE 的 BCD（免 root）

`wimlib` 在本机没有打包（`libwim-dev` 无候选版本），所以 **`boot.wim` 只能读不能可靠回写**（`7z` 可解 LZX:15）。但 BCD 是注册表 hive，位于 FAT 镜像内，可以离线改：

```bash
mkdir -p /tmp/hivexdl && cd /tmp/hivexdl
apt-get download hivex                 # 不需要 sudo
ar x hivex_*_amd64.deb                 # 得到 data.tar.zst
tar -C root --zstd -xf data.tar.zst
export PATH=/tmp/hivexdl/root/usr/bin:$PATH
export LD_LIBRARY_PATH=/tmp/hivexdl/root/usr/lib
export PERL5LIB=/tmp/hivexdl/root/usr/lib/perl5/site_perl
hivexregedit --export ./bcd '\Objects' > /tmp/bcd.reg
hivexregedit --merge  ./bcd /tmp/patch.reg
```

坑：`hivexget` 是要调 `hivexsh` 的脚本，必须把 `bin` 加进 `PATH`；`hivexsh -f 脚本` 里路径**不能加引号**（它不去引号），也没有 `-c`/`dir`，列子键用 `ls`。整数元素（`0x25xxxxxx`）值是 **8 字节小端**，写成 `"Element"=hex(3):01,00,00,00,00,00,00,00`。

把改好的文件塞回原始 FAT 镜像（无分区表，`mkfs.fat`，卷标 `CUST`）：

```bash
mcopy -o -i Build/winpe/cust-winpe.img <本地文件> ::efi/microsoft/boot/bcd
mdir    -i Build/winpe/cust-winpe.img ::
```

已确认的元素 ID：`BcdOSLoaderInteger_SafeBoot = 0x25000080`；`BcdLibraryBoolean_DebuggerEnabled = 0x16000010`、`DebuggerType = 0x15000011`、`SerialDebuggerPort = 0x15000013`、`SerialDebuggerBaudRate = 0x15000014`（这个 ISO 的 `{fwbootmgr}` 已预置 serial/port=1/115200）。`bcdedit /set sos` 对应的 ID **没有可靠来源，别往引导关键文件里猜**。本镜像的启动对象是 `{7619dcc9-fafe-11d9-b411-000476eba25f}`，原始 BCD 备份在同目录 `bcd.orig`。

---

## 9. 待办

1. **Windows 起得来之后的收尾**（当前主线）：① **持久化 UEFI 变量** —— 现在是 `PcdEmuVariableNvModeEnable=TRUE`（纯 RAM 模拟，重启即丢），Windows 引导项/BCD 迟早需要真 NVRAM；② **真实 RTC** —— `RealTimeClockLibNull`，`GetTime`/`SetTime` 在但没有时基；③ **USB 输入** —— MT6789 只有 USB 2.0 MUSB（`usb@11200000`, `mt6789-usb20`），无 xHCI，UEFI 与 Windows 都没有 MTK MUSB host 驱动，只能设备模式 mass-storage gadget 或另想办法；④ **RELEASE 构建** —— DEBUG 已稳定，RELEASE 未验证，构建后确认诊断停顿与 Shell 不清屏的改动不会带入。
2. **摘掉调试脚手架**（0x2B 排查期的观测件，正式版不要）：`HandoffAuditDxe`（屏上 `AUDIT:` 五组行）、`HandoffQuiesceDxe`（before-EBS 静默 GIC/定时器）、各种 GICR/DTB 审计。摘的时候 INF/DSC/FDF 三处要同时改。
3. **多核**：MADT/DSDT 目前只报 1 核可用（8 核在场），要放开。
4. **触摸（已撤回，按需重启）**：Novatek NVT 在 **SPI0**（`0x1100A000`），GPIO30-33 func1（CLK/CSB/MO/MI）、IRQ GPIO8；固件下载只存在于 vendor `nt36xxx` 驱动，主线 `novatek-nvt-ts.c` 没有。计划文档在 `archive/touch-2026-11-06/TOUCH-WINDOWS-PLAN.md`。Windows 侧没有免费午餐：`hidspi.sys`（PNP0C51）需要 HIDSPI + SPB + GpioClx，MTK 都没有，且芯片是私有协议 ⇒ 只能薄 KMDF + VHF（而 WinPE 大概率缺 `vhf.sys`）。
5. **不要再回头排查**：ACPI 表集/中断号、安全启动与 MAT、EL 级别、armed 中断、`CNTFRQ_EL0`、pending SError、**以及 `0x2B` 本身（根因是内存表两个洞，见第 1 节）** —— 完整清单见第 2 节"不要再回头排查"。
6. 固件树无 git 元数据，重装/迁移前先备份 `mu-silicium`、`bootchain` 与 `Build/`；基线快照 `/home/zhaoqing412/yunluo-uefi-src-20261025.tar.zst`（10-25 10:47）是当前树的权威对照。

---

## 10. 维护约定

- 每次实机验证 → 更新第 1、6.2 节（保留关键日志原文）。
- 每次确认硬件事实 → 更新第 6.4 节并标注来源。
- 每次排除一个方向 → 写进第 2 节"不要再排查"，防止下个会话重复。
- 每次 build 成功 → 更新第 2 节的产物大小/镜像 sha256。
- 里程碑完成后 → 更新第 1、9 节，写清"为什么这么做"，并把过期历史整篇归档到 `archive/`（不要留在正文里）。
