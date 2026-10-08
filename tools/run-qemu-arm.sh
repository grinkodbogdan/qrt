#!/usr/bin/env bash
# run-qemu-arm.sh [extra qemu args] - boot build/arm64/Image on QEMU's virt board: a
# Cortex-A53 (the Mi A1's core), GICv2, a phone-sized ramfb screen, virtio keyboard and
# a tablet that plays the touch screen.  The console is on stdio.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
exec qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a53 -m 2048 \
    -kernel "$here/build/arm64/Image" -initrd "$here/build/arm64/ramdisk.cpio" \
    -device ramfb -device virtio-keyboard-device -device virtio-tablet-device \
    -global virtio-mmio.force-legacy=false -rtc base=localtime -serial stdio "$@"
