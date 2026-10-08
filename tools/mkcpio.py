#!/usr/bin/env python3
"""mkcpio.py DIR OUT - a "newc" cpio archive of DIR (the ARM boot image's ramdisk)."""
import os, sys

def entry(out, name, mode, data, ino):
    nb = name.encode() + b"\0"
    hdr = "070701" + "".join("%08x" % v for v in (ino, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(nb), 0))
    out += hdr.encode() + nb
    out += b"\0" * (-len(out) % 4)
    out += data
    out += b"\0" * (-len(out) % 4)

root, dest = sys.argv[1], sys.argv[2]
out, ino = bytearray(), 1
for d, dirs, files in sorted(os.walk(root)):
    dirs.sort()
    rel = os.path.relpath(d, root)
    if rel != ".":
        entry(out, rel, 0o040755, b"", ino); ino += 1
    for f in sorted(files):
        p = os.path.join(d, f)
        entry(out, os.path.normpath(os.path.join(rel, f)), 0o100644 | (os.stat(p).st_mode & 0o111), open(p, "rb").read(), ino); ino += 1
entry(out, "TRAILER!!!", 0, b"", 0)
open(dest, "wb").write(out)
