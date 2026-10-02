# Roadmap: a native browser, GPU drawing, audio and Bluetooth on Tessera

QRT keeps its own kernel (Tessera) and its own drivers. Each goal below is a series of
milestones, each one shipped and tested on its own. This page says where each one stands
and does not promise dates.

## 1. A modern browser, ported natively: Ladybird

The browser is a **native QRT program**: built with the QRT SDK, on QRT's own system
calls, with its window in the QRT shell. It is not a Linux binary in the Linux layer.
The engine is Ladybird. Why Ladybird and not Firefox, what it needs, and the steps are
in [ladybird.md](ladybird.md).

| Step | What it brings | State |
|---|---|---|
| L1. Native programs | the native personality, the SDK (musl on QRT system calls, `qrt-cc`, `qrt-cargo`), `libqrt` windows, Rust std | **done in 0.8.0** ([sdk.md](sdk.md)) |
| L2. C++ | LLVM 21; libc++, libc++abi, libunwind, compiler-rt for QRT | next |
| L3. Ladybird's base | AK, LibCore, LibJS + ICU and friends: the `js` shell runs | |
| L4. Rendering | LibWeb, LibGfx with Skia (CPU), FreeType, HarfBuzz, image libraries: a page rendered to PNG | |
| L5. Network, processes | RequestServer (curl, OpenSSL), WebContent and ImageDecoder processes: a real HTTPS site loads | |
| L6. The QRT front-end | tabs, address bar, touch, keyboard, desk mode; a bigger image read on demand: the default browser | |

### What the Linux layer brought (and still serves)

These were planned as the road for Firefox *through* the Linux layer. They are kernel
services both personalities use, so the native browser builds on them.

| Milestone | What it brings | State |
|---|---|---|
| Dynamic programs and threads | `PT_INTERP` + ld.so, file-backed `mmap`/`MAP_FIXED`, `munmap`, `clone` threads, `futex` | **done in 0.6.0** |
| Processes | `fork`/`vfork`, `execve`, `wait4`, pipes, `dup2`, `kill`; shells and pipelines | **done in 0.6.3**, signal handlers **0.7.0** |
| Memory | page protections (NX, `mprotect`, W^X for a JIT), 448 GiB of address space, shared memory (`memfd_create`, `MAP_SHARED`, `/dev/shm`) | **done in 0.7.0** |
| Event loops | `epoll`, `eventfd`, `timerfd`, `signalfd`, Unix sockets with `SCM_RIGHTS`, `socketpair`, `listen`/`accept` | **done in 0.7.0** |
| Files | a writable file system that survives reboots (eMMC/SD or USB storage), files read on demand | needed for L6 |

## 2. The GPU draws everything

| Milestone | State |
|---|---|
| The GPU presents each frame (copy and rotation), self-test, fallback | **done in 0.5.8**, works on the tablet |
| Scrolling moves pixels instead of redrawing | **done in 0.5.9** (CPU) |
| GPU fills, rounded rectangles and blits with alpha blending, batched per frame | next |
| Text from a glyph atlas texture; the wallpaper and app windows as textures; transitions blended on the GPU | |
| Native windows composited as textures (the browser's pages) | |
| A Mesa-compatible path for Linux programs (`/dev/dri`, i915 ioctls) | long term |
| External monitors on the USB-C port (DP Alt Mode): modeset of pipe B/C, mirroring scaled by the GPU | **0.6.4**, works on the tablet |
| Desk mode: the shell on the monitor, the tablet as its touchpad and keyboard; USB mice | **0.6.5** |
| The external monitor as a second screen with its own windows | |

## 3. Audio (Cherry Trail SST + a Realtek codec)

The 5855's sound goes through Intel's Smart Sound Technology DSP (PCI
`8086:22a8`) to a Realtek codec on I2C. The DSDT lists three (`10EC5672`,
`10EC5670`, `10EC5640`) and enables one with `_STA`; which one is fitted
will be read on the tablet first. Unlike HD Audio, there is no simple
controller: the DSP runs Intel's firmware (`fw_sst_22a8.bin`), and the codec
is set up register by register over I2C.

| Milestone | State |
|---|---|
| Find the fitted codec: its id registers over I2C2 (as Linux's rt5670.c/rt5640.c do) | **0.6.1**: an RT5670/RT5672 (id 0x6271) on the tablet |
| Codec power-up, headphone and speaker paths and volume over the DesignWare I2C driver (the volume keys already drive a mock control) | next |
| SST DSP: load the firmware into its memory, the IPC mailbox, start an SSP (I2S) port to the codec | |
| PCM playback: a ring buffer the DSP reads by DMA, a beep from the shell, then sounds | |
| A sound API for native programs (the browser's audio), and `/dev/snd` for Linux ones | |

## 4. Bluetooth (Intel Wireless 8260)

The tablet's Wi-Fi card, an Intel Wireless 8260 (PCI `8086:24f3`, driven by
QRT's `iwm` port), is a combo chip: its Bluetooth half is a USB device on
the internal xHCI controller (PCI `8086:22b5`). The Broadcom ids in the DSDT
(`BCM2E1A`, `BCM2E7B`...) are alternatives from Intel's reference design.

| Milestone | State |
|---|---|
| xHCI host controller driver, enumeration of root-port devices, boot keyboards | **0.6.1**; on the tablet it found a hub (0424:2807) on port 1 and the Bluetooth controller (8087:0a2b) on port 4 |
| USB 2 hubs | **0.6.3** (QEMU-tested) |
| USB mass storage (sticks) | |
| Bluetooth over USB (HCI on the control, interrupt and bulk endpoints), Intel's firmware download (`ibt-11-5.sfi`, `.ddc`) | **0.6.2**, works on the tablet |
| HCI: inquiry and LE scan, the list of nearby devices in the Bluetooth app | **0.6.2**, works on the tablet |
| L2CAP, pairing (Secure Simple Pairing), HID keyboards and mice | |
| A2DP audio (needs the audio work in section 3 and an SBC encoder) | |

## Other drivers on the list

- Battery: ACPI battery through the PMIC's fuel gauge (I2C), shown in the top bar.
- Storage: the eMMC (SDHCI, `80860F14`) and USB (xHCI, `8086:22b5`), so files
  persist and the system no longer has to copy everything into RAM.
- Suspend: S0ix through the PMC, instead of only turning the backlight off.
- Sensors (accelerometer for automatic rotation) and the cameras.
