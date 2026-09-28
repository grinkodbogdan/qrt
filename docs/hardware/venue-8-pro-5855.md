# Dell Venue 8 Pro 5855: hardware notes

These notes come from the tablet's own ACPI tables and PCI scan (QRT
hardware report, BIOS 1.11.0). The dump itself is **not** committed:
`MSDM.aml` in it holds the tablet's Windows product key.

## Platform

| | |
|---|---|
| SoC | Intel Atom x5-Z8500 (Cherry Trail), 4 cores, 1.44 GHz |
| RAM | 4 GB (3.9 GB visible to firmware) |
| Firmware | AMI Aptio, UEFI 2.4, **64-bit** (boots `BOOTX64.EFI`) |
| Panel | 1200 × 1920 native portrait, GOP mode 0 of 4 |
| ACPI | OEM `DELL`, table id `CBX3`; DSDT 105 KB, 9 SSDTs |

## PCI devices

| BDF | ID | What |
|---|---|---|
| 00:02.0 | 8086:22b0 | Intel HD graphics (Gen8) |
| 00:03.0 | 8086:22b8 | Imaging unit (ISP, cameras) |
| 00:10.0 / 00:12.0 | 8086:2294 / 2296 | SD host controllers (eMMC / microSD) |
| 00:14.0 | 8086:22b5 | xHCI USB 3 |
| 00:15.0 | 8086:22a8 | Audio DSP (LPE/SST) |
| 00:18.0–7 | 8086:22c0–22c7 | LPSS: DMA + I2C1–I2C7 (DesignWare) |
| 00:1a.0 | 8086:2298 | TXE |
| 00:1c.0 → 01:00.0 | 8086:24f3 | **Intel Wireless 8260** (Wi-Fi) on PCIe |
| 00:1e.x | 8086:2286–22ac | LPSS: DMA, HSUARTs, SPI, PWM |

## I2C buses (from the DSDT)

- **I2C6 (00:18.6)**: the touchscreen. The board ID in GNVS picks the variant:
  - `TCS0`/`TCS3` `ATML1000`: Atmel maXTouch @ `0x4A`, HID descriptor register `0x0000`, 1.7 MHz
  - `SYN1` `SYNP1000`: Synaptics @ `0x2C`, HID descriptor register `0x0020`, 400 kHz
  - `WCOM` `WCOM48xx`: Wacom pen digitizer @ `0x0A`, HID descriptor register `0x0001`
  - Interrupt: GpioInt on `\_SB.GPO3` pin 0x4D (TCS0) or `\_SB.GPO1` pin 0x11 (TCS3)
- **I2C7**: PMIC variants (`PMIC`, `PMI1`, `PMI2`, `PMI5`) and the battery fuel gauge (`BATC`, `WIDR`)
- **I2C3**: TI charger (`TIDR`) on some board IDs

## Board variables (GNVS)

`OperationRegion (GNVS, SystemMemory, 0x7A1E1000, 0x036C)`. The Touch Lab uses
these byte offsets: `OSID` 38, `ITSA` 793, `BDID` 802, `MPNL` 842, `WLID` 871.

## Other devices of interest

- `PNP0C40`/`INTCFD9`: button array (power, volume)
- `PNP0C0A`: battery
- `ACPI0003`: AC adapter
- `PNP0C0D`: lid
- `_BCM`/`_BQC`: backlight control on `\_SB.PCI0.GFX0.DD01`
- `10EC5640/5670/5672`: Realtek codec candidates
- OmniVision camera sensors
- `INT340x`: DPTF thermal participants
