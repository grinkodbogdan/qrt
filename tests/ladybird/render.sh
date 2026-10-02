#!/bin/sh
# render.sh - step L4 in the QEMU test: Ladybird renders tests/ladybird/page.html headless,
# and the PNG goes to the kernel log as base64 (the test decodes it into build/shots).
mkdir -p /tmp/shot
ladybird --headless=screenshot --screenshot-path=/tmp/shot/page.png --screenshot-delay=20 file:///share/tests/page.html >/dev/kmsg 2>&1
echo "lb-exit=$?" >/dev/kmsg
for f in /tmp/shot/*.png; do
    echo "PNGBEGIN $f" >/dev/kmsg
    base64 "$f" >/dev/kmsg
    echo "PNGEND" >/dev/kmsg
done
echo "lb-done" >/dev/kmsg
