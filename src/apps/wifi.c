/* Wi-Fi: networks, password entry and connection status. */
#include "../ui/shell.h"

static void icon(canvas_t *c, float cx, float cy, float r, u32 fg) {
    for (int i = 1; i <= 3; i++) {
        float rr = r * (0.28f + 0.3f * i);
        gfx_line(c, cx - rr * 0.7f, cy + r * 0.45f - rr * 0.7f, cx, cy + r * 0.45f - rr, r * 0.14f, fg);
        gfx_line(c, cx, cy + r * 0.45f - rr, cx + rr * 0.7f, cy + r * 0.45f - rr * 0.7f, r * 0.14f, fg);
    }
    gfx_circle(c, cx, cy + r * 0.45f, r * 0.14f, fg);
}

static void draw(canvas_t *c, rect_t a) {
    gfx_text(c, ui.body, a.x + dp(24), a.y + dp(20), "No Wi-Fi driver yet.", ui.text2);
}

static int event(const event_t *e, rect_t a) { (void)e; (void)a; return 0; }

const char *shell_net_status(void) { return NULL; }

const app_t app_wifi = { "Wi-Fi", "Networks", RGB(0x2e, 0x8b, 0xff), icon, NULL, draw, event, NULL };
