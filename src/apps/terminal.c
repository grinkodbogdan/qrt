/* Terminal: run Linux programs (static x86-64 ELF) on the native kernel. */
#include "../ui/shell.h"
#include "../kernel/vfs.h"
#if defined(__x86_64__)
#include "../arch/x64/proc.h"
#endif

#define SNAP_MAX (48 * 1024)

static const char *presets[] = {
    "hello", "uname -a", "ls -l /", "cat /etc/os-release", "cat /proc/cpuinfo", "free",
    "date", "ls /bin", "sha256sum /bin/hello", "wc -l /qrt/welcome.txt", "echo Hello from Linux", "busybox",
};
#define N_PRESETS ((int)ARRAY_LEN(presets))

static struct {
#if defined(__x86_64__)
    term_t term;
    proc_t *proc;
#endif
    char line[128];
    char snap[SNAP_MAX + 1];      /* stable copy for drawing (draw may run on any core) */
    usize snap_len;
    u32 seen_serial;
    int scroll;                    /* wrapped lines scrolled up from the bottom */
    int drag_y, dragging;
    tap_t tap;
    int pressed_chip;
    char status[96];
} st = { .pressed_chip = -1 };

static void icon(canvas_t *c, float cx, float cy, float r, u32 fg) {
    gfx_line(c, cx - r * 0.7f, cy - r * 0.45f, cx - r * 0.15f, cy, r * 0.22f, fg);
    gfx_line(c, cx - r * 0.15f, cy, cx - r * 0.7f, cy + r * 0.45f, r * 0.22f, fg);
    gfx_line(c, cx + r * 0.05f, cy + r * 0.5f, cx + r * 0.75f, cy + r * 0.5f, r * 0.22f, fg);
}

static void out(const char *s) {
#if defined(__x86_64__)
    term_append(&st.term, s, strlen(s));
#else
    usize n = strlen(s);
    if (st.snap_len + n > SNAP_MAX) st.snap_len = 0;
    memcpy(st.snap + st.snap_len, s, n);
    st.snap_len += n;
    st.snap[st.snap_len] = 0;
#endif
}

static void run(const char *cmd) {
    char buf[128], shown[160];
    strlcpy(buf, cmd, sizeof buf);
    fmt(shown, sizeof shown, "$ %s\n", cmd);
    out(shown);
    st.scroll = 0;
#if defined(__x86_64__)
    if (st.proc && !st.proc->exited) { out("[a program is still running - stop it first]\n"); return; }
    const char *argv[24];
    int argc = 0;
    for (char *p = buf; *p && argc < 23;) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (*p == '"') { argv[argc++] = ++p; while (*p && *p != '"') p++; }
        else { argv[argc++] = p; while (*p && *p != ' ') p++; }
        if (*p) *p++ = 0;
    }
    if (!argc) return;
    if (!strcmp(argv[0], "clear")) { st.term.len = 0; st.term.serial++; return; }
    if (!strcmp(argv[0], "help")) {
        out("Runs static Linux x86-64 programs through QRT's Linux system-call layer.\n"
            "Programs live in /bin; any other name is tried as a busybox applet.\n"
            "No pipes, redirection or fork yet. 'clear' empties the screen.\n");
        return;
    }
    char path[96];
    if (strchr(argv[0], '/')) strlcpy(path, argv[0], sizeof path);
    else {
        fmt(path, sizeof path, "/bin/%s", argv[0]);
        if (!vfs_lookup(path)) strlcpy(path, "/bin/busybox", sizeof path);
    }
    char err[96];
    st.proc = proc_spawn(path, argc, argv, &st.term, err, sizeof err);
    if (!st.proc) { char m[128]; fmt(m, sizeof m, "%s\n", err); out(m); }
#else
    out("Linux programs need QRT's native 64-bit kernel (this is the firmware-hosted 32-bit build).\n");
#endif
}

/* ---- layout ---- */
static rect_t chip_rect(rect_t a, int i, int *rows_out) {
    int x = a.x + dp(16), y = a.y, row = 0, h = dp(36), gap = dp(8);
    for (int j = 0; j <= i; j++) {
        int w = text_width(ui.small, presets[j]) + dp(24);
        if (x + w > a.x + a.w - dp(16)) { x = a.x + dp(16); y += h + gap; row++; }
        if (j == i) { if (rows_out) *rows_out = row + 1; return (rect_t){ x, y, w, h }; }
        x += w + gap;
    }
    return (rect_t){ 0 };
}
static int chips_height(rect_t a) { int rows = 1; chip_rect(a, N_PRESETS - 1, &rows); return rows * dp(44); }
static rect_t input_rect(rect_t a) { return (rect_t){ a.x + dp(16), a.y + chips_height(a), a.w - dp(32) - dp(108), dp(44) }; }
static rect_t stop_rect(rect_t a) { rect_t i = input_rect(a); return (rect_t){ i.x + i.w + dp(8), i.y, dp(100), dp(44) }; }
static rect_t screen_rect(rect_t a) { rect_t i = input_rect(a); int y = i.y + i.h + dp(10); return (rect_t){ a.x + dp(16), y, a.w - dp(32), a.y + a.h - y - dp(28) }; }

static void draw(canvas_t *c, rect_t a) {
    for (int i = 0; i < N_PRESETS; i++) ui_chip(c, chip_rect(a, i, NULL), presets[i], st.pressed_chip == i);
    rect_t in = input_rect(a);
    gfx_rrect(c, in, dp(12), RGBA(0, 0, 0, 90));
    const font_t *m = font_pick(F_MONO, dp(15));
    int tx = gfx_text(c, m, in.x + dp(12), in.y + (in.h - m->line) / 2, "$ ", ui.accent);
    tx = gfx_text(c, m, tx, in.y + (in.h - m->line) / 2, st.line, ui.text);
    gfx_fill(c, (rect_t){ tx + 1, in.y + dp(10), dp(2), in.h - dp(20) }, ui.accent);
#if defined(__x86_64__)
    int running = st.proc && !st.proc->exited;
#else
    int running = 0;
#endif
    ui_button(c, stop_rect(a), running ? "Stop" : "Run", running ? RGB(0xe5, 0x48, 0x4d) : ui.accent, RGB(255, 255, 255));

    rect_t s = screen_rect(a);
    gfx_rrect(c, s, dp(14), RGB(0x0c, 0x0a, 0x14));
    gfx_rrect_outline(c, s, dp(14), 1, ui.stroke);
    rect_t txt = { s.x + dp(12), s.y + dp(10), s.w - dp(24), s.h - dp(20) - ui.small->line };
    const font_t *f = font_pick(F_MONO, dp(13));
    int cw = MAX(1, text_width(f, "M")), cols = MAX(10, txt.w / cw), rows = MAX(1, txt.h / f->line);

    /* walk backwards from the end, wrapping at 'cols', to find the first visible line */
    const char *b = st.snap;
    int n = (int)st.snap_len, skip = st.scroll, need = rows + skip;
    int start = n, lines = 0;
    while (start > 0 && lines < need) {
        int e = start;
        if (e > 0 && b[e - 1] == '\n') e--;
        int ls = e;
        while (ls > 0 && b[ls - 1] != '\n') ls--;
        int len = e - ls, wrapped = len ? (len + cols - 1) / cols : 1;
        if (lines + wrapped > need) { start = ls + (wrapped - (need - lines)) * cols; lines = need; break; }
        lines += wrapped;
        start = ls;
    }
    gfx_clip(c, txt);
    int y = txt.y, drawn = 0;
    char row[256];
    for (int i = start; i < n && drawn < rows;) {
        int e = i;
        while (e < n && b[e] != '\n' && e - i < cols) e++;
        int len = MIN(e - i, (int)sizeof row - 1);
        memcpy(row, b + i, (usize)len);
        row[len] = 0;
        gfx_text(c, f, txt.x, y, row, RGB(0xd8, 0xf5, 0xd0));
        y += f->line;
        drawn++;
        i = (e < n && b[e] == '\n') ? e + 1 : e;
    }
    gfx_unclip(c);
    gfx_clip(c, a);
    gfx_text_fit(c, ui.small, s.x + dp(12), s.y + s.h - dp(8) - ui.small->line, s.w - dp(24), st.status, ui.text3);
}

static int event(const event_t *e, rect_t a) {
    if (e->type == EV_KEY) {
        usize n = strlen(st.line);
        if (e->ch == '\r') { if (n) { run(st.line); st.line[0] = 0; } }
        else if (e->ch == 8) { if (n) st.line[n - 1] = 0; }
        else if (e->scan == SCAN_UP) st.scroll += 3;
        else if (e->scan == SCAN_DOWN) st.scroll = MAX(0, st.scroll - 3);
        else if (e->ch >= 32 && e->ch < 127 && n + 1 < sizeof st.line) { st.line[n] = (char)e->ch; st.line[n + 1] = 0; }
        return 1;
    }
    rect_t s = screen_rect(a);
    if (e->type == EV_DOWN && in_rect(s, e->x, e->y)) { st.dragging = 1; st.drag_y = e->y; }
    if (e->type == EV_MOVE && st.dragging) {
        int d = (e->y - st.drag_y) / MAX(1, font_pick(F_MONO, dp(13))->line);
        if (d) { st.scroll = MAX(0, st.scroll + d); st.drag_y = e->y; return 1; }
    }
    if (e->type == EV_UP) st.dragging = 0;
    if (e->type == EV_SCROLL) { st.scroll = MAX(0, st.scroll - e->dy * 3); return 1; }
    int hit = -1;
    for (int i = 0; i < N_PRESETS; i++) if (in_rect(chip_rect(a, i, NULL), e->x, e->y)) hit = i;
    if (e->type == EV_DOWN) { st.pressed_chip = hit; }
    int tapped = tap_track(&st.tap, e, dp(12));
    if (e->type == EV_UP) {
        if (tapped && hit >= 0 && hit == st.pressed_chip) run(presets[hit]);
        else if (tapped && in_rect(stop_rect(a), e->x, e->y)) {
#if defined(__x86_64__)
            if (st.proc && !st.proc->exited) proc_kill(st.proc);
            else
#endif
            if (st.line[0]) { run(st.line); st.line[0] = 0; }
        }
        if (tapped && in_rect(input_rect(a), e->x, e->y)) shell_keyboard(1);
        st.pressed_chip = -1;
        return 1;
    }
    return e->type == EV_DOWN;
}

static void on_open(void) {
    static int greeted;
    if (greeted++) return;
#if defined(__x86_64__)
    const char *why;
    if (proc_user_supported(&why))
        out("QRT Terminal - Linux x86-64 programs on the Tessera kernel.\nTap a command or type one. 'help' explains.\n\n");
    else { char m[160]; fmt(m, sizeof m, "Linux programs unavailable: %s\n", why); out(m); }
#else
    out("Linux programs need QRT's native 64-bit kernel (this is the firmware-hosted 32-bit build).\n");
#endif
}

static int tick(u64 now) {
    (void)now;
    int redraw = 0;
#if defined(__x86_64__)
    if (st.term.serial != st.seen_serial) {
        u64 fl;
        __asm__ volatile("pushfq; pop %0; cli" : "=r"(fl) :: "memory");
        usize from = st.term.len > SNAP_MAX ? st.term.len - SNAP_MAX : 0;
        st.snap_len = st.term.len - from;
        if (st.term.buf) memcpy(st.snap, st.term.buf + from, st.snap_len);
        st.snap[st.snap_len] = 0;
        st.seen_serial = st.term.serial;
        if (fl & 0x200) __asm__ volatile("sti");
        redraw = 1;
    }
    char s[96];
    if (st.proc && !st.proc->exited) fmt(s, sizeof s, "running %s (pid %d) \xc2\xb7 %llu system calls", st.proc->name, st.proc->pid, st.proc->syscalls);
    else if (st.proc) fmt(s, sizeof s, "%s exited with status %d \xc2\xb7 %llu system calls", st.proc->name, st.proc->exit_code, st.proc->syscalls);
    else strlcpy(s, "idle", sizeof s);
    if (strcmp(s, st.status)) { strlcpy(st.status, s, sizeof st.status); redraw = 1; }
#endif
    return redraw;
}

const app_t app_terminal = { "Terminal", "Linux programs", RGB(0x55, 0x5b, 0x6e), icon, on_open, draw, event, tick };
