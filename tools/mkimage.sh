#!/usr/bin/env bash
# mkimage.sh OUT.img BOOTIA32.EFI BOOTX64.EFI
#
# Builds a GPT disk (64 MiB, more when the programs need it) with a single FAT32 EFI System Partition that
# carries both loaders in the removable-media fallback path
# (\EFI\BOOT\BOOTIA32.EFI and \EFI\BOOT\BOOTX64.EFI), so the same stick
# boots on 32-bit UEFI (Venue 8 Pro 5830, Bay Trail) and 64-bit UEFI
# (Venue 8 Pro 5855, Cherry Trail) firmware.
set -euo pipefail
out=$1 ia32=$2 x64=$3 rootfs=${4:-}
here=$(cd "$(dirname "$0")/.." && pwd)

# 64 MiB, or more when /bin and the rest need it (Ladybird): the contents plus a quarter
size_mib=64
if [ -n "$rootfs" ] && [ -d "$rootfs" ]; then
    need=$(( $(du -sm "$rootfs" | cut -f1) * 5 / 4 + 24 ))
    [ $need -gt $size_mib ] && size_mib=$(( (need + 15) / 16 * 16 ))
fi
part_start=2048                      # sectors (1 MiB alignment)
part_sectors=$(( (size_mib - 2) * 2048 ))

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

truncate -s 0 "$out"
truncate -s ${size_mib}M "$out"
sgdisk -o \
       -n 1:${part_start}:+${part_sectors} -t 1:ef00 -c 1:"QRT ESP" "$out" >/dev/null

esp=$tmp/esp.img
truncate -s $(( part_sectors * 512 )) "$esp"
mkfs.fat -F 32 -n QRT "$esp" >/dev/null
mmd   -i "$esp" ::/EFI ::/EFI/BOOT ::/qrt
mcopy -i "$esp" "$ia32" ::/EFI/BOOT/BOOTIA32.EFI
mcopy -i "$esp" "$x64"  ::/EFI/BOOT/BOOTX64.EFI
# a few files so the Files app has something to show on first boot
for f in "$here"/image/*; do
    [ -e "$f" ] && mcopy -i "$esp" "$f" ::/qrt/
done

# Linux programs for the Terminal (/bin), skipping empty placeholders
if [ -n "$rootfs" ] && [ -d "$rootfs/bin" ]; then
    mmd -i "$esp" ::/bin
    for f in "$rootfs"/bin/*; do
        [ -s "$f" ] && mcopy -i "$esp" "$f" ::/bin/
    done
fi

# shared libraries for dynamically linked programs (/lib64, /lib/x86_64-linux-gnu) and
# data (/share: tests, Ladybird's resources, fonts; /etc: fontconfig)
for d in lib lib64 share etc; do
    if [ -n "$rootfs" ] && [ -d "$rootfs/$d" ]; then
        mcopy -s -i "$esp" "$rootfs/$d" ::/
    fi
done

# device firmware (Wi-Fi) under \lib\firmware, with its licence
if [ -d "$here/firmware" ]; then
    mmd -i "$esp" ::/lib 2>/dev/null || true
    mmd -i "$esp" ::/lib/firmware
    for f in "$here"/firmware/*; do
        [ -s "$f" ] && mcopy -i "$esp" "$f" ::/lib/firmware/
    done
fi

dd if="$esp" of="$out" bs=512 seek=$part_start conv=notrunc status=none
echo "wrote $out ($(du -h "$out" | cut -f1) on disk, ${size_mib} MiB image)"
