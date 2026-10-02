/*
 * hidmouse.c - USB HID mice (and absolute pointers) from their report
 * descriptor, as Linux's hid-input and OpenBSD's hidms(4) read them: the
 * buttons (Button page), X and Y (relative for mice, absolute for tablets
 * and KVMs), the wheel and horizontal pan (Consumer page AC Pan).  Devices
 * whose descriptor cannot be read fall back to the boot protocol (three or
 * four bytes: buttons, dx, dy, wheel).
 */
#include "hidmouse.h"
#ifdef HID_HOST_TEST
#include <string.h>
#endif

#define USAGE(page, id) (((u32)(page) << 16) | (id))

int hm_parse(hidmouse_t *m, const u8 *d, int len) {
    u32 usage_page = 0, report_size = 0, report_count = 0;
    i32 lmin = 0, lmax = 0;
    u8 report_id = 0;
    u32 usages[16];
    int n_usages = 0, have_range = 0;
    u32 umin = 0, umax = 0;
    u16 offsets[256];
    struct { u32 usage; u8 id; hm_field_t f; } fl[48];      /* every variable input field */
    int nf = 0;
    memset(offsets, 0, sizeof offsets);
    memset(m, 0, sizeof *m);

    for (int i = 0; i < len;) {
        u8 b = d[i++];
        if (b == 0xfe) {                                   /* long item */
            if (i + 1 >= len) break;
            i += 2 + d[i];
            continue;
        }
        int size = (b & 3) == 3 ? 4 : (b & 3);
        int type = (b >> 2) & 3, tag = b >> 4;
        if (i + size > len) break;
        u32 v = 0;
        for (int j = 0; j < size; j++) v |= (u32)d[i + j] << (8 * j);
        i32 sv = size == 1 ? (i8)v : size == 2 ? (i16)v : (i32)v;
        i += size;

        if (type == 1) {                                    /* global */
            switch (tag) {
            case 0: usage_page = v; break;
            case 1: lmin = sv; break;
            case 2: lmax = lmin >= 0 && size < 4 ? (i32)v : sv; break;   /* with min >= 0, 0xff is 255 */
            case 7: report_size = v; break;
            case 8: report_id = (u8)v; m->uses_ids = 1; break;
            case 9: report_count = v; break;
            }
        } else if (type == 2) {                             /* local */
            u32 u = size == 4 ? v : (usage_page << 16) | v;
            if (tag == 0 && n_usages < 16) usages[n_usages++] = u;
            else if (tag == 1) { umin = u; have_range = 1; }
            else if (tag == 2) { umax = u; have_range = 1; }
        } else if (type == 0) {                             /* main */
            if (tag == 8) {                                 /* Input */
                int is_const = v & 1, is_var = v & 2, is_rel = (v & 4) != 0;
                for (u32 k = 0; k < report_count; k++) {
                    u32 u = 0;
                    if (have_range) u = umin + k <= umax ? umin + k : 0;
                    else if (n_usages) u = usages[k < (u32)n_usages ? k : (u32)n_usages - 1];
                    u16 off = offsets[report_id];
                    if (!is_var) { offsets[report_id] += (u16)(report_size * report_count); break; }
                    offsets[report_id] += (u16)report_size;
                    if (is_const || !u || nf >= (int)ARRAY_LEN(fl)) continue;
                    fl[nf].usage = u; fl[nf].id = report_id;
                    fl[nf].f = (hm_field_t){ off, (u16)report_size, lmin, lmax, 1, (u8)is_rel };
                    nf++;
                }
            }
            if (tag == 8 || tag == 9 || tag == 0xb || tag == 0xa) { n_usages = 0; have_range = 0; }
        }
    }
    /* the pointer is the report with the first X axis; its buttons, Y, wheel and pan */
    int xi = -1;
    for (int i = 0; i < nf && xi < 0; i++) if (fl[i].usage == USAGE(1, 0x30)) xi = i;
    if (xi < 0) return 0;
    m->report_id = fl[xi].id;
    m->x = fl[xi].f;
    for (int i = 0; i < nf; i++) {
        if (fl[i].id != m->report_id) continue;
        u32 u = fl[i].usage;
        hm_field_t *f = u == USAGE(1, 0x31) ? &m->y : u == USAGE(1, 0x38) ? &m->wheel : u == USAGE(0x0c, 0x238) ? &m->pan : NULL;
        if (f && !f->present) *f = fl[i].f;
        if (u >> 16 == 9 && (u & 0xffff) >= 1 && (u & 0xffff) <= 8 && fl[i].f.bits == 1) {
            int bn = (int)(u & 0xffff) - 1;
            m->btn_off[bn] = fl[i].f.off;
            if (bn + 1 > m->nbuttons) m->nbuttons = bn + 1;
        }
    }
    return m->y.present && m->x.bits && m->y.bits;
}

void hm_boot(hidmouse_t *m) {
    memset(m, 0, sizeof *m);
    m->boot = 1;
    m->nbuttons = 3;
    for (int i = 0; i < 3; i++) m->btn_off[i] = (u16)i;
    m->x = (hm_field_t){ 8, 8, -127, 127, 1, 1 };
    m->y = (hm_field_t){ 16, 8, -127, 127, 1, 1 };
    m->wheel = (hm_field_t){ 24, 8, -127, 127, 1, 1 };
}

static i32 field(const hm_field_t *f, const u8 *p, int len) {
    u32 v = 0;
    for (int i = 0; i < f->bits && i < 32; i++) {
        int bit = f->off + i;
        if (bit / 8 >= len) return 0;
        v |= (u32)((p[bit / 8] >> (bit % 8)) & 1) << i;
    }
    if (f->lmin < 0 && f->bits < 32 && (v >> (f->bits - 1)) & 1) v |= ~0u << f->bits;   /* sign-extend */
    return (i32)v;
}

int hm_decode(const hidmouse_t *m, const u8 *rep, int len, mouse_report_t *out) {
    memset(out, 0, sizeof *out);
    if (m->uses_ids && !m->boot) {
        if (len < 1 || rep[0] != m->report_id) return 0;
        rep++; len--;
    }
    if (len < (m->boot ? 3 : 1)) return 0;
    for (int b = 0; b < m->nbuttons; b++)
        if (m->btn_off[b] / 8 < len && (rep[m->btn_off[b] / 8] >> (m->btn_off[b] % 8)) & 1) out->buttons |= 1u << b;
    i32 x = field(&m->x, rep, len), y = field(&m->y, rep, len);
    if (m->x.rel) { out->x = x; out->y = y; }
    else {
        out->abs = 1;
        i32 xr = m->x.lmax - m->x.lmin, yr = m->y.lmax - m->y.lmin;
        out->x = (int)((i64)(CLAMP(x, m->x.lmin, m->x.lmax) - m->x.lmin) * 65535 / (xr > 0 ? xr : 1));
        out->y = (int)((i64)(CLAMP(y, m->y.lmin, m->y.lmax) - m->y.lmin) * 65535 / (yr > 0 ? yr : 1));
    }
    if (m->wheel.present && (!m->boot || len >= 4)) out->wheel = field(&m->wheel, rep, len);
    if (m->pan.present) out->pan = field(&m->pan, rep, len);
    return 1;
}
