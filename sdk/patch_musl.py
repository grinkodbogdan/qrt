#!/usr/bin/env python3
"""Turn musl into the C library of native QRT programs: its system-call numbers become
QRT's (sdk/syscalls.txt).  musl's C code takes them from bits/syscall.h; six places in its
x86-64 assembly have them written out, and are rewritten here (each must be found).

usage: patch_musl.py sdk/syscalls.txt <musl source dir>"""
import os, sys

rows = [l.split() for l in open(sys.argv[1]) if l.strip() and not l.startswith('#')]
qrt = {name: int(n) for n, name, lin in rows}
musl = sys.argv[2]

# bits/syscall.h.in: every __NR_x with QRT's number (musl's build adds the SYS_x names).
# That header is what musl itself is compiled with.  Programs get the Linux numbers in
# their <sys/syscall.h> instead (written to syscall.public.h, installed by build.sh), and
# the syscall() function translates: code that calls syscall(SYS_futex, ...) directly -
# Rust's standard library, for one - is source compatible, and the kernel still only
# ever sees QRT numbers.
hdr = os.path.join(musl, 'arch/x86_64/bits/syscall.h.in')
orig = open(hdr).read()
linux = {l.split()[1][5:]: int(l.split()[2]) for l in orig.splitlines() if l.startswith('#define __NR_')}
names = list(linux)
with open(os.path.join(musl, 'syscall.public.h'), 'w') as f:
    f.write('/* System-call numbers for syscall(): Linux\'s, translated to QRT\'s by the C library */\n')
    for n in names: f.write('#define __NR_%-24s %d\n' % (n, linux[n]))
    for n in names: f.write('#define SYS_%-25s %d\n' % (n, linux[n]))
with open(hdr, 'w') as f:
    f.write('/* QRT native system-call numbers (sdk/syscalls.txt) */\n')
    for n in names:
        f.write('#define __NR_%-24s %d\n' % (n, qrt[n]))

def patch(rel, old, new):
    p = os.path.join(musl, rel)
    s = open(p).read()
    if old not in s:
        sys.exit('patch_musl: %s: "%s" not found' % (rel, old))
    open(p, 'w').write(s.replace(old, new))

# syscall(): Linux number in, QRT number to the kernel (QRT-only calls, 1024 and up, pass)
top = max(linux.values()) + 1
table = [0] * top
for n in names: table[linux[n]] = qrt[n]
patch('src/misc/syscall.c', 'long syscall(long n, ...)\n{', 'static const unsigned short qrt_from_linux[%d] = { %s };\n\nlong syscall(long n, ...)\n{' % (top, ', '.join(map(str, table))))
patch('src/misc/syscall.c', 'return __syscall_ret(__syscall(n,a,b,c,d,e,f));',
      'if (n < 1024) { if (n < 0 || n >= %d || !qrt_from_linux[n]) return __syscall_ret(-38); n = qrt_from_linux[n]; }\n\treturn __syscall_ret(__syscall(n,a,b,c,d,e,f));' % top)
patch('src/process/x86_64/vfork.s', 'mov $58,%eax', 'mov $%d,%%eax' % qrt['vfork'])
patch('src/signal/x86_64/restore.s', 'mov $15, %rax', 'mov $%d, %%rax' % qrt['rt_sigreturn'])
patch('src/thread/x86_64/clone.s', 'mov $56,%al', 'mov $%d,%%eax' % qrt['clone'])
patch('src/thread/x86_64/clone.s', 'mov $60,%al', 'mov $%d,%%eax' % qrt['exit'])
patch('src/thread/x86_64/__unmapself.s', 'movl $11,%eax', 'movl $%d,%%eax' % qrt['munmap'])
patch('src/thread/x86_64/__unmapself.s', 'movl $60,%eax', 'movl $%d,%%eax' % qrt['exit'])
patch('src/thread/x86_64/__set_thread_area.s', 'movl $158,%eax', 'movl $%d,%%eax' % qrt['arch_prctl'])
print('patch_musl: %d system calls renumbered' % len(names))
