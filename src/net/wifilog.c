/* wifilog.c - ring of the last Wi-Fi log lines. */
#include "wifilog.h"

#define LINES 400
#define WIDTH 160

static char ring[LINES][WIDTH];
static int head, count;
static u32 serial;

void wifilog(const char *f, ...) {
    va_list ap;
    va_start(ap, f);
    char *l = ring[head];
    int n = fmt(l, WIDTH, "%6llu ", k_now_ms());
    vfmt(l + n, WIDTH - (usize)n, f, ap);
    va_end(ap);
    klog("%s", l + n);
    head = (head + 1) % LINES;
    if (count < LINES) count++;
    serial++;
}

int wifilog_count(void) { return count; }
const char *wifilog_line(int i) { return i < 0 || i >= count ? NULL : ring[(head - count + i + LINES) % LINES]; }
u32 wifilog_serial(void) { return serial; }

int wifilog_save(void) {
    usize cap = (usize)count * (WIDTH + 2) + 1, o = 0;
    char *buf = kalloc(cap);
    for (int i = 0; i < count; i++) o += (usize)fmt(buf + o, cap - o, "%s\r\n", wifilog_line(i));
    int r = hwreport_write("wifi.txt", buf, o);
    kfree(buf);
    return r;
}
