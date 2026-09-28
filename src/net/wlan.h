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
