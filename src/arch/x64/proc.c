/*
 * proc.c - running Linux programs.
 *
 * Every process gets its own page tables: the first GiB of the address space
 * is the process's; everything above is the kernel's identity map,
 * supervisor-only.  So a classic non-PIE binary linked at 0x400000 loads at
 * its own addresses; PIE binaries are placed at 256 MiB.  The stack sits at
 * the top of the first GiB and grows on demand; brk and anonymous mmap
 * regions are also populated lazily, page by page, from page faults.
 *
 * A dynamically linked program names its loader (PT_INTERP, e.g.
 * /lib64/ld-linux-x86-64.so.2): the loader is placed in the mmap window and
 * started instead, with AT_BASE/AT_ENTRY/AT_PHDR telling it about the
 * program, exactly as Linux does; it then maps the libraries itself.
 *
 * Threads (clone with CLONE_VM | CLONE_THREAD) share the page tables and
 * get their own TLS pointer; they start from a copy of the parent's
 * system-call frame with RAX = 0.  All threads of all processes run in ring
 * 3 on the boot core as ordinary scheduler threads and enter the kernel
 * through SYSCALL (linux.c).
 */
#include "proc.h"
#include "lsock.h"

extern void enter_user(u64 rip, u64 rsp);
extern void syscall_entry(void);
extern int (*user_fault_hook)(frame_t *f);
extern int (*page_fault_hook)(frame_t *f);

static int next_pid = 100;

/* every process ever started, for wait4 / kill / getppid (proc_t's are never freed) */
#define MAX_PROCS 1024
static proc_t *procs[MAX_PROCS];
static int nprocs;
static void proc_register(proc_t *p) { if (nprocs < MAX_PROCS) procs[nprocs++] = p; }
proc_t *proc_at(int i) { return i >= 0 && i < nprocs ? procs[i] : NULL; }

thread_t *proc_thread(proc_t *p, int tid) {
    thread_t *all[64];
    int n = sched_threads(all, 64);
    for (int i = 0; i < n; i++)
        if (all[i]->proc == p && all[i]->state != T_DEAD && (!tid || all[i]->tid == tid)) return all[i];
    return NULL;
}

/* a blocking system call should give up: the process is going, or a signal waits for a handler */
int proc_interrupted(proc_t *p) { return p->killed || sig_deliverable(p) != 0; }

proc_t *proc_by_pid(int pid) {
    for (int i = nprocs - 1; i >= 0; i--) if (procs[i]->pid == pid) return procs[i];
    return NULL;
}

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

/* ---- the address space: VMAs ---------------------------------------------------------
 * Each VMA has Linux's PROT_* bits and, for shared memory, the object whose pages it
 * shows.  Pages appear lazily on the first touch, with the VMA's protection: no
 * PROT_WRITE - read-only, no PROT_EXEC - no-execute, PROT_NONE - every access faults
 * (Firefox reserves gigabytes that way and commits pieces with mprotect). */
static int as_prot(u32 prot, int shared) {
    return ((prot & PROT_WRITE) ? AS_W : 0) | ((prot & PROT_EXEC) ? AS_X : 0) | (shared ? AS_SHARED : 0);
}

int proc_add_vma_prot(proc_t *p, u64 start, u64 end, u32 prot, kobj_t *obj, u64 off) {
    if (!obj)
        for (int i = 0; i < p->nvma; i++)                   /* extend a private neighbour (brk) */
            if (p->vma[i].end == start && !p->vma[i].obj && p->vma[i].prot == prot) { p->vma[i].end = end; return 0; }
    if (p->nvma == MAX_VMAS) return -1;
    p->vma[p->nvma++] = (vma_t){ start, end, prot, obj != NULL, obj, off };
    if (obj) kobj_get(obj);
    return 0;
}

int proc_add_vma(proc_t *p, u64 start, u64 end) { return proc_add_vma_prot(p, start, end, PROT_READ | PROT_WRITE, NULL, 0); }

vma_t *proc_vma(proc_t *p, u64 a) {
    for (int i = 0; i < p->nvma; i++) if (a >= p->vma[i].start && a < p->vma[i].end) return &p->vma[i];
    return NULL;
}

static int in_stack(u64 a) { return a >= USER_STACK_TOP - USER_STACK_SIZE && a < USER_STACK_TOP; }

void proc_vmas_release(proc_t *p) {
    for (int i = 0; i < p->nvma; i++) if (p->vma[i].obj) { kobj_put(p->vma[i].obj); p->vma[i].obj = NULL; }
    p->nvma = 0;
}

static int range_user(u64 start, u64 end) {
    if (end <= start) return 0;
    if (end <= USER_STACK_TOP - USER_STACK_SIZE) return start >= PAGE;
    return user_high_range(start, end);
}

int proc_range_free(proc_t *p, u64 start, u64 end) {
    if (!range_user(start, end)) return 0;
    if (start < p->brk + (64ull << 20) && end > p->brk_start) return 0;     /* leave brk room to grow */
    for (int i = 0; i < p->nvma; i++) if (start < p->vma[i].end && p->vma[i].start < end) return 0;
    return 1;
}

static u64 find_in(proc_t *p, u64 lo, u64 hi, u64 len) {
    u64 a = lo;
    for (int pass = 0; pass < MAX_VMAS + 2; pass++) {
        if (a + len > hi) return 0;
        int moved = 0;
        for (int i = 0; i < p->nvma; i++)
            if (a < p->vma[i].end + PAGE && p->vma[i].start < a + len + PAGE) { a = p->vma[i].end + PAGE; moved = 1; }
        if (!moved) return a;
    }
    return 0;
}

/* first fit in the mmap window (below the kernel's hole first), one guard page after every mapping */
u64 proc_find_free(proc_t *p, u64 len) {
    u64 a = find_in(p, USER_MMAP_BASE, USER_HOLE_BASE, len);
    return a ? a : find_in(p, USER_HOLE_END, USER_MMAP_END, len);
}
u64 proc_find_free_low(proc_t *p, u64 len) { return find_in(p, USER_LOW_MMAP, USER_STACK_TOP - USER_STACK_SIZE - (1ull << 20), len); }

/* cut [start, end) out of the VMAs: returns how many pieces were removed or trimmed */
static void vma_cut(proc_t *p, u64 start, u64 end) {
    for (int i = 0; i < p->nvma; i++) {
        vma_t *v = &p->vma[i];
        if (end <= v->start || start >= v->end) continue;
        if (start <= v->start && end >= v->end) {             /* all of it */
            if (v->obj) kobj_put(v->obj);
            *v = p->vma[--p->nvma];
            i--;
            continue;
        }
        if (start > v->start && end < v->end) {               /* a hole: split */
            if (p->nvma < MAX_VMAS) {
                vma_t tail = *v;
                tail.off += end - v->start;
                tail.start = end;
                if (tail.obj) kobj_get(tail.obj);
                p->vma[p->nvma++] = tail;
            }
            v->end = start;
            continue;
        }
        if (start <= v->start) { v->off += end - v->start; v->start = end; }
        else v->end = start;
    }
}

/* munmap: free the pages, cut the VMAs (splitting one that spans the hole) */
void proc_unmap(proc_t *p, u64 start, u64 end) {
    as_unmap_range(p->cr3, start, end);
    vma_cut(p, start, end);
}

/* mprotect: split the VMAs at the edges, give the inside the new protection */
i64 proc_protect(proc_t *p, u64 start, u64 end, u32 prot) {
    /* every page of the range must be mapped (Linux: ENOMEM otherwise) */
    for (u64 a = start; a < end;) {
        vma_t *v = proc_vma(p, a);
        if (!v) { if (in_stack(a)) { a += PAGE; continue; } return -12; }
        a = v->end;
    }
    for (int i = 0; i < p->nvma; i++) {
        vma_t *v = &p->vma[i];
        if (end <= v->start || start >= v->end || v->prot == prot) continue;
        if (p->nvma + 2 > MAX_VMAS) return -12;
        if (v->start < start) {                               /* the part before keeps its protection */
            vma_t head = *v;
            head.end = start;
            if (head.obj) kobj_get(head.obj);
            v->off += start - v->start;
            v->start = start;
            p->vma[p->nvma++] = head;
            v = &p->vma[i];
        }
        if (v->end > end) {                                   /* and the part after */
            vma_t tail = *v;
            tail.off += end - v->start;
            tail.start = end;
            if (tail.obj) kobj_get(tail.obj);
            v->end = end;
            p->vma[p->nvma++] = tail;
            v = &p->vma[i];
        }
        v->prot = prot;
    }
    as_protect_range(p->cr3, start, end, prot ? as_prot(prot, 0) : AS_NONE);   /* PROT_NONE keeps the contents */
    return 0;
}

u64 shm_frame(kobj_t *o, u64 page);                           /* linux.c: a page of a shared object */

/* map the page at a for an access (write: a store); 0 if the program may not */
static int fault_in_access(proc_t *p, u64 a, int write) {
    if (!user_va(a)) return 0;
    vma_t *v = proc_vma(p, a);
    u64 page = a & ~(PAGE - 1);
    if (!v) {
        if (!in_stack(a)) return 0;
        if (!as_translate(p->cr3, a)) as_map(p->cr3, page, pmm_alloc(0), AS_W);
        return 1;
    }
    if (!v->prot || (write && !(v->prot & PROT_WRITE))) return 0;
    if (as_translate(p->cr3, a)) return !write || as_pte_writable(p->cr3, a);
    if (v->obj) {
        u64 f = shm_frame(v->obj, (page - v->start + v->off) / PAGE);
        if (!f) return 0;
        as_map(p->cr3, page, f, as_prot(v->prot, 1));
    } else as_map(p->cr3, page, pmm_alloc(0), as_prot(v->prot, 0));
    return 1;
}

int proc_user_ok(proc_t *p, u64 addr, u64 len) {
    if (!p || len > USER_HIGH_END || !user_va(addr) || (len && !user_va(addr + len - 1))) return 0;
    if (addr < USER_TOP && addr + len > USER_TOP) return 0;
    for (u64 a = addr & ~(PAGE - 1); a < addr + len; a += PAGE)
        if (!as_translate(p->cr3, a) && !fault_in_access(p, a, 0)) return 0;
    return 1;
}

/* ---- faults ------------------------------------------------------------------ */
/* a page fault: not present - map it if a VMA allows the access; present - a protection
 * violation (a store to a read-only page, a jump into data): the program's fault */
static int on_page_fault(frame_t *f) {
    proc_t *p = proc_current();
    if (!p || f->vector != 14 || (f->err & 1)) return 0;
    return fault_in_access(p, read_cr2(), (f->err & 2) != 0);
}

static int on_user_fault(frame_t *f) {
    proc_t *p = proc_current();
    if (!p) return 0;
    if (sig_fault(p, f)) return 1;                            /* the program handles it (SIGSEGV...) */
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
    mm_enable_nx();                       /* PROT_EXEC means something now */
    /* the kernel writes into user buffers after checking them (proc_user_ok); a read-only
     * page there must not panic it (Linux would copy with a fault fixup) */
    u64 cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0 & ~(1ull << 16)));
    sig_init();
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

typedef struct { u64 entry, phdr, hi, base; u16 phnum; char interp[96]; int native; } image_t;

/* a native QRT program carries a note named "QRT" (type 1), put there by the SDK's crt */
static int has_qrt_note(const u8 *img, u64 size, const phdr_t *ph) {
    u64 o = ph->offset, end = ph->offset + ph->filesz;
    if (end > size) return 0;
    while (o + 12 <= end) {
        u32 nsz = *(const u32 *)(img + o), dsz = *(const u32 *)(img + o + 4), type = *(const u32 *)(img + o + 8);
        if (nsz == 4 && type == 1 && o + 16 <= end && !memcmp(img + o + 12, "QRT", 4)) return 1;
        o += 12 + ((nsz + 3) & ~3u) + ((dsz + 3) & ~3u);
    }
    return 0;
}

/* /proc/self/status: the lines programs read (Ladybird's debugger check reads TracerPid) */
int proc_status_text(char *b, int cap) {
    proc_t *p = proc_current();
    if (!p) return 0;
    return fmt(b, (usize)cap, "Name:\t%s\nState:\tR (running)\nTgid:\t%d\nPid:\t%d\nPPid:\t%d\nTracerPid:\t0\n"
               "Uid:\t0\t0\t0\t0\nGid:\t0\t0\t0\t0\nThreads:\t1\n", p->name, p->pid, p->pid, p->ppid);
}

/* page-aligned memory outside the heap: a big file's own pages */
static int file_pages(const u8 *d) { return d && !((u64)(usize)d & (PAGE - 1)) && !heap_owns(d); }

/* Load one ELF image: an executable (ET_EXEC at its own addresses, ET_DYN at
 * USER_PIE_BASE) or, with base != 0, a shared object such as ld.so. */
static int load_elf(proc_t *p, const u8 *img, u64 size, u64 base, image_t *out, char *err, usize cap) {
    const ehdr_t *e = (const ehdr_t *)img;
    memset(out, 0, sizeof *out);
    if (size < sizeof *e || memcmp(e->ident, "\x7f" "ELF", 4) || e->ident[4] != 2 || e->machine != 62) {
        fmt(err, cap, "not an x86-64 ELF program"); return 0;
    }
    if (e->type != 2 && e->type != 3) { fmt(err, cap, "ELF type %u is not executable", e->type); return 0; }
    if (!base) base = e->type == 3 ? USER_PIE_BASE : 0;
    if (e->phoff + (u64)e->phnum * e->phentsize > size) { fmt(err, cap, "truncated program headers"); return 0; }
    for (int i = 0; i < e->phnum; i++) {
        const phdr_t *ph = (const phdr_t *)(img + e->phoff + (u64)i * e->phentsize);
        if (ph->type == 3) {                                       /* PT_INTERP */
            u64 n = MIN(ph->filesz, sizeof out->interp - 1);
            if (ph->offset + n > size) { fmt(err, cap, "bad PT_INTERP"); return 0; }
            memcpy(out->interp, img + ph->offset, n);
            out->interp[n] = 0;
        }
        if (ph->type == 6) out->phdr = base + ph->vaddr;          /* PT_PHDR */
        if (ph->type == 4 && has_qrt_note(img, size, ph)) out->native = 1;   /* PT_NOTE */
        if (ph->type != 1) continue;
        u64 va = base + ph->vaddr, end = va + ph->memsz;
        if (!range_user(va & ~(PAGE - 1), end) || ph->offset + ph->filesz > size) { fmt(err, cap, "segment at %llx does not fit", va); return 0; }
        /* a read-only segment of a file held in whole pages maps those pages, shared;
         * other pages get a private copy (zeroed past the file's bytes) */
        int share = file_pages(img) && !(ph->flags & 2) && ((ph->offset - ph->vaddr) & (PAGE - 1)) == 0;
        for (u64 a = va & ~(PAGE - 1); a < end; a += PAGE) {
            u64 lo = MAX(a, va), hi = MIN(a + PAGE, va + ph->filesz);
            if (!as_translate(p->cr3, a)) {
                u64 fo = ph->offset + (a - va);          /* wraps below va: the page's file offset */
                if (share && a + PAGE <= va + ph->filesz && fo + PAGE <= ((size + PAGE - 1) & ~(PAGE - 1))) {
                    as_map(p->cr3, a, (u64)(usize)(img + fo), AS_SHARED);
                    continue;
                }
                as_map(p->cr3, a, pmm_alloc(0), 1);
            } else if (file_pages(img)) {                /* a file page another segment shares: copy it first */
                u64 cur = as_translate(p->cr3, a) & ~(PAGE - 1);
                if (cur >= (u64)(usize)img && cur < (u64)(usize)img + size) {
                    u64 f = pmm_alloc(0);
                    phys_write(f, (const void *)(usize)cur, PAGE);
                    as_map(p->cr3, a, f, 1);
                }
            }
            if (lo < hi) put_user(p, lo, img + ph->offset + (lo - va), hi - lo);
        }
        if (!out->phdr && ph->offset <= e->phoff && e->phoff < ph->offset + ph->filesz)
            out->phdr = va + (e->phoff - ph->offset);
        if (end > out->hi) out->hi = end;
        /* the segment's own protection; a page shared with the previous segment gets both */
        u32 prot = ((ph->flags & 4) ? PROT_READ : 0) | ((ph->flags & 2) ? PROT_WRITE : 0) | ((ph->flags & 1) ? PROT_EXEC : 0);
        u64 s0 = va & ~(PAGE - 1), e0 = (end + PAGE - 1) & ~(PAGE - 1);
        vma_t *prev = proc_vma(p, s0);
        if (prev) { prev->prot |= prot; as_protect_range(p->cr3, s0, s0 + PAGE, as_prot(prev->prot, 0)); s0 += PAGE; }
        if (s0 < e0) {
            proc_add_vma_prot(p, s0, e0, prot, NULL, 0);
            as_protect_range(p->cr3, s0, e0, as_prot(prot, 0));
        }
    }
    out->entry = base + e->entry;
    out->base = base;
    out->phnum = e->phnum;
    return 1;
}

/* the size of the address range an image's PT_LOAD segments span */
static u64 elf_span(const u8 *img, u64 size) {
    const ehdr_t *e = (const ehdr_t *)img;
    u64 lo = ~0ull, hi = 0;
    if (size < sizeof *e || e->phoff + (u64)e->phnum * e->phentsize > size) return 0;
    for (int i = 0; i < e->phnum; i++) {
        const phdr_t *ph = (const phdr_t *)(img + e->phoff + (u64)i * e->phentsize);
        if (ph->type != 1) continue;
        if (ph->vaddr < lo) lo = ph->vaddr;
        if (ph->vaddr + ph->memsz > hi) hi = ph->vaddr + ph->memsz;
    }
    return hi > lo ? ((hi + PAGE - 1) & ~(PAGE - 1)) : 0;
}

/* a file's contents: a big file's own pages (see vfs.c's deep_copy; kfree ignores them),
 * anything else copied into the heap */
static u8 *read_file(const char *path, u64 *size) {
    vnode_t *n = vfs_lookup(path);
    if (!n || n->dir) return NULL;
    *size = vfs_size(n);
    if (file_pages(n->data)) return n->data;
    u8 *img = kalloc(*size ? *size : 1);
    vfs_read(n, 0, img, *size);
    return img;
}

/* argv, envp and the auxiliary vector, laid out the way the Linux kernel does */
static const char *const default_env[] = { "PATH=/bin:/usr/bin", "HOME=/", "TERM=dumb", "USER=root", "PWD=/",
                                           "LD_LIBRARY_PATH=/lib:/usr/lib:/lib/x86_64-linux-gnu", NULL };

static u64 build_stack(proc_t *p, int argc, const char *const *argv, const char *const *envp, u64 phdr, u16 phnum) {
    if (!envp) envp = default_env;
    u64 sp = USER_STACK_TOP;
    u64 strs[64], envs[64];
    int nenv = 0;
    argc = MIN(argc, 64);
    for (int i = argc - 1; i >= 0; i--) { u64 n = strlen(argv[i]) + 1; sp -= n; put_user(p, sp, argv[i], n); strs[i] = sp; }
    p->trace = 0;
    for (int i = 0; envp[i]; i++) if (!strcmp(envp[i], "QRT_TRACE=1")) p->trace = 1;
    for (int i = 0; envp[i] && nenv < 64; i++) { u64 n = strlen(envp[i]) + 1; sp -= n; put_user(p, sp, envp[i], n); envs[nenv++] = sp; }
    sp -= 8; put_user(p, sp, "x86_64", 7); u64 platform = sp;
    u64 execfn = strs[0];
    u8 rnd[16];
    for (int i = 0; i < 16; i++) rnd[i] = (u8)rand32();
    sp -= 16; put_user(p, sp, rnd, 16); u64 random = sp;
    sp &= ~15ull;

    u32 id[4];
    cpuid(1, 0, id);
    u64 aux[] = { 3, phdr, 4, 56, 5, phnum, 6, PAGE, 7, p->interp_base, 8, 0, 9, p->entry, 11, 0, 12, 0, 13, 0, 14, 0,
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
    enter_user(p->start, p->sp);
}

proc_t *proc_spawn(const char *path, int argc, const char *const *argv, term_t *term, char *err, usize cap) {
    const char *why;
    if (!proc_user_supported(&why)) { fmt(err, cap, "%s", why); return NULL; }
    u64 size = 0;
    u8 *img = read_file(path, &size);
    if (!img) { fmt(err, cap, "%s: not found", path); return NULL; }

    proc_t *p = kalloc(sizeof *p);
    p->pid = next_pid++;
    const char *base = path;
    for (const char *c = path; *c; c++) if (*c == '/') base = c + 1;
    strlcpy(p->name, argc > 0 ? argv[0] : base, sizeof p->name);
    strlcpy(p->exe, path, sizeof p->exe);
    strlcpy(p->cwd, "/", sizeof p->cwd);
    p->term = term;
    p->cr3 = as_create();
    image_t im;
    int ok = load_elf(p, img, size, 0, &im, err, cap);
    kfree(img);
    if (ok && im.interp[0]) {
        /* dynamically linked: load the program's loader and start there */
        u64 isize = 0;
        u8 *iimg = read_file(im.interp, &isize);
        u64 span = iimg ? elf_span(iimg, isize) : 0;
        u64 ibase = span ? proc_find_free(p, span) : 0;
        image_t ii;
        if (!iimg) { fmt(err, cap, "%s needs %s, which is not installed", base, im.interp); ok = 0; }
        else if (!ibase) { fmt(err, cap, "no room for %s", im.interp); ok = 0; }
        else ok = load_elf(p, iimg, isize, ibase, &ii, err, cap);
        if (iimg) kfree(iimg);
        if (ok) { p->interp_base = ibase; p->start = ii.entry; }
    } else p->start = im.entry;
    if (!ok) { as_destroy(p->cr3); kfree(p); return NULL; }
    p->native = im.native;
    p->entry = im.entry;
    p->brk_start = p->brk = (im.hi + PAGE - 1) & ~(PAGE - 1);
    proc_add_vma(p, p->brk_start, p->brk);
    p->mmap_next = USER_MMAP_BASE;
    p->sp = build_stack(p, argc, argv, NULL, im.phdr, im.phnum);
    p->fd[0] = (ufile_t){ F_NULL };                        /* stdin: empty */
    p->fd[1] = (ufile_t){ F_TTY };
    p->fd[2] = (ufile_t){ F_TTY };
    if (!term) p->fd[1].flags = p->fd[2].flags = 1 << 30;   /* no terminal: output goes to the kernel log (KMSG in linux.c) */
    p->nthreads = 1;
    proc_register(p);
    u64 fl = irq_save();                       /* the thread must not run before it knows its process */
    p->th = thread_create(p->name, user_thread, p, p->cr3);
    p->th->proc = p;
    p->th->tid = p->pid;
    irq_restore(fl);
    return p;
}

/* ---- threads -------------------------------------------------------------------- */
#define CLONE_VM             0x00000100
#define CLONE_THREAD         0x00010000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000

extern void enter_user_frame(frame_t *f);
extern i64 futex_wake(proc_t *p, u64 uaddr, int n);       /* linux.c */

static void clone_thread(void *arg) {
    frame_t fr = *(frame_t *)arg;                          /* onto this thread's own kernel stack */
    kfree(arg);
    enter_user_frame(&fr);
}

#define CLONE_VFORK          0x00004000

/* fork (and vfork, and clone without CLONE_THREAD): a new process with a copy
 * of the address space and the descriptors; its thread resumes from a copy of
 * the caller's frame with RAX = 0.  vfork's parent waits for exec or exit. */
static i64 proc_fork(proc_t *p, frame_t *f, u64 flags, u64 newsp, u64 ptid, u64 ctid, u64 tls) {
    proc_t *c = kalloc(sizeof *c);
    c->pid = next_pid++;
    c->ppid = p->pid;
    strlcpy(c->name, p->name, sizeof c->name);
    strlcpy(c->exe, p->exe, sizeof c->exe);
    strlcpy(c->cwd, p->cwd, sizeof c->cwd);
    c->term = p->term;
    c->entry = p->entry; c->start = p->start; c->interp_base = p->interp_base;
    c->native = p->native;
    c->brk_start = p->brk_start; c->brk = p->brk; c->mmap_next = p->mmap_next;
    memcpy(c->vma, p->vma, sizeof c->vma);
    c->nvma = p->nvma;
    for (int i = 0; i < c->nvma; i++) if (c->vma[i].obj) kobj_get(c->vma[i].obj);   /* shared memory stays shared */
    memcpy(c->sa, p->sa, sizeof c->sa);                    /* handlers are inherited, pending signals are not */
    c->cr3 = as_clone(p->cr3);
    memcpy(c->fd, p->fd, sizeof c->fd);
    for (int i = 0; i < MAX_FDS; i++) fd_addref(c, i);
    frame_t *cf = kalloc(sizeof *cf);
    *cf = *f;
    cf->rax = 0;
    if (newsp) cf->rsp = newsp;
    if ((flags & CLONE_PARENT_SETTID) && proc_user_ok(p, ptid, 4)) *(i32 *)(usize)ptid = c->pid;
    if ((flags & CLONE_CHILD_SETTID) && ctid) {             /* in the child's copy */
        u64 pa = as_translate(c->cr3, ctid);
        i32 v = c->pid;
        if (pa) phys_write(pa, &v, 4);
    }
    c->nthreads = 1;
    proc_register(c);
    u64 fl = irq_save();
    thread_t *t = thread_create(c->name, clone_thread, cf, c->cr3);
    t->proc = c;
    t->tid = c->pid;
    t->fs_base = (flags & CLONE_SETTLS) ? tls : thread_current()->fs_base;
    t->clear_tid = (flags & CLONE_CHILD_CLEARTID) ? ctid : 0;
    t->sig_mask = thread_current()->sig_mask;
    t->alt_sp = thread_current()->alt_sp; t->alt_size = thread_current()->alt_size; t->alt_flags = thread_current()->alt_flags;
    c->th = t;
    irq_restore(fl);
    if (flags & CLONE_VFORK)
        while (!c->exec_done && !c->exited && !p->killed) thread_sleep_ms(1);
    return c->pid;
}

i64 proc_clone(proc_t *p, frame_t *f, u64 flags, u64 newsp, u64 ptid, u64 ctid, u64 tls) {
    if (!(flags & CLONE_THREAD)) return proc_fork(p, f, flags, newsp, ptid, ctid, tls);
    if (!(flags & CLONE_VM)) return -22;
    frame_t *cf = kalloc(sizeof *cf);
    *cf = *f;
    cf->rax = 0;                                           /* the child sees clone() return 0 */
    if (newsp) cf->rsp = newsp;
    int tid = next_pid++;
    if ((flags & CLONE_PARENT_SETTID) && proc_user_ok(p, ptid, 4)) *(i32 *)(usize)ptid = tid;
    if ((flags & CLONE_CHILD_SETTID) && proc_user_ok(p, ctid, 4)) *(i32 *)(usize)ctid = tid;
    u64 fl = irq_save();                                   /* do not run it before it is complete */
    thread_t *t = thread_create(p->name, clone_thread, cf, p->cr3);
    t->proc = p;
    t->tid = tid;
    t->fs_base = (flags & CLONE_SETTLS) ? tls : thread_current()->fs_base;
    t->clear_tid = (flags & CLONE_CHILD_CLEARTID) ? ctid : 0;
    t->sig_mask = thread_current()->sig_mask;
    p->nthreads++;
    irq_restore(fl);
    return tid;
}

static void release_tid(proc_t *p, thread_t *t) {
    if (!t->clear_tid || !proc_user_ok(p, t->clear_tid, 4)) return;
    *(i32 *)(usize)t->clear_tid = 0;                       /* pthread_join waits for this */
    futex_wake(p, t->clear_tid, 1);
}

/* SIGCHLD for the parent (ignored unless it installed a handler) */
static void notify_parent(proc_t *p) {
    proc_t *pp = proc_by_pid(p->ppid);
    if (pp && !pp->exited && pp->sa[17].handler > 1)
        sig_post(pp, NULL, 17, p->sig ? 2 : 1, p->pid, (u64)(p->sig ? p->sig : p->exit_code & 0xff));
}

static void teardown(proc_t *p, int code) {
    write_cr3(kernel_cr3());                   /* leave the address space before freeing it */
    thread_current()->cr3 = kernel_cr3();
    proc_vmas_release(p);
    as_destroy(p->cr3);
    p->exit_code = code;
    p->exited = 1;
    qrt_proc_gone(p);
    notify_parent(p);
    klog("proc: %s (pid %d) exited with %d, %llu system calls", p->name, p->pid, code, p->syscalls);
    p->term->serial++;
}

/* One thread ends (exit, or a thread that noticed its process is going). */
void proc_thread_exit(int code) {
    proc_t *p = proc_current();
    thread_t *t = thread_current();
    cli();
    release_tid(p, t);
    if (--p->nthreads > 0) thread_exit();
    p->exiting = 1;                            /* from here this thread finishes the job: a kill() */
    sti();                                     /* socket cleanup takes the network lock */
    fds_release_all(p);
    cli();
    teardown(p, p->killed ? (p->exit_code ? p->exit_code : 137) : code);
    thread_exit();
}

/* exit_group: the whole process.  Threads that are not inside a system call
 * are stopped right away; the others finish their call and leave by
 * themselves (they see 'killed'), and the last one out tears down. */
void proc_exit(int code) {
    proc_t *p = proc_current();
    thread_t *me = thread_current();
    cli();
    p->killed = 1;
    p->exit_code = code;
    thread_t *all[64];
    int n = sched_threads(all, 64);
    for (int i = 0; i < n; i++) {
        thread_t *t = all[i];
        if (t == me || t->proc != p || t->state == T_DEAD || t->in_sys) continue;
        t->state = T_DEAD;
        p->nthreads--;
    }
    if (--p->nthreads > 0) thread_exit();
    p->exiting = 1;
    sti();
    fds_release_all(p);
    cli();
    teardown(p, code);
    thread_exit();
}

void proc_kill(proc_t *p) {
    if (!p) return;
    for (int i = 0; i < nprocs; i++)                  /* its children first (pipelines under a shell) */
        if (procs[i]->ppid == p->pid && procs[i] != p && !procs[i]->exited) proc_kill(procs[i]);
    /* a process whose last thread is already tearing it down (with interrupts on, while
     * its descriptors close) finishes by itself: killing it now would mark that thread
     * dead mid-way and release everything a second time */
    if (p->exited || p->exiting) return;
    u64 fl = irq_save();
    int by_signal = p->sig != 0;                     /* kill() from a program: no message */
    if (!p->killed && !by_signal) term_append(p->term, "\n[stopping]\n", 13);
    p->killed = 1;
    p->exit_code = 137;
    if (!p->sig) p->sig = 9;
    /* threads that are running user code (not this one: we are the shell)
     * stop now; those in a system call leave when it returns */
    thread_t *all[64];
    int n = sched_threads(all, 64);
    int left = 0;
    for (int i = 0; i < n; i++) {
        thread_t *t = all[i];
        if (t->proc != p || t->state == T_DEAD) continue;
        if (t->in_sys) { left++; thread_wake(t); continue; }
        t->state = T_DEAD;
        p->nthreads--;
    }
    if (!left && p->nthreads <= 0) {
        irq_restore(fl);
        fds_release_all(p);                         /* nothing of it runs any more: its sockets are free to close */
        fl = irq_save();
        proc_vmas_release(p);
        as_destroy(p->cr3);
        p->exited = 1;
        notify_parent(p);
        if (!by_signal) term_append(p->term, "\n[stopped]\n", 11);
        klog("proc: %s (pid %d) stopped", p->name, p->pid);
    }
    irq_restore(fl);
}

/* ---- exec ---------------------------------------------------------------------------- */
#define EXEC_MAX_ARGS 64

/* Replace the program of the calling process.  argv/envp are kernel copies. */
i64 proc_exec(proc_t *p, frame_t *f, const char *path, char **argv, int argc, char **envp, int envc) {
    char resolved[128];
    if (!strcmp(path, "/proc/self/exe")) strlcpy(resolved, p->exe, sizeof resolved);
    else strlcpy(resolved, path, sizeof resolved);
    u64 size = 0;
    u8 *img = read_file(resolved, &size);
    if (!img) return -2;                                       /* ENOENT */
    /* #! scripts: run the interpreter with the script's path */
    char *sargv[EXEC_MAX_ARGS + 3];
    char interp[96], iarg[64];
    if (size > 2 && img[0] == '#' && img[1] == '!') {
        usize i = 2, n = 0;
        while (i < size && img[i] == ' ') i++;
        while (i < size && img[i] != ' ' && img[i] != '\n' && n + 1 < sizeof interp) interp[n++] = (char)img[i++];
        interp[n] = 0;
        n = 0;
        while (i < size && img[i] == ' ') i++;
        while (i < size && img[i] != '\n' && img[i] != '\r' && n + 1 < sizeof iarg) iarg[n++] = (char)img[i++];
        while (n && iarg[n - 1] == ' ') n--;
        iarg[n] = 0;
        kfree(img);
        int k2 = 0;
        sargv[k2++] = interp;
        if (iarg[0]) sargv[k2++] = iarg;
        sargv[k2++] = resolved;
        for (int i2 = 1; i2 < argc && k2 < EXEC_MAX_ARGS + 2; i2++) sargv[k2++] = argv[i2];
        sargv[k2] = NULL;
        argv = sargv; argc = k2;
        strlcpy(resolved, interp, sizeof resolved);
        img = read_file(resolved, &size);
        if (!img) return -2;
    }
    if (size < 4 || memcmp(img, "\x7f" "ELF", 4)) { kfree(img); return -8; }   /* ENOEXEC */

    /* other threads end now (exec in a threaded program) */
    thread_t *me = thread_current();
    u64 fl = irq_save();
    thread_t *all[64];
    int nt = sched_threads(all, 64);
    for (int i = 0; i < nt; i++)
        if (all[i] != me && all[i]->proc == p && all[i]->state != T_DEAD) { all[i]->state = T_DEAD; p->nthreads--; }
    irq_restore(fl);

    /* build the new image beside the old one */
    u64 old_cr3 = p->cr3;
    vma_t *old_vma = kalloc(sizeof p->vma);
    memcpy(old_vma, p->vma, sizeof p->vma);
    int old_nvma = p->nvma;
    u64 old_interp = p->interp_base;
    p->cr3 = as_create();
    p->nvma = 0;
    p->interp_base = 0;
    char err[96];
    image_t im;
    int ok = load_elf(p, img, size, 0, &im, err, sizeof err);
    kfree(img);
    if (ok && im.interp[0]) {
        u64 isize = 0;
        u8 *iimg = read_file(im.interp, &isize);
        u64 span = iimg ? elf_span(iimg, isize) : 0;
        u64 ibase = span ? proc_find_free(p, span) : 0;
        image_t ii;
        ok = iimg && ibase && load_elf(p, iimg, isize, ibase, &ii, err, sizeof err);
        if (iimg) kfree(iimg);
        if (ok) { p->interp_base = ibase; p->start = ii.entry; }
    } else if (ok) p->start = im.entry;
    if (!ok) {                                                 /* keep running the old program */
        as_destroy(p->cr3);
        p->cr3 = old_cr3;
        memcpy(p->vma, old_vma, sizeof p->vma);
        p->nvma = old_nvma;
        p->interp_base = old_interp;
        kfree(old_vma);
        klog("proc: exec %s failed: %s", resolved, err);
        return -8;
    }
    for (int i = 0; i < old_nvma; i++) if (old_vma[i].obj) kobj_put(old_vma[i].obj);
    p->native = im.native;                                     /* the new image decides the personality */
    kfree(old_vma);
    for (int i = 0; i < NSIG; i++) if (p->sa[i].handler > 1) p->sa[i] = (ksigaction_t){ 0 };   /* caught -> default; ignored stays */
    me->alt_sp = me->alt_size = 0; me->alt_flags = 0;
    p->entry = im.entry;
    p->brk_start = p->brk = (im.hi + PAGE - 1) & ~(PAGE - 1);
    proc_add_vma(p, p->brk_start, p->brk);
    p->mmap_next = USER_MMAP_BASE;
    const char *const *ev = envc ? (const char *const *)envp : NULL;
    p->sp = build_stack(p, argc, (const char *const *)argv, ev, im.phdr, im.phnum);

    fl = irq_save();
    me->cr3 = p->cr3;
    write_cr3(p->cr3);
    as_destroy(old_cr3);
    me->fs_base = 0;
    wrmsr(MSR_FS_BASE, 0);
    me->clear_tid = 0;
    irq_restore(fl);

    for (int i = 0; i < MAX_FDS; i++) if (p->fd[i].type && p->fd[i].cloexec) fd_release(p, i);
    const char *base = resolved;
    for (const char *c = resolved; *c; c++) if (*c == '/') base = c + 1;
    strlcpy(p->name, argc > 0 && argv[0][0] ? argv[0] : base, sizeof p->name);
    strlcpy(p->exe, resolved, sizeof p->exe);
    me->name = p->name;
    p->exec_done = 1;

    memset(f, 0, sizeof *f);                                   /* a fresh start: rip, rsp, nothing else */
    f->rip = p->start;
    f->rsp = p->sp;
    f->cs = 0x2b; f->ss = 0x23;
    f->rflags = 0x202;
    return 0;
}

/* ---- wait4 / kill ------------------------------------------------------------------------ */
i64 proc_wait(proc_t *p, int pid, u64 ustatus, int options) {
    for (;;) {
        int have = 0;
        for (int i = 0; i < nprocs; i++) {
            proc_t *c = procs[i];
            if (c->ppid != p->pid || c->reaped || c == p) continue;
            if (pid > 0 && c->pid != pid) continue;
            have = 1;
            if (!c->exited) continue;
            c->reaped = 1;
            if (ustatus && proc_user_ok(p, ustatus, 4))
                *(i32 *)(usize)ustatus = c->sig ? c->sig : (c->exit_code & 0xff) << 8;
            return c->pid;
        }
        if (!have) return -10;                                 /* ECHILD */
        if (options & 1) return 0;                             /* WNOHANG */
        if (proc_interrupted(p)) return -4;                    /* EINTR */
        thread_sleep_ms(2);
    }
}

/* kill(): signals with a handler (or blocked) are queued for the process; SIGKILL and
 * signals whose default action ends the process end it at once */
static int sig_default_ignored(int sig) { return sig == 17 || sig == 18 || sig == 23 || sig == 28 || sig == 19 || sig == 20 || sig == 21 || sig == 22; }

i64 proc_signal(proc_t *p, int pid, int sig) {
    if (sig < 0 || sig >= NSIG) return -22;
    int n = 0;
    for (int i = 0; i < nprocs; i++) {
        proc_t *t = procs[i];
        if (t->exited) continue;
        if (pid > 0 ? t->pid != pid : pid == -1 ? t == p : t->ppid != p->pid && t != p) continue;
        n++;
        if (!sig) continue;
        u64 h = t->sa[sig].handler;
        thread_t *main = t->th && t->th->state != T_DEAD ? t->th : proc_thread(t, 0);
        int blocked = main && (main->sig_mask >> (sig - 1)) & 1;
        if (sig != 9 && (h > 1 || blocked)) { sig_post(t, NULL, sig, 0, p->pid, 0); continue; }
        if (sig != 9 && (h == 1 || sig_default_ignored(sig))) continue;    /* SIG_IGN, or ignored by default */
        t->sig = sig;
        if (t == p) { p->exit_code = 128 + sig; thread_current()->in_sys = 0; proc_exit(128 + sig); }
        proc_kill(t);
    }
    return n ? 0 : -3;                                         /* ESRCH */
}
