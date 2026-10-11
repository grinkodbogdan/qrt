#!/bin/sh
# tools/mkrootfs.sh OUT DIR... - QRT's files for the Mi A1 as an ext4 image to flash to a
# spare partition (the system partition of the slot Android does not use): the DIRs
# merged, /etc/qrt-rootfs marking it (src/arch/arm64/linux.c finds and copies it at boot),
# sized to the files plus room, written as an Android sparse image (tools/img2simg.py).
set -eu
out=$1; shift
t=$(mktemp -d)
for d in "$@"; do cp -a "$d"/. "$t"/; done
mkdir -p "$t/etc"
echo "QRT rootfs $(date -u +%Y-%m-%dT%H:%MZ)" > "$t/etc/qrt-rootfs"
kb=$(du -sk "$t" | cut -f1)
size=$(( (kb + kb / 4 + 16384) / 1024 ))M
rm -f "$out.raw"
mke2fs -q -t ext4 -L qrt-rootfs -O ^has_journal -E root_owner=0:0 -d "$t" "$out.raw" "$size"
python3 "$(dirname "$0")/img2simg.py" "$out.raw" "$out"
rm -rf "$t" "$out.raw"
