# Drivers in QRT

This page covers how a driver attaches to hardware in QRT, what the kernel
gives it, and how the Linux driver ecosystem could be used. It describes the
current state and does not promise more than that.

## The model

`src/kernel/dev.h` defines two structures:

- **`device_t`**: one entry per device that the buses enumerate.
- **`driver_t`**: a name, match tables, `probe()`, and an optional `status()`.

The buses:

| Bus | Enumerated from | Device name |
|---|---|---|
| PCI | ECAM config space (ACPI `MCFG`), `src/drivers/pci.c` | `00:18.6` |
| ACPI | the `_HID`/`_CID` ids in the DSDT/SSDTs (`hwreport.c`) | `PNP0C50`, `808622C1` |
| platform | fixed legacy devices | `com1` |

`dev_init()` walks every device and binds the first driver whose table
matches and whose `probe()` does not answer `DEV_NOT_MINE`. Unbound devices
stay in the list. Under **System → Devices** they show up after the bound
ones with a plain description ("SD/eMMC host (SDHCI) – no driver yet"). On
the tablet, that list is the to-do list for native drivers.

The drivers built in today (`src/drivers/builtin.c`):

| Driver | Matches | What it does |
|---|---|---|
| `i915` | Cherry Trail graphics `8086:22b0–22b3`, native mode | Starts the Gen8 render engine (forcewake, GGTT, PPAT, workarounds, legacy ring, golden state, self-test) and presents frames with a 3D draw (`src/drivers/i915/gpu.c`). Since 0.6.4 also drives external monitors on DisplayPort B/C/D - the USB-C port's DP Alt Mode - and mirrors the screen to them (`display.c`). Code and data from Linux's i915 and intel-vaapi-driver (MIT). |
| `xhci` | PCI class 0c/03, prog-if 0x30, native mode | USB 3 host controller (`src/drivers/usb/xhci.c`): BIOS hand-off, Intel port routing, command/event rings, root-port enumeration, USB 2 hubs, boot-protocol keyboards, mice and other pointers from their HID report descriptors (`src/drivers/hidmouse.c`, boot protocol as a fallback). Reference: xHCI 1.1 and OpenBSD's xhci(4). |
| `bt` (via `xhci`) | USB class e0/01/01 (Intel 8260: 8087:0a2b) | Bluetooth (`src/drivers/bt/`): HCI over the control, interrupt and bulk endpoints; Intel bootloader firmware download (Secure Send of `ibt-11-5.sfi`, Intel Reset, DDC) after Linux's btusb.c/btintel.c; classic inquiry and LE scanning. Runs in its own kernel thread. |
| `audio` | `10EC5672`/`10EC5670`/`10EC5640`, PCI `8086:22a8` | Reads the Realtek codec's id registers over I2C2 (`src/drivers/audio.c`, after Linux's rt5670/rt5640); lists the SST DSP. |
| `framebuffer` | PCI class 03 | Takes over the linear framebuffer the firmware set up and marks it write-combining. |
| `uart16550` | `com1`, `PNP0501` | Kernel log and test input. IRQ 4 goes through the I/O APIC, and received bytes are buffered by the interrupt handler. |
| `dw-i2c` | Intel LPSS I2C, Bay Trail `8086:0f41–0f47`, Cherry Trail `8086:22c1–22c7` | The DesignWare I2C controller (`dwi2c.c`). |
| `i2c-hid` | `PNP0C50` | HID over I2C and the touchscreen service (`i2chid.c`, `hidparse.c`, `touch.c`). |
| `chipset` | PCI bridges, PCI roots, PIC/PIT/HPET, processors | Devices the kernel handles itself or leaves alone, each with its reason. |

## What the native kernel gives a driver

- **MMIO:** all physical memory up to 64 GB is identity-mapped, so a BAR
  address is a pointer. `pci_bar()` decodes 32- and 64-bit BARs. Device
  memory stays uncached because the firmware's MTRRs say so.
- **DMA memory:** `pmm_alloc(1)` returns one zeroed 4 KiB frame from the
  kernel pool, and `pmm_alloc_contig(n)` returns contiguous frames. Physical
  addresses equal virtual addresses, so the pointer is also the bus address.
  There is no IOMMU. The kernel pool starts at 1 GB and can reach above 4 GB
  on a 4 GB tablet, so a DMA engine that only takes 32-bit addresses needs a
  below-4-GB allocator. That allocator is not written yet.
- **Interrupts** (`src/arch/x64/irq.h`):
  - `irq_attach_isa(irq, …)` handles legacy lines. It applies the MADT's
    source overrides.
  - `irq_attach_gsi(gsi, level, active_low, …)` is for ACPI `_CRS`
    interrupts.
  - `irq_attach_msi(bus, dev, fn, …)` is for PCI devices. Prefer MSI, because
    it needs no routing tables.
  - Handlers run on the boot core with interrupts off, after the local APIC
    has been acknowledged. **System → Kernel → Interrupts** shows a live
    count for each line.
  - `irq_attach_msi` is written to the PCI spec but is not yet exercised by
    any device in QEMU. The I/O APIC path is tested: COM1 runs on it.
- **Threads:** `thread_create()`, `thread_sleep_ms()`, `thread_block()` and
  `thread_wake()` (`sched.h`). A driver that needs a bottom half wakes a
  thread from its interrupt handler.
- **Delays and time:** `k_delay_us()`, `k_now_us()` and `k_now_ms()`.

- **Polled drivers:** `hal_poll()` runs in the shell's main loop in both
  kernel modes. Drivers that do not need interrupt latency hook in there.
  The GPIO buttons, e1000 and the Wi-Fi driver all work this way, so they
  behave the same in firmware mode, where QRT owns no interrupts.
  `hal_dma_alloc()` gives page-aligned memory whose address is its bus
  address in both modes.

Not there yet:

- an ACPI interpreter (AML is scanned for ids only, never executed, so
  `_CRS`, `_PS0`, GPIO and PMIC methods are unavailable)
- GPIO interrupts (pads are polled: see `src/drivers/buttons.c`)
- ACPI methods such as `_BCM`: the backlight driver (`src/drivers/backlight.c`)
  writes the PWM that `\_SB.PCI0.GFX0` saves and restores, found through the
  firmware NVS variable `P10A`, instead of running AML
- runtime power management
- a block-device layer

## Linux compatibility

QRT has **Linux application** compatibility today. Static x86-64 ELF
programs run unmodified. `src/arch/x64/linux.c` implements the part of the
Linux system-call ABI that glibc, musl and busybox need:

- files, directories, `mmap`/`brk`
- TLS (`arch_prctl`)
- time, `uname`, `sysinfo`
- `getrandom`
- `AF_INET` sockets (TCP, UDP, ICMP), `poll`, `select` (`lsock.c`)

The Terminal app starts these programs, and the image includes a static
busybox. Still missing: `fork`/`exec`/`clone` (so no shell pipelines or
threads), signals, listening sockets, and dynamic linking.

## Networking

`src/net/` is QRT's own stack, written for the job:

| File | What |
|---|---|
| `net.c` | interfaces (`netif_t`), ARP, IPv4, ICMP, UDP, DHCP client, DNS resolver |
| `tcp.c` | TCP client: connect, go-back-N retransmission, FIN/RST |
| `wlan.c` | 802.11 station: scan, auth, association, WPA2-PSK handshakes, group CCMP |
| `crypto.c` | SHA-1, HMAC, PBKDF2, the 802.11 PRF, AES-128, key wrap, CCM |
| `netstack.c` | polling and the lock shared by the shell and Linux programs |
| `http.c` | HTTP/1.1 client for the browser: GET/POST, redirects, chunked replies |
| `tls.c` | TLS 1.3 client: TLS_AES_128_GCM_SHA256, X25519; certificates not verified yet |
| `crypto_tls.c` | SHA-256, HMAC, HKDF, X25519, AES-GCM |

A NIC driver fills a `netif_t` (MAC and a `send` hook), calls
`net_register()`, and passes received Ethernet frames to `net_input()`.
The Wi-Fi code converts between 802.11 data frames and Ethernet frames, so
the IP layer sees the same interface either way.

**Intel Wireless 8260 (`src/drivers/iwm/`).** This is a port of OpenBSD's
`iwm(4)`, which is ISC-licensed. Its register and command definitions
(`if_iwmreg.h`) are copied verbatim. `iwm_compat.h` provides the few BSD
types they need. The driver body keeps OpenBSD's structure and names for
these parts:
- firmware TLV parsing, section loading, firmware paging
- NVM
- PHY DB, calibration
- the MAC/PHY/binding/STA/TIME_EVENT commands
- UMAC scan
- the TX/RX rings

What changed from OpenBSD:
- **net80211 → `wlan.c`.** The net80211 state machine is replaced by the
  small client in `wlan.c`.
- **Polled.** The driver reads `CSR_INT` and the RX ring's `closed_rb_num`
  instead of taking MSI interrupts.
- **Deferred delivery.** Received 802.11 frames are queued and handed to
  the client only from `iwm_poll()`, never from the service routine.
  Command waits also run the service routine, and the client sends
  commands from its frame handlers, so direct delivery would nest. 0.5.5
  delivered directly, and connecting hung.
- **Rates.** Only legacy rates are used. HT/VHT and aggregation are left
  out.

The firmware is `iwlwifi-8000C-36.ucode` from linux-firmware. The image
loads it from `/lib/firmware`.

The client is tested on the host against a simulated access point
(`tests/test_wlan.c`). The IP stack is tested in QEMU on the e1000 driver.
The 8260 itself still has to be tested on the tablet.

**Linux drivers** are a different job. Drivers use the kernel's internal
API, not system calls. FreeBSD solved this with *LinuxKPI*, a layer that
re-creates Linux's internal API on top of FreeBSD's own kernel. It lets
FreeBSD run Linux's Intel/AMD GPU and Wi-Fi drivers mostly unchanged. The
plan for QRT is the same, in steps:

1. **Porting by hand (now possible).** A Linux platform/PCI driver's
   `probe()`, id tables and register code map almost one-to-one onto a
   `driver_t`. Only `devm_*`, `readl`/`writel`, `request_irq` and
   `msleep` change. This is how the DesignWare I2C and HID-over-I2C drivers
   were written. SDHCI (eMMC and microSD) and xHCI (USB) are the next
   candidates.
2. **A LinuxKPI-style shim.** This means headers that provide `struct
   device`, `platform_driver`, `pci_driver`, `request_irq`, `ioremap`,
   `dma_alloc_coherent`, `kmalloc`, spinlocks, mutexes, wait queues,
   workqueues, `jiffies` and timers, all built on the primitives above. With
   it, whole driver source files compile unmodified.
3. **Subsystem cores.** Wi-Fi (`cfg80211`/`mac80211`), sound (ALSA SoC) and
   DRM each need the Linux core layer they plug into. These are large, and
   they must be ported with their GPL-2.0 licence.

A general caution: much of the Venue 8 Pro's hardware is reached through
ACPI methods and the Crystal Cove PMIC. That includes the Wi-Fi's power
GPIO, the audio codec's clocks and the backlight PWM. An AML interpreter
(ACPICA is BSD/GPL dual-licensed and designed to be embedded) is therefore a
prerequisite for most of those drivers, whichever way they are written.
