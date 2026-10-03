/*
 * net.c - Ethernet, ARP, IPv4, ICMP, UDP, DHCP, DNS and SNTP (TCP is in tcp.c).
 *
 * Deliberately small: one active interface, no IP fragments, no IPv6.
 * Received frames are handled at once; outgoing IP packets wait in a short
 * queue while their next hop's MAC address is resolved.
 */
#include "net.h"
#include "wifilog.h"
#include "crypto.h"
#include "http.h"

#define LOG(...) wifilog("net: " __VA_ARGS__)

u16 be16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }
u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
void put16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
void put32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }

static const u8 bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

#define MAX_IF 4
static netif_t *ifs[MAX_IF];
static int n_ifs;
static u16 ip_id = 1;

void ip_to_str(u32 ip, char *b, usize cap) { fmt(b, cap, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255); }

int str_to_ip(const char *s, u32 *ip) {
    u32 v = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return 0;
        u32 n = 0;
        while (*s >= '0' && *s <= '9') { n = n * 10 + (u32)(*s++ - '0'); if (n > 255) return 0; }
        v = v << 8 | n;
        if (part < 3) { if (*s != '.') return 0; s++; }
    }
    if (*s) return 0;
    *ip = v;
    return 1;
}

u16 net_checksum(const void *data, usize len, u32 sum) {
    const u8 *p = data;
    for (; len > 1; len -= 2, p += 2) sum += (u32)(p[0] << 8 | p[1]);
    if (len) sum += (u32)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (u16)~sum;
}

void net_register(netif_t *n) { if (n_ifs < MAX_IF) ifs[n_ifs++] = n; }

netif_t *net_primary(void) {
    for (int i = 0; i < n_ifs; i++) if (ifs[i]->link && ifs[i]->ip) return ifs[i];
    return NULL;
}
static netif_t *net_up(void) {
    for (int i = 0; i < n_ifs; i++) if (ifs[i]->link) return ifs[i];
    return NULL;
}

/* ---- Ethernet + ARP ------------------------------------------------------------- */
static int eth_send(netif_t *n, const u8 dst[6], u16 type, const u8 *payload, usize len) {
    u8 f[1600];
    if (len > 1500) return -1;
    memcpy(f, dst, 6);
    memcpy(f + 6, n->mac, 6);
    put16(f + 12, type);
    memcpy(f + 14, payload, len);
    usize total = 14 + len;
    if (total < 60) { memset(f + total, 0, 60 - total); total = 60; }
    return n->send(n, f, total);
}

typedef struct { u32 ip; u8 mac[6]; u64 expires; } arp_entry_t;
static arp_entry_t arp[16];

typedef struct { int used; u32 hop; u64 since, last_req; u8 pkt[1500]; usize len; } pending_t;
static pending_t pending[8];

static arp_entry_t *arp_find(u32 ip) {
    u64 now = k_now_ms();
    for (usize i = 0; i < ARRAY_LEN(arp); i++) if (arp[i].ip == ip && arp[i].expires > now) return &arp[i];
    return NULL;
}

static void arp_request(netif_t *n, u32 target) {
    u8 a[28];
    put16(a, 1); put16(a + 2, 0x0800); a[4] = 6; a[5] = 4; put16(a + 6, 1);
    memcpy(a + 8, n->mac, 6); put32(a + 14, n->ip);
    memset(a + 18, 0, 6); put32(a + 24, target);
    eth_send(n, bcast, 0x0806, a, 28);
}

static void arp_learn(u32 ip, const u8 mac[6]) {
    u64 now = k_now_ms();
    arp_entry_t *e = NULL;
    for (usize i = 0; i < ARRAY_LEN(arp); i++) if (arp[i].ip == ip) e = &arp[i];
    if (!e) {
        e = &arp[0];
        for (usize i = 1; i < ARRAY_LEN(arp); i++) if (arp[i].expires < e->expires) e = &arp[i];
    }
    e->ip = ip;
    memcpy(e->mac, mac, 6);
    e->expires = now + 10 * 60 * 1000;
    netif_t *n = net_up();
    for (usize i = 0; i < ARRAY_LEN(pending); i++)
        if (pending[i].used && pending[i].hop == ip) {
            if (n) eth_send(n, mac, 0x0800, pending[i].pkt, pending[i].len);
            pending[i].used = 0;
        }
}

static void arp_input(netif_t *n, const u8 *a, usize len) {
    if (len < 28 || be16(a) != 1 || be16(a + 2) != 0x0800 || a[4] != 6 || a[5] != 4) return;
    u16 op = be16(a + 6);
    u32 sip = be32(a + 14), tip = be32(a + 24);
    if (sip) arp_learn(sip, a + 8);
    if (op == 1 && n->ip && tip == n->ip) {
        u8 r[28];
        put16(r, 1); put16(r + 2, 0x0800); r[4] = 6; r[5] = 4; put16(r + 6, 2);
        memcpy(r + 8, n->mac, 6); put32(r + 14, n->ip);
        memcpy(r + 18, a + 8, 6); put32(r + 24, sip);
        eth_send(n, a + 8, 0x0806, r, 28);
    }
}

/* ---- IPv4 ---------------------------------------------------------------------- */
int ip_output(u32 dst, u8 proto, const u8 *payload, usize len) {
    netif_t *n = net_up();
    if (!n || len > 1480) return -1;
    u8 pkt[1500];
    u8 *h = pkt;
    h[0] = 0x45; h[1] = 0;
    put16(h + 2, (u16)(20 + len));
    put16(h + 4, ip_id++);
    put16(h + 6, 0x4000);                       /* don't fragment */
    h[8] = 64; h[9] = proto;
    put16(h + 10, 0);
    put32(h + 12, n->ip);
    put32(h + 16, dst);
    put16(h + 10, net_checksum(h, 20, 0));
    memcpy(pkt + 20, payload, len);
    usize total = 20 + len;
    if (dst == 0xffffffffu || (n->mask && (dst | n->mask) == 0xffffffffu))
        return eth_send(n, bcast, 0x0800, pkt, total);
    u32 hop = (!n->mask || (dst & n->mask) == (n->ip & n->mask)) ? dst : n->gw;
    if (!hop) return -1;
    arp_entry_t *e = arp_find(hop);
    if (e) return eth_send(n, e->mac, 0x0800, pkt, total);
    /* queue until ARP answers */
    pending_t *p = NULL;
    for (usize i = 0; i < ARRAY_LEN(pending); i++) if (!pending[i].used) { p = &pending[i]; break; }
    if (!p) return -1;
    p->used = 1; p->hop = hop; p->since = p->last_req = k_now_ms();
    memcpy(p->pkt, pkt, total);
    p->len = total;
    arp_request(n, hop);
    return 0;
}

static void icmp_input(netif_t *n, u32 src, const u8 *p, usize len);
static void udp_input(netif_t *n, u32 src, u32 dst, const u8 *p, usize len);
void tcp_input(u32 src, u32 dst, const u8 *p, usize len);
void tcp_poll(void);

static void ip_input(netif_t *n, const u8 *h, usize len) {
    if (len < 20 || (h[0] >> 4) != 4) return;
    usize ihl = (usize)(h[0] & 15) * 4, total = be16(h + 2);
    if (ihl < 20 || total > len || total < ihl) return;
    if (net_checksum(h, ihl, 0) != 0) return;
    if (be16(h + 6) & 0x3fff) return;               /* fragments: not supported */
    u32 src = be32(h + 12), dst = be32(h + 16);
    int for_us = dst == n->ip || dst == 0xffffffffu || !n->ip || (n->mask && (dst | n->mask) == 0xffffffffu);
    if (!for_us) return;
    const u8 *p = h + ihl;
    usize plen = total - ihl;
    switch (h[9]) {
    case 1: icmp_input(n, src, p, plen); break;
    case 6: tcp_input(src, dst, p, plen); break;
    case 17: udp_input(n, src, dst, p, plen); break;
    }
}

void net_input(netif_t *n, const u8 *f, usize len) {
    if (len < 14) return;
    if (memcmp(f, n->mac, 6) && memcmp(f, bcast, 6) && !(f[0] & 1)) return;
    u16 type = be16(f + 12);
    if (type == 0x0806) arp_input(n, f + 14, len - 14);
    else if (type == 0x0800) ip_input(n, f + 14, len - 14);
}

/* ---- ICMP ------------------------------------------------------------------------ */
static ping_fn on_reply[4];
void net_on_echo_reply(ping_fn fn) {
    for (int i = 0; i < 4; i++) if (on_reply[i] == fn) return;
    for (int i = 0; i < 4; i++) if (!on_reply[i]) { on_reply[i] = fn; return; }
}

static void icmp_input(netif_t *n, u32 src, const u8 *p, usize len) {
    if (len < 8 || net_checksum(p, len, 0) != 0) return;
    if (p[0] == 8 && n->ip) {                        /* echo request: answer */
        u8 r[1480];
        if (len > sizeof r) return;
        memcpy(r, p, len);
        r[0] = 0;
        put16(r + 2, 0);
        put16(r + 2, net_checksum(r, len, 0));
        ip_output(src, 1, r, len);
    } else if (p[0] == 0) {
        for (int i = 0; i < 4; i++) if (on_reply[i]) on_reply[i](src, be16(p + 4), be16(p + 6), p + 8, len - 8);
    }
}

int net_ping(u32 dst, u16 id, u16 seq, const void *data, usize len) {
    u8 m[1480];
    if (len + 8 > sizeof m) return -1;
    m[0] = 8; m[1] = 0;
    put16(m + 2, 0);
    put16(m + 4, id);
    put16(m + 6, seq);
    memcpy(m + 8, data, len);
    put16(m + 2, net_checksum(m, len + 8, 0));
    return ip_output(dst, 1, m, len + 8);
}

/* ---- UDP -------------------------------------------------------------------------- */
typedef struct { u16 port; udp_fn fn; void *ctx; } udp_bind_t;
static udp_bind_t binds[16];
static u16 next_ephemeral = 49152;

int udp_bind(u16 port, udp_fn fn, void *ctx) {
    for (usize i = 0; i < ARRAY_LEN(binds); i++) if (binds[i].port == port) return -1;
    for (usize i = 0; i < ARRAY_LEN(binds); i++)
        if (!binds[i].port) { binds[i] = (udp_bind_t){ port, fn, ctx }; return 0; }
    return -1;
}
void udp_unbind(u16 port) { for (usize i = 0; i < ARRAY_LEN(binds); i++) if (binds[i].port == port) binds[i].port = 0; }

u16 udp_ephemeral(void) {
    for (int tries = 0; tries < 16384; tries++) {
        u16 p = next_ephemeral++;
        if (next_ephemeral < 49152) next_ephemeral = 49152;
        int used = 0;
        for (usize i = 0; i < ARRAY_LEN(binds); i++) if (binds[i].port == p) used = 1;
        if (!used) return p;
    }
    return 0;
}

u32 net_pseudo_sum(u32 src, u32 dst, u8 proto, usize len) {
    return (src >> 16) + (src & 0xffff) + (dst >> 16) + (dst & 0xffff) + proto + (u32)len;
}

int udp_send(u32 dst, u16 sport, u16 dport, const void *data, usize len) {
    netif_t *n = net_up();
    if (!n || len + 8 > 1480) return -1;
    u8 u[1480];
    put16(u, sport); put16(u + 2, dport); put16(u + 4, (u16)(len + 8)); put16(u + 6, 0);
    memcpy(u + 8, data, len);
    u16 c = net_checksum(u, len + 8, net_pseudo_sum(n->ip, dst, 17, len + 8));
    put16(u + 6, c ? c : 0xffff);
    return ip_output(dst, 17, u, len + 8);
}

static void udp_input(netif_t *n, u32 src, u32 dst, const u8 *p, usize len) {
    if (len < 8) return;
    usize ulen = be16(p + 4);
    if (ulen < 8 || ulen > len) return;
    if (be16(p + 6) && net_checksum(p, ulen, net_pseudo_sum(src, dst, 17, ulen)) != 0) return;
    u16 dport = be16(p + 2);
    for (usize i = 0; i < ARRAY_LEN(binds); i++)
        if (binds[i].port == dport) { binds[i].fn(binds[i].ctx, src, be16(p), p + 8, ulen - 8); return; }
}

/* ---- DHCP client --------------------------------------------------------------------- */
enum { DH_OFF, DH_SELECTING, DH_REQUESTING, DH_BOUND };
static struct {
    netif_t *n;
    int state, tries;
    u32 xid, offered, server;
    u64 next_ms, t1_ms;
} dh;

static void dhcp_send(int type) {
    u8 m[300];
    memset(m, 0, sizeof m);
    m[0] = 1; m[1] = 1; m[2] = 6;
    put32(m + 4, dh.xid);
    put16(m + 10, 0x8000);                           /* broadcast replies: we have no address yet */
    if (dh.state == DH_BOUND) put32(m + 12, dh.n->ip);
    memcpy(m + 28, dh.n->mac, 6);
    put32(m + 236, 0x63825363);
    u8 *o = m + 240;
    *o++ = 53; *o++ = 1; *o++ = (u8)type;
    *o++ = 61; *o++ = 7; *o++ = 1; memcpy(o, dh.n->mac, 6); o += 6;
    if (type == 3 && dh.state == DH_REQUESTING) {
        *o++ = 50; *o++ = 4; put32(o, dh.offered); o += 4;
        *o++ = 54; *o++ = 4; put32(o, dh.server); o += 4;
    }
    *o++ = 12; *o++ = 3; memcpy(o, "qrt", 3); o += 3;
    *o++ = 55; *o++ = 5; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 51; *o++ = 54;
    *o++ = 255;
    usize len = (usize)(o - m);
    if (len < 300) len = 300;
    udp_send(0xffffffffu, 68, 67, m, len);
}

static void dhcp_input(void *ctx, u32 src, u16 sport, const u8 *m, usize len) {
    (void)ctx; (void)src; (void)sport;
    if (!dh.n || len < 240 || m[0] != 2 || be32(m + 4) != dh.xid || be32(m + 236) != 0x63825363) return;
    int type = 0;
    u32 mask = 0, router = 0, dns = 0, server = 0, lease = 3600;
    for (usize i = 240; i < len;) {
        u8 opt = m[i];
        if (opt == 255) break;
        if (opt == 0) { i++; continue; }
        if (i + 1 >= len) break;
        u8 l = m[i + 1];
        const u8 *v = m + i + 2;
        if (i + 2 + l > len) break;
        if (opt == 53 && l >= 1) type = v[0];
        if (opt == 1 && l >= 4) mask = be32(v);
        if (opt == 3 && l >= 4) router = be32(v);
        if (opt == 6 && l >= 4) dns = be32(v);
        if (opt == 54 && l >= 4) server = be32(v);
        if (opt == 51 && l >= 4) lease = be32(v);
        i += 2u + l;
    }
    u32 yiaddr = be32(m + 16);
    if (type == 2 && dh.state == DH_SELECTING) {           /* OFFER */
        dh.offered = yiaddr;
        dh.server = server;
        dh.state = DH_REQUESTING;
        dh.tries = 0;
        dhcp_send(3);
        dh.next_ms = k_now_ms() + 3000;
    } else if (type == 5 && (dh.state == DH_REQUESTING || dh.state == DH_BOUND)) {   /* ACK */
        netif_t *n = dh.n;
        int fresh = n->ip != yiaddr;
        n->ip = yiaddr;
        n->mask = mask ? mask : 0xffffff00u;
        n->gw = router;
        n->dns = dns ? dns : router;
        n->lease_until_ms = k_now_ms() + (u64)lease * 1000;
        dh.state = DH_BOUND;
        dh.t1_ms = k_now_ms() + (u64)lease * 500;
        if (fresh) {
            char a[16], g[16], d[16];
            ip_to_str(n->ip, a, sizeof a); ip_to_str(n->gw, g, sizeof g); ip_to_str(n->dns, d, sizeof d);
            LOG("%s: address %s, gateway %s, DNS %s, lease %u s", n->name, a, g, d, lease);
        }
    } else if (type == 6) {                                  /* NAK */
        LOG("DHCP server refused the address; starting over");
        dh.n->ip = 0;
        dh.state = DH_SELECTING;
        dh.tries = 0;
        dh.next_ms = k_now_ms();
    }
}

static void dhcp_start(netif_t *n) {
    dh.n = n;
    n->ip = n->mask = n->gw = n->dns = 0;
    dh.state = DH_SELECTING;
    dh.tries = 0;
    random_bytes((u8 *)&dh.xid, 4);
    dh.next_ms = k_now_ms();
    udp_unbind(68);
    udp_bind(68, dhcp_input, NULL);
}

static void dhcp_poll(void) {
    if (!dh.n || dh.state == DH_OFF) return;
    u64 now = k_now_ms();
    if (!dh.n->link) { dh.state = DH_OFF; return; }
    if (dh.state == DH_BOUND) {
        if (now >= dh.t1_ms) { dh.t1_ms = now + 60000; dhcp_send(3); }   /* renew */
        return;
    }
    if (now < dh.next_ms) return;
    if (dh.tries++ >= 5) { dh.state = DH_SELECTING; dh.tries = 0; dh.next_ms = now + 10000; LOG("no DHCP answer; retrying in 10 s"); return; }
    if (dh.state == DH_SELECTING) { if (dh.tries == 1) LOG("%s: asking for an address (DHCP)", dh.n->name); dhcp_send(1); }
    else dhcp_send(3);
    dh.next_ms = now + 2000u * (u32)dh.tries;
}

void net_link_changed(netif_t *n) {
    if (n->link) { memset(arp, 0, sizeof arp); dhcp_start(n); }
    else {
        n->ip = n->mask = n->gw = n->dns = 0;
        if (dh.n == n) dh.state = DH_OFF;
    }
}

/* ---- DNS -------------------------------------------------------------------------------- */
typedef struct { int used, done; u16 id, port; char name[128]; u32 ip; int tries; u64 next_ms; } dnsq_t;
static dnsq_t dq[8];

static void dns_input(void *ctx, u32 src, u16 sport, const u8 *m, usize len) {
    dnsq_t *q = ctx;
    (void)src; (void)sport;
    if (!q->used || q->done || len < 12 || be16(m) != q->id || !(m[2] & 0x80)) return;
    int rcode = m[3] & 15, qd = be16(m + 4), an = be16(m + 6);
    usize i = 12;
    for (int k = 0; k < qd && i < len; k++) {                  /* skip questions */
        while (i < len && m[i]) { if ((m[i] & 0xc0) == 0xc0) { i++; break; } i += (usize)m[i] + 1; }
        i += 5;
    }
    q->ip = 0;
    for (int k = 0; k < an && i + 10 <= len; k++) {
        while (i < len && m[i]) { if ((m[i] & 0xc0) == 0xc0) { i++; break; } i += (usize)m[i] + 1; }
        i++;
        if (i + 10 > len) break;
        u16 type = be16(m + i), cls = be16(m + i + 2), rdl = be16(m + i + 8);
        i += 10;
        if (i + rdl > len) break;
        if (type == 1 && cls == 1 && rdl == 4) { q->ip = be32(m + i); break; }
        i += rdl;
    }
    if (rcode && !q->ip) LOG("DNS: %s: error %d", q->name, rcode);
    q->done = 1;
    udp_unbind(q->port);
}

static void dns_send(dnsq_t *q) {
    netif_t *n = net_primary();
    if (!n || !n->dns) return;
    u8 m[300];
    memset(m, 0, 12);
    put16(m, q->id); m[2] = 0x01; put16(m + 4, 1);
    usize o = 12;
    const char *s = q->name;
    while (*s && o < 280) {
        const char *dot = strchr(s, '.');
        usize l = dot ? (usize)(dot - s) : strlen(s);
        if (!l || l > 63) break;
        m[o++] = (u8)l;
        memcpy(m + o, s, l);
        o += l;
        s += l;
        if (*s == '.') s++;
    }
    m[o++] = 0;
    put16(m + o, 1); put16(m + o + 2, 1);
    o += 4;
    udp_send(n->dns, q->port, 53, m, o);
}

int dns_start(const char *name) {
    for (int i = 0; i < (int)ARRAY_LEN(dq); i++) {
        dnsq_t *q = &dq[i];
        if (q->used) continue;
        memset(q, 0, sizeof *q);
        q->used = 1;
        strlcpy(q->name, name, sizeof q->name);
        random_bytes((u8 *)&q->id, 2);
        q->port = udp_ephemeral();
        if (!q->port || udp_bind(q->port, dns_input, q)) { q->used = 0; return -1; }
        u32 lit;
        if (str_to_ip(name, &lit)) { q->ip = lit; q->done = 1; udp_unbind(q->port); return i; }
        dns_send(q);
        q->tries = 1;
        q->next_ms = k_now_ms() + 2000;
        return i;
    }
    return -1;
}

int dns_result(int i, u32 *ip) {
    if (i < 0 || i >= (int)ARRAY_LEN(dq) || !dq[i].used) return -1;
    if (!dq[i].done) return 0;
    *ip = dq[i].ip;
    dq[i].used = 0;
    return 1;
}

static void dns_poll(void) {
    u64 now = k_now_ms();
    for (usize i = 0; i < ARRAY_LEN(dq); i++) {
        dnsq_t *q = &dq[i];
        if (!q->used || q->done || now < q->next_ms) continue;
        if (q->tries >= 4) { q->done = 1; q->ip = 0; udp_unbind(q->port); LOG("DNS: %s: no answer", q->name); continue; }
        dns_send(q);
        q->tries++;
        q->next_ms = now + 2000;
    }
}

/* ---- SNTP: the wall clock from the network (RFC 4330) --------------------------------------- *
 * Certificates are checked against the clock, so https:// needs the right date.  Once an
 * interface has an address: ask the servers in turn (every 5 s until one answers), then again
 * every 6 hours. */
static const char *const ntp_servers[] = { "pool.ntp.org", "time.google.com", "time.cloudflare.com" };
static struct {
    int state;                 /* 0 idle, 1 resolving, 2 waiting for the answer */
    int server, dq, misses;    /* misses: servers in a row that did not answer */
    http_t *http;              /* the fallback: an HTTP server's Date: header */
    int want_http;
    u16 port;
    u8 sent[8];                /* our transmit timestamp, echoed back as the originate one */
    u64 next_ms, sent_ms;
} ntp;

static void ntp_input(void *ctx, u32 src, u16 sport, const u8 *m, usize len) {
    (void)ctx; (void)src;
    if (ntp.state != 2 || sport != 123 || len < 48) return;
    int mode = m[0] & 7, stratum = m[1];
    if (mode != 4 || stratum == 0 || stratum > 15 || memcmp(m + 24, ntp.sent, 8)) return;   /* not our answer, or kiss-o'-death */
    u64 secs = be32(m + 40), frac = be32(m + 44);
    if (secs < 2208988800ull) return;
    u64 rtt = k_now_ms() - ntp.sent_ms;
    u64 utc = secs - 2208988800ull + ((frac * 1000 >> 32) + rtt / 2) / 1000;   /* the server's time, plus half the round trip */
    udp_unbind(ntp.port);
    ntp.state = 0;
    ntp.misses = 0;
    ntp.next_ms = k_now_ms() + 6 * 3600 * 1000ull;
    time_set_utc(utc, ntp_servers[ntp.server]);
}

/* some networks block NTP: a plain-HTTP reply's Date: header is good to a second or two.
 * http.c takes the network lock itself, so this runs from netstack_poll() outside it. */
#define DATE_URL "http://www.google.com/generate_204"
void net_time_http_poll(void) {
    if (ntp.want_http && !ntp.http) {
        ntp.want_http = 0;
        if (net_primary()) ntp.http = http_get(DATE_URL);
    }
    if (!ntp.http) return;
    int r = net_primary() ? http_poll(ntp.http) : -1;
    if (r == 0) return;
    long long t = r > 0 ? http_date_parse(http_date(ntp.http)) : -1;
    if (t <= 0) LOG("SNTP: the HTTP date failed too: %s", r > 0 ? "no usable Date header" : http_error(ntp.http));
    http_free(ntp.http);
    ntp.http = NULL;
    if (t > 0) {
        ntp.misses = 0;
        ntp.next_ms = k_now_ms() + 6 * 3600 * 1000ull;
        time_set_utc((u64)t, "an HTTP Date header");
    } else ntp.next_ms = k_now_ms() + 30000;
}

static void ntp_poll(void) {
    u64 now = k_now_ms();
    netif_t *n = net_primary();
    if (!n || !n->ip) {
        if (ntp.state == 2) udp_unbind(ntp.port);
        ntp.state = 0;
        return;
    }
    if (ntp.http || ntp.want_http) return;
    /* not set yet: one silent server is enough to try HTTP too (https:// waits on the date) */
    if (ntp.state == 0 && now >= ntp.next_ms && ntp.misses >= (time_synced() ? (int)ARRAY_LEN(ntp_servers) : 1)) {
        ntp.misses = 0;
        if (!time_synced()) { LOG("SNTP: no answer; asking an HTTP server for the date"); ntp.want_http = 1; }
        else ntp.next_ms = now + 6 * 3600 * 1000ull;
        return;
    }
    if (ntp.state == 0 && now >= ntp.next_ms) {
        ntp.dq = dns_start(ntp_servers[ntp.server]);
        ntp.state = ntp.dq >= 0 ? 1 : 0;
        ntp.next_ms = now + 5000;
    } else if (ntp.state == 1) {
        u32 ip = 0;
        int r = dns_result(ntp.dq, &ip);
        if (r == 0) return;
        ntp.state = 0;
        if (r < 0 || !ip) { ntp.server = (ntp.server + 1) % (int)ARRAY_LEN(ntp_servers); ntp.misses++; return; }
        ntp.port = udp_ephemeral();
        if (!ntp.port || udp_bind(ntp.port, ntp_input, NULL)) return;
        u8 m[48];
        memset(m, 0, sizeof m);
        m[0] = 0x23;                                     /* version 4, client */
        random_bytes(ntp.sent, sizeof ntp.sent);         /* a nonce for the transmit timestamp */
        memcpy(m + 40, ntp.sent, 8);
        ntp.sent_ms = now;
        udp_send(ip, ntp.port, 123, m, sizeof m);
        ntp.state = 2;
        ntp.next_ms = now + 5000;
    } else if (ntp.state == 2 && now >= ntp.next_ms) {   /* no answer: the next server */
        udp_unbind(ntp.port);
        ntp.state = 0;
        ntp.server = (ntp.server + 1) % (int)ARRAY_LEN(ntp_servers);
        ntp.misses++;
    }
}

/* ---- poll + status ------------------------------------------------------------------------ */
void net_poll(void) {
    u64 now = k_now_ms();
    netif_t *n = net_up();
    for (usize i = 0; i < ARRAY_LEN(pending); i++) {
        pending_t *p = &pending[i];
        if (!p->used) continue;
        if (now - p->since > 3000 || !n) { p->used = 0; continue; }
        if (now - p->last_req > 1000) { p->last_req = now; arp_request(n, p->hop); }
    }
    dhcp_poll();
    dns_poll();
    ntp_poll();
    tcp_poll();
}

const char *net_status(void) {
    static char buf[96];
    netif_t *n = net_up();
    if (!n) return NULL;
    char a[16];
    if (n->ip) { ip_to_str(n->ip, a, sizeof a); fmt(buf, sizeof buf, "%s%s%s  \xc2\xb7  %s", n->name, n->detail[0] ? " " : "", n->detail, a); }
    else fmt(buf, sizeof buf, "%s%s%s  \xc2\xb7  getting an address...", n->name, n->detail[0] ? " " : "", n->detail);
    return buf;
}
