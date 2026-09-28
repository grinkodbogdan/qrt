# QRT

A small custom operating system for the **Dell Venue 8 Pro**, with a touch-first
UI inspired by Fuchsia's Armadillo/Ermine shells. Its kernel, **Tessera**, is
written from scratch. It is not Linux, but it can run Linux programs.

<p>
<img src="docs/screenshots/home-portrait.png" width="30%">
<img src="docs/screenshots/home.png" width="66%">
</p>

| | |
|---|---|
| ![terminal](docs/screenshots/terminal.png) | ![system](docs/screenshots/system.png) |
| ![sketch](docs/screenshots/sketch.png) | ![files](docs/screenshots/files-text.png) |

## The kernel: Tessera

QRT is one UEFI application, and it boots in two stages.

**1. Under the firmware.** Tessera starts as a UEFI program. It does the
following:
- reads the machine's identity (SMBIOS, ACPI, CPUID, memory map)
- asks the firmware to connect every driver
- dumps the ACPI tables
- copies the boot stick into RAM
- probes the touchscreen with its own I2C driver

**2. Native (64-bit firmware).** Tessera then calls `ExitBootServices()` and
takes over the machine. From then on, everything is QRT's own code:

| Subsystem | Native Tessera |
|---|---|
| Memory | physical page allocator, kernel heap, 4-level page tables (identity map with 2 MB pages, write-combining framebuffer via PAT) |
| CPU | per-core GDT/TSS/IDT, exception handling with a crash screen |
| Interrupts | local APIC (xAPIC/x2APIC); I/O APIC routing from the MADT; MSI for PCI |
| Time | TSC calibrated at boot, 1 kHz local-APIC timer |
| Scheduling | preemptive threads, 10 ms round-robin; idle cores halt |
| Multicore | other cores started by QRT itself (INIT/SIPI through a real-mode trampoline) and used for rendering |
| Files | in-memory file system: the boot stick plus `/proc`, `/etc`, `/tmp`, `/dev` |
| Processes | ring-3 address spaces, ELF loader, demand paging, `SYSCALL` entry |
| Linux ABI | the system calls static glibc, musl and busybox programs need |
| Drivers | a driver model over PCI, ACPI and platform devices; see below |

The firmware is kept only for its *runtime* services: the RTC, NVRAM
variables (where settings are stored) and reset/power-off.

The UI and the apps behave the same in both stages. Underneath, a HAL
(`src/kernel/hal.c`) hands them either firmware services or native drivers.

### Safety

Native mode has fallbacks, because without the firmware QRT must bring
touch back up on its own:

- QRT goes native only if it can drive an input device itself: the native
  touchscreen, or a serial console.
- If the touchscreen does not answer after the handover, QRT records the
  failure and reboots once into firmware mode.
- **Holding any key or hardware button** at boot starts in firmware mode.
- **Settings → Kernel mode** switches between native and firmware for the
  next boot.

On 32-bit UEFI (Venue 8 Pro 5830) QRT always runs in firmware mode.

## Linux programs

Open **Terminal**. It runs static x86-64 Linux ELF programs, unmodified,
through Tessera's Linux system-call layer (`src/arch/x64/linux.c`). The
image includes a static **busybox**, so `ls -l /`, `cat /proc/cpuinfo`,
`free`, `date`, `sha256sum`, `wc`, `uname -a`, and anything else busybox
provides work. `/bin` also holds two test programs, one linked against glibc
(`hello`) and one against musl (`hello-musl`). They check stdio, `malloc`,
files, directory listing, `/proc` and thread-local storage.

To add your own program, compile it with `gcc -static` (or `musl-gcc
-static`) and copy it into `\bin` on the stick.

Not supported yet:
- `fork`/`exec`/threads (so there are no pipelines or shells)
- signals
- sockets
- dynamic linking
- keyboard input to programs

`clear` and `help` are handled by the Terminal itself.

## Drivers

`src/kernel/dev.h` is the driver model. Buses enumerate devices from PCI
(ECAM), ACPI (the `_HID`s in the DSDT/SSDTs) and fixed platform devices.
Drivers declare id tables and a `probe()`. **System → Devices** lists every
device, first those with a QRT driver and then those still waiting for one.
On the tablet, that second part is the to-do list.

Built in:
- **framebuffer**
- **16550 UART**: interrupt-driven through the I/O APIC
- **DesignWare I2C**
- **HID over I2C**: the Venue's Wacom touchscreen
- **chipset**: devices the kernel handles itself

[docs/drivers.md](docs/drivers.md) covers the primitives a driver gets
(MMIO, DMA memory, IRQ/MSI, threads). It also lays out the plan for using
Linux drivers: port them by hand now, then a LinuxKPI-style shim like
FreeBSD's.

## Hardware status on the Venue 8 Pro 5855

| | |
|---|---|
| Display | works (framebuffer, 1200×1920, rotation) |
| Touch | works: QRT's own Wacom driver; tested on the tablet in firmware mode. Native mode needs the same driver after the handover, which is **new in 0.5 and untested on hardware**. |
| Storage | the boot stick is read into RAM at boot; writes go to RAM only |
| Wi-Fi, Bluetooth, audio, camera, sensors, battery, backlight | no drivers yet (they need ACPI/PMIC support first; see docs/drivers.md) |
| USB keyboard | firmware mode only |

## Hardware report

On every boot QRT writes the machine's hardware description to the stick,
under `\qrt\hwdump\`:
- every ACPI table
- the SMBIOS table
- `report.txt` (PCI devices, ACPI ids, boot log)

The dump is taken before the handover, while the stick is still writable.
Reports saved later (Touch Lab's `touch.txt` in native mode) go to the RAM
copy.

**Keep that folder private.** On Windows tablets, `MSDM.aml` contains the
Windows product key.

## Rendering

Everything is drawn in software. Each change records a damage rectangle,
and only that area is redrawn and copied to the panel. Big redraws are cut
into strips (4 per core), and every core claims strips from a shared
counter. In native mode those cores were started by QRT. Under the firmware
they come from UEFI's MP Services.

In QEMU with 4 cores, a full 1280×800 redraw drops from 74 ms on one core to
24 ms. Type `bench` in the Ask bar to measure; the result appears in
**System → Graphics**.

## Touch Lab

**Touch Lab** runs the native touchscreen stack by hand:
- **Probe** reads the chip's HID descriptors.
- **Go native** takes touch over from the firmware.
- **Save report** writes `\qrt\hwdump\touch.txt`.

The stack is `src/drivers/dwi2c.c`, `i2chid.c`, `hidparse.c` and `touch.c`.
`make check` runs the HID parser's host tests against the Venue's real Wacom
descriptor. Hardware notes are in `docs/hardware/venue-8-pro-5855.md`.

## Put it on the tablet

Your Windows install on the eMMC is not touched: QRT runs entirely from the stick.

1. Use `dist/qrt-0.5.0.img.gz`, or build the image with `make`.
2. Write it to a USB stick with Rufus or balenaEtcher, or on Linux:
   `gunzip -c dist/qrt-0.5.0.img.gz | sudo dd of=/dev/sdX bs=4M conv=fsync`.
3. Plug the stick into the tablet's micro-USB port with an OTG adapter.
4. In the firmware setup, disable **Secure Boot** (the image is not signed)
   and boot from the stick.

**Settings → Firmware** reboots into the BIOS setup.

## Build it yourself

You need:
- `clang`, `lld`, `mtools`, `dosfstools` and `gdisk`
- `qemu-system-x86`, `ovmf` and `ovmf-ia32`, for `make run` and `make test`
- `gcc`, `musl-tools` and `busybox-static`, for the Linux programs in `/bin`
- Pillow, only to regenerate the fonts

```sh
make            # build/BOOTIA32.EFI, build/BOOTX64.EFI, build/qrt.img
make run64      # boot in QEMU on 64-bit UEFI (native mode, 4 cores)
make run        # boot in QEMU on 32-bit UEFI (like a 5830)
make test       # headless boot on both, scripted walkthrough, screenshots in build/shots/
make check      # host unit tests (HID parser)
```

OVMF has no touch driver, and native QRT has no USB keyboard driver yet. In
native mode, QRT therefore reads keys from the serial port: in the QEMU
window, choose *View → serial0* and type there. Arrow keys, Enter and Esc
navigate the UI. `tools/qemu-test.py` injects touch the same way.

## Layout

```
src/efi.h              UEFI ABI subset (written from the spec)
src/kernel/            boot, HAL, VFS, driver model (dev.c), hardware report, runtime
src/arch/x64/          native kernel: memory, CPU/IDT, APIC, I/O APIC + MSI, scheduler,
                       SMP trampoline, processes, Linux system calls
src/drivers/           PCI, UART, DesignWare I2C, HID over I2C, touch service, driver table
src/ui/                gfx (anti-aliased shapes, text), font atlases, shell
src/apps/              Clock, Sketch, Files, System, Settings, Life, Touch Lab, Terminal
tests/                 HID parser tests, Linux test program
tools/                 mkfont.py, mkimage.sh, run-qemu.sh, qemu-test.py, gen_isr.py
assets/                Inter (SIL OFL 1.1), DejaVu Sans Mono (Bitstream Vera licence)
```

Apps implement a five-function ABI (`open`, `draw`, `event`, `tick`, `icon`)
in `src/ui/shell.h`.

`/bin/busybox` on the image is an unmodified copy of Ubuntu's
`busybox-static`, licensed under GPL-2.0. It runs as a separate program,
and `\bin\BUSYBOX.txt` says where to get its source.
