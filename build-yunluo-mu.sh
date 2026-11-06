#!/usr/bin/env bash
#
# Build the yunluo (Redmi Pad, MT6789) Mu-Silicium firmware and package it
# with our verified boot chain:
#
#   LK -> boot.img kernel = gzip(BootShim + SILICIUM_UEFI.fd)
#      -> BootShim copies the 7 MiB FV to 0x50000000 and jumps
#
#   ./build-yunluo-mu.sh [DEBUG|RELEASE]
#
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
M="$ROOT/mu-silicium"
OURS="$ROOT/bootchain"
EDK2="$M/Mu_Basecore"
WORK="$ROOT/mu-work"
CONF="$WORK/edk2-conf"
LOGS="$WORK/logs"
OUT="$ROOT/Build/yunluo-mu"
TARGET="${1:-DEBUG}"

FD_BASE="${FD_BASE:-0x50000000}"
FD_SIZE="${FD_SIZE:-0x00700000}"
FD_BLOCKS=$((FD_SIZE / 0x1000))

mkdir -p "$CONF" "$LOGS" "$OUT"

[ -d "$M/Platforms/Xiaomi/yunluoPkg" ] || { echo "no yunluoPkg in $M" >&2; exit 1; }

PACKAGES="$M:$M/Platforms/Xiaomi:$M/Common/Mu:$M/Common/Mu_OEM_Sample:$EDK2:$M/Silicon/MediaTek:$M/Silicon/Silicium:$M/Silicium-ACPI:$M/Silicium-ACPI/Platforms/Xiaomi:$M/Silicium-ACPI/Silicon/MediaTek"

export WORKSPACE="$M"
export EDK_TOOLS_PATH="$EDK2/BaseTools"
export CONF_PATH="$CONF"
export PACKAGES_PATH="$PACKAGES"
export PYTHON_COMMAND=python3
export PATH="$EDK_TOOLS_PATH/BinWrappers/PosixLike:$EDK_TOOLS_PATH/Source/C/bin:$PATH"
export CLANG_BIN="/usr/lib/llvm-22/bin/"
export CLANG_HOST_BIN=""
export GCC_AARCH64_PREFIX="aarch64-linux-gnu-"

for f in target tools_def build_rule; do
  [ -f "$CONF/$f.txt" ] || cp "$EDK_TOOLS_PATH/Conf/$f.template" "$CONF/$f.txt"
done

LOG="$LOGS/uefi-yunluo-mu-$(date +%Y%m%d-%H%M%S).log"
cd "$M" || exit 1
echo "== build yunluoPkg ($TARGET), log: $LOG"
build -p yunluoPkg/yunluo.dsc -a AARCH64 -b "$TARGET" -t CLANGPDB -n "$(nproc)" \
  -D FD_BASE="$FD_BASE" -D FD_SIZE="$FD_SIZE" -D FD_BLOCKS="$FD_BLOCKS" \
  -D DEVICE_MODEL=0 -D ENABLE_SECUREBOOT=0 -D MEMORY_PROTECTION=TRUE \
  -D SHIP_MODE=FALSE -D BUILDID_STRING=yunluo-mu \
  > "$LOG" 2>&1
rc=$?
echo "build rc=$rc"
if [ $rc -ne 0 ]; then
  grep -a -E "error:|: error|Error 0|错误" "$LOG" | head -40
  tail -20 "$LOG"
  exit $rc
fi

FD="$M/Build/yunluoPkg/${TARGET}_CLANGPDB/FV/SILICIUM_UEFI.fd"
[ -f "$FD" ] || { echo "FD MISSING: $FD" >&2; exit 1; }
echo "FD: $FD"
ls -l "$FD"

# Our verified BootShim: UEFI_BASE 0x50000000, UEFI_SIZE 0x700000.
make -C "$OURS/BootShim" clean >/dev/null 2>&1 || true
make -C "$OURS/BootShim" UEFI_BASE=0x50000000 UEFI_SIZE=0x700000 >/dev/null || exit 1

cat "$OURS/BootShim/BootShim.bin" "$FD" > "$FD.shimmed"
gzip -9 -c "$FD.shimmed" > "$FD.shimmed.gz"
python3 "$OURS/Tools/mkbootimg.py" boot "$FD.shimmed.gz" "$OUT/boot-yunluo-mu.img" || exit 1

cp -f "$OURS/vendor_boot.img" "$OUT/vendor_boot-yunluo-mu.img"
python3 "$OURS/Tools/mkbootimg.py" inspect "$OUT/boot-yunluo-mu.img" "$OUT/vendor_boot-yunluo-mu.img"

echo ""
echo "=========== SUCCESS! ==========="
echo "FD:         $FD"
echo "boot.img:   $OUT/boot-yunluo-mu.img"
echo "vendor:     $OUT/vendor_boot-yunluo-mu.img"
echo "================================"
sha256sum "$OUT/boot-yunluo-mu.img" "$OUT/vendor_boot-yunluo-mu.img"
