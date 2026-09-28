/* Host test for src/net/wlan.c: a simulated access point runs a WPA2-PSK
 * association end to end against the real client code - beacon, open
 * authentication, association (checks the RSN element), the EAPOL-Key
 * 4-way handshake (checks both MICs, the nonces and the key installed in
 * the "card"), then a group-key CCMP broadcast that must come out as an
 * Ethernet frame.  The AP side derives keys and builds frames on its own,
 * from IEEE 802.11-2016 12.7, so framing or key-ordering mistakes show.
 * Build/run: make check */
#include <stdio.h>
#include <stdlib.h>
#include "../src/net/wlan.h"
#include "../src/net/net.h"
#include "../src/net/crypto.h"
#include "../src/net/wifilog.h"
#include "../src/drivers/iwm/iwm.h"

/* ---- the kernel environment wlan.c expects -------------------------------------- */
kernel_t k;
static u64 now_ms = 10000;
u64 k_now_ms(void) { return now_ms; }
void hal_delay_us(u32 us) { (void)us; }
static u32 settings[8];
u32 hal_setting_get(const c16 *n, u32 def) { (void)n; return settings[0] ? settings[0] : def; }
void hal_setting_set(const c16 *n, u32 v) { (void)n; settings[0] = v; }
static u8 blob[2][64]; static usize bloblen[2];
static int blob_idx(const c16 *n) { return n[7] == 'S' ? 0 : 1; }    /* QrtWifiSsid / QrtWifiPmk */
usize hal_setting_get_blob(const c16 *n, void *b, usize cap) { int i = blob_idx(n); usize l = bloblen[i] < cap ? bloblen[i] : cap; memcpy(b, blob[i], l); return l; }
void hal_setting_set_blob(const c16 *n, const void *b, usize len) { int i = blob_idx(n); bloblen[i] = len; if (len) memcpy(blob[i], b, len); }
void strlcpy(char *d, const char *s, usize cap) { if (!cap) return; usize n = strlen(s); if (n >= cap) n = cap - 1; memcpy(d, s, n); d[n] = 0; }
int fmt(char *buf, usize cap, const char *f, ...) { va_list ap; va_start(ap, f); int n = vsnprintf(buf, cap, f, ap); va_end(ap); return n; }
void wifilog(const char *f, ...) { va_list ap; va_start(ap, f); if (getenv("VERBOSE")) { vprintf(f, ap); printf("\n"); } va_end(ap); }
int wifilog_save(void) { return 1; }            /* firmware-mode log autosave: nothing to write here */

u16 be16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }
u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
void put16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
void put32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
void ip_to_str(u32 ip, char *b, usize cap) { fmt(b, cap, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255); }
static netif_t *nif;
static u8 got_eth[1600]; static usize got_eth_len;
void net_register(netif_t *n) { nif = n; }
void net_link_changed(netif_t *n) { (void)n; }
void net_input(netif_t *n, const u8 *eth, usize len) { (void)n; memcpy(got_eth, eth, len); got_eth_len = len; }

/* ---- the "card" -------------------------------------------------------------------- */
static const u8 sta[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
static const u8 ap[6] = { 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee };
static iwm_rx_fn rxfn;
static u8 tx[4096]; static usize tx_len; static int n_tx;
static u8 installed[16]; static int key_installed, assoc_done;
int iwm_present(void) { return 1; }
int iwm_start(void) { return 0; }
void iwm_stop(void) {}
void iwm_poll(void) {}
void iwm_set_rx(iwm_rx_fn f) { rxfn = f; }
int iwm_scan(void) { return 0; }
int iwm_scanning(void) { return 0; }
void iwm_scan_forget(void) {}
u32 iwm_missed_beacons(void) { return 0; }
const u8 *iwm_macaddr(void) { return sta; }
int iwm_auth_prepare(const iwm_bss_t *b) { (void)b; return 0; }
int iwm_assoc_done(const iwm_bss_t *b) { assoc_done = b->assoc_id; return 0; }
void iwm_disconnect(void) {}
int iwm_tx(const u8 *f, usize len, int mgmt) { (void)mgmt; memcpy(tx, f, len); tx_len = len; n_tx++; return 0; }
int iwm_set_pairwise_key(const u8 key[16]) { memcpy(installed, key, 16); key_installed = 1; return 0; }

#include "../src/net/crypto.c"
#include "../src/net/wlan.c"

/* ---- the access point ---------------------------------------------------------------- */
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static const iwm_rxinfo_t ri = { .channel = 6, .rssi = -48 };
static const u8 rsn_ap[] = { 48, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 2, 0x0c, 0 };

static usize mgmt(u8 *f, u8 subtype, const u8 *da, const u8 *body, usize blen) {
    memset(f, 0, 24);
    f[0] = subtype;
    memcpy(f + 4, da, 6); memcpy(f + 10, ap, 6); memcpy(f + 16, ap, 6);
    memcpy(f + 24, body, blen);
    return 24 + blen;
}

static usize eapol_frame(u8 *f, const u8 *eapol, usize len) {           /* FromDS data + LLC/SNAP */
    memset(f, 0, 24);
    f[0] = 0x08; f[1] = 0x02;
    memcpy(f + 4, sta, 6); memcpy(f + 10, ap, 6); memcpy(f + 16, ap, 6);
    static const u8 snap[8] = { 0xaa, 0xaa, 0x03, 0, 0, 0, 0x88, 0x8e };
    memcpy(f + 24, snap, 8);
    memcpy(f + 32, eapol, len);
    return 32 + len;
}

static usize key_msg(u8 *e, u16 info, u64 replay, const u8 *nonce, const u8 *kd, usize kdlen, const u8 *kck) {
    usize len = 4 + 95 + kdlen;
    memset(e, 0, len);
    e[0] = 2; e[1] = 3; put16(e + 2, (u16)(95 + kdlen));
    u8 *kk = e + 4;
    kk[0] = 2; put16(kk + 1, info); put16(kk + 3, 16);
    for (int i = 0; i < 8; i++) kk[5 + i] = (u8)(replay >> (56 - 8 * i));
    if (nonce) memcpy(kk + 13, nonce, 32);
    put16(kk + 93, (u16)kdlen);
    memcpy(kk + 95, kd, kdlen);
    if (kck) { u8 h[20]; hmac_sha1(kck, 16, e, len, h); memcpy(kk + 77, h, 16); }
    return len;
}

static int check_mic(const u8 *e, usize len, const u8 *kck) {
    u8 copy[512], h[20];
    memcpy(copy, e, len);
    memset(copy + 4 + 77, 0, 16);
    hmac_sha1(kck, 16, copy, len, h);
    return !memcmp(h, e + 4 + 77, 16);
}

int main(void) {
    const char *ssid = "TestNet", *pass = "correct horse";
    k.boot_ms = 1;
    settings[0] = 0;

    CHECK(wlan_power(1) == 0);
    CHECK(nif && !memcmp(nif->mac, sta, 6));

    /* beacon */
    u8 f[2048], body[256];
    usize o = 0;
    memset(body, 0, 12);
    body[8] = 100; body[10] = 0x11; body[11] = 0x04;                         /* ESS, privacy, short slot */
    o = 12;
    body[o++] = 0; body[o++] = 7; memcpy(body + o, ssid, 7); o += 7;
    static const u8 rates[] = { 1, 8, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24 };
    memcpy(body + o, rates, sizeof rates); o += sizeof rates;
    body[o++] = 3; body[o++] = 1; body[o++] = 6;
    body[o++] = 5; body[o++] = 4; body[o++] = 0; body[o++] = 2; body[o++] = 0; body[o++] = 0;
    memcpy(body + o, rsn_ap, sizeof rsn_ap); o += sizeof rsn_ap;
    static const u8 bc[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    rxfn(f, mgmt(f, 0x80, bc, body, o), &ri);
    wlan_net_t nets[4];
    CHECK(wlan_networks(nets, 4) == 1 && !strcmp(nets[0].ssid, ssid) && nets[0].security == SEC_WPA2_PSK && nets[0].channel == 6);

    /* authentication */
    CHECK(wlan_connect(ssid, pass) == 0);
    CHECK(tx_len == 30 && tx[0] == 0xb0 && !memcmp(tx + 4, ap, 6) && !memcmp(tx + 10, sta, 6) && tx[26] == 1);
    u8 auth_resp[6] = { 0, 0, 2, 0, 0, 0 };
    rxfn(f, mgmt(f, 0xb0, sta, auth_resp, 6), &ri);
    CHECK(w.state == WL_ASSOC);

    /* association request: SSID, rates and our RSN element (CCMP/CCMP/PSK) */
    CHECK(tx[0] == 0x00);
    const u8 *ies = tx + 24 + 4;
    CHECK(ies[0] == 0 && ies[1] == 7 && !memcmp(ies + 2, ssid, 7));
    int found_rsn = 0;
    for (usize i = 28; i + 2 <= tx_len; i += 2u + tx[i + 1])
        if (tx[i] == 48) { found_rsn = 1; CHECK(tx[i + 1] == 20 && tx[i + 7] == 4 && tx[i + 13] == 4 && tx[i + 19] == 2); }
    CHECK(found_rsn);
    u8 aresp[] = { 0x11, 0x04, 0, 0, 0x01, 0xc0, 1, 4, 0x82, 0x84, 0x8b, 0x96 };
    rxfn(f, mgmt(f, 0x10, sta, aresp, sizeof aresp), &ri);
    CHECK(assoc_done == 0xc001 && w.state == WL_HANDSHAKE);

    /* the AP's key hierarchy, computed independently */
    u8 pmk[32], anonce[32], ptk[48];
    pbkdf2_sha1(pass, (const u8 *)ssid, 7, 4096, pmk, 32);
    for (int i = 0; i < 32; i++) anonce[i] = (u8)(0xa0 + i);

    /* message 1 */
    u8 e[512];
    usize el = key_msg(e, 2 | 8 | 0x80, 1, anonce, NULL, 0, NULL);
    rxfn(f, eapol_frame(f, e, el), &ri);
    /* message 2 */
    CHECK(tx[0] == 0x08 && (tx[1] & 0x01) && !(tx[1] & 0x40));
    CHECK(tx[30] == 0x88 && tx[31] == 0x8e);
    const u8 *m2 = tx + 32;
    usize m2len = 4 + be16(m2 + 2);
    u8 snonce[32];
    memcpy(snonce, m2 + 4 + 13, 32);
    CHECK(be16(m2 + 4 + 1) == (2 | 8 | 0x100));
    u8 data[76];
    int af = memcmp(ap, sta, 6) < 0, nf = memcmp(anonce, snonce, 32) < 0;
    memcpy(data, af ? ap : sta, 6); memcpy(data + 6, af ? sta : ap, 6);
    memcpy(data + 12, nf ? anonce : snonce, 32); memcpy(data + 44, nf ? snonce : anonce, 32);
    prf_sha1(pmk, 32, "Pairwise key expansion", data, 76, ptk, 48);
    CHECK(check_mic(m2, m2len, ptk));
    CHECK(be16(m2 + 4 + 93) == 22 && m2[4 + 95] == 48);

    /* message 3 with the group key */
    u8 gtk[16], kd[64], wrapped[72];
    for (int i = 0; i < 16; i++) gtk[i] = (u8)(0x40 + i);
    usize kl = 0;
    memcpy(kd, rsn_ap, sizeof rsn_ap); kl = sizeof rsn_ap;
    u8 kde[] = { 0xdd, 22, 0x00, 0x0f, 0xac, 1, 1, 0 };
    memcpy(kd + kl, kde, sizeof kde); kl += sizeof kde;
    memcpy(kd + kl, gtk, 16); kl += 16;
    for (int first = 1; kl % 8; first = 0) kd[kl++] = first ? 0xdd : 0;             /* padding: dd 00 ... */
    aes_wrap(ptk + 16, kd, kl, wrapped);
    el = key_msg(e, 2 | 8 | 0x40 | 0x80 | 0x100 | 0x200 | 0x1000, 2, anonce, wrapped, kl + 8, ptk);
    int before = n_tx;
    rxfn(f, eapol_frame(f, e, el), &ri);
    /* message 4, keys, connected */
    CHECK(n_tx == before + 1);
    const u8 *m4 = tx + 32;
    CHECK(be16(m4 + 4 + 1) == (2 | 8 | 0x100 | 0x200));
    CHECK(check_mic(m4, 4 + be16(m4 + 2), ptk));
    CHECK(key_installed && !memcmp(installed, ptk + 32, 16));
    CHECK(w.state == WL_CONNECTED && w.have_gtk && w.gtk_id == 1 && !memcmp(w.gtk, gtk, 16));

    /* a broadcast ARP under the group key (CCMP, PN 7) */
    u8 plain[64];
    static const u8 snap[8] = { 0xaa, 0xaa, 0x03, 0, 0, 0, 0x08, 0x06 };
    memcpy(plain, snap, 8);
    for (int i = 0; i < 28; i++) plain[8 + i] = (u8)i;
    usize plen = 36;
    const u8 src[6] = { 0x02, 0x99, 0x88, 0x77, 0x66, 0x55 };
    memset(f, 0, 24);
    f[0] = 0x08; f[1] = 0x02 | 0x40;
    memcpy(f + 4, bc, 6); memcpy(f + 10, ap, 6); memcpy(f + 16, src, 6);
    f[22] = 0x10;                                                             /* sequence number 1, fragment 0 */
    u64 pn = 7;
    u8 *iv = f + 24;
    iv[0] = (u8)pn; iv[1] = (u8)(pn >> 8); iv[2] = 0; iv[3] = 0x20 | (1 << 6); iv[4] = iv[5] = iv[6] = iv[7] = 0;
    u8 nonce[13], aad[22];
    nonce[0] = 0; memcpy(nonce + 1, ap, 6);
    for (int i = 0; i < 6; i++) nonce[7 + i] = (u8)(pn >> (8 * (5 - i)));
    aad[0] = 0x08; aad[1] = 0x42; memcpy(aad + 2, f + 4, 18); aad[20] = 0; aad[21] = 0;
    aes128_t a;
    aes128_init(&a, gtk);
    u8 mic[8];
    ccm_encrypt(&a, nonce, aad, 22, plain, plen, f + 32, mic);
    memcpy(f + 32 + plen, mic, 8);
    got_eth_len = 0;
    rxfn(f, 32 + plen + 8, &ri);
    CHECK(got_eth_len == 14 + 28 && !memcmp(got_eth, bc, 6) && !memcmp(got_eth + 6, src, 6) && got_eth[12] == 0x08 && got_eth[13] == 0x06);
    CHECK(got_eth_len >= 14 + 28 && got_eth[14 + 27] == 27);
    /* the same frame again is a replay */
    got_eth_len = 0;
    rxfn(f, 32 + plen + 8, &ri);
    CHECK(got_eth_len == 0);
    /* a tampered frame fails the MIC */
    f[40] ^= 1; pn++; iv[0] = (u8)pn;
    rxfn(f, 32 + plen + 8, &ri);
    CHECK(got_eth_len == 0);

    /* the network and its PMK were remembered */
    now_ms += 20; wlan_poll();
    char saved[33];
    CHECK(wlan_saved(saved, sizeof saved) && !strcmp(saved, ssid) && bloblen[1] == 32 && !memcmp(blob[1], pmk, 32));

    /* a wrong password: message 3's MIC does not verify, the client gives up cleanly */
    CHECK(wlan_connect(ssid, "not the password") == 0);
    rxfn(f, mgmt(f, 0xb0, sta, auth_resp, 6), &ri);
    rxfn(f, mgmt(f, 0x10, sta, aresp, sizeof aresp), &ri);
    CHECK(w.state == WL_HANDSHAKE);
    el = key_msg(e, 2 | 8 | 0x80, 10, anonce, NULL, 0, NULL);
    rxfn(f, eapol_frame(f, e, el), &ri);
    el = key_msg(e, 2 | 8 | 0x40 | 0x80 | 0x100 | 0x200 | 0x1000, 11, anonce, wrapped, kl + 8, ptk);
    rxfn(f, eapol_frame(f, e, el), &ri);
    CHECK(w.state == WL_FAILED && !strcmp(wlan_state_text(), "wrong password"));

    if (fails) { printf("wlan: %d failures\n", fails); return 1; }
    printf("wlan: WPA2 association, 4-way handshake and group-key decryption pass\n");
    return 0;
}
