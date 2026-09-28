/* Life: Conway's cellular automaton. Tap cells, watch them go. */
#include "../ui/shell.h"

#define GW 96
#define GH 96

static struct {
    u8 cell[GH][GW], next[GH][GW];
    int cols, rows, running, gen;
    u64 last;
    tap_t tap;
    int painting, paint_val;
} st = { .running = 1 };

static void icon(canvas_t *c, float cx, float cy, float r, u32 fg) {
    static const int glider[3][3] = { {0, 1, 0}, {0, 0, 1}, {1, 1, 1} };
    float s = r * 0.62f;
    for (int y = 0; y < 3; y++)
        for (int x = 0; x < 3; x++)
            gfx_circle(c, cx + (x - 1) * s, cy + (y - 1) * s, r * (glider[y][x] ? 0.26f : 0.1f), fg);
}

static void seed(void) {
    for (int y = 0; y < GH; y++) for (int x = 0; x < GW; x++) st.cell[y][x] = (rand32() % 100) < 28;
    st.gen = 0;
}

static void step(void) {
    for (int y = 0; y < st.rows; y++)
        for (int x = 0; x < st.cols; x++) {
            int n = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    if (!dx && !dy) continue;
                    n += st.cell[(y + dy + st.rows) % st.rows][(x + dx + st.cols) % st.cols];
                }
            st.next[y][x] = n == 3 || (n == 2 && st.cell[y][x]);
        }
    memcpy(st.cell, st.next, sizeof st.cell);
    st.gen++;
}

static rect_t board(rect_t a) { return (rect_t){ a.x + dp(16), a.y + dp(64), a.w - dp(32), a.h - dp(72) }; }
static rect_t btn(rect_t a, int i) { return (rect_t){ a.x + dp(16) + i * dp(118), a.y + dp(4), dp(108), dp(46) }; }
static int cell_px(void) { return dp(14); }

static void dims(rect_t a, int *cols, int *rows) {
    rect_t b = board(a);
    *cols = MIN(GW, (b.w - dp(16)) / cell_px());
    *rows = MIN(GH, (b.h - dp(16)) / cell_px());
}

/* boot core only (open/event/tick): may reseed */
static void geometry(rect_t a) {
    int cols, rows;
    dims(a, &cols, &rows);
    if (st.cols && (cols != st.cols || rows != st.rows)) seed();   /* rotated: new board */
    st.cols = cols;
    st.rows = rows;
}

static void on_open(void) {
    if (!st.gen) seed();
    geometry(shell_app_area());
}

static void draw(canvas_t *c, rect_t a) {
    int cols, rows;              /* draw() may run on any core: no reseeding here */
    dims(a, &cols, &rows);
    rect_t b = board(a);
    ui_card(c, b, dp(20), 0);
    int cp = cell_px();
    int ox = b.x + (b.w - cols * cp) / 2, oy = b.y + (b.h - rows * cp) / 2;
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols; x++)
            if (st.cell[y][x])
                gfx_rrect(c, (rect_t){ ox + x * cp + 1, oy + y * cp + 1, cp - 2, cp - 2 }, dp(3), ui.accent);
    ui_button(c, btn(a, 0), st.running ? "Pause" : "Play", ui.accent, RGB(255, 255, 255));
    ui_button(c, btn(a, 1), "Shuffle", RGBA(255, 255, 255, 30), ui.text);
    ui_button(c, btn(a, 2), "Clear", RGBA(255, 255, 255, 30), ui.text);
    char buf[32];
    fmt(buf, sizeof buf, "Generation %d", st.gen);
    rect_t g = btn(a, 3);
    gfx_text(c, ui.small, g.x + dp(8), g.y + (g.h - ui.small->line) / 2, buf, ui.text2);
}

static int cell_at(rect_t a, int px, int py, int *cx, int *cy) {
    rect_t b = board(a);
    int cp = cell_px();
    int ox = b.x + (b.w - st.cols * cp) / 2, oy = b.y + (b.h - st.rows * cp) / 2;
    *cx = (px - ox) / cp; *cy = (py - oy) / cp;
    return px >= ox && py >= oy && *cx < st.cols && *cy < st.rows;
}

static int event(const event_t *e, rect_t a) {
    int cx, cy;
    geometry(a);
    if (e->type == EV_KEY && e->ch == ' ') { st.running = !st.running; return 1; }
    if (e->type == EV_DOWN && cell_at(a, e->x, e->y, &cx, &cy)) {
        st.painting = 1;
        st.paint_val = !st.cell[cy][cx];
        st.cell[cy][cx] = (u8)st.paint_val;
        return 1;
    }
    if (e->type == EV_MOVE && st.painting && cell_at(a, e->x, e->y, &cx, &cy)) {
        st.cell[cy][cx] = (u8)st.paint_val;
        return 1;
    }
    if (e->type == EV_UP && st.painting) { st.painting = 0; st.tap.down = 0; return 1; }
    if (!tap_track(&st.tap, e, dp(12))) return 0;
    if (in_rect(btn(a, 0), e->x, e->y)) st.running = !st.running;
    else if (in_rect(btn(a, 1), e->x, e->y)) seed();
    else if (in_rect(btn(a, 2), e->x, e->y)) { memset(st.cell, 0, sizeof st.cell); st.gen = 0; }
    else return 0;
    return 1;
}

static int tick(u64 now) {
    if (!st.running || st.painting || now - st.last < 120) return 0;
    geometry(shell_app_area());
    st.last = now;
    step();
    return 1;
}

const app_t app_life = { "Life", "Cellular automaton", RGB(0x3c, 0xc8, 0x6e), icon, on_open, draw, event, tick };
