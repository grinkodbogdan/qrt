/*
 * qrtcall.c - the system calls only native QRT programs have (numbers 1024 and up in
 * sdk/syscalls.txt): windows in the QRT shell and their input.  sdk/libqrt wraps them.
 */
#include "proc.h"
#include "lfile.h"
#include "native_sys.h"
#include "../../ui/clientwin.h"

enum { EINTR = 4, EFAULT = 14, EINVAL = 22, ENOSYS = 38 };
#define UOK(a, n) proc_user_ok(p, (u64)(a), (u64)(n))

static int get_str(proc_t *p, u64 u, char *out, usize cap) {
    if (!u) { out[0] = 0; return 0; }
    for (usize i = 0; i < cap; i++) {
        if (!UOK(u + i, 1)) return -EFAULT;
        out[i] = ((const char *)(usize)u)[i];
        if (!out[i]) return 0;
    }
    out[cap - 1] = 0;
    return 0;
}

static i64 event_wait(proc_t *p, u64 uev, i64 timeout_ms) {
    if (!UOK(uev, sizeof(qrt_event_t))) return -EFAULT;
    u64 end = timeout_ms < 0 ? ~0ull : k_now_ms() + (u64)timeout_ms;
    for (;;) {
        qrt_event_t e;
        if (cw_next_event(p, &e)) { memcpy((void *)(usize)uev, &e, sizeof e); return 1; }
        if (k_now_ms() >= end) return 0;
        if (proc_interrupted(p)) return -EINTR;
        thread_sleep_ms(2);
    }
}

i64 qrt_call(proc_t *p, u64 nr, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4) {
    char s[64];
    switch (nr) {
    case QRT_SYS_WINDOW_CREATE:
        if (get_str(p, a0, s, sizeof s)) return -EFAULT;
        return cw_create(p, s);
    case QRT_SYS_WINDOW_MAP: {                       /* (id, info*): info = { addr, width, height, stride (pixels), pad } */
        if (!UOK(a1, 24)) return -EFAULT;
        kobj_t *o;
        int w, h, stride;
        u64 bytes;
        int e = cw_buffer(p, (int)a0, &o, &w, &h, &stride, &bytes);
        if (e) return e;
        i64 va = map_shared(p, o, bytes);
        kobj_put(o);                                 /* the mapping holds its own reference */
        if (va < 0) return va;
        u64 *info = (u64 *)(usize)a1;
        info[0] = (u64)va;
        ((i32 *)(info + 1))[0] = w;
        ((i32 *)(info + 1))[1] = h;
        ((i32 *)(info + 2))[0] = stride;
        ((i32 *)(info + 2))[1] = 0;
        return 0;
    }
    case QRT_SYS_WINDOW_PRESENT: return cw_present(p, (int)a0, (int)a1, (int)a2, (int)a3, (int)a4);
    case QRT_SYS_WINDOW_CLOSE: return cw_close(p, (int)a0);
    case QRT_SYS_EVENT_WAIT: return event_wait(p, a0, (i64)(i32)a1);
    case QRT_SYS_WINDOW_TITLE:
        if (get_str(p, a1, s, sizeof s)) return -EFAULT;
        return cw_title(p, (int)a0, s);
    case QRT_SYS_KEYBOARD: cw_keyboard(p, a0 != 0); return 0;
    case QRT_SYS_EVENT_FD: return -ENOSYS;           /* reserved: an fd that polls readable with events */
    }
    return -ENOSYS;
}

void qrt_proc_gone(proc_t *p) { cw_proc_gone(p); }
