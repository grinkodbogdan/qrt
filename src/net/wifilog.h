/* wifilog.h - a log of the Wi-Fi driver and network stack for the Wi-Fi app. */
#pragma once
#include "../kernel/kernel.h"

void wifilog(const char *f, ...);            /* also goes to the kernel log */
int  wifilog_count(void);
const char *wifilog_line(int i);             /* oldest first */
u32  wifilog_serial(void);                   /* bumps on every line */
int  wifilog_save(void);                     /* \qrt\hwdump\wifi.txt (on the stick in firmware mode) */
