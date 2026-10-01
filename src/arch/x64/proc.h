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

enum { F_NONE, F_FILE, F_DIR, F_TTY, F_NULL, F_SOCK, F_PIPE };
struct upipe;
typedef struct {
    int type;
    vnode_t *vn;
    u64 off;
    int flags;
    int dir_index;
    int sock;                     /* F_SOCK: index into lsock.c's table */
    struct upipe *pipe;           /* F_PIPE: the pipe; flags & 1 = the write end */
    int cloexec;                  /* FD_CLOEXEC: closed by execve */
} ufile_t;

#define MAX_FDS  32
#define MAX_VMAS 512

typedef struct { u64 start, end; } vma_t;

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
    vma_t vma[MAX_VMAS];
    int nvma;
    ufile_t fd[MAX_FDS];
    char cwd[128];
    volatile int exited;
    int exit_code;
    u64 syscalls;
    volatile int killed;
    volatile int exec_done;       /* vfork: the parent waits for exec or exit */
    int reaped;                   /* wait4 collected it */
    int sig;                      /* killed by this signal (0: exited) */
} proc_t;

#define USER_STACK_TOP   USER_TOP
#define USER_STACK_SIZE  (8ull << 20)
#define USER_MMAP_BASE   0x20000000ull
#define USER_MMAP_END    (USER_TOP - USER_STACK_SIZE - (1ull << 20))
#define USER_PIE_BASE    0x10000000ull

int     proc_user_supported(const char **why);
proc_t *proc_spawn(const char *path, int argc, const char *const *argv, term_t *term, char *err, usize errcap);
void    proc_kill(proc_t *p);
proc_t *proc_current(void);
void    proc_exit(int code);                      /* current process; never returns */
int     proc_user_ok(proc_t *p, u64 addr, u64 len);/* validate + fault in; 1 if accessible */
int     proc_add_vma(proc_t *p, u64 start, u64 end);
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
