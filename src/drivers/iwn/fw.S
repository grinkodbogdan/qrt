/* fw.S - the Centrino Advanced-N 6235's firmware (linux-firmware iwlwifi-6000g2b-6.ucode,
 * Intel's redistributable licence: firmware/LICENCE.iwlwifi_firmware), linked into the
 * kernel so Wi-Fi does not depend on the boot stick's file names. */
    .section .rdata, "dr"
    .p2align 4
    .globl iwn_fw_blob
iwn_fw_blob:
    .incbin "firmware/iwlwifi-6000g2b-6.ucode"
    .globl iwn_fw_blob_end
iwn_fw_blob_end:
