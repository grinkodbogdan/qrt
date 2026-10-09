/* wlan.h - the 802.11 client: networks, association, WPA2-Personal. */
#pragma once
#include "../kernel/kernel.h"

enum { SEC_OPEN, SEC_WEP, SEC_WPA_TKIP, SEC_WPA2_PSK, SEC_WPA2_ENTERPRISE, SEC_WPA3, SEC_UNSUPPORTED_CIPHER };

typedef struct {
    u8 bssid[6];
    char ssid[33];
    int channel, rssi, security;
    u64 seen_ms;
} wlan_net_t;

enum { WL_OFF, WL_STARTING, WL_IDLE, WL_SCANNING, WL_AUTH, WL_ASSOC, WL_HANDSHAKE, WL_CONNECTED, WL_FAILED };

int  wlan_available(void);            /* a supported card exists */
int  wlan_power(int on);              /* start/stop the radio (blocks ~1-2 s to load the firmware) */
int  wlan_state(void);
const char *wlan_state_text(void);
int  wlan_scan(void);
int  wlan_networks(wlan_net_t *out, int max);    /* best BSS per SSID, strongest first */
int  wlan_connect(const char *ssid, const char *passphrase);   /* passphrase NULL: use the saved key */
void wlan_disconnect(void);
int  wlan_saved(char *ssid, usize cap);          /* 1 if a network is remembered */
void wlan_forget(void);
void wlan_poll(void);
const char *wlan_security_name(int sec);
int  wlan_supported(int sec);

/* A Wi-Fi card whose 802.11 association runs elsewhere - Linux's mac80211 behind nl80211
 * (the Xiaomi Mi A1's wcn36xx, arch/arm64/lwifi.c).  wlan.c keeps the network list, the
 * password, the WPA2 4-way handshake and the IP side; the backend scans, associates,
 * carries Ethernet frames (EAPOL included) and installs the keys. */
typedef struct {
    int  (*present)(void);
    int  (*start)(void);                                   /* radio on: 0 done, 1 in progress (asked again), -1 failed */
    void (*stop)(void);
    int  (*scan)(void);
    int  (*scanning)(void);
    int  (*connect)(const u8 bssid[6], const char *ssid, int freq_mhz, const u8 *ie, usize ielen);
    void (*disconnect)(void);
    int  (*send)(const u8 *eth, usize len);               /* an Ethernet frame */
    int  (*set_keys)(const u8 tk[16], int gtk_id, const u8 gtk[16]);   /* tk or gtk NULL: not that one */
    const u8 *(*mac)(void);
    void (*poll)(void);                                    /* events: the wlan_sm_* calls below */
} wlan_softmac_t;
const wlan_softmac_t *wlan_softmac(void);                 /* NULL: none (weak default) */
/* from the backend's poll */
void wlan_sm_bss(const u8 bssid[6], int freq_mhz, int rssi_dbm, u16 capinfo, u16 beacon_int, const u8 *ies, usize ielen);
void wlan_sm_assoc(int ok, int status);
void wlan_sm_eth(const u8 *eth, usize len);
void wlan_sm_lost(int reason);
