/*
 * html.c - a forgiving HTML reader for the browser.
 *
 * One pass over the bytes: tags change the current style (bold, mono,
 * heading, link, list depth), block tags become breaks and gaps, text
 * becomes words, entities are decoded to UTF-8, and <script>, <style>,
 * <svg> and friends are skipped whole.  Malformed markup never fails: the
 * worst case is odd styling.
 */
#include "html.h"
#ifndef HTML_HOST_TEST
#include "../net/http.h"
#endif

static void *grow(void *p, int n, int *cap, usize elem) {
    if (n < *cap) return p;
    int nc = *cap ? *cap * 2 : 256;
    void *q = kalloc((usize)nc * elem);
    if (p) { memcpy(q, p, (usize)*cap * elem); kfree(p); }
    *cap = nc;
    return q;
}

static u32 text_put(hdoc_t *d, const char *s, usize n) {
    if (d->text_len + n + 1 > d->text_cap) {
        usize nc = MAX(d->text_cap * 2, d->text_len + n + 4096);
        char *q = kalloc(nc);
        if (d->text) { memcpy(q, d->text, d->text_len); kfree(d->text); }
        d->text = q; d->text_cap = nc;
    }
    u32 off = (u32)d->text_len;
    memcpy(d->text + d->text_len, s, n);
    d->text_len += n;
    d->text[d->text_len++] = 0;
    return off;
}

static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int is_space(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static int streq_ci(const char *a, const char *b) { while (*a && *b) if (lc(*a++) != lc(*b++)) return 0; return !*a && !*b; }

/* ---- entities ------------------------------------------------------------------- */
static int put_utf8(char *o, u32 cp) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xc0 | cp >> 6); o[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) { o[0] = (char)(0xe0 | cp >> 12); o[1] = (char)(0x80 | ((cp >> 6) & 63)); o[2] = (char)(0x80 | (cp & 63)); return 3; }
    o[0] = (char)(0xf0 | cp >> 18); o[1] = (char)(0x80 | ((cp >> 12) & 63)); o[2] = (char)(0x80 | ((cp >> 6) & 63)); o[3] = (char)(0x80 | (cp & 63)); return 4;
}

static const struct { const char *n; u16 cp; } entities[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", 0xa0 },
    { "copy", 0xa9 }, { "reg", 0xae }, { "trade", 0x2122 }, { "mdash", 0x2014 }, { "ndash", 0x2013 },
    { "hellip", 0x2026 }, { "lsquo", 0x2018 }, { "rsquo", 0x2019 }, { "ldquo", 0x201c }, { "rdquo", 0x201d },
    { "laquo", 0xab }, { "raquo", 0xbb }, { "middot", 0xb7 }, { "bull", 0x2022 }, { "deg", 0xb0 },
    { "times", 0xd7 }, { "euro", 0x20ac }, { "pound", 0xa3 }, { "yen", 0xa5 }, { "cent", 0xa2 }, { "sect", 0xa7 },
    { "para", 0xb6 }, { "larr", 0x2190 }, { "rarr", 0x2192 }, { "uarr", 0x2191 }, { "darr", 0x2193 },
    { "eacute", 0xe9 }, { "egrave", 0xe8 }, { "aacute", 0xe1 }, { "agrave", 0xe0 }, { "ouml", 0xf6 }, { "uuml", 0xfc },
    { "auml", 0xe4 }, { "szlig", 0xdf }, { "ccedil", 0xe7 }, { "ntilde", 0xf1 }, { "iacute", 0xed }, { "oacute", 0xf3 },
    { "uacute", 0xfa }, { "shy", 0 }, { "zwj", 0 }, { "zwnj", 0 }, { "thinsp", ' ' }, { "ensp", ' ' }, { "emsp", ' ' },
    { "minus", 0x2212 }, { "prime", 0x2032 }, { "dagger", 0x2020 }, { "frac12", 0xbd }, { "frac14", 0xbc }, { "plusmn", 0xb1 },
};

/* at s[0] == '&': decodes into out, returns bytes consumed (0 = not an entity) */
static usize entity(const u8 *s, usize len, char *out, int *olen) {
    usize i = 1;
    u32 cp = 0;
    if (i < len && s[i] == '#') {
        i++;
        int hex = i < len && (s[i] == 'x' || s[i] == 'X');
        if (hex) i++;
        usize start = i;
        while (i < len && i - start < 8) {
            int c = s[i], v = c >= '0' && c <= '9' ? c - '0' : hex && lc(c) >= 'a' && lc(c) <= 'f' ? lc(c) - 'a' + 10 : -1;
            if (v < 0) break;
            cp = cp * (hex ? 16 : 10) + (u32)v;
            i++;
        }
        if (i == start) return 0;
        if (i < len && s[i] == ';') i++;
        if (cp == 0 || cp > 0x10ffff || (cp >= 0xd800 && cp < 0xe000)) cp = 0xfffd;
        if (cp >= 0x80 && cp < 0xa0) {                     /* Windows-1252 quotes in numeric references */
            static const u16 w[32] = { 0x20ac, 0, 0x201a, 0x192, 0x201e, 0x2026, 0x2020, 0x2021, 0x2c6, 0x2030, 0x160, 0x2039, 0x152, 0, 0x17d, 0,
                                       0, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x2dc, 0x2122, 0x161, 0x203a, 0x153, 0, 0x17e, 0x178 };
            cp = w[cp - 0x80] ? w[cp - 0x80] : '?';
        }
        *olen = put_utf8(out, cp);
        return i;
    }
    char name[12];
    usize n = 0;
    while (i < len && n < sizeof name - 1 && ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9'))) name[n++] = (char)s[i++];
    name[n] = 0;
    if (!n) return 0;
    for (usize k = 0; k < sizeof entities / sizeof entities[0]; k++) {
        const char *a = entities[k].n, *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a || *b) continue;
        if (i < len && s[i] == ';') i++;
        *olen = entities[k].cp ? put_utf8(out, entities[k].cp) : 0;
        return i;
    }
    return 0;
}

/* ---- parser state ------------------------------------------------------------------ */
typedef struct {
    hdoc_t *d;
    int latin1;
    char word[512]; usize wlen; int wspace;   /* the word being collected; space before it */
    int pending_space, at_line_start;
    int bold, mono, pre, heading, link, small, indent;
    int ol[8], ol_n[8], lists;                 /* list stack: ordered?, counter */
    int form, in_title, in_button, button_field;
    char title[160]; usize tlen;
} ps_t;

static void add_node(ps_t *p, hnode_t n) {
    hdoc_t *d = p->d;
    d->nodes = grow(d->nodes, d->n_nodes, &d->cap_nodes, sizeof(hnode_t));
    d->nodes[d->n_nodes++] = n;
}

static u8 cur_flags(ps_t *p) { return (u8)((p->bold ? HF_BOLD : 0) | (p->mono ? HF_MONO : 0) | (p->pre ? HF_PRE : 0) | (p->small ? HF_SMALL : 0)); }

static void flush_word(ps_t *p) {
    if (!p->wlen) return;
    if (p->in_title) { p->wlen = 0; return; }
    hnode_t n = { N_TEXT, cur_flags(p), (u8)p->heading, (u8)MIN(p->indent, 12), (u8)p->wspace, p->link, -1, 0, 0 };
    n.off = text_put(p->d, p->word, p->wlen);
    n.len = (u32)p->wlen;
    add_node(p, n);
    p->wlen = 0;
    p->at_line_start = 0;
}

/* the line holds only a list bullet: blocks inside the <li> continue after it */
static int after_bullet(hdoc_t *d) { return d->n_nodes && d->nodes[d->n_nodes - 1].kind == N_TEXT && (d->nodes[d->n_nodes - 1].flags & HF_BULLET); }

static void brk(ps_t *p) {
    flush_word(p);
    hdoc_t *d = p->d;
    if (!d->n_nodes) { p->pending_space = 0; p->at_line_start = 1; return; }   /* nothing above it */
    if (after_bullet(d)) { p->pending_space = 0; return; }
    if (d->nodes[d->n_nodes - 1].kind != N_TEXT && d->nodes[d->n_nodes - 1].kind != N_FIELD) { p->pending_space = 0; p->at_line_start = 1; return; }
    hnode_t n = { N_BREAK, 0, 0, (u8)p->indent, 0, -1, -1, 0, 0 };
    add_node(p, n);
    p->pending_space = 0;
    p->at_line_start = 1;
}

static void hard_break(ps_t *p) {                 /* <br>: always a new line, even an empty one */
    flush_word(p);
    hnode_t n = { N_BREAK, 1, 0, (u8)p->indent, 0, -1, -1, 0, 0 };
    add_node(p, n);
    p->pending_space = 0;
    p->at_line_start = 1;
}

static void gap(ps_t *p, int size) {
    flush_word(p);
    hdoc_t *d = p->d;
    if (!d->n_nodes || after_bullet(d)) return;   /* nothing above it, or right after a bullet */
    hnode_t *last = &d->nodes[d->n_nodes - 1];
    if (last->kind == N_GAP) { if (last->space < size) last->space = (u8)size; return; }
    if (last->kind == N_BREAK && !last->flags) { last->kind = N_GAP; last->space = (u8)size; return; }
    hnode_t n = { N_GAP, 0, 0, 0, (u8)size, -1, -1, 0, 0 };
    add_node(p, n);
    p->pending_space = 0;
    p->at_line_start = 1;
}

static void put_char(ps_t *p, const char *c, int n) {
    if (p->in_title) {
        if (p->tlen + (usize)n < sizeof p->title - 1) { memcpy(p->title + p->tlen, c, (usize)n); p->tlen += (usize)n; }
        return;
    }
    if (p->in_button) {                           /* <button>label</button> */
        hfield_t *f = &p->d->fields[p->button_field];
        usize l = strlen(f->value);
        if (l + (usize)n < sizeof f->value) { memcpy(f->value + l, c, (usize)n); f->value[l + (usize)n] = 0; }
        return;
    }
    if (p->wlen + (usize)n >= sizeof p->word) { flush_word(p); p->pending_space = 0; }   /* an endless "word": split it */
    if (!p->wlen) { p->wspace = p->pending_space && !p->at_line_start; p->pending_space = 0; }
    memcpy(p->word + p->wlen, c, (usize)n);
    p->wlen += (usize)n;
}

static void text_space(ps_t *p) {
    if (p->in_title) { if (p->tlen && p->title[p->tlen - 1] != ' ' && p->tlen < sizeof p->title - 1) p->title[p->tlen++] = ' '; return; }
    if (p->in_button) {
        hfield_t *f = &p->d->fields[p->button_field];
        usize l = strlen(f->value);
        if (l && f->value[l - 1] != ' ' && l + 1 < sizeof f->value) { f->value[l] = ' '; f->value[l + 1] = 0; }
        return;
    }
    flush_word(p);
    p->pending_space = 1;
}

static void text_bytes(ps_t *p, const u8 *s, usize len) {
    for (usize i = 0; i < len;) {
        u8 c = s[i];
        if (p->pre && c == '\n') { hard_break(p); i++; continue; }
        if (p->pre && c == '\t') { for (int k = 0; k < 4; k++) put_char(p, " ", 1); i++; continue; }
        if (!p->pre && is_space(c)) { text_space(p); i++; continue; }
        if (c == '&') {
            char out[4];
            int ol = 0;
            usize used = entity(s + i, len - i, out, &ol);
            if (used) {
                if (ol == 2 && (u8)out[0] == 0xc2 && (u8)out[1] == 0xa0 && !p->pre) put_char(p, " ", 1);   /* nbsp: keeps words together */
                else if (ol) put_char(p, out, ol);
                i += used;
                continue;
            }
        }
        if (c >= 0x80 && p->latin1) { char o[2]; put_char(p, o, put_utf8(o, c)); i++; continue; }
        put_char(p, (const char *)s + i, 1);
        i++;
    }
}

/* ---- tags ----------------------------------------------------------------------------- */
typedef struct { char name[16]; int close, self; char href[1024], alt[256], type[24], nm[64], value[256], action[1024], method[8]; } tag_t;

static void get_attr(const char *an, const char *av, tag_t *t) {
    if (streq_ci(an, "href")) strlcpy(t->href, av, sizeof t->href);
    else if (streq_ci(an, "alt")) strlcpy(t->alt, av, sizeof t->alt);
    else if (streq_ci(an, "type")) strlcpy(t->type, av, sizeof t->type);
    else if (streq_ci(an, "name")) strlcpy(t->nm, av, sizeof t->nm);
    else if (streq_ci(an, "value")) strlcpy(t->value, av, sizeof t->value);
    else if (streq_ci(an, "action")) strlcpy(t->action, av, sizeof t->action);
    else if (streq_ci(an, "method")) strlcpy(t->method, av, sizeof t->method);
}

/* parses a tag at s[0] == '<'; returns its length, 0 if it is not a tag */
static usize parse_tag(const u8 *s, usize len, tag_t *t) {
    memset(t, 0, sizeof *t);
    usize i = 1;
    if (i < len && s[i] == '/') { t->close = 1; i++; }
    usize n = 0;
    if (i >= len || !((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z'))) return 0;
    while (i < len && !is_space(s[i]) && s[i] != '>' && s[i] != '/') { if (n < sizeof t->name - 1) t->name[n++] = (char)lc(s[i]); i++; }
    t->name[n] = 0;
    while (i < len && s[i] != '>') {
        while (i < len && (is_space(s[i]) || s[i] == '/')) { if (s[i] == '/') t->self = 1; i++; }
        if (i >= len || s[i] == '>') break;
        char an[24], av[1024];
        usize a = 0, v = 0;
        while (i < len && !is_space(s[i]) && s[i] != '=' && s[i] != '>' && s[i] != '/') { if (a < sizeof an - 1) an[a++] = (char)s[i]; i++; }
        an[a] = 0;
        while (i < len && is_space(s[i])) i++;
        if (i < len && s[i] == '=') {
            i++;
            while (i < len && is_space(s[i])) i++;
            u8 q = i < len && (s[i] == '"' || s[i] == '\'') ? s[i++] : 0;
            while (i < len && (q ? s[i] != q : !is_space(s[i]) && s[i] != '>')) {
                if (s[i] == '&') {
                    char out[4]; int ol = 0;
                    usize used = entity(s + i, len - i, out, &ol);
                    if (used) { for (int k = 0; k < ol && v < sizeof av - 1; k++) av[v++] = out[k]; i += used; continue; }
                }
                if (v < sizeof av - 1) av[v++] = (char)s[i];
                i++;
            }
            if (q && i < len) i++;
        }
        av[v] = 0;
        if (a) get_attr(an, av, t);
        else if (i < len && s[i] != '>') i++;
    }
    return i < len ? i + 1 : len;
}

static int is(const tag_t *t, const char *n) { return !strcmp(t->name, n); }

static int add_link(ps_t *p, const char *href) {
    hdoc_t *d = p->d;
    if (!href[0] || !strncmp(href, "javascript:", 11) || !strncmp(href, "mailto:", 7)) return -1;
    char abs[2300];
#ifndef HTML_HOST_TEST
    url_resolve(d->base, href, abs, sizeof abs);
#else
    strlcpy(abs, href, sizeof abs);
#endif
    d->links = grow(d->links, d->n_links, &d->cap_links, sizeof(char *));
    usize l = strlen(abs);
    char *c = kalloc(l + 1);
    memcpy(c, abs, l + 1);
    d->links[d->n_links] = c;
    return d->n_links++;
}

static void add_field(ps_t *p, int kind, const char *name, const char *value) {
    hdoc_t *d = p->d;
    d->fields = grow(d->fields, d->n_fields, &d->cap_fields, sizeof(hfield_t));
    hfield_t *f = &d->fields[d->n_fields];
    memset(f, 0, sizeof *f);
    f->form = p->form;
    f->kind = kind;
    strlcpy(f->name, name, sizeof f->name);
    strlcpy(f->value, value, sizeof f->value);
    if (kind != FIELD_HIDDEN) {
        flush_word(p);
        hnode_t n = { N_FIELD, 0, 0, (u8)p->indent, (u8)(p->pending_space && !p->at_line_start), -1, d->n_fields, 0, 0 };
        add_node(p, n);
        p->pending_space = 1;
        p->at_line_start = 0;
    }
    d->n_fields++;
}

static void tag(ps_t *p, const tag_t *t) {
    const char *nm = t->name;
    int open = !t->close;
    /* blocks */
    if (is(t, "p") || is(t, "div") || is(t, "section") || is(t, "article") || is(t, "header") || is(t, "footer") ||
        is(t, "nav") || is(t, "main") || is(t, "aside") || is(t, "figure") || is(t, "figcaption") || is(t, "address") ||
        is(t, "center") || is(t, "details") || is(t, "summary") || is(t, "fieldset") || is(t, "legend") || is(t, "caption") ||
        is(t, "dl") || is(t, "table")) {
        if (is(t, "p") || is(t, "table") || is(t, "dl") || is(t, "figure")) gap(p, 1); else brk(p);
        return;
    }
    if (nm[0] == 'h' && nm[1] >= '1' && nm[1] <= '6' && !nm[2]) {
        gap(p, nm[1] <= '2' ? 2 : 1);
        p->heading = open ? nm[1] - '0' : 0;
        return;
    }
    if (is(t, "br")) { hard_break(p); return; }
    if (is(t, "hr")) { brk(p); hnode_t n = { N_HR, 0, 0, 0, 0, -1, -1, 0, 0 }; add_node(p, n); p->at_line_start = 1; return; }
    if (is(t, "tr")) { brk(p); return; }
    if (is(t, "td") || is(t, "th")) { if (open) { text_space(p); p->bold += is(t, "th"); } else { p->bold -= p->bold > 0 && is(t, "th"); text_space(p); } return; }
    if (is(t, "dt")) { brk(p); p->bold = open; return; }
    if (is(t, "dd")) { brk(p); p->indent += open ? 1 : (p->indent > 0 ? -1 : 0); return; }
    if (is(t, "blockquote")) { gap(p, 1); p->indent += open ? 1 : (p->indent > 0 ? -1 : 0); return; }
    if (is(t, "pre") || is(t, "listing") || is(t, "xmp")) { gap(p, 1); p->pre = open; p->mono = open; return; }
    if (is(t, "ul") || is(t, "ol") || is(t, "menu")) {
        if (open) { if (p->lists < 8) { p->ol[p->lists] = is(t, "ol"); p->ol_n[p->lists] = 0; } p->lists++; p->indent++; if (p->lists == 1) gap(p, 1); else brk(p); }
        else { if (p->lists) p->lists--; if (p->indent) p->indent--; if (!p->lists) gap(p, 1); else brk(p); }
        return;
    }
    if (is(t, "li")) {
        brk(p);
        if (!open) return;
        int lv = MIN(p->lists, 8) - 1;
        char m[16];
        if (lv >= 0 && p->ol[lv]) fmt(m, sizeof m, "%d.", ++p->ol_n[lv]);
        else strlcpy(m, "\xe2\x80\xa2", sizeof m);                  /* bullet */
        hnode_t n = { N_TEXT, (u8)(HF_BULLET | cur_flags(p)), 0, (u8)MIN(p->indent, 12), 0, -1, -1, 0, 0 };
        n.off = text_put(p->d, m, strlen(m));
        n.len = (u32)strlen(m);
        add_node(p, n);
        p->at_line_start = 0;
        p->pending_space = 0;
        return;
    }
    /* inline */
    if (is(t, "b") || is(t, "strong")) { flush_word(p); p->bold += open ? 1 : (p->bold > 0 ? -1 : 0); return; }
    if (is(t, "code") || is(t, "tt") || is(t, "kbd") || is(t, "samp")) { flush_word(p); p->mono += open ? 1 : (p->mono > 0 ? -1 : 0); return; }
    if (is(t, "small") || is(t, "sup") || is(t, "sub")) { flush_word(p); p->small += open ? 1 : (p->small > 0 ? -1 : 0); return; }
    if (is(t, "a")) { flush_word(p); p->link = open && t->href[0] ? add_link(p, t->href) : -1; return; }
    if (is(t, "img") && t->alt[0]) {
        flush_word(p);
        char m[270];
        fmt(m, sizeof m, "[%s]", t->alt);
        hnode_t n = { N_TEXT, (u8)(HF_ALT | cur_flags(p)), (u8)p->heading, (u8)MIN(p->indent, 12), (u8)(p->pending_space && !p->at_line_start), p->link, -1, 0, 0 };
        n.off = text_put(p->d, m, strlen(m));
        n.len = (u32)strlen(m);
        add_node(p, n);
        p->pending_space = 0;
        p->at_line_start = 0;
        return;
    }
    if (is(t, "form")) {
        brk(p);
        if (!open) { p->form = -1; return; }
        hdoc_t *d = p->d;
        d->forms = grow(d->forms, d->n_forms, &d->cap_forms, sizeof(hform_t));
        hform_t *f = &d->forms[d->n_forms];
#ifndef HTML_HOST_TEST
        url_resolve(d->base, t->action[0] ? t->action : d->base, f->action, sizeof f->action);
#else
        strlcpy(f->action, t->action, sizeof f->action);
#endif
        f->post = streq_ci(t->method, "post");
        p->form = d->n_forms++;
        return;
    }
    if (is(t, "input") && open) {
        const char *ty = t->type;
        if (!ty[0] || streq_ci(ty, "text") || streq_ci(ty, "search") || streq_ci(ty, "email") || streq_ci(ty, "url") || streq_ci(ty, "tel") || streq_ci(ty, "number"))
            add_field(p, FIELD_TEXT, t->nm, t->value);
        else if (streq_ci(ty, "hidden")) add_field(p, FIELD_HIDDEN, t->nm, t->value);
        else if (streq_ci(ty, "submit")) add_field(p, FIELD_SUBMIT, t->nm, t->value[0] ? t->value : "Submit");
        return;
    }
    if (is(t, "button")) {
        if (open && p->form >= 0 && (!t->type[0] || streq_ci(t->type, "submit"))) {
            add_field(p, FIELD_SUBMIT, t->nm, "");
            p->in_button = 1;
            p->button_field = p->d->n_fields - 1;
        } else if (!open && p->in_button) {
            hfield_t *f = &p->d->fields[p->button_field];
            usize l = strlen(f->value);
            while (l && f->value[l - 1] == ' ') f->value[--l] = 0;
            if (!f->value[0]) strlcpy(f->value, "Submit", sizeof f->value);
            p->in_button = 0;
        }
        return;
    }
    if (is(t, "title")) { flush_word(p); p->in_title = open; if (open) p->tlen = 0; else { p->title[p->tlen] = 0; strlcpy(p->d->title, p->title, sizeof p->d->title); } return; }
}

/* the raw-text elements whose content is skipped (or, for <title>, kept) */
static const char *skip_tags[] = { "script", "style", "svg", "template", "math", "select", "textarea", "iframe", "object", "noembed", NULL };

int html_is_utf8(const u8 *s, usize len) {
    for (usize i = 0; i < len;) {
        u8 c = s[i];
        int n = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : -1;
        if (n < 0) return 0;
        for (int k = 1; k <= n; k++) {
            if (i + (usize)k >= len) return 1;            /* cut off at the end: fine */
            if ((s[i + (usize)k] & 0xc0) != 0x80) return 0;
        }
        i += (usize)n + 1;
    }
    return 1;
}

void html_parse(hdoc_t *d, const char *url, const u8 *s, usize len, int plain_text, int latin1) {
    memset(d, 0, sizeof *d);
    strlcpy(d->base, url, sizeof d->base);
    ps_t *p = kalloc(sizeof *p);
    p->d = d;
    p->link = -1;
    p->form = -1;
    p->at_line_start = 1;
    p->latin1 = latin1;
    if (plain_text) { p->pre = p->mono = 1; text_bytes(p, s, len); flush_word(p); kfree(p); return; }
    usize i = 0, text_start = 0;
    while (i < len) {
        if (s[i] != '<') { i++; continue; }
        if (i > text_start) text_bytes(p, s + text_start, i - text_start);
        if (i + 3 < len && s[i + 1] == '!' && s[i + 2] == '-' && s[i + 3] == '-') {         /* comment */
            usize j = i + 4;
            while (j + 2 < len && !(s[j] == '-' && s[j + 1] == '-' && s[j + 2] == '>')) j++;
            i = text_start = MIN(j + 3, len);
            continue;
        }
        if (i + 1 < len && (s[i + 1] == '!' || s[i + 1] == '?')) {                            /* doctype, CDATA, PI */
            usize j = i;
            while (j < len && s[j] != '>') j++;
            i = text_start = MIN(j + 1, len);
            continue;
        }
        tag_t t;
        usize tl = parse_tag(s + i, len - i, &t);
        if (!tl) { text_bytes(p, s + i, 1); i++; text_start = i; continue; }            /* a lone '<' */
        i += tl;
        text_start = i;
        int skipped = 0;
        if (!t.close && !t.self)
            for (int k = 0; skip_tags[k]; k++) {
                if (strcmp(t.name, skip_tags[k])) continue;
                /* jump to the matching close tag */
                usize nl = strlen(t.name);
                usize j = i;
                while (j + nl + 2 < len) {
                    if (s[j] == '<' && s[j + 1] == '/') {
                        usize m = 0;
                        while (m < nl && lc(s[j + 2 + m]) == t.name[m]) m++;
                        if (m == nl) break;
                    }
                    j++;
                }
                while (j < len && s[j] != '>') j++;
                i = text_start = MIN(j + 1, len);
                skipped = 1;
                break;
            }
        if (!skipped) tag(p, &t);
    }
    if (len > text_start) text_bytes(p, s + text_start, len - text_start);
    flush_word(p);
    while (d->n_nodes && (d->nodes[d->n_nodes - 1].kind == N_BREAK || d->nodes[d->n_nodes - 1].kind == N_GAP)) d->n_nodes--;
    if (!d->title[0] && p->in_title) { p->title[p->tlen] = 0; strlcpy(d->title, p->title, sizeof d->title); }
    kfree(p);
}

void html_free(hdoc_t *d) {
    for (int i = 0; i < d->n_links; i++) kfree(d->links[i]);
    if (d->links) kfree(d->links);
    if (d->text) kfree(d->text);
    if (d->nodes) kfree(d->nodes);
    if (d->fields) kfree(d->fields);
    if (d->forms) kfree(d->forms);
    memset(d, 0, sizeof *d);
}
