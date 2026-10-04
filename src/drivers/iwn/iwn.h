/* iwn.h - Intel Centrino Advanced-N 6230/6235 (the Panasonic FZ-G1's Wi-Fi).
 * The same calls as iwm.h, with its types; src/drivers/wifi.c picks the card. */
#pragma once
#include "../iwm/iwm.h"

int  iwn_probe(pci_dev_t *d);
int  iwn_attach(void);
int  iwn_start(void);
void iwn_stop(void);
void iwn_poll(void);
void iwn_set_rx(iwm_rx_fn fn);
int  iwn_scan(void);
int  iwn_scanning(void);
void iwn_scan_forget(void);
u32  iwn_missed_beacons(void);
int  iwn_auth_prepare(const iwm_bss_t *b);
int  iwn_assoc_done(const iwm_bss_t *b);
void iwn_disconnect(void);
int  iwn_tx(const u8 *frame, usize len, int mgmt);
int  iwn_set_pairwise_key(const u8 key[16]);
const u8 *iwn_macaddr(void);
const char *iwn_status(void);
const char *iwn_fw_version(void);
int  iwn_present(void);
void iwn_check_firmware(void);
int  iwn_ready(void);
