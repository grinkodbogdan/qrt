/* backlight.h - panel backlight of the Venue 8 Pro 5855 (LPSS PWM #1, native mode). */
#pragma once
#include "../kernel/kernel.h"

int  backlight_init(void);            /* 1 if the backlight PWM was found running */
int  backlight_available(void);
int  backlight_level(void);           /* 5..100 (%) */
void backlight_set_level(int pct);    /* also saved as QrtBrightness */
void backlight_power(int on);         /* off for sleep; on restores the level */
void backlight_status(char *buf, usize cap);
