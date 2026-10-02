/*
 * touch.c - native touchscreen for the Dell Venue 8 Pro 5855.
 *
 * From the tablet's DSDT: every touch variant hangs off \_SB.PCI0.I2C6,
 * which is the LPSS I2C controller at PCI 00:18.6 (8086:22c6).  Which chip
 * is fitted depends on the board ID in the firmware's GNVS area:
 *   TCS0/TCS3  ATML1000  Atmel maXTouch      0x4A  HID descriptor at 0x0000
 *   SYN1       SYNP1000  Synaptics           0x2C  HID descriptor at 0x0020
 *   WCOM       WCOM48xx  Wacom touch + pen   0x0A  HID descriptor at 0x0001
 * On the first tested tablet (BDID 3, WLID 4) only the Wacom answered: it is
 * the touchscreen itself (056a:4808, 5 fingers, 0..4304 x 0..6888) as well
 * as the pen digitizer.
 * Rather than trust decoded board variables, we probe each address and use
 * the one that answers with a valid HID descriptor containing fingers.
 */
#include "touch.h"
#include "pci.h"

ntouch_t nt;

static const struct { u8 addr; u16 reg; const char *name; } candidates[TOUCH_CANDIDATES] = {
    { 0x4a, 0x0000, "Atmel maXTouch" },
    { 0x2c, 0x0020, "Synaptics" },
    { 0x0a, 0x0001, "Wacom touch + pen" },
};

/* The firmware's global NVS area, found in this board's DSDT (OEM table id
 * CBX3, region length 0x36C); 0 on any other machine. */
u32 venue_gnvs(void) {
    static u32 cached, done;
    if (done) return cached;
    done = 1;
    if (!k.dsdt || memcmp(k.dsdt + 16, "CBX3", 4)) return 0;
    for (u32 i = 36; i + 16 < k.dsdt_len; i++) {
        const u8 *p = k.dsdt + i;
        /* OperationRegion (GNVS, SystemMemory, DWordConst addr, WordConst 0x036C) */
        if (p[0] == 0x5b && p[1] == 0x80 && !memcmp(p + 2, "GNVS", 4) && p[6] == 0 && p[7] == 0x0c &&
            p[12] == 0x0b && (p[13] | p[14] << 8) == 0x36c)
            return cached = p[8] | p[9] << 8 | p[10] << 16 | (u32)p[11] << 24;
    }
    return 0;
}

/* GNVS offsets in this DSDT */
static void read_board_vars(void) {
    u32 addr = venue_gnvs();
    if (!addr) return;
    const volatile u8 *g = (const volatile u8 *)(usize)addr;
    nt.gnvs = addr;
    nt.osid = g[38]; nt.itsa = g[793]; nt.bdid = g[802]; nt.mpnl = g[842]; nt.wlid = g[871];
    nt.board_valid = 1;
}

void ntouch_probe(void) {
    if (nt.active) return;
    nt.probed = 1;
    nt.primary = -1;
    read_board_vars();
    nt.bus_err = dwi2c_find(&nt.bus, 0, 0x18, 6);
    for (int i = 0; i < TOUCH_CANDIDATES; i++) {
        touch_candidate_t *c = &nt.cand[i];
        c->addr = candidates[i].addr;
        c->desc_reg = candidates[i].reg;
        c->name = candidates[i].name;
        c->result = 1;
        if (nt.bus_err) continue;
        c->result = i2chid_probe(&c->hid, &nt.bus, c->addr, c->desc_reg);
        if (!c->result && c->hid.nfingers && nt.primary < 0) nt.primary = i;
    }
    if (nt.bus_err) fmt(nt.status, sizeof nt.status, "I2C6 controller: %s", dwi2c_strerror(nt.bus_err));
    else if (nt.primary < 0) fmt(nt.status, sizeof nt.status, "controller found, no finger-touch device answered");
    else fmt(nt.status, sizeof nt.status, "ready: %s at 0x%02x", nt.cand[nt.primary].name, nt.cand[nt.primary].addr);
    if (nt.primary >= 0) dwi2c_save(&nt.bus);
    klog("touch: %s", nt.status);
}

int ntouch_active(void) { return nt.active; }

int ntouch_go_native(void) {
    if (!nt.probed) ntouch_probe();
    if (nt.primary < 0) return 0;
    i2chid_t *h = &nt.cand[nt.primary].hid;

    /* Detach every firmware driver bound to the I2C controller (and so its
     * touch child).  Forget the firmware pointers first: they are about to
     * be uninstalled. */
    k.n_abs = 0;
    k.splitter_abs = 0;
    EFI_STATUS s = k.bs->DisconnectController(nt.bus.pci_handle, NULL, NULL);
    if (EFI_ERROR(s)) {
        fmt(nt.status, sizeof nt.status, "firmware refused to release I2C6 (%llx) - kept firmware touch", (u64)s);
        hal_reprobe_input();
        return 0;
    }
    int err = i2chid_set_power(h, 1);
    if (!err) {
        /* confirm the chip still talks to us after the handover */
        u8 reg[2] = { (u8)h->desc_reg, (u8)(h->desc_reg >> 8) }, b[4];
        err = dwi2c_xfer(&nt.bus, h->addr, reg, 2, b, 4);
        if (!err && (b[0] | b[1] << 8) != 30) err = DW_ENODEV;
    }
    if (err) {
        fmt(nt.status, sizeof nt.status, "chip silent after handover (%s) - back to firmware", dwi2c_strerror(err));
        ntouch_revert();
        return 0;
    }
    h->tracking = h->down = 0;
    h->reports = h->empty_reads = h->errors = 0;
    nt.consecutive_errors = 0;
    nt.last_report_ms = nt.native_since = k_now_ms();
    nt.active = 1;
    fmt(nt.status, sizeof nt.status, "NATIVE: %s at 0x%02x via I2C6", nt.cand[nt.primary].name, h->addr);
    klog("touch: native driver active");
    return 1;
}

/*
 * After ExitBootServices: the firmware's I2C and HID drivers are gone and may
 * have reset the controller or put the chip to sleep on their way out.
 * Restore the controller, wake the chip, confirm it answers.
 */
int ntouch_native_resume(void) {
    if (nt.primary < 0) return 0;
    i2chid_t *h = &nt.cand[nt.primary].hid;
    if (dwi2c_restore(&nt.bus, 6)) { fmt(nt.status, sizeof nt.status, "I2C6 did not come back after the handover"); return 0; }
    int err = DW_ENODEV;
    for (int attempt = 0; attempt < 5 && err; attempt++) {
        i2chid_set_power(h, 1);
        hal_delay_us(20000);
        u8 reg[2] = { (u8)h->desc_reg, (u8)(h->desc_reg >> 8) }, b[4];
        err = dwi2c_xfer(&nt.bus, h->addr, reg, 2, b, 4);
        if (!err && (b[0] | b[1] << 8) != 30) err = DW_ENODEV;
        if (err && attempt == 2) {
            /* HID RESET (opcode 1), then drain the chip's reset acknowledgement */
            u8 cmd[4] = { (u8)h->d.cmd_reg, (u8)(h->d.cmd_reg >> 8), 0x00, 0x01 };
            dwi2c_xfer(&nt.bus, h->addr, cmd, 4, NULL, 0);
            hal_delay_us(100000);
            u8 tmp[64];
            i2chid_read(h, tmp, sizeof tmp);
        }
    }
    if (err) { fmt(nt.status, sizeof nt.status, "touch chip silent after handover (%s)", dwi2c_strerror(err)); return 0; }
    h->tracking = h->down = 0;
    h->reports = h->empty_reads = h->errors = 0;
    nt.consecutive_errors = 0;
    nt.last_report_ms = nt.native_since = k_now_ms();
    nt.active = 1;
    pci_set_driver(0, 0x18, 6, "dw-i2c + i2c-hid (QRT)");
    fmt(nt.status, sizeof nt.status, "NATIVE: %s at 0x%02x via I2C6", nt.cand[nt.primary].name, h->addr);
    return 1;
}

void ntouch_revert(void) {
    if (k.native) {                       /* no firmware to go back to: try to recover the chip */
        nt.active = 0;
        ntouch_native_resume();
        return;
    }
    nt.active = 0;
    if (nt.bus.pci_handle) k.bs->ConnectController(nt.bus.pci_handle, NULL, NULL, 1);
    hal_reprobe_input();
    klog("touch: handed back to firmware (%d touch devices)", k.n_abs);
}

int ntouch_poll(event_t *out, int max) {
    if (!nt.active || max <= 0) return 0;
    i2chid_t *h = &nt.cand[nt.primary].hid;
    int n = 0;
    if (!k.native && !h->reports && k_now_ms() - nt.native_since > 15000) {
        /* never saw a single touch report: don't leave the user without input */
        ntouch_revert();
        fmt(nt.status, sizeof nt.status, "no touch reports in 15 s - back to firmware touch");
        return 0;
    }
    /*
     * Drain everything the chip has queued, not just a few reports: a frame
     * can take tens of ms to draw on the Atom, and a digitizer reporting at
     * 100+ Hz would otherwise build a backlog that replays late (the pointer
     * "glides" after the finger has lifted).  Moves are coalesced to the
     * newest position; touch-down and lift are never dropped.
     */
    int pending_move = 0, mx = 0, my = 0;
    static int fingers = 1;                          /* the count the last event carried */
    for (int i = 0; i < 64 && n < max - 2; i++) {    /* a report can emit two events */
        u8 rep[64];
        int len = i2chid_read(h, rep, sizeof rep);
        if (len < 0) {
            if (++nt.consecutive_errors > 25) {
                fmt(nt.status, sizeof nt.status, "native touch lost the chip - back to firmware");
                ntouch_revert();
                return n;
            }
            break;
        }
        nt.consecutive_errors = 0;
        if (len == 0) break;
        memcpy(nt.last, rep, (usize)len);
        nt.last_len = len;
        nt.last_report_ms = k_now_ms();
        int was_down = h->down;
        if (!hid_touch_update(h, rep, len)) continue;
        /* 0..65535 -> physical pixels, honouring the same axis fix-ups as firmware touch */
        int ax = h->x, ay = h->y;
        if (k.touch_map & TOUCH_SWAP_XY) { int t = ax; ax = ay; ay = t; }
        int x = (int)((i64)ax * (k.fb_w - 1) / 65535), y = (int)((i64)ay * (k.fb_h - 1) / 65535);
        if (k.touch_map & TOUCH_FLIP_X) x = (int)k.fb_w - 1 - x;
        if (k.touch_map & TOUCH_FLIP_Y) y = (int)k.fb_h - 1 - y;
        int f = h->fingers ? h->fingers : 1;
        if (h->down && was_down) {
            if (f != fingers) {                         /* a finger more or less: say so now */
                out[n++] = (event_t){ .type = EV_MOVE, .x = x, .y = y, .fingers = f };
                fingers = f; pending_move = 0;
                continue;
            }
            pending_move = 1; mx = x; my = y; continue;
        }
        if (pending_move) { out[n++] = (event_t){ .type = EV_MOVE, .x = mx, .y = my, .fingers = fingers }; pending_move = 0; }
        out[n++] = (event_t){ .type = h->down ? EV_DOWN : EV_UP, .x = x, .y = y, .fingers = h->down ? f : 0 };
        fingers = h->down ? f : 1;
    }
    if (pending_move && n < max) out[n++] = (event_t){ .type = EV_MOVE, .x = mx, .y = my, .fingers = fingers };
    return n;
}

int ntouch_save(void) {
    char *b = kalloc(8192);
    usize o = 0;
#define P(...) do { if (o < 8192) o += fmt(b + o, 8192 - o, __VA_ARGS__); if (o > 8192) o = 8192; } while (0)
    P("QRT touch lab report\r\n\r\nstatus: %s\r\n", nt.status);
    if (nt.board_valid)
        P("board (GNVS @ %08x): OSID=%u BDID=%u MPNL=%u ITSA=0x%02x WLID=%u\r\n", nt.gnvs, nt.osid, nt.bdid, nt.mpnl, nt.itsa, nt.wlid);
    P("I2C6: %s, pci id %08x, DW comp %08x ver %08x param %08x, fifo tx %u rx %u\r\n\r\n",
      dwi2c_strerror(nt.bus_err), nt.bus.pci_id, nt.bus.comp_type, nt.bus.comp_ver, nt.bus.comp_param,
      nt.bus.tx_depth, nt.bus.rx_depth);
    for (int i = 0; i < TOUCH_CANDIDATES; i++) {
        touch_candidate_t *c = &nt.cand[i];
        hid_desc_t *d = &c->hid.d;
        P("%s @0x%02x reg 0x%04x: %s", c->name, c->addr, c->desc_reg, c->result == 1 ? "not tried" : dwi2c_strerror(c->result));
        if (c->result == DW_EABORT) P(" (abort source %08x)", nt.bus.last_abort);
        P("\r\n");
        if (c->result) continue;
        P("  HID desc len %u bcd %04x  VID:PID %04x:%04x ver %04x\r\n", d->desc_len, d->bcd, d->vid, d->pid, d->ver);
        P("  report desc %u bytes @%04x, input @%04x max %u, cmd @%04x data @%04x\r\n",
          d->rdesc_len, d->rdesc_reg, d->in_reg, d->in_max, d->cmd_reg, d->data_reg);
        P("  fingers %d, touch report id %u, X %d..%d, Y %d..%d, fields %d\r\n",
          c->hid.nfingers, c->hid.touch_report, c->hid.xmin, c->hid.xmax, c->hid.ymin, c->hid.ymax, c->hid.nf);
        if (c->hid.rdesc) {
            P("  report descriptor:");
            for (int j = 0; j < d->rdesc_len; j++) P("%s%02x", j % 32 ? " " : "\r\n    ", c->hid.rdesc[j]);
            P("\r\n");
        }
    }
    if (nt.primary >= 0) {
        i2chid_t *h = &nt.cand[nt.primary].hid;
        P("\r\nnative stats: reports %u, empty reads %u, errors %u\r\nlast report:", h->reports, h->empty_reads, h->errors);
        for (int j = 0; j < nt.last_len; j++) P(" %02x", nt.last[j]);
        P("\r\n");
    }
#undef P
    int ok = hwreport_write("touch.txt", b, o);
    kfree(b);
    return ok;
}
