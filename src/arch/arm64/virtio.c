/*
 * virtio.c - QEMU's virtio-mmio input devices on the virt board: a keyboard (Linux key
 * codes -> key events) and a tablet, which plays the phone's touch screen (absolute
 * position + button -> finger down / move / up).  Polled; virtio 1.0 MMIO (QEMU:
 * -global virtio-mmio.force-legacy=false).
 */
#include "arm.h"

#define NQ 64
struct vdesc { u64 addr; u32 len; u16 flags, next; };
struct vin { u64 base; int tablet; struct vdesc *d; volatile u16 *avail; volatile u16 *used; u16 last; u8 *ev; int x, y, down, shift, ctrl; };
static struct vin ins[4];
static int nin;

static u8 qmem[4][3][4096] __attribute__((aligned(4096)));

static void dev_init(u64 b, int idx) {
    struct vin *v = &ins[nin];
    W32(b + 0x70, 0);                                     /* reset */
    W32(b + 0x70, 1 | 2);                                 /* acknowledge, driver */
    W32(b + 0x24, 1); W32(b + 0x20, 1);                   /* VIRTIO_F_VERSION_1 */
    W32(b + 0x24, 0); W32(b + 0x20, 0);
    W32(b + 0x70, 1 | 2 | 8);                             /* features OK */
    if (!(R32(b + 0x70) & 8)) return;
    W32(b + 0x30, 0);
    u32 max = R32(b + 0x34);
    u32 n = max < NQ ? max : NQ;
    if (!n) return;
    v->d = (struct vdesc *)qmem[idx][0];
    v->avail = (volatile u16 *)qmem[idx][1];
    v->used = (volatile u16 *)qmem[idx][2];
    v->ev = qmem[idx][0] + 2048;                          /* 8-byte events after the descriptors */
    for (u32 i = 0; i < n; i++) {
        v->d[i].addr = (u64)(usize)(v->ev + 8 * i); v->d[i].len = 8; v->d[i].flags = 2; v->d[i].next = 0;
        v->avail[2 + i] = (u16)i;
    }
    v->avail[1] = (u16)n;
    W32(b + 0x38, n);
    u64 d = (u64)(usize)v->d, a = (u64)(usize)v->avail, u = (u64)(usize)v->used;
    W32(b + 0x80, (u32)d); W32(b + 0x84, (u32)(d >> 32));
    W32(b + 0x90, (u32)a); W32(b + 0x94, (u32)(a >> 32));
    W32(b + 0xa0, (u32)u); W32(b + 0xa4, (u32)(u >> 32));
    W32(b + 0x44, 1);                                     /* queue ready */
    W32(b + 0x70, 1 | 2 | 8 | 4);                         /* driver OK */
    W32(b + 0x50, 0);
    /* a tablet has absolute axes: config select EV_BITS (0x11), subsel EV_ABS (3) */
    *(volatile u8 *)(usize)(b + 0x100) = 0x11;
    *(volatile u8 *)(usize)(b + 0x101) = 3;
    v->tablet = *(volatile u8 *)(usize)(b + 0x102) != 0;
    v->base = b;
    v->last = 0;
    klog("virtio: input device at %llx (%s)", b, v->tablet ? "tablet: the touch screen" : "keyboard");
    nin++;
}

static int mmio_one(int c, void *arg) {
    (void)arg;
    u64 a, s;
    if (nin >= 4 || !fdt_reg(c, 0, &a, &s)) return 0;
    if (R32(a) != 0x74726976 || R32(a + 4) != 2 || R32(a + 8) != 18) return 0;   /* "virt", v2, input */
    dev_init(a, nin);
    return 0;
}

void virtio_input_init(void) {
    for (int n = -1; (n = fdt_find_compatible(n, "virtio,mmio")) >= 0; ) mmio_one(n, NULL);
}

/* Linux key codes -> (scan, char); HID usages as in the USB keyboard driver */
static void key_of(int code, int shift, int ctrl, u16 *scan, c16 *ch) {
    static const char row1[] = "1234567890-=", row2[] = "qwertyuiop[]", row3[] = "asdfghjkl;'`", row4[] = "\\zxcvbnm,./";
    static const char row1s[] = "!@#$%^&*()_+", row2s[] = "QWERTYUIOP{}", row3s[] = "ASDFGHJKL:\"~", row4s[] = "|ZXCVBNM<>?";
    *scan = 0; *ch = 0;
    char c = 0;
    if (code >= 2 && code <= 13) c = shift ? row1s[code - 2] : row1[code - 2];
    else if (code >= 16 && code <= 27) c = shift ? row2s[code - 16] : row2[code - 16];
    else if (code >= 30 && code <= 41) c = shift ? row3s[code - 30] : row3[code - 30];
    else if (code >= 43 && code <= 53) c = shift ? row4s[code - 43] : row4[code - 43];
    if (c) { *ch = (c16)(ctrl && ((c | 32) >= 'a' && (c | 32) <= 'z') ? (c & 0x1f) : c); return; }
    switch (code) {
    case 28: case 96: *ch = '\r'; break;
    case 1: *scan = SCAN_ESC; break;
    case 14: *ch = 8; break;
    case 15: *ch = '\t'; break;
    case 57: *ch = ' '; break;
    case 102: *scan = SCAN_HOME; break;
    case 104: *scan = SCAN_PGUP; break;
    case 111: *scan = 0x08; break;
    case 107: *scan = SCAN_END; break;
    case 109: *scan = SCAN_PGDN; break;
    case 106: *scan = SCAN_RIGHT; break;
    case 105: *scan = SCAN_LEFT; break;
    case 108: *scan = SCAN_DOWN; break;
    case 103: *scan = SCAN_UP; break;
    case 115: *scan = SCAN_VOLUP; break;
    case 114: *scan = SCAN_VOLDN; break;
    case 116: *scan = SCAN_POWER; break;
    }
}

int virtio_input_poll(event_t *out, int max) {
    int n = 0;
    for (int i = 0; i < nin; i++) {
        struct vin *v = &ins[i];
        __asm__ volatile("dmb ish" ::: "memory");
        u16 used = v->used[1];
        while (v->last != used && n < max) {
            u32 slot = *(volatile u32 *)(v->used + 2 + 4 * (v->last % NQ));     /* used ring: {u32 id, u32 len} */
            const u8 *e = v->ev + 8 * slot;
            u16 type = (u16)(e[0] | e[1] << 8), code = (u16)(e[2] | e[3] << 8);
            i32 value = (i32)(e[4] | e[5] << 8 | e[6] << 16 | (u32)e[7] << 24);
            if (type == 1) {                                            /* EV_KEY */
                if (code == 42 || code == 54) v->shift = value != 0;
                else if (code == 29 || code == 97) v->ctrl = value != 0;
                else if (code == 0x110 || code == 0x14a) {               /* the tablet's button: a finger */
                    event_t ev = { .type = value ? EV_DOWN : EV_UP, .x = v->x, .y = v->y, .fingers = value ? 1 : 0 };
                    v->down = value != 0;
                    out[n++] = ev;
                } else if (value) {
                    event_t ev = { .type = EV_KEY };
                    key_of(code, v->shift, v->ctrl, &ev.scan, &ev.ch);
                    if (ev.scan || ev.ch) out[n++] = ev;
                }
            } else if (type == 3 && code <= 1) {                        /* EV_ABS: 0..32767 */
                int px = (int)((i64)value * (code == 0 ? (int)k.fb_w - 1 : (int)k.fb_h - 1) / 32767);
                if (code == 0) v->x = px; else v->y = px;
            } else if (type == 0 && v->tablet && v->down) {             /* SYN: a finger moved */
                if (n && out[n - 1].type == EV_MOVE) { out[n - 1].x = v->x; out[n - 1].y = v->y; }
                else { event_t ev = { .type = EV_MOVE, .x = v->x, .y = v->y, .fingers = 1 }; out[n++] = ev; }
            }
            v->avail[2 + (v->avail[1] % NQ)] = (u16)slot;             /* give the buffer back */
            __asm__ volatile("dmb ish" ::: "memory");
            v->avail[1]++;
            v->last++;
        }
        if (v->last == used) W32(v->base + 0x50, 0);
    }
    return n;
}
