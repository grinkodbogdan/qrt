/* vfs.c - RAM file system seeded from the boot volume, plus /proc and /etc. */
#include "vfs.h"
#include "smp.h"
#include "../net/net.h"
#if defined(__x86_64__)
#include "../arch/x64/mm.h"
#endif

static vnode_t *root;
static u64 total_bytes;
#if defined(__x86_64__)                 /* Ladybird is one 150 MB program */
#define MAX_FILE  (256ull << 20)
#define MAX_TOTAL (768ull << 20)
#else
#define MAX_FILE  (48ull << 20)
#define MAX_TOTAL (160ull << 20)
#endif

vnode_t *vfs_root(void) { return root; }
u64 vfs_total_bytes(void) { return total_bytes; }

static vnode_t *new_node(vnode_t *parent, const char *name, int dir) {
    vnode_t *n = kalloc(sizeof *n);
    strlcpy(n->name, name, sizeof n->name);
    n->dir = dir;
    n->parent = parent ? parent : n;
    if (parent) {                         /* append, keeping directory order stable */
        vnode_t **pp = &parent->child;
        while (*pp) pp = &(*pp)->sibling;
        *pp = n;
    }
    return n;
}

static vnode_t *find_child(vnode_t *d, const char *name, usize len) {
    for (vnode_t *c = d->child; c; c = c->sibling)
        if (strlen(c->name) == len && !memcmp(c->name, name, len)) return c;
    return NULL;
}

/* ---- remote mounts ---- */
i64 (*vfs_remote_call)(void *mnt, int op, const char *path, const char *path2, u64 off, void *buf, u64 len);

static void rpath(vnode_t *n, char *out, usize cap) {      /* path inside its mount */
    char tmp[512] = "";
    for (; n && !n->mroot; n = n->parent) {
        char seg[600];
        fmt(seg, sizeof seg, "/%s%s", n->name, tmp);
        strlcpy(tmp, seg, sizeof tmp);
    }
    strlcpy(out, tmp[0] ? tmp : "/", cap);
}
static i64 rcall(vnode_t *n, int op, const char *path2, u64 off, void *buf, u64 len) {
    char p[512];
    if (!vfs_remote_call) return -5;
    rpath(n, p, sizeof p);
    return vfs_remote_call(n->mnt, op, p, path2, off, buf, len);
}
static void detach(vnode_t *n);
static vnode_t *walk(const char *path, int create, int dir);
#define LIST_MAX (256u << 10)
static void remote_refresh(vnode_t *d) {
    u8 *b = kalloc(LIST_MAX);
    i64 got = rcall(d, VR_LIST, NULL, 0, b, LIST_MAX);
    d->stamp = k_now_ms();
    if (got < 0) { kfree(b); return; }
    for (vnode_t *c = d->child; c; c = c->sibling) c->seen = 0;
    for (i64 o = 0; o + 12 <= got; ) {
        u8 dir = b[o], len = b[o + 11];
        u16 mode = (u16)(b[o + 1] | b[o + 2] << 8);
        u64 size = 0;
        memcpy(&size, b + o + 3, 8);
        if (o + 12 + len > got) break;
        char name[64];
        usize l = MIN((usize)len, sizeof name - 1);
        memcpy(name, b + o + 12, l); name[l] = 0;
        o += 12 + len;
        vnode_t *c = find_child(d, name, l);
        if (c && c->dir != dir) { detach(c); c = NULL; }
        if (!c) { c = new_node(d, name, dir); c->mnt = d->mnt; }
        if (!dir) c->size = size;
        c->mode = mode;
        c->seen = 1;
    }
    for (vnode_t *c = d->child, *nx; c; c = nx) { nx = c->sibling; if (!c->seen) detach(c); }
    kfree(b);
}
static void remote_fresh(vnode_t *d) { if (d && d->mnt && d->dir && k_now_ms() - d->stamp > 1000) remote_refresh(d); }

vnode_t *vfs_mount_remote(const char *path, void *mnt) {
    vnode_t *n = walk(path, 1, 1);
    if (!n || n->child || n->mnt) return NULL;
    n->mnt = mnt;
    n->mroot = 1;
    n->stamp = 0;
    return n;
}
void vfs_unmount_remote(vnode_t *m) {
    if (!m || !m->mroot) return;
    m->child = NULL;
    detach(m);
}

/* Walk path from root; with create, make missing components (last one as 'dir'). */
static vnode_t *walk(const char *path, int create, int dir) {
    if (!root) root = new_node(NULL, "", 1);
    vnode_t *cur = root;
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != '/') p++;
        usize len = (usize)(p - s);
        if (len == 1 && s[0] == '.') continue;
        if (len == 2 && s[0] == '.' && s[1] == '.') { cur = cur->parent; continue; }
        if (!cur->dir) return NULL;
        vnode_t *c = find_child(cur, s, len);
        if (!c && cur->mnt) { remote_refresh(cur); c = find_child(cur, s, len); }
        if (!c) {
            if (!create) return NULL;
            char name[64];
            usize l = MIN(len, sizeof name - 1);
            memcpy(name, s, l);
            name[l] = 0;
            while (*p == '/') p++;
            int d = *p ? 1 : dir;
            if (cur->mnt) {                                 /* make it on the remote side first */
                char rp[512], full[600];
                rpath(cur, rp, sizeof rp);
                fmt(full, sizeof full, "%s/%s", strcmp(rp, "/") ? rp : "", name);
                if (!vfs_remote_call || vfs_remote_call(cur->mnt, d ? VR_MKDIR : VR_CREATE, full, NULL, 0, NULL, 0) < 0) return NULL;
            }
            c = new_node(cur, name, d);
            c->mnt = cur->mnt;
        }
        cur = c;
    }
    return cur;
}

vnode_t *vfs_lookup(const char *path) { return walk(path, 0, 0); }

static void detach(vnode_t *n) {
    vnode_t *d = n->parent;
    for (vnode_t **pp = &d->child; *pp; pp = &(*pp)->sibling)
        if (*pp == n) { *pp = n->sibling; n->sibling = NULL; return; }
}

/* Unlink: the name goes; an open file keeps its node (its memory is not reclaimed - a
 * small leak, in exchange for never pulling data from under an open descriptor). */
int vfs_unlink(const char *path, int dir) {
    vnode_t *n = walk(path, 0, 0);
    if (!n || n == root) return -2;                   /* ENOENT */
    if (dir && !n->dir) return -20;                   /* ENOTDIR */
    if (!dir && n->dir) return -21;                   /* EISDIR */
    if (n->mroot) return -16;                         /* EBUSY */
    if (n->mnt) {
        remote_fresh(n);
        i64 r = rcall(n, dir ? VR_RMDIR : VR_UNLINK, NULL, 0, NULL, 0);
        if (r < 0) return (int)r;
    } else if (dir && n->child) return -39;           /* ENOTEMPTY */
    detach(n);
    return 0;
}

int vfs_rename(const char *from, const char *to) {
    vnode_t *n = walk(from, 0, 0);
    if (!n || n == root) return -2;
    vnode_t *old = walk(to, 0, 0);
    if (old == n) return 0;
    if (old) {
        if (old->dir && !n->dir) return -21;
        if (!old->dir && n->dir) return -20;
        if (old->dir && old->child) return -39;
    }
    /* the new parent: everything up to the last '/' */
    char dpath[256];
    strlcpy(dpath, to, sizeof dpath);
    char *slash = NULL;
    for (char *c = dpath; *c; c++) if (*c == '/') slash = c;
    if (!slash) return -2;
    const char *base = slash + 1;
    if (!*base) return -22;
    char name[64];
    strlcpy(name, base, sizeof name);
    *slash = 0;
    vnode_t *d = walk(dpath[0] ? dpath : "/", 0, 0);
    if (!d || !d->dir) return -2;
    for (vnode_t *a = d; a && a != root; a = a->parent) if (a == n) return -22;   /* into itself */
    if (n->mroot || (old && old->mroot)) return -16;
    if (n->mnt || d->mnt) {
        if (n->mnt != d->mnt) return -18;                     /* EXDEV: copy instead */
        char rp[512], full[600];
        rpath(d, rp, sizeof rp);
        fmt(full, sizeof full, "%s/%s", strcmp(rp, "/") ? rp : "", name);
        i64 r = rcall(n, VR_RENAME, full, 0, NULL, 0);
        if (r < 0) return (int)r;
    }
    if (old) detach(old);
    detach(n);
    strlcpy(n->name, name, sizeof n->name);
    n->parent = d;
    vnode_t **pp = &d->child;
    while (*pp) pp = &(*pp)->sibling;
    *pp = n;
    return 0;
}
vnode_t *vfs_create(const char *path, int dir) { return walk(path, 1, dir); }

int vfs_children(vnode_t *d) { remote_fresh(d); int n = 0; for (vnode_t *c = d ? d->child : NULL; c; c = c->sibling) n++; return n; }
vnode_t *vfs_child_at(vnode_t *d, int i) {
    if (i == 0) remote_fresh(d);
    for (vnode_t *c = d ? d->child : NULL; c; c = c->sibling) if (i-- == 0) return c;
    return NULL;
}

static void gen_refresh(vnode_t *n) {
    if (!n->gen) return;
    if (!n->data) { n->cap = 8192; n->data = kalloc(n->cap); }
    n->size = (u64)n->gen((char *)n->data, (int)n->cap);
}

u64 vfs_size(vnode_t *n) { gen_refresh(n); return n->size; }

#define RCHUNK (256u << 10)
i64 vfs_read(vnode_t *n, u64 off, void *buf, u64 len) {
    if (n->dir) return -21;                          /* EISDIR */
    if (n->mnt) {                                    /* through a kernel buffer: the server is another process */
        u64 done = 0;
        u8 *b = kalloc(MIN(len, RCHUNK) ? MIN(len, RCHUNK) : 1);
        while (done < len) {
            u64 c = MIN(len - done, RCHUNK);
            i64 r = rcall(n, VR_READ, NULL, off + done, b, c);
            if (r < 0) { kfree(b); return done ? (i64)done : r; }
            memcpy((u8 *)buf + done, b, (usize)r);
            done += (u64)r;
            if ((u64)r < c) break;
        }
        kfree(b);
        if (off + done > n->size) n->size = off + done;
        return (i64)done;
    }
    if (off == 0) gen_refresh(n);
    if (off >= n->size) return 0;
    u64 m = MIN(len, n->size - off);
    memcpy(buf, n->data + off, m);
    return (i64)m;
}

i64 vfs_write(vnode_t *n, u64 off, const void *buf, u64 len) {
    if (n->dir || n->gen) return -1;
    if (n->mnt) {
        u64 done = 0;
        u8 *b = kalloc(MIN(len, RCHUNK) ? MIN(len, RCHUNK) : 1);
        while (done < len) {
            u64 c = MIN(len - done, RCHUNK);
            memcpy(b, (const u8 *)buf + done, (usize)c);
            i64 r = rcall(n, VR_WRITE, NULL, off + done, b, c);
            if (r <= 0) { kfree(b); return done ? (i64)done : (r ? r : -5); }
            done += (u64)r;
        }
        kfree(b);
        if (off + done > n->size) n->size = off + done;
        return (i64)done;
    }
    if (off + len > n->cap) {
        u64 cap = MAX(off + len, n->cap * 2);
        if (cap > MAX_FILE) return -27;              /* EFBIG */
        u8 *d = kalloc(cap);
        if (n->data) { memcpy(d, n->data, n->size); kfree(n->data); }
        n->data = d;
        n->cap = cap;
    }
    memcpy(n->data + off, buf, len);
    if (off + len > n->size) n->size = off + len;
    return (i64)len;
}

void vfs_truncate(vnode_t *n) {
    if (n->mnt && !n->dir) rcall(n, VR_TRUNC, NULL, 0, NULL, 0);
    n->size = 0;
}

void vfs_path(vnode_t *n, char *out, usize cap) {
    char tmp[256] = "";
    while (n && n != root) {
        char seg[320];
        fmt(seg, sizeof seg, "/%s%s", n->name, tmp);
        strlcpy(tmp, seg, sizeof tmp);
        n = n->parent;
    }
    strlcpy(out, tmp[0] ? tmp : "/", cap);
}

/* ---- synthetic files ------------------------------------------------------- */
static int gen_version(char *b, int cap) {
    return fmt(b, cap, "QRT version " QRT_VERSION " (Tessera kernel, %s) - Linux-compatible system call layer\n", QRT_ARCH);
}
static int gen_os_release(char *b, int cap) {
    return fmt(b, cap, "NAME=\"QRT\"\nVERSION=\"" QRT_VERSION "\"\nID=qrt\nPRETTY_NAME=\"QRT " QRT_VERSION "\"\n");
}
static int gen_cpuinfo(char *b, int cap) {
    int n = 0, cores = 1 + smp_workers();
    for (int i = 0; i < cores && n < cap; i++)
        n += fmt(b + n, (usize)(cap - n), "processor\t: %d\nvendor_id\t: GenuineIntel\nmodel name\t: %s\ncpu MHz\t\t: %llu\n\n",
                 i, k.cpu, k.tsc_per_ms / 1000);
    return n;
}
static int gen_uptime(char *b, int cap) {
    u64 ms = k_now_ms();
    return fmt(b, cap, "%llu.%02llu 0.00\n", ms / 1000, ms / 10 % 100);
}
static int gen_meminfo(char *b, int cap) {
    u64 total = k.ram_bytes, avail = total;
#if defined(__x86_64__)
    if (k.native) { total = pmm_total_bytes(); avail = pmm_free_bytes(); }
#endif
    return fmt(b, cap, "MemTotal:       %8llu kB\nMemFree:        %8llu kB\nMemAvailable:   %8llu kB\n",
               total >> 10, avail >> 10, avail >> 10);
}
static int gen_text(char *b, int cap, const char *t) { strlcpy(b, t, (usize)cap); return (int)strlen(b); }
/* name resolution for Linux programs: the DNS server DHCP handed out */
static int gen_resolv(char *b, int cap) {
    netif_t *n = net_primary();
    if (!n || !n->dns) return gen_text(b, cap, "# not connected\n");
    char ip[16];
    ip_to_str(n->dns, ip, sizeof ip);
    return fmt(b, (usize)cap, "nameserver %s\noptions timeout:2 attempts:2\n", ip);
}
static int gen_hosts(char *b, int cap) { return gen_text(b, cap, "127.0.0.1 localhost\n"); }
static int gen_nsswitch(char *b, int cap) { return gen_text(b, cap, "hosts: files dns\nnetworks: files\n"); }
static int gen_hostname(char *b, int cap) { return gen_text(b, cap, k.is_venue ? "venue\n" : "qrt\n"); }
static int gen_passwd(char *b, int cap) { return gen_text(b, cap, "root:x:0:0:root:/:/bin/sh\n"); }
static int gen_group(char *b, int cap) { return gen_text(b, cap, "root:x:0:\n"); }

static void synth(const char *path, int (*g)(char *, int)) { vnode_t *n = vfs_create(path, 0); n->gen = g; }

#if defined(__x86_64__)
int proc_status_text(char *b, int cap);   /* proc.c: /proc/self/status of the calling process */
#endif

static void add_synthetic(void) {
#if defined(__x86_64__)
    synth("/proc/self/status", proc_status_text);
#endif
    synth("/proc/version", gen_version);
    synth("/proc/cpuinfo", gen_cpuinfo);
    synth("/proc/meminfo", gen_meminfo);
    synth("/proc/uptime", gen_uptime);
    synth("/etc/os-release", gen_os_release);
    synth("/etc/hostname", gen_hostname);
    synth("/etc/passwd", gen_passwd);
    synth("/etc/group", gen_group);
    synth("/etc/resolv.conf", gen_resolv);
    synth("/etc/hosts", gen_hosts);
    synth("/etc/nsswitch.conf", gen_nsswitch);
    vfs_create("/tmp", 1);
    vfs_create("/dev", 1);
}

/* ---- boot volume copy ---------------------------------------------------------- */

static void copy_dir(EFI_FILE_PROTOCOL *d, vnode_t *into, int depth) {
    u8 *buf = kalloc(1024);
    for (;;) {
        UINTN sz = 1024;
        if (EFI_ERROR(d->Read(d, &sz, buf)) || !sz) break;
        EFI_FILE_INFO *fi = (EFI_FILE_INFO *)buf;
        if (fi->FileName[0] == '.' && (!fi->FileName[1] || (fi->FileName[1] == '.' && !fi->FileName[2]))) continue;
        char name[64];
        str16_to_utf8(name, sizeof name, fi->FileName);
        EFI_FILE_PROTOCOL *f;
        if (EFI_ERROR(d->Open(d, &f, fi->FileName, EFI_FILE_MODE_READ, 0))) continue;
        int is_dir = (fi->Attribute & EFI_FILE_DIRECTORY) != 0;
        vnode_t *n = new_node(into, name, is_dir);
        if (is_dir && depth < 8) copy_dir(f, n, depth + 1);
        else if (!is_dir && fi->FileSize <= MAX_FILE && total_bytes + fi->FileSize <= MAX_TOTAL) {
            n->cap = fi->FileSize ? fi->FileSize : 1;
            n->data = kalloc(n->cap);
            UINTN len = fi->FileSize;
            if (!EFI_ERROR(f->Read(f, &len, n->data))) n->size = len;
            total_bytes += n->size;
        }
        f->Close(f);
    }
    kfree(buf);
}

void vfs_load_boot_volume(void) {
    if (!root) root = new_node(NULL, "", 1);
    for (int i = 0; i < k.n_vol; i++) {
        if (!k.vol[i].boot) continue;
        EFI_FILE_PROTOCOL *r;
        if (EFI_ERROR(k.vol[i].fs->OpenVolume(k.vol[i].fs, &r))) break;
        copy_dir(r, root, 0);
        r->Close(r);
    }
    add_synthetic();
    char sz[24];
    fmt_bytes(sz, sizeof sz, total_bytes);
    klog("vfs: boot volume copied to RAM (%s)", sz);
}

/* ---- native: move the tree out of firmware pool memory ---------------------------- */
static vnode_t *deep_copy(vnode_t *n, vnode_t *parent) {
    vnode_t *c = kalloc(sizeof *c);
    *c = *n;
    c->parent = parent ? parent : c;
    c->child = c->sibling = NULL;
#if defined(__x86_64__)
    if (n->data && !n->gen && n->size >= (1u << 20)) {
        /* big files (programs): whole pages outside the heap, so that exec can map a
         * program's read-only segments straight from here, shared by every process
         * running it (load_elf).  They are never freed: kfree ignores them, and a
         * rewritten or deleted file just leaves its old pages to whoever maps them. */
        c->cap = (n->size + PAGE - 1) & ~(PAGE - 1);
        c->data = (u8 *)(usize)pmm_alloc_contig(c->cap / PAGE);
        memcpy(c->data, n->data, n->size);
    } else
#endif
    if (n->data && !n->gen) {
        c->data = kalloc(n->cap ? n->cap : 1);
        memcpy(c->data, n->data, n->size);
    } else c->data = NULL;
    vnode_t **pp = &c->child;
    for (vnode_t *ch = n->child; ch; ch = ch->sibling) { *pp = deep_copy(ch, c); pp = &(*pp)->sibling; }
    return c;
}

void vfs_relocate(void) { if (root) root = deep_copy(root, NULL); }
