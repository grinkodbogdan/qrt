# QRT

A small custom operating system for the **Dell Venue 8 Pro**, with a touch-first
desktop styled after GNOME (libadwaita, dark). Its kernel, **Tessera**, is
written from scratch. It is not Linux, but it can run Linux programs.

<p>
<img src="docs/screenshots/home-portrait.png" width="30%">
<img src="docs/screenshots/home.png" width="66%">
</p>

| | |
|---|---|
| ![open apps](docs/screenshots/overview.png) | ![settings](docs/screenshots/settings.png) |
| ![launcher](docs/screenshots/launcher.png) | ![keyboard](docs/screenshots/keyboard.png) |
| ![terminal](docs/screenshots/terminal.png) | ![network](docs/screenshots/terminal-net.png) |
| ![wifi](docs/screenshots/wifi.png) | ![system](docs/screenshots/system.png) |
| ![browser](docs/screenshots/browser.png) | ![browser over https](docs/screenshots/browser-https.png) |
| ![lock screen](docs/screenshots/lock.png) | ![volume](docs/screenshots/volume.png) |
| ![dragging the dock](docs/screenshots/dock-dragging.png) | ![dock on the left](docs/screenshots/dock-left.png) |

## The shell

- **Look.** Dark, flat and in GNOME's colours.
  - A black top bar shows the date and time in the middle and network and
    volume icons at the right.
  - Every app is a window: a header bar with its title, a minimise button
    and a close button.
  - Settings, Wi-Fi, Files and System Monitor follow GNOME's layouts:
    grouped rows, switches and sliders.
- **Open apps.** Swipe up on the home screen. Every open app appears as a
  picture of its window, taken when you last left it.
  - Tap a window to switch to it.
  - Tap its × or swipe it up to close it.
  - Tap the empty space to go back.
  - The launcher's search finds this view too ("Open apps").
- **Closing apps.** The × in a window's header bar closes the app; the –
  minimises it to the home screen. A closed app leaves the dock unless it
  is pinned. The Terminal ends its program and starts a fresh session.
- **Dock.** A rounded panel floating along one edge holds pinned apps
  (Files, Terminal, Browser, Wi-Fi, Drawing, Settings) and any others you
  open. A dot marks running apps. Tapping the app in front minimises it.
  - Drag the dock to move it. It stays under your finger at the spot you
    picked it up, and takes the shape it will have on the nearest edge.
    On release it glides to that edge (left, right, top or bottom).
  - While it moves, only the dock's own area is redrawn, not the whole
    screen, which keeps dragging smooth on the tablet.
  - The edge is saved in NVRAM, so the dock stays there after a reboot.
- **Launcher.** The 3×3 dots at the end of the dock open a grid of every
  app. Its search field also finds actions, such as open apps, rotate,
  restart and the touch fixes. Typing anywhere on the home screen opens
  it.
- **On-screen keyboard.** Tapping a text field brings it up: the launcher
  search, the Terminal command line, a Wi-Fi password, or the browser's
  address bar and form fields. It has letters,
  digits and two symbol pages, shift and caps lock, a repeating backspace,
  and cursor keys.
- **Hardware buttons** (Venue 8 Pro 5855, both kernel modes):
  - **power**: locks the screen. Pressed on the lock screen, the tablet
    sleeps; pressed while asleep, it wakes. Held for a second, it opens the
    power menu (sleep, restart, shut down, firmware setup).
  - **volume up / down**: change the volume and show an on-screen volume
    bar. Holding a key repeats. There is no sound driver yet, so this
    is a mock control: the level is kept and saved, but nothing plays.
  - **Windows button**: opens and closes the launcher (the dock's list of
    all apps).

  QRT reads them from three sources:
  - the input bit of the Cherry Trail GPIO pads the DSDT names. At start-up
    QRT switches each pad to GPIO-input mode, as Linux's pinctrl-cherryview
    does.
  - the GPIO controller's interrupt-status register, which latches every
    edge on those pads even when the input bit does not move.
  - for power, the ACPI fixed power button: this tablet's FADT declares
    one, so a press sets `PWRBTN_STS` in `PM1_STS` (I/O port 0x400). QRT
    switches the firmware into ACPI mode first (`SMI_CMD`), as an OS does,
    so that the firmware's SMI handler stops taking the press. This source
    reports presses only, so a power press it catches is always short.

  **Button test** (type "button" in the launcher) shows each source live.
  In QEMU, keys on the serial console stand in for the buttons: F9 and F10
  for volume, F11 for power, F12 for power held, F8 for Windows.
- **Animations.** Changes of view animate:
  - Apps rise in when they open and sink away when minimised or closed.
  - The launcher and the open-apps view slide up, and the lock screen
    fades in.
  - Swiping up for the open apps, and swiping up to unlock, follow the
    finger. On release the view finishes the move, or snaps back if it
    went less than a third of the way.
  - The dock and the top bar stay still while the content moves.
  - An animation frame only blends or shifts two finished pictures (the
    old screen and the new state, drawn once), on every core, so it costs
    about one copy of the screen.
- **Lock screen and sleep.** The lock screen shows the time and the date.
  Swipe up to unlock (or press Enter on a keyboard).
  - After a period without a touch or a button press, QRT locks and
    sleeps. Set the period in **Settings → Sleep after**: never, 30 s,
    1, 2 (the default), 5 or 10 minutes. On the lock screen it sleeps
    after 20 seconds.
  - Asleep, the backlight is off (on machines without backlight control,
    the screen is black) and the frame loop slows down. Wi-Fi stays
    connected.
  - Only the power or Windows button wakes the tablet; touch does not.
    On machines without these buttons, a touch wakes it.
  - This is not ACPI suspend: QRT cannot enter S0ix/S3 yet, so the
    tablet still draws more power asleep than it would under Windows.
- **Brightness.** **Settings → Power → Screen brightness** sets the
  backlight. It drives the SoC's PWM controller, which the DSDT links to
  the panel.
- **No boot text.** The boot log goes to the serial port and to **System
  Monitor → Log**, not to the screen.

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
| Time | TSC calibrated at boot, 1 kHz local-APIC timer; the wall clock from the RTC, set from the network (SNTP, or an HTTP `Date:` header where NTP is blocked) |
| Scheduling | preemptive threads, 10 ms round-robin; idle cores halt |
| Multicore | other cores started by QRT itself (INIT/SIPI through a real-mode trampoline) and used for rendering |
| Files | in-memory file system: the boot stick plus `/proc`, `/etc`, `/tmp`, `/dev` |
| Processes | ring-3 address spaces, ELF loader, demand paging, `SYSCALL` entry |
| Linux ABI | the system calls static glibc, musl and busybox programs need, including sockets and poll/select |
| Network | Ethernet/802.11 → ARP, IPv4, ICMP, UDP, TCP; DHCP client, DNS resolver, SNTP (`src/net/`) |
| Drivers | a driver model over PCI, ACPI and platform devices; see below |

The firmware is kept only for its *runtime* services: the RTC, NVRAM
variables (where settings are stored) and reset/power-off.

### The clock

Secure sites need the right date: a certificate is valid only between two dates, so a
clock in the past makes every https:// page fail with "SSL verification failed". The
Venue's RTC goes back to a default date (a day in January) when the battery runs
completely flat. QRT therefore:
- treats an RTC date before the build as a reset and starts from the build date;
- sets the time from the network as soon as it has an address: SNTP (`pool.ntp.org`,
  `time.google.com`, `time.cloudflare.com`), or, when NTP gets no answer, the `Date:`
  header of `http://www.google.com/generate_204`. It checks again every 6 hours;
- writes the corrected time back to the RTC, so the next boot starts right even offline.

The RTC holds local time, as Windows keeps it. When it was right, QRT learns the time zone
from it on the first sync. Otherwise, set the zone in **Settings → Date & time**, which also
shows whether the time came from the network.

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

QRT is 64-bit only (since 0.9.0): the 32-bit UEFI build for the Venue 8 Pro 5830 is gone.

### When something goes wrong (0.9.3)

- **A kernel bug hit through a program** (a fault in kernel code while it serves that
  program's system call, as in the browser page fault reported on 0.9.2) now stops only
  that program, like Linux's "oops": the browser closes, QRT and the other apps go on, and
  the log (System Monitor, the serial console) names the kernel function and its callers.
  The network lock it held is given back.
- **Anything else** (a fault in an interrupt handler, a driver thread, the shell) still
  stops QRT, but the "QRT stopped" screen now says what happened in words: the kernel
  function and its callers (QRT carries its own symbol table), the thread and program,
  the registers and the last lines of the log.  A photo of that screen is enough to find
  the bug.

## 0.21.11: a panic stays on the screen; touch positions kept on the panel

0.21.8 on the Mi A1 mounted every eMMC partition and kept the modem off - and still
reset, in the shell but not while the log was up.  The log was up when a thin red band
appeared: a Tessera panic.  While the log is up, input goes nowhere; in the shell, the
touch screen's events arrive - the panic came with them.

- **Touch positions are clamped to the panel** and the finger count to 0..5
  (`linux.c`): a controller reporting past the panel's edge, or a tracking count gone
  astray, reached the shell as it was.
- **A panic shows the log** (`main.c`, `logview.c`): its message, the registers and the
  program counter as an offset into the image (`in the image: pc +...`) are drawn on the
  screen, under a thin red band.
- **A panic no longer resets the phone**: it masked FIQs too, and Qualcomm's secure
  world services its watchdog on FIQs - so the phone reset seconds later (the "random
  reboots") and took the panic screen with it.  Now only IRQs are masked.

## 0.21.10: Linux keeps away from what needs the secure world

0.21.6's last log on the Mi A1 stopped right after the touch screen came up - not even
the 10-second CPU report followed - and the phone reset.  Besides the modem, Wi-Fi and
DSP (off since 0.21.8), the clock controller (0.21.5) let more of Linux's drivers start
that hand requests to Qualcomm's secure world, which resets the phone when one does not
suit it.  QRT has no use for them yet, so Linux does not see them (`main.c`, the same
way as the display): the IOMMU, the GPU, the video codec, the modem, Wi-Fi and the audio
DSP.  `qrt.allhw` on the command line gives them back.

## 0.21.9: the log shows whether touches arrive

The first touch data a touch screen sends and the first five presses are logged
(`linux: first touch data from ...`, `linux: touch down at x,y`): touching the screen
with the log up tells whether the controller sends nothing or the presses get lost on
the way to the shell.  Built on 0.21.8 (the modem, Wi-Fi and DSP stay off - starting
them reset the phone in 0.21.6).

## 0.21.8: touch is up - the modem stays off; a reset leaves its log on screen

0.21.6 on the Mi A1: `event3 (generic ft5x06) ... (touch)` - with the clock controller
up (0.21.5) the touch screen's driver binds.  A few seconds later the screen faded to
black.  The eMMC controller probes now too, so Argon's storage thread mounted the modem
partition and started the phone's remote processors (modem, Wi-Fi, DSP) - through the
secure world, which resets the phone when a start goes wrong: the likely end of that
boot.

- The remote processors stay off unless `qrt.remoteproc` is on the command line
  (Wi-Fi and the modem have no use in QRT yet); each start is logged before it happens.
- A boot that ended without QRT shutting down or restarting (a reset, a crash, the
  power key held) has its log shown at the next boot (`plog.c`): what came last before
  it ended is on the screen for a photo.

## 0.21.7: no sleeping without a touch screen

The shell no longer puts the screen to sleep while Linux has found no touch screen
(`shell.c`, `linux.c`): with nothing to touch, the idle timeout would only hide the log
that says why.  Also in this build: 0.21.6 (the display stays Tessera's) and 0.21.5
(the clock controller waits for its power domain - the I2C bus the touch screen is on
needs it).

## 0.21.6: the display stays Tessera's; Linux has the rest

0.21.5 on the Mi A1: with the clock controller finally up, Linux's MSM display driver
probed too - it reset the panel (the picture faded to black within seconds) and did not
bring it back.  Until it does, the display is Tessera's: it draws into the boot loader's
display as 0.17.0 and 0.21.4 did (clean), and Linux's display driver does not see the
display subsystem (`main.c`: its `qcom,` compatibles become `qrt-,`, so no Linux driver
matches).  Linux keeps everything else - touch, keys, power, storage.  To try Linux's
display driver anyway: `fastboot boot -c "<the usual command line> qrt.linuxdisplay" ...`.

Also fixed: 0.21.3 meant to keep Linux's `msm_serial` off the UART Tessera logs to by
writing "disabled" over its "okay" - which does not fit, so it never happened; the UART
is hidden the same way as the display now.

## 0.21.5: the clock controller waits for its power domain - I2C, touch and the rest follow

0.21.4 on the Mi A1: a clean screen, fast keys - and no touch.  Its log listed 12
devices waiting (both I2C buses, the DMA engines, USB, the interconnect, SCM, the
IOMMU, the modem) and no I2C or eMMC controller bound; everything waiting takes its
clocks from the clock controller (GCC), which was neither bound nor waiting.

The cause: GCC (and the eMMC controller) sit in the RPM's CX power domain, and the
RPM's power domains register late - once the RPM's channel is up, after Linux's
initcalls.  A device whose power domain is missing by then is told "assuming no
driver" (`-ENODEV`, printed only as a debug line) and never probed again.  Real Linux
does not get there: its device links (`fw_devlink=on`) hold GCC back until the power
domain exists; QRT boots Linux without them.  Now (LKL `drivers/pmdomain/core.c`) a
missing power domain means "wait" inside QRT, and GCC probes when the RPM is up - and
with it the I2C bus the touch screen is on.

The log's summary now also keeps probes that gave up quietly (`probe of ... returned
-19`, "ignoring dependency") and says whether `gcc-msm8953` bound.

## 0.21.4: the boot loader's display as 0.17.0 drove it

0.17.0 showed a clean screen on the Mi A1; the scrambling came with the display changes
of 0.19.x.  Until Linux's display driver takes the panel (and in a safe boot), Tessera
now drives the boot loader's display the way 0.17.0 did (`fb.c`, `main.c`):

- **No MDP register writes.**  Tessera no longer switches the boot loader's other layers
  off (0.19.6) nor turns on underrun interrupts (0.19.8): it only draws into the buffer
  the splash pipe scans out.  The other layers' memory stays out of Tessera's heap, so
  they keep what the boot loader drew.  (A command-mode panel still gets its START.)
- **The framebuffer is uncached again**, as in 0.17.0 (it was write-back with cache
  cleaning since 0.19.8); rows still go out as 8-byte stores, built in cached memory.

Checked in QEMU with a 3-byte-pixel framebuffer like the Mi A1's, with Linux off.

## 0.21.3: a serial console on the Mi A1; booting through lk2nd

- **Serial console** (`main.c`): Tessera writes its whole log - Linux's lines too, from
  the first line of the boot - to the MSM8953's debug UART (BLSP1 UART1 at `78af000`,
  115200 8N1), the way Linux's `msm_serial` drives it.  Only when the boot loader left its
  clocks running (an unclocked UART can hang the bus); the log says which.  On the Mi A1
  the UART's TX/RX are test pads on the board: a 1.8 V USB serial adapter there shows
  where a boot stops even when the screen shows nothing.  While Tessera uses the UART,
  Linux's own driver for it is off.
- **lk2nd** (postmarketOS's boot loader for MSM8953 phones) should work as the boot loader (not yet tried):
  it names the panel in the device tree itself (the same three panels, the same names
  Tessera uses from the stock boot loader's arguments).  To boot QRT the way
  postmarketOS boots: `fastboot flash boot lk2nd.img` (from lk2nd's releases; the stock
  boot image can be flashed back), reboot into lk2nd's fastboot (volume down), then
  `fastboot boot qrt-<version>-mi-a1-boot.img`.  QRT does not need it.
- **Debugging in QEMU**: QEMU has no MSM8953, so the phone's own drivers cannot run
  there; the QEMU `virt` machine runs the same Tessera and Argon (Linux's virtio drivers)
  and is where 0.21.1's scheduler, interrupt and register bugs were found, with QEMU's
  monitor sampling where the CPU was.

## 0.21.2: Linux is not switched off by a reboot; the display keeps its bandwidth

- **Safe boot only after two failed Linux starts** (`plog.c`, `linux.c`): a boot counted
  as failed unless it lived 40 s - holding the power key to get back to fastboot, or
  restarting early, made the next boot a safe one with Linux off: Tessera's own display
  (the scrambled one) and no touch at all.  Now Linux counts as started the moment its
  kernel is up (about 10 s), a reset before that point is not Linux's, and safe boot needs
  two failed starts in a row.
- **The display engine keeps the boot loader's memory bandwidth** (LKL
  `drivers/interconnect/core.c`): once probing ended, Linux's interconnect driver dropped
  every bus to what had been voted for - before the display voted, the display engine
  starved and underran (the flat bands).  Inside QRT the boot-time maximum now stays.

## 0.21.1: three bugs under Linux's drivers - starved timers, lost interrupts, clobbered registers

0.21.0 on the Mi A1: the screen stayed scrambled and touch did nothing.  Three bugs in
Tessera itself, under every driver Argon runs, found in QEMU:

- **The scheduler starved threads** (`sched.c`): each tick the shell, woken, ran first;
  when it slept again the round robin went on from the shell's place in the ring - so a
  busy Linux thread, which follows it, always came next, and the threads between (Linux's
  timer thread, its interrupt poller) waited seconds for a turn.  Linux's clock, its
  timeouts, the touch screen's interrupt: all late or never.  The round robin now goes on
  from the thread the shell interrupted.
- **Linux's interrupts waited for a busy Linux thread** (`linux.c`, LKL `irq.c`): LKL
  takes an interrupt only when the thread holding Linux's CPU lets it.  A driver that
  waits on `jiffies` without `cpu_relax()` (RAID6's speed test was one) waited forever.
  Tessera's tick now runs Linux's pending interrupts on top of Linux code, as a CPU would
  (only in Linux's own code, only with Linux's interrupts on).  The RAID6 test that hung
  0.21.0 in QEMU now finishes in under a second.
- **A preempted thread lost its FP/SIMD registers** (`boot.S`): the interrupt entry saved
  only the integer registers; the compiler uses q0-q31 in copies and pixel loops, and the
  next thread to run could change them under the preempted one - in Linux's drivers or in
  the shell's drawing.  All 32, FPSR and FPCR are saved now.

Also: Tessera no longer turns on the display engine's underrun interrupts once Linux's
display driver is starting (they would reach Linux's handler unasked), and it keeps
waiting for Linux's display (`/dev/fb0`) past 60 s instead of giving up.

## 0.21.0: Argon - Linux's drivers for the whole Mi A1

Tessera stays the kernel; **Argon** is a layer inside it that runs the Linux kernel (as a
library, at the same privilege, like an Android phone's vendor kernel under a different
system) with **every driver postmarketOS builds for the MSM8953** - 1121 options from its
`msm8953` kernel config, and postmarketOS's own device tree for the Mi A1 (`tissot`).
Tessera's own Qualcomm code no longer drives the hardware; Linux's does, and Tessera
talks to it the way a Linux program would.

- **Linux gets real memory management** (`CONFIG_MMU=y`): Tessera maps Linux's memory into
  a window of its own address space (`mmu.c`: `argon_map`/`argon_unmap`), and Linux's DMA
  addresses are translated back to physical ones, so the IOMMU, the display engine and
  virtio all see real memory.
- **The screen**: Linux's MSM display driver (MDP5 + DSI + the panel, chosen from the
  boot loader's `mdss_dsi_*` argument: otm1911, ili7807 or ft8716) owns the panel.
  Tessera draws into a shadow copy and a thread writes the changed rows to `/dev/fb0`.
  No more Tessera code poking the display engine - the scrambling came from that.
- **Touch, buttons, sensors**: Linux's input drivers, read through evdev.
- **Storage**: every partition Linux can read (ext4, f2fs, vfat, exfat) appears read-only as
  `/mnt/<partition>`.
- **Firmware**: taken from the modem partition's `image/` folder; the modem, Wi-Fi and
  other remote processors are started once it is there.
- **Battery**: from Linux's power-supply class (top bar and Settings).
- **Not yet**: Wi-Fi networking (Linux's side has no supplicant yet), sound through Tessera's
  mixer, the GPU (no firmware on the phone), auto-rotation.

Fixed on the way: a Linux boot that hung on its RAID6 speed test (the test busy-waits on a
timer tick that cannot arrive while it runs; the test is off).  Tested in QEMU with Linux's
own virtio-gpu driver showing the shell and taps reaching it.

## 0.19.8: lighter framebuffer writes; underruns counted; a scrollable log

The Mi A1's screen still showed flat-coloured bands in its lower part.  On a video-mode
panel that is what an **underrun** looks like: the display engine cannot fetch the frame
from memory in time and fills the rest of it.

- **The framebuffer is cached** (`mmu.c`, `fb.c`): Tessera wrote it with millions of
  8-byte uncached stores per frame, each its own trip to memory, competing with the
  display engine.  It is now mapped write-back and each written row is cleaned to memory
  (`dc cvac`): whole 64-byte lines, far fewer memory transactions.
- **Underruns counted**: the display engine's underrun flags (INTF0..3) are watched; the
  `cpu: display:` line in the log says how many frames underran.
- **The log scrolls**: with the log up, volume down goes a screen further back (past the
  oldest line, back to the newest); the summary repeats the boot's `display:` and `gic:`
  lines.

## 0.19.7: one layer on the Mi A1's screen

0.19.6 on the Mi A1: parts of the screen showed old pictures - the dock where it had
been, dimmed wallpaper, pieces of earlier frames - under the live one.  Tessera's
drawing is right: the same build at the phone's 1080 x 1920 and 3-byte pixels
(`qrt.fb=1080x1920 qrt.fb24` on QEMU's ramfb) drags the dock cleanly, and the copy into
the framebuffer checks out against a reference over thousands of random rectangles.

- **The boot loader's other layers** (`fb.c`): the display engine's mixer can blend
  several source pipes; the boot loader can leave more than one on (a logo, a notice
  over its splash), each scanning its own memory.  That memory is ordinary RAM to
  Tessera, whose heap later holds its wallpaper and frames there - which then showed on
  the screen.  At boot Tessera now logs every staged layer (`display: CTL... layers`)
  and switches off all but its own (`display: the boot loader's layer ... switched off`);
  their memory is also kept out of the heap in case one stays on.
- The `display:` line names the panel the boot loader reported.
- `qrt.fb24` on QEMU: a 3-byte-pixel ramfb, the Mi A1's pixel format, for testing.

## 0.19.6: the boot loader's clocks stay

0.19.5 on the Mi A1: the screen still scrambled and drags still broke the touch screen.
postmarketOS's boot loader project (lk2nd) lists the Mi A1's three panels (ILI7807,
OTM1911, FT8716): all are **video mode** - the display engine reads the framebuffer
continuously, so a screen that stays scrambled means it could not read memory in time.
Both troubles began when Linux's RPM drivers came up (0.19.2).

- **RPM clock scaling off** (`drivers/clk/qcom/clk-smd-rpm.c` in `qrt.patch`): Linux's
  RPM clock driver switched on clock scaling, from which point the RPM sets the bus and
  memory clocks from the processor's votes - and Tessera's Linux has no display or
  interconnect driver to vote for the display engine's bandwidth, nor for the buses the
  I2C controller sits on.  Under QRT scaling stays off: the RPM keeps the boot loader's
  clocks.  The RPM's regulators (the touch screen's supply) are separate and unchanged.
- **Panel mode from the boot loader** (`fb.c`): the boot loader names the panel on the
  command line (`..._fhd_video`); that now decides video or command mode, and all four
  display interfaces are checked.

## 0.19.5: whole frames on the panel; I2C that does not wait on a lost interrupt

0.19.4 on the Mi A1: touch worked, but a drag now and then ended in
`qup: timeout: no interrupt` (the controller with bytes still to send, nobody serving it
for 2 s); and after some changes the lower part of the screen kept an older picture.

- **Whole frames** (`fb.c`): a 1080x1920 frame takes about 15 ms to go to the panel, and
  0.19.1's display thread sent START every 16 ms - a new START cut the frame going out,
  so the lower part of the screen was never refreshed.  START now goes at most every
  40 ms while frames come, and once more 80 ms after the last.
- **I2C** (`i2c-qup.c` in `qrt.patch`): besides its interrupt, a transfer now looks at the
  controller itself every 10 ms and serves it, so a late or missed interrupt costs 10 ms,
  not a 2-second timeout and a lost touch report.

## 0.19.4: reliable I2C for the touch screen

0.19.3 on the Mi A1: the touch controller's probe failed with -ETIMEDOUT after 15 s
(0.19.2's log already had "Unable to fetch data, error: -110" from it).

- **I2C by the CPU** (`drivers/i2c/busses/i2c-qup.c` in `qrt.patch`): the QUP I2C
  controller moved longer transfers (the touch reports) with the BAM DMA engine, and
  those timed out.  Under QRT it now always uses its FIFO, filled and drained by the CPU
  on the controller's own interrupt.  A transfer that still times out is logged with the
  controller's state.
- **udelay** (`linux.c`, `main.c`): Linux's busy-wait delays read the host's clock, which
  counted whole microseconds - `udelay(1)`, which the I2C driver waits with for a state
  change, could return at once.  The clock is now the ARM counter in nanoseconds.
- The summary lists the device interrupts Linux has received (SPI and count), so a
  device whose interrupt never comes shows.

## 0.19.3: a clean screen and quicker touch on the Mi A1

0.19.2 on the Mi A1: the screen kept up, the RPM's 22 regulators came up and with them
the touch screen (FT5x06 on I2C) - but the screen was scrambled and touch was slow.

- **Scrambled screen** (`fb.c`): the shell presents a rectangle of its frame and passes
  the whole frame; the rectangle's pixels are at the same place in it (as GOP's Blt and
  the x86-64 kernel do).  The ARM kernel copied from the frame's corner instead, so
  every partial update put the wrong pixels on the screen - only full-screen frames
  were right.
- **Quicker touch** (`gic.c`, `sched.c`): an idle CPU woke only at the 10 ms tick, so the
  interrupt thread that hands Linux its interrupts ran every 10 ms instead of every
  1 ms, and each I2C transfer to the touch controller waited for it.  The idle loop now
  sets the timer for the soonest sleeping thread.

## 0.19.2: the scheduler's tick on the Mi A1

The Mi A1 was slow at everything - the screen, the keys, Linux - with the CPU 99% idle.

- **The tick** (`gic.c`): Tessera took the virtual timer as interrupt 27, its usual number
  (and QEMU's).  Qualcomm's MSM8953 numbers its timer PPIs differently: the device
  tree's timer node says the virtual timer is PPI 4, interrupt 20.  Tessera enabled the
  wrong interrupt, so on the phone there was no 10 ms tick at all: no preemption, and an
  idle CPU slept in `wfi` until some unrelated interrupt woke it - the shell's frames,
  Linux's timers and its interrupt thread all ran late.  The interrupt now comes from
  the device tree; the log's `gic:` line names it, and the `cpu:` lines count the ticks
  (about 100 a second).  Until a first tick arrives the idle loop watches the clock
  instead of sleeping.
- **Probing in order** (`drivers/base/dd.c` in `qrt.patch`): drivers that prefer to probe
  in the background (the RPM's regulators among them) now probe in order like the rest;
  0.19.1's log showed the RPM answering requests but its regulator driver with no device.
  The summary lists the RPM, I2C and SD drivers one per line and keeps their probe
  results.

## 0.19.1: the screen keeps up; tracing the RPM's regulators

- **Stale screen on the Mi A1** (`fb.c`): its panel is in command mode - it shows memory
  only when the display engine is told to send a frame (CTL START), and a START sent
  while the previous frame is still going out is lost.  The newest frame could then stay
  unseen until the next change (half-old screens after a volume change).  A display
  thread now sends START every 16 ms for half a second after each change, and once a
  second otherwise.
- **The RPM link** (0.19.0's log): SMEM is found, the `rpm_requests` channel opens and the
  RPM driver starts, but the RPM's regulators never appear (2 regulators in all).  This
  release logs the first RPM requests (sent, answered), a wait for room in the SMD FIFO,
  and in the summary how many devices each of the RPM regulator, RPM clock, RPM power
  domain, I2C and SD drivers has.

## 0.19.0: the Mi A1 bring-up so far

Where the Mi A1 stands, with Linux's own drivers running inside Tessera (LKL):

- **Working on the phone**: Linux boots in under a second; gpio-keys (volume up), the
  PMIC's power key and volume down, the WLED backlight; Tessera's log page, the safe
  boot after a reset, the log closing by itself.
- **Since 0.18.6**: thread wake-ups are on time (Linux's 30-second regulator cleanup now
  runs at 33 s, not 173 s).  0.18.7's faster screen copy and the shell-first scheduling
  are in this release.
- **Not yet**: the touch screen and the eMMC/SD card.  Both need the RPM co-processor's
  regulators; 0.18.6's log shows the touch screen waiting with "failed to request
  regulator".  This release logs each step of the RPM link (SMEM found, SMD channels
  found and opened, the RPM's requests channel up, a request timing out) and counts the
  regulators Linux has, in the summary 25 seconds after boot.
- **Why LKL** rather than writing drivers the way Haiku, Redox or Neptune OS do: those
  need a driver written for every chip; none of them drives a Qualcomm phone.  LKL runs
  Linux's own, already working drivers for this phone.

## 0.18.7: a faster screen on the Mi A1, and where the CPU goes

On the Mi A1 the keys worked but the screen showed changes long after they happened.

- **Display** (`fb.c`): the phone's framebuffer is 3 bytes a pixel in uncached memory, and
  every frame was written a byte at a time (about 6 million uncached stores).  Each row
  is now converted in cached memory and stored 8 bytes at a time.
- **The shell first** (`sched.c`): when the shell wakes (its next frame is due, or an
  event it waited for came), it runs next instead of waiting its turn behind every ready
  Linux thread.
- **CPU report** every 10 seconds in the log: the shell's, idle's and the other threads'
  share of the CPU (the busiest three by name), the timer ticks (100 a second when
  preemption works), thread switches, and how many frames reached the screen and how long
  each took to copy.

## 0.18.6: the RPM link, and a summary of what is missing

0.18.5 on the Mi A1: gpio-keys, the power and volume-down keys (PM8953 PON) and the
WLED backlight came up; the touch screen kept deferring and both MMC controllers (eMMC, SD card) failed
with -ENODEV.  All of those need the RPM co-processor (the touch screen's l10 supply,
the eMMC's supplies, clock and power domain) - and the RPM link was never made.

- **SMEM** (`drivers/soc/qcom/smem.c` in `ports/lkl/qrt.patch`): the shared memory the
  RPM link lives in is a reserved-memory node; its driver looked it up in Linux's
  reserved-memory table, which LKL never fills, and gave up.  It now falls back to the
  node's own address.  Everything above SMEM (the RPM's SMD channel, its regulators,
  clocks and power domains) depended on it.
- **A summary in the log** 25 seconds after boot, just before the log page opens: each
  device still waiting and what it waits for (Linux's `devices_deferred`, debugfs) and
  Linux's error lines, so one photo of the log's tail says what is missing.

## 0.18.5: the last boot's log closes by itself

- The log page that opens by itself (the last boot's log after a reset, or the current
  log when there is no touch screen) now **goes away after 30 seconds**, with a
  countdown in its title; on the Mi A1 volume up could not close it.  Opened by hand
  (volume up x3) it stays until closed the same way.
- A safe boot marks itself finished, so the boot after it tries Linux again even if
  the phone is reset during the safe boot.

## 0.18.4: GPIO interrupts, and the shell never waits for Linux

0.18.3 on the Mi A1: nothing responded, not even volume up - the shell itself had stopped.
Its log (the safe-boot page) showed `gpio-keys: error -ENXIO: Unable to get irq` and the
touch screen (1-0038) deferring.

- **GPIO interrupts** (`ports/lkl/qrt.patch`, `arch/lkl/kernel/irq.c`): LKL gave every
  one of its 4096 Linux interrupt numbers a placeholder handler at boot, which marks them
  all allocated - so an interrupt controller that allocates numbers as it goes (the
  phone's TLMM pin controller, the PMIC's SPMI controller) got none.  Now LKL keeps only
  the 64 it uses itself.  The volume key, the touch screen's interrupt and the PMIC's
  interrupts all come through these controllers.  Reproduced and fixed in QEMU with
  Linux's PL061 GPIO driver: the power key now goes GPIO interrupt -> gpio-keys ->
  evdev -> the shell (it locks the screen).
- **Input devices are opened once**: the first file Linux opens is fd 0, which the
  evdev scan took for "not open", so it opened that device again and every key from it
  arrived twice.

- **Backlight** (`stubs.c`, `linux.c`): the shell used to set the brightness through
  Linux's sysfs on its own thread; while Linux was busy probing, that call waited for
  Linux, and the whole UI with it.  The shell now only records the level; a separate
  thread applies it.
- **Interrupts** (`gic.c`): every interrupt the boot loader left enabled is disabled, and
  every device interrupt is put under the CPU interface's priority mask, before Tessera
  enables interrupts.  One that still reaches the CPU is masked and logged instead of
  firing again and again.
- **Volume up** stays native until Linux has actually delivered a key, not just until
  Linux's gpio-keys device exists.
- Tested in QEMU: a tap goes through Linux's virtio-input driver into the shell.

## 0.18.3: preemption, sturdier interrupts, the log when touch is missing

0.18.2 reached the home screen on the Mi A1 but nothing responded.

- **Preemptive threads** (`sched.c`, `gic.c`): the ARM generic timer interrupts every
  10 ms and the scheduler gives the CPU to the next ready thread.  Before, a Linux driver
  that spins on a register (common on real hardware) kept the shell, the timer thread
  and the interrupt thread from running at all.  Every scheduler, semaphore and mutex
  operation now runs with interrupts masked; each thread keeps its own interrupt mask
  across a switch.
- **Interrupts**: Tessera owns the GIC's CPU interface (its timer, PPI 27).  Linux's
  device interrupts (SPIs) sit under the CPU interface's priority mask, never
  interrupting the CPU; the interrupt thread finds them pending and enabled in the
  distributor and runs Linux's handlers (edge-triggered ones are cleared first).  The
  LKL irqchip no longer touches the CPU interface, SGIs or PPIs.
- **Volume up keeps working** natively until Linux's gpio-keys device exists, so the log
  page (volume up x3) always opens.  **No touch screen 30 seconds after boot: the log
  opens by itself** - a photo of it is what is needed.
- Tested in QEMU: Linux boots on the preemptive threads; a tap goes through Linux's
  virtio-input driver into the shell.

## 0.18.2: the Mi A1 reset, found

0.18.0 and 0.18.1 reset the Mi A1 just after the home screen appeared.  The cause was in
Tessera's own memory map, not in Linux:

- **The kernel's size was read as an address.**  The arm64 kernel is position-
  independent, and `_image_size` - a linker symbol - was relocated like an address
  (load address + size).  On the phone (loaded at 0x80008000) "the kernel image" so
  became about 0x80008000 to past 4 GB: mapped cacheable across the modem's, TrustZone's
  and the DSPs' memory, swept by a cache-maintenance loop, and kept out of the heap.
  With 0.17.0's small image nothing touched it; with Linux inside (18 MB) the phone's
  memory protection reset it.  The size is now `__bss_end - _start`, and the Image
  header carries the true size (it said 0, so the boot loader did not know how much
  room the kernel needs).
- **Reserved memory is mapped to the page.**  Where a reserved region's edge falls
  inside a 2 MB block, that block is mapped in 4 KB pages, so shared memory Linux reads
  (SMEM, uncached) never shares a block with TrustZone's memory next to it.  Checked in
  QEMU with the Mi A1's own device tree (`qrt.mmucheck`: every reserved region's edges).
- Safe boot also follows a boot that reset before Linux had even started.  The boot
  options `qrt.nolinux` (Linux off) and `qrt.mmucheck` are for testing.

## 0.18.1: the Mi A1 reset - the likely cause, and a log that survives it

0.18.0 showed the home screen on the Mi A1 and then the phone reset.

- **The likely cause**: Linux's RPM power-domain driver (rpmpd), once boot settles
  (`sync_state`), lowers the votes for the SoC's CX and MX rails - logic and memory - to
  what Linux's own drivers asked for.  Inside QRT most of the SoC's drivers do not run,
  so that is nearly nothing: the rails drop under the CPU and the phone resets.  The LKL
  patch now keeps the boot loader's votes (`rpmpd_sync_state` does nothing under
  `LKL_QRT_SOC`).
- **A log that survives a reset** (`src/arch/arm64/plog.c`): every log line is also
  written to the device tree's `ramoops` region (1 MB at 0x9ff00000 on the Mi A1, mapped
  uncached), which a phone's RAM keeps through a reset, with a note of how far the boot
  got.
- **Safe boot**: if the last boot reset while Linux's drivers were starting, the next
  boot leaves Linux off and opens the last boot's log by itself (its end is where the
  phone reset; Linux now logs every driver it starts - `initcall_debug`), so the next
  boot after that tries Linux again.
- Tested in QEMU: a reset while Linux starts -> the next boot is a safe boot showing
  the last boot's log; a normal boot still runs Linux's drivers (a tap travels Linux's
  virtio-input driver into the shell).

On the phone: `fastboot boot qrt-0.18.1-mi-a1-boot.img`.  If it still resets, run the
same `fastboot boot` once more: that boot is a safe boot and shows the last boot's log
full screen - a photo of it shows which driver reset the phone.

## 0.18.0: Linux's drivers inside Tessera on the Mi A1

The Mi A1's hardware - touch screen, keys, backlight, eMMC - sits on the Snapdragon 625
and only works after clocks, pins, power rails (through the RPM co-processor) and buses
are set up.  0.18.0 brings that up **with Linux's own Qualcomm drivers, running inside
Tessera's kernel**: the Linux 6.12 kernel as a library (LKL), built for 64-bit ARM
(`ports/lkl-arm64.sh`) and linked into the arm64 `Image`.

- **Linux gets the boot loader's device tree** and populates the SoC's devices, then its
  drivers bring the phone up as they do on Linux: GCC clocks and resets, TLMM pins and
  GPIO interrupts, SMEM / SMD / the RPM and its regulators and power domains, SPMI and
  the PMICs, BLSP I2C with BAM DMA, the FocalTech touch screen, gpio-keys (volume up),
  the PMIC power and volume-down keys, the PMI8950 WLED backlight, SDHCI (eMMC, SD).
  The panel itself stays as the boot loader set it up: Linux's display driver is not
  built, and `clk_ignore_unused pd_ignore_unused regulator_ignore_unused` keep Linux from
  switching off what it does not know is in use.
- **What Tessera gives Linux** (`src/arch/arm64/linux.c`, `sched.c`): threads
  (a cooperative scheduler: Linux runs while the shell waits for its next frame),
  semaphores, mutexes, timers; RAM below 4 GB; device registers mapped 1:1 so physical
  addresses are DMA addresses; an uncached pool for coherent DMA (the SoC's DMA does not
  snoop the caches) with cache maintenance for streaming DMA; Qualcomm shared memory
  (SMEM) mapped as memory.
- **The LKL patch** (`ports/lkl/qrt.patch`, `LKL_QRT_SOC`): the device tree
  (unflattened from the host's), `qrt-gic` - an irqchip for the GICv2 distributor whose
  interrupts Tessera takes from the CPU interface and hands to Linux - non-coherent DMA
  with a global coherent pool, SMC calls, `ARCH_QCOM` so the Qualcomm drivers build, and
  an idle hook so a cooperative host's timer thread runs while Linux idles.
- **Bridges**: Linux's input devices (evdev) become the shell's touches and keys (power
  -> lock / screen off, volume keys); the brightness slider and screen-off drive Linux's
  backlight; System shows "Linux drivers" and the whole kernel log (600 lines), Linux's
  boot messages included - the log to photograph when something does not work.
- A fault in a Linux driver stops Linux only: the shell keeps running so the log can
  be read.
- **Tested in QEMU** (virt, Cortex-A53, GICv2): Linux boots inside Tessera, its
  virtio-mmio driver finds the keyboard and tablet through the device tree and the GIC,
  and a tap travels Linux's virtio-input driver -> GIC interrupt -> evdev -> the shell
  (Settings opens).  The Mi A1 path (the Qualcomm drivers) needs the phone.

### On the Mi A1

`fastboot boot qrt-0.18.0-mi-a1-boot.img` as before (nothing is written).  After the
home screen appears, Linux needs a few seconds to bring the drivers up; then touch,
the power key (screen off/on) and the volume keys should work.

**Press volume up three times** (within two seconds) for the full-screen kernel log -
it needs no touch; three more presses go back.  If something does not work, a photo
of that page (the last "linux:" lines) is what tells me why.

## 0.17.0: Tessera on 64-bit ARM, a first image for the Xiaomi Mi A1

Tessera now also builds for **64-bit ARM** (`make arm64` -> `build/arm64/Image`), and
there is a first boot image for the **Xiaomi Mi A1** (`make mi-a1` ->
`build/arm64/qrt-mi-a1-boot.img`, shipped as `dist/qrt-0.17.0-mi-a1-boot.img`).

- **The same shell and apps**: the shell, the apps, the network stack (TCP, TLS,
  HTTP), the VFS and the sound mixer are shared code; `src/arch/arm64` is the new
  platform layer: a Linux-style arm64 `Image` (so Android boot loaders and QEMU load it
  like a Linux kernel), position-independent and self-relocating, entered at EL2 or EL1
  with the device tree in x0.
- **Memory** from the device tree: RAM from `/memory`, minus every `/reserved-memory`
  region (on a Qualcomm phone the modem, TrustZone and DSP firmware live there - those
  blocks are mapped as device memory so the CPU never reads into them speculatively);
  an identity-mapped MMU with caches; the heap and page allocator in the largest free
  stretch.
- **Display**: on QEMU, ramfb; on Qualcomm phones the panel the boot loader left on:
  Tessera reads the MDP5 source pipe's registers (address, size, stride, byte order) and
  draws into that buffer in its format (RGB888), mapped uncached; a command-mode panel is
  told to refresh after each frame; if the registers say nothing, the device tree's
  continuous-splash region is used.
- **Input**: QEMU's virtio keyboard and tablet (the tablet plays the touch screen); on the
  Mi A1 the volume-up key (TLMM GPIO 85, from the device tree's gpio-keys).
- **Clock**: the ARM generic timer; the wall clock from a PL031 RTC (QEMU).
- One core and no interrupts yet - the shell polls, as Tessera did on the firmware.
- **Tested in QEMU** (`tools/run-qemu-arm.sh`: virt board, Cortex-A53 like the Mi A1,
  GICv2, a 720 x 1280 screen): boots to the home screen in 1.7 s, taps open Settings and
  Files, the ramdisk's files are in the file tree.

### Trying it on the Mi A1 (unlocked boot loader)

Nothing is written to the phone: `fastboot boot` runs the image once from RAM.

1. Power off; hold **volume down + power** for the fastboot screen; connect USB.
2. `fastboot boot qrt-0.17.0-mi-a1-boot.img`
3. Expected: QRT's home screen (clock, dock) on the panel, drawn into the boot
   loader's framebuffer.  Volume up works; **touch does not yet** (see below).
4. To leave: hold **power** for about 10 seconds (the PMIC's hard reset); Android boots
   as before.

If the screen stays black, keeps the fastboot picture, or the colours are swapped,
a photo tells me which of the display paths to fix.

### What the Mi A1 still needs

The phone's devices are not on a bus Tessera can enumerate; they sit on the SoC and
work only after clocks, power rails (through the RPM co-processor), pins and
interconnects are set up - Linux does that with its Qualcomm drivers.  The plan is the
0.16.0 approach on ARM: the Linux driver host (LKL for arm64) given the device tree,
the SoC's register ranges and its interrupt lines, running Linux's own drivers for the
touch screen (FocalTech FT5x06 on I2C), USB, eMMC, Wi-Fi (WCN3680), sound and the
display.  That needs processes and the Linux system-call layer on ARM first - next.

## 0.16.0: any hardware Linux has a driver for

0.15.0 ran one Linux driver (e1000e) for one device.  0.16.0 makes Linux's driver
collection Tessera's fallback for **everything**: at boot, every PCI device no Tessera
driver runs goes to one driver host, the unmodified Linux 6.12 drivers take what they
know, and what they make - network interfaces, disks, input devices, sound cards -
is bridged into Tessera.  Tessera's own drivers still come first (they know the Venue and
the FZ-G1 best); Linux fills every gap.

- **One driver host for all devices**: `linuxdrv 00:19.0 00:1f.2 ...` (started by the
  kernel with every PCI device that has no Tessera driver - graphics, bridges and SMBus
  aside).  LKL is patched (`ports/lkl/qrt.patch`) to take a list of PCI devices and to
  add them after boot, once the host's firmware helper runs.  Each device's line in the
  System app says which Linux driver took it; devices no Linux driver takes are given
  back with their command register restored.
- **The drivers** (`ports/lkl/config`): wired network (Intel e1000e, igb, igc, ixgbe,
  i40e, ice; Realtek r8169; Broadcom tg3, bnx2, bnxt; Atheros alx, atl1c/e; Marvell
  sky2; Aquantia; Mellanox; nVidia forcedeth; VIA; virtio), storage (AHCI SATA, NVMe,
  SD/MMC host controllers incl. Realtek and Ricoh card readers, LSI/Broadcom SAS RAID,
  virtio), USB (xHCI, EHCI, OHCI host controllers; mass storage and UAS; USB Ethernet:
  ASIX, Realtek r8152, CDC ECM/NCM, RNDIS, iPhone tethering, SMSC, LAN78xx; serial:
  FTDI, PL2303, CP210x, CH341; HID: keyboards, mice, multi-touch, Wacom, Logitech,
  Apple, Microsoft), sound (HD Audio with every codec family - Realtek, Analog, IDT,
  VIA, Conexant, Cirrus, Creative, C-Media, HDMI; USB audio; RME Hammerfall; virtio)
  and file systems (ext4, FAT, exFAT, NTFS, XFS, Btrfs, F2FS, ISO 9660, UDF).
- **Bridges into Tessera** (`src/arch/x64/lkldev.c`, system calls 1040-1056):
  - *network*: every Ethernet interface becomes an interface in Tessera's stack
    ("Ethernet (Linux r8169)"); Tessera still does ARP, DHCP and TCP.
  - *disks*: each partition Linux can mount appears as **`/mnt/<name>`** in Tessera's
    file tree (`/mnt/sda1`, `/mnt/nvme0n1p2`) - the VFS gained remote mounts whose
    listings, reads and writes the driver host answers with Linux's file systems.
    Programs and the Terminal use them like any directory; writes reach the disk a
    second after the last change and on shutdown (SIGTERM: sync, unmount).
    NTFS is mounted read-only (Windows' fast startup leaves it hibernated).
  - *input*: keyboards, mice, touch screens and tablets found by Linux's input drivers
    (evdev) feed the shell's event stream.
  - *sound*: the first ALSA playback device becomes a sound output; Tessera's mixer
    plays through it (mixer controls unmuted at 80 %).
  - *firmware*: a driver's `request_firmware()` is answered from Tessera's
    `/lib/firmware`, so drivers that need firmware blobs work when the blob is on the
    stick.
- **Interrupts**: MSI and MSI-X now work - each vector's message is aimed at a word of a
  page the driver host watches (a memory write is all an MSI is), alongside the polled
  legacy INTx.  DMA from a buffer outside Linux's RAM (a thread's stack) is translated
  by Tessera (`qrt_dma_addr`).  LKL's memory barriers are real fences on x86-64, which
  NVMe's shadow doorbells need.
- **`/etc/linuxdrv.conf`**: `never <vendor>:<device>` keeps a device from Linux;
  `always <vendor>:<device>` gives Linux a device Tessera has a driver for - e.g.
  `always 8086:1e31` hands the FZ-G1's USB 3 controller to Linux, so *every* USB device
  works as it does on Linux (keyboards, mice and touch come back through the input
  bridge; Tessera's own USB audio and Bluetooth then do not run); `linux <arguments>`
  adds Linux kernel arguments.
- **Tested in QEMU** (q35, all at once): Linux's e1000e, nvme, ahci and snd_hda_intel
  take an Intel 82574, an NVMe disk, the ICH9 SATA controller and ICH9 HD Audio; the
  disk's ext4 and FAT partitions and the NVMe FAT disk appear under `/mnt` and the
  Terminal reads and writes them (written files checked on the host afterwards); `wget`
  fetches a page through the e1000e; `/dev/dsp` output is recorded from the HD Audio
  codec.  And with QEMU's USB controller given to Linux (`always 1b36:000d`): Linux's
  xhci_hcd, usbhid and usb-storage run the tablet, the keyboard and a USB stick - the
  Terminal is opened by a tap and typed into through the input bridge, and the stick is
  read at `/mnt/sdb`.
- **Not yet**: Wi-Fi cards Tessera has no driver for (Linux's iwlwifi, ath9k/10k, rtw88
  need a WPA supplicant bridge); Bluetooth through Linux; GPUs (DRM); devices that need
  x86 I/O ports or ACPI platform buses (PS/2, I2C touch pads, SMBus); USB devices on a
  controller Tessera runs are not passed to Linux one by one (give Linux the controller).
- **Kernel fix**: `thread_current()` read its CPU and that CPU's current thread in two
  steps; a thread moved to another CPU in between got the other CPU's thread (a kernel
  panic in the system call path, seen once the driver host's many threads made system
  calls all the time).  It now reads both with interrupts off.
- Building: `ports/lkl.sh` (about 10 minutes on 16 cores) then `make`; `/bin/linuxdrv`
  is 22 MB.  The suite passes except its scroll-redraw pixel comparison, which fails on
  this 4-core build machine for 0.14.2 and 0.15.0 as well.

## 0.15.0: Linux's drivers on Tessera (the Linux kernel as a library)

Writing every driver twice - once for the Venue, once for the FZ-G1, once for the next
machine - does not scale.  From 0.15.0 QRT can run **Linux's own, unmodified device
drivers**, while Tessera stays what it is (QRT's shell, its native and Linux system
calls, its scheduler and memory manager):

- **`/bin/linuxdrv`, the driver host**: the Linux kernel built as a library (LKL,
  github.com/lkl/linux: the anykernel / LibOS approach) linked into an ordinary program,
  one per PCI device.  Linux's scheduler, memory manager, PCI core, network stack and the
  driver run as threads of that program.  `src/linuxdrv/linuxdrv.c` is LKL's *host* on
  Tessera: pthreads, condition-variable timers, and a PCI backend.
- **Tessera gives it just what Linux's PCI core asks of a platform**
  (`src/arch/x64/lkldev.c`, system calls 1040-1047): claiming a device no Tessera driver
  has, its configuration space, its memory BARs mapped uncached, and Linux's RAM as one
  physically contiguous block - so a driver's DMA address is simply its RAM offset plus
  the physical base, no IOMMU needed.  Interrupts: the PCI status register's Interrupt
  Status bit, polled every millisecond (as LKL's VFIO host does).
- **Network cards are bridged into Tessera's stack**: Linux drives the card and nothing
  more (no address, IPv6 off, GRO off); a packet socket passes every Ethernet frame to a
  network interface in Tessera ("Ethernet (Linux)"), which does ARP, DHCP, DNS and TCP for
  QRT's programs exactly as for the cards it drives itself.
- **Automatic**: at boot Tessera starts a driver host for each PCI device that no Tessera
  driver took and that `/etc/linuxdrv.conf` lists (vendor:device).  This build carries
  Linux's **e1000e** (Intel 82574 ... I219 Ethernet), so Tessera's own e1000 driver now
  keeps only the classic e1000; more Linux drivers are a config line in
  `ports/lkl/config` and a line in `src/linuxdrv/linuxdrv.conf` away.
- **Building**: `ports/lkl.sh` fetches LKL at a fixed commit, configures it
  (`arch/lkl` defconfig + `ports/lkl/config`) and builds the kernel object with musl
  (`build/linuxdrv/lkl.o`); `make` then links `/bin/linuxdrv` (12 MB) into the image.
- **Tested in QEMU** with an e1000e as the only network card: Tessera starts the driver
  host, Linux 6.12's e1000e driver brings the card up (legacy interrupts), the card is
  bridged, Tessera gets an address over DHCP and a `wget` from the Terminal fetches a page
  through it.  `linuxdrv <bus:dev.fn> --test host:port` instead lets Linux's own TCP/IP
  fetch a page (DHCP in Linux, `HTTP/1.0 200 OK`).
- Next: Linux's Wi-Fi (iwlwifi with mac80211), sound (snd-hda) and graphics drivers in
  the driver host, MSI interrupts, and more than one device per host.
- The suite: on this 4-core build machine its timing checks (Ladybird's first frame, the
  touchpad's scroll, USB audio dropouts) fail now and then for 0.14.2 as well.

## 0.14.2: HDMI for TVs, control mode on Ivy Bridge

- **TVs**: when the EDID says the monitor is an HDMI sink (a CEA extension with the HDMI
  vendor block), the port runs in HDMI mode with an AVI infoframe as Linux sends it
  (`intel_hdmi_set_avi_infoframe`, `cpt_write_infoframe`): RGB, full range, underscan
  (the TV should not crop the edges), 16:9 and the CEA video code for 1080p / 720p.
  Before, the LG TV got plain DVI and cropped and washed out the picture.
- **Control mode on the FZ-G1**: the shell now knows about the HDMI monitor, so the
  power menu's "Use the external screen" works: pipe B switches from mirroring the
  panel to a buffer of the monitor's own size that the blitter fills with the shell,
  and the tablet becomes its touchpad and keyboard; back to mirroring when you leave.

## 0.14.1: Ivy Bridge GPU wakes

- The FZ-G1 reported "the GT did not wake": QRT chose the forcewake method from ECOBUS,
  which reads 0 while the GT sleeps.  Now as Linux's intel_uncore.c: the multi-threaded
  forcewake first, the legacy one if that is not acknowledged, then a wait for the GT
  threads to leave C6; the log names the method used, or the acknowledge registers.
- Wi-Fi (6235) confirmed working on the FZ-G1; HDMI found an LG TV at 1920 x 1080.
- Confirmed on the FZ-G1: the blitter draws the screen.

## 0.14.0: Ivy Bridge GPU and HDMI (FZ-G1), Wi-Fi firmware fix

- **The FZ-G1's GPU draws the shell** (`src/drivers/i915/ivb.c`, Intel HD Graphics
  2500/4000): the blitter engine copies every changed part of the shell's picture to
  the screen (XY_SRC_COPY_BLT on the blitter ring, as Linux's i915 and the X server did
  on this generation): forcewake, our pages in the GTT with LLC caching (coherent, no
  cache flushes), a fence written by MI_FLUSH_DW.  A self-test at start; a failure or a
  copy that takes over 100 ms hands the screen back to the CPU, and a start that froze
  is skipped on the next boot.  Settings' GPU switch turns it off.
- **HDMI output**: plug a monitor into the FZ-G1's HDMI port and it mirrors the screen.
  The display engine is set up as Linux's `ironlake_crtc_enable` does it for a chipset
  HDMI port: EDID over GMBUS, the PCH DPLL, pipe B timings and FDI M/N, FDI link
  training, the PCH transcoder, the HDMI port; plane B shows the panel's own picture
  and the panel fitter scales it to the monitor's mode (shape kept), so mirroring costs
  nothing.  Checked every two seconds; unplugging turns it off.  System -> Hardware ->
  External display shows the monitor and mode, or the step that failed.
- **Wi-Fi firmware**: the 6235's firmware is also linked into the kernel, so the Wi-Fi
  driver no longer depends on how the tablet's firmware reports the stick's file names
  (the FZ-G1 log said "not found"); when the file is missing the log lists what
  `/lib/firmware` holds.
- QEMU has no Intel GPU: these paths were not run here.  The suite passes; please
  report what System -> Hardware says about Acceleration and External display.

## 0.13.1: EHCI (the FZ-G1's internal USB), its Bluetooth

- **EHCI**, the USB 2 host controller, `src/drivers/usb/ehci.c`: on Intel 7-series
  chipsets (the FZ-G1) only four USB 2 ports can be switched to xHCI; the internal
  devices on the others hang from the two EHCI controllers (each through Intel's
  rate-matching hub).  Control, bulk and interrupt transfers, 64-bit data structures,
  split transactions for full and low speed devices behind high speed hubs; the same
  device code as on xHCI (`src/drivers/usb/usbint.h`): keyboards, mice, touchscreens,
  hubs, Bluetooth.  No isochronous transfers (USB audio) on EHCI yet.  Tested in QEMU
  (usb-ehci with a high speed keyboard: typed into the Terminal).
- **Bluetooth of the Centrino Advanced-N 6235** (USB 8087:07da, a CSR chip): standard
  HCI, as Linux's btusb treats it; Intel's bootloader set-up now only runs on the
  controllers that have one (8260 and later).

## 0.13.0: ACPI on PCs (battery, AC, lid, buttons, hotkeys)

- **ACPICA**, the ACPI interpreter Linux uses (Intel's, BSD licence, unchanged in
  `src/acpi/acpica` apart from one line in `acenv.h` choosing QRT's host header), runs
  the firmware's AML on PCs such as the Panasonic FZ-G1.  `src/acpi/osl.c` is its
  operating-system layer; `src/acpi/acpidev.c` does what Linux's ACPI drivers do:
  - the **embedded controller** (drivers/acpi/ec.c): its address space for the AML, and
    its events (each query runs the matching `_Qxx` method);
  - **battery** (drivers/acpi/battery.c: `_BIX`/`_BIF`, `_BST`, milliwatts converted),
    **AC adapter** (`_PSR`), **lid** (`_LID`): the top bar and System app show them, and
    closing the lid puts QRT to sleep as on the Venue;
  - the **power and sleep buttons** (fixed event or button devices);
  - **Panasonic hotkeys** (drivers/platform/x86/panasonic-laptop.c: `HINF` after a
    Notify): volume and brightness keys; the ACPI video brightness keys;
  - every Notify goes to the kernel log with its device, so a button QRT does not
    handle yet can be reported from System -> Log.
  It runs in its own kernel thread, polling the SCI handler every 20 ms; the Venue keeps
  its own drivers.  `QrtAcpi = 0` (NVRAM) turns it off.
- Tested in QEMU: ACPICA loads QEMU's tables in 0.3 s, QEMU's ACPI power button locks
  the screen, and a test SSDT's battery (milliwatt units), AC adapter, lid and
  Panasonic HKEY device are found; the battery shows 75 % in the top bar.

## 0.12.1: FZ-G1 sound and touch

- **Sound on the FZ-G1** (Realtek ALC269 codec): the codec setup Linux's patch_realtek
  does before playing (the processing coefficients that power the outputs, by codec
  variant), and the PCH settings Linux's azx_init_pci makes (traffic class 0, snooped
  DMA); the audio ring is flushed from the CPU cache after each fill.
- **Touch is faster**: USB touchscreens (and every interrupt endpoint) now keep 8
  transfers queued, as Linux's usbhid keeps its request resubmitted.  Before, one
  report was read per frame, so a touchscreen sending a report per finger fell behind.
- System -> Hardware -> Built-in codec shows the HD Audio codec on PCs.

## 0.12.0: Wi-Fi on the Panasonic FZ-G1

- **Intel Centrino Advanced-N 6235 and 6230** (the FZ-G1's Wi-Fi card): a new driver,
  `src/drivers/iwn`, ported from OpenBSD's iwn(4) like the Venue's 8260 driver was from
  iwm(4).  It reads the card's EEPROM (MAC address, regulatory domain, the 2.4 and 5 GHz
  channels it may use, crystal calibration), runs Intel's initialization firmware for
  its calibration results and hands them to the runtime firmware, sets up Bluetooth
  coexistence, scans, associates and encrypts with WPA2 (CCMP in the card) through the
  same Wi-Fi app and network stack as the Venue.  Legacy rates up to 54 Mb/s (no
  802.11n yet).  Firmware: `iwlwifi-6000g2b-6.ucode` (Intel's, from linux-firmware),
  installed under `\lib\firmware` like the 8260's.
- `src/drivers/wifi.c` hands the Wi-Fi layer's calls to whichever driver found a card.
- Tested in QEMU, which has no Intel Wi-Fi: the firmware file is found and parsed
  (both images and their sizes appear in the Wi-Fi log); the card itself needs the
  tablet - please send the Wi-Fi log (Wi-Fi app) if it does not connect.

## 0.11.0: install, a proper shutdown, the Panasonic FZ-G1

- **Install QRT on the internal disk.**  Settings -> System -> Install QRT on this
  device -> Install... -> Erase and install.  The row names the disk it will erase
  (the largest fixed disk that QRT did not start from).  QRT restarts and, while the
  firmware's disk access still works, copies its GPT disk from the USB stick, writes
  the partition table for the bigger disk (with new disk and partition GUIDs, so the
  boot entry cannot point at the stick), adds a "QRT" boot entry first in the boot
  order, and restarts from the internal disk.  **Everything on that disk, Windows
  included, is erased.**  (Windows' licence key is in the firmware, not on the disk.)
  Tested in QEMU with a second, empty disk: the copy, `sgdisk -v` ("No problems
  found") and the next boot from the new disk through the new entry.
- **Shutting down and restarting stop everything first**: every program is asked to
  stop (SIGTERM), what is left after 3 s is stopped; Bluetooth audio and Wi-Fi
  disconnect; the backlight goes off.  Then, to turn off, ACPI S5 as Linux does (the
  \\_S5 values from the DSDT, through the PM1 control registers or, on
  hardware-reduced ACPI machines like the Cherry Trail tablets, the sleep control
  register), with the firmware's shutdown as the fallback; to restart, the firmware,
  then the ACPI reset register, then the chipset (0xCF9), then the keyboard controller.
- **Panasonic Toughpad FZ-G1 mk1** (Core i5 Ivy Bridge, 1920x1200), from its
  specifications - please report what works:
  - it boots the native kernel when there is a USB 3 controller (its touchscreen,
    keyboards and mice are USB devices; the Venue's touchscreen is on I2C);
  - **USB touchscreens**: multi-touch through the same finger tracking as the Venue's
    (taps, drags, two-finger gestures), switched from their mouse mode to multi-touch
    (the Input Mode feature report, as Linux's hid-multitouch does); their mouse-mode
    reports still work;
  - **HD Audio**: a new driver for Intel High Definition Audio (most PCs and laptops):
    codecs found, a path from each speaker, headphone and line-out pin to a DAC,
    amplifiers on, 48 kHz stereo.  Tested in QEMU (intel-hda): a 1 kHz tone recorded
    from the codec;
  - **brightness**: Intel Core graphics' backlight PWM, as Linux drives it;
  - not yet (0.12.0 adds Wi-Fi): the battery level (ACPI methods), the accelerometer, the GPU (pages are drawn by the
    CPU, as on the Venue).  If the touchscreen does not answer, hold a key of a USB
    keyboard while QRT starts for firmware mode.
- `tools/qemu-test.py`: `QRT_NO_USB_AUDIO=1` leaves QEMU's USB audio out.

## 0.10.0: every core, and memory that runs out without stopping the tablet

- **Programs run on all four cores again by default.**  On the tablet the browser opened
  and ran faster with all cores; the descriptor race fixed in 0.9.9 (two threads given
  one descriptor number, far likelier with four cores running them) is the probable
  cause of the multi-core hang seen since 0.9.5.  Settings -> Startup -> Programs run on
  still offers One core.
- **Running out of memory no longer stops QRT.**  The tablet stopped with "out of
  physical memory" while a page loaded.  Now, when free memory falls under 96 MB, the
  program with the most memory is stopped (a web page's WebContent, normally) and the
  rest carries on; the browser says "This page stopped" and reload tries again.  Tested
  in QEMU with a page that allocates until memory runs out (it got to 1.3 GB).
- **120 MB less memory for the browser.**  Zero-filled program memory (.bss: 17 MB in
  Ladybird, which runs as seven processes) was allocated when a program started; now
  each page is given when first touched, as Linux does.  With the browser open, 1425 MB
  are free instead of 1307.
- Web pages are laid out at their own size (device pixel ratio 1); the browser's
  toolbar is 1.5 times its first size.
- The test-channel dump (Ctrl-T p) also reports free memory, shared memory and each
  program's own pages.

## 0.9.9: the network a web page needs

The tablet's log showed pages failing with "Unable to connect" and "can't resolve host".
A test page like a real site's (121 requests to 20 host names, through real DNS) failed
101 of its 121 requests in QEMU.  The causes, all fixed:

- **TCP had 16 connections for the whole system**, the socket layer 32, UDP 16 ports, and
  only 8 packets could wait for an address lookup (ARP).  A browser opens dozens of
  connections and two DNS sockets per host name at once; the rest failed straight away.
  Now: 256 TCP connections, 512 sockets, 256 UDP ports, 64 waiting packets, and 1024
  descriptors per program (and `RLIMIT_NOFILE` says so).
- **Two threads could get the same descriptor number.**  Finding a free descriptor and
  taking it were two steps, and a system call can be preempted between them: a DNS
  thread opening a socket and the main thread making a socket pair got the same number,
  and the first close pulled it from under the other.  Ladybird's RequestServer then
  crashed and every request in flight failed.  A descriptor is now taken at once.
- **TCP kept only segments that arrived in order.**  On Wi-Fi one lost packet made the
  server wait for its timeout (a second or more) and resend everything after it.  Early
  segments are now kept and the gap is acknowledged at once, so the server resends just
  the missing one; three duplicate ACKs make QRT resend its own.  Receive buffers are
  128 KB with window scaling (they were 64 KB).
- **The network ran only when the shell got the processor.**  With the browser busy on
  its one core, the shell (which drives Wi-Fi and TCP) waited behind every browser
  thread.  The shell and the sound mixer now run at the tick they wake up.
- The kernel log keeps 240-character lines (was 96) without colour codes, and System
  Monitor's log wraps them, so an error's reason is no longer cut off.
- A test-channel command dumps every program thread (state, last system call) and the
  descriptors that have data waiting: for the next "the page never loads".

With all that, the test page loads all 121 requests (and a 3 MB file) in QEMU.

- The browser opens **Google** as its home page.

## 0.9.8: a browser sized for the tablet

- **Bigger browser**: the toolbar is 1.25 times the shell's own controls, and pages
  are laid out at a device pixel ratio of 1.5 on the 8" panel - text and buttons on
  web pages are 1.5 times larger, and in portrait a page is 533 CSS pixels wide, as on
  a phone, so sites send their mobile layouts.
- **Scrolling follows the finger** (in CSS pixels, whatever the scale), and a flick
  keeps the page moving and slowing down.
- **WebAssembly is compiled**, not interpreted: Cranelift's compiler process
  (`/bin/cranelift-compiler`) is now in the image.  Anubis' WebAssembly challenge
  (the "making sure you're not a bot" page in front of Invidious and many other
  sites) at difficulty 4 now passes in about 8 s in QEMU; interpreted, difficulty 2
  (16 times less work) took as long.
- Anubis checked in QEMU against a real Anubis server: versions 1.21, 1.24 and
  1.28, all its challenges (fast, sha256, hashx, argon2id), over HTTP and HTTPS,
  on a CPU like the tablet's (`QRT_QEMU_CPU=Westmere,+movbe,+rdrand
  tools/run-qemu.sh`: no AVX).  All of them pass.
- Ladybird's start-up options reached it only partly (the shell gave the wrong
  argument count): the in-memory HTTP disk cache is now really off.

## 0.9.7: one core by default, a browser that says why it waits

- Programs run on **one core again by default**: on the tablet the browser still did
  not start with all cores in 0.9.6.  Settings -> Startup -> **Programs run on: All
  cores** turns 0.9.5's multi-core scheduling back on (after a restart) to try it.
- If Ladybird has not opened its window after 15 s, the "Starting Ladybird" screen
  shows what its processes are doing (threads running, blocked, in the kernel, system
  calls, CPU time) and the last log lines about it.
- Brightness: the slider's level reaches the hardware once per frame (the latest one),
  not once per touch event; software dimming redraws about 8 times a second while
  dragging.  Settings names the dimming method in use.
- New threads start with their own FPU state, not the previous thread's.

## 0.9.6: fixes from the tablet

- **The other three cores ran with their caches off.**  A core started with INIT/SIPI
  comes up with CR0.CD and NW set, and QRT never cleared them (QEMU ignores the bits).
  Until 0.9.5 those cores only helped draw; in 0.9.5 the browser ran on them,
  dozens of times slower, holding the kernel lock - the browser never got past
  "Starting Ladybird" and the whole shell stuttered.  Fixed: caches on at start-up.
- Sending an IPI took two register writes that an interrupt could split (xAPIC, the
  tablet's mode; QEMU uses x2APIC): fixed.
- **Choose where sound plays**: Settings -> Sound -> Play on: Automatic (Bluetooth,
  then USB, then the speaker) or any output there is.
- USB audio: the controller's endpoint is configured before the device switches
  setting (as Linux does), every streaming setting is tried in turn, and the log says
  why one fails.
- Bluetooth headphones that connect by themselves but leave the audio to the
  tablet: QRT starts the audio after 3 s instead of giving up.
- The brightness slider no longer writes NVRAM on every step (once it rests).
- Settings shows why the speaker or the sensors are not working, under Output and
  Screen rotation.

## 0.9.5: every core, battery, speakers, sensors

- **All four cores run programs.**  Until 0.9.4 the other three cores only helped the
  shell draw; a browser's processes and threads shared one core.  Now any core runs
  program threads, so JavaScript, layout, painting and video decoding run side by
  side.  The kernel itself stays serialised by one big kernel lock (taken on system
  calls, faults and device interrupts, given back on the way out and on thread
  switches), the way early SMP Linux and BSD did it: the kernel code written for one
  core stays correct.  Page-table changes are announced to the other cores (TLB
  shootdowns), and a process's memory is freed only when no core still uses it.
  `QrtSmpPrograms = 0` (an NVRAM variable) goes back to one core.
- **Battery.**  The Venue's battery, charger and cover sensor answer through a small
  embedded controller on I2C3 (address 0x78): QRT reads it the way the DSDT does.  The
  top bar shows the charge (green while charging, red when low); System Monitor has
  the details (voltage, current, capacity).  Closing a magnetic cover puts QRT to sleep.
- **Brightness** works without the backlight PWM too: the Crystal Cove PMIC's PWM if
  that is the one in use, otherwise QRT dims the picture itself.
- **The speakers.**  The tablet's own speakers sit behind Intel's audio DSP.  QRT now
  loads Sound Open Firmware into it (`sof-cht.ri`, BSD-licensed), builds the same DSP
  pipelines Linux uses with this codec (from SOF's `sof-cht-rt5670` topology), sets up
  the Realtek RT5672 codec over I2C (its PLL from the SoC's 19.2 MHz clock, the DAC,
  the speaker path) and streams QRT's mix to it.  USB audio and Bluetooth headphones
  still take over when connected.  `QrtSpeaker = 0` turns it off.
- **Sensors.**  The accelerometer and light sensor are behind the Integrated Sensor
  Hub, a microcontroller with its own firmware.  QRT speaks its protocol (ISHTP) and
  the HID sensor reports on top: Settings → Screen rotation → **Auto** turns the
  screen with the tablet.  `QrtSensors = 0` turns the hub off.
- Also: `ls` in the Terminal no longer dies ("stack smashing detected": QRT wrote 60
  bytes into a 36-byte terminal-settings structure).

The battery, speaker and sensor drivers could not be tried here (QEMU has none of
this hardware).  Each was tested against a simulated device (`make check`: the
speaker test decodes every message to the DSP with Linux's own SOF structures), and
each logs every step: if one does not work on the tablet, System Monitor's log says
where it stopped.

## Native QRT programs (0.8.0)

QRT has its own kind of program now, built with the **QRT SDK** (`sdk/`, `make sdk`):
- musl as the C library, compiled for QRT's own system-call numbers (`sdk/syscalls.txt`);
- `libqrt` for windows, input and drawing;
- `qrt-cc` for C, `qrt-cargo` for Rust.

A native program carries a `QRT` ELF note, and the kernel runs it with the native
personality, not as a Linux binary. Its windows are apps in the shell: they appear in the
dock and the overview, follow rotation, the keyboard and desk mode, and get touches,
mouse clicks and keys as events.

`/bin` has these examples:
- `native-test` (C library),
- `cxx-test` (C++23 with libc++),
- `rust-hello` (Rust's standard library),
- `hello-window` (a window),
- `js`: **Ladybird's JavaScript engine**, built natively for QRT. Try
  `js /share/tests/js-test.js`, or `js` alone for its prompt.
- `ladybird`: **the Ladybird web browser**, native on QRT; the dock's Browser opens it.

The QEMU test runs all of them. They are steps L1–L3 of porting a modern browser,
**Ladybird**, natively: see [docs/sdk.md](docs/sdk.md) and [docs/ladybird.md](docs/ladybird.md).

## Linux programs

Open **Terminal**. It runs x86-64 Linux ELF programs, unmodified, through
Tessera's Linux system-call layer (`src/arch/x64/linux.c`, `proc.c`), the way
WSL1 or FreeBSD's Linuxulator do: this is the Linux ABI, not the Linux
kernel.

Since 0.6.0 that includes **dynamically linked programs**. A program that
names a loader (`PT_INTERP`) gets it: the kernel maps
`/lib64/ld-linux-x86-64.so.2` next to it and starts there, and glibc's loader
maps `libc.so.6` and the rest itself with file-backed `mmap` (`MAP_FIXED`
included), as on any Linux system. Threads work too: `clone` for threads
(the new thread starts from a copy of its parent's registers and gets its own
TLS), and `futex` with `WAIT`/`WAKE`, bitsets, timeouts, `REQUEUE` and
`WAKE_OP`, which is what glibc's mutexes, condition variables and
`pthread_join` sleep on. The image ships glibc 2.39's loader, `libc`, `libm`,
`libstdc++` and `libgcc_s` in `/lib/x86_64-linux-gnu`, and three test
programs:
- `dynhello` loads `libm` at run time with `dlopen`.
- `threads` runs four threads through a mutex, a condition variable and
  `pthread_join`.
- `cxx` uses libstdc++: containers, exceptions, `std::thread`, iostreams.

The QEMU test runs all three and checks that they exit with 0.

The image also includes a static **busybox**, so `ls -l /`, `cat
/proc/cpuinfo`, `free`, `date`, `sha256sum`, `wc`, `uname -a`, and anything
else busybox provides work. `hello` (static glibc) and `hello-musl` check
stdio, `malloc`, files, directory listing, `/proc` and TLS.

To add your own program, build it on an x86-64 Linux machine (dynamically
against glibc, or `-static`) and copy it into `\bin` on the stick, with any
extra libraries in `\lib\x86_64-linux-gnu`.

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

Since 0.6.3 there are **processes**: `fork`, `vfork`, `clone` without
`CLONE_THREAD`, `execve` (with `#!` scripts; names in `/bin` that are not
files run as busybox applets), `wait4`, `pipe`/`pipe2`, `dup`/`dup2`/`dup3`,
`FD_CLOEXEC`, `kill` and `getppid`. A command line with shell syntax (`|`,
`>`, `;`, `&&`, `$`, `*` and so on) runs through busybox `sh -c`, so
`ls /bin | wc -l` or `cat /proc/cpuinfo > /tmp/cpu; wc -l /tmp/cpu` work.
`procs` tests fork, pipes, exec, `posix_spawn`, `popen` and `kill`.

Since 0.7.0 (kernel services that native programs use too, see [docs/roadmap.md](docs/roadmap.md)):

- **Signals** (`src/arch/x64/signal.c`): `rt_sigaction` with and without
  `SA_SIGINFO`, per-thread masks, pending sets, `sigaltstack`, `kill`,
  `tgkill` (`raise`, `pthread_kill`), `sigsuspend`/`pause`, `sigtimedwait`,
  `alarm`/`setitimer`, `SIGCHLD`, `SIGPIPE`, and the default actions. A
  handler runs on Linux's own `rt_sigframe` layout (so glibc's and musl's
  `siglongjmp`, `ucontext` and restorers work). It is called when the thread
  next returns to user mode: after a system call, after an interrupt (so a
  busy loop gets its `SIGALRM`), or on a fault. Faults become signals: a store
  to a read-only page or a jump into data gives `SIGSEGV` with the address,
  division by zero `SIGFPE`. Blocking calls return `EINTR`, or start again
  under `SA_RESTART`.
- **Memory**: every process now has its first GiB plus 448 GiB more (64 to
  512 GiB) for `mmap`, so reservations of several GiB, as Firefox makes for
  its JIT and WebAssembly, fit. Pages have real protections:
  - read-only and no-execute (NX) pages;
  - `PROT_NONE` reservations committed piece by piece with `mprotect`;
  - a JIT's write-then-execute.

  Shared memory works through `memfd_create`, `/dev/shm` (`shm_open`) and
  `MAP_SHARED`, also anonymous and across `fork`. `mremap`,
  `MADV_DONTNEED`, `unlink`/`rename` and 256 descriptors per process are
  supported too.
- **Event loops**: `epoll`, `eventfd`, `timerfd`, `signalfd`, and Unix
  sockets (`src/arch/x64/unix.c`):
  - stream, datagram and seqpacket types; `socketpair`;
  - path and abstract names; `listen`/`accept`;
  - descriptors passed with `SCM_RIGHTS`; `SO_PEERCRED`;
  - hang-up through `poll`/`epoll`.

  This is what Wayland and Firefox's own IPC run on.

`signals`, `memory` and `events` in `/bin` test all of this, and the QEMU test
runs them. They pass on Linux too, so a failure means QRT differs from it.
Writing to `/dev/kmsg` puts a line in QRT's boot log (System Monitor → Log).

Not supported yet:
- IPv6
- keyboard input to Linux programs, and graphical Linux programs (graphical programs
  are native QRT programs now: see above)
- writes that survive a reboot (milestone 5)

## Sound (0.9.2)

QRT plays sound on **USB audio devices** and **Bluetooth headphones and speakers**:

- **The sound core** (`src/kernel/sound.c`) mixes every stream - programs, the browser,
  the system's own sounds - at the output's rate, applies the volume (the volume keys,
  Settings → Sound) and hands the mix to the output.  The newest output plays: plug in
  USB audio or connect headphones and the sound moves there.
- **Programs** play through `/dev/dsp`, the OSS interface: open it, set the rate and
  channels with ioctls, write 16-bit samples.  Ladybird does (videos and web audio have
  sound), and so does `play` in the Terminal: `play song.wav`, or `play -t 440 2` for a
  test tone.
- **USB audio** (`src/drivers/usb/uaudio.c`): the USB Audio Class, versions 1 and 2 - USB
  headsets, USB-C to 3.5 mm dongles, docks with a headphone jack.  QRT picks a 16/24/32-bit
  stereo setting at 48 or 44.1 kHz and streams it with isochronous transfers, 40 ms ahead.
- **Bluetooth headphones and speakers** (A2DP, `src/drivers/bt/`): in the **Bluetooth** app,
  Scan, then **Connect** on the headphones (put them in pairing mode first).  QRT pairs the
  way headphones expect ("just works"), keeps the link key so the next connection is
  immediate, and streams SBC at 44.1 kHz, stereo, bitpool 53 (about 330 kbit/s).
  Headphones that reconnect by themselves are accepted too.
- **Settings → Sound** shows the output and has a **Test** button (a short chime);
  System Monitor → Hardware → Sound shows the streams.
- **Not yet**: the built-in speaker and headphone jack (they need a driver for Intel's SST
  audio DSP and its firmware), microphones, Bluetooth headsets' call audio (HFP), AAC.

How it is tested: `make check` checks the mixer (pitch after resampling, volume, clipping),
the SBC encoder against ffmpeg's decoder (65.9 dB SNR), and the whole Bluetooth path against
a simulated headset - pairing, SDP, AVDTP set-up (skipping an AAC end-point for the SBC
one), two seconds of streaming that ffmpeg decodes back to the 1 kHz tone, and
reconnecting with the stored key.  The QEMU test plays a tone with `play` and a WebM video
in Ladybird on QEMU's USB audio device and checks the recording: 1000.0 Hz for 1 s, then
the video's 660 Hz for 4 s.

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

Since 0.9.0 the dock's **Browser** is **Ladybird**, ported natively (not through the
Linux layer): full HTML, CSS and JavaScript, images and fonts, rendered with Skia on the
CPU, with OpenSSL checking certificates.  Since 0.9.2 videos play with sound (VP9, AV1,
VP8 with Opus or Vorbis - what YouTube sends browsers like Ladybird); see
[docs/gpu.md](docs/gpu.md) for drawing pages on the GPU and for YouTube. Its window has back, forward, reload, an address
bar and a keyboard button. A finger drag scrolls the page and a tap clicks. The
on-screen keyboard opens when a text field takes the focus. In desk mode, the mouse and
the two-finger touchpad scroll work too. See [docs/ladybird.md](docs/ladybird.md).

The rest of this section describes the built-in text browser. It is the fallback when
the image is built without Ladybird. It shows pages over http:// and https:// as text
and links, with no images, CSS or scripts.

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
Drivers declare id tables and a `probe()`. **System Monitor → Hardware** lists every
device, first those with a QRT driver and then those still waiting for one.
On the tablet, that second part is the to-do list.

Built in:
- **i915**: the Cherry Trail GPU's 3D engine puts the screen on the panel
  (see Rendering)
- **framebuffer**
- **16550 UART**: interrupt-driven through the I/O APIC
- **DesignWare I2C**
- **HID over I2C**: the Venue's Wacom touchscreen
- **gpio-buttons**: the Venue's power, volume and Windows buttons
- **backlight**: the panel backlight through LPSS PWM #1
- **iwm**: Intel Wireless 8260 (Wi-Fi)
- **iwn**: Intel Centrino Advanced-N 6230/6235 (Wi-Fi, the Panasonic FZ-G1)
- **xhci**: USB 3 host controller; devices on root ports, boot-protocol
  keyboards (written from the xHCI specification, with OpenBSD's xhci(4)
  as the reference)
- **audio**: identifies the Realtek codec (RT5670/RT5672 on the 5855); the Intel SST DSP is listed
- **bt**: Bluetooth over USB (`src/drivers/bt/`): Intel firmware loading after Linux's btusb.c/btintel.c, HCI, classic and LE scanning
- **e1000**: Intel 8254x/82574 Ethernet. This is QEMU's NIC; it is here so
  the network stack can be tested.
- **chipset**: devices the kernel handles itself

[docs/roadmap.md](docs/roadmap.md) lists what comes next (Firefox, GPU drawing, audio, Bluetooth). [docs/drivers.md](docs/drivers.md) covers the primitives a driver gets
(MMIO, DMA memory, IRQ/MSI, threads). It also lays out the plan for using
Linux drivers: port them by hand now, then a LinuxKPI-style shim like
FreeBSD's.

## Hardware status on the Venue 8 Pro 5855

| | |
|---|---|
| Display | works (framebuffer, 1200×1920, rotation) |
| GPU | Intel Gen8 3D engine copies and turns each frame: **works** (0.5.8, coherent mode); it tests itself at boot and falls back to the CPU |
| Touch | works: QRT's own Wacom driver; tested on the tablet in firmware mode. Native mode needs the same driver after the handover, which is **new in 0.5 and untested on hardware**. |
| Storage | the boot stick is read into RAM at boot; writes go to RAM only |
| Buttons | power, volume and Windows through GPIO (input bit and edge latch) and the ACPI fixed power button, both kernel modes. Pins confirmed on the tablet with the Button test's pad scanner: SW/5f Windows, SW/5d volume up, N/08 volume down (power: PMU_PWRBTN_B, left in its native function for the ACPI power button). Up to 0.5.9 the driver never started on the tablet: the ACPI device list did not bind it (the Button test said "not started"). 0.5.9.1 starts it from the input path: **works** (volume, Windows); 0.5.9.2 also raises the device table limit that dropped the button devices. |
| Wi-Fi | Intel 8260 driver, WPA2-Personal: **works** (scanning, connecting, DHCP) |
| Backlight | LPSS PWM #1, native mode: brightness and sleep (0.5.5.3). QRT only takes control if the firmware left that PWM running; 0.9.5 then tries the Crystal Cove PMIC's PWM, and otherwise dims the picture in software, so the Settings slider always works. System Monitor names the method in use. |
| Sleep | backlight off and a slower frame loop; not ACPI suspend |
| Audio | 0.9.2: **USB audio** (headsets, USB-C dongles, docks' headphone jacks: USB Audio Class 1 and 2) and **Bluetooth headphones and speakers** (A2DP), mixed by QRT's sound core; the volume keys set the real volume. 0.9.5: the **built-in speakers** - Sound Open Firmware on the audio DSP, SSP2, the RT5672 codec - **new, untested on hardware**. See [0.9.5](#095-every-core-battery-speakers-sensors) |
| USB | 0.6.1: QRT's own xHCI driver in native mode: devices on the root ports and behind USB 2 hubs (0.6.3) are listed under System Monitor → Hardware → USB; USB keyboards work, also behind a hub (tested in QEMU); USB mice since 0.6.5. USB-C docks show up by name (USB billboard class). |
| External display | 0.6.4: a monitor on a USB-C dock (DisplayPort Alt Mode, HDMI behind the dock's converter): **works** (detected and mirrored on the tablet). 0.6.5: desk mode - the shell moves to the monitor and the tablet becomes its touchpad and keyboard; see [External display](#external-display-064) |
| Mouse | 0.6.5: USB mice, also wireless receivers and keyboard-and-mouse combos (HID report descriptors; buttons, wheel, absolute pointers); a cursor on whichever screen the shell is on |
| Bluetooth | 0.6.2: the Intel 8260's Bluetooth (USB 8087:0a2b, root port 4): firmware download as Linux's btusb/btintel do it, then scanning for classic and LE devices in the **Bluetooth** app (works on the tablet: it finds devices). 0.9.2: pairing (Secure Simple Pairing, link keys kept) and **A2DP audio** to headphones and speakers - tested against a simulated headset, **new on hardware** |
| Battery, charger, cover | 0.9.5: the embedded controller on I2C3 (as the DSDT reads it): charge, charging, AC, the cover sensor (closing it sleeps); the top bar shows the charge - **new, untested on hardware** |
| Sensors | 0.9.5: the Integrated Sensor Hub (ISHTP, HID sensors): the accelerometer turns the screen (Settings: Auto-rotate), the light sensor is read - **new, untested on hardware** |
| Camera | no driver: the OmniVision sensors sit behind Intel's imaging unit (ISP), which needs its own firmware and a large driver |
| CPU cores | 0.9.5: programs run on all four cores (the kernel under one big lock); before, only the boot core ran them |
| USB keyboard | both modes (native: QRT's xHCI driver, 0.6.1) |

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

Everything is drawn on the CPU. Each change records a damage rectangle,
and only that area is redrawn and copied to the panel. Big redraws are cut
into strips (4 per core), and every core claims strips from a shared
counter. In native mode those cores were started by QRT. Under the firmware
they come from UEFI's MP Services.

In QEMU with 4 cores, a full 1280×800 redraw drops from 74 ms on one core to
24 ms. Type `bench` in the launcher's search field to measure; the result
appears in **System Monitor → Hardware**.

When the screen is rotated (the 5855's panel is portrait, so landscape use
is rotated), every frame must be turned before it reaches the panel. Up
to 0.5.6 that was a straight per-pixel loop: each write landed on a new
cache line, and the rotation took most of the frame time. In 0.5.7 the
frame is turned in 32×32 tiles, which keeps both reads and writes in the
cache. The tiles are spread over every core and written straight into
the framebuffer in native mode, instead of through a staging copy. With
the screen rotated in QEMU, a full frame went from 12.6 ms (10.7 ms of it
for the rotation) to 6.2 ms (4.0 ms).

### Scrolling (0.5.9)

Scrolling used to redraw the whole app on every finger movement. Now, once
a drag is past the tap distance, the shell moves the pixels that are
already on screen and asks the app to draw only the strip that came into
view (plus a narrow column at the right edge for scroll indicators). If
anything else changed in that area during the same frame, it redraws the
area as before. A flick keeps the content moving and slows it down, and a
touch stops it. The frame loop also no longer waits a fixed 10 ms after
every frame: it sleeps only what is left of the 10 ms period. The QEMU test
compares a scrolled screen with a full redraw of the same state, pixel for
pixel.

### GPU acceleration (0.5.8)

On the tablet in native mode, the GPU now does that last step. The driver
(`src/drivers/i915/`) brings up the render engine of the Cherry Trail's
Intel Gen8 graphics the way Linux's i915 does for a legacy render ring: it
keeps the render power well awake, enters its buffers and the canvas pages
in the global GTT, applies the Cherryview workarounds, starts the ring and
runs Linux's "golden" render state once. Each frame is then one 3D draw,
modelled on intel-vaapi-driver's Gen8 `put_surface`: the canvas is a texture,
the framebuffer is the render target, and every damaged rectangle is a
rectangle whose texture coordinates carry the rotation. The CPU only waits
for the GPU to finish.

What it does not do: the apps and the shell are still drawn by the CPU.
The GPU takes the copy and the rotation, which was the largest part of a
rotated frame.

Safety (it was released before it could be tried on a tablet; QEMU has no
Intel GPU). It has since been confirmed working on the 5855:
- It only starts in native mode, and only on 8086:22b0–22b3.
- At boot it turns a 64×64 test picture on the GPU and checks every pixel.
  If the GPU does not see the CPU's writes, it retries with explicit cache
  flushes. Any mismatch, and the CPU path is used.
- Every GPU job has a 100 ms limit. If one runs over, the GPU is reset and
  switched off for the rest of the session.
- If a start ever hangs the tablet, the next boot notices (a flag in NVRAM
  that is cleared after 20 good frames) and leaves the GPU off for that one
  boot. (Up to 0.5.9.1 the flag needed 300 frames and then switched the GPU
  off for good, so a short session could turn it off by mistake.)
- **Settings → Startup → Graphics acceleration** turns it on or off.

**System Monitor → Hardware → Graphics** shows its state and how long the
GPU takes per frame; `bench` in the launcher compares it with the CPU path
(turn acceleration off, run `bench` again). `make check` tests the parts
that can be tested without the GPU: the batch's command lengths, the
golden-state relocations, and that the texture coordinates sample exactly
the pixels the CPU rotation copies, for all four rotations.

### External display (0.6.4)

The 5855's USB-C port can carry DisplayPort (Alternate Mode). The switch to
DisplayPort happens in hardware - the DSDT has no Type-C or Power Delivery
controller for an OS to drive - so a dock's monitor appears to the display
engine as an ordinary DisplayPort sink on one of Cherry Trail's ports B, C or
D. Which one the USB-C port is wired to is not documented, so QRT looks at all
three. The internal panel (MIPI DSI on pipe A) is not touched.

`src/drivers/i915/display.c` follows Linux's i915 (v4.9) for Cherryview step
by step: it powers the DPIO PHY's common lanes through the PUNIT, talks to
the sink over the AUX channel (DPCD, and the monitor's EDID over I2C-over-AUX),
picks the monitor's preferred mode if the link can carry it (else the largest
listed one, else 1080p or 720p), sets up the PHY and the DP PLL with Linux's
fixed dividers, trains the link (clock recovery, then channel equalization,
with Linux's swing and de-emphasis table), and starts pipe B (ports B and C)
or pipe C (port D) with its own scanout buffer. The shell's picture is then
scaled into that buffer, centred, with black bars: by the GPU (bilinear)
when acceleration is on, by the CPU otherwise. A kernel thread checks the
ports once a second, so plugging and unplugging work while QRT runs.

- **Settings → Startup → External display** turns it on or off and says what
  it found: the monitor's name, the mode and the link, or the step that failed.
- If setting up a monitor ever hangs the tablet, the next boot notices (a
  flag in NVRAM) and leaves the external display off for that boot.
- `make check` runs the whole path against a simulated display engine and
  DP sink (`tests/test_display.c`): AUX messages, EDID parsing, the link
  choice, training, timings and M/N values, the plane, mirroring, unplugging.

It works on the tablet: the monitor is detected and mirrors the screen.

#### Desk mode (0.6.5)

When a monitor is connected, the shell moves to it: it is laid out for the
monitor's resolution and drawn only there (1:1, no scaling), with a mouse
cursor. The tablet's screen becomes the controller:

- a **touchpad**: slide to move the pointer, tap to click, tap and then slide
  to drag (lists scroll by dragging, as with a finger);
  **slide two fingers to scroll** like a laptop touchpad (0.9.0; fingers down scrolls
  down);
- a **scroll strip** beside it, like a mouse wheel;
- **Click**, **Hold to drag** (the button stays down until tapped again) and
  **Apps** (the launcher);
- the **on-screen keyboard**, always open. Its hide key closes the
  controller and goes back to mirroring, as does **Mirror** at the top.

Holding the power button shows the power menu on the tablet, with
**Mirror to the external screen** or **Control the external screen** at the
top while a monitor is connected, to switch either way. **Settings →
Startup → When a monitor is connected** chooses what happens on plugging in:
Control (the default) or Mirror. Unplugging always brings the shell back to
the tablet. USB mice and keyboards (on the dock's ports) work in both modes.

The QEMU test plugs in a "monitor" that is only memory (QEMU has no
DisplayPort) and drives the controller: its Apps button and keyboard open an
app on the monitor, and the power menu brings it back to the panel.

## Touchscreen

The touchscreen stack is `src/drivers/dwi2c.c`, `i2chid.c`, `hidparse.c` and
`touch.c`. Its diagnostic app (Touch Lab, `src/apps/lab.c`) and the Life demo
are no longer in the app list since 0.5.6.
`make check` runs the HID parser's host tests against the Venue's real Wacom
descriptor. Hardware notes are in `docs/hardware/venue-8-pro-5855.md`.

## Put it on the tablet

Your Windows install on the eMMC is not touched: QRT runs entirely from the stick.

1. Use `dist/qrt-0.11.0.img.gz`, or build the image with `make`.
2. Write it to a USB stick with Rufus or balenaEtcher, or on Linux:
   `gunzip -c dist/qrt-0.11.0.img.gz | sudo dd of=/dev/sdX bs=4M conv=fsync`.
3. Plug the stick into the tablet's USB-C port (directly, with an adapter, or through a dock).
4. In the firmware setup, disable **Secure Boot** (the image is not signed)
   and boot from the stick.

**Settings → Firmware** reboots into the BIOS setup.

## Build it yourself

You need:
- `clang`, `lld`, `mtools`, `dosfstools` and `gdisk`
- `qemu-system-x86` and `ovmf`, for `make run` and `make test`
- `gcc`, `musl-tools` and `busybox-static`, for the Linux programs in `/bin`
- Pillow, only to regenerate the fonts

```sh
make            # build/BOOTX64.EFI, build/qrt.img
make run        # boot in QEMU on 64-bit UEFI (native mode, 4 cores)
make test       # headless boot, scripted walkthrough, screenshots in build/shots/
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
                       e1000, iwm/ (Intel 8260 Wi-Fi), iwn/ (Intel 6235 Wi-Fi), i915/ (Gen8 GPU), driver table
src/net/               802.11 client + WPA2 (wlan.c), ARP/IP/ICMP/UDP/DHCP/DNS (net.c), TCP (tcp.c),
                       HTTP client (http.c), TLS 1.3 client (tls.c), crypto (crypto.c, crypto_tls.c)
src/ui/                gfx (anti-aliased shapes, text), font atlases, shell (dock, launcher), on-screen keyboard
src/apps/              Files, Terminal, Browser (+ html.c), Wi-Fi, Settings, System Monitor, Clock, Drawing
                       (Touch Lab and Life are built but not listed)
firmware/              Intel 8260 and 6235 firmware (Intel redistributable licence)
tests/                 HID parser, crypto vectors, WPA2 client (simulated AP), HTTP, HTML, TLS, Linux test program
tools/                 mkfont.py, mkimage.sh, run-qemu.sh, qemu-test.py, gen_isr.py
assets/                Inter (SIL OFL 1.1), DejaVu Sans Mono (Bitstream Vera licence)
```

Apps implement a five-function ABI (`open`, `draw`, `event`, `tick`, `icon`)
in `src/ui/shell.h`.

`/bin/busybox` on the image is an unmodified copy of Ubuntu's
`busybox-static`, licensed under GPL-2.0. It runs as a separate program,
and `\bin\BUSYBOX.txt` says where to get its source.
