/*
 * inflate.c - DEFLATE (RFC 1951) and gzip (RFC 1952) decompression, for the ARM boot
 * image's ramdisk (a gzip'd cpio archive: Ladybird alone is 150 MB).
 *
 * A small canonical-Huffman decoder in the manner of zlib's "puff": tables of code
 * counts and symbols, one bit at a time.  The output buffer is the caller's.
 */
#include "kernel.h"
#include "rt.h"

typedef struct {
    const u8 *in; usize inlen, ip;
    u8 *out; usize outcap, op;
    u32 bitbuf; int bitcnt;
    int err;
} st_t;

static int bits(st_t *s, int need) {
    u32 v = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->ip >= s->inlen) { s->err = 1; return 0; }
        v |= (u32)s->in[s->ip++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = v >> need;
    s->bitcnt -= need;
    return (int)(v & ((1u << need) - 1));
}

typedef struct { short *count, *symbol; } huff_t;

static int decode(st_t *s, const huff_t *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= bits(s, 1);
        if (s->err) return -1;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    s->err = 2;
    return -1;
}

static int construct(huff_t *h, const short *length, int n) {
    short offs[16];
    for (int len = 0; len <= 15; len++) h->count[len] = 0;
    for (int sym = 0; sym < n; sym++) h->count[length[sym]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len <= 15; len++) { left <<= 1; left -= h->count[len]; if (left < 0) return left; }
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (int sym = 0; sym < n; sym++) if (length[sym]) h->symbol[offs[length[sym]]++] = (short)sym;
    return left;
}

static const short lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const short lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const short dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
                                 4097, 6145, 8193, 12289, 16385, 24577 };
static const short dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int codes(st_t *s, const huff_t *lc, const huff_t *dc) {
    for (;;) {
        int sym = decode(s, lc);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (s->op >= s->outcap) { s->err = 3; return -1; }
            s->out[s->op++] = (u8)sym;
        } else if (sym == 256) return 0;
        else {
            sym -= 257;
            if (sym >= 29) { s->err = 4; return -1; }
            usize len = (usize)lbase[sym] + (usize)bits(s, lext[sym]);
            int ds = decode(s, dc);
            if (ds < 0 || ds >= 30) { s->err = 4; return -1; }
            usize dist = (usize)dbase[ds] + (usize)bits(s, dext[ds]);
            if (s->err) return -1;
            if (dist > s->op || s->op + len > s->outcap) { s->err = 5; return -1; }
            u8 *o = s->out + s->op;
            for (usize i = 0; i < len; i++) o[i] = o[(isize)i - (isize)dist];
            s->op += len;
        }
    }
}

static int stored(st_t *s) {
    s->bitbuf = 0; s->bitcnt = 0;
    if (s->ip + 4 > s->inlen) { s->err = 1; return -1; }
    usize len = s->in[s->ip] | (usize)s->in[s->ip + 1] << 8;
    s->ip += 4;
    if (s->ip + len > s->inlen || s->op + len > s->outcap) { s->err = 3; return -1; }
    memcpy(s->out + s->op, s->in + s->ip, len);
    s->ip += len; s->op += len;
    return 0;
}

static int fixed(st_t *s) {
    static short lcnt[16], lsym[288], dcnt[16], dsym[30];
    static int made;
    static huff_t lc = { lcnt, lsym }, dc = { dcnt, dsym };
    if (!made) {
        short l[288];
        int i;
        for (i = 0; i < 144; i++) l[i] = 8;
        for (; i < 256; i++) l[i] = 9;
        for (; i < 280; i++) l[i] = 7;
        for (; i < 288; i++) l[i] = 8;
        construct(&lc, l, 288);
        for (i = 0; i < 30; i++) l[i] = 5;
        construct(&dc, l, 30);
        made = 1;
    }
    return codes(s, &lc, &dc);
}

static int dynamic(st_t *s) {
    static const short order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    short lengths[320], lcnt[16], lsym[288], dcnt[16], dsym[30];
    huff_t lc = { lcnt, lsym }, dc = { dcnt, dsym };
    int nlen = bits(s, 5) + 257, ndist = bits(s, 5) + 1, ncode = bits(s, 4) + 4;
    if (s->err || nlen > 286 || ndist > 30) { s->err = 6; return -1; }
    int i;
    for (i = 0; i < ncode; i++) lengths[order[i]] = (short)bits(s, 3);
    for (; i < 19; i++) lengths[order[i]] = 0;
    if (construct(&lc, lengths, 19) != 0) { s->err = 6; return -1; }
    i = 0;
    while (i < nlen + ndist) {
        int sym = decode(s, &lc);
        if (sym < 0) return -1;
        if (sym < 16) lengths[i++] = (short)sym;
        else {
            int len = 0, rep;
            if (sym == 16) { if (!i) { s->err = 6; return -1; } len = lengths[i - 1]; rep = 3 + bits(s, 2); }
            else if (sym == 17) rep = 3 + bits(s, 3);
            else rep = 11 + bits(s, 7);
            if (i + rep > nlen + ndist) { s->err = 6; return -1; }
            while (rep--) lengths[i++] = (short)len;
        }
    }
    int e = construct(&lc, lengths, nlen);
    if (e < 0 || (e > 0 && nlen - lc.count[0] != 1)) { s->err = 6; return -1; }
    e = construct(&dc, lengths + nlen, ndist);
    if (e < 0 || (e > 0 && ndist - dc.count[0] != 1)) { s->err = 6; return -1; }
    return codes(s, &lc, &dc);
}

/* raw DEFLATE: bytes written to out, or -1 */
isize inflate_raw(const u8 *in, usize inlen, u8 *out, usize outcap, usize *used) {
    st_t s = { .in = in, .inlen = inlen, .out = out, .outcap = outcap };
    int last;
    do {
        last = bits(&s, 1);
        int type = bits(&s, 2);
        if (s.err) return -1;
        int r = type == 0 ? stored(&s) : type == 1 ? fixed(&s) : type == 2 ? dynamic(&s) : -1;
        if (r || s.err) return -1;
    } while (!last);
    if (used) *used = s.ip;
    return (isize)s.op;
}

/* the size a gzip member says it unpacks to (mod 4 GB) */
usize gzip_size(const u8 *in, usize inlen) {
    if (inlen < 18 || in[0] != 0x1f || in[1] != 0x8b) return 0;
    return in[inlen - 4] | (usize)in[inlen - 3] << 8 | (usize)in[inlen - 2] << 16 | (usize)in[inlen - 1] << 24;
}

/* gzip: bytes written to out, or -1 */
isize gunzip(const u8 *in, usize inlen, u8 *out, usize outcap) {
    if (inlen < 18 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8) return -1;
    u8 flg = in[3];
    usize p = 10;
    if (flg & 4) { if (p + 2 > inlen) return -1; p += 2 + (in[p] | (usize)in[p + 1] << 8); }
    if (flg & 8) { while (p < inlen && in[p]) p++; p++; }
    if (flg & 16) { while (p < inlen && in[p]) p++; p++; }
    if (flg & 2) p += 2;
    if (p >= inlen) return -1;
    return inflate_raw(in + p, inlen - p, out, outcap, NULL);
}
