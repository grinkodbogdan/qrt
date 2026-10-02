# The QRT SDK: native programs

Since 0.8.0, QRT has its own kind of program, separate from Linux binaries. A native QRT
program is built with the SDK in `sdk/`. It uses QRT's own system calls and draws its
windows directly in the QRT shell. The Linux layer is still there for Linux binaries
(glibc programs, busybox), but native programs don't go through it.

## What the SDK contains

`make sdk` (or `sh sdk/build.sh build/sdk`) builds it in about 20 seconds:

| Part | What it is |
|---|---|
| `build/sdk/sysroot` | **musl 1.2.6** (`sdk/musl-1.2.6.tar.gz`), built for QRT's system calls; the QRT note is linked into `crt1.o`, so every program carries it |
| `libqrt.a`, `qrt.h` | the QRT platform API: windows, events, the on-screen keyboard, simple drawing, text in Inter |
| `bin/qrt-cc` | clang for QRT: `qrt-cc -O2 -o app app.c -lqrt` (static executables) |
| `bin/qrt-cargo` | cargo for QRT: Rust's `x86_64-unknown-linux-musl` target, linked by `qrt-cc` against QRT's musl (`rustup target add x86_64-unknown-linux-musl` first) |
| `bin/qrt-c++`, `qrt.cmake` | C++23 (`sdk/build-cxx.sh`): LLVM 20's libc++, libc++abi, libunwind and compiler-rt built for QRT; a CMake toolchain file for libraries and programs |
| `hostlib/` | musl with Linux's system calls (`sdk/build-hostlib.sh`): a build tool compiled for QRT and linked with `${QRT_HOST_LINK_OPTIONS}` runs on the build machine with the target's exact layouts |

Requirements: clang, ld.lld, llvm-ar, python3 with PIL (for the font), and gcc's `libgcc.a`.

## The native ABI

- **Marking**: an ELF note named `QRT` (type 1). The kernel checks it when it loads the
  program (`load_elf` in `src/arch/x64/proc.c`) and runs the process with the native
  personality. `uname` reports `QRT` and QRT's version.
- **System calls**: `sdk/syscalls.txt` is the single source of truth.
  - Numbers 1–384 are QRT's own, in alphabetical order. They keep the argument and
    structure layouts of the Linux call with the same name, because musl is written for
    those layouts.
  - Numbers from 1024 up exist only on QRT (windows, events, the keyboard).
  - `tools/gen_native_sys.py` turns the file into the kernel's table
    (`src/arch/x64/native_sys.h`).
  - `sdk/patch_musl.py` turns it into musl's numbers. That includes the six places
    where musl's x86-64 assembly has a number written out.
- **`syscall()`** takes Linux's numbers: the public `<sys/syscall.h>` has them, and the
  C library translates them to QRT's. Code that calls `syscall(SYS_futex, …)` directly
  compiles unchanged, which is what lets Rust's standard library run. The kernel only ever
  sees QRT's numbers from a native program.
- Signals, threads, `fork`/`exec`, shared memory, epoll, Unix sockets and the rest are the
  same kernel services the Linux layer uses (see the README's Linux section). Both
  personalities sit on the same kernel, as the subsystems of Windows NT do.

## Windows (`qrt.h`)

```c
int win = qrt_window_create("My app");      /* an app in the shell: dock, overview, desk mode */
qrt_buffer b;
qrt_window_buffer(win, &b);                  /* map its pixels: b.px, b.w, b.h, b.stride */
qrt_fill(&b, 0, 0, b.w, b.h, 0x242424);
qrt_text(&b, 20, 20, "Hello", 0xffffff, QRT_TEXT_TITLE);
qrt_window_present(win, 0, 0, 0, 0);         /* show it (a rectangle, or 0 = all) */
qrt_event e;
while (qrt_wait_event(&e, -1) > 0) {
    if (e.type == QRT_EV_RESIZE) { qrt_window_buffer(win, &b); /* redraw */ }
    if (e.type == QRT_EV_CLOSE) break;       /* the user closed it */
    /* QRT_EV_DOWN/MOVE/UP (x, y; scan = 1 for the mouse), QRT_EV_KEY (ch, scan), QRT_EV_SCROLL */
}
```

A window fills the app area of whichever screen the shell is on, under the shell's header
bar (title, minimise, close). It follows that area through rotation, the on-screen
keyboard and desk mode by sending `QRT_EV_RESIZE`. Its buffer is one contiguous piece of
kernel memory mapped into the program, so the shell copies the presented rectangle with
no extra hand-off. The shell side is `src/ui/clientwin.c`, the system calls are
`src/arch/x64/qrtcall.c`. Up to seven native windows can be open at once.

## Examples (in the image's `/bin`)

- `native-test` (C) exercises the C library: stdio, `malloc`/`mmap`, files, directories,
  time, threads, `fork`/`exec`/pipes, signals, `syscall()`. It prints `native: ok`.
- `rust-hello` (Rust std) runs threads, a mutex, channels, files, time and `HashMap`.
  It prints `rust: ok`.
- `hello-window` (C + libqrt) is a window with buttons, typing, finger/mouse drawing and
  close.

The QEMU test runs all three. Run any of them from the Terminal on the tablet. Copy your
own programs into `\bin` on the stick.

## Libraries (`ports/`)

`bash ports/build.sh` cross-builds third-party libraries static into the sysroot, in the
order of `ports/ORDER`, each from a recipe `ports/<name>.sh` (its version, where its source
comes from, and its build). Today these are Ladybird's 34 dependencies: zlib, libpng,
brotli, zstd, libjpeg-turbo, libwebp, expat, libxml2, sqlite3, libtommath, simdutf,
fast-float, fmt, simdjson, mimalloc, ncurses, libedit, OpenSSL, nghttp2, libpsl, curl,
FreeType, ICU, HarfBuzz, fontconfig, woff2, dav1d, libavif, wuffs, ffmpeg, Skia, SDL3
and ANGLE's headers. It takes about 40 minutes the first time. A port that is built is
skipped; delete `build/ports/done/<name>` to rebuild one.

## What comes next

Ladybird: see [ladybird.md](ladybird.md).
