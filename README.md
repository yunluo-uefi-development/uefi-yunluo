# yunluo UEFI — Xiaomi Redmi Pad (MT6789)

Project Silicium / Mu-Silicium firmware for the Redmi Pad (`yunluo`, MT6789 /
Helio G99). The final goal is running **Windows on ARM64**; UniXNU/DarwinBoot is
not used.

## Provenance

| Piece | Source |
|---|---|
| Baseline tree | `yunluo-uefi-src-20261025.tar.zst` (2026-10-25 10:47), SHA-256 `46de770e761f0c13198994125ec3232920fe9be31afa544e8165de1602a64eaf` |
| Merged fixes | [`yunluo-uefi-development/uefi-yunluo`](https://github.com/yunluo-uefi-development/uefi-yunluo) (itself derived from the same snapshot) |
| Upstream framework | Mu-Silicium v4 snapshot `0b68d714…`, paired Basecore `9e56daa9…` |

The snapshot has no git ancestry, so this repository starts from it honestly
rather than inventing history. `TRACKING.md` is the authoritative engineering
log: it records every hardware-confirmed fact, every exclusion, and the build
recipes.

## Layout

```
mu-silicium/Platforms/Xiaomi/yunluoPkg/    board package (drivers, ACPI, libs)
mu-silicium/Mu_Basecore/.../UfsPassThru.c  the one Mu file this platform patches
bootchain/BootShim/                        BootShim that copies the 7 MiB FV
bootchain/Tools/mkbootimg.py               boot v4 packaging
build-yunluo-mu.sh                         one-shot build + package
TRACKING.md                                engineering log (read this first)
UFS-PORTING.md                             UFS bring-up notes for other MTK devices
```

## Build

```bash
./build-yunluo-mu.sh DEBUG        # 8 GiB units (default) -> Build/yunluo-mu/boot-yunluo-mu.img
./build-yunluo-mu.sh DEBUG 6g     # 6 GiB units          -> Build/yunluo-mu/boot-yunluo-mu-6g.img
```

Requires clang/LLVM 22 at `/usr/lib/llvm-22/bin/` and an `aarch64-linux-gnu-`
toolchain for BootShim. Flash `boot_a` only — the device does **not** support
`fastboot boot`, and `vendor_boot` must stay byte-identical to stock.

## Hardware-verified state

Display 1200×2000 (panel mounted upside-down, so the OVL scanout is rotated
180°), UEFI Shell, UFS on HS-G1 at ~115 MB/s, Windows PE 22631 reaching its
desktop, graphical UEFI setup available from the boot menu. Only UFS LU2 is
exposed (LUN0/LUN1/RPMB are hidden).

Open items: persistent UEFI variables, real RTC, USB input, RELEASE build,
multi-core MADT/DSDT, and stripping the debug scaffolding
(`HandoffAuditDxe`, `HandoffQuiesceDxe`).
