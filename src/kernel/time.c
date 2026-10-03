/* time.c - the wall clock.
 *
 * The kernel keeps one clock: Unix time (UTC) as an offset from the TSC, and the
 * local time zone as an offset from UTC.  It starts from the tablet's RTC, which
 * holds local time (Windows keeps it that way).  The RTC can be wrong: it goes back
 * to a default date when the battery runs completely flat.  A date before this build
 * is taken as such a reset and replaced by the build date.  Once the network is up,
 * SNTP (src/net/net.c) sets the real time with time_set_utc(); the RTC is corrected
 * so the next boot starts right even without a network.
 *
 * Certificates are only valid between two dates, so a clock in the past makes every
 * https:// page fail ("SSL verification failed"). */
#include "kernel.h"
#if defined(__x86_64__)
#include "../arch/x64/cpu.h"
#endif

#ifndef QRT_BUILD_EPOCH
#define QRT_BUILD_EPOCH 1767225600ull           /* 2026-01-01 */
#endif
#define TZ_UNSET 0x7fffffffu

static i64 utc_at_boot;      /* Unix time when k_now_ms() was 0 */
static i32 tz_offset;        /* local = UTC + tz_offset (seconds) */
static int tz_known;         /* the zone was chosen or learnt (saved in NVRAM) */
static int synced;           /* set from the network since boot */
static u64 synced_at_ms;
static i64 rtc_local_at_boot;   /* what the RTC said, as seconds (local time) */
static int rtc_ok;

static i64 days_from_civil(i64 y, i64 m, i64 d) {
    y -= m <= 2;
    i64 era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    i64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    i64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_secs(i64 s, EFI_TIME *t) {
    i64 days = s >= 0 ? s / 86400 : (s - 86399) / 86400, rem = s - days * 86400;
    i64 z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    i64 doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    i64 y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    i64 d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    memset(t, 0, sizeof *t);
    t->Year = (u16)(y + (m <= 2)); t->Month = (u8)m; t->Day = (u8)d;
    t->Hour = (u8)(rem / 3600); t->Minute = (u8)(rem / 60 % 60); t->Second = (u8)(rem % 60);
    t->TimeZone = 2047;
}

static i64 secs_of(const EFI_TIME *t) {
    return days_from_civil(t->Year, t->Month, t->Day) * 86400 + t->Hour * 3600 + t->Minute * 60 + t->Second;
}

/* at boot, while the firmware's services are still there */
void time_init(void) {
    u32 tz = hal_setting_get(u"QrtTimeZone", TZ_UNSET);
    tz_known = tz != TZ_UNSET;
    tz_offset = tz_known ? (i32)tz : 0;
    EFI_TIME t;
    memset(&t, 0, sizeof t);
    rtc_ok = !EFI_ERROR(k.rt->GetTime(&t, NULL)) && t.Year >= 1970 && t.Month >= 1 && t.Month <= 12 && t.Day >= 1 && t.Day <= 31;
    i64 now_s = (i64)(k_now_ms() / 1000);
    if (rtc_ok) rtc_local_at_boot = secs_of(&t) - now_s;
    i64 utc = rtc_ok ? secs_of(&t) - tz_offset : 0;
    if (utc < (i64)QRT_BUILD_EPOCH) {
        if (rtc_ok) klog("clock: the RTC says %04u-%02u-%02u %02u:%02u: reset; using the build date until the network sets the time",
                         t.Year, t.Month, t.Day, t.Hour, t.Minute);
        else klog("clock: no RTC; starting from the build date until the network sets the time");
        rtc_ok = 0;
        utc = (i64)QRT_BUILD_EPOCH;
    }
    utc_at_boot = utc - now_s;
    k.epoch_at_boot = (u64)utc_at_boot;
}

u64 time_utc(void) { return (u64)(utc_at_boot + (i64)(k_now_ms() / 1000)); }
u64 time_utc_us(void) { return (u64)utc_at_boot * 1000000ull + k_now_us(); }
int time_synced(void) { return synced; }
u64 time_synced_ago_ms(void) { return synced ? k_now_ms() - synced_at_ms : ~0ull; }
int time_zone(void) { return tz_offset; }
int time_zone_known(void) { return tz_known; }

/* write local time to the RTC (runtime service; no preemption during the call) */
static void rtc_write(void) {
    EFI_TIME t;
    civil_from_secs((i64)time_utc() + tz_offset, &t);
#if defined(__x86_64__)
    u64 fl = k.native ? irq_save() : 0;
#endif
    EFI_STATUS st = ((EFI_STATUS (*)(EFI_TIME *))k.rt->SetTime)(&t);
#if defined(__x86_64__)
    if (k.native) irq_restore(fl);
#endif
    if (EFI_ERROR(st)) klog("clock: the RTC refused the new time (status %llx)", (u64)st);
}

void time_set_zone(int offset_s) {
    offset_s = CLAMP(offset_s, -12 * 3600, 14 * 3600);
    tz_offset = offset_s;
    tz_known = 1;
    hal_setting_set(u"QrtTimeZone", (u32)offset_s);
    if (synced) rtc_write();            /* the RTC holds local time: keep it right for the next boot */
}

/* the network's answer (SNTP): the real time, in UTC */
void time_set_utc(u64 utc, const char *source) {
    i64 now_s = (i64)(k_now_ms() / 1000);
    i64 old = utc_at_boot + now_s, delta = (i64)utc - old;
    if (!tz_known && rtc_ok) {
        /* the RTC was plausible: it holds local time, so the difference is the zone */
        i64 rtc_now = rtc_local_at_boot + now_s, diff = rtc_now - (i64)utc;
        if (diff >= -12 * 3600 && diff <= 14 * 3600) {
            int z = (int)((diff >= 0 ? diff + 450 : diff - 450) / 900 * 900);   /* to 15 minutes */
            time_set_zone(z);
            klog("clock: time zone UTC%c%d:%02d, from the RTC", z < 0 ? '-' : '+', ABS_I(z) / 3600, ABS_I(z) / 60 % 60);
        }
    }
    utc_at_boot = (i64)utc - now_s;
    k.epoch_at_boot = (u64)utc_at_boot;
    int first = !synced;
    synced = 1;
    synced_at_ms = k_now_ms();
    if (first || delta > 2 || delta < -2) {
        EFI_TIME t;
        civil_from_secs((i64)utc, &t);
        klog("clock: %04u-%02u-%02u %02u:%02u:%02u UTC from %s (%lld s %s)", t.Year, t.Month, t.Day, t.Hour, t.Minute, t.Second,
             source, (long long)(delta < 0 ? -delta : delta), delta < 0 ? "ahead" : "behind");
        if (delta > 2 || delta < -2) rtc_write();
    }
}

/* local date and time, for the shell and the apps */
void k_walltime(EFI_TIME *t) { civil_from_secs((i64)time_utc() + tz_offset, t); }
