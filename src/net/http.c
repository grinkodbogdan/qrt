/*
 * http.c - a small HTTP/1.1 client for the browser.
 *
 * GET and form POST, one request per connection ("Connection: close"), identity
 * encoding, chunked transfer decoding, up to 8 redirects, 8 MB bodies.
 * http:// runs on QRT's TCP directly, https:// through tls.c.  Everything
 * is a state machine driven by http_poll(), so the shell never blocks.
 */
#include "http.h"
#ifndef HTTP_HOST_TEST
#include "net.h"
#include "netstack.h"
#include "tls.h"
#endif

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int ci_prefix(const char *s, const char *p) { while (*p) if (lower(*s++) != lower(*p++)) return 0; return 1; }

/* ---- URLs ------------------------------------------------------------------------- */
int url_parse(const char *s, url_t *u) {
    memset(u, 0, sizeof *u);
    while (*s == ' ') s++;
    if (ci_prefix(s, "https://")) { u->https = 1; s += 8; }
    else if (ci_prefix(s, "http://")) s += 7;
    else if (strstr(s, "://")) return -1;                  /* another scheme */
    usize n = 0;
    while (*s && *s != '/' && *s != ':' && *s != '?' && *s != '#') { if (n + 1 < sizeof u->host) u->host[n++] = (char)lower(*s); s++; }
    u->host[n] = 0;
    if (!n) return -1;
    u->port = u->https ? 443 : 80;
    if (*s == ':') {
        u32 p = 0;
        s++;
        while (*s >= '0' && *s <= '9') p = p * 10 + (u32)(*s++ - '0');
        if (!p || p > 65535) return -1;
        u->port = (u16)p;
    }
    n = 0;
    if (*s != '/') u->path[n++] = '/';
    while (*s && *s != '#' && n + 1 < sizeof u->path) {
        char c = *s++;
        if (c == ' ') { if (n + 3 < sizeof u->path) { u->path[n++] = '%'; u->path[n++] = '2'; u->path[n++] = '0'; } }
        else u->path[n++] = c;
    }
    u->path[n] = 0;
    return 0;
}

void url_format(const url_t *u, char *out, usize cap) {
    int def = u->port == (u->https ? 443 : 80);
    if (def) fmt(out, cap, "%s://%s%s", u->https ? "https" : "http", u->host, u->path);
    else fmt(out, cap, "%s://%s:%u%s", u->https ? "https" : "http", u->host, u->port, u->path);
}

void url_resolve(const char *base, const char *ref, char *out, usize cap) {
    while (*ref == ' ' || *ref == '\t' || *ref == '\n' || *ref == '\r') ref++;
    if (ci_prefix(ref, "http://") || ci_prefix(ref, "https://")) { strlcpy(out, ref, cap); return; }
    url_t b;
    if (url_parse(base, &b)) { strlcpy(out, ref, cap); return; }
    if (ref[0] == '/' && ref[1] == '/') { fmt(out, cap, "%s:%s", b.https ? "https" : "http", ref); return; }
    char path[2048];
    if (ref[0] == '/') strlcpy(path, ref, sizeof path);
    else if (ref[0] == '?') {
        strlcpy(path, b.path, sizeof path);
        char *q = strchr(path, '?');
        if (q) *q = 0;
        strlcat(path, ref, sizeof path);
    } else if (ref[0] == '#' || !ref[0]) strlcpy(path, b.path, sizeof path);
    else {
        strlcpy(path, b.path, sizeof path);
        char *q = strchr(path, '?');
        if (q) *q = 0;
        char *slash = strrchr(path, '/');
        if (slash) slash[1] = 0; else strlcpy(path, "/", sizeof path);
        strlcat(path, ref, sizeof path);
    }
    char *hash = strchr(path, '#');
    if (hash) *hash = 0;
    /* remove "./" and "seg/../" (RFC 3986 5.2.4, enough for real pages) */
    char clean[2048];
    usize o = 0;
    const char *qs = strchr(path, '?');
    usize plen = qs ? (usize)(qs - path) : strlen(path);
    usize i = 0;
    while (i < plen) {
        if (path[i] == '/' && i + 2 <= plen && path[i + 1] == '.' && (i + 2 == plen || path[i + 2] == '/')) { i += 2; if (i == plen) clean[o++] = '/'; continue; }
        if (path[i] == '/' && i + 3 <= plen && path[i + 1] == '.' && path[i + 2] == '.' && (i + 3 == plen || path[i + 3] == '/')) {
            while (o > 0 && clean[o - 1] != '/') o--;
            if (o > 0) o--;
            i += 3;
            if (i == plen) clean[o++] = '/';
            continue;
        }
        if (o + 1 < sizeof clean) clean[o++] = path[i];
        i++;
    }
    if (!o) clean[o++] = '/';
    clean[o] = 0;
    if (qs) strlcat(clean, qs, sizeof clean);
    usize w = 0;
    for (const char *c = clean; *c && w + 4 < sizeof b.path; c++) {
        if (*c == ' ') { b.path[w++] = '%'; b.path[w++] = '2'; b.path[w++] = '0'; }
        else b.path[w++] = *c;
    }
    b.path[w] = 0;
    url_format(&b, out, cap);
}

void url_encode_component(const char *in, char *out, usize cap) {
    static const char hex[] = "0123456789ABCDEF";
    usize o = 0;
    for (; *in && o + 4 < cap; in++) {
        u8 c = (u8)*in;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') out[o++] = (char)c;
        else if (c == ' ') out[o++] = '+';
        else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
    }
    out[o] = 0;
}

/* ---- response parsing ---------------------------------------------------------------- */
static void header_value(const char *line, usize len, usize name_len, char *out, usize cap) {
    usize i = name_len;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    usize e = len;
    while (e > i && (line[e - 1] == ' ' || line[e - 1] == '\t')) e--;
    usize n = MIN(e - i, cap - 1);
    memcpy(out, line + i, n);
    out[n] = 0;
}

int http_parse_head(const u8 *buf, usize len, http_head_t *h) {
    const char *b = (const char *)buf;
    usize end = 0;
    for (usize i = 0; i + 3 < len; i++) if (b[i] == '\r' && b[i + 1] == '\n' && b[i + 2] == '\r' && b[i + 3] == '\n') { end = i + 4; break; }
    if (!end) return len > 65536 ? -1 : 0;
    memset(h, 0, sizeof *h);
    h->content_length = -1;
    h->header_len = end;
    if (len < 12 || !ci_prefix(b, "HTTP/1.")) return -1;
    h->code = (b[9] - '0') * 100 + (b[10] - '0') * 10 + (b[11] - '0');
    usize i = 0;
    while (i < end && b[i] != '\n') i++;
    i++;
    while (i < end) {
        usize s = i;
        while (i < end && b[i] != '\r') i++;
        usize l = i - s;
        const char *line = b + s;
        char v[64];
        if (l > 13 && ci_prefix(line, "content-type:")) header_value(line, l, 13, h->content_type, sizeof h->content_type);
        else if (l > 9 && ci_prefix(line, "location:")) header_value(line, l, 9, h->location, sizeof h->location);
        else if (l > 17 && ci_prefix(line, "content-encoding:")) header_value(line, l, 17, h->encoding, sizeof h->encoding);
        else if (l > 15 && ci_prefix(line, "content-length:")) {
            header_value(line, l, 15, v, sizeof v);
            long n = 0;
            for (char *p = v; *p >= '0' && *p <= '9'; p++) n = n * 10 + (*p - '0');
            h->content_length = n;
        } else if (l > 18 && ci_prefix(line, "transfer-encoding:")) {
            header_value(line, l, 18, v, sizeof v);
            for (char *p = v; *p; p++) if (ci_prefix(p, "chunked")) h->chunked = 1;
        }
        i += 2;
    }
    return 1;
}

long http_dechunk(u8 *body, usize len, int *complete) {
    usize r = 0, w = 0;
    *complete = 0;
    for (;;) {
        u64 size = 0;
        int digits = 0;
        while (r < len) {
            u8 c = body[r];
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) break;
            size = size * 16 + (u64)d;
            digits++;
            r++;
        }
        while (r < len && body[r] != '\n') r++;             /* chunk extensions */
        if (r >= len) return (long)w;                        /* size line not complete yet */
        r++;
        if (!digits) return -1;
        if (size == 0) { *complete = 1; return (long)w; }
        if (size > len - r) return (long)w;                  /* cut short: only whole chunks count */
        memmove(body + w, body + r, size);
        w += size;
        r += size;
        if (r < len && body[r] == '\r') r++;
        if (r < len && body[r] == '\n') r++;
    }
}

#ifndef HTTP_HOST_TEST
/* ---- the fetch state machine -------------------------------------------------------------- */
enum { H_RESOLVE, H_CONNECT, H_TLS, H_SEND, H_RECV, H_DONE, H_FAILED };
#define MAX_BODY (8u << 20)

struct http {
    url_t u;
    char url[2300], err[160], progress[160];
    int state, dns_q, sock, redirects, secure;
    u32 ip;
    tls_t *tls;
    u8 *buf; usize len, cap;
    http_head_t head;
    u64 phase_ms;
    const u8 *body; usize body_len;
    char *post;                           /* form body; dropped after a 301/302/303 redirect */
};

static int io_send(void *ctx, const u8 *d, usize n) {
    int s = *(int *)ctx;
    int st = tcp_state(s);
    if (st != TCP_ESTABLISHED && st != TCP_CLOSE_WAIT) return -1;
    return tcp_send(s, d, n);
}
static int io_recv(void *ctx, u8 *d, usize n) {
    int s = *(int *)ctx;
    int r = tcp_recv(s, d, n);
    if (r > 0) return r;
    if (tcp_peer_closed(s)) return -1;
    int st = tcp_state(s);
    if (st == TCP_CLOSED || st == TCP_FAILED) return -1;
    return 0;
}

static int h_fail(http_t *h, const char *why) { strlcpy(h->err, why, sizeof h->err); h->state = H_FAILED; return -1; }

static void close_conn(http_t *h) {
    if (h->tls) { tls_free(h->tls); h->tls = NULL; }
    if (h->sock >= 0) { tcp_abort(h->sock); h->sock = -1; }
}

static void start(http_t *h) {
    url_format(&h->u, h->url, sizeof h->url);
    h->len = 0;
    h->secure = h->u.https;
    h->phase_ms = k_now_ms();
    if (str_to_ip(h->u.host, &h->ip)) { h->state = H_CONNECT; h->sock = -1; return; }   /* an IP address: no DNS */
    h->dns_q = dns_start(h->u.host);
    h->state = h->dns_q < 0 ? H_FAILED : H_RESOLVE;
    if (h->dns_q < 0) h_fail(h, "no network: connect to Wi-Fi first");
    fmt(h->progress, sizeof h->progress, "Looking up %s...", h->u.host);
}

http_t *http_post(const char *url, const char *form_body) {
    http_t *h = kalloc(sizeof *h);
    h->sock = -1;
    if (form_body) {
        usize n = strlen(form_body);
        h->post = kalloc(n + 1);
        memcpy(h->post, form_body, n + 1);
    }
    if (url_parse(url, &h->u)) { h_fail(h, "QRT's browser opens http:// and https:// addresses only"); strlcpy(h->url, url, sizeof h->url); return h; }
    net_lock();
    if (!net_primary()) { net_unlock(); h_fail(h, "no network: connect to Wi-Fi first"); url_format(&h->u, h->url, sizeof h->url); return h; }
    start(h);
    net_unlock();
    return h;
}

http_t *http_get(const char *url) { return http_post(url, NULL); }

static void add_bytes(http_t *h, const u8 *d, usize n) {
    if (h->len + n > h->cap) {
        usize cap = MAX(h->cap * 2, h->len + n + 16384);
        cap = MIN(cap, (usize)MAX_BODY + 65536);
        if (h->len + n > cap) return;
        u8 *p = kalloc(cap);
        if (h->len) memcpy(p, h->buf, h->len);
        if (h->buf) kfree(h->buf);
        h->buf = p; h->cap = cap;
    }
    memcpy(h->buf + h->len, d, n);
    h->len += n;
}

/* the response is complete: decode, follow a redirect or finish */
static int finish(http_t *h) {
    close_conn(h);
    http_head_t *hd = &h->head;
    if (http_parse_head(h->buf, h->len, hd) != 1) return h_fail(h, "the server's answer was not HTTP");
    if ((hd->code == 301 || hd->code == 302 || hd->code == 303 || hd->code == 307 || hd->code == 308) && hd->location[0]) {
        if (++h->redirects > 8) return h_fail(h, "too many redirects");
        if (h->post && hd->code != 307 && hd->code != 308) { kfree(h->post); h->post = NULL; }   /* becomes a GET */
        char next[2300];
        url_resolve(h->url, hd->location, next, sizeof next);
        if (url_parse(next, &h->u)) return h_fail(h, "redirected to an address QRT cannot open");
        start(h);
        return h->state == H_FAILED ? -1 : 0;
    }
    u8 *body = h->buf + hd->header_len;
    usize bl = h->len - hd->header_len;
    if (hd->chunked) {
        int complete;
        long n = http_dechunk(body, bl, &complete);
        if (n < 0) return h_fail(h, "bad chunked encoding");
        bl = (usize)n;
    } else if (hd->content_length >= 0 && (usize)hd->content_length < bl) bl = (usize)hd->content_length;
    if (hd->encoding[0] && !ci_prefix(hd->encoding, "identity")) {
        fmt(h->err, sizeof h->err, "the page came %s-compressed, which QRT cannot unpack yet", hd->encoding);
        h->state = H_FAILED;
        return -1;
    }
    h->body = body;
    h->body_len = bl;
    h->state = H_DONE;
    return 1;
}

/* has everything arrived without waiting for the close? */
static int complete_early(http_t *h) {
    http_head_t hd;
    if (http_parse_head(h->buf, h->len, &hd) != 1) return 0;
    usize bl = h->len - hd.header_len;
    if (hd.content_length >= 0) return bl >= (usize)hd.content_length;
    if (hd.chunked && bl >= 5) return !memcmp(h->buf + h->len - 5, "0\r\n\r\n", 5);
    return hd.code == 204 || hd.code == 304;
}

int http_poll(http_t *h) {
    if (h->state == H_DONE) return 1;
    if (h->state == H_FAILED) return -1;
    u64 now = k_now_ms();
    if (now - h->phase_ms > 30000) { net_lock(); close_conn(h); net_unlock(); return h_fail(h, "timed out"); }
    net_lock();
    int r = 0;
    switch (h->state) {
    case H_RESOLVE: {
        u32 ip;
        int d = dns_result(h->dns_q, &ip);
        if (d < 0 || (d == 1 && !ip)) { fmt(h->err, sizeof h->err, "%s: no such site (DNS)", h->u.host); h->state = H_FAILED; r = -1; break; }
        if (d == 1) {
            h->ip = ip; h->state = H_CONNECT; h->sock = -1; h->phase_ms = now;
            char ips[20];
            ip_to_str(ip, ips, sizeof ips);
            klog("http: %s is %s", h->u.host, ips);
        }
        break;
    }
    case H_CONNECT:
        if (h->sock < 0) {
            h->sock = tcp_connect(h->ip, h->u.port);
            if (h->sock < 0) { r = h_fail(h, "out of connections"); break; }
            char ips[20];
            ip_to_str(h->ip, ips, sizeof ips);
            fmt(h->progress, sizeof h->progress, "Connecting to %s (%s)...", h->u.host, ips);
        }
        if (tcp_state(h->sock) == TCP_ESTABLISHED) {
            h->phase_ms = now;
            if (h->u.https) {
                tls_io_t io = { io_send, io_recv, &h->sock };
                h->tls = tls_new(&io, h->u.host);
                h->state = H_TLS;
                fmt(h->progress, sizeof h->progress, "Securing the connection to %s (TLS 1.3)...", h->u.host);
            } else h->state = H_SEND;
        } else if (tcp_state(h->sock) == TCP_FAILED || tcp_state(h->sock) == TCP_CLOSED) {
            fmt(h->err, sizeof h->err, "%s refused the connection or did not answer", h->u.host);
            close_conn(h);
            h->state = H_FAILED; r = -1;
        }
        break;
    case H_TLS: {
        int t = tls_poll(h->tls);
        if (t < 0) { fmt(h->err, sizeof h->err, "secure connection failed: %s", tls_error(h->tls)); close_conn(h); h->state = H_FAILED; r = -1; }
        else if (t == 1) { h->state = H_SEND; h->phase_ms = now; }
        break;
    }
    case H_SEND: {
        usize plen = h->post ? strlen(h->post) : 0;
        usize cap = 2800 + plen;
        char *req = kalloc(cap);
        int def = h->u.port == (h->u.https ? 443 : 80);
        char hostport[300], extra[96] = "";
        if (def) strlcpy(hostport, h->u.host, sizeof hostport); else fmt(hostport, sizeof hostport, "%s:%u", h->u.host, h->u.port);
        if (h->post) fmt(extra, sizeof extra, "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: %u\r\n", (u32)plen);
        int n = fmt(req, cap, "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Mozilla/5.0 (QRT " QRT_VERSION "; Tessera) QRTBrowser/0.1\r\n"
                    "Accept: text/html, text/plain;q=0.9, */*;q=0.5\r\nAccept-Encoding: identity\r\nAccept-Language: en\r\n%sConnection: close\r\n\r\n",
                    h->post ? "POST" : "GET", h->u.path, hostport, extra);
        if (h->post) { memcpy(req + n, h->post, plen); n += (int)plen; }
        /* TCP may take part of it now and the rest later; TLS queues it all */
        int sent = h->tls ? tls_write(h->tls, req, (usize)n) : tcp_send(h->sock, req, (usize)n);
        kfree(req);
        if (sent != n) { r = h_fail(h, "could not send the request"); close_conn(h); break; }
        h->state = H_RECV;
        h->phase_ms = now;
        fmt(h->progress, sizeof h->progress, "Waiting for %s...", h->u.host);
        break;
    }
    case H_RECV: {
        u8 tmp[8192];
        int eof = 0;
        for (int budget = 0; budget < 32; budget++) {
            int n;
            if (h->tls) {
                if (tls_poll(h->tls) < 0 && !tls_eof(h->tls)) {
                    if (h->len) { eof = 1; break; }             /* some servers just drop the connection */
                    fmt(h->err, sizeof h->err, "secure connection failed: %s", tls_error(h->tls)); close_conn(h); h->state = H_FAILED; r = -1; break;
                }
                n = tls_read(h->tls, tmp, sizeof tmp);
                if (!n && tls_eof(h->tls)) { eof = 1; break; }
            } else {
                n = io_recv(&h->sock, tmp, sizeof tmp);
                if (n < 0) { eof = 1; break; }
            }
            if (n <= 0) break;
            add_bytes(h, tmp, (usize)n);
            h->phase_ms = now;
            if (h->len > MAX_BODY) { eof = 1; break; }
        }
        if (h->state == H_FAILED) break;
        fmt(h->progress, sizeof h->progress, "Receiving from %s: %u KB", h->u.host, (u32)(h->len / 1024));
        if (eof || complete_early(h)) r = finish(h);
        break;
    }
    }
    net_unlock();
    return r;
}

const char *http_progress(http_t *h) { return h->progress; }
const char *http_error(http_t *h) { return h->err; }
int  http_code(http_t *h) { return h->head.code; }
const char *http_url(http_t *h) { return h->url; }
const char *http_content_type(http_t *h) { return h->head.content_type; }
const u8 *http_body(http_t *h, usize *len) { *len = h->body_len; return h->body; }
int  http_secure(http_t *h) { return h->secure; }

void http_free(http_t *h) {
    if (!h) return;
    net_lock();
    close_conn(h);
    net_unlock();
    if (h->buf) kfree(h->buf);
    if (h->post) kfree(h->post);
    kfree(h);
}
#endif
