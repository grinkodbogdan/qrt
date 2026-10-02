/*
 * osk.c - the on-screen keyboard.
 *
 * A panel docked to the bottom of the content area (left of the app dock).
 * Taps become ordinary EV_KEY events, exactly what a hardware keyboard
 * produces, so apps need no special handling: they only ask the shell to
 * show the keyboard when one of their text fields is tapped
 * (shell_keyboard(1)).  Three layers: letters, digits/punctuation and more
 * symbols; shift is one-shot, a double tap locks caps.  Backspace repeats
 * while held.
 */
#include "osk.h"

enum { K_CHAR, K_SHIFT, K_BACK, K_LAYER, K_ENTER, K_SPACE, K_HIDE, K_LEFT, K_RIGHT };
typedef struct { const char *label; int kind; char ch; float w; } osk_key_t;

#define ROWS 4
#define MAXK 11

/* layer 0 letters (drawn upper-case when shifted), 1 digits, 2 symbols */
static const osk_key_t layers[3][ROWS][MAXK] = {
    {
        { {"q",0,'q',1},{"w",0,'w',1},{"e",0,'e',1},{"r",0,'r',1},{"t",0,'t',1},{"y",0,'y',1},{"u",0,'u',1},{"i",0,'i',1},{"o",0,'o',1},{"p",0,'p',1} },
        { {"a",0,'a',1},{"s",0,'s',1},{"d",0,'d',1},{"f",0,'f',1},{"g",0,'g',1},{"h",0,'h',1},{"j",0,'j',1},{"k",0,'k',1},{"l",0,'l',1} },
        { {"shift",K_SHIFT,0,1.5f},{"z",0,'z',1},{"x",0,'x',1},{"c",0,'c',1},{"v",0,'v',1},{"b",0,'b',1},{"n",0,'n',1},{"m",0,'m',1},{"back",K_BACK,0,1.5f} },
        { {"?123",K_LAYER,1,1.5f},{"-",0,'-',1},{"/",0,'/',1},{"space",K_SPACE,' ',4},{".",0,'.',1},{"enter",K_ENTER,'\r',1.5f},{"hide",K_HIDE,0,1} },
    },
    {
        { {"1",0,'1',1},{"2",0,'2',1},{"3",0,'3',1},{"4",0,'4',1},{"5",0,'5',1},{"6",0,'6',1},{"7",0,'7',1},{"8",0,'8',1},{"9",0,'9',1},{"0",0,'0',1} },
        { {"@",0,'@',1},{"#",0,'#',1},{"$",0,'$',1},{"_",0,'_',1},{"&",0,'&',1},{"-",0,'-',1},{"+",0,'+',1},{"(",0,'(',1},{")",0,')',1},{"/",0,'/',1} },
        { {"=\\<",K_LAYER,2,1.5f},{"*",0,'*',1},{"\"",0,'"',1},{"'",0,'\'',1},{":",0,':',1},{";",0,';',1},{"!",0,'!',1},{"?",0,'?',1},{"back",K_BACK,0,1.5f} },
        { {"ABC",K_LAYER,0,1.5f},{",",0,',',1},{"left",K_LEFT,0,1},{"space",K_SPACE,' ',3},{"right",K_RIGHT,0,1},{".",0,'.',1},{"enter",K_ENTER,'\r',1.5f},{"hide",K_HIDE,0,1} },
    },
    {
        { {"~",0,'~',1},{"`",0,'`',1},{"|",0,'|',1},{"^",0,'^',1},{"%",0,'%',1},{"=",0,'=',1},{"{",0,'{',1},{"}",0,'}',1},{"[",0,'[',1},{"]",0,']',1} },
        { {"<",0,'<',1},{">",0,'>',1},{"\\",0,'\\',1},{"tab",0,'\t',1.4f},{"esc",0,27,1.4f},{":",0,':',1},{";",0,';',1},{"!",0,'!',1},{"?",0,'?',1} },
        { {"?123",K_LAYER,1,1.5f},{".",0,'.',1},{",",0,',',1},{"'",0,'\'',1},{"\"",0,'"',1},{"*",0,'*',1},{"&",0,'&',1},{"back",K_BACK,0,1.5f} },
        { {"ABC",K_LAYER,0,1.5f},{"left",K_LEFT,0,1},{"space",K_SPACE,' ',4},{"right",K_RIGHT,0,1},{"enter",K_ENTER,'\r',1.5f},{"hide",K_HIDE,0,1} },
    },
};

static struct {
    int visible, layer, shift, caps;
    int pinned;                    /* the desk-mode controller: only the hide key closes it */
    int down_r, down_k;            /* key under the finger, -1 none */
    u64 repeat_at;
    u64 last_shift_ms;
} o = { .down_r = -1, .down_k = -1 };

static int row_len(int layer, int r) { int n = 0; while (n < MAXK && layers[layer][r][n].label) n++; return n; }

int osk_visible(void) { return o.visible; }
void osk_show(void) { if (!o.visible) { o.visible = 1; o.layer = 0; o.shift = 0; } }
void osk_hide(void) { if (!o.pinned) { o.visible = 0; o.down_r = o.down_k = -1; } }
void osk_pin(int on) { o.pinned = on; if (on) osk_show(); }
int osk_height(void) { return o.visible ? dp(4 * 54 + 16) : 0; }

/* The panel spans 'area' (the content area left of the dock) at its bottom. */
rect_t osk_rect(rect_t area) {
    int h = osk_height();
    return (rect_t){ area.x, area.y + area.h - h, area.w, h };
}

static rect_t key_rect(rect_t panel, int r, int k) {
    int pad = dp(8), gap = dp(6), kh = dp(48);
    int maxw = MIN(panel.w - 2 * pad, dp(820));
    int x0 = panel.x + (panel.w - maxw) / 2;
    /* width unit: the widest row fits exactly */
    float units_max = 0;
    for (int rr = 0; rr < ROWS; rr++) {
        float u = 0;
        for (int kk = 0; kk < row_len(o.layer, rr); kk++) u += layers[o.layer][rr][kk].w;
        units_max = MAX(units_max, u);
    }
    float unit = (maxw - gap * 10) / units_max;
    float u = 0;
    int n = row_len(o.layer, r);
    for (int kk = 0; kk < n; kk++) u += layers[o.layer][r][kk].w;
    float row_w = u * unit + gap * (n - 1);
    float x = x0 + (maxw - row_w) / 2;
    for (int kk = 0; kk < k; kk++) x += layers[o.layer][r][kk].w * unit + gap;
    int y = panel.y + pad + r * (kh + gap);
    return (rect_t){ (int)x, y, (int)(layers[o.layer][r][k].w * unit), kh };
}

static void glyph(canvas_t *c, rect_t r, const osk_key_t *key, u32 fg) {
    float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f, s = dp(7), w = dp(2);
    switch (key->kind) {
    case K_SHIFT:
        gfx_line(c, cx - s, cy + s * 0.1f, cx, cy - s, w, fg);
        gfx_line(c, cx, cy - s, cx + s, cy + s * 0.1f, w, fg);
        gfx_line(c, cx, cy - s, cx, cy + s, w, fg);
        if (o.caps) gfx_line(c, cx - s, cy + s * 1.4f, cx + s, cy + s * 1.4f, w, fg);
        return;
    case K_BACK:
        gfx_line(c, cx - s * 1.2f, cy, cx - s * 0.4f, cy - s * 0.8f, w, fg);
        gfx_line(c, cx - s * 1.2f, cy, cx - s * 0.4f, cy + s * 0.8f, w, fg);
        gfx_line(c, cx - s * 0.4f, cy - s * 0.8f, cx + s * 1.2f, cy - s * 0.8f, w, fg);
        gfx_line(c, cx - s * 0.4f, cy + s * 0.8f, cx + s * 1.2f, cy + s * 0.8f, w, fg);
        gfx_line(c, cx + s * 1.2f, cy - s * 0.8f, cx + s * 1.2f, cy + s * 0.8f, w, fg);
        gfx_line(c, cx - s * 0.05f, cy - s * 0.35f, cx + s * 0.75f, cy + s * 0.35f, w, fg);
        gfx_line(c, cx - s * 0.05f, cy + s * 0.35f, cx + s * 0.75f, cy - s * 0.35f, w, fg);
        return;
    case K_ENTER:
        gfx_line(c, cx + s, cy - s * 0.8f, cx + s, cy + s * 0.2f, w, fg);
        gfx_line(c, cx + s, cy + s * 0.2f, cx - s, cy + s * 0.2f, w, fg);
        gfx_line(c, cx - s, cy + s * 0.2f, cx - s * 0.4f, cy - s * 0.4f, w, fg);
        gfx_line(c, cx - s, cy + s * 0.2f, cx - s * 0.4f, cy + s * 0.8f, w, fg);
        return;
    case K_HIDE:
        gfx_rrect_outline(c, (rect_t){ (int)(cx - s * 1.2f), (int)(cy - s * 0.9f), (int)(s * 2.4f), (int)(s * 1.3f) }, dp(2), dp(1.5f), fg);
        gfx_line(c, cx - s * 0.5f, cy + s * 0.75f, cx, cy + s * 1.15f, w, fg);
        gfx_line(c, cx, cy + s * 1.15f, cx + s * 0.5f, cy + s * 0.75f, w, fg);
        return;
    case K_LEFT: case K_RIGHT: {
        float d = key->kind == K_LEFT ? -1 : 1;
        gfx_line(c, cx - d * s * 0.4f, cy - s * 0.7f, cx + d * s * 0.4f, cy, w, fg);
        gfx_line(c, cx + d * s * 0.4f, cy, cx - d * s * 0.4f, cy + s * 0.7f, w, fg);
        return;
    }
    case K_SPACE:
        gfx_line(c, cx - s * 2, cy + s * 0.3f, cx + s * 2, cy + s * 0.3f, w, ALPHA(fg, 120));
        return;
    }
    char buf[8];
    const char *label = key->label;
    if (key->kind == K_CHAR && o.layer == 0 && (o.shift || o.caps) && key->ch >= 'a' && key->ch <= 'z') {
        buf[0] = (char)(key->ch - 32); buf[1] = 0; label = buf;
    }
    const font_t *f = strlen(label) > 1 ? ui.small : ui.title;
    gfx_text_center(c, f, r, label, fg);
}

void osk_draw(canvas_t *c, rect_t area) {
    if (!o.visible) return;
    rect_t p = osk_rect(area);
    gfx_fill(c, p, RGBA(0x10, 0x0d, 0x1c, 235));
    gfx_fill(c, (rect_t){ p.x, p.y, p.w, 1 }, ui.stroke);
    for (int r = 0; r < ROWS; r++)
        for (int k = 0; k < row_len(o.layer, r); k++) {
            const osk_key_t *key = &layers[o.layer][r][k];
            rect_t kr = key_rect(p, r, k);
            int pressed = o.down_r == r && o.down_k == k;
            int special = key->kind != K_CHAR && key->kind != K_SPACE;
            u32 fill = pressed ? ALPHA(ui.accent, 200)
                     : key->kind == K_ENTER ? ALPHA(ui.accent, 150)
                     : (key->kind == K_SHIFT && (o.shift || o.caps)) ? RGBA(255, 255, 255, 90)
                     : special ? RGBA(255, 255, 255, 20) : RGBA(255, 255, 255, 38);
            gfx_rrect(c, kr, dp(9), fill);
            glyph(c, kr, key, ui.text);
        }
}

static int hit(rect_t p, int x, int y, int *rr, int *kk) {
    for (int r = 0; r < ROWS; r++)
        for (int k = 0; k < row_len(o.layer, r); k++) {
            rect_t kr = key_rect(p, r, k);
            /* generous targets: the gaps belong to the nearest key */
            kr.x -= dp(3); kr.w += dp(6); kr.y -= dp(3); kr.h += dp(6);
            if (in_rect(kr, x, y)) { *rr = r; *kk = k; return 1; }
        }
    return 0;
}

static int emit(const osk_key_t *key, event_t *out) {
    memset(out, 0, sizeof *out);
    out->type = EV_KEY;
    switch (key->kind) {
    case K_CHAR: {
        char ch = key->ch;
        if (ch == 27) { out->scan = SCAN_ESC; return 1; }
        if (o.layer == 0 && (o.shift || o.caps) && ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        out->ch = (c16)(u8)ch;
        if (o.shift) o.shift = 0;
        return 1;
    }
    case K_SPACE: out->ch = ' '; return 1;
    case K_BACK: out->ch = 8; return 1;
    case K_ENTER: out->ch = '\r'; return 1;
    case K_LEFT: out->scan = SCAN_LEFT; return 1;
    case K_RIGHT: out->scan = SCAN_RIGHT; return 1;
    }
    return 0;
}

int osk_pointer(const event_t *e, rect_t area, event_t *out, int max) {
    if (!o.visible || max < 1) return 0;
    rect_t p = osk_rect(area);
    int r, kk;
    if (e->type == EV_DOWN) {
        if (!hit(p, e->x, e->y, &r, &kk)) { o.down_r = o.down_k = -1; return 0; }
        o.down_r = r; o.down_k = kk;
        const osk_key_t *key = &layers[o.layer][r][kk];
        if (key->kind == K_BACK) { o.repeat_at = k_now_ms() + 450; return emit(key, out); }
        return 0;
    }
    if (e->type == EV_MOVE && o.down_r >= 0) {
        if (hit(p, e->x, e->y, &r, &kk)) { o.down_r = r; o.down_k = kk; }   /* slide to correct */
        return 0;
    }
    if (e->type != EV_UP || o.down_r < 0) return 0;
    const osk_key_t *key = &layers[o.layer][o.down_r][o.down_k];
    o.down_r = o.down_k = -1;
    switch (key->kind) {
    case K_SHIFT: {
        u64 now = k_now_ms();
        if (o.caps) { o.caps = 0; o.shift = 0; }
        else if (o.shift && now - o.last_shift_ms < 400) { o.caps = 1; o.shift = 0; }
        else o.shift = !o.shift;
        o.last_shift_ms = now;
        return 0;
    }
    case K_LAYER: o.layer = key->ch; return 0;
    case K_HIDE: o.visible = 0; return 0;
    case K_BACK: return 0;                     /* sent on press */
    }
    return emit(key, out);
}

int osk_tick(u64 now, event_t *out, int max) {
    if (!o.visible || o.down_r < 0 || max < 1) return 0;
    const osk_key_t *key = &layers[o.layer][o.down_r][o.down_k];
    if (key->kind != K_BACK || now < o.repeat_at) return 0;
    o.repeat_at = now + 70;
    return emit(key, out);
}
