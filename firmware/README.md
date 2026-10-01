# Firmware shipped on the QRT stick

| File | For | Source | Licence |
|---|---|---|---|
| `ibt-11-5.sfi`, `ibt-11-5.ddc` | Intel Wireless 8260 Bluetooth (USB 8087:0a2b), loaded by `src/drivers/bt/hci.c` | Ubuntu `linux-firmware-intel-wireless` 20240318.git3b128b60-0ubuntu3.1 (`intel/ibt-11-5.*`), unmodified | Intel redistributable licence, `LICENCE.ibt_firmware` |
| `iwlwifi-8000C-36.ucode` | Intel Wireless 8260 (Venue 8 Pro 5855 Wi-Fi) | Ubuntu `linux-firmware-intel-wireless` 20240318.git3b128b60-0ubuntu3.1 (linux-firmware.git, `intel/iwlwifi/iwlwifi-8000C-36.ucode`), unmodified | Intel redistributable licence, `LICENCE.iwlwifi_firmware` |

SHA-256: `iwlwifi-8000C-36.ucode` `479931721f5e168d69d67c297c11738acd75da390adb94f96030a1055f4cf57a`,
`ibt-11-5.sfi` `62e0ffb26d8f04adb5b9a2838ec7c60840bdabb38c024a98b905b991ed08e000`,
`ibt-11-5.ddc` `9e57302abc15ac991b71350705c4b5060f69c7111813bc5034704f6edea30872`.

The image puts these under `\lib\firmware\`; the driver loads
`/lib/firmware/iwlwifi-8000C-36.ucode` from QRT's file system.
