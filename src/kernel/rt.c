/* rt.c - freestanding runtime support. */
#include "rt.h"
#include "kernel.h"
#if defined(__x86_64__)
#include "../arch/x64/cpu.h"
#endif
#include "../drivers/uart.h"
#if defined(__x86_64__)
#include "../arch/x64/mm.h"
void native_panic(const char *what, void *frame);
#elif defined(__aarch64__)
#include "../arch/arm64/arm.h"
#endif

#if defined(__aarch64__)
/* ARM64: word loops (-ffreestanding keeps the compiler from making them calls to themselves) */
void *memset(void *d, int c, usize n) {
    u8 *p = d;
    u64 w = (u8)c * 0x0101010101010101ull;
    while (n && ((usize)p & 7)) { *p++ = (u8)c; n--; }
    for (; n >= 8; n -= 8, p += 8) *(volatile u64 *)p = w;
    while (n--) *p++ = (u8)c;
    return d;
}
void *memcpy(void *d, const void *s, usize n) {
    u8 *dp = d;
    const u8 *sp = s;
    if ((((usize)dp | (usize)sp) & 7) == 0)
        for (; n >= 8; n -= 8, dp += 8, sp += 8) *(volatile u64 *)dp = *(const u64 *)sp;
    while (n--) *dp++ = *sp++;
    return d;
}
#else
void *memset(void *d, int c, usize n) {
    void *p = d;
    __asm__ volatile("rep stosb" : "+D"(p), "+c"(n) : "a"(c) : "memory");
    return d;
}

/* x86 string instructions: fast-string microcode on Silvermont/Airmont moves
 * whole cache lines, several times quicker than a C loop for frame buffers. */
void *memcpy(void *d, const void *s, usize n) {
    void *dst = d;
    usize words = n / sizeof(usize), tail = n % sizeof(usize);
#if defined(__x86_64__)
    __asm__ volatile("rep movsq" : "+D"(dst), "+S"(s), "+c"(words) : : "memory");
#else
    __asm__ volatile("rep movsl" : "+D"(dst), "+S"(s), "+c"(words) : : "memory");
#endif
    __asm__ volatile("rep movsb" : "+D"(dst), "+S"(s), "+c"(tail) : : "memory");
    return d;
}
#endif

void *memmove(void *d, const void *s, usize n) {
    u8 *dp = d;
    const u8 *sp = s;
    if (dp < sp) return memcpy(d, s, n);
    while (n--) dp[n] = sp[n];
    return d;
}

int memcmp(const void *a, const void *b, usize n) {
    const u8 *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y) return *x - *y;
    return 0;
}

usize strlen(const char *s) { usize n = 0; while (s[n]) n++; return n; }

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return (u8)*a - (u8)*b;
}

char *strchr(const char *s, int c) {
    for (; *s; s++) if (*s == (char)c) return (char *)s;
    return c ? NULL : (char *)s;
}

int strncmp(const char *a, const char *b, usize n) {
    for (; n; n--, a++, b++) {
        if (*a != *b) return (u8)*a - (u8)*b;
        if (!*a) return 0;
    }
    return 0;
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    for (;; s++) { if (*s == (char)c) last = s; if (!*s) break; }
    return (char *)last;
}

char *strstr(const char *h, const char *n) {
    usize nl = strlen(n);
    if (!nl) return (char *)h;
    for (; *h; h++) if (*h == *n && !memcmp(h, n, nl)) return (char *)h;
    return NULL;
}

void strlcat(char *d, const char *s, usize cap) {
    usize l = strlen(d);
    if (l + 1 < cap) strlcpy(d + l, s, cap - l);
}

void strlcpy(char *d, const char *s, usize cap) {
    usize i = 0;
    if (!cap) return;
    for (; s[i] && i + 1 < cap; i++) d[i] = s[i];
    d[i] = 0;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

int str_icontains(const char *hay, const char *needle) {
    if (!*needle) return 1;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && lower(*h) == lower(*n)) h++, n++;
        if (!*n) return 1;
    }
    return 0;
}

usize str16len(const c16 *s) { usize n = 0; while (s[n]) n++; return n; }

void str16_to_utf8(char *d, usize cap, const c16 *s) {
    usize o = 0;
    for (; *s && o + 4 < cap; s++) {
        c16 c = *s;
        if (c < 0x80) d[o++] = (char)c;
        else if (c < 0x800) { d[o++] = 0xc0 | (c >> 6); d[o++] = 0x80 | (c & 63); }
        else { d[o++] = 0xe0 | (c >> 12); d[o++] = 0x80 | ((c >> 6) & 63); d[o++] = 0x80 | (c & 63); }
    }
    d[o] = 0;
}

void utf8_to_str16(c16 *d, usize cap, const char *s) {
    usize o = 0;
    while (*s && o + 1 < cap) {
        u8 c = (u8)*s++;
        if (c < 0x80) d[o++] = c;
        else if ((c & 0xe0) == 0xc0 && *s) { d[o++] = ((c & 31) << 6) | (*s++ & 63); }
        else if ((c & 0xf0) == 0xe0 && s[0] && s[1]) {
            d[o++] = ((c & 15) << 12) | ((s[0] & 63) << 6) | (s[1] & 63); s += 2;
        } else d[o++] = '?';
    }
    d[o] = 0;
}

/* ---- tiny printf: %d %i %u %x %X %s %c %S(UCS-2) %%, flags 0/-, width, l/ll ---- */
typedef struct { char *buf; usize cap, len; } out_t;

static void put(out_t *o, char c) {
    if (o->len + 1 < o->cap) o->buf[o->len] = c;
    o->len++;
}

static void put_num(out_t *o, u64 v, int base, int upper, int neg, int width, char pad, int left) {
    char tmp[24];
    int n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do { tmp[n++] = digits[v % (u64)base]; v /= (u64)base; } while (v);
    if (neg) tmp[n++] = '-';
    int padn = width > n ? width - n : 0;
    if (!left && pad == '0' && neg) { put(o, '-'); n--; }
    if (!left) while (padn--) put(o, pad);
    while (n) put(o, tmp[--n]);
    if (left) while (padn-- > 0) put(o, ' ');
}

int vfmt(char *buf, usize cap, const char *f, va_list ap) {
    out_t o = { buf, cap, 0 };
    for (; *f; f++) {
        if (*f != '%') { put(&o, *f); continue; }
        f++;
        int left = 0, width = 0, lng = 0;
        char pad = ' ';
        if (*f == '-') { left = 1; f++; }
        if (*f == '0') { pad = '0'; f++; }
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        while (*f == 'l') { lng++; f++; }
        if (*f == 'z') { lng = sizeof(usize) == 8 ? 2 : 0; f++; }
        switch (*f) {
        case 'd': case 'i': {
            i64 v = lng >= 2 ? va_arg(ap, i64) : (i64)va_arg(ap, int);
            put_num(&o, v < 0 ? (u64)-v : (u64)v, 10, 0, v < 0, width, pad, left);
            break;
        }
        case 'u': case 'x': case 'X': {
            u64 v = lng >= 2 ? va_arg(ap, u64) : (u64)va_arg(ap, unsigned);
            put_num(&o, v, *f == 'u' ? 10 : 16, *f == 'X', 0, width, pad, left);
            break;
        }
        case 'p':
            put(&o, '0'); put(&o, 'x');
            put_num(&o, (u64)(usize)va_arg(ap, void *), 16, 0, 0, 0, ' ', 0);
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int n = (int)strlen(s);
            if (!left) for (int i = n; i < width; i++) put(&o, ' ');
            while (*s) put(&o, *s++);
            if (left) for (int i = n; i < width; i++) put(&o, ' ');
            break;
        }
        case 'S': {
            const c16 *s = va_arg(ap, const c16 *);
            char tmp[256];
            str16_to_utf8(tmp, sizeof tmp, s ? s : u"(null)");
            for (char *p = tmp; *p; p++) put(&o, *p);
            break;
        }
        case 'c': put(&o, (char)va_arg(ap, int)); break;
        case '%': put(&o, '%'); break;
        default: put(&o, '%'); put(&o, *f); break;
        }
    }
    if (cap) buf[o.len < cap ? o.len : cap - 1] = 0;
    return (int)o.len;
}

int fmt(char *buf, usize cap, const char *f, ...) {
    va_list ap;
    va_start(ap, f);
    int n = vfmt(buf, cap, f, ap);
    va_end(ap);
    return n;
}

void fmt_bytes(char *buf, usize cap, u64 b) {
    static const char *unit[] = { "B", "KB", "MB", "GB", "TB" };
    int u = 0;
    u64 whole = b, frac = 0;
    while (whole >= 1024 && u < 4) { frac = (whole % 1024) * 10 / 1024; whole /= 1024; u++; }
    if (u == 0 || whole >= 100) fmt(buf, cap, "%llu %s", whole, unit[u]);
    else fmt(buf, cap, "%llu.%llu %s", whole, frac, unit[u]);
}

/* ---- memory: the firmware's pool allocator is our heap ---------------- */
/* The firmware's pool while it runs; the kernel heap afterwards. */
void *kalloc(usize n) {
#if defined(__x86_64__) || defined(__aarch64__)
    if (k.native) return heap_alloc(n);
#endif
    void *p = NULL;
    if (EFI_ERROR(k.bs->AllocatePool(EfiLoaderData, n ? n : 1, &p)) || !p)
        panic("out of memory");
    memset(p, 0, n);
    return p;
}

void kfree(void *p) {
    if (!p) return;
#if defined(__x86_64__) || defined(__aarch64__)
    if (k.native) {                 /* pool blocks from before the handover are simply kept */
        if (heap_owns(p)) heap_free(p);
        return;
    }
#endif
    k.bs->FreePool(p);
}

/* ---- logging ------------------------------------------------------------ */
#define LOG_LINES 128
#define LOG_COLS  240
static char log_ring[LOG_LINES][LOG_COLS];
static int log_count;

void klog(const char *f, ...) {
    char line[LOG_COLS];
    c16 wide[LOG_COLS + 4];
    va_list ap;
    va_start(ap, f);
    vfmt(line, sizeof line, f, ap);
    va_end(ap);
    /* one line at a time: a thread preempted halfway through its line would let another
     * line land in the middle of it (the QEMU test reads the log back) */
#if defined(__x86_64__) || defined(__aarch64__)
    u64 fl = k.native ? irq_save() : 0;
#endif
    strlcpy(log_ring[log_count % LOG_LINES], line, sizeof line);
    log_count++;
    if (k.native || k.graphics_up) {
        if (uart_present()) { uart_write(line); uart_write("\n"); }
#if defined(__x86_64__) || defined(__aarch64__)
        if (k.native) irq_restore(fl);
#endif
    } else if (k.st && k.st->ConOut) {
        utf8_to_str16(wide, LOG_COLS + 2, line);
        usize n = str16len(wide);
        wide[n] = '\r'; wide[n + 1] = '\n'; wide[n + 2] = 0;
        k.st->ConOut->OutputString(k.st->ConOut, wide);
    }
}

const char *klog_line(int i) {
    int first = log_count > LOG_LINES ? log_count - LOG_LINES : 0;
    if (i < 0 || first + i >= log_count) return NULL;
    return log_ring[(first + i) % LOG_LINES];
}

void panic(const char *msg) {
#if defined(__x86_64__) || defined(__aarch64__)
    if (k.native) native_panic(msg, NULL);
#endif
    k.graphics_up = 0;
    if (k.st && k.st->ConOut) k.st->ConOut->SetAttribute(k.st->ConOut, 0x0f);   /* visible again */
    klog("*** QRT kernel panic: %s", msg);
    for (;;) k.bs->Stall(1000000);
}

/* ---- math ------------------------------------------------------------ */
float fsqrt(float x) {
    if (x <= 0) return 0;
    return __builtin_sqrtf(x);
}

float fsin(float x) {
    /* range-reduce to [-pi, pi], then a 7th-order minimax-ish polynomial */
    const float tau = 2 * PI_F;
    int n = (int)(x / tau);
    x -= (float)n * tau;
    if (x > PI_F) x -= tau;
    if (x < -PI_F) x += tau;
    /* fold into [-pi/2, pi/2] */
    if (x > PI_F / 2) x = PI_F - x;
    if (x < -PI_F / 2) x = -PI_F - x;
    float x2 = x * x;
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72))));
}

float fcos(float x) { return fsin(x + PI_F / 2); }

static u32 rng_state = 0x9e3779b9;
u32 rand32(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* ---- libgcc helpers needed for 64-bit arithmetic on IA32 --------------- */
#if defined(__i386__)
static u64 udivmod64(u64 n, u64 d, u64 *rem) {
    u64 q = 0, r = 0;
    if (d == 0) { if (rem) *rem = 0; return ~0ULL; }
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= 1ULL << i; }
    }
    if (rem) *rem = r;
    return q;
}
u64 __udivdi3(u64 n, u64 d) { return udivmod64(n, d, NULL); }
u64 __umoddi3(u64 n, u64 d) { u64 r; udivmod64(n, d, &r); return r; }
i64 __divdi3(i64 n, i64 d) {
    int neg = (n < 0) ^ (d < 0);
    u64 q = udivmod64(n < 0 ? -(u64)n : (u64)n, d < 0 ? -(u64)d : (u64)d, NULL);
    return neg ? -(i64)q : (i64)q;
}
i64 __moddi3(i64 n, i64 d) {
    u64 r;
    udivmod64(n < 0 ? -(u64)n : (u64)n, d < 0 ? -(u64)d : (u64)d, &r);
    return n < 0 ? -(i64)r : (i64)r;
}
#endif
