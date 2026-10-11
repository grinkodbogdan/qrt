#!/usr/bin/env python3
"""img2simg.py RAW OUT - an Android sparse image (what fastboot flashes in pieces): runs of
identical 4-byte-patterned blocks become FILL chunks (zeros too - written, not skipped:
an ext4 image's empty inode tables must be zero on the partition, whatever it held
before), the rest RAW."""
import struct, sys

BS = 4096
raw = open(sys.argv[1], "rb").read()
if len(raw) % BS: raw += b"\0" * (BS - len(raw) % BS)
nblk = len(raw) // BS
chunks = []                                   # (type, nblocks, payload)
i = 0
while i < nblk:
    b = raw[i * BS:(i + 1) * BS]
    pat = b[:4]
    if b == pat * (BS // 4):                  # a fill block: extend the run
        j = i + 1
        while j < nblk and raw[j * BS:(j + 1) * BS] == b: j += 1
        chunks.append((0xCAC2, j - i, pat))
    else:
        j = i + 1
        while j < nblk:
            c = raw[j * BS:(j + 1) * BS]
            if c == c[:4] * (BS // 4): break
            j += 1
        chunks.append((0xCAC1, j - i, raw[i * BS:j * BS]))
    i = j
with open(sys.argv[2], "wb") as f:
    f.write(struct.pack("<IHHHHIIII", 0xED26FF3A, 1, 0, 28, 12, BS, nblk, len(chunks), 0))
    for t, n, data in chunks:
        f.write(struct.pack("<HHII", t, 0, n, 12 + len(data)))
        f.write(data)
