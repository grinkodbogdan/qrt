/*
 * i2chid.c - HID over I2C, written from Microsoft's "HID over I2C Protocol
 * Specification" v1.0 and the USB HID 1.11 report-descriptor format.
 *
 * Only what a touchscreen needs: fetch the HID descriptor and the report
 * descriptor, locate the finger collections (Digitizers page, usage 0x22)
 * and follow one primary contact.  Polled: a read with nothing pending
 * returns a zero length.
 */
#include "i2chid.h"

static u16 le16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }

/* ---- device side ------------------------------------------------------------ */
int i2chid_probe(i2chid_t *h, dwi2c_t *bus, u8 addr, u16 desc_reg) {
    u8 reg[2] = { (u8)desc_reg, (u8)(desc_reg >> 8) }, b[30];
    kfree(h->rdesc);
    memset(h, 0, sizeof *h);
    h->bus = bus; h->addr = addr; h->desc_reg = desc_reg;
    int err = dwi2c_xfer(bus, addr, reg, 2, b, 30);
    if (err) return err;
    hid_desc_t *d = &h->d;
    d->desc_len = le16(b); d->bcd = le16(b + 2); d->rdesc_len = le16(b + 4); d->rdesc_reg = le16(b + 6);
    d->in_reg = le16(b + 8); d->in_max = le16(b + 10); d->out_reg = le16(b + 12); d->out_max = le16(b + 14);
    d->cmd_reg = le16(b + 16); d->data_reg = le16(b + 18); d->vid = le16(b + 20); d->pid = le16(b + 22);
    d->ver = le16(b + 24);
    if (d->desc_len != 30 || d->bcd != 0x0100 || !d->rdesc_len || d->rdesc_len > 8192) return DW_ENODEV;
    h->rdesc = kalloc(d->rdesc_len);
    u8 rreg[2] = { (u8)d->rdesc_reg, (u8)(d->rdesc_reg >> 8) };
    if ((err = dwi2c_xfer(bus, addr, rreg, 2, h->rdesc, d->rdesc_len))) return err;
    hid_parse_report_desc(h, h->rdesc, d->rdesc_len);
    return DW_OK;
}

int i2chid_set_power(i2chid_t *h, int on) {
    /* command register <- SET_POWER (opcode 8), power state 0 = ON, 1 = SLEEP */
    u8 cmd[4] = { (u8)h->d.cmd_reg, (u8)(h->d.cmd_reg >> 8), (u8)(on ? 0 : 1), 0x08 };
    return dwi2c_xfer(h->bus, h->addr, cmd, 4, NULL, 0);
}

int i2chid_read(i2chid_t *h, u8 *buf, int cap) {
    u8 tmp[256];
    int want = MIN((int)h->d.in_max, (int)sizeof tmp);
    if (want < 2) want = 64;
    int err = dwi2c_xfer(h->bus, h->addr, NULL, 0, tmp, want);
    if (err) { h->errors++; return err; }
    int n = le16(tmp);
    if (n == 0 || n == 0xffff) { h->empty_reads++; return 0; }   /* nothing pending / reset ack */
    n = MIN(n, want) - 2;
    if (n <= 0) { h->empty_reads++; return 0; }
    n = MIN(n, cap);
    memcpy(buf, tmp + 2, (usize)n);
    return n;
}
