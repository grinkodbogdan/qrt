/* Clock: analog face, digital readout and a stopwatch. */
#include "../ui/shell.h"

static struct { int running; u64 start, acc; int last_sec; tap_t tap; } st;

static void icon(canvas_t *c, float cx, float cy, float r, u32 fg) {
    gfx_ring(c, cx, cy, r, r * 0.22f, fg);
    gfx_line(c, cx, cy, cx, cy - r * 0.55f, r * 0.2f, fg);
    gfx_line(c, cx, cy, cx + r * 0.4f, cy + r * 0.1f, r * 0.2f, fg);
}

static u64 elapsed(void) { return st.acc + (st.running ? k_now_ms() - st.start : 0); }

static void layout(rect_t a, float *cx, float *cy, float *rad, rect_t *digital, rect_t *btn1, rect_t *btn2) {
    *rad = ui.landscape ? MIN(a.w * 0.22f, a.h * 0.40f) : MIN(a.w * 0.36f, a.h * 0.27f);
    if (ui.landscape) { *cx = a.x + a.w * 0.30f; *cy = a.y + a.h * 0.48f; }
    else { *cx = a.x + a.w / 2.0f; *cy = a.y + *rad + dp(24); }
    int px = ui.landscape ? a.x + a.w * 58 / 100 : a.x + dp(24);
    int pw = ui.landscape ? a.w * 38 / 100 : a.w - dp(48);
    int py = ui.landscape ? a.y + a.h / 2 - dp(120) : (int)(*cy + *rad + dp(40));
    *digital = (rect_t){ px, py, pw, dp(200) };
    int bw = (pw - dp(12)) / 2;
    *btn1 = (rect_t){ px, py + dp(200), bw, dp(52) };
    *btn2 = (rect_t){ px + bw + dp(12), py + dp(200), bw, dp(52) };
}

static void draw(canvas_t *c, rect_t a) {
    EFI_TIME t;
    k_walltime(&t);
    float cx, cy, R;
    rect_t dig, b1, b2;
    layout(a, &cx, &cy, &R, &dig, &b1, &b2);

    gfx_circle(c, cx, cy + dp(6), R + dp(4), RGBA(0, 0, 0, 60));
    gfx_circle(c, cx, cy, R, RGBA(0x1c, 0x18, 0x2c, 220));
    gfx_ring(c, cx, cy, R, dp(2), RGBA(255, 255, 255, 40));
    for (int i = 0; i < 60; i++) {
        float ang = i * (2 * PI_F / 60), s = fsin(ang), co = fcos(ang);
        float r0 = R * (i % 5 ? 0.92f : 0.84f), r1 = R * 0.96f;
        gfx_line(c, cx + s * r0, cy - co * r0, cx + s * r1, cy - co * r1,
                 i % 5 ? dp(1.2f) : dp(3), i % 5 ? RGBA(255, 255, 255, 90) : ui.text);
    }
    float sec = t.Second, min = t.Minute + sec / 60, hr = (t.Hour % 12) + min / 60;
    float ah = hr * (2 * PI_F / 12), am = min * (2 * PI_F / 60), as = sec * (2 * PI_F / 60);
    gfx_line(c, cx, cy, cx + fsin(ah) * R * 0.5f, cy - fcos(ah) * R * 0.5f, dp(7), ui.text);
    gfx_line(c, cx, cy, cx + fsin(am) * R * 0.75f, cy - fcos(am) * R * 0.75f, dp(4.5f), ui.text);
    gfx_line(c, cx - fsin(as) * R * 0.15f, cy + fcos(as) * R * 0.15f,
             cx + fsin(as) * R * 0.85f, cy - fcos(as) * R * 0.85f, dp(2), ui.accent);
    gfx_circle(c, cx, cy, dp(7), ui.accent);
    gfx_circle(c, cx, cy, dp(3), RGB(0x1c, 0x18, 0x2c));

    char buf[48];
    fmt(buf, sizeof buf, "%02d:%02d:%02d", t.Hour, t.Minute, t.Second);
    gfx_text(c, ui.display, dig.x, dig.y, buf, ui.text);
    fmt(buf, sizeof buf, "%04d-%02d-%02d  \xc2\xb7  UEFI real-time clock", t.Year, t.Month, t.Day);
    gfx_text_fit(c, ui.small, dig.x, dig.y + ui.display->line, dig.w, buf, ui.text2);

    u64 e = elapsed();
    ui_section(c, dig.x, dig.y + dp(104), "STOPWATCH");
    fmt(buf, sizeof buf, "%02llu:%02llu.%02llu", e / 60000, e / 1000 % 60, e / 10 % 100);
    gfx_text(c, ui.h1, dig.x, dig.y + dp(104) + ui.small->line, buf, st.running ? ui.accent : ui.text);
    ui_button(c, b1, st.running ? "Pause" : "Start", ui.accent, RGB(255, 255, 255));
    ui_button(c, b2, "Reset", RGBA(255, 255, 255, 30), ui.text);
}

static int event(const event_t *e, rect_t a) {
    float cx, cy, R;
    rect_t dig, b1, b2;
    layout(a, &cx, &cy, &R, &dig, &b1, &b2);
    if (e->type == EV_KEY && e->ch == ' ') goto toggle;
    if (!tap_track(&st.tap, e, dp(12))) return 0;
    if (in_rect(b1, e->x, e->y)) goto toggle;
    if (in_rect(b2, e->x, e->y)) { st.acc = 0; st.start = k_now_ms(); return 1; }
    return 0;
toggle:
    if (st.running) st.acc += k_now_ms() - st.start;
    else st.start = k_now_ms();
    st.running = !st.running;
    return 1;
}

static int tick(u64 now) {
    static u64 last;
    if (st.running && now - last >= 40) { last = now; return 1; }
    EFI_TIME t;
    if (now - last < 200) return 0;
    last = now;
    k_walltime(&t);
    if (t.Second != st.last_sec) { st.last_sec = t.Second; return 1; }
    return 0;
}

const app_t app_clock = { "Clock", "Time, stopwatch", RGB(0xff, 0x9f, 0x43), icon, NULL, draw, event, tick };
