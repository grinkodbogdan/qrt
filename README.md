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
| ![launcher](docs/screenshots/launcher.png) | ![keyboard](docs/screenshots/keyboard.png) |
| ![terminal](docs/screenshots/terminal.png) | ![network](docs/screenshots/terminal-net.png) |
| ![wifi](docs/screenshots/wifi.png) | ![system](docs/screenshots/system.png) |
| ![browser](docs/screenshots/browser.png) | ![browser over https](docs/screenshots/browser-https.png) |
| ![lock screen](docs/screenshots/lock.png) | ![volume](docs/screenshots/volume.png) |
| ![dragging the dock](docs/screenshots/dock-dragging.png) | ![dock on the left](docs/screenshots/dock-left.png) |

## The shell

- **Dock.** A rounded panel floating along one edge holds pinned apps
  (Files, Terminal, Browser, Wi-Fi, Sketch, Settings) and any others you
  open. A dot marks running apps. Tapping the app in front minimises it.
  - Drag the dock to move it. It stays under your finger at the spot you
    picked it up, and takes the shape it will have on the nearest edge.
    On release it glides to that edge (left, right, top or bottom).
  - While it moves, only the dock's own area is redrawn, not the whole
    screen, which keeps dragging smooth on the tablet.
  - The edge is saved in NVRAM, so the dock stays there after a reboot.
- **Launcher.** The 3×3 dots at the end of the dock open a grid of every
  app. Its search field also finds actions, such as rotate, restart,
  benchmark and the touch fixes. Typing anywhere on the home screen opens
  it.
- **On-screen keyboard.** Tapping a text field brings it up: the launcher
  search, the Terminal command line, a Wi-Fi password, or the browser's
  address bar and form fields. It has letters,
  digits and two symbol pages, shift and caps lock, a repeating backspace,
  and cursor keys.
- **Hardware buttons** (Venue 8 Pro 5855, native mode):
  - **power**: locks the screen. Pressed on the lock screen, the tablet
    sleeps; pressed while asleep, it wakes. Held for a second, it opens the
    power menu (sleep, restart, shut down, firmware setup).
  - **volume up / down**: change the volume and show an on-screen volume
    bar. Holding a key repeats. There is no sound driver yet, so this
    is a mock control: the level is kept and saved, but nothing plays.
  - **Windows button**: opens and closes the launcher (the dock's list of
    all apps).

  QRT reads these straight from the Cherry Trail GPIO pads the DSDT names.
  In QEMU, keys on the serial console stand in for them: F9 and F10 for
  volume, F11 for power, F12 for power held, F8 for Windows.
- **Lock screen and sleep.** The lock screen shows the time, the date and
  the network. Swipe up to unlock (or press Enter on a keyboard).
  - After a period without a touch or a button press, QRT locks and
    sleeps. Set the period in **Settings → Sleep after**: never, 30 s,
    1, 2 (the default), 5 or 10 minutes. On the lock screen it sleeps
    after 20 seconds.
  - Asleep, the backlight is off (on machines without backlight control,
    the screen is black) and the frame loop slows down. Wi-Fi stays
    connected.
  - Only the power or Windows button wakes the tablet; touch does not.
    In firmware mode, where QRT has no button driver, a touch wakes it.
  - This is not ACPI suspend: QRT cannot enter S0ix/S3 yet, so the
    tablet still draws more power asleep than it would under Windows.
- **Brightness.** **Settings → Brightness** sets the backlight. It drives
  the SoC's PWM controller, which the DSDT links to the panel.

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
| Linux ABI | the system calls static glibc, musl and busybox programs need, including sockets and poll/select |
| Network | Ethernet/802.11 → ARP, IPv4, ICMP, UDP, TCP; DHCP client, DNS resolver (`src/net/`) |
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

Programs can use the network. Linux `AF_INET` sockets (TCP, UDP and ICMP),
`poll` and `select` sit on QRT's own TCP/IP stack. glibc resolves names
through the generated `/etc/resolv.conf`, so busybox `wget` and `nslookup`
work.

The Terminal also has a few built-ins:
- `ping [-c N] HOST`
- `ifconfig`
- `wifi`
- `clear`
- `help`

Not supported yet:
- `fork`/`exec`/threads (so there are no pipelines or shells)
- signals (busybox `ping` sends only one packet: use the built-in `ping`)
- listening sockets
- IPv6
- dynamic linking
- keyboard input to programs

## Wi-Fi and networking

The 5855's Wi-Fi is an **Intel Wireless 8260** on PCIe. QRT's driver
(`src/drivers/iwm/`) is a port of OpenBSD's `iwm(4)`. Unlike the original,
it is polled, so it runs the same in native and firmware mode. It covers:
- firmware loading, including the 8000 family's CPU1/CPU2 sections and
  firmware paging
- NVM and calibration
- UMAC scanning
- MAC/PHY/binding/station contexts
- the TX/RX rings
- hardware CCMP

On top of it, `src/net/wlan.c` does what net80211 does for iwm:
- scan results
- open-system authentication and association
- the WPA2-Personal 4-way and group-key handshakes (PBKDF2, the 802.11 PRF,
  AES key wrap)
- software CCMP for group-key broadcasts

`src/net/` then carries ARP, IPv4, ICMP, UDP, a DHCP client, a DNS resolver
and TCP.

To connect:
1. Open **Wi-Fi** and switch it on. Loading the firmware takes a second or
   two.
2. Pick a network.
3. Type the password on the on-screen keyboard.

The network and its derived key (not the password) are saved in NVRAM.
If Wi-Fi was on, QRT turns it on at the next boot and reconnects.

Supported: open networks and WPA2-Personal with CCMP.
Not supported:
- WEP, WPA1/TKIP
- WPA3/SAE, and networks that require management frame protection
- Enterprise (802.1X)
- hidden networks
- 802.11n/ac rates (QRT associates as an 802.11a/g station, up to 54 Mbit/s)

**Status: works on the tablet.** Scanning, WPA2 connection and DHCP were
confirmed on a Venue 8 Pro 5855 with 0.5.5.2. In 0.5.5, connecting froze
the tablet: the association response made the client send commands, and
while each command waited for its answer, the driver re-read the same
receive buffer and delivered the response again, recursing until the stack
ran out. The driver now queues received frames and hands them to the client
only from its poll loop.

QEMU has no 8260, so these parts are checked separately:
- The firmware file parses in QEMU.
- The WPA2 client logic passes a host test with a simulated access point
  (`tests/test_wlan.c`).
- The IP/TCP stack runs in QEMU on an e1000e driver: DHCP, DNS, `ping` and
  `wget` all work.

If a network does not connect, its log (**Wi-Fi → Log**) shows every step.
To save it:
1. Switch to **Settings → Kernel mode → Firmware** and reboot. In that mode
   the stick is writable.
2. Tap **Wi-Fi → Save log**. It writes `\qrt\hwdump\wifi.txt` to the stick.
   In firmware mode QRT also saves this file at every connection step, so
   the file survives even if the tablet hangs.

The firmware file (`\lib\firmware\iwlwifi-8000C-36.ucode`) comes unmodified
from linux-firmware, under Intel's redistribution licence; see
[firmware/](firmware/).

## Browser

**Browser** opens web pages over http:// and https://, as text and links.
There are no images, CSS or scripts. Its purpose is reading simple pages
and testing the network.

- **Address bar.** Tap it and type an address. Anything that is not an
  address is searched on [FrogFind](http://frogfind.com/), a search engine
  for vintage browsers whose result links pass through a proxy that strips
  pages down to plain HTML.
- **Pages.** It shows headings, paragraphs, lists, links, `<pre>`, tables
  (as lines), `<img alt>` text, Latin-1 and UTF-8 text, and HTML entities.
- **Forms.** GET and POST forms with text fields and submit buttons work,
  which covers search boxes such as DuckDuckGo Lite's.
- **Navigation.** Redirects and chunked replies are handled. Back keeps up
  to 32 pages, with their scroll positions.
- **The network code.** `src/net/http.c` is an HTTP/1.1 client. For
  https:// it runs over `src/net/tls.c`, QRT's own TLS 1.3 client
  (X25519 key exchange, AES-128-GCM). SHA-256, HKDF, X25519 and GCM are in
  `src/net/crypto_tls.c`, checked against the RFC and NIST test vectors.
  `make check-tls` tests the handshake against OpenSSL.

**Security: certificates are not checked yet.** An https:// page is
encrypted, so someone listening on the network cannot read it. But QRT
does not verify the server's certificate, so it cannot prove it is talking
to the real site. The status line says so on every https page. Do not
enter passwords.

Not supported yet:
- gzip-compressed replies (QRT asks for uncompressed pages, and most
  servers comply)
- servers that speak only TLS 1.2 or offer no X25519 key exchange
- cookies

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
- **gpio-buttons**: the Venue's power, volume and Windows buttons
- **backlight**: the panel backlight through LPSS PWM #1
- **iwm**: Intel Wireless 8260 (Wi-Fi)
- **e1000**: Intel 8254x/82574 Ethernet. This is QEMU's NIC; it is here so
  the network stack can be tested.
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
| Buttons | power, volume and Windows through GPIO, native mode. Power long-press, volume repeat and the Windows button are **new in 0.5.5.3, untested on hardware**. |
| Wi-Fi | Intel 8260 driver, WPA2-Personal: **works** (scanning, connecting, DHCP) |
| Backlight | LPSS PWM #1, native mode: brightness and sleep (**new in 0.5.5.3, untested on hardware**). QRT only takes control if the firmware left that PWM running. |
| Sleep | backlight off and a slower frame loop; not ACPI suspend |
| Audio | no driver: the volume keys drive a mock volume control |
| Bluetooth, camera, sensors, battery | no drivers yet (they need ACPI/PMIC support first; see docs/drivers.md) |
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
24 ms. Type `bench` in the launcher's search field to measure; the result
appears in **System → Graphics**.

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

1. Use `dist/qrt-0.5.5.3.img.gz`, or build the image with `make`.
2. Write it to a USB stick with Rufus or balenaEtcher, or on Linux:
   `gunzip -c dist/qrt-0.5.5.3.img.gz | sudo dd of=/dev/sdX bs=4M conv=fsync`.
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
make check      # host tests: HID parser, crypto vectors (WPA2 and TLS 1.3), WPA2 client vs a simulated AP,
                #             URL/HTTP parsing, HTML reader
make check-tls  # the TLS 1.3 client against a local OpenSSL server (needs openssl and python3)
```

OVMF has no touch driver, and native QRT has no USB keyboard driver yet. In
native mode, QRT therefore reads keys from the serial port: in the QEMU
window, choose *View → serial0* and type there. Arrow keys, Enter and Esc
navigate the UI. `tools/qemu-test.py` injects touch the same way. QEMU's
e1000e network card gets an address by DHCP at boot, so the Terminal's
network commands work there too.

## Layout

```
src/efi.h              UEFI ABI subset (written from the spec)
src/kernel/            boot, HAL, VFS, driver model (dev.c), hardware report, runtime
src/arch/x64/          native kernel: memory, CPU/IDT, APIC, I/O APIC + MSI, scheduler,
                       SMP trampoline, processes, Linux system calls
src/arch/x64/lsock.c   Linux sockets over the network stack
src/drivers/           PCI, UART, DesignWare I2C, HID over I2C, touch service, GPIO buttons,
                       e1000, iwm/ (Intel 8260 Wi-Fi), driver table
src/net/               802.11 client + WPA2 (wlan.c), ARP/IP/ICMP/UDP/DHCP/DNS (net.c), TCP (tcp.c),
                       HTTP client (http.c), TLS 1.3 client (tls.c), crypto (crypto.c, crypto_tls.c)
src/ui/                gfx (anti-aliased shapes, text), font atlases, shell (dock, launcher), on-screen keyboard
src/apps/              Files, Terminal, Browser (+ html.c), Wi-Fi, Sketch, Settings, System, Clock, Life, Touch Lab
firmware/              Intel 8260 firmware (Intel redistributable licence)
tests/                 HID parser, crypto vectors, WPA2 client (simulated AP), HTTP, HTML, TLS, Linux test program
tools/                 mkfont.py, mkimage.sh, run-qemu.sh, qemu-test.py, gen_isr.py
assets/                Inter (SIL OFL 1.1), DejaVu Sans Mono (Bitstream Vera licence)
```

Apps implement a five-function ABI (`open`, `draw`, `event`, `tick`, `icon`)
in `src/ui/shell.h`.

`/bin/busybox` on the image is an unmodified copy of Ubuntu's
`busybox-static`, licensed under GPL-2.0. It runs as a separate program,
and `\bin\BUSYBOX.txt` says where to get its source.
