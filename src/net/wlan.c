/*
 * wlan.c - the 802.11 client on top of the iwm driver.
 *
 * What net80211 does for iwm(4) in OpenBSD, cut down to one use: a station
 * joining an open or WPA2-Personal (CCMP) network.
 *   - scan results from beacons and probe responses;
 *   - open-system authentication and association (legacy rates, no QoS);
 *   - the EAPOL-Key 4-way and group-key handshakes (IEEE 802.11-2016 12.7),
 *     PMK = PBKDF2(passphrase, SSID); pairwise CCMP runs in the card,
 *     group-key frames (broadcasts: ARP, DHCP) are decrypted here;
 *   - Ethernet <-> 802.11 data frames (LLC/SNAP), as a netif for net.c.
 * Not supported: WEP, WPA1/TKIP, WPA3/SAE, 802.1X (Enterprise),
 * management frame protection, hidden networks.
 */
#include "wlan.h"
#include "net.h"
#include "crypto.h"
#include "wifilog.h"
#include "../drivers/wifi.h"

#define LOG(...) wifilog("wlan: " __VA_ARGS__)

typedef struct {
    u8 bssid[6];
    char ssid[33];
    int channel, rssi, security;
    u16 capinfo, beacon_int;
    u8 rates[16]; int n_rates;
    u8 basic[16]; int n_basic;
    u8 dtim_period;
    u64 tsf; u32 rstamp;
    u8 group_cipher;
    u64 seen_ms;
} bss_t;

#define MAX_BSS 64
static bss_t bss[MAX_BSS];
static int n_bss;

static const u8 oui_ieee[3] = { 0x00, 0x0f, 0xac };

static struct {
    int state, powered;
    bss_t cur;
    u64 deadline, last_beacon, handshake_deadline;
    int tries;
    int rsn;                          /* WPA2 in use */
    u8 pmk[32], ptk[48], anonce[32], snonce[32];
    u64 replay;                       /* last accepted replay counter (+1 so 0 = none) */
    int have_ptk_candidate;
    u8 gtk[16]; int gtk_id, have_gtk;
    u64 gtk_pn;
    u8 rsn_ie[22];
    char want_ssid[33];
    netif_t nif;
    char state_text[96];
    u8 eapol_ver;
} w;

const char *wlan_security_name(int s) {
    static const char *n[] = { "open", "WEP", "WPA (TKIP)", "WPA2", "WPA2-Enterprise", "WPA3", "WPA2 (TKIP group key)" };
    return s >= 0 && s < (int)ARRAY_LEN(n) ? n[s] : "?";
}
int wlan_supported(int s) { return s == SEC_OPEN || s == SEC_WPA2_PSK; }

/* ---- frames out ---------------------------------------------------------------------- */
static u16 le16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }

static int mgmt_send(u8 subtype, const u8 *body, usize blen) {
    u8 f[512];
    if (blen + 24 > sizeof f) return -1;
    memset(f, 0, 24);
    f[0] = subtype;                                   /* type 0 (management) */
    memcpy(f + 4, w.cur.bssid, 6);
    memcpy(f + 10, wifi_macaddr(), 6);
    memcpy(f + 16, w.cur.bssid, 6);
    memcpy(f + 24, body, blen);
    return wifi_tx(f, 24 + blen, 1);
}

static int data_send(const u8 dst[6], u16 ethertype, const u8 *payload, usize len, int protect) {
    u8 f[1600];
    if (len > 1500) return -1;
    memset(f, 0, 24);
    f[0] = 0x08;                                      /* data */
    f[1] = 0x01 | (protect ? 0x40 : 0);               /* to the distribution system */
    memcpy(f + 4, w.cur.bssid, 6);
    memcpy(f + 10, wifi_macaddr(), 6);
    memcpy(f + 16, dst, 6);
    static const u8 snap[6] = { 0xaa, 0xaa, 0x03, 0, 0, 0 };
    memcpy(f + 24, snap, 6);
    f[30] = (u8)(ethertype >> 8); f[31] = (u8)ethertype;
    memcpy(f + 32, payload, len);
    return wifi_tx(f, 32 + len, 0);
}

/* the netif's transmit: Ethernet frame in, 802.11 data frame out */
static int nif_send(netif_t *n, const u8 *eth, usize len) {
    (void)n;
    if (w.state != WL_CONNECTED || len < 14) return -1;
    return data_send(eth, (u16)(eth[12] << 8 | eth[13]), eth + 14, len - 14, w.rsn);
}

/* ---- scan results -------------------------------------------------------------------- */
static void parse_rsn(bss_t *b, const u8 *ie, int len) {
    /* version, group cipher, pairwise ciphers, AKMs, capabilities */
    if (len < 2 + 4) { b->security = SEC_UNSUPPORTED_CIPHER; return; }
    const u8 *p = ie + 2, *end = ie + len;
    int group = memcmp(p, oui_ieee, 3) ? -1 : p[3];
    p += 4;
    int ccmp = 0, psk = 0, sae = 0, eap = 0;
    if (p + 2 <= end) {
        int n = le16(p); p += 2;
        for (int i = 0; i < n && p + 4 <= end; i++, p += 4) if (!memcmp(p, oui_ieee, 3) && p[3] == 4) ccmp = 1;
    } else ccmp = group == 4;
    if (p + 2 <= end) {
        int n = le16(p); p += 2;
        for (int i = 0; i < n && p + 4 <= end; i++, p += 4) {
            if (memcmp(p, oui_ieee, 3)) continue;
            if (p[3] == 2) psk = 1;
            if (p[3] == 8) sae = 1;
            if (p[3] == 1) eap = 1;
        }
    }
    int mfp_required = 0;
    if (p + 2 <= end) mfp_required = (le16(p) >> 6) & 1;
    b->group_cipher = (u8)group;
    if (psk && ccmp && !mfp_required) b->security = group == 4 ? SEC_WPA2_PSK : SEC_UNSUPPORTED_CIPHER;
    else if (sae) b->security = SEC_WPA3;
    else if (eap) b->security = SEC_WPA2_ENTERPRISE;
    else b->security = SEC_UNSUPPORTED_CIPHER;
}

static void scan_result(const u8 *f, usize len, const iwm_rxinfo_t *ri) {
    if (len < 24 + 12) return;
    const u8 *bssid = f + 16, *body = f + 24;
    usize blen = len - 24;
    bss_t tmp;
    memset(&tmp, 0, sizeof tmp);
    memcpy(tmp.bssid, bssid, 6);
    memcpy(&tmp.tsf, body, 8);
    tmp.beacon_int = le16(body + 8);
    tmp.capinfo = le16(body + 10);
    tmp.channel = ri->channel;
    tmp.rssi = ri->rssi;
    tmp.rstamp = ri->rstamp;
    tmp.security = (tmp.capinfo & 0x10) ? SEC_WEP : SEC_OPEN;
    int have_rsn = 0, have_wpa = 0;
    for (usize i = 12; i + 2 <= blen;) {
        u8 id = body[i], l = body[i + 1];
        const u8 *v = body + i + 2;
        if (i + 2 + l > blen) break;
        switch (id) {
        case 0: if (l <= 32) { memcpy(tmp.ssid, v, l); tmp.ssid[l] = 0; } break;
        case 1: case 50:
            for (int k = 0; k < l && tmp.n_rates < 16; k++) {
                tmp.rates[tmp.n_rates++] = v[k];
                if ((v[k] & 0x80) && tmp.n_basic < 16) tmp.basic[tmp.n_basic++] = v[k] & 0x7f;
            }
            break;
        case 3: if (l >= 1) tmp.channel = v[0]; break;
        case 5: if (l >= 2) tmp.dtim_period = v[1]; break;
        case 48: have_rsn = 1; parse_rsn(&tmp, v, l); break;
        case 221: if (l >= 4 && v[0] == 0x00 && v[1] == 0x50 && v[2] == 0xf2 && v[3] == 1) have_wpa = 1; break;
        }
        i += 2u + l;
    }
    if (!have_rsn && have_wpa) tmp.security = SEC_WPA_TKIP;
    if (!tmp.ssid[0]) return;                             /* hidden network */
    tmp.seen_ms = k_now_ms();
    for (int i = 0; i < n_bss; i++)
        if (!memcmp(bss[i].bssid, tmp.bssid, 6)) { bss[i] = tmp; goto current; }
    if (n_bss < MAX_BSS) bss[n_bss++] = tmp;
    else {
        int oldest = 0;
        for (int i = 1; i < n_bss; i++) if (bss[i].seen_ms < bss[oldest].seen_ms) oldest = i;
        bss[oldest] = tmp;
    }
current:
    if (w.state >= WL_AUTH && !memcmp(tmp.bssid, w.cur.bssid, 6)) {
        w.last_beacon = k_now_ms();
        w.cur.tsf = tmp.tsf; w.cur.rstamp = tmp.rstamp; w.cur.rssi = tmp.rssi;
        if (tmp.dtim_period) w.cur.dtim_period = tmp.dtim_period;
    }
}

int wlan_networks(wlan_net_t *out, int max) {
    int n = 0;
    u64 now = k_now_ms();
    for (int i = 0; i < n_bss; i++) {
        if (now - bss[i].seen_ms > 120000) continue;
        int j;
        for (j = 0; j < n; j++) if (!strcmp(out[j].ssid, bss[i].ssid)) break;
        if (j < n) { if (bss[i].rssi > out[j].rssi) { memcpy(out[j].bssid, bss[i].bssid, 6); out[j].rssi = bss[i].rssi; out[j].channel = bss[i].channel; } continue; }
        if (n >= max) continue;
        memcpy(out[n].bssid, bss[i].bssid, 6);
        strlcpy(out[n].ssid, bss[i].ssid, sizeof out[n].ssid);
        out[n].channel = bss[i].channel;
        out[n].rssi = bss[i].rssi;
        out[n].security = bss[i].security;
        out[n].seen_ms = bss[i].seen_ms;
        n++;
    }
    for (int i = 1; i < n; i++)                          /* strongest first */
        for (int j = i; j > 0 && out[j].rssi > out[j - 1].rssi; j--) { wlan_net_t t = out[j]; out[j] = out[j - 1]; out[j - 1] = t; }
    return n;
}

/* ---- association ------------------------------------------------------------------------ */
/* In firmware mode the stick is writable: the log goes to \qrt\hwdump\wifi.txt
 * at every state change, so a hang still leaves a trace of how far it got.
 * Saved from wlan_poll(), after the frame that caused the change has been
 * answered, so the write never delays the answer. */
static int log_dirty;

static void set_state(int s) {
    if (w.state == s) return;
    w.state = s;
    log_dirty = 1;
    static const char *names[] = { "off", "starting", "idle", "scanning", "authenticating", "associating", "handshake", "connected", "failed" };
    LOG("state: %s", names[s]);
}

static void fail(const char *why) {
    LOG("connection failed: %s", why);
    strlcpy(w.state_text, why, sizeof w.state_text);
    wifi_disconnect();
    if (w.nif.link) { w.nif.link = 0; net_link_changed(&w.nif); }
    set_state(WL_FAILED);
}

static void build_rsn_ie(void) {
    static const u8 tmpl[22] = { 48, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 2, 0, 0 };
    memcpy(w.rsn_ie, tmpl, sizeof tmpl);
}

static void send_auth(void) {
    u8 body[6] = { 0, 0, 1, 0, 0, 0 };                   /* open system, sequence 1 */
    mgmt_send(0xb0, body, sizeof body);
    w.deadline = k_now_ms() + 500;
}

static void send_assoc(void) {
    u8 b[256];
    usize o = 0;
    u16 cap = 0x0001;                                     /* ESS */
    if (w.cur.capinfo & 0x0020) cap |= 0x0020;            /* short preamble */
    if (w.cur.capinfo & 0x0400) cap |= 0x0400;            /* short slot time */
    if (w.rsn) cap |= 0x0010;                             /* privacy */
    b[o++] = (u8)cap; b[o++] = (u8)(cap >> 8);
    b[o++] = 10; b[o++] = 0;                               /* listen interval */
    usize sl = strlen(w.cur.ssid);
    b[o++] = 0; b[o++] = (u8)sl; memcpy(b + o, w.cur.ssid, sl); o += sl;
    int nr = MIN(w.cur.n_rates, 8);
    b[o++] = 1; b[o++] = (u8)nr; memcpy(b + o, w.cur.rates, (usize)nr); o += (usize)nr;
    if (w.cur.n_rates > 8) { b[o++] = 50; b[o++] = (u8)(w.cur.n_rates - 8); memcpy(b + o, w.cur.rates + 8, (usize)(w.cur.n_rates - 8)); o += (usize)(w.cur.n_rates - 8); }
    if (w.rsn) { memcpy(b + o, w.rsn_ie, sizeof w.rsn_ie); o += sizeof w.rsn_ie; }
    mgmt_send(0x00, b, o);
    w.deadline = k_now_ms() + 700;
}

static iwm_bss_t driver_bss(void) {
    iwm_bss_t d;
    memset(&d, 0, sizeof d);
    memcpy(d.bssid, w.cur.bssid, 6);
    d.channel = w.cur.channel;
    d.beacon_int = w.cur.beacon_int;
    d.capinfo = w.cur.capinfo;
    d.dtim_period = w.cur.dtim_period ? w.cur.dtim_period : 1;
    d.n_rates = w.cur.n_rates;
    memcpy(d.rates, w.cur.rates, 16);
    d.n_basic = w.cur.n_basic;
    memcpy(d.basic_rates, w.cur.basic, 16);
    d.tsf = w.cur.tsf;
    d.rstamp = w.cur.rstamp;
    d.short_preamble = !!(w.cur.capinfo & 0x20);
    d.short_slot = !!(w.cur.capinfo & 0x400);
    return d;
}

static void connected(void) {
    set_state(WL_CONNECTED);
    w.state_text[0] = 0;
    w.last_beacon = k_now_ms();
    strlcpy(w.nif.detail, w.cur.ssid, sizeof w.nif.detail);
    w.nif.link = 1;
    net_link_changed(&w.nif);
}

/* ---- WPA2: EAPOL-Key ---------------------------------------------------------------------- */
enum { KI_TYPE_PAIRWISE = 1 << 3, KI_INSTALL = 1 << 6, KI_ACK = 1 << 7, KI_MIC = 1 << 8, KI_SECURE = 1 << 9,
       KI_ERROR = 1 << 10, KI_REQUEST = 1 << 11, KI_ENCRYPTED = 1 << 12 };
#define EAPOL_HDR 4
#define KEY_HDR   95          /* type(1) info(2) keylen(2) replay(8) nonce(32) iv(16) rsc(8) reserved(8) mic(16) datalen(2) */

static void derive_ptk(void) {
    u8 data[76];
    const u8 *aa = w.cur.bssid, *spa = wifi_macaddr();
    int a_first = memcmp(aa, spa, 6) < 0;
    memcpy(data, a_first ? aa : spa, 6);
    memcpy(data + 6, a_first ? spa : aa, 6);
    int n_first = memcmp(w.anonce, w.snonce, 32) < 0;
    memcpy(data + 12, n_first ? w.anonce : w.snonce, 32);
    memcpy(data + 44, n_first ? w.snonce : w.anonce, 32);
    prf_sha1(w.pmk, 32, "Pairwise key expansion", data, sizeof data, w.ptk, 48);
}

static void eapol_mic(u8 *frame, usize len, u8 *mic_field) {
    u8 h[20];
    memset(mic_field, 0, 16);
    hmac_sha1(w.ptk, 16, frame, len, h);                    /* KCK = PTK[0..15] */
    memcpy(mic_field, h, 16);
}

static void eapol_send(u16 info, const u8 *replay, const u8 *nonce, const u8 *kdata, usize klen) {
    u8 f[256];
    usize len = EAPOL_HDR + KEY_HDR + klen;
    if (len > sizeof f) return;
    memset(f, 0, len);
    f[0] = w.eapol_ver ? w.eapol_ver : 2;
    f[1] = 3;                                              /* EAPOL-Key */
    put16(f + 2, (u16)(KEY_HDR + klen));
    u8 *k = f + 4;
    k[0] = 2;                                              /* RSN key descriptor */
    put16(k + 1, info);
    put16(k + 3, 0);
    memcpy(k + 5, replay, 8);
    if (nonce) memcpy(k + 13, nonce, 32);
    put16(k + 93, (u16)klen);
    if (klen) memcpy(k + 95, kdata, klen);
    if (info & KI_MIC) eapol_mic(f, len, k + 77);
    static const u8 bc[6] = { 0 };
    (void)bc;
    data_send(w.cur.bssid, 0x888e, f, len, w.have_ptk_candidate == 2);
}

static int find_gtk(const u8 *kd, usize len) {
    for (usize i = 0; i + 2 <= len;) {
        u8 id = kd[i], l = kd[i + 1];
        if (id == 0xdd && l >= 6 && i + 2 + l <= len && !memcmp(kd + i + 2, oui_ieee, 3) && kd[i + 5] == 1) {
            int glen = l - 6;
            if (glen != 16) { LOG("group key of %d bytes: only CCMP (16) is supported", glen); return -1; }
            w.gtk_id = kd[i + 6] & 3;
            memcpy(w.gtk, kd + i + 8, 16);
            w.have_gtk = 1;
            w.gtk_pn = 0;
            return 0;
        }
        if (id == 0xdd && l == 0) break;                  /* padding */
        i += 2u + l;
    }
    return -1;
}

static void eapol_input(const u8 *f, usize len) {
    if (len < EAPOL_HDR + KEY_HDR || f[1] != 3) return;
    const u8 *k = f + 4;
    if (k[0] != 2) return;                                /* not an RSN key */
    u16 info = be16(k + 1);
    if ((info & 7) != 2) { LOG("EAPOL key version %d unsupported", info & 7); return; }
    u64 replay = 0;
    for (int i = 0; i < 8; i++) replay = replay << 8 | k[5 + i];
    usize klen = be16(k + 93);
    if (EAPOL_HDR + KEY_HDR + klen > len) return;
    w.eapol_ver = f[0];
    if (w.replay && replay < w.replay) { LOG("EAPOL: old replay counter"); return; }

    /* MIC check for everything after message 1 */
    if (info & KI_MIC) {
        if (!w.have_ptk_candidate) return;
        u8 copy[512];
        usize total = EAPOL_HDR + KEY_HDR + klen;
        if (total > sizeof copy) return;
        memcpy(copy, f, total);
        u8 want[16];
        memcpy(want, k + 77, 16);
        eapol_mic(copy, total, copy + 4 + 77);
        if (memcmp(copy + 4 + 77, want, 16)) { LOG("EAPOL: bad MIC (wrong password?)"); if (w.state == WL_HANDSHAKE) fail("wrong password"); return; }
    }
    w.replay = replay + 1;

    if ((info & KI_TYPE_PAIRWISE) && (info & KI_ACK) && !(info & KI_MIC)) {        /* message 1 of 4 */
        if (w.state != WL_HANDSHAKE && w.state != WL_CONNECTED) return;
        memcpy(w.anonce, k + 13, 32);
        random_bytes(w.snonce, 32);
        derive_ptk();
        if (!w.have_ptk_candidate) w.have_ptk_candidate = 1;
        eapol_send(2 | KI_TYPE_PAIRWISE | KI_MIC, k + 5, w.snonce, w.rsn_ie, sizeof w.rsn_ie);
        LOG("4-way handshake: message 1 received, 2 sent");
        return;
    }
    if ((info & KI_TYPE_PAIRWISE) && (info & KI_ACK) && (info & KI_MIC) && (info & KI_INSTALL)) {   /* message 3 */
        if (!(info & KI_ENCRYPTED) || klen < 24 || klen % 8) { fail("handshake: unexpected message 3"); return; }
        if (memcmp(k + 13, w.anonce, 32)) { fail("handshake: ANonce changed"); return; }
        u8 kd[256];
        if (klen - 8 > sizeof kd || aes_unwrap(w.ptk + 16, k + 95, klen, kd)) { fail("handshake: key data did not decrypt"); return; }
        if (find_gtk(kd, klen - 8)) LOG("handshake: no group key in message 3");
        eapol_send(2 | KI_TYPE_PAIRWISE | KI_MIC | KI_SECURE, k + 5, NULL, NULL, 0);
        LOG("4-way handshake: message 3 received, 4 sent");
        /* keys become active after message 4 has gone out */
        wifi_poll();
        hal_delay_us(2000);
        if (wifi_set_pairwise_key(w.ptk + 32)) { fail("could not install the key"); return; }
        w.have_ptk_candidate = 2;
        connected();
        return;
    }
    if (!(info & KI_TYPE_PAIRWISE) && (info & KI_ACK) && (info & KI_MIC)) {         /* group key handshake */
        if (!(info & KI_ENCRYPTED) || klen < 24 || klen % 8) return;
        u8 kd[256];
        if (klen - 8 > sizeof kd || aes_unwrap(w.ptk + 16, k + 95, klen, kd)) { LOG("group key did not decrypt"); return; }
        if (find_gtk(kd, klen - 8) == 0) LOG("group key updated (id %d)", w.gtk_id);
        eapol_send(2 | KI_MIC | KI_SECURE, k + 5, NULL, NULL, 0);
    }
}

/* ---- received frames ------------------------------------------------------------------------ */
static int ccmp_group_decrypt(const u8 *f, usize len, u8 *out, usize *olen) {
    if (!w.have_gtk || len < 24 + 8 + 8) return -1;
    const u8 *iv = f + 24;
    if (!(iv[3] & 0x20) || (iv[3] >> 6) != w.gtk_id) return -1;
    u64 pn = (u64)iv[0] | (u64)iv[1] << 8 | (u64)iv[4] << 16 | (u64)iv[5] << 24 | (u64)iv[6] << 32 | (u64)iv[7] << 40;
    if (pn <= w.gtk_pn && w.gtk_pn) return -1;              /* replay */
    u8 nonce[13], aad[22];
    nonce[0] = 0;
    memcpy(nonce + 1, f + 10, 6);                            /* A2 */
    for (int i = 0; i < 6; i++) nonce[7 + i] = (u8)(pn >> (8 * (5 - i)));
    aad[0] = f[0] & 0x8f;
    aad[1] = (u8)((f[1] & ~0x38) | 0x40);
    memcpy(aad + 2, f + 4, 18);                             /* A1, A2, A3 */
    aad[20] = f[22] & 0x0f; aad[21] = 0;
    usize clen = len - 24 - 8 - 8;
    aes128_t a;
    aes128_init(&a, w.gtk);
    if (ccm_decrypt(&a, nonce, aad, sizeof aad, f + 32, clen, out, f + len - 8)) return -1;
    w.gtk_pn = pn;
    *olen = clen;
    return 0;
}

static void data_input(const u8 *f, usize len, const iwm_rxinfo_t *ri) {
    if (!(f[1] & 0x02) || (f[1] & 0x01)) return;              /* FromDS only */
    if (memcmp(f + 10, w.cur.bssid, 6)) return;
    usize hl = 24;
    if (f[0] & 0x80) hl = 26;                                 /* QoS data */
    if (f[0] & 0x40) return;                                  /* null data */
    const u8 *dst = f + 4, *src = f + 16, *body;
    usize blen;
    static u8 plain[2400];
    if (f[1] & 0x40) {                                        /* protected */
        if (ri->decrypted) {
            if (len < hl + 8) return;
            body = f + hl + 8;                                /* skip the CCMP header */
            blen = len - hl - 8;                              /* (a trailing MIC is ignored by the length fields above IP) */
        } else if (dst[0] & 1) {
            if (hl != 24 || len > sizeof plain + 40 || ccmp_group_decrypt(f, len, plain, &blen)) return;
            body = plain;
        } else return;
    } else {
        body = f + hl;
        blen = len - hl;
    }
    if (blen < 8 || body[0] != 0xaa || body[1] != 0xaa || body[2] != 0x03) return;
    u16 type = (u16)(body[6] << 8 | body[7]);
    if (type == 0x888e) { eapol_input(body + 8, blen - 8); return; }
    if (w.state != WL_CONNECTED) return;
    if (w.rsn && !(f[1] & 0x40)) return;                        /* no plaintext data on a protected network */
    static u8 eth[1600];
    if (blen - 8 > 1500) return;
    memcpy(eth, dst, 6);
    memcpy(eth + 6, src, 6);
    eth[12] = body[6]; eth[13] = body[7];
    memcpy(eth + 14, body + 8, blen - 8);
    net_input(&w.nif, eth, 14 + blen - 8);
}

static void rx(const u8 *f, usize len, const iwm_rxinfo_t *ri) {
    if (len < 24) return;
    u8 type = f[0] & 0x0c, sub = f[0] & 0xf0;
    if (type == 0x08) { if (w.state >= WL_ASSOC) data_input(f, len, ri); return; }
    if (type != 0x00) return;
    if (sub == 0x80 || sub == 0x50) { scan_result(f, len, ri); return; }
    if (w.state < WL_AUTH || memcmp(f + 16, w.cur.bssid, 6) || memcmp(f + 4, wifi_macaddr(), 6)) return;
    const u8 *b = f + 24;
    usize bl = len - 24;
    switch (sub) {
    case 0xb0:                                                  /* authentication */
        if (w.state != WL_AUTH || bl < 6) return;
        if (le16(b + 2) != 2) return;
        if (le16(b + 4) != 0) { fail("the access point refused authentication"); return; }
        set_state(WL_ASSOC);
        w.tries = 0;
        send_assoc();
        return;
    case 0x10: case 0x30: {                                     /* (re)association response */
        if (w.state != WL_ASSOC || bl < 6) return;
        u16 status = le16(b + 2), aid = le16(b + 4);
        if (status) { char m[64]; fmt(m, sizeof m, "association refused (status %u)", status); fail(m); return; }
        iwm_bss_t d = driver_bss();
        d.assoc_id = aid;
        if (wifi_assoc_done(&d)) { fail("the card did not accept the association"); return; }
        LOG("associated with %s (AID %u)", w.cur.ssid, aid & 0x3fff);
        if (w.rsn) { set_state(WL_HANDSHAKE); w.handshake_deadline = k_now_ms() + 6000; }
        else connected();
        return;
    }
    case 0xc0: case 0xa0:                                       /* deauthentication / disassociation */
        LOG("the access point dropped us (reason %u)", bl >= 2 ? le16(b) : 0);
        fail(w.state == WL_HANDSHAKE ? "wrong password (the access point disconnected)" : "disconnected by the access point");
        return;
    }
}

/* ---- public ------------------------------------------------------------------------------------- */
int wlan_available(void) { return wifi_present(); }
int wlan_state(void) { return w.state; }

const char *wlan_state_text(void) {
    switch (w.state) {
    case WL_OFF: return "Off";
    case WL_STARTING: return "Turning on...";
    case WL_IDLE: return "Not connected";
    case WL_SCANNING: return "Searching...";
    case WL_AUTH: case WL_ASSOC: fmt(w.state_text, sizeof w.state_text, "Connecting to %s...", w.cur.ssid); return w.state_text;
    case WL_HANDSHAKE: fmt(w.state_text, sizeof w.state_text, "Checking the password for %s...", w.cur.ssid); return w.state_text;
    case WL_CONNECTED: {
        static char b[96];
        if (w.nif.ip) fmt(b, sizeof b, "Connected to %s", w.cur.ssid);
        else fmt(b, sizeof b, "Connected to %s, getting an address...", w.cur.ssid);
        return b;
    }
    case WL_FAILED: return w.state_text[0] ? w.state_text : "Connection failed";
    }
    return "";
}

int wlan_power(int on) {
    if (on && w.state == WL_OFF) {
        set_state(WL_STARTING);
        wifi_set_rx(rx);
        if (wifi_start()) {
            set_state(WL_OFF);
            strlcpy(w.state_text, "the radio did not start (see the log)", sizeof w.state_text);
            hal_setting_set(u"QrtWifiOn", 0);          /* do not retry at every boot */
            return -1;
        }
        if (!w.nif.name) {
            w.nif.name = "Wi-Fi";
            memcpy(w.nif.mac, wifi_macaddr(), 6);
            w.nif.send = nif_send;
            net_register(&w.nif);
        }
        w.powered = 1;
        set_state(WL_IDLE);
        hal_setting_set(u"QrtWifiOn", 1);
        return 0;
    }
    if (!on && w.state != WL_OFF) {
        wlan_disconnect();
        wifi_stop();
        w.powered = 0;
        set_state(WL_OFF);
        hal_setting_set(u"QrtWifiOn", 0);
    }
    return 0;
}

int wlan_scan(void) {
    if (w.state == WL_OFF || w.state == WL_STARTING) return -1;
    if (w.state >= WL_AUTH && w.state <= WL_CONNECTED) return -1;     /* no background scans yet */
    if (wifi_scan()) return -1;
    set_state(WL_SCANNING);
    w.deadline = k_now_ms() + 15000;
    return 0;
}

void wlan_disconnect(void) {
    if (w.state >= WL_AUTH && w.state <= WL_CONNECTED) {
        u8 reason[2] = { 3, 0 };                                    /* leaving */
        mgmt_send(0xc0, reason, 2);
        wifi_poll();
        hal_delay_us(5000);
    }
    wifi_disconnect();
    w.have_ptk_candidate = 0;
    w.have_gtk = 0;
    if (w.nif.link) { w.nif.link = 0; net_link_changed(&w.nif); }
    if (w.state != WL_OFF) set_state(WL_IDLE);
}

int wlan_saved(char *ssid, usize cap) {
    char s[33] = { 0 };
    usize n = hal_setting_get_blob(u"QrtWifiSsid", s, 32);
    if (!n) return 0;
    s[MIN(n, (usize)32)] = 0;
    if (ssid) strlcpy(ssid, s, cap);
    return 1;
}

void wlan_forget(void) {
    hal_setting_set_blob(u"QrtWifiSsid", NULL, 0);
    hal_setting_set_blob(u"QrtWifiPmk", NULL, 0);
}

int wlan_connect(const char *ssid, const char *pass) {
    if (w.state == WL_OFF || w.state == WL_STARTING) return -1;
    if (w.state >= WL_AUTH && w.state <= WL_CONNECTED) wlan_disconnect();
    bss_t *best = NULL;
    for (int i = 0; i < n_bss; i++)
        if (!strcmp(bss[i].ssid, ssid) && (!best || bss[i].rssi > best->rssi)) best = &bss[i];
    if (!best) { strlcpy(w.state_text, "network not in range", sizeof w.state_text); set_state(WL_FAILED); return -1; }
    if (!wlan_supported(best->security)) {
        fmt(w.state_text, sizeof w.state_text, "%s networks are not supported yet", wlan_security_name(best->security));
        set_state(WL_FAILED);
        return -1;
    }
    w.cur = *best;
    w.rsn = best->security == SEC_WPA2_PSK;
    w.replay = 0;
    w.have_ptk_candidate = 0;
    w.have_gtk = 0;
    if (w.rsn) {
        char saved[33];
        if (pass && *pass) {
            if (strlen(pass) < 8 || strlen(pass) > 63) { strlcpy(w.state_text, "WPA2 passwords have 8 to 63 characters", sizeof w.state_text); set_state(WL_FAILED); return -1; }
            u64 t0 = k_now_ms();
            pbkdf2_sha1(pass, (const u8 *)ssid, strlen(ssid), 4096, w.pmk, 32);
            LOG("derived the key from the password in %llu ms", k_now_ms() - t0);
        } else if (wlan_saved(saved, sizeof saved) && !strcmp(saved, ssid) &&
                   hal_setting_get_blob(u"QrtWifiPmk", w.pmk, 32) == 32) {
            LOG("using the saved key for %s", ssid);
        } else { strlcpy(w.state_text, "password needed", sizeof w.state_text); set_state(WL_FAILED); return -1; }
        build_rsn_ie();
        w.rsn_ie[4 + 3] = best->group_cipher;
    }
    strlcpy(w.want_ssid, ssid, sizeof w.want_ssid);
    iwm_bss_t d = driver_bss();
    LOG("joining %s (%02x:%02x:%02x:%02x:%02x:%02x, channel %d, %d dBm, %s)", best->ssid,
        best->bssid[0], best->bssid[1], best->bssid[2], best->bssid[3], best->bssid[4], best->bssid[5],
        best->channel, best->rssi, wlan_security_name(best->security));
    if (wifi_auth_prepare(&d)) { fail("the card could not prepare the connection"); return -1; }
    set_state(WL_AUTH);
    w.tries = 0;
    send_auth();
    return 0;
}

static void remember(void) {
    hal_setting_set_blob(u"QrtWifiSsid", w.cur.ssid, strlen(w.cur.ssid));
    if (w.rsn) hal_setting_set_blob(u"QrtWifiPmk", w.pmk, 32);
    else hal_setting_set_blob(u"QrtWifiPmk", NULL, 0);
}

void wlan_poll(void) {
    if (w.state == WL_OFF) {
        /* came up on at boot last time: start again once the shell runs */
        static int boot_checked;
        if (!boot_checked && k_now_ms() > k.boot_ms + 1500 && k.boot_ms) {
            boot_checked = 1;
            if (wifi_present() && hal_setting_get(u"QrtWifiOn", 0) == 1) {
                LOG("Wi-Fi was on: starting");
                if (wlan_power(1) == 0) {
                    char s[33];
                    if (wlan_saved(s, sizeof s)) { strlcpy(w.want_ssid, s, sizeof w.want_ssid); wlan_scan(); }
                }
            }
        }
        return;
    }
    wifi_poll();
    u64 now = k_now_ms();
    static int prev = -1;
    switch (w.state) {
    case WL_SCANNING:
        if (wifi_scanning() && now > w.deadline) { LOG("no scan-complete notification: using what arrived"); wifi_scan_forget(); }
        if (!wifi_scanning()) {
            set_state(WL_IDLE);
            wlan_net_t nets[32];
            int n = wlan_networks(nets, 32);
            LOG("%d networks in range", n);
            /* reconnect to the saved network */
            if (w.want_ssid[0]) {
                for (int i = 0; i < n; i++) if (!strcmp(nets[i].ssid, w.want_ssid)) { wlan_connect(w.want_ssid, NULL); break; }
            }
        }
        break;
    case WL_AUTH:
        if (now > w.deadline) { if (++w.tries >= 4) fail("no answer from the access point"); else send_auth(); }
        break;
    case WL_ASSOC:
        if (now > w.deadline) { if (++w.tries >= 4) fail("the access point did not answer the association"); else send_assoc(); }
        break;
    case WL_HANDSHAKE:
        if (now > w.handshake_deadline) fail(w.have_ptk_candidate ? "wrong password" : "no answer during the handshake");
        break;
    case WL_CONNECTED:
        if (prev != WL_CONNECTED) remember();
        /* the firmware stops passing our AP's beacons once associated; it reports losses */
        if (wifi_missed_beacons() >= 20) fail("lost the access point");
        break;
    }
    prev = w.state;
    if (log_dirty && !k.native) { log_dirty = 0; wifilog_save(); }
}
