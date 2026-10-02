#!/usr/bin/env python3
"""kmsg-png.py SERIAL_LOG OUT.png - decode the PNG that tests/ladybird/render.sh writes to
the kernel log as base64 (between PNGBEGIN and PNGEND), dropping other processes' log
lines that the serial console interleaves with it."""
import base64, re, sys
log = open(sys.argv[1], errors="replace").read().replace("\r", "")
m = re.search(r"PNGBEGIN[^\n]*\n(.*?)\n[^\n]*PNGEND", log, re.S)
if not m:
    sys.exit("no PNG in the log")
text = re.sub(r"(proc|linux|trace|net|shell|usb): [^\n]*?(calls|\n)", "", m.group(1))
data = "".join(l[len("base64: "):] if l.startswith("base64: ") else l for l in text.splitlines())
data = re.sub(r"[^A-Za-z0-9+/=]", "", data)
open(sys.argv[2], "wb").write(base64.b64decode(data))
