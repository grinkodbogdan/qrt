#!/usr/bin/env bash
# run-qemu.sh [ia32|x64] [extra qemu args...]
# Boots build/qrt.img under OVMF with a USB touchscreen-like tablet device.
set -euo pipefail
arch=${1:-ia32}; shift || true
here=$(cd "$(dirname "$0")/.." && pwd)
ovmf=${OVMF_DIR:-/usr/share/OVMF}
if [ "$arch" = ia32 ]; then code=$ovmf/OVMF32_CODE_4M.fd vars=$ovmf/OVMF32_VARS_4M.fd
else code=$ovmf/OVMF_CODE_4M.fd vars=$ovmf/OVMF_VARS_4M.fd; fi

mkdir -p "$here/build"
myvars=$here/build/vars-$arch.fd
[ -f "$myvars" ] || cp "$vars" "$myvars"

exec qemu-system-x86_64 \
    -machine q35,i8042=off -m 2048 -cpu max -smp 4 \
    -drive if=pflash,format=raw,readonly=on,file="$code" \
    -drive if=pflash,format=raw,file="$myvars" \
    -drive format=raw,file="$here/build/qrt.img" \
    -device qemu-xhci,id=xhci -device usb-tablet,bus=xhci.0,port=1 \
    -device usb-hub,bus=xhci.0,port=2 -device usb-kbd,bus=xhci.0,port=2.1 \
    -device VGA,xres=1280,yres=800 \
    -rtc base=localtime \
    "$@"
