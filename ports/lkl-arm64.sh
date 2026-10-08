#!/bin/bash
# ports/lkl-arm64.sh - the Linux kernel as a library (LKL) for 64-bit ARM phones, linked
# into QRT's own kernel (src/arch/arm64/linux.c): fetches LKL at the commit ports/lkl.sh
# uses, applies QRT's patches (ports/lkl/*.patch: device trees and the GIC, non-coherent
# DMA, SMC calls), configures it (defconfig + ports/lkl/config-arm64) and builds lkl.o
# with the aarch64 cross compiler into build/arm64/lkl/.
# Needs: git, aarch64-linux-gnu-gcc, flex, bison, bc.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
COMMIT=d0f76a77e8c2ca68bd7d96190b8da09040eab072
SRC="${LKL_SRC:-$root/build/lkl-src-arm64}"
OUT="$root/build/arm64/lkl"
X=aarch64-linux-gnu-
mkdir -p "$OUT"
if [ ! -d "$SRC/.git" ]; then
    git init -q "$SRC"
    git -C "$SRC" remote add origin https://github.com/lkl/linux.git
fi
if [ "$(git -C "$SRC" rev-parse HEAD 2>/dev/null)" != "$COMMIT" ]; then
    git -C "$SRC" fetch -q --depth 1 origin "$COMMIT"
    git -C "$SRC" checkout -q FETCH_HEAD
fi
cd "$SRC"
for p in "$here"/lkl/*.patch; do
    git apply --reverse --check "$p" 2>/dev/null || git apply "$p"
done
make ARCH=lkl CROSS_COMPILE=$X defconfig >/dev/null
cat "$here/lkl/config-arm64" >> .config
make ARCH=lkl CROSS_COMPILE=$X olddefconfig >/dev/null
make -C tools/lkl CROSS_COMPILE=$X -j"$(nproc)" "$SRC/tools/lkl/lib/lkl.o"
${X}objcopy --strip-debug tools/lkl/lib/lkl.o "$OUT/lkl.o"
rm -rf "$OUT/include"
cp -r tools/lkl/include "$OUT/include"
echo "ports/lkl-arm64.sh: $OUT/lkl.o ($(du -h "$OUT/lkl.o" | cut -f1)); now run make mi-a1"
