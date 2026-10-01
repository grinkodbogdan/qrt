/*
 * browser.c - QRT's web browser: text, links and simple forms.
 *
 * Pages come from http.c (HTTP/1.1 over TCP, or TLS 1.3 for https://) and
 * are read by html.c into styled words; this file lays those out into
 * lines, draws the visible part and handles taps, the address bar,
 * history and GET/POST forms.  No CSS, scripts or images: the point is to
 * read pages and test the network.
 *
 * https:// pages are encrypted but the certificate is not checked (see
 * src/net/tls.h), and the status line says so.
 */
#include "../ui/shell.h"
#include "../net/http.h"
#include "html.h"

typedef struct { int x, y, w, h, node; const font_t *f; } box_t;

#define HIST 32
#define HOME_URL "about:home"

static const char home_html[] =
    "<title>Start</title><h1>QRT Browser</h1>"
    "<p>Type an address in the bar above, or words to search. Pages are shown as text and links: "
    "no pictures, styles or scripts.</p>"
    "<h2>Try</h2><ul>"
    "<li><a href=\"http://frogfind.com/\">FrogFind</a> - search made for simple browsers; it turns any site into a plain page</li>"
    "<li><a href=\"https://lite.duckduckgo.com/lite/\">DuckDuckGo Lite</a></li>"
    "<li><a href=\"https://en.wikipedia.org/wiki/Special:Random\">A random Wikipedia article</a></li>"
    "<li><a href=\"https://text.npr.org/\">NPR, text only</a></li>"
    "<li><a href=\"https://lite.cnn.com/\">CNN Lite</a></li>"
    "<li><a href=\"http://68k.news/\">68k.news</a> - news for vintage computers</li>"
    "<li><a href=\"http://neverssl.com/\">NeverSSL</a> - a plain http:// test page</li>"
    "<li><a href=\"https://example.com/\">example.com</a></li>"
    "</ul><h2>Secure pages</h2>"
    "<p>https:// pages are encrypted with TLS 1.3, but QRT does not check certificates yet, so it cannot prove "
    "who is on the other end. Do not type passwords or other secrets here.</p>";

static struct {
    char url[2300];                  /* the page shown */
    char edit[2300]; int editing, fresh;   /* address bar being typed in; fresh: the first key replaces it */
    http_t *req;
    char loading_url[2300];
    char status[200];
    int secure;
    hdoc_t doc;
    box_t *boxes; int n_boxes, cap_boxes;
    int layout_w, doc_h;
    scroll_t sc;
    tap_t tap;
    int focus;                       /* focused form field, -1 = none */
    int pressed_link;
    char *hist[HIST]; int hist_off[HIST]; int n_hist;
} B = { .focus = -1, .pressed_link = -1 };

/* ---- geometry ------------------------------------------------------------------ */
static rect_t bar_rect(rect_t a) { return (rect_t){ a.x + dp(12), a.y, a.w - dp(24), dp(48) }; }
static rect_t back_btn(rect_t a) { rect_t b = bar_rect(a); return (rect_t){ b.x, b.y, dp(48), dp(48) }; }
static rect_t reload_btn(rect_t a) { rect_t b = bar_rect(a); return (rect_t){ b.x + dp(56), b.y, dp(48), dp(48) }; }
static rect_t url_rect(rect_t a) { rect_t b = bar_rect(a); return (rect_t){ b.x + dp(112), b.y, b.w - dp(112), dp(48) }; }
static int status_y(rect_t a) { return a.y + dp(56); }
static rect_t page_rect(rect_t a) {
    int y = status_y(a) + ui.small->line + dp(8);
    return (rect_t){ a.x + dp(12), y, a.w - dp(24), a.y + a.h - y };
}

/* ---- fonts per style ----------------------------------------------------------- */
static const font_t *node_font(const hnode_t *n) {
    if (n->heading == 1) return font_pick(F_SEMIBOLD, dp(24));
    if (n->heading == 2) return font_pick(F_SEMIBOLD, dp(20));
    if (n->heading == 3) return font_pick(F_SEMIBOLD, dp(17));
    if (n->heading) return font_pick(F_SEMIBOLD, dp(15));
    if (n->flags & HF_MONO) return font_pick(F_MONO, dp(13));
    if (n->flags & HF_SMALL) return font_pick(F_REGULAR, dp(12));
    if (n->flags & HF_BOLD) return font_pick(F_SEMIBOLD, dp(15));
    return font_pick(F_REGULAR, dp(15));
}
static const font_t *body_font(void) { return font_pick(F_REGULAR, dp(15)); }

/* ---- layout: nodes -> positioned boxes ------------------------------------------- */
static void add_box(box_t b) {
    if (B.n_boxes == B.cap_boxes) {
        int nc = B.cap_boxes ? B.cap_boxes * 2 : 1024;
        box_t *q = kalloc((usize)nc * sizeof(box_t));
        if (B.boxes) { memcpy(q, B.boxes, (usize)B.n_boxes * sizeof(box_t)); kfree(B.boxes); }
        B.boxes = q; B.cap_boxes = nc;
    }
    B.boxes[B.n_boxes++] = b;
}

static int field_w(const hfield_t *f, int avail) {
    if (f->kind == FIELD_SUBMIT) return text_width(ui.label, f->value) + dp(36);
    return MIN(avail, dp(300));
}

/* finish a line: align the boxes on a common baseline */
static int end_line(int first, int y) {
    int asc = 0, desc = 0;
    for (int i = first; i < B.n_boxes; i++) {
        box_t *b = &B.boxes[i];
        int a = b->f ? b->f->ascent : b->h * 3 / 4, d = b->f ? b->f->line - b->f->ascent : b->h - b->h * 3 / 4;
        asc = MAX(asc, a); desc = MAX(desc, d);
    }
    if (first == B.n_boxes) return y;
    for (int i = first; i < B.n_boxes; i++) {
        box_t *b = &B.boxes[i];
        int a = b->f ? b->f->ascent : b->h * 3 / 4;
        b->y = y + asc - a;
    }
    return y + asc + desc + dp(3);
}

static void layout(int width) {
    B.n_boxes = 0;
    B.layout_w = width;
    int margin = dp(16), right = width - margin;
    int y = dp(12), x = margin, line_first = 0, left = margin;
    const font_t *bf = body_font();
    int space_w = text_width(bf, " ");
    for (int i = 0; i < B.doc.n_nodes; i++) {
        hnode_t *n = &B.doc.nodes[i];
        int indent_left = margin + n->indent * dp(22);
        switch (n->kind) {
        case N_TEXT: {
            const font_t *f = node_font(n);
            const char *s = B.doc.text + n->off;
            int w = text_width(f, s);
            if (B.n_boxes == line_first) { left = indent_left; x = left; }
            int sp = n->space && x > left ? text_width(f, " ") : 0;
            if (x + sp + w > right && x > left && !(n->flags & HF_PRE)) {
                y = end_line(line_first, y);
                line_first = B.n_boxes;
                x = left;
                sp = 0;
            }
            x += sp;
            add_box((box_t){ x, y, w, f->line, i, f });
            x += w + ((n->flags & HF_BULLET) ? dp(8) : 0);
            break;
        }
        case N_FIELD: {
            hfield_t *fd = &B.doc.fields[n->field];
            if (B.n_boxes == line_first) { left = indent_left; x = left; }
            int w = field_w(fd, right - left), h = dp(40);
            int sp = n->space && x > left ? space_w : 0;
            if (x + sp + w > right && x > left) { y = end_line(line_first, y); line_first = B.n_boxes; x = left; sp = 0; }
            x += sp;
            add_box((box_t){ x, y, w, h, i, NULL });
            x += w;
            break;
        }
        case N_BREAK: {
            int empty = B.n_boxes == line_first;
            y = end_line(line_first, y);
            if (empty && n->flags) y += bf->line;            /* <br><br>: an empty line */
            line_first = B.n_boxes;
            x = left = indent_left;
            break;
        }
        case N_GAP:
            y = end_line(line_first, y);
            y += n->space * dp(7);
            line_first = B.n_boxes;
            x = left = indent_left;
            break;
        case N_HR:
            y = end_line(line_first, y);
            add_box((box_t){ margin, y + dp(8), width - 2 * margin, 1, i, NULL });
            y += dp(17);
            line_first = B.n_boxes;
            x = left = indent_left;
            break;
        }
    }
    y = end_line(line_first, y);
    B.doc_h = y + dp(40);
}

/* ---- pages ----------------------------------------------------------------------- */
static void set_doc_html(const char *url, const u8 *body, usize len, int plain, int latin1) {
    html_free(&B.doc);
    html_parse(&B.doc, url, body, len, plain, latin1);
    B.n_boxes = 0;                                     /* the old boxes point into the old document */
    B.focus = -1;
    B.layout_w = 0;                                    /* laid out on the next tick */
}

static void show_message(const char *url, const char *title, const char *msg) {
    char html[1600];
    fmt(html, sizeof html, "<title>%s</title><h2>%s</h2><p>%s</p><p><small>%s</small></p>", title, title, msg, url);
    set_doc_html(url, (const u8 *)html, strlen(html), 0, 0);
}

static void push_history(void) {
    if (!B.url[0]) return;
    if (B.n_hist == HIST) { kfree(B.hist[0]); memmove(B.hist, B.hist + 1, (HIST - 1) * sizeof B.hist[0]); memmove(B.hist_off, B.hist_off + 1, (HIST - 1) * sizeof B.hist_off[0]); B.n_hist--; }
    usize l = strlen(B.url);
    B.hist[B.n_hist] = kalloc(l + 1);
    memcpy(B.hist[B.n_hist], B.url, l + 1);
    B.hist_off[B.n_hist] = B.sc.off;
    B.n_hist++;
}

static void cancel_load(void) { if (B.req) { http_free(B.req); B.req = NULL; } }

static void load(const char *url, const char *post) {
    cancel_load();
    B.editing = 0;
    shell_keyboard(0);
    if (!strcmp(url, HOME_URL)) {
        strlcpy(B.url, HOME_URL, sizeof B.url);
        set_doc_html(B.url, (const u8 *)home_html, sizeof home_html - 1, 0, 0);
        B.secure = 0;
        B.sc.off = 0;
        strlcpy(B.status, "Built-in start page", sizeof B.status);
        shell_redraw();
        return;
    }
    strlcpy(B.loading_url, url, sizeof B.loading_url);
    B.sc.off = 0;
    B.req = post ? http_post(url, post) : http_get(url);
    strlcpy(B.status, "Starting...", sizeof B.status);
    shell_redraw();
}

static void navigate(const char *url, const char *post) {
    push_history();
    load(url, post);
}

/* what the address bar means: an address, or words to search for */
static void go_typed(const char *typed) {
    while (*typed == ' ') typed++;
    if (!*typed) return;
    int has_scheme = strstr(typed, "://") != NULL, has_space = strchr(typed, ' ') != NULL, has_dot = strchr(typed, '.') != NULL;
    if (!strcmp(typed, HOME_URL) || has_scheme || (!has_space && (has_dot || !strncmp(typed, "localhost", 9)))) { navigate(typed, NULL); return; }
    char q[600], url[800];
    url_encode_component(typed, q, sizeof q);
    fmt(url, sizeof url, "http://frogfind.com/?q=%s", q);
    navigate(url, NULL);
}

static void go_back(void) {
    if (!B.n_hist) return;
    B.n_hist--;
    char *u = B.hist[B.n_hist];
    int off = B.hist_off[B.n_hist];
    load(u, NULL);
    kfree(u);
    B.sc.off = off;                                    /* restored once the page is laid out */
}

static void submit_form(int form) {
    if (form < 0 || form >= B.doc.n_forms) return;
    hform_t *f = &B.doc.forms[form];
    char q[2048] = "";
    for (int i = 0; i < B.doc.n_fields; i++) {
        hfield_t *fd = &B.doc.fields[i];
        if (fd->form != form || !fd->name[0] || fd->kind == FIELD_SUBMIT) continue;
        char k[200], v[800];
        url_encode_component(fd->name, k, sizeof k);
        url_encode_component(fd->value, v, sizeof v);
        if (q[0]) strlcat(q, "&", sizeof q);
        strlcat(q, k, sizeof q);
        strlcat(q, "=", sizeof q);
        strlcat(q, v, sizeof q);
    }
    if (f->post) { navigate(f->action, q); return; }
    char url[3200];
    strlcpy(url, f->action, sizeof url);
    char *qm = strchr(url, '?');
    if (qm) *qm = 0;                                   /* a GET form replaces the query */
    strlcat(url, "?", sizeof url);
    strlcat(url, q, sizeof url);
    navigate(url, NULL);
}

/* ---- drawing ---------------------------------------------------------------------- */
static void icon(canvas_t *c, float cx, float cy, float r, u32 fg) {
    float w = dp(2);
    gfx_ring(c, cx, cy, r * 0.9f, w, fg);
    gfx_line(c, cx - r * 0.9f, cy, cx + r * 0.9f, cy, w, fg);
    gfx_line(c, cx - r * 0.75f, cy - r * 0.45f, cx + r * 0.75f, cy - r * 0.45f, w * 0.8f, fg);
    gfx_line(c, cx - r * 0.75f, cy + r * 0.45f, cx + r * 0.75f, cy + r * 0.45f, w * 0.8f, fg);
    /* a meridian: an ellipse through the poles */
    float px = cx, py = cy - r * 0.9f;
    for (int i = 1; i <= 16; i++) {
        float a = PI_F * i / 16;
        float qx = cx + fsin(a) * r * 0.4f, qy = cy - fcos(a) * r * 0.9f;
        gfx_line(c, px, py, qx, qy, w, fg);
        gfx_line(c, 2 * cx - px, py, 2 * cx - qx, qy, w, fg);
        px = qx; py = qy;
    }
}

static void draw_bar(canvas_t *c, rect_t a) {
    rect_t bb = back_btn(a), rb = reload_btn(a), ur = url_rect(a);
    u32 btn = RGBA(255, 255, 255, 26);
    gfx_circle(c, bb.x + bb.w / 2.0f, bb.y + bb.h / 2.0f, bb.w / 2.0f, btn);
    float cx = bb.x + bb.w / 2.0f, cy = bb.y + bb.h / 2.0f, s = dp(8);
    u32 fg = B.n_hist ? ui.text : ui.text3;
    gfx_line(c, cx - s, cy, cx + s, cy, dp(2.4f), fg);
    gfx_line(c, cx - s, cy, cx - s * 0.2f, cy - s * 0.8f, dp(2.4f), fg);
    gfx_line(c, cx - s, cy, cx - s * 0.2f, cy + s * 0.8f, dp(2.4f), fg);
    gfx_circle(c, rb.x + rb.w / 2.0f, rb.y + rb.h / 2.0f, rb.w / 2.0f, btn);
    cx = rb.x + rb.w / 2.0f; cy = rb.y + rb.h / 2.0f;
    if (B.req) {                                        /* stop: a cross */
        gfx_line(c, cx - s * 0.8f, cy - s * 0.8f, cx + s * 0.8f, cy + s * 0.8f, dp(2.4f), ui.text);
        gfx_line(c, cx - s * 0.8f, cy + s * 0.8f, cx + s * 0.8f, cy - s * 0.8f, dp(2.4f), ui.text);
    } else {                                            /* reload: an open ring with an arrow head */
        float px = cx + s, py = cy;
        for (int i = 1; i <= 12; i++) {
            float ang = 1.9f * PI_F * i / 12;
            float qx = cx + fcos(ang) * s, qy = cy - fsin(ang) * s;
            gfx_line(c, px, py, qx, qy, dp(2.2f), ui.text);
            px = qx; py = qy;
        }
        gfx_line(c, cx + s, cy, cx + s - dp(5), cy - dp(4), dp(2.2f), ui.text);
        gfx_line(c, cx + s, cy, cx + s + dp(4), cy - dp(5), dp(2.2f), ui.text);
    }
    gfx_rrect(c, ur, ur.h / 2, B.editing ? RGBA(0xf5, 0xf3, 0xfa, 235) : RGBA(255, 255, 255, 22));
    const char *shown = B.editing ? B.edit : B.req ? B.loading_url : B.url;
    u32 tc = B.editing ? RGB(0x20, 0x1a, 0x30) : ui.text;
    int tx = ur.x + dp(18), ty = ur.y + (ur.h - ui.body->line) / 2, maxw = ur.w - dp(36);
    /* show the end of a long address while typing, the start otherwise */
    const char *s2 = shown;
    if (B.editing) while (*s2 && text_width(ui.body, s2) > maxw) s2++;
    if (B.editing && B.fresh && shown[0]) {
        int w = MIN(text_width(ui.body, s2), maxw);
        gfx_rrect(c, (rect_t){ tx - dp(3), ty, w + dp(6), ui.body->line }, dp(4), ALPHA(ui.accent, 90));
    }
    int end = gfx_text_fit(c, ui.body, tx, ty, maxw, s2, tc);
    if (B.editing) gfx_fill(c, (rect_t){ MIN(end, ur.x + ur.w - dp(16)) + 1, ty + dp(2), MAX(1, dp(2)), ui.body->line - dp(4) }, ui.accent);
}

static void draw_status(canvas_t *c, rect_t a) {
    int y = status_y(a), x = a.x + dp(28);
    char line[260];
    if (B.req) fmt(line, sizeof line, "%s", http_progress(B.req)[0] ? http_progress(B.req) : "Loading...");
    else if (!strcmp(B.url, HOME_URL)) strlcpy(line, "Start page", sizeof line);
    else if (B.secure) fmt(line, sizeof line, "Encrypted (TLS 1.3), certificate not checked%s%s", B.doc.title[0] ? "  \xc2\xb7  " : "", B.doc.title);
    else fmt(line, sizeof line, "Not encrypted (http)%s%s", B.doc.title[0] ? "  \xc2\xb7  " : "", B.doc.title);
    u32 dot = B.req ? ui.accent : B.secure ? RGB(0xf2, 0xc1, 0x4e) : !strcmp(B.url, HOME_URL) ? ui.text3 : RGB(0xe5, 0x6b, 0x6f);
    gfx_circle(c, x - dp(12), y + ui.small->line / 2.0f, dp(4), dot);
    gfx_text_fit(c, ui.small, x, y, a.w - dp(48), line, ui.text2);
}

static void draw_page(canvas_t *c, rect_t a) {
    rect_t pr = page_rect(a);
    gfx_rrect(c, pr, dp(12), RGB(0x1e, 0x1e, 0x1e));
    rect_t old = c->clip;
    gfx_clip(c, pr);
    int top = B.sc.off, bottom = top + pr.h;
    /* first visible box: boxes are in reading order, so y only grows */
    int lo = 0, hi = B.n_boxes;
    while (lo < hi) { int mid = (lo + hi) / 2; if (B.boxes[mid].y + B.boxes[mid].h < top) lo = mid + 1; else hi = mid; }
    u32 link_col = RGB(0x8f, 0xb8, 0xff);
    for (int i = lo; i < B.n_boxes; i++) {
        box_t *b = &B.boxes[i];
        if (b->y > bottom) break;
        int x = pr.x + b->x, y = pr.y + b->y - top;
        hnode_t *n = &B.doc.nodes[b->node];
        if (n->kind == N_HR) { gfx_fill(c, (rect_t){ x, y, b->w, 1 }, ui.stroke); continue; }
        if (n->kind == N_FIELD) {
            hfield_t *f = &B.doc.fields[n->field];
            rect_t r = { x, y, b->w, b->h };
            if (f->kind == FIELD_SUBMIT) { ui_button(c, r, f->value, ALPHA(ui.accent, 200), ui.text); continue; }
            int foc = n->field == B.focus;
            gfx_rrect(c, r, dp(10), foc ? RGBA(0xf5, 0xf3, 0xfa, 235) : RGBA(255, 255, 255, 26));
            gfx_rrect_outline(c, r, dp(10), 1, foc ? ui.accent : ui.stroke);
            int tx = r.x + dp(10), ty = r.y + (r.h - ui.label->line) / 2;
            const char *v = f->value;
            while (*v && text_width(ui.label, v) > r.w - dp(24)) v++;
            int end = gfx_text(c, ui.label, tx, ty, v, foc ? RGB(0x20, 0x1a, 0x30) : ui.text);
            if (foc) gfx_fill(c, (rect_t){ end + 1, ty + dp(2), MAX(1, dp(2)), ui.label->line - dp(4) }, ui.accent);
            continue;
        }
        const char *s = B.doc.text + n->off;
        u32 col = n->link >= 0 ? link_col : (n->flags & HF_ALT) ? ui.text3 : n->heading ? ui.text : RGB(0xe6, 0xe3, 0xef);
        if (n->link >= 0 && n->link == B.pressed_link) gfx_rrect(c, (rect_t){ x - dp(2), y, b->w + dp(4), b->h }, dp(4), ALPHA(link_col, 50));
        gfx_text(c, b->f, x, y, s, col);
        if (n->link >= 0) gfx_fill(c, (rect_t){ x, y + b->f->ascent + dp(2), b->w, MAX(1, dp(1)) }, ALPHA(link_col, 120));
    }
    if (!B.n_boxes && B.req) gfx_text_center(c, ui.body, pr, "Loading...", ui.text3);
    c->clip = old;
    /* scroll position */
    if (B.sc.max > 0) {
        int th = MAX(dp(30), pr.h * pr.h / MAX(1, B.doc_h));
        int ty = pr.y + (pr.h - th) * B.sc.off / B.sc.max;
        gfx_rrect(c, (rect_t){ pr.x + pr.w - dp(6), ty, dp(3), th }, dp(2), RGBA(255, 255, 255, 70));
    }
}

static void draw(canvas_t *c, rect_t a) {
    draw_bar(c, a);
    draw_status(c, a);
    draw_page(c, a);
}

/* ---- input ------------------------------------------------------------------------ */
static int edit_key(char *buf, usize cap, const event_t *e, int *fresh) {
    usize n = strlen(buf);
    if (e->ch == 8) { if (fresh && *fresh) buf[0] = 0; else if (n) { n--; while (n && ((u8)buf[n] & 0xc0) == 0x80) n--; buf[n] = 0; } }
    else if (e->ch >= 32 && e->ch < 127) {
        if (fresh && *fresh) { buf[0] = 0; n = 0; }
        if (n + 1 < cap) { buf[n] = (char)e->ch; buf[n + 1] = 0; }
    } else return 0;
    if (fresh) *fresh = 0;
    return 1;
}

static int hit_box(rect_t a, int x, int y) {
    rect_t pr = page_rect(a);
    if (!in_rect(pr, x, y)) return -1;
    int px = x - pr.x, py = y - pr.y + B.sc.off;
    for (int i = 0; i < B.n_boxes; i++) {
        box_t *b = &B.boxes[i];
        if (b->y > py + dp(8)) break;
        if (px >= b->x - dp(4) && px < b->x + b->w + dp(4) && py >= b->y - dp(4) && py < b->y + b->h + dp(4)) return i;
    }
    return -1;
}

static int event(const event_t *e, rect_t a) {
    if (e->type == EV_KEY) {
        if (B.editing) {
            if (e->ch == '\r' || e->ch == '\n') { go_typed(B.edit); return 1; }
            if (e->scan == SCAN_ESC) { B.editing = 0; shell_keyboard(0); return 1; }
            return edit_key(B.edit, sizeof B.edit, e, &B.fresh);
        }
        if (B.focus >= 0) {
            hfield_t *f = &B.doc.fields[B.focus];
            if (e->ch == '\r' || e->ch == '\n') { int form = f->form; B.focus = -1; shell_keyboard(0); submit_form(form); return 1; }
            return edit_key(f->value, sizeof f->value, e, NULL);
        }
        if (e->ch == 8) { go_back(); return 1; }
        return scroll_event(&B.sc, e, page_rect(a), dp(48));
    }
    rect_t pr = page_rect(a);
    int redraw = in_rect(pr, e->x, e->y) || B.sc.dragging ? scroll_event(&B.sc, e, pr, dp(48)) : 0;
    if (e->type == EV_DOWN) {
        int hb = hit_box(a, e->x, e->y);
        B.pressed_link = hb >= 0 ? B.doc.nodes[B.boxes[hb].node].link : -1;
        if (B.pressed_link >= 0) redraw = 1;
    }
    if (e->type == EV_MOVE && B.pressed_link >= 0 && B.sc.moved > dp(8)) { B.pressed_link = -1; redraw = 1; }
    int tapped = tap_track(&B.tap, e, dp(12));
    if (e->type == EV_UP && B.pressed_link >= 0 && !tapped) { B.pressed_link = -1; redraw = 1; }
    if (!tapped) return redraw;
    int pl = B.pressed_link;
    B.pressed_link = -1;
    if (in_rect(back_btn(a), e->x, e->y)) { go_back(); return 1; }
    if (in_rect(reload_btn(a), e->x, e->y)) {
        if (B.req) { cancel_load(); strlcpy(B.status, "Stopped", sizeof B.status); }
        else load(B.url, NULL);
        return 1;
    }
    if (in_rect(url_rect(a), e->x, e->y)) {
        B.editing = 1;
        B.fresh = 1;
        B.focus = -1;
        strlcpy(B.edit, !strcmp(B.url, HOME_URL) ? "" : B.url, sizeof B.edit);
        shell_keyboard(1);
        return 1;
    }
    if (B.editing) { B.editing = 0; shell_keyboard(0); redraw = 1; }
    int hb = hit_box(a, e->x, e->y);
    if (hb < 0) { if (B.focus >= 0) { B.focus = -1; shell_keyboard(0); } return 1; }
    hnode_t *n = &B.doc.nodes[B.boxes[hb].node];
    if (n->kind == N_FIELD) {
        hfield_t *f = &B.doc.fields[n->field];
        if (f->kind == FIELD_SUBMIT) { B.focus = -1; submit_form(f->form); }
        else { B.focus = n->field; shell_keyboard(1); }
        return 1;
    }
    if (n->link >= 0 && n->link == pl) { navigate(B.doc.links[n->link], NULL); return 1; }
    return redraw;
}

/* ---- loading and layout, from the frame loop ---------------------------------------- */
static int ci_has(const char *hay, const char *needle) { return str_icontains(hay, needle); }

static void page_arrived(void) {
    usize len;
    const u8 *body = http_body(B.req, &len);
    const char *ct = http_content_type(B.req);
    strlcpy(B.url, http_url(B.req), sizeof B.url);
    B.secure = http_secure(B.req);
    int code = http_code(B.req);
    int html = !ct[0] || ci_has(ct, "html") || ci_has(ct, "xml");
    int text = ci_has(ct, "text/");
    int latin1 = ci_has(ct, "8859") || ci_has(ct, "1252") || ci_has(ct, "latin");
    if (!latin1 && !html_is_utf8(body, len)) latin1 = 1;
    if (html) set_doc_html(B.url, body, len, 0, latin1);
    else if (text || ci_has(ct, "json") || ci_has(ct, "javascript")) set_doc_html(B.url, body, len, 1, latin1);
    else {
        char m[300];
        fmt(m, sizeof m, "This address is a %s file (%u KB). QRT's browser shows web pages and text only.", ct, (u32)(len / 1024));
        show_message(B.url, "Not a web page", m);
    }
    if (code >= 400 && !B.doc.n_nodes) {
        char m[64];
        fmt(m, sizeof m, "The server answered %d.", code);
        show_message(B.url, "Error", m);
    }
    fmt(B.status, sizeof B.status, "%d", code);
    klog("browser: %s -> %d, %s, %u bytes%s", B.url, code, ct[0] ? ct : "no type", (u32)len, B.secure ? " (TLS 1.3)" : "");
}

static int tick(u64 now) {
    (void)now;
    int redraw = 0;
    rect_t a = shell_app_area();
    if (B.req) {
        static char last[160];
        int r = http_poll(B.req);
        const char *p = http_progress(B.req);
        if (strcmp(p, last)) { strlcpy(last, p, sizeof last); shell_damage((rect_t){ a.x, status_y(a) - dp(2), a.w, ui.small->line + dp(4) }); }
        if (r == 1) { int keep = B.sc.off; page_arrived(); http_free(B.req); B.req = NULL; B.sc.off = keep; redraw = 1; }
        else if (r < 0) {
            char u[2300];
            strlcpy(u, B.loading_url, sizeof u);
            strlcpy(B.url, u, sizeof B.url);
            show_message(u, "Could not open this page", http_error(B.req));
            klog("browser: %s failed: %s", u, http_error(B.req));
            B.secure = 0;
            http_free(B.req);
            B.req = NULL;
            B.sc.off = 0;
            redraw = 1;
        }
    }
    rect_t pr = page_rect(a);
    if (B.layout_w != pr.w) { layout(pr.w); redraw = 1; }
    B.sc.max = MAX(0, B.doc_h - pr.h);
    B.sc.off = CLAMP(B.sc.off, 0, B.sc.max);
    /* a partial shell_damage() above tells the shell this app reports its own
     * damage, so a full redraw must be reported the same way */
    if (redraw) shell_damage(a);
    return redraw;
}

static void on_open(void) {
    if (!B.url[0] && !B.req) load(HOME_URL, NULL);
}

const app_t app_browser = { "Browser", "Web pages as text", RGB(0xff, 0x70, 0x43), icon, on_open, draw, event, tick };
