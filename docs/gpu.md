# Drawing web pages on the GPU: the options (0.9.2)

QRT already uses the GPU: since 0.5.8 the Cherry Trail's Gen8 3D engine copies, scales and
rotates every frame onto the tablet's panel and the external monitor
([README](../README.md#gpu-acceleration-058)).  What it does not do yet is *paint* the
pages: Ladybird rasterises them with Skia on the CPU, and that is what makes scrolling and
animation slow on an Atom.  This note is about closing that gap.

## What Skia needs

Ladybird's Compositor process (Services/Compositor) already has an OpenGL path
(OpenGLContext.cpp): Skia's Ganesh GL backend paints the display list into a GPU texture.  On
Linux it gets OpenGL from Mesa through EGL.  So the question is not "can Ladybird use a GPU"
- it can - but "where does an OpenGL (or Vulkan) driver for Intel Gen8 come from on QRT".

## Why kgsl (and friends) do not apply

kgsl is the kernel interface of Qualcomm's Adreno GPUs (Android); Mesa's freedreno and
turnip drive it.  Panfrost/panthor are Arm Mali, etnaviv Vivante, v3d Broadcom.  The tablet
has Intel graphics, so the matching userspace is Mesa's Intel drivers:

| Mesa driver | API | Intel generations | Gen8 (Cherry Trail)? |
|---|---|---|---|
| iris | OpenGL 4.6 / GLES 3.2 | Gen8 and newer | yes |
| crocus | OpenGL | Gen4 - Gen7.5 | no |
| hasvk | Vulkan | Gen7 - Gen8 | yes |
| anv | Vulkan | Gen9 and newer | no |

All of them talk to the kernel through Linux's **i915 DRM interface**: GEM buffer objects,
mmap of buffers, GPU address spaces, `execbuffer2` command submission, contexts, fences and
waits, and queries for the engine and its configuration.

## The options, easiest first

1. **Use all four CPU cores for Ladybird** (not the GPU, but the biggest win available).
   **Done in 0.9.5**: program threads run on every core, the kernel under one big lock
   (README, "0.9.5").  A finer-grained kernel (per-subsystem locks) would let system calls
   run in parallel too; for now they take turns.

2. **Paint at a lower resolution and let the GPU scale.**  Ladybird painting at 0.75x
   (1.8x fewer pixels), the GPU upscaling as it composes.  Pages get a little softer, but
   scrolling gets faster in proportion.  A setting, cheap to build, testable in QEMU.

3. **An i915 DRM layer in QRT, and Mesa's iris** - real GPU painting.
   - The kernel: a `/dev/dri/renderD128` that implements the i915 calls iris uses, on top of
     QRT's existing Gen8 driver (it already manages the global GTT, runs batches on the
     render ring, and waits with timeouts).  GEM objects can be QRT's shared-memory objects;
     the missing pieces are per-context address spaces (or softpin in the global GTT),
     `execbuffer2` with fences, and the queries.
   - Mesa: cross-built static for QRT like the other ports (meson; iris and its compiler
     need no LLVM), with EGL "surfaceless" - the Compositor renders off-screen and hands
     the shell a buffer, as it does now.
   - Ladybird: build the Compositor with its OpenGL context on QRT.
   This is weeks of work, and it cannot be tried in QEMU (QEMU has no Intel GPU): every
   step has to be tested on the tablet.  Video decoding on the GPU (Cherry Trail decodes
   H.264, VP8 and VP9 in hardware) would come later through the same layer and the
   VA-API driver.

4. **A Vulkan layer instead (hasvk).**  The same kernel work as option 3; Skia's Graphite or
   Ganesh-Vulkan on top.  hasvk is less used than iris, so option 3 is the safer start.

## The plan

Option 1 first: it speeds up everything (pages, video decoding, the shell) and can be
developed and tested in QEMU.  Option 2 next as a quick setting.  Then option 3 in stages
that can each be checked on the tablet: buffer objects and a batch that clears a buffer;
Mesa's own test programs; Skia's GL backend in a test program; then Ladybird.

## YouTube

What YouTube needs, and where QRT is (0.9.2):

| | |
|---|---|
| Media Source Extensions | in Ladybird |
| VP9 and Opus (what YouTube sends browsers like Ladybird), AV1 | FFmpeg and dav1d, ported in 0.8 |
| Sound | 0.9.2: Ladybird's OSS backend, QRT's mixer, USB audio and Bluetooth headphones |
| A VP9 WebM with sound | plays in QEMU (tests/ladybird/video.html, in the QEMU test) |
| youtube.com itself | not testable here (the build machine's network does not reach it); try it on the tablet |

Decoding VP9 on one Atom core is enough for 360p, probably not for 720p - another reason
for option 1.
