/*
 * msm.c - what Tessera drives itself on a Qualcomm MSM8953 phone (Xiaomi Mi A1) before
 * Linux's drivers take the rest: the volume-up key, a GPIO the boot loader left as an
 * input (TLMM: one 4 KB block per pin, IN_OUT at +4, bit 0 the level; the key pulls
 * it low).  The device tree's gpio-keys node says which pin.
 */
#include "arm.h"

static u64 tlmm;
static int vol_pin = -1, vol_was;

static int key_one(int c, void *arg) {
    (void)arg;
    int len;
    const u8 *code = fdt_prop(c, "linux,code", &len);
    const u8 *g = fdt_prop(c, "gpios", &len);
    if (code && g && len >= 12 && fdt_u32(code) == 115) vol_pin = (int)fdt_u32(g + 4);   /* KEY_VOLUMEUP */
    return 0;
}

int msm_init(void) {
    int n = fdt_find_compatible(-1, "qcom,msm8953-pinctrl");
    u64 a, s;
    if (n < 0 || !fdt_reg(n, 0, &a, &s)) return 0;
    tlmm = a;
    int keys = fdt_find_compatible(-1, "gpio-keys");
    if (keys >= 0) fdt_children(keys, key_one, NULL);
    if (vol_pin >= 0) vol_was = !(R32(tlmm + 0x1000ull * (u32)vol_pin + 4) & 1);
    klog("msm8953: TLMM at %llx, volume up on GPIO %d", tlmm, vol_pin);
    return 1;
}

int msm_poll(event_t *out, int max) {
    if (!tlmm || vol_pin < 0 || max < 1) return 0;
    int down = !(R32(tlmm + 0x1000ull * (u32)vol_pin + 4) & 1);
    int n = 0;
    if (down && !vol_was) { event_t e = { .type = EV_KEY, .scan = SCAN_VOLUP }; out[n++] = e; }
    vol_was = down;
    return n;
}
