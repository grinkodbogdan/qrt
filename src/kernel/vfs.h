/* vfs.h - the file tree QRT's apps and Linux programs see.
 *
 * Before the firmware exits, the boot volume is copied into RAM; in native
 * mode that copy (plus generated /proc and /etc files) is the file system.
 * Writes stay in RAM until a native storage driver lands. */
#pragma once
#include "kernel.h"

typedef struct vnode {
    char name[64];
    int dir;
    u8 *data;
    u64 size, cap;
    int (*gen)(char *buf, int cap);   /* synthetic file: contents produced on open */
    struct vnode *parent, *child, *sibling;
} vnode_t;

void     vfs_load_boot_volume(void);      /* under UEFI: copy the boot volume into RAM */
void     vfs_relocate(void);              /* native: move everything into the kernel heap */
vnode_t *vfs_root(void);
vnode_t *vfs_lookup(const char *path);    /* absolute path; NULL if missing */
vnode_t *vfs_create(const char *path, int dir);
int      vfs_unlink(const char *path, int dir);   /* 0 or -errno (rmdir with dir) */
int      vfs_rename(const char *from, const char *to);
vnode_t *vfs_child_at(vnode_t *dir, int index);
int      vfs_children(vnode_t *dir);
u64      vfs_size(vnode_t *n);
i64      vfs_read(vnode_t *n, u64 off, void *buf, u64 len);
i64      vfs_write(vnode_t *n, u64 off, const void *buf, u64 len);
void     vfs_truncate(vnode_t *n);
void     vfs_path(vnode_t *n, char *out, usize cap);
u64      vfs_total_bytes(void);
