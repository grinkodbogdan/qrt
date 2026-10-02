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

- **Compiler**: C++23 with clang 21 (the build machine has 18; LLVM 21 release builds
  download from GitHub). Ladybird's CI uses gcc 14 / clang 21.
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
| L2 | C++: LLVM 21 toolchain; compiler-rt, libunwind, libc++abi, libc++ built for QRT; `qrt-c++` | a C++23 test (exceptions, threads, `<format>`, `std::filesystem`) passes on QRT |
| L3 | Ladybird's base: AK, LibCore (event loop on `poll`/`epoll`, timers, notifiers, sockets), LibThreading, LibFileSystem, LibJS with ICU, simdutf, fast_float, fmt, libtommath, simdjson; a QRT platform define (`AK_OS_QRT`, Linux-like on musl) | `js`, Ladybird's JavaScript shell, runs natively on QRT |
| L4 | Rendering: LibWeb and LibGfx with Skia's CPU rasteriser, FreeType, HarfBuzz and the image libraries; fonts from the image | Ladybird's `headless-browser` renders a local page to a PNG on QRT |
| L5 | Network and processes: RequestServer (curl, OpenSSL, brotli, zstd, nghttp2, libpsl), WebContent and ImageDecoder as separate processes over Unix sockets, bitmaps in memfd shared memory | `headless-browser` loads a real HTTPS site |
| L6 | The QRT front-end (`UI/QRT`, on `libqrt`): a window with tabs, an address bar, touch scrolling, the on-screen keyboard, desk mode with the mouse; the bigger image read on demand | Ladybird is the default browser on the tablet |
| L7 | Later: media (ffmpeg; sound once the RT5670/SST driver exists), the GPU (Skia on Vulkan/GL needs a Mesa-class driver), sandboxing | |

The libraries are cross-compiled with each project's own build system (CMake, Meson,
autotools) through a QRT toolchain file pointing at the SDK. vcpkg's overlay triplets are
an option once the toolchain is stable.

## What the Linux layer is for now

Linux binaries keep working (busybox, glibc programs, the tests). The browser does not
depend on the Linux layer. The native and Linux personalities share the kernel's
services, so everything built for the Linux layer in 0.6–0.7 carries straight over to
native programs:
- signals, a big address space with page protections, shared memory;
- epoll, eventfd, timerfd, signalfd;
- Unix sockets with descriptor passing.
