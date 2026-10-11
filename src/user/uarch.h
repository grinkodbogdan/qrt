/* uarch.h - what the user-mode layer (src/user: processes, the Linux system calls) needs
 * from the architecture: threads and the scheduler, page tables, the trap frame. */
#pragma once
#if defined(__aarch64__)
#include "../arch/arm64/uarch.h"
#else
#include "../arch/x64/sched.h"
#include "../arch/x64/mm.h"
#endif

#if !defined(__aarch64__)
#define FRAME_RET(f)       ((f)->rax)
#define FRAME_SP(f)        ((f)->rsp)
#define FRAME_PC(f)        ((f)->rip)
#define USER_LOW_START     PAGE
#endif
