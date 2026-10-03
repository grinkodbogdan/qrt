/* battery.h - battery, charger and lid of the Venue 8 Pro 5855 (an embedded controller on I2C3). */
#pragma once
#include "../kernel/kernel.h"

typedef struct {
    int present;                /* the controller answered */
    int percent;                /* 0..100 */
    int charging, discharging, critical;
    int ac;                     /* the charger is plugged in */
    int lid_closed;             /* a cover's magnet over the sensor */
    int mv, ma;                 /* voltage, current (mA, positive) */
    int mah, full_mah, design_mah;
    int minutes;                /* to empty (discharging) or to full (charging); -1 unknown */
} battery_t;

void battery_probe(void);       /* firmware stage: find I2C3, read once */
void battery_native_resume(void);
void battery_poll(void);        /* the shell, about once a second: AC and lid each time, the battery every 15 s */
const battery_t *battery_get(void);
void battery_status(char *buf, usize cap);
