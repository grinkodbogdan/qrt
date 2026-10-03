/*
 * audio.c - the first step towards sound on the Venue 8 Pro 5855.
 *
 * Sound on Cherry Trail goes through Intel's SST audio DSP (PCI 8086:22a8)
 * to a Realtek codec on I2C.  This DSDT lists three codecs, all at address
 * 0x1C on \_SB.PCI0.I2C2 (PCI 00:18.2), and picks one with _STA:
 *   RTEK  10EC5672  unless WPID == 3
 *   RTK1  10EC5670  unless BDID == 1 or OSID == 1
 *   RTKC  10EC5640  only for OSID == 1 and BDID == 1
 * Rather than evaluate that, ask the chip: like Linux's rt5670.c and
 * rt5640.c, read its vendor id (register 0xFE, 0x10EC) and device id
 * (register 0xFF): 0x6271 is the RT5670/RT5672 family, 0x6231 the
 * RT5640/RT5639.  Registers are 8-bit addresses with 16-bit big-endian
 * values.  Done while the firmware still runs, as for the touchscreen, so
 * the firmware's PCI access finds the controller.
 *
 * Next steps (docs/roadmap.md): power up the codec's playback path, then
 * the DSP's firmware and an I2S stream.
 */
#include "audio.h"
#include "dwi2c.h"

static char status[96] = "not probed";

static int read_reg(dwi2c_t *bus, u8 reg, u16 *val) {
    u8 b[2];
    int e = dwi2c_xfer(bus, 0x1c, &reg, 1, b, 2);
    if (e) return e;
    *val = (u16)(b[0] << 8 | b[1]);
    return 0;
}

void audio_probe(void) {
    if (!k.is_venue) { strlcpy(status, "no known codec on this machine", sizeof status); return; }
    static dwi2c_t bus;
    int e = dwi2c_find(&bus, 0, 0x18, 2);
    if (e) { fmt(status, sizeof status, "I2C2 controller: %s", dwi2c_strerror(e)); klog("audio: %s", status); return; }
    u16 vendor = 0, dev = 0;
    e = read_reg(&bus, 0xfe, &vendor);
    if (!e) e = read_reg(&bus, 0xff, &dev);
    if (e) { fmt(status, sizeof status, "codec at I2C2 0x1c does not answer (%s)", dwi2c_strerror(e)); klog("audio: %s", status); return; }
    const char *name = dev == 0x6271 ? "Realtek RT5670/RT5672" : dev == 0x6231 ? "Realtek RT5640/RT5639" :
                       dev == 0x6281 ? "Realtek RT5651" : NULL;
    if (name) fmt(status, sizeof status, "%s codec found (I2C2 0x1c, id %04x, vendor %04x); the speaker needs the SST DSP driver, not written yet", name, dev, vendor);
    else fmt(status, sizeof status, "unknown codec at I2C2 0x1c (id %04x, vendor %04x)", dev, vendor);
    klog("audio: %s", status);
}

const char *audio_status(void) { return status; }
