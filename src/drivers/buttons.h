/* buttons.h - the tablet's hardware buttons on Cherry Trail GPIO (native mode). */
#pragma once
#include "../kernel/kernel.h"

int  buttons_init(void);
int  buttons_active(void);                  /* the power button can wake the tablet */                    /* 1 if the Venue 8 Pro 5855 button pins were found */
int  buttons_poll(event_t *out, int max);   /* key events for presses (called from hal_poll) */
void buttons_status(char *buf, usize cap);
