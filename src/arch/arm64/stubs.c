/*
 * stubs.c - drivers the shared shell knows from the PCs and the Venue that this ARM
 * build does not have (yet): each answers "nothing here", so Settings and System show
 * the hardware as absent instead of failing.  On a phone these come from Linux's
 * drivers through the driver host.
 */
#include "arm.h"
#include "../../kernel/dev.h"
#include "../../kernel/smp.h"
#include "../../drivers/audio.h"
#include "../../drivers/backlight.h"
#include "../../drivers/battery.h"
#include "../../drivers/buttons.h"
#include "../../drivers/touch.h"
#include "../../drivers/ish.h"
#include "../../drivers/speaker.h"
#include "../../drivers/hda.h"
#include "../../drivers/dwi2c.h"
#include "../../drivers/e1000.h"
#include "../../drivers/wifi.h"
#include "../../drivers/bt/bt.h"
#include "../../drivers/i915/gpu.h"
#include "../../drivers/i915/display.h"
#include "../../drivers/usb/xhci.h"

device_t devs[DEV_MAX];
int n_devs;
void dev_refresh(void) {}
ntouch_t nt = { .primary = -1 };
void ntouch_probe(void) { nt.probed = 1; strlcpy(nt.status, "no I2C touch screen driver on ARM yet", sizeof nt.status); }
int  ntouch_go_native(void) { return 0; }
void ntouch_revert(void) {}
int  ntouch_save(void) { return 0; }
const char *dwi2c_strerror(int err) { (void)err; return "no I2C controller"; }

/* one core for now: jobs run in the caller */
int  smp_enabled(void) { return 0; }
int  smp_workers(void) { return 0; }
void smp_set_enabled(int on) { (void)on; }
void smp_run(smp_job_t job, void *arg, int count) { for (int i = 0; i < count; i++) job(arg, i, count); }

const char *audio_status(void) { return "no sound driver on ARM yet"; }
const char *speaker_status(void) { return "none"; }
const char *hda_status(void) { return "none"; }
/* the backlight through Linux's driver (linux.c), once it is up */
int linux_backlight_set(int pct);
int linux_backlight_present(void);
static int bl_level = 70, bl_on = 1, bl_want = -1;
int  backlight_available(void) { return linux_backlight_present(); }
int  backlight_dim_alpha(void) { return 0; }
int  backlight_level(void) { return bl_level; }
const char *backlight_method(void) { return linux_backlight_present() ? "Linux's backlight driver" : "the boot loader's setting"; }
void backlight_power(int on) { bl_on = on; bl_want = on ? bl_level : 0; }
void backlight_set_level(int pct) { bl_level = CLAMP(pct, 5, 100); if (bl_on) bl_want = bl_level; }
void backlight_tick(void) { if (bl_want >= 0) { linux_backlight_set(bl_want); bl_want = -1; } }   /* never waits */
static battery_t no_battery = { .minutes = -1 };
const battery_t *linux_battery(void);                 /* linux.c: Linux's fuel gauge, NULL if none */
const battery_t *battery_get(void) { const battery_t *b = linux_battery(); return b ? b : &no_battery; }
void battery_poll(void) {}
int  buttons_active(void) { return 0; }
int  buttons_debug(char lines[][112], int max) { (void)lines; (void)max; return 0; }
void buttons_scan_start(void) {}
int  ish_orientation(void) { return -1; }
const char *ish_status(void) { return "none"; }
int  hwreport_write(const char *name, const void *data, usize len) { (void)name; (void)data; (void)len; return 0; }
u64  install_target_bytes(void) { return 0; }
void e1000_poll(void) {}
int  xhci_devices(char lines[][96], int max) { (void)lines; (void)max; return 0; }

/* Wi-Fi: none yet (the Mi A1's WCN3680 comes with Linux's wcn36xx) */
int  wifi_present(void) { return 0; }
int  wifi_start(void) { return -1; }
void wifi_stop(void) {}
void wifi_poll(void) {}
int  wifi_scan(void) { return -1; }
int  wifi_scanning(void) { return 0; }
void wifi_scan_forget(void) {}
void wifi_check_firmware(void) {}
void wifi_disconnect(void) {}
static const u8 zero_mac[6];
const u8 *wifi_macaddr(void) { return zero_mac; }
u32  wifi_missed_beacons(void) { return 0; }
int  wifi_auth_prepare(const iwm_bss_t *b) { (void)b; return -1; }
int  wifi_assoc_done(const iwm_bss_t *b) { (void)b; return -1; }
int  wifi_set_pairwise_key(const u8 key[16]) { (void)key; return -1; }
void wifi_set_rx(iwm_rx_fn fn) { (void)fn; }
int  wifi_tx(const u8 *frame, usize len, int mgmt) { (void)frame; (void)len; (void)mgmt; return -1; }

int  bt_state(void) { return 0; }
const char *bt_status(void) { return "no Bluetooth driver on ARM yet"; }
void bt_scan(void) {}
int  bt_devices(bt_device_t *out, int max) { (void)out; (void)max; return 0; }
const char *bt_kind(u32 cod, int le) { (void)cod; (void)le; return "device"; }
void bt_audio_connect(const u8 addr[6]) { (void)addr; }
int  bt_audio_connected(const u8 addr[6]) { (void)addr; return 0; }
void bt_audio_disconnect(void) {}
const char *bt_audio_status(void) { return ""; }

int  gpu_active(void) { return 0; }
void gpu_autostart(void) {}
int  gpu_enabled(void) { return 0; }
int  gpu_present(const u32 *src, int sw, int sh, int stride, int rot, int x, int y, int w, int h) { (void)src; (void)sw; (void)sh; (void)stride; (void)rot; (void)x; (void)y; (void)w; (void)h; return 0; }
void gpu_set_enabled(int on) { (void)on; }
void gpu_stats(u32 *frames, u32 *avg_us, int *coherent) { *frames = 0; *avg_us = 0; *coherent = 0; }
const char *gpu_status(void) { return "the boot loader's framebuffer"; }
int  gpu_supported(void) { return 0; }
int  display_connected(void) { return 0; }
int  display_enabled(void) { return 0; }
void display_mirror(const u32 *px, int w, int h, int stride, int x, int y, int rw, int rh) { (void)px; (void)w; (void)h; (void)stride; (void)x; (void)y; (void)rw; (void)rh; }
const char *display_monitor(void) { return ""; }
void display_set_enabled(int on) { (void)on; }
int  display_size(int *w, int *h) { (void)w; (void)h; return 0; }
void display_start(void) {}
const char *display_status(void) { return "none"; }
