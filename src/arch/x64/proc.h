/* proc.h - user processes: Linux x86-64 static binaries in ring 3. */
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

enum { F_NONE, F_FILE, F_DIR, F_TTY, F_NULL };
typedef struct {
    int type;
    vnode_t *vn;
    u64 off;
    int flags;
    int dir_index;
} ufile_t;

#define MAX_FDS  32
#define MAX_VMAS 64

typedef struct { u64 start, end; } vma_t;

typedef struct proc {
    int pid;
    char name[32];
    char exe[128];
    u64 cr3;
    thread_t *th;
    term_t *term;
    u64 entry, sp;
    u64 brk_start, brk;
    u64 mmap_next;
    vma_t vma[MAX_VMAS];
    int nvma;
    ufile_t fd[MAX_FDS];
    char cwd[128];
    volatile int exited;
    int exit_code;
    u64 syscalls;
    int killed;
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
void    proc_init(void);
