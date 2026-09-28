/* iwm.h - Intel Wireless 7000/8000 family (the Venue 8 Pro 5855's 8260). */
#pragma once
#include "../../kernel/kernel.h"
#include "../pci.h"

/* what the driver hands the 802.11 layer for every received frame */
typedef struct {
    int channel;      /* IEEE channel number */
    int rssi;         /* dBm */
    int decrypted;    /* the firmware already removed CCMP (pairwise key installed) */
    u32 rstamp;       /* device timestamp of reception (for the MAC context's DTIM timing) */
} iwm_rxinfo_t;

typedef void (*iwm_rx_fn)(const u8 *frame, usize len, const iwm_rxinfo_t *ri);

int  iwm_probe(pci_dev_t *d);              /* 1 if this is a supported Intel NIC */
int  iwm_attach(void);                     /* map, load firmware file, read NVM (MAC address) */
int  iwm_start(void);                      /* calibrate + regular firmware + queues: ready to scan */
void iwm_stop(void);
void iwm_poll(void);                       /* service the device; call often */
void iwm_set_rx(iwm_rx_fn fn);

int  iwm_scan(void);                       /* start a scan of every channel; results arrive as frames */
int  iwm_scanning(void);
void iwm_scan_forget(void);                /* stop waiting for a scan-complete notification */
u32  iwm_missed_beacons(void);             /* consecutive, from the firmware's notification */

/* association support: the 802.11 layer drives the state machine */
typedef struct {
    u8 bssid[6];
    int channel;
    u16 beacon_int, capinfo, assoc_id;
    u8 dtim_period;
    u8 basic_rates[16]; int n_basic;       /* rate values in 500 kb/s units, as in the IEs */
    u8 rates[16]; int n_rates;
    u64 tsf;                               /* from the last beacon */
    u32 rstamp;                            /* when it arrived (iwm_rxinfo_t.rstamp) */
    int short_preamble, short_slot;
} iwm_bss_t;
int  iwm_auth_prepare(const iwm_bss_t *b); /* PHY/MAC contexts, binding, station, time event */
int  iwm_assoc_done(const iwm_bss_t *b);   /* the AP accepted us: switch the firmware to associated */
void iwm_disconnect(void);
int  iwm_tx(const u8 *frame, usize len, int mgmt);   /* a complete 802.11 frame (header + body) */
int  iwm_set_pairwise_key(const u8 key[16]);         /* CCMP in hardware from now on */

const u8 *iwm_macaddr(void);
const char *iwm_status(void);              /* one line for the UI */
const char *iwm_fw_version(void);
int  iwm_present(void);
void iwm_check_firmware(void);            /* no card: parse the firmware file anyway, for the log */
int  iwm_ready(void);
