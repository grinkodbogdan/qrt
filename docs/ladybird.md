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
built-in text browser remains for the 32-bit kernel. The window has:
- back, forward, and reload (stop while loading);
- a keyboard button (the on-screen keyboard);
- the address bar: tap it, type, and press Enter.

On the page:
- a finger drag scrolls, and a tap clicks;
- the mouse and the keyboard go to the page;
- the window follows the shell (rotation, the keyboard, desk mode) like any native
  window.

Not yet: tabs (links that open a new tab load in the background), file downloads, and
the GPU. A known problem: in QEMU the machine has sometimes reset while Ladybird starts.
The cause hasn't been found yet; a run with full logging never reproduces it.

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
