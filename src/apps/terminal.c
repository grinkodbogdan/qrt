/* Terminal: run Linux programs (static x86-64 ELF) on the native kernel. */
#include "../ui/shell.h"
#include "../kernel/vfs.h"
#include "../net/net.h"
#include "../net/netstack.h"
#include "../net/wlan.h"
#if defined(__x86_64__)
#include "../arch/x64/proc.h"
#endif

#define SNAP_MAX (48 * 1024)

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
    int running;
    /* built-in ping */
    struct { int active, dns, sent, got, count; u32 ip; u16 id; u64 next_ms, sent_ms[64], end_ms; char host[64]; } ping;
} st;

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

#if defined(__x86_64__)
static u32 parse_u32(const char *s) { u32 v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (u32)(*s++ - '0'); return v; }

/* ---- built-in network commands (they run in the shell, no Linux process) ---- */
static void ping_reply(u32 src, u16 id, u16 seq, const u8 *data, usize len) {
    (void)data;
    if (!st.ping.active || id != st.ping.id || src != st.ping.ip || seq >= 64) return;
    u64 rtt = k_now_us() / 1000 - st.ping.sent_ms[seq];
    char a[16], m[96];
    ip_to_str(src, a, sizeof a);
    fmt(m, sizeof m, "%u bytes from %s: seq=%u time=%llu ms\n", (u32)len + 8, a, seq, rtt);
    out(m);
    st.ping.got++;
}

static void ping_start(const char *host, int count) {
    memset(&st.ping, 0, sizeof st.ping);
    strlcpy(st.ping.host, host, sizeof st.ping.host);
    st.ping.count = CLAMP(count, 1, 64);
    st.ping.id = (u16)(0x7100 + (k_now_ms() & 0xff));
    net_lock();
    if (!net_primary()) { net_unlock(); out("ping: not connected (open Wi-Fi)\n"); return; }
    net_on_echo_reply(ping_reply);
    st.ping.dns = dns_start(host);
    net_unlock();
    if (st.ping.dns < 0) { out("ping: cannot resolve\n"); return; }
    st.ping.active = 1;
}

static int ping_tick(u64 now) {
    if (!st.ping.active) return 0;
    net_lock();
    if (st.ping.dns >= 0) {
        u32 ip;
        int r = dns_result(st.ping.dns, &ip);
        if (r) {
            st.ping.dns = -1;
            if (r < 0 || !ip) { net_unlock(); out("ping: unknown host\n"); st.ping.active = 0; return 1; }
            st.ping.ip = ip;
            char a[16], m[128];
            ip_to_str(ip, a, sizeof a);
            fmt(m, sizeof m, "PING %s (%s): 56 data bytes\n", st.ping.host, a);
            net_unlock();
            out(m);
            net_lock();
            st.ping.next_ms = now;
        }
    }
    if (st.ping.ip && st.ping.sent < st.ping.count && now >= st.ping.next_ms) {
        u8 payload[56];
        for (int i = 0; i < 56; i++) payload[i] = (u8)i;
        st.ping.sent_ms[st.ping.sent] = now;
        net_ping(st.ping.ip, st.ping.id, (u16)st.ping.sent, payload, sizeof payload);
        st.ping.sent++;
        st.ping.next_ms = now + 1000;
        if (st.ping.sent == st.ping.count) st.ping.end_ms = now + 2000;
    }
    net_unlock();
    if (st.ping.end_ms && now >= st.ping.end_ms) {
        char m[128];
        fmt(m, sizeof m, "--- %s: %d sent, %d received, %d%% loss\n", st.ping.host, st.ping.sent, st.ping.got,
            st.ping.sent ? (st.ping.sent - st.ping.got) * 100 / st.ping.sent : 0);
        out(m);
        st.ping.active = 0;
        return 1;
    }
    return 0;
}

static void show_ifconfig(void) {
    net_lock();
    netif_t *n = net_primary();
    char m[256];
    if (!n) { const char *ns = net_status(); fmt(m, sizeof m, "%s\n", ns ? ns : "no network: turn on Wi-Fi (Wi-Fi app)"); }
    else {
        char a[16], k[16], g[16], d[16];
        ip_to_str(n->ip, a, sizeof a); ip_to_str(n->mask, k, sizeof k); ip_to_str(n->gw, g, sizeof g); ip_to_str(n->dns, d, sizeof d);
        fmt(m, sizeof m, "%s%s%s  HWaddr %02x:%02x:%02x:%02x:%02x:%02x\n  inet %s  mask %s  gateway %s  dns %s\n",
            n->name, n->detail[0] ? " " : "", n->detail, n->mac[0], n->mac[1], n->mac[2], n->mac[3], n->mac[4], n->mac[5], a, k, g, d);
    }
    net_unlock();
    out(m);
}

#endif

static void run(const char *cmd) {
    char buf[256];
    strlcpy(buf, cmd, sizeof buf);                 /* the event handler already echoed it after the prompt */
    st.scroll = 0;
#if defined(__x86_64__)
    if ((st.proc && !st.proc->exited) || st.ping.active) { out("[a program is still running - stop it first]\n"); return; }
    const char *argv[24];
    int argc = 0;
    /* shell syntax (pipes, redirection, ; && $ * ...): let busybox sh run the line */
    for (const char *c = buf; *c; c++)
        if (strchr("|;&<>$`*?'\\(", *c)) {
            const char *sh[] = { "sh", "-c", buf };
            char err[96];
            st.proc = proc_spawn("/bin/busybox", 3, sh, &st.term, err, sizeof err);
            if (!st.proc) { char m[128]; fmt(m, sizeof m, "%s\n", err); out(m); }
            return;
        }
    for (char *p = buf; *p && argc < 23;) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (*p == '"') { argv[argc++] = ++p; while (*p && *p != '"') p++; }
        else { argv[argc++] = p; while (*p && *p != ' ') p++; }
        if (*p) *p++ = 0;
    }
    if (!argc) return;
    if (!strcmp(argv[0], "clear")) { st.term.len = 0; st.term.serial++; return; }
    if (!strcmp(argv[0], "ping") && argc >= 2) {
        int count = 4;
        const char *host = argv[argc - 1];
        for (int i = 1; i < argc - 1; i++) if (!strcmp(argv[i], "-c") && i + 1 < argc - 1) count = (int)parse_u32(argv[i + 1]);
        ping_start(host, count);
        return;
    }
    if (!strcmp(argv[0], "ifconfig") || (!strcmp(argv[0], "ip") && argc == 2 && !strcmp(argv[1], "a"))) { show_ifconfig(); return; }
    if (!strcmp(argv[0], "wifi")) { char m[128]; fmt(m, sizeof m, "Wi-Fi: %s\n", wlan_available() ? wlan_state_text() : "no supported card"); out(m); return; }
    if (!strcmp(argv[0], "help")) {
        out("Runs static Linux x86-64 programs through QRT's Linux system-call layer.\n"
            "Programs live in /bin; any other name is tried as a busybox applet.\n"
            "Pipes, redirection and ; run through busybox sh. 'clear' empties the screen.\n"
            "Built in: ping [-c N] HOST, ifconfig, wifi. Network programs: wget, nslookup.\n");
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
    out("Programs need the 64-bit kernel.\n");
#endif
}

/* ---- layout: one console filling the window, the prompt inline ---- */
static rect_t screen_rect(rect_t a) { return a; }
static rect_t stop_rect(rect_t a) { int w = dp(84), h = dp(34); return (rect_t){ a.x + a.w - w - dp(12), a.y + dp(10), w, h }; }
static const font_t *term_font(void) { return font_pick(F_MONO, dp(14)); }

static void draw(canvas_t *c, rect_t a) {
    rect_t s = screen_rect(a);
    gfx_fill(c, s, RGB(0x1d, 0x1d, 0x20));
    rect_t txt = { s.x + dp(12), s.y + dp(8), s.w - dp(24), s.h - dp(16) };
    const font_t *f = term_font();
    int cw = MAX(1, text_width(f, "M")), cols = MAX(10, txt.w / cw), rows = MAX(2, txt.h / f->line);
    int prompt = !st.running;
    int out_rows = rows - prompt;

    /* walk backwards from the end, wrapping at 'cols', to find the first visible line */
    const char *b = st.snap;
    int n = (int)st.snap_len, skip = st.scroll, need = out_rows + skip;
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
    rect_t old = c->clip;
    gfx_clip(c, txt);
    int y = txt.y, drawn = 0;
    char row[256];
    for (int i = start; i < n && drawn < out_rows;) {
        int e = i;
        while (e < n && b[e] != '\n' && e - i < cols) e++;
        int len = MIN(e - i, (int)sizeof row - 1);
        memcpy(row, b + i, (usize)len);
        row[len] = 0;
        gfx_text(c, f, txt.x, y, row, RGB(0xde, 0xdd, 0xda));
        y += f->line;
        drawn++;
        i = (e < n && b[e] == '\n') ? e + 1 : e;
    }
    if (prompt && !st.scroll) {
        /* user@qrt:~$ in bash's colours */
        int x = gfx_text(c, font_pick(F_MONO, dp(14)), txt.x, y, "user@qrt", RGB(0x8a, 0xe2, 0x34));
        x = gfx_text(c, f, x, y, ":", RGB(0xde, 0xdd, 0xda));
        x = gfx_text(c, f, x, y, "~", RGB(0x72, 0x9f, 0xcf));
        x = gfx_text(c, f, x, y, "$ ", RGB(0xde, 0xdd, 0xda));
        /* the end of a long line, like a shell that scrolls its input */
        const char *ln = st.line;
        int room = MAX(1, (txt.x + txt.w - x) / cw - 1);
        int ll = (int)strlen(ln);
        if (ll > room) ln += ll - room;
        x = gfx_text(c, f, x, y, ln, RGB(0xff, 0xff, 0xff));
        gfx_fill(c, (rect_t){ x, y + dp(1), cw, f->line - dp(2) }, RGBA(0xde, 0xdd, 0xda, 200));   /* block cursor */
    }
    c->clip = old;
    if (st.running) ui_button(c, stop_rect(a), "Stop", RGB(0xc0, 0x1c, 0x28), RGB(255, 255, 255));
}

static void stop(void) {
#if defined(__x86_64__)
    if (st.ping.active) { st.ping.active = 0; out("^C\n"); }
    else if (st.proc && !st.proc->exited) { proc_kill(st.proc); out("^C\n"); }
#endif
}

static int event(const event_t *e, rect_t a) {
    if (e->type == EV_KEY) {
        usize n = strlen(st.line);
        if (e->ch == '\r') {
            if (st.running) return 0;
            /* echo the command after the prompt, as a shell would */
            char echo[160];
            fmt(echo, sizeof echo, "user@qrt:~$ %s\n", st.line);
            out(echo);
            if (n) run(st.line);
            st.line[0] = 0;
            st.scroll = 0;
        }
        else if (e->ch == 8) { if (n) st.line[n - 1] = 0; }
        else if (e->ch == 3) stop();                                   /* Ctrl-C from a keyboard */
        else if (e->scan == SCAN_UP) st.scroll += 3;
        else if (e->scan == SCAN_DOWN) st.scroll = MAX(0, st.scroll - 3);
        else if (e->ch >= 32 && e->ch < 127 && n + 1 < sizeof st.line) { st.line[n] = (char)e->ch; st.line[n + 1] = 0; st.scroll = 0; }
        return 1;
    }
    rect_t s = screen_rect(a);
    if (e->type == EV_DOWN && in_rect(s, e->x, e->y)) { st.dragging = 1; st.drag_y = e->y; }
    if (e->type == EV_MOVE && st.dragging) {
        int d = (e->y - st.drag_y) / MAX(1, term_font()->line);
        if (d) { st.scroll = MAX(0, st.scroll + d); st.drag_y = e->y; return 1; }
    }
    if (e->type == EV_SCROLL) { st.scroll = MAX(0, st.scroll - e->dy * 3); return 1; }
    int tapped = tap_track(&st.tap, e, dp(12));
    if (e->type == EV_UP) {
        st.dragging = 0;
        if (tapped && st.running && in_rect(stop_rect(a), e->x, e->y)) stop();
        else if (tapped) shell_keyboard(1);                            /* tap the console to type */
        return 1;
    }
    return 0;
}

static void on_open(void) {
    static int greeted;
    if (greeted++) return;
#if defined(__x86_64__)
    const char *why;
    if (!proc_user_supported(&why)) { char m[160]; fmt(m, sizeof m, "Programs cannot run here: %s\n", why); out(m); }
#else
    out("Programs need the 64-bit kernel.\n");
#endif
}

static int tick(u64 now) {
#if defined(__x86_64__)
    int redraw = ping_tick(now);
#else
    int redraw = 0;
    (void)now;
#endif
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
    int running = (st.proc && !st.proc->exited) || st.ping.active;
    if (running != st.running) { st.running = running; redraw = 1; }
#endif
    return redraw;
}

/* closing the window ends the program and starts a fresh session */
static void on_close(void) {
    stop();
    st.line[0] = 0;
    st.scroll = 0;
#if defined(__x86_64__)
    st.term.len = 0;
    st.term.serial++;
#else
    st.snap_len = 0;
    st.snap[0] = 0;
#endif
}

const app_t app_terminal = { "Terminal", "Command line", RGB(0x3d, 0x3d, 0x45), icon, on_open, draw, event, tick, on_close };
