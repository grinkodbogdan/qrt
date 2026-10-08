/*
 * fdt.c - read the flattened device tree (DTB) the boot loader passes in x0: memory,
 * the console, the display and the devices.  Read-only, no allocation; nodes are named
 * by their offset in the structure block.
 */
#include "arm.h"

#define FDT_MAGIC      0xd00dfeed
#define FDT_BEGIN_NODE 1
#define FDT_END_NODE   2
#define FDT_PROP       3
#define FDT_NOP        4
#define FDT_END        9

static const u8 *blob, *structs, *strings;
static u32 struct_size, total;
static u32 root_ac = 2, root_sc = 1;

u32 fdt_u32(const void *p) { const u8 *b = p; return (u32)b[0] << 24 | (u32)b[1] << 16 | (u32)b[2] << 8 | b[3]; }
u64 fdt_u64(const void *p) { return (u64)fdt_u32(p) << 32 | fdt_u32((const u8 *)p + 4); }
usize fdt_size(void) { return total; }

static u32 tok(int off) { return fdt_u32(structs + off); }
static int align4(int v) { return (v + 3) & ~3; }

/* the offset after this node's name; or after a property */
static int skip_name(int off) { return align4(off + 4 + (int)strlen((const char *)structs + off + 4) + 1); }
static int skip_prop(int off) { u32 len = fdt_u32(structs + off + 4); return align4(off + 12 + (int)len); }

int fdt_init(const void *b) {
    if (!b || fdt_u32(b) != FDT_MAGIC) return 0;
    blob = b;
    total = fdt_u32(blob + 4);
    structs = blob + fdt_u32(blob + 8);
    strings = blob + fdt_u32(blob + 12);
    struct_size = fdt_u32(blob + 36);
    int len;
    const void *p = fdt_prop(0, "#address-cells", &len);
    if (p) root_ac = fdt_u32(p);
    p = fdt_prop(0, "#size-cells", &len);
    if (p) root_sc = fdt_u32(p);
    return 1;
}

/* the end of the node starting at off (its END_NODE token's next offset) */
static int node_end(int off) {
    int depth = 0;
    for (;;) {
        u32 t = tok(off);
        if (t == FDT_BEGIN_NODE) { depth++; off = skip_name(off); }
        else if (t == FDT_END_NODE) { off += 4; if (--depth == 0) return off; }
        else if (t == FDT_PROP) off = skip_prop(off);
        else if (t == FDT_NOP) off += 4;
        else return off;
    }
}

const void *fdt_prop(int node, const char *name, int *len) {
    if (!blob || tok(node) != FDT_BEGIN_NODE) return NULL;
    int off = skip_name(node);
    for (;;) {
        u32 t = tok(off);
        if (t == FDT_NOP) { off += 4; continue; }
        if (t != FDT_PROP) return NULL;                 /* properties come before children */
        u32 l = fdt_u32(structs + off + 4), nameoff = fdt_u32(structs + off + 8);
        if (!strcmp((const char *)strings + nameoff, name)) {
            if (len) *len = (int)l;
            return structs + off + 12;
        }
        off = skip_prop(off);
    }
}

/* a child of node called name (with or without its @unit) */
static int child(int node, const char *name, usize nl) {
    int off = skip_name(node);
    while (tok(off) == FDT_PROP || tok(off) == FDT_NOP) off = tok(off) == FDT_NOP ? off + 4 : skip_prop(off);
    while (tok(off) == FDT_BEGIN_NODE) {
        const char *n = (const char *)structs + off + 4;
        if (!strncmp(n, name, nl) && (n[nl] == 0 || n[nl] == '@')) return off;
        off = node_end(off);
        while (tok(off) == FDT_NOP) off += 4;
    }
    return -1;
}

int fdt_node(const char *path) {
    if (!blob) return -1;
    int node = 0;
    while (*path == '/') path++;
    while (*path) {
        const char *e = path;
        while (*e && *e != '/') e++;
        node = child(node, path, (usize)(e - path));
        if (node < 0) return -1;
        path = e;
        while (*path == '/') path++;
    }
    return node;
}

static int has_compat(int node, const char *compat) {
    int len;
    const char *c = fdt_prop(node, "compatible", &len);
    for (int i = 0; c && i < len; i += (int)strlen(c + i) + 1)
        if (!strcmp(c + i, compat)) return 1;
    return 0;
}

int fdt_find_compatible(int from, const char *compat) {
    if (!blob) return -1;
    int off = from < 0 ? 0 : skip_name(from);
    for (; off < (int)struct_size; ) {
        u32 t = tok(off);
        if (t == FDT_BEGIN_NODE) {
            if (off != from && has_compat(off, compat)) return off;
            off = skip_name(off);
        } else if (t == FDT_PROP) off = skip_prop(off);
        else if (t == FDT_END_NODE || t == FDT_NOP) off += 4;
        else break;
    }
    return -1;
}

/* the parent of a node: walk from the root keeping the path */
static int parent_of(int node) {
    int stack[32], depth = 0, off = 0;
    while (off < (int)struct_size) {
        u32 t = tok(off);
        if (t == FDT_BEGIN_NODE) {
            if (off == node) return depth ? stack[depth - 1] : -1;
            if (depth < 32) stack[depth] = off;
            depth++;
            off = skip_name(off);
        } else if (t == FDT_END_NODE) { depth--; off += 4; }
        else if (t == FDT_PROP) off = skip_prop(off);
        else if (t == FDT_NOP) off += 4;
        else break;
    }
    return -1;
}

/* the i-th (address, size) of a node's reg, in its parent's cell sizes; buses QRT reads
 * map 1:1 (QEMU virt, MSM8953's /soc@0), so no ranges translation */
int fdt_reg(int node, int i, u64 *addr, u64 *size) {
    int len, par = parent_of(node);
    u32 ac = root_ac, sc = root_sc;
    if (par >= 0) {
        const void *p = fdt_prop(par, "#address-cells", NULL);
        if (p) ac = fdt_u32(p);
        p = fdt_prop(par, "#size-cells", NULL);
        if (p) sc = fdt_u32(p);
    }
    const u8 *r = fdt_prop(node, "reg", &len);
    int ent = (int)(ac + sc) * 4;
    if (!r || ent <= 0 || len < ent * (i + 1)) return 0;
    r += ent * i;
    *addr = ac == 2 ? fdt_u64(r) : fdt_u32(r);
    *size = sc == 2 ? fdt_u64(r + ac * 4) : sc == 1 ? fdt_u32(r + ac * 4) : 0;
    return 1;
}

const char *fdt_name(int node) { return (const char *)structs + node + 4; }

/* call fn for each child of node; stops when fn returns non-zero */
int fdt_children(int node, int (*fn)(int child, void *arg), void *arg) {
    if (!blob) return 0;
    int off = skip_name(node);
    while (tok(off) == FDT_PROP || tok(off) == FDT_NOP) off = tok(off) == FDT_NOP ? off + 4 : skip_prop(off);
    while (tok(off) == FDT_BEGIN_NODE) {
        if (fn(off, arg)) return 1;
        off = node_end(off);
        while (tok(off) == FDT_NOP) off += 4;
    }
    return 0;
}
