/*
 * l2cap.c - L2CAP channels over the ACL link (Bluetooth Core, vol 3 part A), and the
 * small SDP server headphones expect to find (vol 3 part B).
 *
 * Basic mode only: a channel is opened with a Connection Request to a PSM (AVDTP is
 * 25), then both sides send a Configuration Request (the only option we use is the
 * MTU) and accept the other's; then it is open.  Channels the other side opens - the
 * headphones' SDP queries, their AVDTP channels when they reconnect by themselves -
 * are accepted for the PSMs someone listens on (l2cap_listen).
 *
 * SDP: one record, an A2DP Audio Source on AVDTP; ServiceSearch,
 * ServiceAttribute and ServiceSearchAttribute requests are answered from it.  Some
 * headphones ask before they agree to stream.
 *
 * Runs in the bluetooth thread (hci.c hands frames over); l2cap_send may also be
 * called from the sound thread for media, which only touches its own open channel.
 */
#include "bt.h"

#define MAX_CH  8
#define OUR_MTU 1024                       /* what we take in (signalling; media only goes out) */

enum { S_FREE, S_WAIT_CONN, S_CONFIG, S_OPEN, S_WAIT_DISC };

struct l2cap_ch {
    int state;
    u16 handle, lcid, rcid, psm;
    int remote_mtu;
    int ours_ok, theirs_ok;                /* our configuration accepted / theirs accepted */
    const l2cap_ops_t *ops;
    void *user;
    u8 ident;                              /* of our pending request */
};

static l2cap_ch_t chans[MAX_CH];
static struct { u16 psm; const l2cap_ops_t *ops; } listeners[4];
static u8 next_ident = 1;
static u16 next_cid = 0x40;

static u8 new_ident(void) { u8 i = next_ident++; if (!next_ident) next_ident = 1; return i; }   /* never 0 */

static u16 le16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static void put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }

void *l2cap_user(l2cap_ch_t *ch) { return ch->user; }
u16 l2cap_handle(l2cap_ch_t *ch) { return ch->handle; }
int l2cap_mtu(l2cap_ch_t *ch) { return ch->remote_mtu; }

static l2cap_ch_t *ch_alloc(u16 handle) {
    for (int i = 0; i < MAX_CH; i++)
        if (chans[i].state == S_FREE) {
            l2cap_ch_t *c = &chans[i];
            memset(c, 0, sizeof *c);
            c->handle = handle;
            c->lcid = next_cid++;
            if (next_cid < 0x40) next_cid = 0x40;
            c->remote_mtu = 672;                                  /* L2CAP's default */
            return c;
        }
    return NULL;
}

static l2cap_ch_t *ch_by_lcid(u16 handle, u16 cid) {
    for (int i = 0; i < MAX_CH; i++) if (chans[i].state != S_FREE && chans[i].handle == handle && chans[i].lcid == cid) return &chans[i];
    return NULL;
}

static void ch_free(l2cap_ch_t *c, int tell) {
    int was = c->state;
    c->state = S_FREE;
    if (tell && was != S_FREE && c->ops && c->ops->closed) c->ops->closed(c);
}

/* ---- signalling -------------------------------------------------------------------------- */
static void sig_send(u16 handle, u8 code, u8 ident, const u8 *d, int len) {
    u8 f[4 + 4 + 64];
    if (len > 64) return;
    put16(f, (u16)(4 + len)); put16(f + 2, 0x0001);
    f[4] = code; f[5] = ident; put16(f + 6, (u16)len);
    if (len) memcpy(f + 8, d, (usize)len);
    hci_send_acl(handle, f, 8 + len);
}

static void send_config(l2cap_ch_t *c) {
    u8 d[8];
    put16(d, c->rcid); put16(d + 2, 0);                           /* flags: complete */
    d[4] = 0x01; d[5] = 2; put16(d + 6, OUR_MTU);                 /* MTU option */
    c->ident = new_ident();
    sig_send(c->handle, 0x04, c->ident, d, 8);
}

static void maybe_open(l2cap_ch_t *c) {
    if (c->state == S_CONFIG && c->ours_ok && c->theirs_ok) {
        c->state = S_OPEN;
        klog("bt: L2CAP channel %04x (PSM %u) open, MTU %d", c->lcid, c->psm, c->remote_mtu);
        if (c->ops && c->ops->open) c->ops->open(c);
    }
}

static void signal_cmd(u16 handle, u8 code, u8 ident, const u8 *d, int len) {
    switch (code) {
    case 0x01:                                                    /* Command Reject: our request failed */
        for (int i = 0; i < MAX_CH; i++)
            if (chans[i].state != S_FREE && chans[i].handle == handle && chans[i].ident == ident && chans[i].state != S_OPEN) ch_free(&chans[i], 1);
        break;
    case 0x02: {                                                  /* Connection Request */
        if (len < 4) break;
        u16 psm = le16(d), scid = le16(d + 2);
        const l2cap_ops_t *ops = NULL;
        for (int i = 0; i < 4; i++) if (listeners[i].ops && listeners[i].psm == psm) ops = listeners[i].ops;
        u8 r[8];
        l2cap_ch_t *c = ops ? ch_alloc(handle) : NULL;
        put16(r, c ? c->lcid : 0); put16(r + 2, scid);
        put16(r + 4, c ? 0 : ops ? 0x0004 : 0x0002);             /* success / no resources / PSM not supported */
        put16(r + 6, 0);
        sig_send(handle, 0x03, ident, r, 8);
        if (!c) break;
        c->rcid = scid; c->psm = psm; c->ops = ops; c->state = S_CONFIG;
        send_config(c);
        break;
    }
    case 0x03: {                                                  /* Connection Response */
        if (len < 8) break;
        u16 dcid = le16(d), scid = le16(d + 2), result = le16(d + 4);
        l2cap_ch_t *c = ch_by_lcid(handle, scid);
        if (!c || c->state != S_WAIT_CONN) break;
        if (result == 1) break;                                   /* pending */
        if (result) { klog("bt: L2CAP PSM %u refused (%u)", c->psm, result); ch_free(c, 1); break; }
        c->rcid = dcid;
        c->state = S_CONFIG;
        send_config(c);
        break;
    }
    case 0x04: {                                                  /* Configuration Request */
        if (len < 4) break;
        l2cap_ch_t *c = ch_by_lcid(handle, le16(d));
        u8 r[6];
        put16(r, c ? c->rcid : 0); put16(r + 2, 0); put16(r + 4, c ? 0 : 0x0002);   /* success / rejected */
        if (c) for (int o = 4; o + 2 <= len && o + 2 + d[o + 1] <= len; o += 2 + d[o + 1])
            if ((d[o] & 0x7f) == 0x01 && d[o + 1] == 2) c->remote_mtu = le16(d + o + 2);
        sig_send(handle, 0x05, ident, r, 6);
        if (c && !(le16(d + 2) & 1)) { c->theirs_ok = 1; maybe_open(c); }   /* not continued */
        break;
    }
    case 0x05: {                                                  /* Configuration Response */
        if (len < 6) break;
        l2cap_ch_t *c = ch_by_lcid(handle, le16(d));
        if (!c || c->state != S_CONFIG) break;
        if (le16(d + 4) == 0) { c->ours_ok = 1; maybe_open(c); }
        else if (le16(d + 4) == 1) {                              /* unacceptable: try again without options */
            u8 r[4]; put16(r, c->rcid); put16(r + 2, 0);
            c->ident = new_ident();
            sig_send(handle, 0x04, c->ident, r, 4);
        } else ch_free(c, 1);
        break;
    }
    case 0x06: {                                                  /* Disconnection Request */
        if (len < 4) break;
        l2cap_ch_t *c = ch_by_lcid(handle, le16(d));
        sig_send(handle, 0x07, ident, d, 4);
        if (c) ch_free(c, 1);
        break;
    }
    case 0x07: {                                                  /* Disconnection Response */
        if (len < 4) break;
        l2cap_ch_t *c = ch_by_lcid(handle, le16(d + 2));
        if (c) ch_free(c, 1);
        break;
    }
    case 0x08: sig_send(handle, 0x09, ident, d, MIN(len, 64)); break;   /* Echo */
    case 0x0a: {                                                  /* Information Request */
        if (len < 2) break;
        u16 type = le16(d);
        u8 r[12] = { 0 };
        put16(r, type);
        int rl = 4;
        if (type == 1) { put16(r + 2, 0); put16(r + 4, OUR_MTU); rl = 6; }            /* connectionless MTU */
        else if (type == 2) { put16(r + 2, 0); rl = 8; }                               /* extended features: none */
        else put16(r + 2, 1);                                                           /* not supported */
        sig_send(handle, 0x0b, ident, r, rl);
        break;
    }
    }
}

/* ---- frames -------------------------------------------------------------------------------- */
void l2cap_rx(u16 handle, const u8 *f, int len) {
    if (len < 4) return;
    int l = le16(f);
    u16 cid = le16(f + 2);
    if (4 + l > len) return;
    const u8 *p = f + 4;
    if (cid == 0x0001) {
        for (int o = 0; o + 4 <= l; ) {
            int cl = le16(p + o + 2);
            if (o + 4 + cl > l) break;
            signal_cmd(handle, p[o], p[o + 1], p + o + 4, cl);
            o += 4 + cl;
        }
        return;
    }
    l2cap_ch_t *c = ch_by_lcid(handle, cid);
    if (c && c->state == S_OPEN && c->ops && c->ops->data) c->ops->data(c, p, l);
}

int l2cap_send(l2cap_ch_t *c, const u8 *p, int len) {
    if (!c || c->state != S_OPEN || len > c->remote_mtu) return -1;
    static u8 f[4 + 2048];
    if (len > 2048) return -1;
    put16(f, (u16)len); put16(f + 2, c->rcid);
    memcpy(f + 4, p, (usize)len);
    return hci_send_acl(c->handle, f, 4 + len);
}

l2cap_ch_t *l2cap_connect(u16 handle, u16 psm, const l2cap_ops_t *ops, void *user) {
    l2cap_ch_t *c = ch_alloc(handle);
    if (!c) return NULL;
    c->psm = psm; c->ops = ops; c->user = user;
    c->state = S_WAIT_CONN;
    c->ident = new_ident();
    u8 d[4];
    put16(d, psm); put16(d + 2, c->lcid);
    sig_send(handle, 0x02, c->ident, d, 4);
    return c;
}

void l2cap_close(l2cap_ch_t *c) {
    if (!c || c->state == S_FREE) return;
    if (c->state == S_WAIT_DISC) return;
    u8 d[4];
    put16(d, c->rcid); put16(d + 2, c->lcid);
    c->state = S_WAIT_DISC;
    c->ident = new_ident();
    sig_send(c->handle, 0x06, c->ident, d, 4);
}

void l2cap_listen(u16 psm, const l2cap_ops_t *ops) {
    for (int i = 0; i < 4; i++) if (!listeners[i].ops || listeners[i].psm == psm) { listeners[i].psm = psm; listeners[i].ops = ops; return; }
}

void l2cap_link_up(u16 handle) { (void)handle; }

void l2cap_link_down(u16 handle) {
    for (int i = 0; i < MAX_CH; i++) if (chans[i].state != S_FREE && chans[i].handle == handle) ch_free(&chans[i], 1);
}

/* ---- SDP: one record, an A2DP Audio Source ------------------------------------------------------ */
static const u8 rec_attrs[][20] = {
    /* attribute id (2), then its data element */
    { 0x00, 0x00, 0x0a, 0x00, 0x01, 0x00, 0x00 },                                                  /* handle 0x10000 */
    { 0x00, 0x01, 0x35, 0x03, 0x19, 0x11, 0x0a },                                                  /* AudioSource */
    { 0x00, 0x04, 0x35, 0x10, 0x35, 0x06, 0x19, 0x01, 0x00, 0x09, 0x00, 0x19, 0x35, 0x06, 0x19, 0x00, 0x19, 0x09, 0x01, 0x03 },   /* L2CAP 25, AVDTP 1.3 */
    { 0x00, 0x05, 0x35, 0x03, 0x19, 0x10, 0x02 },                                                  /* public browse group */
    { 0x00, 0x09, 0x35, 0x08, 0x35, 0x06, 0x19, 0x11, 0x0d, 0x09, 0x01, 0x03 },                    /* A2DP 1.3 */
    { 0x03, 0x11, 0x09, 0x00, 0x01 },                                                              /* supported features: player */
};
static const int rec_len[] = { 7, 7, 20, 7, 12, 5 };

/* the UUIDs of a search pattern; 1 if one names our record */
static int pattern_matches(const u8 *p, int len) {
    for (int i = 0; i < len; ) {
        u8 h = p[i];
        if (h == 0x35) { i += 2; continue; }                       /* sequences: look inside */
        if (h == 0x36) { i += 3; continue; }
        u32 u = 0;
        if (h == 0x19 && i + 3 <= len) { u = (u32)(p[i + 1] << 8 | p[i + 2]); i += 3; }
        else if (h == 0x1a && i + 5 <= len) { u = (u32)p[i + 1] << 24 | (u32)p[i + 2] << 16 | (u32)p[i + 3] << 8 | p[i + 4]; i += 5; }
        else if (h == 0x1c && i + 17 <= len) { u = (u32)(p[i + 3] << 8 | p[i + 4]); i += 17; }
        else return 0;
        if (u == 0x110a || u == 0x110d || u == 0x0100 || u == 0x0019 || u == 0x1002) return 1;
    }
    return 0;
}

/* the attributes in the requested id list, as one data element sequence */
static int attr_list(const u8 *ids, int idlen, u8 *out, int cap) {
    int o = 2;
    for (int a = 0; a < 6; a++) {
        u16 id = (u16)(rec_attrs[a][0] << 8 | rec_attrs[a][1]);
        int want = 0;
        for (int i = 0; i < idlen; ) {
            u8 h = ids[i];
            if (h == 0x35) { i += 2; continue; }
            if (h == 0x09 && i + 3 <= idlen) { want |= (u16)(ids[i + 1] << 8 | ids[i + 2]) == id; i += 3; }
            else if (h == 0x0a && i + 5 <= idlen) {
                u16 lo = (u16)(ids[i + 1] << 8 | ids[i + 2]), hi = (u16)(ids[i + 3] << 8 | ids[i + 4]);
                want |= id >= lo && id <= hi;
                i += 5;
            } else break;
        }
        if (!want || o + 1 + rec_len[a] > cap) continue;
        out[o++] = 0x09;
        memcpy(out + o, rec_attrs[a], (usize)rec_len[a]);
        o += rec_len[a];
    }
    out[0] = 0x35; out[1] = (u8)(o - 2);
    return o;
}

/* a data element's total length (header + data) at p */
static int de_len(const u8 *p, int avail) {
    if (avail < 1) return avail + 1;
    u8 h = p[0];
    int sz = h & 7;
    if ((h >> 3) == 0) return 1;
    if (sz < 5) return 1 + (1 << sz);
    if (sz == 5) return avail >= 2 ? 2 + p[1] : 99999;
    if (sz == 6) return avail >= 3 ? 3 + (p[1] << 8 | p[2]) : 99999;
    return 99999;
}

static void sdp_data(l2cap_ch_t *c, const u8 *p, int len) {
    if (len < 5) return;
    u8 pdu = p[0];
    u8 r[256];
    int o = 5;
    const u8 *q = p + 5;
    int ql = len - 5;
    if (pdu == 0x06 || pdu == 0x02) {                              /* ServiceSearch(Attribute)Request */
        int pl = de_len(q, ql);
        if (pl > ql) goto bad;
        int match = pattern_matches(q, pl);
        if (pdu == 0x02) {
            r[o++] = 0; r[o++] = (u8)match; r[o++] = 0; r[o++] = (u8)match;
            if (match) { r[o++] = 0x00; r[o++] = 0x01; r[o++] = 0x00; r[o++] = 0x00; }
        } else {
            const u8 *ids = q + pl + 2;
            int il = de_len(ids, ql - pl - 2);
            if (pl + 2 + il > ql) goto bad;
            u8 lists[200];
            int n = 2;
            if (match) n += attr_list(ids, il, lists + n, (int)sizeof lists - n);
            lists[0] = 0x35; lists[1] = (u8)(n - 2);
            r[o++] = (u8)(n >> 8); r[o++] = (u8)n;
            memcpy(r + o, lists, (usize)n); o += n;
        }
    } else if (pdu == 0x04) {                                      /* ServiceAttributeRequest */
        if (ql < 7) goto bad;
        const u8 *ids = q + 6;
        int il = de_len(ids, ql - 6);
        if (6 + il > ql) goto bad;
        u8 list[200];
        int n = attr_list(ids, il, list, sizeof list);
        r[o++] = (u8)(n >> 8); r[o++] = (u8)n;
        memcpy(r + o, list, (usize)n); o += n;
    } else goto bad;
    r[o++] = 0;                                                    /* no continuation */
    r[0] = (u8)(pdu + 1); r[1] = p[1]; r[2] = p[2];
    r[3] = (u8)((o - 5) >> 8); r[4] = (u8)(o - 5);
    l2cap_send(c, r, o);
    return;
bad:
    r[0] = 0x01; r[1] = p[1]; r[2] = p[2]; r[3] = 0; r[4] = 2; r[5] = 0x00; r[6] = 0x03;   /* invalid request syntax */
    l2cap_send(c, r, 7);
}

static const l2cap_ops_t sdp_ops = { NULL, sdp_data, NULL };
void sdp_init(void) { l2cap_listen(0x0001, &sdp_ops); }
