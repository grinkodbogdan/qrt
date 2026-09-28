/*
 * hidparse.c - USB HID 1.11 report-descriptor parser, reduced to what a
 * touchscreen needs: find the finger collections (Digitizers page, usage
 * 0x22) and their tip/contact-id/X/Y fields, then follow one primary contact.
 */
#include "hidparse.h"
#ifdef HID_HOST_TEST
#include <string.h>
#endif

/* ---- report descriptor parser ---------------------------------------------- */
int hid_parse_report_desc(i2chid_t *h, const u8 *d, int len) {
    u32 usage_page = 0, report_size = 0, report_count = 0;
    i32 lmin = 0, lmax = 0;
    u8 report_id = 0;
    u32 usages[32];
    int n_usages = 0;
    u32 umin = 0, umax = 0;
    int have_range = 0;
    static u16 offsets[256];
    int finger_stack[16], depth = 0, finger = -1;
    memset(offsets, 0, sizeof offsets);
    h->nf = 0; h->nfingers = 0; h->uses_ids = 0; h->touch_report = 0;

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
            case 2: lmax = sv; break;
            case 7: report_size = v; break;
            case 8: report_id = (u8)v; h->uses_ids = 1; break;
            case 9: report_count = v; break;
            }
        } else if (type == 2) {                             /* local */
            u32 u = size == 4 ? v : (usage_page << 16) | v;
            if (tag == 0 && n_usages < 32) usages[n_usages++] = u;
            else if (tag == 1) { umin = u; have_range = 1; }
            else if (tag == 2) { umax = u; have_range = 1; }
        } else if (type == 0) {                             /* main */
            if (tag == 0xa) {                               /* Collection */
                if (depth < 16) finger_stack[depth++] = finger;
                if (n_usages && usages[0] == HID_USAGE(0x0d, 0x22)) finger = h->nfingers++;
            } else if (tag == 0xc) {                        /* End Collection */
                if (depth > 0) finger = finger_stack[--depth];
            } else if (tag == 8) {                          /* Input */
                int is_const = v & 1, is_var = v & 2;
                for (u32 k2 = 0; k2 < report_count; k2++) {
                    u32 u = 0;
                    if (have_range) u = umin + k2 <= umax ? umin + k2 : umax;
                    else if (n_usages) u = usages[k2 < (u32)n_usages ? k2 : (u32)n_usages - 1];
                    if (!is_const && is_var && u && h->nf < HID_MAX_FIELDS &&
                        (u == U_X || u == U_Y || u == U_TIP || u == U_CONTACT || u == U_CCOUNT)) {
                        hid_field_t *f = &h->f[h->nf++];
                        f->usage = u; f->report_id = report_id;
                        f->bit_off = offsets[report_id]; f->bits = (u16)report_size;
                        f->lmin = lmin; f->lmax = lmax; f->finger = (i16)finger;
                        if (finger >= 0 && u == U_X) {
                            h->touch_report = report_id;
                            if (h->xmax <= h->xmin) { h->xmin = lmin; h->xmax = lmax; }
                        }
                        if (finger >= 0 && u == U_Y && h->ymax <= h->ymin) { h->ymin = lmin; h->ymax = lmax; }
                    }
                    if (!is_var) { offsets[report_id] += (u16)(report_size * report_count); break; }
                    offsets[report_id] += (u16)report_size;
                }
            }
            if (tag == 8 || tag == 9 || tag == 0xb || tag == 0xa) { n_usages = 0; have_range = 0; }
        }
    }
    return h->nfingers > 0 && h->xmax > h->xmin && h->ymax > h->ymin;
}

static u32 get_bits(const u8 *p, int len, int off, int bits) {
    u32 v = 0;
    for (int i = 0; i < bits && i < 32; i++) {
        int bit = off + i;
        if (bit / 8 >= len) break;
        v |= (u32)((p[bit / 8] >> (bit % 8)) & 1) << i;
    }
    return v;
}

int hid_touch_update(i2chid_t *h, const u8 *rep, int len) {
    u8 id = 0;
    if (h->uses_ids) { if (len < 1) return 0; id = rep[0]; rep++; len--; }
    if (id != h->touch_report) return 0;
    h->reports++;

    /* gather per-finger values for this report */
    struct { int tip, cid, has_x, has_y; i32 x, y; } fg[10];
    int nfg = MIN(h->nfingers, 10);
    memset(fg, 0, sizeof fg);
    for (int i = 0; i < nfg; i++) fg[i].cid = -1;
    for (int i = 0; i < h->nf; i++) {
        hid_field_t *f = &h->f[i];
        if (f->report_id != id || f->finger < 0 || f->finger >= nfg) continue;
        u32 v = get_bits(rep, len, f->bit_off, f->bits);
        if (f->usage == U_TIP) fg[f->finger].tip = v != 0;
        else if (f->usage == U_CONTACT) fg[f->finger].cid = (int)v;
        else if (f->usage == U_X) { fg[f->finger].x = (i32)v; fg[f->finger].has_x = 1; }
        else if (f->usage == U_Y) { fg[f->finger].y = (i32)v; fg[f->finger].has_y = 1; }
    }

    int pick = -1;
    if (h->tracking) {
        for (int i = 0; i < nfg; i++)
            if (fg[i].has_x && fg[i].cid == h->track_id) { pick = i; break; }
        if (pick < 0) return 0;          /* report is about other fingers (hybrid mode) */
        if (!fg[pick].tip) {             /* our finger lifted */
            h->tracking = 0;
            h->down = 0;
            return 1;
        }
    } else {
        for (int i = 0; i < nfg; i++)
            if (fg[i].has_x && fg[i].tip) { pick = i; break; }
        if (pick < 0) return 0;
        h->tracking = 1;
        h->track_id = fg[pick].cid;
    }
    i32 xr = h->xmax - h->xmin, yr = h->ymax - h->ymin;
    int nx = (int)((i64)(CLAMP(fg[pick].x, h->xmin, h->xmax) - h->xmin) * 65535 / (xr ? xr : 1));
    int ny = (int)((i64)(CLAMP(fg[pick].y, h->ymin, h->ymax) - h->ymin) * 65535 / (yr ? yr : 1));
    int changed = !h->down || nx != h->x || ny != h->y;
    h->down = 1; h->x = nx; h->y = ny;
    return changed;
}

