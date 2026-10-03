# A modern browser on QRT: porting Ladybird

The goal: a full modern browser (HTML, CSS, JavaScript, WebAssembly, real sites) running
as a native QRT program, not as a Linux binary in the Linux layer. The engine is
**Ladybird** (github.com/LadybirdBrowser/ladybird, BSD-2-Clause).

## Why Ladybird

- It is an independent engine: LibWeb, LibJS and LibWasm are its own, not WebKit or
  Blink. It renders real sites and is improving fast.
- It was born on a hobby operating system (SerenityOS). Its platform layer (AK, LibCore)
  was written to be ported, and its UI front-ends (Qt, AppKit, Android) are thin.
- Its processes (WebContent, RequestServer, ImageDecoder) talk over Unix sockets and pass
  descriptors and shared bitmaps. QRT has had those since 0.7.0.
- It builds on this project's build machine: about an hour and about 10 GB. Firefox
  would need about 40 GB and a new widget back-end of tens of thousands of lines.

## What Ladybird needs (checked against its tree, October 2026)

- **Compiler**: C++23. Ladybird's CI uses gcc 14 / clang 21; clang 20 (Ubuntu's
  `clang-20`) builds it too, and that is what QRT's SDK uses.
- **Rust** (1.98) is required. AK, LibJS, LibWeb, LibGfx, LibURL, LibRegex, LibUnicode,
  LibWasm and others have Rust crates. The SDK already runs Rust's standard library
  natively (`rust-hello`).
- **About 35 libraries** (from its `vcpkg.json`):
  - text: ICU, HarfBuzz, FreeType, fontconfig;
  - graphics: Skia;
  - network: curl, OpenSSL, brotli, zstd, libpsl;
  - images: libpng, libjpeg-turbo, libwebp, libavif with dav1d, tiff, woff2;
  - data: libxml2, sqlite3, simdjson, simdutf, fast-float, fmt, libtommath, zlib,
    mimalloc;
  - media: ffmpeg.

  ANGLE, Vulkan, dbus, libproxy, Qt and SDL3 are platform pieces QRT replaces or leaves
  out.
- **Disk/RAM**: Ladybird with its libraries and ICU's data is roughly 150–250 MB. The
  image is 64 MB and is copied into RAM at boot. Shipping it needs a bigger image read
  from the stick on demand (USB mass storage or the eMMC driver).

## The plan

Each step is a release with its own test, as with everything else in QRT.

| Step | What | Done when |
|---|---|---|
| L1 | Native QRT programs: the native personality, the SDK (musl on QRT system calls, `qrt-cc`), `libqrt` windows, Rust std | **0.8.0**: `native-test`, `rust-hello`, `hello-window` pass in the QEMU test |
| L2 | C++: LLVM 20's compiler-rt, libunwind, libc++abi, libc++ built for QRT; `qrt-c++` | **done**: `cxx-test` (exceptions, threads, `<format>`, `std::filesystem`, coroutines) passes on QRT |
| L3 | Ladybird's libraries (`ports/`) and its base: AK, LibCore, LibGC, LibJS with ICU, simdutf, fast_float, fmt, libtommath, simdjson, LibJS's Rust crates | **done**: `js`, Ladybird's JavaScript shell, runs natively on QRT and passes `tests/ladybird/js-test.js` (classes, generators, BigInt, Proxy, RegExp, typed arrays, `Intl` with ICU's data, promise jobs) |
| L4 | Rendering: LibWeb and LibGfx with Skia's CPU rasteriser, FreeType, HarfBuzz and the image libraries; fonts from the image | **done**: `ladybird --headless=screenshot` renders `tests/ladybird/page.html` (CSS grid and flex, fonts, SVG, a canvas, a PNG, script) with its WebContent, ImageDecoder and other processes; the QEMU test checks the PNG |
| L5 | Network and processes: RequestServer (curl, OpenSSL, brotli, zstd, nghttp2, libpsl), WebContent and ImageDecoder as separate processes over Unix sockets, bitmaps in memfd shared memory | `headless-browser` loads a real HTTPS site |
| L6 | The QRT front-end (`UI/QRT`, on `libqrt`): a window with tabs, an address bar, touch scrolling, the on-screen keyboard, desk mode with the mouse; the bigger image read on demand | Ladybird is the default browser on the tablet |
| L7 | Later: media (ffmpeg; sound once the RT5670/SST driver exists), the GPU (Skia on Vulkan/GL needs a Mesa-class driver), sandboxing | |

## Using it (L6, first version)

The dock's **Browser** is Ladybird when the image has it (`/bin/ladybird`); the old
built-in text browser remains as a fallback for an image built without it. The window has:
- back, forward, and reload (stop while loading);
- a keyboard button (the on-screen keyboard);
- the address bar: tap it, type, and press Enter.

On the page:
- a finger drag scrolls, and a tap clicks;
- the mouse and the keyboard go to the page;
- the window follows the shell (rotation, the keyboard, desk mode) like any native
  window.

Since 0.9.0:
- a link that opens a new window (`target=_blank`, `window.open`) loads in the same
  view, since there are no tabs yet;
- the on-screen keyboard comes up by itself when a text field on the page takes the
  focus, and goes away when it loses it.

Since 0.9.1:
- pages no longer go blank after a while (see below);
- Ladybird's window is the dock's **Browser**: one entry, not two;
- the HTTP disk cache is off: QRT's file system is in memory, so it only held a second copy
  of Ladybird's memory cache;
- less lag: a woken thread runs within 1 ms (it used to wait up to 10 ms at each hand-over
  between Ladybird's processes); queued finger movements and wheel steps for a window that
  is behind are merged; Skia is compiled for the tablet's Atom (SSE4.2, SSSE3), so its CPU
  rasteriser uses the instructions that chip has.

Since 0.9.2: video and audio play with sound - LibMedia gets an OSS backend
(`Audio/PlaybackStreamOSS.cpp`, patch 0005) that writes to QRT's `/dev/dsp`; the QEMU test plays
a VP9/Opus WebM (`tests/ladybird/video.html`) and checks its tone in QEMU's USB audio recording.

Not yet: tabs, file downloads, and the GPU (see [gpu.md](gpu.md)). The GPU already puts every frame on the screen
(QRT's compositor), but pages are painted by Skia's CPU rasteriser: painting them on the GPU
needs an OpenGL or Vulkan driver for the Gen8 graphics (a Mesa-class port). Every program
also still runs on the first CPU core only; the other three draw the shell.

### Blank pages (fixed in 0.9.1)

On the tablet, pages went blank after some browsing. WebContent crashed with
`mprotect: Out of memory`: a QRT process could have 1024 mappings, and LibJS maps every
JavaScript function's bytecode on its own and makes it read-only. The VMAs are now a sorted
array that grows up to 65530 entries (Linux's `max_map_count`), searched by binary search,
with agreeing neighbours merged. `tests/ladybird/functions.html` (6000 functions) crashes
0.9.0 and passes now; the QEMU test loads it. Ladybird's disk cache failed too
("error reading a cached HTTP response"): it sends cached responses with `sendfile`, which
QRT answered with EINVAL. `sendfile` works now.

`tests/ladybird/fps.html` measures the frame rate (about 23 fps in QEMU, whose CPU emulation
limits it; the tablet is the real measure).

### The triple fault (fixed in 0.9.0)

In 0.8.0, QEMU sometimes reset (a triple fault) while Ladybird started, in a few runs out
of ten. A window's shell entry was published before it was filled in: `cw_create` marked
the slot used with a zeroed `app_t`, then allocated the window's buffer (which can be
preempted) before setting the entry's `draw` callback. If the shell thread ran in that
gap, it opened the window and its render jobs called `draw`, which was NULL. The worker
cores jumped to address 0 and ran through zeroed low memory (`00 00` is a valid
instruction) into the SMP startup trampoline at 0x9e000. Its 16-bit code, run in long
mode, loaded a garbage GDT and reset the machine.

A window now becomes visible to the shell only when it is complete (`ready`), and the
shell never calls a missing `draw`. Two other races found during the hunt were fixed in
0.8.0's tree: descriptors released twice for a process killed while it exited, and render
jobs reaching a worker core half-written.

## Building it

```sh
make sdk && sh sdk/build-cxx.sh && sh sdk/build-hostlib.sh   # C, C++, the host libc
bash ports/build.sh                                         # the 34 libraries, ~40 min
bash ports/ladybird/build.sh                                # Ladybird's js, ~15 min
make                                                        # js goes into the image's /bin
```

`ports/ladybird/build.sh` fetches Ladybird at a fixed commit, applies the patches in
`ports/ladybird/`, and configures it with the SDK's toolchain file (`build/sdk/qrt.cmake`):
static, Release, no GUI targets yet, Rust crates for `x86_64-unknown-linux-musl`. To
QRT's clang, Ladybird is Linux on musl, so `AK_OS_LINUX` code paths are used. QRT's
kernel provides what those paths expect.

## What it took (L2–L3)

- **The libraries** are cross-compiled with each project's own build system (CMake,
  Meson, autotools, GN) at the versions Ladybird pins (its `vcpkg.json`), with their
  sources from GitHub or Ubuntu's archive. Recipes are in `ports/`.
- **Skia** (version 148) is built from the commit vcpkg pins, with vcpkg's patches, CPU
  rasteriser only, using the sysroot's FreeType, HarfBuzz, ICU, fontconfig and expat. It
  installs a `skia.pc`.
- **ANGLE and SDL3.** Ladybird requires them, but QRT has no GPU driver or gamepads
  yet:
  - ANGLE: only its headers, plus a stub EGL/GLES library in which there is no display,
    so Ladybird paints with Skia on the CPU;
  - SDL3: built without video or audio, only for its gamepad API, which finds no
    gamepads.
- **Build tools.** Ladybird compiles one C++ tool and runs it during its build
  (`generate_interpreter_layout`, which prints the JS interpreter's structure offsets).
  A QRT program can't run on the build machine, so this tool is linked with the SDK's
  host libc (`sdk/build-hostlib.sh`), the one Ladybird patch so far. Its Rust build
  tools are Rust musl programs and run on the build machine as they are.
- **Fixes in the SDK**:
  - libc++abi had wrongly detected glibc's `__cxa_thread_atexit_impl`;
  - libtommath's `mp_set_double` needs `__STDC_IEC_559__`, which only glibc predefines.
- **Kernel**:
  - Ladybird's garbage collector reserves two 4 TiB "cages" of address space, so a
    process's address space grew from 448 GiB to 127 TiB (all of x86-64's lower half
    except the kernel's device window at 512 GiB–1 TiB).
  - `/dev/kmsg` logs every line of a write, which is how the QEMU test reads `js`'s
    output.
- **One program (L4)**: `UI/QRT` is QRT's front-end. Started as `ladybird <Helper>`,
  the same executable is each helper process (WebContent, RequestServer, ImageDecoder,
  Compositor, WebWorker, MediaServer, WasmCompiler), so LibWeb, LibJS and ICU's data are
  on disk once (150 MB stripped). The kernel maps a program's read-only pages from the
  file, shared by every process running it.
- **What else L4 needed**:
  - libpng with the APNG patch; OpenSSL with threads (its Configure turns them off when
    `LDFLAGS` has `-static`); SQLite in exclusive locking mode (WAL without a shared
    `-shm` mapping);
  - fonts (Liberation, DejaVu) and `/etc/fonts/fonts.conf`;
  - in the kernel: `close_range`, `/proc/self/status`, `/dev/urandom`, `fsync`,
    `statfs`, `preadv`/`pwritev`, the `chmod`/`chown` family, and files up to 256 MB.
- **Tracing**: run a program with `QRT_TRACE=1` in its environment and every system call
  that fails is logged with its path, which is how these were found.
- **Size**: `js` is 48 MB stripped, 33 MB of which is ICU's data. The image now grows
  to fit what it carries (96 MB with `js`). Trimming ICU's data to the locales QRT needs
  is part of L6.

## What the Linux layer is for now

Linux binaries keep working (busybox, glibc programs, the tests). The browser does not
depend on the Linux layer. The native and Linux personalities share the kernel's
services, so everything built for the Linux layer in 0.6–0.7 carries straight over to
native programs:
- signals, a big address space with page protections, shared memory;
- epoll, eventfd, timerfd, signalfd;
- Unix sockets with descriptor passing.
