/*
 * uentry.c - programs on 64-bit ARM: EL0 and the way in and out of it, for the shared
 * user-mode layer (src/user: processes, the Linux system calls, signals).
 *
 * A program's thread is an ordinary kernel thread (sched.c) with an address space
 * (TTBR0, switched with it) and a thread pointer (TPIDR_EL0).  It enters EL0 by
 * returning from a made-up exception frame (boot.S's exc_return); everything that
 * brings it back - SVC for a system call, an abort for a page that is not there yet,
 * an interrupt - arrives in arm_exception (main.c) and is handed here with the frame,
 * whose FP/SIMD part the vectors saved too.  System calls run with interrupts on,
 * under the big lock (sched.c), like the x86-64 kernel's.
 */
#include "uarch.h"
#include "../../user/proc.h"
#include "../../net/netstack.h"

void syscall_dispatch(frame_t *f);                        /* src/user/linux.c */
int  proc_page_fault(u64 a, int write, int permission);  /* src/user/proc.c */
void arm_enter_user(frame_t *f);                          /* boot.S */

void (*user_return_hook)(frame_t *f);                     /* signals (signal.c's sig_init sets them) */
int (*user_return_check)(void);

/* ---- threads, by x86-64's names --------------------------------------------------------- */
thread_t *thread_create_suspended(const char *name, void (*fn)(void *), void *arg, u64 cr3) {
    thread_t *t = thr_create_suspended(name, fn, arg, 64 << 10);
    t->cr3 = cr3 == kernel_cr3() ? 0 : cr3;
    return t;
}
thread_t *thread_create(const char *name, void (*fn)(void *), void *arg, u64 cr3) {
    thread_t *t = thread_create_suspended(name, fn, arg, cr3);
    thr_wake(t);
    return t;
}
void thread_block(void) { thr_block(); }
void cpu_load_cr3(u64 cr3) {
    __asm__ volatile("msr ttbr0_el1, %0; isb" : : "r"(cr3) : "memory");
    thr_self()->cr3 = cr3 == kernel_cr3() ? 0 : cr3;
}
void kernel_symbol(u64 addr, char *out, usize cap) {
    extern char _start[];
    fmt(out, cap, "kernel+%llx", addr - (u64)(usize)_start);
}

/* AT_HWCAP: what the cores have (ID registers), in Linux's bits */
u64 arm_hwcap(void) {
    u64 isar0 = SYSREG_R(id_aa64isar0_el1), pfr0 = SYSREG_R(id_aa64pfr0_el1), h = 0;
    if (((pfr0 >> 16) & 0xf) != 0xf) h |= 1;                      /* FP */
    if (((pfr0 >> 20) & 0xf) != 0xf) h |= 2;                      /* ASIMD */
    h |= 4;                                                       /* EVTSTRM */
    if ((isar0 >> 4) & 0xf) h |= 8;                               /* AES */
    if (((isar0 >> 4) & 0xf) >= 2) h |= 16;                       /* PMULL */
    if ((isar0 >> 8) & 0xf) h |= 32;                              /* SHA1 */
    if ((isar0 >> 12) & 0xf) h |= 64;                             /* SHA2 */
    if ((isar0 >> 16) & 0xf) h |= 128;                            /* CRC32 */
    if (((isar0 >> 20) & 0xf) >= 2) h |= 256;                     /* ATOMICS */
    return h;
}

/* ---- into EL0 ------------------------------------------------------------------------------ */
void arm_user_frame(frame_t *f, u64 pc, u64 sp) {
    memset(f, 0, sizeof *f);
    f->pc = pc;
    f->sp = sp;
    f->pstate = 0;                                                /* EL0t, interrupts on */
}
/* a new program's first thread (proc.c) */
void enter_user(u64 pc, u64 sp) {
    frame_t f;
    arm_user_frame(&f, pc, sp);
    SYSREG_W(tpidr_el0, thr_self()->fs_base);
    arm_enter_user(&f);
}
/* a new thread or a forked process: from a copy of its parent's frame */
void enter_user_frame(frame_t *src) {
    frame_t f = *src;
    SYSREG_W(tpidr_el0, thr_self()->fs_base);
    arm_enter_user(&f);
}

/* ---- out of EL0 ------------------------------------------------------------------------------ */
static void user_fault(proc_t *p, frame_t *f) {
    if (sig_fault(p, f)) return;                                  /* the program handles it */
    u64 ec = thr_self()->fault_esr >> 26;
    int seg = ec == 0x20 || ec == 0x24;
    char msg[160];
    fmt(msg, sizeof msg, "\n[%s: %s at %llx, address %llx - killed]\n", p->name,
        seg ? "segmentation fault" : ec == 0 ? "illegal instruction" : "fault", f->pc, thr_self()->fault_far);
    term_append(p->term, msg, strlen(msg));
    proc_exit(seg ? 139 : 132);
}

/* before going back to EL0: a stopped thread goes; signals get their handlers */
static void user_return(frame_t *f) {
    thread_t *t = thr_self();
    if (t->state == T_DEAD) thr_exit();
    if (user_return_hook && (!user_return_check || user_return_check())) {
        bkl_lock();
        if (t->state == T_DEAD) thr_exit();
        user_return_hook(f);
        bkl_unlock();
    }
}

/* EL0 synchronous exceptions (arm_exception's kind 8), interrupts masked on entry */
void arm_user_sync(frame_t *f) {
    thread_t *t = thr_self();
    u64 esr = SYSREG_R(esr_el1), far = SYSREG_R(far_el1), ec = esr >> 26;
    proc_t *p = t->proc;
    if (!p) { klog("EL0 exception (esr %llx) in kernel thread %s", esr, t->name ? t->name : "?"); thr_park(); }
    if (t->state == T_DEAD) thr_exit();
    t->fs_base = SYSREG_R(tpidr_el0);                             /* the program sets it itself: fork and clone copy it */
    __asm__ volatile("msr daifclr, #2" ::: "memory");
    if (ec == 0x15) {                                             /* SVC: linux.c (signals included) */
        syscall_dispatch(f);
        int as_close_write(u64 cr3, u64 va);
        while (t->wp_n > 0) {
            t->wp_n--;
            for (u64 a = t->wp_page[t->wp_n * 2]; a < t->wp_page[t->wp_n * 2 + 1]; a += PAGE) as_close_write(p->cr3, a);
        }
    }
    else {
        bkl_lock();
        int handled = 0;
        if (ec == 0x20 || ec == 0x24) {                           /* instruction / data abort */
            u32 fsc = esr & 0x3f;
            int write = ec == 0x24 && (esr & (1u << 6)) && !(esr & (1u << 8));   /* WnR (not a cache maintenance op) */
            if ((fsc & 0x3c) == 0x04) handled = proc_page_fault(far, write, 0);         /* translation fault */
            else if ((fsc & 0x3c) == 0x0c) handled = proc_page_fault(far, write, 1);    /* permission fault */
            else if ((fsc & 0x3c) == 0x08) handled = proc_page_fault(far, write, 0);    /* access flag */
        }
        if (!handled) {
            t->fault_esr = esr; t->fault_far = far;
            user_fault(p, f);
        }
        bkl_unlock();
    }
    user_return(f);
    __asm__ volatile("msr daifset, #2" ::: "memory");
}

/* after an interrupt taken from EL0 (the scheduler may have run others meanwhile) */
void arm_user_irq_tail(frame_t *f) {
    user_return(f);
}

/* An abort at a program's address in the kernel (EL1): its system call touching a page
 * that is not there yet - map it; anything else is a kernel bug reached through what
 * the program asked: stop that program, as Linux's oops does, and keep the rest running.
 * Returns 0 if it is not a program's (the caller panics). */
int arm_kernel_user_fault(frame_t *f) {
    thread_t *t = thr_self();
    proc_t *p = t->proc;
    u64 esr = SYSREG_R(esr_el1), far = SYSREG_R(far_el1), ec = esr >> 26;
    if (!p || ec != 0x25) return 0;
    u32 fsc = esr & 0x3f;
    int write = (esr & (1u << 6)) && !(esr & (1u << 8));
    if (user_va(far)) {
        if ((fsc & 0x3c) == 0x04 || (fsc & 0x3c) == 0x08) { if (proc_page_fault(far, write, 0)) return 1; }
        else if ((fsc & 0x3c) == 0x0c && proc_page_fault(far, write, 1)) return 1;
        int as_open_write(u64 cr3, u64 va);
        u64 pg = far & ~(PAGE - 1);
        int last = t->wp_n - 1, extend = last >= 0 && t->wp_page[last * 2 + 1] == pg;
        if ((fsc & 0x3c) == 0x0c && write && t->in_sys && (extend || t->wp_n < 8) && as_open_write(p->cr3, far)) {   /* see umm.c */
            if (extend) t->wp_page[last * 2 + 1] = pg + PAGE;     /* a run of pages (a file copied in) */
            else { t->wp_page[t->wp_n * 2] = pg; t->wp_page[t->wp_n * 2 + 1] = pg + PAGE; t->wp_n++; }
            return 1;
        }
    }
    if (!t->in_sys || p->exiting || p->exited) return 0;
    char at[64];
    kernel_symbol(f->pc, at, sizeof at);
    klog("kernel fault in a system call of %s (pid %d): at %s (lr %llx), address %llx, esr %llx - %s stopped, QRT goes on",
         p->name, p->pid, at, f->x[30], far, esr, p->name);
    char msg[160];
    fmt(msg, sizeof msg, "\n[%s: a QRT kernel bug stopped it - see System Monitor's log]\n", p->name);
    term_append(p->term, msg, strlen(msg));
    net_lock_forfeit(t);
    t->in_sys = 0;
    __asm__ volatile("msr daifclr, #2" ::: "memory");
    proc_exit(139);                                               /* never returns */
    return 1;
}

/* ---- qrt.run=PROG[,PROG...]: programs run one after another at boot (tests), their
 * output to the kernel log ----------------------------------------------------------- */
static char run_list[256];
static term_t run_term;
static void run_thread(void *arg) {
    (void)arg;
    char *s = run_list;
    while (*s) {
        char *e = s;
        while (*e && *e != ',') e++;
        char path[96];
        usize n = MIN((usize)(e - s), sizeof path - 1);
        memcpy(path, s, n); path[n] = 0;
        s = *e ? e + 1 : e;
        char err[128];
        const char *argv[] = { path };
        klog("run: %s", path);
        u64 t0 = k_now_us();
        proc_t *p = proc_spawn(path, 1, argv, &run_term, err, sizeof err);
        if (!p) { klog("run: %s: %s", path, err); continue; }
        while (!p->exited) thr_sleep_us(10000);
        usize from = 0;                                           /* its output, line by line */
        for (usize i = 0; i <= run_term.len; i++)
            if (i == run_term.len || run_term.buf[i] == '\n') {
                if (i > from) {
                    char ln[200];
                    usize n = MIN(i - from, sizeof ln - 1);
                    memcpy(ln, run_term.buf + from, n); ln[n] = 0;
                    klog("%s| %s", p->name, ln);
                }
                from = i + 1;
            }
        run_term.len = 0;
        klog("run: %s exited with %d after %llu ms", path, p->exit_code, (k_now_us() - t0) / 1000);
    }
    klog("run: done");
}
void arm_run_programs(const char *list) {
    usize n = 0;
    while (list[n] && list[n] != ' ' && n < sizeof run_list - 1) { run_list[n] = list[n]; n++; }
    run_list[n] = 0;
    thr_create("run", run_thread, NULL, 32 << 10);
}
