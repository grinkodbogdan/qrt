/* crypto.c - SHA-1, HMAC, PBKDF2, 802.11 PRF, AES-128, key wrap, CCM.
 * Written from FIPS 180-4, RFC 2104, RFC 2898, FIPS 197, RFC 3394 and
 * RFC 3610; tests/test_crypto.c checks each against published vectors. */
#include "crypto.h"

/* ---- SHA-1 ------------------------------------------------------------------ */
static u32 rol(u32 x, int n) { return (x << n) | (x >> (32 - n)); }

static void sha1_block(u32 h[5], const u8 *p) {
    u32 w[80];
    for (int i = 0; i < 16; i++) w[i] = (u32)p[4 * i] << 24 | (u32)p[4 * i + 1] << 16 | (u32)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5a827999; }
        else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8f1bbcdc; }
        else             { f = b ^ c ^ d;                    k = 0xca62c1d6; }
        u32 t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void sha1_init(sha1_t *s) {
    s->h[0] = 0x67452301; s->h[1] = 0xefcdab89; s->h[2] = 0x98badcfe; s->h[3] = 0x10325476; s->h[4] = 0xc3d2e1f0;
    s->len = 0; s->n = 0;
}

void sha1_update(sha1_t *s, const void *data, usize len) {
    const u8 *p = data;
    s->len += len;
    while (len) {
        u32 take = (u32)MIN((usize)(64 - s->n), len);
        memcpy(s->buf + s->n, p, take);
        s->n += take; p += take; len -= take;
        if (s->n == 64) { sha1_block(s->h, s->buf); s->n = 0; }
    }
}

void sha1_final(sha1_t *s, u8 out[20]) {
    u64 bits = s->len * 8;
    u8 pad = 0x80;
    sha1_update(s, &pad, 1);
    u8 z = 0;
    while (s->n != 56) sha1_update(s, &z, 1);
    u8 l[8];
    for (int i = 0; i < 8; i++) l[i] = (u8)(bits >> (56 - 8 * i));
    sha1_update(s, l, 8);
    for (int i = 0; i < 5; i++) { out[4 * i] = (u8)(s->h[i] >> 24); out[4 * i + 1] = (u8)(s->h[i] >> 16); out[4 * i + 2] = (u8)(s->h[i] >> 8); out[4 * i + 3] = (u8)s->h[i]; }
}

void sha1(const void *data, usize len, u8 out[20]) { sha1_t s; sha1_init(&s); sha1_update(&s, data, len); sha1_final(&s, out); }

/* ---- HMAC-SHA1, PBKDF2, PRF --------------------------------------------------- */
void hmac_sha1_vec(const u8 *key, usize klen, int n, const void *const *data, const usize *len, u8 out[20]) {
    u8 k[64] = { 0 }, pad[64], inner[20];
    if (klen > 64) sha1(key, klen, k); else memcpy(k, key, klen);
    sha1_t s;
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha1_init(&s);
    sha1_update(&s, pad, 64);
    for (int i = 0; i < n; i++) sha1_update(&s, data[i], len[i]);
    sha1_final(&s, inner);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sha1_init(&s);
    sha1_update(&s, pad, 64);
    sha1_update(&s, inner, 20);
    sha1_final(&s, out);
}

void hmac_sha1(const u8 *key, usize klen, const void *data, usize len, u8 out[20]) {
    const void *d[1] = { data };
    usize l[1] = { len };
    hmac_sha1_vec(key, klen, 1, d, l, out);
}

void pbkdf2_sha1(const char *pass, const u8 *salt, usize slen, int iter, u8 *out, usize olen) {
    usize plen = strlen(pass);
    for (u32 block = 1; olen; block++) {
        u8 be[4] = { (u8)(block >> 24), (u8)(block >> 16), (u8)(block >> 8), (u8)block };
        const void *d[2] = { salt, be };
        usize l[2] = { slen, 4 };
        u8 u[20], t[20];
        hmac_sha1_vec((const u8 *)pass, plen, 2, d, l, u);
        memcpy(t, u, 20);
        for (int i = 1; i < iter; i++) {
            hmac_sha1((const u8 *)pass, plen, u, 20, u);
            for (int j = 0; j < 20; j++) t[j] ^= u[j];
        }
        usize n = MIN(olen, (usize)20);
        memcpy(out, t, n);
        out += n; olen -= n;
    }
}

void prf_sha1(const u8 *key, usize klen, const char *label, const u8 *data, usize dlen, u8 *out, usize olen) {
    u8 zero = 0;
    for (u8 i = 0; olen; i++) {
        const void *d[4] = { label, &zero, data, &i };
        usize l[4] = { strlen(label), 1, dlen, 1 };
        u8 h[20];
        hmac_sha1_vec(key, klen, 4, d, l, h);
        usize n = MIN(olen, (usize)20);
        memcpy(out, h, n);
        out += n; olen -= n;
    }
}

/* ---- AES-128 ------------------------------------------------------------------ */
static u8 sbox[256], isbox[256];
static int tables_ready;

static u8 rotl8(u8 x, int s) { return (u8)((x << s) | (x >> (8 - s))); }

static void aes_tables(void) {
    /* S-box from the multiplicative inverse in GF(2^8) plus the affine map */
    u8 p = 1, q = 1;
    do {
        p = (u8)(p ^ (p << 1) ^ (p & 0x80 ? 0x1b : 0));
        q ^= (u8)(q << 1); q ^= (u8)(q << 2); q ^= (u8)(q << 4);
        if (q & 0x80) q ^= 0x09;
        u8 x = (u8)(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4));
        sbox[p] = x ^ 0x63;
    } while (p != 1);
    sbox[0] = 0x63;
    for (int i = 0; i < 256; i++) isbox[sbox[i]] = (u8)i;
    tables_ready = 1;
}

static u8 xtime(u8 x) { return (u8)((x << 1) ^ (x & 0x80 ? 0x1b : 0)); }
static u8 mul(u8 a, u8 b) { u8 r = 0; while (b) { if (b & 1) r ^= a; a = xtime(a); b >>= 1; } return r; }

void aes128_init(aes128_t *a, const u8 key[16]) {
    if (!tables_ready) aes_tables();
    for (int i = 0; i < 4; i++) a->rk[i] = (u32)key[4 * i] << 24 | (u32)key[4 * i + 1] << 16 | (u32)key[4 * i + 2] << 8 | key[4 * i + 3];
    u8 rcon = 1;
    for (int i = 4; i < 44; i++) {
        u32 t = a->rk[i - 1];
        if (i % 4 == 0) {
            t = (t << 8) | (t >> 24);
            t = (u32)sbox[t >> 24] << 24 | (u32)sbox[(t >> 16) & 0xff] << 16 | (u32)sbox[(t >> 8) & 0xff] << 8 | sbox[t & 0xff];
            t ^= (u32)rcon << 24;
            rcon = xtime(rcon);
        }
        a->rk[i] = a->rk[i - 4] ^ t;
    }
}

static void add_round_key(u8 s[16], const u32 *rk) {
    for (int c = 0; c < 4; c++) { u32 k = rk[c]; s[4 * c] ^= (u8)(k >> 24); s[4 * c + 1] ^= (u8)(k >> 16); s[4 * c + 2] ^= (u8)(k >> 8); s[4 * c + 3] ^= (u8)k; }
}

void aes128_encrypt(const aes128_t *a, const u8 in[16], u8 out[16]) {
    u8 s[16], t[16];
    memcpy(s, in, 16);
    add_round_key(s, a->rk);
    for (int r = 1; r <= 10; r++) {
        for (int i = 0; i < 16; i++) s[i] = sbox[s[i]];
        for (int c = 0; c < 4; c++) for (int row = 0; row < 4; row++) t[4 * c + row] = s[4 * ((c + row) % 4) + row];   /* ShiftRows */
        if (r != 10)
            for (int c = 0; c < 4; c++) {                                                  /* MixColumns */
                u8 *col = t + 4 * c, a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                col[0] = (u8)(xtime(a0) ^ xtime(a1) ^ a1 ^ a2 ^ a3);
                col[1] = (u8)(a0 ^ xtime(a1) ^ xtime(a2) ^ a2 ^ a3);
                col[2] = (u8)(a0 ^ a1 ^ xtime(a2) ^ xtime(a3) ^ a3);
                col[3] = (u8)(xtime(a0) ^ a0 ^ a1 ^ a2 ^ xtime(a3));
            }
        memcpy(s, t, 16);
        add_round_key(s, a->rk + 4 * r);
    }
    memcpy(out, s, 16);
}

void aes128_decrypt(const aes128_t *a, const u8 in[16], u8 out[16]) {
    u8 s[16], t[16];
    memcpy(s, in, 16);
    add_round_key(s, a->rk + 40);
    for (int r = 9; r >= 0; r--) {
        for (int c = 0; c < 4; c++) for (int row = 0; row < 4; row++) t[4 * ((c + row) % 4) + row] = s[4 * c + row];   /* InvShiftRows */
        for (int i = 0; i < 16; i++) t[i] = isbox[t[i]];
        memcpy(s, t, 16);
        add_round_key(s, a->rk + 4 * r);
        if (r)
            for (int c = 0; c < 4; c++) {                                                  /* InvMixColumns */
                u8 *col = s + 4 * c, a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                col[0] = (u8)(mul(a0, 14) ^ mul(a1, 11) ^ mul(a2, 13) ^ mul(a3, 9));
                col[1] = (u8)(mul(a0, 9) ^ mul(a1, 14) ^ mul(a2, 11) ^ mul(a3, 13));
                col[2] = (u8)(mul(a0, 13) ^ mul(a1, 9) ^ mul(a2, 14) ^ mul(a3, 11));
                col[3] = (u8)(mul(a0, 11) ^ mul(a1, 13) ^ mul(a2, 9) ^ mul(a3, 14));
            }
    }
    memcpy(out, s, 16);
}

/* ---- AES key wrap (RFC 3394) ----------------------------------------------------- */
int aes_unwrap(const u8 kek[16], const u8 *in, usize inlen, u8 *out) {
    if (inlen < 24 || inlen % 8) return -1;
    usize n = inlen / 8 - 1;
    aes128_t a;
    aes128_init(&a, kek);
    u8 A[8], b[16];
    memcpy(A, in, 8);
    memcpy(out, in + 8, n * 8);
    for (int j = 5; j >= 0; j--)
        for (usize i = n; i >= 1; i--) {
            u64 t = (u64)n * (u64)j + i;
            memcpy(b, A, 8);
            for (int k = 0; k < 8; k++) b[7 - k] ^= (u8)(t >> (8 * k));
            memcpy(b + 8, out + (i - 1) * 8, 8);
            aes128_decrypt(&a, b, b);
            memcpy(A, b, 8);
            memcpy(out + (i - 1) * 8, b + 8, 8);
        }
    for (int k = 0; k < 8; k++) if (A[k] != 0xa6) return -1;
    return 0;
}

void aes_wrap(const u8 kek[16], const u8 *in, usize inlen, u8 *out) {
    usize n = inlen / 8;
    aes128_t a;
    aes128_init(&a, kek);
    u8 A[8], b[16];
    memset(A, 0xa6, 8);
    memcpy(out + 8, in, inlen);
    for (int j = 0; j <= 5; j++)
        for (usize i = 1; i <= n; i++) {
            memcpy(b, A, 8);
            memcpy(b + 8, out + i * 8, 8);
            aes128_encrypt(&a, b, b);
            u64 t = (u64)n * (u64)j + i;
            memcpy(A, b, 8);
            for (int k = 0; k < 8; k++) A[7 - k] ^= (u8)(t >> (8 * k));
            memcpy(out + i * 8, b + 8, 8);
        }
    memcpy(out, A, 8);
}

/* ---- CCM (RFC 3610) with M = 8, L = 2 ------------------------------------------------ */
static void ccm_mac(const aes128_t *a, const u8 nonce[13], const u8 *aad, usize alen, const u8 *pt, usize len, u8 tag[16]) {
    u8 x[16], blk[16];
    x[0] = (u8)((alen ? 0x40 : 0) | ((8 - 2) / 2) << 3 | (2 - 1));
    memcpy(x + 1, nonce, 13);
    x[14] = (u8)(len >> 8); x[15] = (u8)len;
    aes128_encrypt(a, x, x);
    if (alen) {
        /* 2-byte length prefix, then the AAD, zero-padded to 16 */
        u8 first[16] = { 0 };
        first[0] = (u8)(alen >> 8); first[1] = (u8)alen;
        usize take = MIN(alen, (usize)14);
        memcpy(first + 2, aad, take);
        for (int i = 0; i < 16; i++) x[i] ^= first[i];
        aes128_encrypt(a, x, x);
        for (usize off = take; off < alen; off += 16) {
            memset(blk, 0, 16);
            memcpy(blk, aad + off, MIN((usize)16, alen - off));
            for (int i = 0; i < 16; i++) x[i] ^= blk[i];
            aes128_encrypt(a, x, x);
        }
    }
    for (usize off = 0; off < len; off += 16) {
        memset(blk, 0, 16);
        memcpy(blk, pt + off, MIN((usize)16, len - off));
        for (int i = 0; i < 16; i++) x[i] ^= blk[i];
        aes128_encrypt(a, x, x);
    }
    memcpy(tag, x, 16);
}

static void ccm_ctr(const aes128_t *a, const u8 nonce[13], u16 counter, u8 out[16]) {
    u8 ctr[16];
    ctr[0] = 2 - 1;
    memcpy(ctr + 1, nonce, 13);
    ctr[14] = (u8)(counter >> 8); ctr[15] = (u8)counter;
    aes128_encrypt(a, ctr, out);
}

static void ccm_crypt(const aes128_t *a, const u8 nonce[13], const u8 *in, usize len, u8 *out) {
    u8 ks[16];
    for (usize off = 0, i = 1; off < len; off += 16, i++) {
        ccm_ctr(a, nonce, (u16)i, ks);
        usize n = MIN((usize)16, len - off);
        for (usize j = 0; j < n; j++) out[off + j] = in[off + j] ^ ks[j];
    }
}

void ccm_encrypt(const aes128_t *a, const u8 nonce[13], const u8 *aad, usize alen, const u8 *pt, usize len, u8 *ct, u8 mic[8]) {
    u8 tag[16], s0[16];
    ccm_mac(a, nonce, aad, alen, pt, len, tag);
    ccm_ctr(a, nonce, 0, s0);
    for (int i = 0; i < 8; i++) mic[i] = tag[i] ^ s0[i];
    ccm_crypt(a, nonce, pt, len, ct);
}

int ccm_decrypt(const aes128_t *a, const u8 nonce[13], const u8 *aad, usize alen, const u8 *ct, usize len, u8 *pt, const u8 mic[8]) {
    u8 tag[16], s0[16];
    ccm_crypt(a, nonce, ct, len, pt);
    ccm_mac(a, nonce, aad, alen, pt, len, tag);
    ccm_ctr(a, nonce, 0, s0);
    u8 diff = 0;
    for (int i = 0; i < 8; i++) diff |= (u8)(mic[i] ^ tag[i] ^ s0[i]);
    return diff ? -1 : 0;
}

/* ---- randomness: RDRAND when the CPU has it, else hashed TSC jitter ---------------- */
static int have_rdrand(void) {
    u32 a = 1, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    return (c >> 30) & 1;
}

void random_bytes(u8 *out, usize n) {
    static u8 pool[20];
    static u64 counter;
    int hw = have_rdrand();
    while (n) {
        u8 blk[20];
        if (hw) {
            u32 v[5];
            for (int i = 0; i < 5; i++) {
                unsigned char ok = 0;
                for (int t = 0; t < 10 && !ok; t++) __asm__ volatile("rdrand %0; setc %1" : "=r"(v[i]), "=qm"(ok));
            }
            memcpy(blk, v, 20);
        }
        u32 lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        struct { u8 pool[20]; u64 tsc, ctr; u8 hw[20]; } mix;
        memcpy(mix.pool, pool, 20);
        mix.tsc = ((u64)hi << 32) | lo;
        mix.ctr = ++counter;
        memcpy(mix.hw, hw ? blk : pool, 20);
        sha1(&mix, sizeof mix, pool);                 /* new state */
        u8 o[21];
        memcpy(o, pool, 20);
        o[20] = 1;
        sha1(o, 21, blk);                             /* output: never the state itself */
        usize take = MIN(n, (usize)20);
        memcpy(out, blk, take);
        out += take; n -= take;
    }
}
