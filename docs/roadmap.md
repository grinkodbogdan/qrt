# Roadmap: Firefox, GPU drawing, audio and Bluetooth on Tessera

QRT keeps its own kernel (Tessera) and its own drivers. Linux programs run
through Tessera's Linux system-call layer, not on a Linux kernel. Each goal
below is a series of milestones, each one shipped and tested on its own.
This page says where each one stands and does not promise dates.

## 1. Firefox through the Linux layer

Firefox is a large, multi-process, multi-threaded C++/Rust program on top of
glibc and GTK 3, which draws through Wayland or X11. "A custom port" here
means the official Linux build (or one built with fewer dependencies), with
QRT supplying the system calls and a display server.

| Milestone | What it brings | State |
|---|---|---|
| 1. Dynamic programs and threads | `PT_INTERP` + ld.so, file-backed `mmap`/`MAP_FIXED`, `munmap` that frees ranges, `clone` threads, `futex` | **done in 0.6.0** |
| 2. Processes | `fork`/`vfork` (copy of the address space), `execve`, `wait4`, pipes, `dup2`, signals (`rt_sigaction`, delivery on return to user, `kill`), so shells and pipelines work | next |
| 3. Memory | page protections (`mprotect`, W^X for the JS JIT), a user address space larger than 1 GiB (Firefox reserves several GiB), shared memory (`memfd_create`, `MAP_SHARED`, `/dev/shm`) | |
| 4. Event loops | `epoll`, `eventfd`, `timerfd`, `signalfd`, Unix sockets with `SCM_RIGHTS` (Firefox's processes talk over them), `socketpair`, `listen`/`accept` | |
| 5. Files | a writable file system that survives reboots (native eMMC/SD driver), `/proc/self/maps`, `/sys` entries glibc and GTK read, fonts and fontconfig files | |
| 6. A display server | a minimal Wayland compositor inside the QRT shell: `wl_compositor`, `xdg_shell`, `wl_shm` buffers composited by the GPU, `wl_seat` for touch and keyboard; each Linux window becomes a QRT window | |
| 7. The GTK stack | glib, cairo, pango, harfbuzz, fontconfig, GTK 3 with its Wayland backend; a GTK demo runs | |
| 8. Firefox | Firefox with `MOZ_ENABLE_WAYLAND=1` and software WebRender; then the GPU (milestone 2 of section 2) for WebRender | |

The image grows with these: Firefox and the GTK stack are about 250 MB, so
the image will need to grow from 64 MB, and the files will have to be read
from the stick on demand instead of being copied into RAM at boot.

## 2. The GPU draws everything

| Milestone | State |
|---|---|
| The GPU presents each frame (copy and rotation), self-test, fallback | **done in 0.5.8**, works on the tablet |
| Scrolling moves pixels instead of redrawing | **done in 0.5.9** (CPU) |
| GPU fills, rounded rectangles and blits with alpha blending, batched per frame | next |
| Text from a glyph atlas texture; the wallpaper and app windows as textures; transitions blended on the GPU | |
| Wayland client buffers composited as textures (feeds section 1, milestone 6) | |
| A Mesa-compatible path for Linux programs (`/dev/dri`, i915 ioctls) | long term |

## 3. Audio (Cherry Trail SST + a Realtek codec)

The 5855's sound goes through Intel's Smart Sound Technology DSP (PCI
`8086:22a8`) to a Realtek codec on I2C. The DSDT lists three (`10EC5672`,
`10EC5670`, `10EC5640`) and enables one with `_STA`; which one is fitted
will be read on the tablet first. Unlike HD Audio, there is no simple
controller: the DSP runs Intel's firmware (`fw_sst_22a8.bin`), and the codec
is set up register by register over I2C.

| Milestone | State |
|---|---|
| Find the fitted codec: its id registers over I2C2 (as Linux's rt5670.c/rt5640.c do) | **0.6.1**, result shown in System Monitor |
| Codec power-up, headphone and speaker paths and volume over the DesignWare I2C driver (the volume keys already drive a mock control) | next |
| SST DSP: load the firmware into its memory, the IPC mailbox, start an SSP (I2S) port to the codec | |
| PCM playback: a ring buffer the DSP reads by DMA, a beep from the shell, then sounds | |
| `/dev/snd` (ALSA PCM ioctls) for Linux programs, which Firefox needs for sound | |

## 4. Bluetooth (Intel Wireless 8260)

The tablet's Wi-Fi card, an Intel Wireless 8260 (PCI `8086:24f3`, driven by
QRT's `iwm` port), is a combo chip: its Bluetooth half is a USB device on
the internal xHCI controller (PCI `8086:22b5`). The Broadcom ids in the DSDT
(`BCM2E1A`, `BCM2E7B`...) are alternatives from Intel's reference design.

| Milestone | State |
|---|---|
| xHCI host controller driver, enumeration of root-port devices, boot keyboards | **0.6.1** (QEMU-tested; the tablet's USB device list will show the Bluetooth device's id) |
| USB hubs, mass storage (sticks) | |
| Bluetooth over USB (HCI on the control and interrupt endpoints), Intel's firmware download (`ibt-11-5.sfi`) | |
| HCI: inquiry (scan), the list of nearby devices in a Bluetooth panel in Settings | |
| L2CAP, pairing (Secure Simple Pairing), HID keyboards and mice | |
| A2DP audio (needs the audio work in section 3 and an SBC encoder) | |

## Other drivers on the list

- Battery: ACPI battery through the PMIC's fuel gauge (I2C), shown in the top bar.
- Storage: the eMMC (SDHCI, `80860F14`) and USB (xHCI, `8086:22b5`), so files
  persist and the system no longer has to copy everything into RAM.
- Suspend: S0ix through the PMC, instead of only turning the backlight off.
- Sensors (accelerometer for automatic rotation) and the cameras.
