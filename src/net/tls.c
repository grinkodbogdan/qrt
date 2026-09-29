/*
 * tls.c - TLS 1.3 client (RFC 8446), written for QRT's browser.
 *
 * One cipher suite (TLS_AES_128_GCM_SHA256, which every TLS 1.3 server
 * must implement), one key exchange group (X25519), no resumption, no
 * early data, no client certificates (an empty one is sent if asked).
 *
 * The handshake: ClientHello -> ServerHello (X25519 share) -> handshake
 * keys -> EncryptedExtensions, Certificate, CertificateVerify, Finished
 * (checked) -> client Finished -> application keys.  The certificate and
 * CertificateVerify are parsed past but not checked; see tls.h.
 */
#include "tls.h"
#include "crypto.h"

enum { CT_CCS = 20, CT_ALERT = 21, CT_HANDSHAKE = 22, CT_APPDATA = 23 };
enum { HS_CLIENT_HELLO = 1, HS_SERVER_HELLO = 2, HS_NEW_SESSION_TICKET = 4, HS_ENCRYPTED_EXTENSIONS = 8,
       HS_CERTIFICATE = 11, HS_CERTIFICATE_REQUEST = 13, HS_CERTIFICATE_VERIFY = 15, HS_FINISHED = 20, HS_KEY_UPDATE = 24 };
enum { ST_HELLO, ST_EE, ST_CERT, ST_CV, ST_FIN, ST_OPEN, ST_FAILED };

typedef struct { u8 *p; usize len, cap; } buf_t;
typedef struct { aes128_t aes; u8 key[16], iv[12], secret[32]; u64 seq; int on; } dir_t;

struct tls {
    tls_io_t io;
    char host[256];
    int state, eof, cert_requested;
    char err[96];
    u8 priv[32], pub[32];
    sha256_t tr;                     /* transcript hash */
    u8 hs_secret[32], c_hs[32], s_hs[32];
    dir_t rd, wr;
    buf_t out, in, hs, app;          /* to send; received records; handshake bytes; plaintext */
    usize app_off;
};

/* ---- small buffers ---------------------------------------------------------------- */
static void buf_put(buf_t *b, const void *d, usize n) {
    if (b->len + n > b->cap) {
        usize cap = MAX(b->cap * 2, b->len + n + 1024);
        u8 *p = kalloc(cap);
        if (b->len) memcpy(p, b->p, b->len);
        if (b->p) kfree(b->p);
        b->p = p; b->cap = cap;
    }
    memcpy(b->p + b->len, d, n);
    b->len += n;
}
static void buf_drop(buf_t *b, usize n) {
    if (n >= b->len) { b->len = 0; return; }
    memmove(b->p, b->p + n, b->len - n);
    b->len -= n;
}
static void buf_free(buf_t *b) { if (b->p) kfree(b->p); b->p = NULL; b->len = b->cap = 0; }

static int fail(tls_t *t, const char *why) {
    if (t->state != ST_FAILED) strlcpy(t->err, why, sizeof t->err);
    t->state = ST_FAILED;
    return -1;
}

static u16 rd16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }
static u32 rd24(const u8 *p) { return (u32)p[0] << 16 | (u32)p[1] << 8 | p[2]; }

/* ---- key schedule ------------------------------------------------------------------ */
static void transcript(tls_t *t, u8 out[32]) { sha256_t c = t->tr; sha256_final(&c, out); }

static void set_keys(dir_t *d, const u8 secret[32]) {
    memcpy(d->secret, secret, 32);
    hkdf_expand_label(secret, "key", NULL, 0, d->key, 16);
    hkdf_expand_label(secret, "iv", NULL, 0, d->iv, 12);
    aes128_init(&d->aes, d->key);
    d->seq = 0;
    d->on = 1;
}

static void nonce(const dir_t *d, u8 out[12]) {
    memcpy(out, d->iv, 12);
    for (int i = 0; i < 8; i++) out[4 + i] ^= (u8)(d->seq >> (56 - 8 * i));
}

/* ---- records out -------------------------------------------------------------------- */
static void send_record(tls_t *t, u8 type, const u8 *data, usize len) {
    while (len) {
        usize n = MIN(len, (usize)16384);
        if (!t->wr.on) {
            u8 h[5] = { type, 3, type == CT_HANDSHAKE ? 1 : 3, (u8)(n >> 8), (u8)n };
            buf_put(&t->out, h, 5);
            buf_put(&t->out, data, n);
        } else {
            usize il = n + 1;                                /* inner plaintext: data, type */
            u8 *inner = kalloc(il), *ct = kalloc(il);
            memcpy(inner, data, n);
            inner[n] = type;
            u8 h[5] = { CT_APPDATA, 3, 3, (u8)((il + 16) >> 8), (u8)(il + 16) }, iv[12], tag[16];
            nonce(&t->wr, iv);
            gcm_encrypt(&t->wr.aes, iv, h, 5, inner, il, ct, tag);
            t->wr.seq++;
            buf_put(&t->out, h, 5);
            buf_put(&t->out, ct, il);
            buf_put(&t->out, tag, 16);
            kfree(inner); kfree(ct);
        }
        data += n; len -= n;
    }
}

static void send_handshake(tls_t *t, u8 type, const u8 *body, usize len) {
    u8 h[4] = { type, (u8)(len >> 16), (u8)(len >> 8), (u8)len };
    u8 *m = kalloc(len + 4);
    memcpy(m, h, 4);
    memcpy(m + 4, body, len);
    sha256_update(&t->tr, m, len + 4);
    send_record(t, CT_HANDSHAKE, m, len + 4);
    kfree(m);
}

static void flush(tls_t *t) {
    while (t->out.len) {
        int n = t->io.send(t->io.ctx, t->out.p, t->out.len);
        if (n < 0) { fail(t, "connection lost while sending"); return; }
        if (n == 0) return;
        buf_drop(&t->out, (usize)n);
    }
}

/* ---- ClientHello ---------------------------------------------------------------------- */
static void put16b(u8 *p, usize v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }

static void client_hello(tls_t *t) {
    u8 b[512];
    usize o = 0;
    b[o++] = 3; b[o++] = 3;                                       /* legacy_version TLS 1.2 */
    random_bytes(b + o, 32); o += 32;                             /* random */
    b[o++] = 32; random_bytes(b + o, 32); o += 32;                /* legacy session id (middlebox compatibility) */
    b[o++] = 0; b[o++] = 2; b[o++] = 0x13; b[o++] = 0x01;         /* TLS_AES_128_GCM_SHA256 */
    b[o++] = 1; b[o++] = 0;                                       /* no compression */
    usize ext_len_at = o; o += 2;
    usize hl = strlen(t->host);
    int is_ip = 1;
    for (usize i = 0; i < hl; i++) if (!((t->host[i] >= '0' && t->host[i] <= '9') || t->host[i] == '.')) is_ip = 0;
    if (!is_ip && hl && hl < 256) {                               /* server_name */
        put16b(b + o, 0); put16b(b + o + 2, hl + 5); put16b(b + o + 4, hl + 3);
        b[o + 6] = 0; put16b(b + o + 7, hl); memcpy(b + o + 9, t->host, hl);
        o += 9 + hl;
    }
    static const u8 groups[] = { 0x00, 0x0a, 0x00, 0x04, 0x00, 0x02, 0x00, 0x1d };            /* x25519 */
    memcpy(b + o, groups, sizeof groups); o += sizeof groups;
    static const u8 sigalgs[] = { 0x00, 0x0d, 0x00, 0x12, 0x00, 0x10,
                                  0x04, 0x03, 0x08, 0x04, 0x04, 0x01, 0x05, 0x03, 0x08, 0x05, 0x05, 0x01, 0x08, 0x06, 0x06, 0x01 };
    memcpy(b + o, sigalgs, sizeof sigalgs); o += sizeof sigalgs;
    static const u8 versions[] = { 0x00, 0x2b, 0x00, 0x03, 0x02, 0x03, 0x04 };               /* TLS 1.3 only */
    memcpy(b + o, versions, sizeof versions); o += sizeof versions;
    static const u8 alpn[] = { 0x00, 0x10, 0x00, 0x0b, 0x00, 0x09, 0x08, 'h', 't', 't', 'p', '/', '1', '.', '1' };
    memcpy(b + o, alpn, sizeof alpn); o += sizeof alpn;
    put16b(b + o, 51); put16b(b + o + 2, 38); put16b(b + o + 4, 36);                          /* key_share */
    put16b(b + o + 6, 0x001d); put16b(b + o + 8, 32); memcpy(b + o + 10, t->pub, 32);
    o += 42;
    put16b(b + ext_len_at, o - ext_len_at - 2);
    send_handshake(t, HS_CLIENT_HELLO, b, o);
}

/* ---- handshake messages in ------------------------------------------------------------- */
static int server_hello(tls_t *t, const u8 *m, usize len) {
    static const u8 hrr[32] = { 0xcf, 0x21, 0xad, 0x74, 0xe5, 0x9a, 0x61, 0x11, 0xbe, 0x1d, 0x8c, 0x02, 0x1e, 0x65, 0xb8, 0x91,
                                0xc2, 0xa2, 0x11, 0x16, 0x7a, 0xbb, 0x8c, 0x5e, 0x07, 0x9e, 0x09, 0xe2, 0xc8, 0xa8, 0x33, 0x9c };
    if (len < 38) return fail(t, "short ServerHello");
    if (!memcmp(m + 2, hrr, 32)) return fail(t, "the server wants a key exchange QRT lacks (no X25519)");
    usize o = 34;
    usize sid = m[o++];
    o += sid;
    if (o + 3 > len) return fail(t, "bad ServerHello");
    if (rd16(m + o) != 0x1301) return fail(t, "the server chose a cipher QRT does not offer");
    o += 3;
    if (o + 2 > len) return fail(t, "the server does not speak TLS 1.3");
    usize el = rd16(m + o); o += 2;
    if (o + el > len) return fail(t, "bad ServerHello extensions");
    int v13 = 0;
    const u8 *share = NULL;
    for (usize e = o; e + 4 <= o + el;) {
        u16 type = rd16(m + e), l = rd16(m + e + 2);
        const u8 *d = m + e + 4;
        if (e + 4 + l > o + el) break;
        if (type == 43 && l == 2 && rd16(d) == 0x0304) v13 = 1;
        if (type == 51 && l >= 4 && rd16(d) == 0x001d && rd16(d + 2) == 32 && l >= 36) share = d + 4;
        e += 4u + l;
    }
    if (!v13) return fail(t, "the server does not speak TLS 1.3");
    if (!share) return fail(t, "no X25519 key share from the server");

    u8 shared[32], zero[32] = { 0 }, early[32], derived[32], empty[32], th[32];
    x25519(shared, t->priv, share);
    u8 acc = 0;
    for (int i = 0; i < 32; i++) acc |= shared[i];
    if (!acc) return fail(t, "bad key share from the server");
    hkdf_extract(NULL, 0, zero, 32, early);
    sha256("", 0, empty);
    hkdf_expand_label(early, "derived", empty, 32, derived, 32);
    hkdf_extract(derived, 32, shared, 32, t->hs_secret);
    transcript(t, th);
    hkdf_expand_label(t->hs_secret, "c hs traffic", th, 32, t->c_hs, 32);
    hkdf_expand_label(t->hs_secret, "s hs traffic", th, 32, t->s_hs, 32);
    set_keys(&t->rd, t->s_hs);
    t->state = ST_EE;
    return 0;
}

static int finished(tls_t *t, const u8 *m, usize len, const u8 th_before[32]) {
    u8 fk[32], want[32];
    hkdf_expand_label(t->s_hs, "finished", NULL, 0, fk, 32);
    hmac_sha256(fk, 32, th_before, 32, want);
    if (len != 32) return fail(t, "bad Finished from the server");
    u8 diff = 0;
    for (int i = 0; i < 32; i++) diff |= want[i] ^ m[i];
    if (diff) return fail(t, "the server's Finished did not verify");
    return 0;
}

static int client_finish(tls_t *t) {
    /* application secrets use the transcript through the server Finished */
    u8 th[32], derived[32], empty[32], zero[32] = { 0 }, master[32], c_ap[32], s_ap[32];
    transcript(t, th);
    sha256("", 0, empty);
    hkdf_expand_label(t->hs_secret, "derived", empty, 32, derived, 32);
    hkdf_extract(derived, 32, zero, 32, master);
    hkdf_expand_label(master, "c ap traffic", th, 32, c_ap, 32);
    hkdf_expand_label(master, "s ap traffic", th, 32, s_ap, 32);

    u8 ccs = 1;
    send_record(t, CT_CCS, &ccs, 1);                  /* middlebox compatibility (still plaintext) */
    set_keys(&t->wr, t->c_hs);
    if (t->cert_requested) send_handshake(t, HS_CERTIFICATE, (const u8 *)"\0\0\0\0", 4);   /* no client certificate */
    u8 fk[32], vd[32], th2[32];
    transcript(t, th2);
    hkdf_expand_label(t->c_hs, "finished", NULL, 0, fk, 32);
    hmac_sha256(fk, 32, th2, 32, vd);
    send_handshake(t, HS_FINISHED, vd, 32);
    set_keys(&t->wr, c_ap);
    set_keys(&t->rd, s_ap);
    t->state = ST_OPEN;
    return 0;
}

/* one complete handshake message */
static int handle_hs(tls_t *t, const u8 *msg, usize total) {
    u8 type = msg[0];
    const u8 *m = msg + 4;
    usize len = total - 4;
    u8 th_before[32];
    transcript(t, th_before);
    if (t->state != ST_OPEN) sha256_update(&t->tr, msg, total);
    switch (t->state) {
    case ST_HELLO:
        if (type != HS_SERVER_HELLO) return fail(t, "expected ServerHello");
        return server_hello(t, m, len);
    case ST_EE:
        if (type != HS_ENCRYPTED_EXTENSIONS) return fail(t, "expected EncryptedExtensions");
        t->state = ST_CERT;
        return 0;
    case ST_CERT:
        if (type == HS_CERTIFICATE_REQUEST) { t->cert_requested = 1; return 0; }
        if (type != HS_CERTIFICATE) return fail(t, "expected the server certificate");
        t->state = ST_CV;                                /* not verified: see tls.h */
        return 0;
    case ST_CV:
        if (type != HS_CERTIFICATE_VERIFY) return fail(t, "expected CertificateVerify");
        t->state = ST_FIN;
        return 0;
    case ST_FIN:
        if (type != HS_FINISHED) return fail(t, "expected Finished");
        if (finished(t, m, len, th_before)) return -1;
        return client_finish(t);
    case ST_OPEN:
        if (type == HS_NEW_SESSION_TICKET) return 0;      /* no resumption */
        if (type == HS_KEY_UPDATE && len == 1) {
            u8 next[32];
            hkdf_expand_label(t->rd.secret, "traffic upd", NULL, 0, next, 32);
            set_keys(&t->rd, next);
            if (m[0] == 1) {                             /* update_requested: answer, then move our side */
                u8 no = 0;
                send_handshake(t, HS_KEY_UPDATE, &no, 1);
                hkdf_expand_label(t->wr.secret, "traffic upd", NULL, 0, next, 32);
                set_keys(&t->wr, next);
            }
            return 0;
        }
        return 0;
    }
    return fail(t, "unexpected handshake message");
}

/* one complete record */
static int handle_record(tls_t *t, u8 type, u8 *body, usize len) {
    if (type == CT_CCS && t->state != ST_OPEN) return 0;
    u8 *pt = body;
    usize plen = len;
    if (t->rd.on) {
        if (type != CT_APPDATA || len < 17) return fail(t, "unencrypted record after the handshake started");
        u8 iv[12];
        nonce(&t->rd, iv);
        pt = kalloc(len);
        if (gcm_decrypt(&t->rd.aes, iv, body - 5, 5, body, len - 16, pt, body + len - 16)) { kfree(pt); return fail(t, "a record failed to decrypt"); }
        t->rd.seq++;
        plen = len - 16;
        while (plen && !pt[plen - 1]) plen--;            /* padding */
        if (!plen) { kfree(pt); return fail(t, "empty inner record"); }
        type = pt[--plen];
    }
    int r = 0;
    if (type == CT_HANDSHAKE) {
        buf_put(&t->hs, pt, plen);
        while (t->hs.len >= 4 && r == 0) {
            usize ml = rd24(t->hs.p + 1);
            if (ml > 65536) { r = fail(t, "handshake message too long"); break; }
            if (t->hs.len < 4 + ml) break;
            r = handle_hs(t, t->hs.p, 4 + ml);    /* key changes fall on record boundaries (RFC 8446 5.1) */
            buf_drop(&t->hs, 4 + ml);
        }
    } else if (type == CT_APPDATA) {
        if (t->state != ST_OPEN) r = fail(t, "data before the handshake finished");
        else buf_put(&t->app, pt, plen);
    } else if (type == CT_ALERT) {
        if (plen >= 2 && pt[1] == 0) t->eof = 1;          /* close_notify */
        else {
            char m[64];
            fmt(m, sizeof m, "the server sent alert %u", plen >= 2 ? pt[1] : 0);
            r = fail(t, m);
        }
    }
    if (pt != body) kfree(pt);
    return r;
}

/* ---- public ------------------------------------------------------------------------------ */
tls_t *tls_new(const tls_io_t *io, const char *host) {
    tls_t *t = kalloc(sizeof *t);
    t->io = *io;
    strlcpy(t->host, host, sizeof t->host);
    random_bytes(t->priv, 32);
    x25519_base(t->pub, t->priv);
    sha256_init(&t->tr);
    client_hello(t);
    t->state = ST_HELLO;
    return t;
}

int tls_poll(tls_t *t) {
    if (t->state == ST_FAILED) return -1;
    flush(t);
    u8 tmp[4096];
    for (int budget = 0; budget < 64 && t->state != ST_FAILED; budget++) {
        int n = t->io.recv(t->io.ctx, tmp, sizeof tmp);
        if (n < 0) { if (t->state != ST_OPEN) return fail(t, "the server closed the connection during the handshake"); t->eof = 1; break; }
        if (n == 0) break;
        buf_put(&t->in, tmp, (usize)n);
    }
    while (t->in.len >= 5 && t->state != ST_FAILED) {
        usize rl = rd16(t->in.p + 3);
        if (rl > 16384 + 256) return fail(t, "oversized record");
        if (t->in.len < 5 + rl) break;
        int r = handle_record(t, t->in.p[0], t->in.p + 5, rl);
        buf_drop(&t->in, 5 + rl);
        if (r) return -1;
    }
    flush(t);
    if (t->state == ST_FAILED) return -1;
    return t->state == ST_OPEN;
}

int tls_write(tls_t *t, const void *data, usize len) {
    if (t->state != ST_OPEN) return -1;
    send_record(t, CT_APPDATA, data, len);
    flush(t);
    return t->state == ST_FAILED ? -1 : (int)len;
}

int tls_read(tls_t *t, void *buf, usize len) {
    usize n = MIN(len, t->app.len);
    if (!n) return 0;
    memcpy(buf, t->app.p, n);
    buf_drop(&t->app, n);
    return (int)n;
}

int tls_eof(tls_t *t) { return t->eof && !t->app.len; }
const char *tls_error(tls_t *t) { return t->err[0] ? t->err : "TLS error"; }

void tls_free(tls_t *t) {
    if (!t) return;
    buf_free(&t->out); buf_free(&t->in); buf_free(&t->hs); buf_free(&t->app);
    memset(t, 0, sizeof *t);
    kfree(t);
}
