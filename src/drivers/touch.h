/* touch.h - native touchscreen service (DesignWare I2C + HID over I2C).
 *
 * Opt-in and reversible: probing only reads descriptors; "go native"
 * detaches the firmware's driver from the I2C controller and polls the
 * touch chip directly, and anything going wrong hands touch back to the
 * firmware. */
#pragma once
#include "i2chid.h"

typedef struct {
    u8 addr;
    u16 desc_reg;
    const char *name;
    int result;               /* DW_* of the descriptor read, 1 = not tried */
    i2chid_t hid;
} touch_candidate_t;

#define TOUCH_CANDIDATES 3

typedef struct {
    int probed;
    dwi2c_t bus;
    int bus_err;
    touch_candidate_t cand[TOUCH_CANDIDATES];
    int primary;              /* index of the finger-touch device, -1 if none */
    int board_valid;
    u8 osid, bdid, mpnl, itsa, wlid;
    u32 gnvs;
    int active;
    char status[96];
    u8 last[64];
    int last_len;
    u64 last_report_ms;
    u32 consecutive_errors;
    u64 native_since;
} ntouch_t;

extern ntouch_t nt;

void ntouch_probe(void);
int  ntouch_go_native(void);     /* 1 on success */
void ntouch_revert(void);        /* give touch back to the firmware */
int  ntouch_active(void);
int  ntouch_poll(event_t *out, int max);
int  ntouch_save(void);          /* \qrt\hwdump\touch.txt */
