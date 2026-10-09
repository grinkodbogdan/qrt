/*
 * plog.c - the kernel log kept across a reset, in the device tree's "ramoops" region
 * (a phone's RAM keeps its contents through a warm reset; the region is mapped uncached,
 * so every line is in RAM the moment it is written).
 *
 * The region also records how far the last boot got.  Two boots in a row that reset
 * while Linux's kernel was starting make the next boot a safe one: Linux stays off and the previous boot's log is shown, so the
 * driver that reset the phone can be read off the screen.
 */
#include "arm.h"

#define PLOG_MAGIC 0x51525450524c4f47ull      /* "QRTPRLOG" */
#define COLS 160
struct plog {
    u64 magic;
    u32 state;                                /* PLOG_* of this boot */
    u32 lines, cap;                           /* lines written (free-running), ring size */
    u32 boot;                                 /* boots counted */
    u32 linux_fails;                          /* boots in a row that reset while Linux started */
    char ring[][COLS];
};

static volatile struct plog *pl;
static char (*prev)[COLS];
static int nprev, prev_state = -1;

/* at boot, after the MMU maps the region uncached: keep the last boot's log */
void plog_init(u64 base, u64 size) {
    if (size < 64 * 1024) return;
    pl = (volatile struct plog *)(usize)base;
    u32 cap = (u32)((size - sizeof(struct plog)) / COLS);
    if (pl->magic == PLOG_MAGIC && pl->cap == cap && pl->lines) {
        prev_state = (int)pl->state;
        u32 n = pl->lines < cap ? pl->lines : cap, first = pl->lines - n;
        prev = kalloc((usize)n * COLS);
        for (u32 i = 0; i < n; i++) {
            const volatile char *src = pl->ring[(first + i) % cap];
            for (int c = 0; c < COLS; c++) prev[i][c] = src[c];
            prev[i][COLS - 1] = 0;
        }
        nprev = (int)n;
    }
    klog("plog: %s at %llx (magic %llx, state %u, %u lines)", nprev ? "the last boot's log" : "a new log", base,
         (u64)pl->magic, pl->state, pl->lines);
    u32 boot = pl->magic == PLOG_MAGIC ? pl->boot + 1 : 1;
    u32 fails = nprev && prev_state == PLOG_LINUX_STARTING ? pl->linux_fails + 1 : 0;
    pl->magic = PLOG_MAGIC;
    pl->cap = cap;
    pl->lines = 0;
    pl->state = PLOG_BOOT;
    pl->boot = boot;
    pl->linux_fails = fails;
    __asm__ volatile("dsb sy" ::: "memory");
}

/* klog (rt.c): every line */
void plog_line(const char *s) {
    if (!pl) return;
    volatile char *d = pl->ring[pl->lines % pl->cap];
    int i = 0;
    for (; s[i] && i < COLS - 1; i++) d[i] = s[i];
    d[i] = 0;
    pl->lines++;
    __asm__ volatile("dsb sy" ::: "memory");
}

void plog_state(int s) { if (pl) { pl->state = (u32)s; __asm__ volatile("dsb sy" ::: "memory"); } }

/* Linux's drivers reset the phone while starting, twice in a row.  Only a reset while
 * Linux's kernel was starting counts (not one before it - the power key held to get
 * back to fastboot - nor one after it was up), and only twice: once may be the user. */
int plog_last_boot_failed(void) { return pl && pl->linux_fails >= 2; }
void plog_clear_fails(void) { if (pl) { pl->linux_fails = 0; __asm__ volatile("dsb sy" ::: "memory"); } }
int plog_prev_lines(void) { return nprev; }
const char *plog_prev_line(int i) { return i >= 0 && i < nprev ? prev[i] : NULL; }
