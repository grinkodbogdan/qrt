#!/bin/bash
# ports/lkl.sh - the Linux kernel as a library (LKL, github.com/lkl/linux) for QRT's
# Linux driver host, /bin/linuxdrv (src/linuxdrv).  Fetches LKL at a fixed commit,
# configures it (arch/lkl defconfig + ports/lkl/config), builds lkl.o with musl and puts
# it, stripped, with LKL's headers in build/linuxdrv/; then `make` links linuxdrv.
# Needs: git, gcc, musl-gcc, flex, bison, bc.  About 10 minutes on 16 cores, 4 GB of disk.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
COMMIT=d0f76a77e8c2ca68bd7d96190b8da09040eab072
SRC="${LKL_SRC:-$root/build/lkl-src}"
OUT="$root/build/linuxdrv"
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
for p in "$here"/lkl/*.patch; do                      # QRT's changes to LKL
    git apply --reverse --check "$p" 2>/dev/null || git apply "$p"
done
make ARCH=lkl defconfig >/dev/null
cat "$here/lkl/config" >> .config
make ARCH=lkl olddefconfig >/dev/null
make -C tools/lkl CC=musl-gcc -j"$(nproc)" "$SRC/tools/lkl/lib/lkl.o"
objcopy --strip-debug tools/lkl/lib/lkl.o "$OUT/lkl.o"
rm -rf "$OUT/include"
cp -r tools/lkl/include "$OUT/include"
echo "ports/lkl.sh: $OUT/lkl.o ($(du -h "$OUT/lkl.o" | cut -f1)); now run make"
