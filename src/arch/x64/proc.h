/* proc.h - user processes: Linux x86-64 programs in ring 3 (static or with ld.so, threads). */
#pragma once
#include "sched.h"
#include "../../kernel/vfs.h"

/* Output of a process goes to a terminal buffer the UI renders. */
typedef struct {
    char *buf;
    usize len, cap;
    volatile u32 serial;          /* bumped on every append */
    int col, esc;                 /* cursor column (tab stops), escape-sequence state */
} term_t;
void term_append(term_t *t, const char *s, usize n);

enum { F_NONE, F_FILE, F_DIR, F_TTY, F_NULL, F_SOCK, F_PIPE, F_OBJ, F_RESV /* taken, being filled in */ };
struct upipe;

/* Kernel objects behind descriptors and mappings, shared by every process that
 * holds one (dup, fork, SCM_RIGHTS): reference counted, freed by kobj_put. */
enum { KO_SHM = 1, KO_EVENTFD, KO_TIMERFD, KO_SIGNALFD, KO_EPOLL, KO_UNIX, KO_DSP };
typedef struct kobj { int kind; volatile int refs; } kobj_t;
void kobj_get(kobj_t *o);
void kobj_put(kobj_t *o);
typedef struct {
    int type;
    vnode_t *vn;
    u64 off;
    int flags;
    int dir_index;
    int sock;                     /* F_SOCK: index into lsock.c's table */
    struct upipe *pipe;           /* F_PIPE: the pipe; flags & 1 = the write end */
    kobj_t *obj;                  /* F_OBJ: memfd, eventfd, timerfd, signalfd, epoll, Unix socket */
    int cloexec;                  /* FD_CLOEXEC: closed by execve */
} ufile_t;

#define MAX_FDS  1024

/* PROT_* as Linux numbers them */
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
typedef struct {
    u64 start, end;
    u32 prot;                     /* PROT_*; 0 = a reservation (any access faults) */
    u32 shared;                   /* backed by obj (MAP_SHARED / memfd): pages are the object's */
    kobj_t *obj;
    u64 off;                      /* the object offset of 'start' */
} vma_t;

/* signals */
#define NSIG 65
typedef struct { u64 handler, flags, restorer, mask; } ksigaction_t;
typedef struct { int code, pid, status; u64 addr; } ksiginfo_t;

typedef struct proc {
    int pid, ppid;
    char name[32];
    char exe[128];
    u64 cr3;
    thread_t *th;
    term_t *term;
    u64 entry, sp;                /* the program's entry point; initial stack */
    u64 start;                    /* where the first thread starts: entry, or ld.so's */
    u64 interp_base;              /* AT_BASE: where ld.so was loaded (0: static) */
    int nthreads;                 /* live threads */
    u64 brk_start, brk;
    u64 mmap_next;
    vma_t *vma;                   /* sorted by address (proc.c); grows as needed */
    int nvma, vcap;
    ufile_t fd[MAX_FDS];
    char cwd[128];
    volatile int exited;
    int exit_code;
    u64 syscalls;
    volatile int killed;
    volatile int exec_done;       /* vfork: the parent waits for exec or exit */
    int reaped;                   /* wait4 collected it */
    int sig;                      /* killed by this signal (0: exited) */
    ksigaction_t sa[NSIG];        /* handlers (shared by the threads) */
    u64 sig_pending;              /* signals for any thread */
    ksiginfo_t sig_info[NSIG];    /* who sent each pending signal */
    u64 alarm_us, alarm_every_us; /* ITIMER_REAL: next SIGALRM (k_now_us), period */
    int native;                   /* a native QRT program (QRT system-call numbers), not a Linux one */
    kobj_t *qrt_events;           /* QRT_SYS_EVENT_FD: an eventfd signalled when a window event is queued */
    int trace;                    /* QRT_TRACE=1 in its environment: failing system calls go to the kernel log */
    volatile int exiting;         /* its last thread is tearing it down: kill() leaves it alone */
} proc_t;

#define USER_STACK_TOP   USER_TOP
#define USER_STACK_SIZE  (8ull << 20)
#define USER_MMAP_BASE   USER_HIGH_BASE            /* mmap: the high region */
#define USER_MMAP_END    (USER_HIGH_END - (1ull << 30))
#define USER_LOW_MMAP    0x30000000ull             /* MAP_32BIT and low hints: [768 MiB, the stack) */
#define USER_BRK_MAX     USER_LOW_MMAP
#define USER_PIE_BASE    0x10000000ull

int     proc_user_supported(const char **why);
proc_t *proc_spawn(const char *path, int argc, const char *const *argv, term_t *term, char *err, usize errcap);
void    proc_kill(proc_t *p);
proc_t *proc_current(void);
void    proc_exit(int code);                      /* current process; never returns */
int     proc_user_ok(proc_t *p, u64 addr, u64 len);/* validate + fault in; 1 if accessible */
int     proc_add_vma(proc_t *p, u64 start, u64 end);      /* private, read-write */
int     proc_add_vma_prot(proc_t *p, u64 start, u64 end, u32 prot, kobj_t *obj, u64 off);   /* takes a reference to obj */
vma_t  *proc_vma(proc_t *p, u64 a);
int     proc_range_mapped(proc_t *p, u64 start, u64 end);    /* any VMA in [start, end) */
u64     proc_vma_lock(void);                                  /* the VMA table's lock (nests); returns what to unlock with */
void    proc_vma_unlock(u64 fl);
int     proc_add_vma_flags(proc_t *p, u64 start, u64 end, u32 prot, kobj_t *obj, u64 off, u32 flags);
void    proc_vmas_copy(proc_t *c, const proc_t *p);
i64     proc_protect(proc_t *p, u64 start, u64 end, u32 prot);
void    proc_vmas_release(proc_t *p);                     /* drop the objects mapped (exit, exec) */
u64     proc_find_free_low(proc_t *p, u64 len);
u64     proc_find_free(proc_t *p, u64 len);      /* a free range in the mmap window, 0 if none */
int     proc_range_free(proc_t *p, u64 start, u64 end);
void    proc_unmap(proc_t *p, u64 start, u64 end);  /* drop pages and VMAs in [start, end) */
i64     proc_clone(proc_t *p, frame_t *f, u64 flags, u64 newsp, u64 ptid, u64 ctid, u64 tls);
void    proc_thread_exit(int code);              /* this thread only; the last one ends the process */
i64     proc_exec(proc_t *p, frame_t *f, const char *path, char **argv, int argc, char **envp, int envc);
i64     proc_wait(proc_t *p, int pid, u64 ustatus, int options);
i64     proc_signal(proc_t *p, int pid, int sig);
proc_t *proc_by_pid(int pid);
/* linux.c: file descriptors */
void    fd_release(proc_t *p, int fd);           /* close, dropping socket/pipe references */
void    fd_addref(proc_t *p, int fd);            /* a copy of the descriptor now exists (dup, fork) */
void    fds_release_all(proc_t *p);
void    proc_init(void);
void    proc_cpu_setup(void);                     /* SYSCALL and paging bits of the calling core */
int     proc_interrupted(proc_t *p);              /* a blocking call should stop: -EINTR (killed, or a signal to handle) */
proc_t *proc_at(int i);                           /* every process ever started, NULL past the end */
thread_t *proc_thread(proc_t *p, int tid);        /* a thread of p by tid (0: any live one) */

/* qrtcall.c: the system calls only native QRT programs have (windows, input) */
i64     qrt_call(proc_t *p, u64 nr, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4);
void    qrt_proc_gone(proc_t *p);                 /* its windows close */

/* signal.c */
void    sig_init(void);
i64     sig_action(proc_t *p, int sig, u64 act, u64 old, u64 size);
i64     sig_procmask(proc_t *p, int how, u64 set, u64 old, u64 size);
i64     sig_return(proc_t *p, frame_t *f);
i64     sig_altstack(proc_t *p, u64 ss, u64 old);
i64     sig_suspend(proc_t *p, u64 mask, u64 size);
i64     sig_timedwait(proc_t *p, u64 set, u64 info, u64 ts, u64 size);
i64     sig_pending_set(proc_t *p, u64 set, u64 size);
i64     sig_kill_thread(proc_t *p, int tgid, int tid, int sig);
i64     sig_setitimer(proc_t *p, int which, u64 nv, u64 ov);
i64     sig_getitimer(proc_t *p, int which, u64 cur);
i64     sig_alarm(proc_t *p, u64 seconds);
void    sig_post(proc_t *p, thread_t *t, int sig, int code, int pid, u64 addr);  /* t NULL: the process */
void    sig_deliver_pending(proc_t *p, frame_t *f, u64 nr, u64 entry_nr, i64 *ret);   /* before returning to user mode; entry_nr: what RAX held */
int     sig_fault(proc_t *p, frame_t *f);                                     /* 1: a handler takes the fault */
int     sig_take(proc_t *p, u64 mask, ksiginfo_t *info);                      /* dequeue one of mask (signalfd, sigtimedwait) */
u64     sig_deliverable(proc_t *p);
