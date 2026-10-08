#!/usr/bin/env python3
"""mkbootimg.py KERNEL RAMDISK OUT [CMDLINE] - an Android boot image, header version 0
(what the Xiaomi Mi A1's aboot loads): base 0x80000000, kernel at +0x8000, ramdisk at
+0x1000000, tags at +0x100, 2 KB pages; the id is mkbootimg's SHA-1."""
import hashlib, struct, sys

kernel, ramdisk = open(sys.argv[1], "rb").read(), open(sys.argv[2], "rb").read()
cmdline = (sys.argv[4] if len(sys.argv) > 4 else "").encode()
base, page = 0x80000000, 2048
sha = hashlib.sha1()
for blob in (kernel, ramdisk, b""):
    sha.update(blob); sha.update(struct.pack("<I", len(blob)))
hdr = b"ANDROID!" + struct.pack("<10I", len(kernel), base + 0x8000, len(ramdisk), base + 0x1000000, 0, base + 0xf00000,
                               base + 0x100, page, 0, 0)
hdr += b"qrt".ljust(16, b"\0") + cmdline[:511].ljust(512, b"\0") + sha.digest().ljust(32, b"\0") + cmdline[511:].ljust(1024, b"\0")
pad = lambda b: b + b"\0" * (-len(b) % page)
open(sys.argv[3], "wb").write(pad(hdr) + pad(kernel) + pad(ramdisk))
