/* Files: browse every FAT volume the firmware can see (eMMC ESP, SD, USB). */
#include "../ui/shell.h"
#include "../kernel/vfs.h"

#define MAX_ENTRIES 256
#define PREVIEW_MAX 8192

typedef struct { char name[96]; int dir; u64 size; } entry_t;

static struct {
    int vol;                    /* -1 = volume list */
    c16 path[512];              /* current directory, "\\"-separated */
    entry_t *ent;
    int n;
    char *preview;              /* non-NULL while viewing a file */
    char preview_name[96];
    scroll_t sc;
    int pressed;
    tap_t tap;
    char error[96];
} st = { .vol = -1, .pressed = -1 };

static void icon(canvas_t *c, float cx, float cy, float r, u32 fg) {
    rect_t body = { (int)(cx - r * 0.85f), (int)(cy - r * 0.45f), (int)(r * 1.7f), (int)(r * 1.2f) };
    rect_t tab = { (int)(cx - r * 0.85f), (int)(cy - r * 0.7f), (int)(r * 0.8f), (int)(r * 0.5f) };
    gfx_rrect(c, tab, (int)(r * 0.15f), fg);
    gfx_rrect(c, body, (int)(r * 0.18f), fg);
}

static EFI_FILE_PROTOCOL *open_path(const c16 *path) {
    EFI_FILE_PROTOCOL *root, *f;
    if (st.vol < 0 || EFI_ERROR(k.vol[st.vol].fs->OpenVolume(k.vol[st.vol].fs, &root))) return NULL;
    if (!path[0]) return root;
    EFI_STATUS s = root->Open(root, &f, path, EFI_FILE_MODE_READ, 0);
    root->Close(root);
    return EFI_ERROR(s) ? NULL : f;
}

static int cmp_entry(const entry_t *a, const entry_t *b) {
    if (a->dir != b->dir) return b->dir - a->dir;
    const char *x = a->name, *y = b->name;
    for (; *x && *y; x++, y++) {
        char p = (*x >= 'A' && *x <= 'Z') ? *x + 32 : *x, q = (*y >= 'A' && *y <= 'Z') ? *y + 32 : *y;
        if (p != q) return p - q;
    }
    return *x - *y;
}

/* native mode: the firmware's FAT driver is gone; browse the RAM copy (vfs) */
static vnode_t *vfs_node(const c16 *path) {
    char p[512];
    str16_to_utf8(p, sizeof p, path);
    for (char *c = p; *c; c++) if (*c == '\\') *c = '/';
    return vfs_lookup(p[0] ? p : "/");
}

static void sort_entries(void);

static void load_dir(void) {
    st.n = 0;
    st.error[0] = 0;
    st.sc.off = 0;
    if (!st.ent) st.ent = kalloc(sizeof(entry_t) * MAX_ENTRIES);
    if (k.native) {
        vnode_t *d = vfs_node(st.path);
        if (!d || !d->dir) { fmt(st.error, sizeof st.error, "Could not open this folder"); return; }
        for (vnode_t *c = d->child; c && st.n < MAX_ENTRIES; c = c->sibling) {
            entry_t *e = &st.ent[st.n++];
            strlcpy(e->name, c->name, sizeof e->name);
            e->dir = c->dir;
            e->size = vfs_size(c);
        }
        sort_entries();
        return;
    }
    EFI_FILE_PROTOCOL *d = open_path(st.path);
    if (!d) { fmt(st.error, sizeof st.error, "Could not open this folder"); return; }
    u8 *buf = kalloc(1024);
    for (;;) {
        UINTN sz = 1024;
        if (EFI_ERROR(d->Read(d, &sz, buf)) || sz == 0) break;
        EFI_FILE_INFO *fi = (EFI_FILE_INFO *)buf;
        if (fi->FileName[0] == '.' && (!fi->FileName[1] || (fi->FileName[1] == '.' && !fi->FileName[2]))) continue;
        if (st.n >= MAX_ENTRIES) break;
        entry_t *e = &st.ent[st.n++];
        str16_to_utf8(e->name, sizeof e->name, fi->FileName);
        e->dir = (fi->Attribute & EFI_FILE_DIRECTORY) != 0;
        e->size = fi->FileSize;
    }
    kfree(buf);
    d->Close(d);
    sort_entries();
}

/* insertion sort: folders first, then case-insensitive name */
static void sort_entries(void) {
    for (int i = 1; i < st.n; i++) {
        entry_t tmp = st.ent[i];
        int j = i - 1;
        while (j >= 0 && cmp_entry(&st.ent[j], &tmp) > 0) { st.ent[j + 1] = st.ent[j]; j--; }
        st.ent[j + 1] = tmp;
    }
}

static void path_push(const char *name) {
    usize n = str16len(st.path);
    c16 wide[96];
    utf8_to_str16(wide, 96, name);
    if (n + str16len(wide) + 2 >= ARRAY_LEN(st.path)) return;
    st.path[n++] = '\\';
    for (usize i = 0; wide[i]; i++) st.path[n++] = wide[i];
    st.path[n] = 0;
}

static void path_pop(void) {
    usize n = str16len(st.path);
    while (n > 0 && st.path[n - 1] != '\\') n--;
    st.path[n > 0 ? n - 1 : 0] = 0;
}

static void open_file(const entry_t *e) {
    path_push(e->name);
    UINTN sz = PREVIEW_MAX;
    if (k.native) {
        vnode_t *n = vfs_node(st.path);
        path_pop();
        if (!n) { fmt(st.error, sizeof st.error, "Could not open %s", e->name); return; }
        if (!st.preview) st.preview = kalloc(PREVIEW_MAX + 1);
        i64 r = vfs_read(n, 0, st.preview, PREVIEW_MAX);
        sz = r > 0 ? (UINTN)r : 0;
    } else {
        EFI_FILE_PROTOCOL *f = open_path(st.path);
        path_pop();
        if (!f) { fmt(st.error, sizeof st.error, "Could not open %s", e->name); return; }
        if (!st.preview) st.preview = kalloc(PREVIEW_MAX + 1);
        if (EFI_ERROR(f->Read(f, &sz, st.preview))) sz = 0;
        f->Close(f);
    }
    st.preview[sz] = 0;
    /* binary files: show a hex dump of the first bytes instead */
    int binary = 0;
    for (UINTN i = 0; i < sz; i++) {
        u8 ch = (u8)st.preview[i];
        if (ch == 0 || (ch < 9) || (ch > 13 && ch < 32)) { binary = 1; break; }
    }
    if (binary) {
        u8 raw[256];
        UINTN n = MIN(sz, sizeof raw);
        memcpy(raw, st.preview, n);
        usize o = 0;
        for (UINTN i = 0; i < n && o + 80 < PREVIEW_MAX; i += 8) {
            o += fmt(st.preview + o, PREVIEW_MAX - o, "%04x  ", (unsigned)i);
            for (UINTN j = i; j < i + 8 && j < n; j++) o += fmt(st.preview + o, PREVIEW_MAX - o, "%02x ", raw[j]);
            o += fmt(st.preview + o, PREVIEW_MAX - o, "\n");
        }
    }
    strlcpy(st.preview_name, e->name, sizeof st.preview_name);
    st.sc.off = 0;
}

static void on_open(void) { st.pressed = -1; }

/* --- drawing --- */
static rect_t list_rect(rect_t a) { return (rect_t){ a.x + dp(16), a.y + dp(44), a.w - dp(32), a.h - dp(52) }; }
static int row_h(void) { return dp(60); }

static void breadcrumb(char *buf, usize cap) {
    if (st.vol < 0) { strlcpy(buf, "All volumes", cap); return; }
    char p[256];
    str16_to_utf8(p, sizeof p, st.path);
    fmt(buf, cap, "%s  %s", k.vol[st.vol].label, p[0] ? p : "\\");
}

static void draw_row(canvas_t *c, rect_t r, const char *title, const char *sub, int dir, int pressed) {
    if (pressed) gfx_rrect(c, r, dp(14), RGBA(255, 255, 255, 26));
    float cx = r.x + dp(26), cy = r.y + r.h / 2.0f;
    gfx_circle(c, cx, cy, dp(17), dir ? ALPHA(app_files.color, 200) : RGBA(255, 255, 255, 26));
    if (dir) icon(c, cx, cy, dp(10), RGB(255, 255, 255));
    else {
        rect_t pg = { (int)cx - dp(6), (int)cy - dp(8), dp(12), dp(16) };
        gfx_rrect_outline(c, pg, dp(2), dp(2), ui.text2);
    }
    gfx_text_fit(c, ui.label, r.x + dp(56), r.y + dp(8), r.w - dp(64), title, ui.text);
    gfx_text_fit(c, ui.small, r.x + dp(56), r.y + dp(8) + ui.label->line, r.w - dp(64), sub, ui.text2);
}

static void draw(canvas_t *c, rect_t a) {
    char buf[300], sub[64];
    breadcrumb(buf, sizeof buf);
    gfx_text_fit(c, ui.body, a.x + dp(24), a.y + dp(8), a.w - dp(48), st.preview ? st.preview_name : buf, ui.text2);
    rect_t L = list_rect(a);
    ui_card(c, L, dp(20), 0);
    rect_t inner = { L.x + dp(8), L.y + dp(8), L.w - dp(16), L.h - dp(16) };
    gfx_clip(c, inner);

    if (st.preview) {
        /* text viewer: wrap on newlines, clip long lines */
        int lh = ui.small->line, y = inner.y + dp(8) - st.sc.off, lines = 0;
        const char *p = st.preview;
        char line[200];
        while (*p) {
            usize n = 0;
            while (*p && *p != '\n' && n + 1 < sizeof line) { if (*p != '\r') line[n++] = (*p == '\t') ? ' ' : *p; p++; }
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            line[n] = 0;
            if (y + lh > inner.y && y < inner.y + inner.h) gfx_text_fit(c, ui.small, inner.x + dp(12), y, inner.w - dp(24), line, ui.text);
            y += lh; lines++;
        }
        st.sc.max = lines * lh + dp(16) - inner.h;
    } else if (st.vol < 0) {
        for (int i = 0; i < k.n_vol; i++) {
            rect_t r = { inner.x, inner.y + i * row_h() - st.sc.off, inner.w, row_h() - dp(4) };
            char size[24], free_[24];
            fmt_bytes(size, sizeof size, k.vol[i].size);
            fmt_bytes(free_, sizeof free_, k.vol[i].free);
            if (k.native) fmt(sub, sizeof sub, "%s", k.vol[i].boot ? "RAM copy of the boot stick" : "needs a native storage driver");
            else fmt(sub, sizeof sub, "%s  \xc2\xb7  %s free%s", size, free_, k.vol[i].boot ? "  \xc2\xb7  QRT boot volume" : "");
            draw_row(c, r, k.vol[i].label, sub, 1, st.pressed == i);
        }
        if (!k.n_vol) gfx_text_center(c, ui.body, inner, "The firmware exposes no FAT volumes", ui.text3);
        if (st.error[0]) gfx_text_center(c, ui.body, (rect_t){ inner.x, inner.y + inner.h - dp(60), inner.w, dp(40) }, st.error, ui.text3);
        st.sc.max = k.n_vol * row_h() - inner.h;
    } else {
        rect_t up = { inner.x, inner.y - st.sc.off, inner.w, row_h() - dp(4) };
        draw_row(c, up, "..", "Up one level", 1, st.pressed == 0);
        for (int i = 0; i < st.n; i++) {
            rect_t r = { inner.x, inner.y + (i + 1) * row_h() - st.sc.off, inner.w, row_h() - dp(4) };
            if (r.y + r.h < inner.y || r.y > inner.y + inner.h) continue;
            if (st.ent[i].dir) strlcpy(sub, "Folder", sizeof sub);
            else fmt_bytes(sub, sizeof sub, st.ent[i].size);
            draw_row(c, r, st.ent[i].name, sub, st.ent[i].dir, st.pressed == i + 1);
        }
        if (st.error[0]) gfx_text_center(c, ui.body, inner, st.error, ui.text3);
        st.sc.max = (st.n + 1) * row_h() - inner.h;
    }
    gfx_unclip(c);
    gfx_clip(c, a);
}

static int hit_row(rect_t a, int x, int y) {
    rect_t L = list_rect(a);
    rect_t inner = { L.x + dp(8), L.y + dp(8), L.w - dp(16), L.h - dp(16) };
    if (!in_rect(inner, x, y)) return -1;
    return (y - inner.y + st.sc.off) / row_h();
}

static void go_up(void) {
    if (st.preview) { kfree(st.preview); st.preview = NULL; st.sc.off = 0; return; }
    if (st.vol < 0) return;
    if (!st.path[0]) { st.vol = -1; st.sc.off = 0; return; }
    path_pop();
    load_dir();
}

static void activate_row(int row) {
    if (st.vol < 0) {
        if (row < 0 || row >= k.n_vol) return;
        if (k.native && !k.vol[row].boot) {
            fmt(st.error, sizeof st.error, "Only the boot stick is available until QRT has its own storage driver");
            return;
        }
        st.vol = row; st.path[0] = 0; load_dir();
        return;
    }
    if (row == 0) { go_up(); return; }
    if (row - 1 >= st.n) return;
    entry_t *e = &st.ent[row - 1];
    if (e->dir) { path_push(e->name); load_dir(); }
    else open_file(e);
}

static int event(const event_t *e, rect_t a) {
    rect_t L = list_rect(a);
    int redraw = scroll_event(&st.sc, e, L, dp(60));
    if (e->type == EV_KEY && e->ch == 8) { go_up(); return 1; }
    if (st.preview) { tap_track(&st.tap, e, dp(10)); return redraw; }
    int row = hit_row(a, e->x, e->y);
    int tapped = tap_track(&st.tap, e, dp(10));
    if (e->type == EV_DOWN) { st.pressed = row; return 1; }
    if (e->type == EV_MOVE && st.sc.moved > dp(10) && st.pressed >= 0) { st.pressed = -1; redraw = 1; }
    if (e->type == EV_UP) {
        if (tapped && row >= 0 && row == st.pressed) activate_row(row);
        st.pressed = -1;
        return 1;
    }
    return redraw;
}

const app_t app_files = { "Files", "eMMC, SD & USB", RGB(0x2e, 0xc4, 0xb6), icon, on_open, draw, event, NULL };
