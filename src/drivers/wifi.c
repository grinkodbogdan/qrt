/* wifi.c - hands the 802.11 layer's calls to the driver that found a card. */
#include "wifi.h"
#include "iwn/iwn.h"

#define PICK(call) (iwn_present() ? iwn_##call : iwm_##call)

int  wifi_start(void) { return PICK(start)(); }
void wifi_stop(void) { PICK(stop)(); }
void wifi_poll(void) { PICK(poll)(); }
void wifi_set_rx(iwm_rx_fn fn) { iwm_set_rx(fn); iwn_set_rx(fn); }
int  wifi_scan(void) { return PICK(scan)(); }
int  wifi_scanning(void) { return PICK(scanning)(); }
void wifi_scan_forget(void) { PICK(scan_forget)(); }
u32  wifi_missed_beacons(void) { return PICK(missed_beacons)(); }
int  wifi_auth_prepare(const iwm_bss_t *b) { return PICK(auth_prepare)(b); }
int  wifi_assoc_done(const iwm_bss_t *b) { return PICK(assoc_done)(b); }
void wifi_disconnect(void) { PICK(disconnect)(); }
int  wifi_tx(const u8 *frame, usize len, int mgmt) { return PICK(tx)(frame, len, mgmt); }
int  wifi_set_pairwise_key(const u8 key[16]) { return PICK(set_pairwise_key)(key); }
const u8 *wifi_macaddr(void) { return PICK(macaddr)(); }
const char *wifi_status(void) { return PICK(status)(); }
const char *wifi_fw_version(void) { return PICK(fw_version)(); }
int  wifi_present(void) { return iwm_present() || iwn_present(); }
int  wifi_ready(void) { return PICK(ready)(); }
void wifi_check_firmware(void) { iwm_check_firmware(); iwn_check_firmware(); }
