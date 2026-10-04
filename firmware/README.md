# Firmware shipped on the QRT stick

| File | For | Source | Licence |
|---|---|---|---|
| `ibt-11-5.sfi`, `ibt-11-5.ddc` | Intel Wireless 8260 Bluetooth (USB 8087:0a2b), loaded by `src/drivers/bt/hci.c` | Ubuntu `linux-firmware-intel-wireless` 20240318.git3b128b60-0ubuntu3.1 (`intel/ibt-11-5.*`), unmodified | Intel redistributable licence, `LICENCE.ibt_firmware` |
| `sof-cht.ri` | Intel Cherry Trail audio DSP (Venue 8 Pro 5855 speakers), Sound Open Firmware, loaded by `src/drivers/speaker.c` | thesofproject/sof-bin, `v2.1.x/sof-v2.1.1/sof-cht.ri`, unmodified | SOF firmware release licence (BSD 3-clause), `LICENCE.sof` |
| `iwlwifi-8000C-36.ucode` | Intel Wireless 8260 (Venue 8 Pro 5855 Wi-Fi) | Ubuntu `linux-firmware-intel-wireless` 20240318.git3b128b60-0ubuntu3.1 (linux-firmware.git, `intel/iwlwifi/iwlwifi-8000C-36.ucode`), unmodified | Intel redistributable licence, `LICENCE.iwlwifi_firmware` |
| `iwlwifi-6000g2b-6.ucode` | Intel Centrino Advanced-N 6230/6235 (Panasonic FZ-G1 Wi-Fi), loaded by `src/drivers/iwn/iwn.c` | linux-firmware.git (`iwlwifi-6000g2b-6.ucode`), unmodified | Intel redistributable licence, `LICENCE.iwlwifi_firmware` |

SHA-256: `sof-cht.ri` `e9c559eaa5ea4cce27a315819af5371edfc789b79c958af5ce8bf84eb15cba33`,
`iwlwifi-8000C-36.ucode` `479931721f5e168d69d67c297c11738acd75da390adb94f96030a1055f4cf57a`,
`ibt-11-5.sfi` `62e0ffb26d8f04adb5b9a2838ec7c60840bdabb38c024a98b905b991ed08e000`,
`ibt-11-5.ddc` `9e57302abc15ac991b71350705c4b5060f69c7111813bc5034704f6edea30872`,
`iwlwifi-6000g2b-6.ucode` `49f2408828b9b8ea3818f487a7c0b736d13879d4fcd21cf1082d41e03fb00e4f`.

The image puts these under `\lib\firmware\`; the driver loads
`/lib/firmware/iwlwifi-8000C-36.ucode` from QRT's file system.
