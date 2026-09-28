# Firmware shipped on the QRT stick

| File | For | Source | Licence |
|---|---|---|---|
| `iwlwifi-8000C-36.ucode` | Intel Wireless 8260 (Venue 8 Pro 5855 Wi-Fi) | Ubuntu `linux-firmware-intel-wireless` 20240318.git3b128b60-0ubuntu3.1 (linux-firmware.git, `intel/iwlwifi/iwlwifi-8000C-36.ucode`), unmodified | Intel redistributable licence, `LICENCE.iwlwifi_firmware` |

SHA-256 `479931721f5e168d69d67c297c11738acd75da390adb94f96030a1055f4cf57a`.

The image puts these under `\lib\firmware\`; the driver loads
`/lib/firmware/iwlwifi-8000C-36.ucode` from QRT's file system.
