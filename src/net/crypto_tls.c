/* crypto_tls.c - SHA-256, HMAC-SHA256, HKDF, X25519 and AES-128-GCM for the
 * TLS 1.3 client.  Written from FIPS 180-4, RFC 2104, RFC 5869, RFC 8446
 * section 7.1, RFC 7748 (the field arithmetic follows TweetNaCl, public
 * domain) and NIST SP 800-38D; tests/test_crypto.c checks each against
 * published vectors. */
#include "crypto.h"

/* ---- SHA-256 ------------------------------------------------------------------ */
static const u32 K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};
static u32 ror(u32 x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_block(u32 h[8], const u8 *p) {
    u32 w[64];
    for (int i = 0; i < 16; i++) w[i] = (u32)p[4 * i] << 24 | (u32)p[4 * i + 1] << 16 | (u32)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        u32 s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        u32 t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        u32 t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void sha256_init(sha256_t *s) {
    static const u32 iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    for (int i = 0; i < 8; i++) s->h[i] = iv[i];
    s->len = 0; s->n = 0;
}

void sha256_update(sha256_t *s, const void *data, usize len) {
    const u8 *p = data;
    s->len += len;
    if (s->n) {
        while (len && s->n < 64) { s->buf[s->n++] = *p++; len--; }
        if (s->n == 64) { sha256_block(s->h, s->buf); s->n = 0; }
    }
    while (len >= 64) { sha256_block(s->h, p); p += 64; len -= 64; }
    while (len) { s->buf[s->n++] = *p++; len--; }
}

void sha256_final(sha256_t *s, u8 out[32]) {
    u64 bits = s->len * 8;
    u8 pad = 0x80;
    sha256_update(s, &pad, 1);
    pad = 0;
    while (s->n != 56) sha256_update(s, &pad, 1);
    u8 l[8];
    for (int i = 0; i < 8; i++) l[i] = (u8)(bits >> (56 - 8 * i));
    sha256_update(s, l, 8);
    for (int i = 0; i < 8; i++) { out[4 * i] = (u8)(s->h[i] >> 24); out[4 * i + 1] = (u8)(s->h[i] >> 16); out[4 * i + 2] = (u8)(s->h[i] >> 8); out[4 * i + 3] = (u8)s->h[i]; }
}

void sha256(const void *data, usize len, u8 out[32]) { sha256_t s; sha256_init(&s); sha256_update(&s, data, len); sha256_final(&s, out); }

void hmac_sha256(const u8 *key, usize klen, const void *data, usize len, u8 out[32]) {
    u8 k0[64] = { 0 }, pad[64], inner[32];
    if (klen > 64) sha256(key, klen, k0); else memcpy(k0, key, klen);
    sha256_t s;
    for (int i = 0; i < 64; i++) pad[i] = k0[i] ^ 0x36;
    sha256_init(&s); sha256_update(&s, pad, 64); sha256_update(&s, data, len); sha256_final(&s, inner);
    for (int i = 0; i < 64; i++) pad[i] = k0[i] ^ 0x5c;
    sha256_init(&s); sha256_update(&s, pad, 64); sha256_update(&s, inner, 32); sha256_final(&s, out);
}

/* ---- HKDF (RFC 5869) and the TLS 1.3 label form ---------------------------------- */
void hkdf_extract(const u8 *salt, usize slen, const u8 *ikm, usize ilen, u8 prk[32]) {
    u8 zero[32] = { 0 };
    if (!salt || !slen) { salt = zero; slen = 32; }
    hmac_sha256(salt, slen, ikm, ilen, prk);
}

void hkdf_expand(const u8 prk[32], const u8 *info, usize ilen, u8 *out, usize olen) {
    u8 t[32], buf[32 + 256 + 1];
    usize tlen = 0, done = 0;
    for (u8 ctr = 1; done < olen; ctr++) {
        usize n = 0;
        memcpy(buf, t, tlen); n += tlen;
        memcpy(buf + n, info, ilen); n += ilen;
        buf[n++] = ctr;
        hmac_sha256(prk, 32, buf, n, t);
        tlen = 32;
        usize take = MIN((usize)32, olen - done);
        memcpy(out + done, t, take);
        done += take;
    }
}

void hkdf_expand_label(const u8 secret[32], const char *label, const u8 *ctx, usize clen, u8 *out, usize olen) {
    u8 info[2 + 1 + 6 + 64 + 1 + 64];
    usize ll = strlen(label), n = 0;
    info[n++] = (u8)(olen >> 8); info[n++] = (u8)olen;
    info[n++] = (u8)(6 + ll);
    memcpy(info + n, "tls13 ", 6); n += 6;
    memcpy(info + n, label, ll); n += ll;
    info[n++] = (u8)clen;
    memcpy(info + n, ctx, clen); n += clen;
    hkdf_expand(secret, info, n, out, olen);
}

/* ---- X25519 (RFC 7748), field arithmetic after TweetNaCl ------------------------ */
typedef i64 gf[16];
static const gf gf121665 = { 0xDB41, 1 };

static void car25519(gf o) {
    for (int i = 0; i < 16; i++) {
        o[i] += (i64)1 << 16;
        i64 c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c * 65536;
    }
}
static void sel25519(gf p, gf q, int b) {
    i64 c = ~((i64)b - 1);
    for (int i = 0; i < 16; i++) { i64 t = c & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t; }
}
static void pack25519(u8 *o, const gf n) {
    gf m, t;
    for (int i = 0; i < 16; i++) t[i] = n[i];
    car25519(t); car25519(t); car25519(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) { m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1); m[i - 1] &= 0xffff; }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) { o[2 * i] = (u8)(t[i] & 0xff); o[2 * i + 1] = (u8)(t[i] >> 8); }
}
static void unpack25519(gf o, const u8 *n) {
    for (int i = 0; i < 16; i++) o[i] = n[2 * i] + ((i64)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}
static void fadd(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void fsub(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }
static void fmul(gf o, const gf a, const gf b) {
    i64 t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++) for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    car25519(o); car25519(o);
}
static void fsq(gf o, const gf a) { fmul(o, a, a); }
static void finv(gf o, const gf in) {
    gf c;
    for (int a = 0; a < 16; a++) c[a] = in[a];
    for (int a = 253; a >= 0; a--) { fsq(c, c); if (a != 2 && a != 4) fmul(c, c, in); }
    for (int a = 0; a < 16; a++) o[a] = c[a];
}

void x25519(u8 out[32], const u8 scalar[32], const u8 point[32]) {
    u8 z[32];
    gf x, a, b, c, d, e, f;
    for (int i = 0; i < 31; i++) z[i] = scalar[i];
    z[31] = (u8)((scalar[31] & 127) | 64);
    z[0] &= 248;
    unpack25519(x, point);
    for (int i = 0; i < 16; i++) { b[i] = x[i]; d[i] = a[i] = c[i] = 0; }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i) {
        int r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, r); sel25519(c, d, r);
        fadd(e, a, c); fsub(a, a, c); fadd(c, b, d); fsub(b, b, d);
        fsq(d, e); fsq(f, a); fmul(a, c, a); fmul(c, b, e);
        fadd(e, a, c); fsub(a, a, c); fsq(b, a); fsub(c, d, f);
        fmul(a, c, gf121665); fadd(a, a, d); fmul(c, c, a); fmul(a, d, f);
        fmul(d, b, x); fsq(b, e);
        sel25519(a, b, r); sel25519(c, d, r);
    }
    finv(c, c);
    fmul(a, a, c);
    pack25519(out, a);
}

void x25519_base(u8 out[32], const u8 scalar[32]) {
    static const u8 nine[32] = { 9 };
    x25519(out, scalar, nine);
}

/* ---- AES-128-GCM (NIST SP 800-38D) ------------------------------------------------ */
static void gf128_mul(u8 x[16], const u8 h[16]) {
    u8 z[16] = { 0 }, v[16];
    memcpy(v, h, 16);
    for (int i = 0; i < 128; i++) {
        if (x[i >> 3] & (0x80 >> (i & 7))) for (int j = 0; j < 16; j++) z[j] ^= v[j];
        int lsb = v[15] & 1;
        for (int j = 15; j > 0; j--) v[j] = (u8)((v[j] >> 1) | (v[j - 1] << 7));
        v[0] >>= 1;
        if (lsb) v[0] ^= 0xe1;
    }
    memcpy(x, z, 16);
}

static void ghash_blocks(u8 y[16], const u8 h[16], const u8 *d, usize len) {
    while (len) {
        usize n = MIN((usize)16, len);
        for (usize i = 0; i < n; i++) y[i] ^= d[i];
        gf128_mul(y, h);
        d += n; len -= n;
    }
}

static void gcm_tag(const aes128_t *a, const u8 j0[16], const u8 *aad, usize alen, const u8 *ct, usize len, u8 tag[16]) {
    u8 h[16] = { 0 }, y[16] = { 0 }, lens[16], ek[16];
    aes128_encrypt(a, h, h);
    ghash_blocks(y, h, aad, alen);
    ghash_blocks(y, h, ct, len);
    u64 ab = (u64)alen * 8, cb = (u64)len * 8;
    for (int i = 0; i < 8; i++) { lens[i] = (u8)(ab >> (56 - 8 * i)); lens[8 + i] = (u8)(cb >> (56 - 8 * i)); }
    ghash_blocks(y, h, lens, 16);
    aes128_encrypt(a, j0, ek);
    for (int i = 0; i < 16; i++) tag[i] = y[i] ^ ek[i];
}

static void gcm_ctr(const aes128_t *a, const u8 j0[16], const u8 *in, usize len, u8 *out) {
    u8 ctr[16], ks[16];
    memcpy(ctr, j0, 16);
    u32 c = (u32)ctr[12] << 24 | (u32)ctr[13] << 16 | (u32)ctr[14] << 8 | ctr[15];
    for (usize off = 0; off < len; off += 16) {
        c++;
        ctr[12] = (u8)(c >> 24); ctr[13] = (u8)(c >> 16); ctr[14] = (u8)(c >> 8); ctr[15] = (u8)c;
        aes128_encrypt(a, ctr, ks);
        usize n = MIN((usize)16, len - off);
        for (usize i = 0; i < n; i++) out[off + i] = in[off + i] ^ ks[i];
    }
}

void gcm_encrypt(const aes128_t *a, const u8 iv[12], const u8 *aad, usize alen, const u8 *pt, usize len, u8 *ct, u8 tag[16]) {
    u8 j0[16];
    memcpy(j0, iv, 12); j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
    gcm_ctr(a, j0, pt, len, ct);
    gcm_tag(a, j0, aad, alen, ct, len, tag);
}

int gcm_decrypt(const aes128_t *a, const u8 iv[12], const u8 *aad, usize alen, const u8 *ct, usize len, u8 *pt, const u8 tag[16]) {
    u8 j0[16], want[16];
    memcpy(j0, iv, 12); j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
    gcm_tag(a, j0, aad, alen, ct, len, want);
    u8 diff = 0;
    for (int i = 0; i < 16; i++) diff |= want[i] ^ tag[i];
    if (diff) return -1;
    gcm_ctr(a, j0, ct, len, pt);
    return 0;
}
