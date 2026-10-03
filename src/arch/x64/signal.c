/*
 * signal.c - POSIX signals for the Linux layer, as Linux x86-64 delivers them.
 *
 * Handlers (rt_sigaction), per-thread masks (rt_sigprocmask), the alternate
 * stack (sigaltstack), and delivery: whenever a thread is about to return to
 * user mode - at the end of a system call, after an interrupt (so a busy loop
 * gets its SIGALRM), or on a fault (SIGSEGV, SIGILL, SIGFPE, SIGTRAP, SIGBUS) -
 * a pending, unblocked signal with a handler gets an rt_sigframe on the user
 * stack: the return address (the libc's restorer, which calls rt_sigreturn),
 * a ucontext (registers in Linux's sigcontext order, the old mask, a pointer
 * to the FXSAVE image of the FPU/SSE state) and a siginfo.  glibc and musl
 * read and write those structures directly, so the layout is Linux's to the
 * byte.  System calls interrupted by a handler with SA_RESTART start again.
 * Blocking calls notice pending signals through proc_interrupted().
 *
 * All user threads run on the boot core: the signal state is protected by
 * disabling interrupts.
 */
#include "proc.h"
#include "mm.h"

enum { ESRCH = 3, EINTR = 4, EAGAIN = 11, ENOMEM = 12, EFAULT = 14, EINVAL = 22 };

#define SA_SIGINFO   0x00000004ull
#define SA_RESTORER  0x04000000ull
#define SA_ONSTACK   0x08000000ull
#define SA_RESTART   0x10000000ull
#define SA_NODEFER   0x40000000ull
#define SA_RESETHAND 0x80000000ull
#define SS_ONSTACK   1
#define SS_DISABLE   2
#define SIGKILL 9
#define SIGSTOP 19
#define BIT(s)  (1ull << ((s) - 1))
#define UNBLOCKABLE (BIT(SIGKILL) | BIT(SIGSTOP))
#define UOK(a, n) proc_user_ok(p, (u64)(a), (u64)(n))

typedef struct {
    u64 r8, r9, r10, r11, r12, r13, r14, r15, rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags;
    u16 cs, gs, fs, ss;
    u64 err, trapno, oldmask, cr2, fpstate;
    u64 reserved1[8];
} sigcontext_t;
typedef struct { u64 ss_sp; i32 ss_flags, pad; u64 ss_size; } ustack_t;
typedef struct { u64 uc_flags, uc_link; ustack_t uc_stack; sigcontext_t mc; u64 sigmask; } ucontext_k_t;
typedef struct { u64 pretcode; ucontext_k_t uc; u8 info[128]; } rt_frame_t;
_Static_assert(sizeof(sigcontext_t) == 256, "sigcontext");
_Static_assert(__builtin_offsetof(ucontext_k_t, sigmask) == 296, "ucontext");

extern void (*user_return_hook)(frame_t *f);
extern int (*user_return_check)(void);

static int default_ignored(int sig) { return sig == 17 || sig == 18 || sig == 23 || sig == 28 || (sig >= 19 && sig <= 22); }
static int ignored(proc_t *p, int sig) { u64 h = p->sa[sig].handler; return h == 1 || (h == 0 && default_ignored(sig)); }

/* the FXSAVE image the entry code made just below the frame (syscall and interrupt paths alike) */
static u8 *fx_area(frame_t *f) { return (u8 *)(usize)((((u64)(usize)f) - 528) & ~15ull); }

/* ---- timers: ITIMER_REAL / alarm ---------------------------------------------------- */
static void check_timers(proc_t *p) {
    if (!p->alarm_us || k_now_us() < p->alarm_us) return;
    p->alarm_us = p->alarm_every_us ? k_now_us() + p->alarm_every_us : 0;
    sig_post(p, NULL, 14, 0x80, 0, 0);                        /* SIGALRM, SI_KERNEL */
}

/* ---- posting and taking ------------------------------------------------------------------ */
void sig_post(proc_t *p, thread_t *t, int sig, int code, int pid, u64 val) {
    if (sig <= 0 || sig >= NSIG || p->exited) return;
    u64 fl = irq_save();
    if (t) t->sig_pending |= BIT(sig); else p->sig_pending |= BIT(sig);
    p->sig_info[sig] = (ksiginfo_t){ code, pid, (int)val, val };
    irq_restore(fl);
    /* sleeping threads look again (blocking calls return -EINTR) */
    thread_t *all[64];
    int n = sched_threads(all, 64);
    for (int i = 0; i < n; i++)
        if (all[i]->proc == p && (!t || all[i] == t) && !(all[i]->sig_mask & BIT(sig))) thread_wake(all[i]);
}

u64 sig_deliverable(proc_t *p) {
    thread_t *t = thread_current();
    if (!p || !t || t->proc != p) return 0;
    check_timers(p);
    u64 fl = irq_save();
    u64 pend = (t->sig_pending | p->sig_pending) & ~t->sig_mask;
    /* unblocked signals that would only be ignored are dropped here */
    for (u64 m = pend; m; m &= m - 1) {
        int s = __builtin_ctzll(m) + 1;
        if (ignored(p, s)) { t->sig_pending &= ~BIT(s); p->sig_pending &= ~BIT(s); pend &= ~BIT(s); }
    }
    irq_restore(fl);
    return pend;
}

int sig_take(proc_t *p, u64 mask, ksiginfo_t *info) {
    thread_t *t = thread_current();
    u64 fl = irq_save();
    u64 pend = (t->sig_pending | p->sig_pending) & mask;
    if (!pend) { irq_restore(fl); return 0; }
    int s = __builtin_ctzll(pend) + 1;
    if (t->sig_pending & BIT(s)) t->sig_pending &= ~BIT(s); else p->sig_pending &= ~BIT(s);
    if (info) *info = p->sig_info[s];
    irq_restore(fl);
    return s;
}

static void fill_info(u8 *out, int sig, const ksiginfo_t *in) {
    memset(out, 0, 128);
    *(i32 *)out = sig;
    *(i32 *)(out + 8) = in->code;
    if (sig == 11 || sig == 7 || sig == 4 || sig == 8 || sig == 5) *(u64 *)(out + 16) = in->addr;   /* si_addr */
    else {
        *(i32 *)(out + 16) = in->pid;                                   /* si_pid, si_uid = 0 */
        if (sig == 17) *(i32 *)(out + 24) = in->status;                 /* si_status */
    }
}

/* ---- delivery ------------------------------------------------------------------------------ */
static int on_altstack(thread_t *t, u64 sp) { return t->alt_size && sp > t->alt_sp && sp <= t->alt_sp + t->alt_size; }

/* build the rt_sigframe and point the thread at the handler; 0 if the stack is unusable */
static int setup_frame(proc_t *p, frame_t *f, int sig, const ksiginfo_t *info, u64 trapno, u64 cr2) {
    thread_t *t = thread_current();
    ksigaction_t *sa = &p->sa[sig];
    u64 sp = f->rsp;
    if ((sa->flags & SA_ONSTACK) && t->alt_size && !(t->alt_flags & SS_DISABLE) && !on_altstack(t, sp)) sp = t->alt_sp + t->alt_size;
    else sp -= 128;                                                     /* the red zone */
    sp = (sp - 512) & ~63ull;
    u64 fp = sp;
    sp = ((sp - sizeof(rt_frame_t)) & ~15ull) - 8;                      /* rsp % 16 == 8 at the handler, as after a call */
    if (!(sa->flags & SA_RESTORER) || !UOK(sp, fp + 512 - sp)) return 0;
    memcpy((void *)(usize)fp, fx_area(f), 512);
    rt_frame_t *fr = (rt_frame_t *)(usize)sp;
    memset(fr, 0, sizeof *fr);
    fr->pretcode = sa->restorer;
    fr->uc.uc_stack = (ustack_t){ t->alt_sp, t->alt_size ? (on_altstack(t, f->rsp) ? SS_ONSTACK : 0) : SS_DISABLE, 0, t->alt_size };
    sigcontext_t *m = &fr->uc.mc;
    m->r8 = f->r8; m->r9 = f->r9; m->r10 = f->r10; m->r11 = f->r11; m->r12 = f->r12; m->r13 = f->r13;
    m->r14 = f->r14; m->r15 = f->r15; m->rdi = f->rdi; m->rsi = f->rsi; m->rbp = f->rbp; m->rbx = f->rbx;
    m->rdx = f->rdx; m->rax = f->rax; m->rcx = f->rcx; m->rsp = f->rsp; m->rip = f->rip; m->eflags = f->rflags;
    m->cs = (u16)f->cs; m->ss = (u16)f->ss;
    m->err = trapno == 14 ? f->err : 0; m->trapno = trapno; m->cr2 = cr2;
    m->fpstate = fp;
    u64 old = t->sig_suspended ? t->sig_saved_mask : t->sig_mask;
    t->sig_suspended = 0;
    m->oldmask = old;
    fr->uc.sigmask = old;
    fill_info(fr->info, sig, info);
    /* the handler runs with its mask and on its stack */
    t->sig_mask |= (sa->mask | ((sa->flags & SA_NODEFER) ? 0 : BIT(sig))) & ~UNBLOCKABLE;
    f->rsp = sp;
    f->rip = sa->handler;
    f->rdi = (u64)sig;
    f->rsi = sp + __builtin_offsetof(rt_frame_t, info);
    f->rdx = sp + __builtin_offsetof(rt_frame_t, uc);
    f->rax = 0;
    f->rflags &= ~(0x100ull | 0x400ull | 0x40000ull);                   /* TF, DF, AC */
    if (sa->flags & SA_RESETHAND) { sa->handler = 0; sa->flags &= ~SA_SIGINFO; }
    return 1;
}

static void die(proc_t *p, int sig) {
    p->sig = sig;
    thread_current()->in_sys = 0;
    proc_exit(128 + sig);
}

static int restartable(u64 nr) {
    switch (nr) {
    case 7: case 23: case 270: case 271: case 232: case 281: case 441:    /* poll, select, epoll_wait */
    case 35: case 230: case 34: case 130: case 128: case 15:             /* sleeps, pause, sigsuspend, sigtimedwait */
        return 0;
    }
    return 1;
}

/* nr: the system call that is returning (ret its result), or ~0 after an interrupt */
void sig_deliver_pending(proc_t *p, frame_t *f, u64 nr, u64 entry_nr, i64 *ret) {
    thread_t *t = thread_current();
    u64 pend = sig_deliverable(p);
    while (pend) {
        int s = __builtin_ctzll(pend) + 1;
        pend &= ~BIT(s);
        ksiginfo_t info;
        if (!sig_take(p, BIT(s), &info)) continue;
        u64 h = p->sa[s].handler;
        if (h == 1 || (h == 0 && default_ignored(s))) continue;
        if (h == 0) die(p, s);                                           /* default: end the process */
        if (ret && *ret == -EINTR && nr != ~0ull && restartable(nr) && (p->sa[s].flags & SA_RESTART)) {
            f->rax = entry_nr;                                           /* run the call again after the handler */
            f->rip -= 2;
        }
        if (!setup_frame(p, f, s, &info, 0, 0)) die(p, 11);
        break;                                                           /* one frame; the next at the next return */
    }
    if (t->sig_suspended) { t->sig_mask = t->sig_saved_mask; t->sig_suspended = 0; }
}

static void on_user_return(frame_t *f) {
    proc_t *p = proc_current();
    if (!p || p->killed || p->exited) return;
    thread_t *t = thread_current();
    if (!t || t->state == T_DEAD) return;
    if (sig_deliverable(p)) sig_deliver_pending(p, f, ~0ull, ~0ull, NULL);
}

/* a CPU exception in user mode: the program's handler, if it has one that is not blocked */
int sig_fault(proc_t *p, frame_t *f) {
    int s, code;
    u64 addr = f->rip;
    switch (f->vector) {
    case 14: { s = 11; addr = read_cr2(); u64 fl = proc_vma_lock(); code = proc_vma(p, addr) ? 2 : 1; proc_vma_unlock(fl); break; }   /* SEGV_ACCERR / SEGV_MAPERR */
    case 13: s = 11; addr = 0; code = 0x80; break;
    case 6:  s = 4; code = 2; break;                                     /* ILL_ILLOPN */
    case 0:  s = 8; code = 1; break;                                     /* FPE_INTDIV */
    case 16: case 19: s = 8; code = 0; break;
    case 1: case 3: s = 5; code = 1; break;                              /* SIGTRAP */
    case 17: s = 7; code = 1; break;                                     /* SIGBUS */
    default: return 0;
    }
    thread_t *t = thread_current();
    if (p->sa[s].handler <= 1 || (t->sig_mask & BIT(s))) {             /* default (or blocked): it dies */
        /* say where: the instruction, the address, and what is mapped there */
        u64 vfl = proc_vma_lock();
        vma_t *v = f->vector == 14 ? proc_vma(p, addr) : NULL;
        u32 vprot = v ? v->prot : 0;
        proc_vma_unlock(vfl);
        klog("proc: %s (pid %d) %s at %llx (rsp %llx), address %llx%s%s", p->name, p->pid,
             s == 11 ? "SIGSEGV" : s == 4 ? "SIGILL" : s == 8 ? "SIGFPE" : s == 7 ? "SIGBUS" : "SIGTRAP",
             f->rip, f->rsp, addr, f->vector == 14 ? (v ? ", mapped " : ", not mapped") : "",
             v ? (vprot & PROT_WRITE ? "rw" : vprot & PROT_READ ? "r" : "none") : "");
        p->sig = s;
        return 0;
    }
    /* the program catches it (Ladybird prints "CRASH" and exits): say where, a few times */
    static int told;
    if (told < 8 && s != 5) {
        told++;
        u64 vfl = proc_vma_lock();
        vma_t *v = f->vector == 14 ? proc_vma(p, addr) : NULL;
        u32 vprot = v ? v->prot : 0;
        proc_vma_unlock(vfl);
        klog("proc: %s (pid %d, thread %d) fault %llu (err %llx) at %llx, address %llx%s%s - to its handler", p->name, p->pid, t->tid,
             f->vector, f->err, f->rip, addr, f->vector == 14 ? (v ? ", mapped " : ", not mapped") : "",
             v ? (vprot & PROT_WRITE ? "rw" : vprot & PROT_READ ? "r" : "none") : "");
    }
    ksiginfo_t info = { code, 0, 0, addr };
    if (!setup_frame(p, f, s, &info, f->vector, f->vector == 14 ? addr : 0)) { p->sig = 11; return 0; }
    return 1;
}

/* ---- system calls -------------------------------------------------------------------------- */
i64 sig_action(proc_t *p, int sig, u64 act, u64 old, u64 size) {
    if (size != 8 || sig <= 0 || sig >= NSIG) return -EINVAL;
    if (old) { if (!UOK(old, sizeof(ksigaction_t))) return -EFAULT; memcpy((void *)(usize)old, &p->sa[sig], sizeof(ksigaction_t)); }
    if (!act) return 0;
    if (sig == SIGKILL || sig == SIGSTOP) return -EINVAL;
    if (!UOK(act, sizeof(ksigaction_t))) return -EFAULT;
    u64 fl = irq_save();
    memcpy(&p->sa[sig], (void *)(usize)act, sizeof(ksigaction_t));
    p->sa[sig].mask &= ~UNBLOCKABLE;
    if (ignored(p, sig)) p->sig_pending &= ~BIT(sig);                     /* now ignored: forget it */
    irq_restore(fl);
    return 0;
}

i64 sig_procmask(proc_t *p, int how, u64 set, u64 old, u64 size) {
    thread_t *t = thread_current();
    if (size != 8) return -EINVAL;
    if (old) { if (!UOK(old, 8)) return -EFAULT; *(u64 *)(usize)old = t->sig_mask; }
    if (!set) return 0;
    if (!UOK(set, 8)) return -EFAULT;
    u64 s = *(u64 *)(usize)set & ~UNBLOCKABLE;
    if (how == 0) t->sig_mask |= s;
    else if (how == 1) t->sig_mask &= ~s;
    else if (how == 2) t->sig_mask = s;
    else return -EINVAL;
    return 0;
}

i64 sig_return(proc_t *p, frame_t *f) {
    thread_t *t = thread_current();
    u64 ucp = f->rsp;                                                    /* the restorer's 'ret' popped pretcode */
    if (!UOK(ucp, sizeof(ucontext_k_t))) die(p, 11);
    ucontext_k_t *uc = (ucontext_k_t *)(usize)ucp;
    sigcontext_t *m = &uc->mc;
    if (!user_va(m->rip) || !user_va(m->rsp - 1)) die(p, 11);
    f->r8 = m->r8; f->r9 = m->r9; f->r10 = m->r10; f->r11 = m->r11; f->r12 = m->r12; f->r13 = m->r13;
    f->r14 = m->r14; f->r15 = m->r15; f->rdi = m->rdi; f->rsi = m->rsi; f->rbp = m->rbp; f->rbx = m->rbx;
    f->rdx = m->rdx; f->rax = m->rax; f->rcx = m->rcx; f->rsp = m->rsp; f->rip = m->rip;
    f->rflags = (m->eflags & 0x3f7fd5ull) | 0x202ull;                   /* user-settable flags, interrupts on */
    f->cs = 0x2b; f->ss = 0x23;
    if (m->fpstate && UOK(m->fpstate, 512)) {
        u8 *fx = fx_area(f);
        memcpy(fx, (void *)(usize)m->fpstate, 512);
        *(u32 *)(fx + 24) &= 0xffff;                                     /* MXCSR: no reserved bits (FXRSTOR would #GP) */
    }
    t->sig_mask = uc->sigmask & ~UNBLOCKABLE;
    return (i64)f->rax;
}

i64 sig_altstack(proc_t *p, u64 ss, u64 old) {
    thread_t *t = thread_current();
    if (old) {
        if (!UOK(old, sizeof(ustack_t))) return -EFAULT;
        ustack_t *o = (ustack_t *)(usize)old;
        o->ss_sp = t->alt_sp; o->ss_size = t->alt_size; o->pad = 0;
        o->ss_flags = !t->alt_size ? SS_DISABLE : 0;
    }
    if (!ss) return 0;
    if (!UOK(ss, sizeof(ustack_t))) return -EFAULT;
    ustack_t *n = (ustack_t *)(usize)ss;
    if (n->ss_flags & SS_DISABLE) { t->alt_sp = t->alt_size = 0; t->alt_flags = 0; return 0; }
    if (n->ss_flags & ~(SS_ONSTACK | (1 << 31))) return -EINVAL;
    if (n->ss_size < 2048) return -ENOMEM;
    t->alt_sp = n->ss_sp; t->alt_size = n->ss_size; t->alt_flags = 0;
    return 0;
}

/* rt_sigsuspend and pause: wait with a temporary mask until a handler is due */
i64 sig_suspend(proc_t *p, u64 mask, u64 size) {
    thread_t *t = thread_current();
    u64 m = t->sig_mask;
    if (mask) {
        if (size != 8 || !UOK(mask, 8)) return -EINVAL;
        m = *(u64 *)(usize)mask & ~UNBLOCKABLE;
    }
    t->sig_saved_mask = t->sig_mask;
    t->sig_suspended = 1;
    t->sig_mask = m;
    while (!proc_interrupted(p)) thread_sleep_ms(20);
    return -EINTR;                                                       /* the mask comes back after the handler */
}

i64 sig_timedwait(proc_t *p, u64 set, u64 info, u64 ts, u64 size) {
    if (size != 8 || !UOK(set, 8)) return -EINVAL;
    u64 want = *(u64 *)(usize)set & ~UNBLOCKABLE;
    u64 end = ~0ull;
    if (ts) {
        if (!UOK(ts, 16)) return -EFAULT;
        const u64 *t2 = (const u64 *)(usize)ts;
        end = k_now_us() + t2[0] * 1000000 + t2[1] / 1000;
    }
    for (;;) {
        ksiginfo_t in;
        check_timers(p);
        int s = sig_take(p, want, &in);
        if (s) {
            if (info) { if (!UOK(info, 128)) return -EFAULT; fill_info((u8 *)(usize)info, s, &in); }
            return s;
        }
        if (proc_interrupted(p)) return -EINTR;
        if (k_now_us() >= end) return -EAGAIN;
        thread_sleep_ms(ts ? MIN((end - k_now_us()) / 1000 + 1, (u64)10) : 10);
    }
}

i64 sig_pending_set(proc_t *p, u64 set, u64 size) {
    thread_t *t = thread_current();
    if (size > 8 || !UOK(set, 8)) return -EFAULT;
    *(u64 *)(usize)set = (t->sig_pending | p->sig_pending) & t->sig_mask;
    return 0;
}

/* tgkill / tkill: to one thread */
i64 sig_kill_thread(proc_t *p, int tgid, int tid, int sig) {
    if (sig < 0 || sig >= NSIG || tid <= 0) return -EINVAL;
    for (int i = 0;; i++) {
        proc_t *q = proc_at(i);
        if (!q) break;
        if (q->exited || (tgid > 0 && q->pid != tgid)) continue;
        thread_t *t = proc_thread(q, tid);
        if (!t) continue;
        if (!sig) return 0;
        if (q->sa[sig].handler > 1 || (t->sig_mask & BIT(sig))) { sig_post(q, t, sig, -6, p->pid, 0); return 0; }   /* SI_TKILL */
        return proc_signal(p, q->pid, sig);                              /* default or ignore: the process's fate */
    }
    return -ESRCH;
}

static u64 tv_us(const u64 *tv) { return tv[0] * 1000000 + tv[1]; }

i64 sig_getitimer(proc_t *p, int which, u64 cur) {
    if (which != 0) return which <= 2 ? 0 : -EINVAL;                     /* only ITIMER_REAL runs */
    if (!UOK(cur, 32)) return -EFAULT;
    u64 *o = (u64 *)(usize)cur, now = k_now_us();
    u64 left = p->alarm_us > now ? p->alarm_us - now : 0;
    o[0] = p->alarm_every_us / 1000000; o[1] = p->alarm_every_us % 1000000;
    o[2] = left / 1000000; o[3] = left % 1000000;
    return 0;
}

i64 sig_setitimer(proc_t *p, int which, u64 nv, u64 ov) {
    if (which != 0) return which <= 2 ? 0 : -EINVAL;
    if (ov && sig_getitimer(p, 0, ov)) return -EFAULT;
    if (!nv) return 0;
    if (!UOK(nv, 32)) return -EFAULT;
    const u64 *n = (const u64 *)(usize)nv;
    u64 val = tv_us(n + 2);
    p->alarm_every_us = tv_us(n);
    p->alarm_us = val ? k_now_us() + val : 0;
    return 0;
}

i64 sig_alarm(proc_t *p, u64 seconds) {
    u64 now = k_now_us();
    u64 left = p->alarm_us > now ? (p->alarm_us - now + 999999) / 1000000 : 0;
    p->alarm_every_us = 0;
    p->alarm_us = seconds ? now + seconds * 1000000 : 0;
    return (i64)left;
}

/* without the big lock: could on_user_return have anything to do? (most returns to user mode: no) */
static int user_return_needed(void) {
    thread_t *t = thread_current();
    proc_t *p = t ? t->proc : NULL;
    if (!p) return 0;
    if (p->killed || p->exited || t->state == T_DEAD) return 1;
    if (p->alarm_us && k_now_us() >= p->alarm_us) return 1;
    return ((t->sig_pending | p->sig_pending) & ~t->sig_mask) != 0;
}

void sig_init(void) { user_return_hook = on_user_return; user_return_check = user_return_needed; }
