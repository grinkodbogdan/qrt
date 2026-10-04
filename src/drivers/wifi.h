/* wifi.h - the Wi-Fi card, whichever driver has it: iwm (Intel 8260, the Venue 8 Pro)
 * or iwn (Intel 6230/6235, the Panasonic FZ-G1).  The calls and types are iwm.h's. */
#pragma once
#include "iwm/iwm.h"

int  wifi_start(void);
void wifi_stop(void);
void wifi_poll(void);
void wifi_set_rx(iwm_rx_fn fn);
int  wifi_scan(void);
int  wifi_scanning(void);
void wifi_scan_forget(void);
u32  wifi_missed_beacons(void);
int  wifi_auth_prepare(const iwm_bss_t *b);
int  wifi_assoc_done(const iwm_bss_t *b);
void wifi_disconnect(void);
int  wifi_tx(const u8 *frame, usize len, int mgmt);
int  wifi_set_pairwise_key(const u8 key[16]);
const u8 *wifi_macaddr(void);
const char *wifi_status(void);
const char *wifi_fw_version(void);
int  wifi_present(void);
void wifi_check_firmware(void);
int  wifi_ready(void);
