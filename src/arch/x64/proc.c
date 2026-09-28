/*
 * proc.c - running Linux programs.
 *
 * Every process gets its own page tables: the first GiB of the address space
 * is the process's; everything above is the kernel's identity map,
 * supervisor-only.  So a classic non-PIE binary linked at 0x400000 loads at
 * its own addresses; static-PIE binaries are placed at 256 MiB.  The stack
 * sits at the top of the first GiB and grows on demand; brk and anonymous
 * mmap regions are also populated lazily, page by page, from page faults.
 *
 * Processes run in ring 3 on the boot core as ordinary scheduler threads and
 * enter the kernel through SYSCALL (linux.c).
 */
#include "proc.h"
#include "lsock.h"

extern void enter_user(u64 rip, u64 rsp);
extern void syscall_entry(void);
extern int (*user_fault_hook)(frame_t *f);
extern int (*page_fault_hook)(frame_t *f);

static int next_pid = 100;

static void term_put(term_t *t, const char *s, usize n) {
    if (t->len + n + 1 > t->cap) {
        usize cap = MAX(t->cap * 2, t->len + n + 4096);
        if (cap > (4u << 20)) {                          /* keep the last ~2 MB */
            usize drop = t->len / 2;
            memmove(t->buf, t->buf + drop, t->len - drop);
            t->len -= drop;
            cap = t->cap;
        }
        if (cap != t->cap) {
            char *nb = kalloc(cap);
            if (t->buf) { memcpy(nb, t->buf, t->len); kfree(t->buf); }
            t->buf = nb;
            t->cap = cap;
        }
    }
    n = MIN(n, t->cap - t->len - 1);
    memcpy(t->buf + t->len, s, n);
    t->len += n;
    t->buf[t->len] = 0;
}

/* The view is a plain line printer: tabs become spaces to the next 8-column
 * stop, CR and other control bytes are dropped, ANSI escape sequences are
 * swallowed (colour and cursor movement are not supported yet). */
void term_append(term_t *t, const char *s, usize n) {
    if (!t || !n) return;
    char chunk[512];
    usize m = 0;
    for (usize i = 0; i < n; i++) {
        u8 c = (u8)s[i];
        if (t->esc == 1) { t->esc = c == '[' ? 2 : 0; continue; }
        if (t->esc == 2) { if (c >= 0x40 && c <= 0x7e) t->esc = 0; continue; }
        if (c == 0x1b) { t->esc = 1; continue; }
        if (m + 8 > sizeof chunk) { term_put(t, chunk, m); m = 0; }
        if (c == '\n') { chunk[m++] = '\n'; t->col = 0; }
        else if (c == '\t') { do { chunk[m++] = ' '; t->col++; } while (t->col % 8); }
        else if (c >= 0x20 && c != 0x7f) { chunk[m++] = (char)c; if ((c & 0xc0) != 0x80) t->col++; }
    }
    if (m) term_put(t, chunk, m);
    t->serial++;
}

proc_t *proc_current(void) {
    thread_t *t = thread_current();
    return t ? t->proc : NULL;
}

int proc_user_supported(const char **why) {
    if (!k.native) { *why = "Linux programs need the native kernel"; return 0; }
    if (k.image_base < USER_TOP) { *why = "kernel image was loaded inside the user window"; return 0; }
    *why = NULL;
    return 1;
}

int proc_add_vma(proc_t *p, u64 start, u64 end) {
    for (int i = 0; i < p->nvma; i++)
        if (p->vma[i].end == start) { p->vma[i].end = end; return 0; }   /* extend (brk) */
    if (p->nvma == MAX_VMAS) return -1;
    p->vma[p->nvma++] = (vma_t){ start, end };
    return 0;
}

static int in_vma(proc_t *p, u64 a) {
    if (a >= USER_STACK_TOP - USER_STACK_SIZE && a < USER_STACK_TOP) return 1;
    for (int i = 0; i < p->nvma; i++) if (a >= p->vma[i].start && a < p->vma[i].end) return 1;
    return 0;
}

static int fault_in(proc_t *p, u64 a) {
    if (a >= USER_TOP || !in_vma(p, a)) return 0;
    if (as_translate(p->cr3, a)) return 1;
    as_map(p->cr3, a & ~(PAGE - 1), pmm_alloc(0), 1);
    return 1;
}

int proc_user_ok(proc_t *p, u64 addr, u64 len) {
    if (!p || addr >= USER_TOP || len > USER_TOP || addr + len > USER_TOP) return 0;
    for (u64 a = addr & ~(PAGE - 1); a < addr + len; a += PAGE)
        if (!as_translate(p->cr3, a) && !fault_in(p, a)) return 0;
    return 1;
}

/* ---- faults ------------------------------------------------------------------ */
static int on_page_fault(frame_t *f) {
    proc_t *p = proc_current();
    return p && f->vector == 14 && fault_in(p, read_cr2());
}

static int on_user_fault(frame_t *f) {
    proc_t *p = proc_current();
    if (!p) return 0;
    char msg[160];
    fmt(msg, sizeof msg, "\n[%s: %s at %llx, address %llx - killed]\n", p->name,
        f->vector == 14 ? "segmentation fault" : f->vector == 6 ? "illegal instruction" :
        f->vector == 13 ? "general protection fault" : "fault", f->rip, read_cr2());
    term_append(p->term, msg, strlen(msg));
    proc_exit(f->vector == 14 ? 139 : 132);
    return 1;
}

void proc_init(void) {
    page_fault_hook = on_page_fault;
    user_fault_hook = on_user_fault;
    /* SYSCALL/SYSRET: kernel CS 0x08 (SS 0x10); SYSRET derives user CS/SS from 0x18 */
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1);
    wrmsr(MSR_STAR, (0x18ull << 48) | (0x08ull << 32));
    wrmsr(MSR_LSTAR, (u64)(usize)syscall_entry);
    wrmsr(MSR_SFMASK, 0x700);             /* clear TF, IF, DF on entry */
}

/* ---- ELF loading -------------------------------------------------------------- */
typedef struct { u8 ident[16]; u16 type, machine; u32 version; u64 entry, phoff, shoff; u32 flags;
                 u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } ehdr_t;
typedef struct { u32 type, flags; u64 offset, vaddr, paddr, filesz, memsz, align; } phdr_t;

/* copy bytes into the process image at va (kernel page tables active) */
static void put_user(proc_t *p, u64 va, const void *src, u64 len) {
    const u8 *s = src;
    while (len) {
        u64 pa = as_translate(p->cr3, va);
        if (!pa) { as_map(p->cr3, va & ~(PAGE - 1), pmm_alloc(0), 1); pa = as_translate(p->cr3, va); }
        u64 chunk = MIN(len, PAGE - (va & (PAGE - 1)));
        phys_write(pa, s, chunk);
        va += chunk; s += chunk; len -= chunk;
    }
}

static int load_elf(proc_t *p, const u8 *img, u64 size, u64 *phdr_va, u16 *phnum, char *err, usize cap) {
    const ehdr_t *e = (const ehdr_t *)img;
    if (size < sizeof *e || memcmp(e->ident, "\x7f" "ELF", 4) || e->ident[4] != 2 || e->machine != 62) {
        fmt(err, cap, "not an x86-64 ELF program"); return 0;
    }
    if (e->type != 2 && e->type != 3) { fmt(err, cap, "ELF type %u is not executable", e->type); return 0; }
    u64 base = e->type == 3 ? USER_PIE_BASE : 0;
    u64 hi = 0;
    *phdr_va = 0;
    for (int i = 0; i < e->phnum; i++) {
        const phdr_t *ph = (const phdr_t *)(img + e->phoff + (u64)i * e->phentsize);
        if (e->phoff + (u64)(i + 1) * e->phentsize > size) break;
        if (ph->type == 3) { fmt(err, cap, "dynamically linked (needs ld.so) - use a static build"); return 0; }
        if (ph->type == 6) *phdr_va = base + ph->vaddr;
        if (ph->type != 1) continue;
        u64 va = base + ph->vaddr, end = va + ph->memsz;
        if (end > USER_MMAP_BASE || ph->offset + ph->filesz > size) { fmt(err, cap, "segment at %llx does not fit", va); return 0; }
        for (u64 a = va & ~(PAGE - 1); a < end; a += PAGE)
            if (!as_translate(p->cr3, a)) as_map(p->cr3, a, pmm_alloc(0), 1);
        put_user(p, va, img + ph->offset, ph->filesz);
        if (!*phdr_va && ph->offset <= e->phoff && e->phoff < ph->offset + ph->filesz)
            *phdr_va = va + (e->phoff - ph->offset);
        if (end > hi) hi = end;
    }
    p->entry = base + e->entry;
    p->brk_start = p->brk = (hi + PAGE - 1) & ~(PAGE - 1);
    *phnum = e->phnum;
    return 1;
}

/* argv, envp and the auxiliary vector, laid out the way the Linux kernel does */
static u64 build_stack(proc_t *p, int argc, const char *const *argv, u64 phdr, u16 phnum) {
    static const char *envp[] = { "PATH=/bin", "HOME=/", "TERM=dumb", "USER=root", "PWD=/", NULL };
    u64 sp = USER_STACK_TOP;
    u64 strs[48], envs[8];
    int nenv = 0;
    argc = MIN(argc, 40);
    for (int i = argc - 1; i >= 0; i--) { u64 n = strlen(argv[i]) + 1; sp -= n; put_user(p, sp, argv[i], n); strs[i] = sp; }
    for (int i = 0; envp[i]; i++) { u64 n = strlen(envp[i]) + 1; sp -= n; put_user(p, sp, envp[i], n); envs[nenv++] = sp; }
    sp -= 8; put_user(p, sp, "x86_64", 7); u64 platform = sp;
    u64 execfn = strs[0];
    u8 rnd[16];
    for (int i = 0; i < 16; i++) rnd[i] = (u8)rand32();
    sp -= 16; put_user(p, sp, rnd, 16); u64 random = sp;
    sp &= ~15ull;

    u32 id[4];
    cpuid(1, 0, id);
    u64 aux[] = { 3, phdr, 4, 56, 5, phnum, 6, PAGE, 7, 0, 8, 0, 9, p->entry, 11, 0, 12, 0, 13, 0, 14, 0,
                  15, platform, 16, id[3], 17, 100, 23, 0, 25, random, 26, 0, 31, execfn, 0, 0 };
    u64 words = 1 + (u64)argc + 1 + (u64)nenv + 1 + ARRAY_LEN(aux);
    sp -= words * 8;
    sp &= ~15ull;                                         /* rsp % 16 == 0 at the entry point */
    u64 w = sp;
    u64 v = (u64)argc;
    put_user(p, w, &v, 8); w += 8;
    for (int i = 0; i < argc; i++) { put_user(p, w, &strs[i], 8); w += 8; }
    v = 0; put_user(p, w, &v, 8); w += 8;
    for (int i = 0; i < nenv; i++) { put_user(p, w, &envs[i], 8); w += 8; }
    put_user(p, w, &v, 8); w += 8;
    put_user(p, w, aux, sizeof aux);
    return sp;
}

static void user_thread(void *arg) {
    proc_t *p = arg;
    wrmsr(MSR_FS_BASE, 0);
    enter_user(p->entry, p->sp);
}

proc_t *proc_spawn(const char *path, int argc, const char *const *argv, term_t *term, char *err, usize cap) {
    const char *why;
    if (!proc_user_supported(&why)) { fmt(err, cap, "%s", why); return NULL; }
    vnode_t *n = vfs_lookup(path);
    if (!n || n->dir) { fmt(err, cap, "%s: not found", path); return NULL; }
    u64 size = vfs_size(n);
    u8 *img = kalloc(size ? size : 1);
    vfs_read(n, 0, img, size);

    proc_t *p = kalloc(sizeof *p);
    p->pid = next_pid++;
    const char *base = path;
    for (const char *c = path; *c; c++) if (*c == '/') base = c + 1;
    strlcpy(p->name, argc > 0 ? argv[0] : base, sizeof p->name);
    strlcpy(p->exe, path, sizeof p->exe);
    strlcpy(p->cwd, "/", sizeof p->cwd);
    p->term = term;
    p->cr3 = as_create();
    u64 phdr = 0;
    u16 phnum = 0;
    int ok = load_elf(p, img, size, &phdr, &phnum, err, cap);
    kfree(img);
    if (!ok) { as_destroy(p->cr3); kfree(p); return NULL; }
    proc_add_vma(p, p->brk_start, p->brk);
    p->mmap_next = USER_MMAP_BASE;
    p->sp = build_stack(p, argc, argv, phdr, phnum);
    p->fd[0] = (ufile_t){ F_NULL };                        /* stdin: empty */
    p->fd[1] = (ufile_t){ F_TTY };
    p->fd[2] = (ufile_t){ F_TTY };
    p->th = thread_create(p->name, user_thread, p, p->cr3);
    p->th->proc = p;
    return p;
}

void proc_exit(int code) {
    proc_t *p = proc_current();
    sti();                                     /* socket cleanup takes the network lock */
    lsock_exit(p);
    cli();
    thread_t *t = thread_current();
    write_cr3(kernel_cr3());                   /* leave the address space before freeing it */
    t->cr3 = kernel_cr3();
    as_destroy(p->cr3);
    p->exit_code = code;
    p->exited = 1;
    p->term->serial++;
    thread_exit();
}

void proc_kill(proc_t *p) {
    if (!p || p->exited) return;
    if (p->in_syscall) {
        /* It may hold the network lock or be mid-way through a file
         * operation: let it finish the call and exit by itself
         * (blocking waits notice 'killed' within milliseconds). */
        if (!p->killed) term_append(p->term, "\n[stopping]\n", 13);
        p->killed = 1;
        return;
    }
    lsock_exit(p);                             /* not in the kernel: its sockets are free to close */
    u64 fl = irq_save();
    /* The process is not running (we are the shell on the only user core),
     * so it can be torn down from here. */
    p->th->state = T_DEAD;
    as_destroy(p->cr3);
    p->exit_code = 137;
    p->exited = 1;
    term_append(p->term, "\n[stopped]\n", 11);
    irq_restore(fl);
}
