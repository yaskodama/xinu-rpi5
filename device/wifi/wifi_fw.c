/* device/wifi/wifi_fw.c — embedded BCM43455/CYW43455 SDIO firmware blobs.
 *
 * Same silicon as the Pi 3 B+ (BCM43455), so the same firmware + CLM work on
 * the Pi 4.  Blobs are nonfree (RPi-Distro/firmware-nonfree), .gitignore'd
 * under wifi-fw/:
 *   wifi-fw/fw_43455.bin    = cyfmac43455-sdio-MINIMAL.bin (~548 KB, has FWSUP)
 *   wifi-fw/nvram_43455.txt = brcmfmac43455-sdio.txt (Pi 3 B+ board nvram)
 *   wifi-fw/clm_43455.blob  = clm blob (chip-specific, board-independent)
 *
 * Linked into the kernel image via .incbin (absolute paths; the assembler
 * runs from compile/).  wifi.c streams them into chip RAM over SDIO.
 */
#ifdef WIFI_SDIO_BASE
asm(
    ".section .rodata\n"
    ".balign 4\n"
    ".globl wifi_fw_bin\n"
    "wifi_fw_bin:\n"
    "  .incbin \"/Users/kodamay/projects/xinu-rpi5/wifi-fw/fw_43455.bin\"\n"
    ".globl wifi_fw_bin_end\n"
    "wifi_fw_bin_end:\n"
    ".balign 4\n"
    ".globl wifi_nvram_txt\n"
    "wifi_nvram_txt:\n"
    "  .incbin \"/Users/kodamay/projects/xinu-rpi5/wifi-fw/nvram_43455.txt\"\n"
    ".globl wifi_nvram_txt_end\n"
    "wifi_nvram_txt_end:\n"
    ".balign 4\n"
    ".globl wifi_clm_blob\n"
    "wifi_clm_blob:\n"
    "  .incbin \"/Users/kodamay/projects/xinu-rpi5/wifi-fw/clm_43455.blob\"\n"
    ".globl wifi_clm_blob_end\n"
    "wifi_clm_blob_end:\n"
    /* Boot WiFi auto-connect config: line1=SSID, line2=password.  Lives in the
     * .gitignore'd wifi-fw/wifi.conf so the password is embedded in the image
     * but never committed.  Empty file => auto-connect disabled. */
    ".balign 4\n"
    ".globl wifi_conf\n"
    "wifi_conf:\n"
    "  .incbin \"/Users/kodamay/projects/xinu-rpi5/wifi-fw/wifi.conf\"\n"
    ".globl wifi_conf_end\n"
    "wifi_conf_end:\n"
    ".balign 4\n"
);
#endif /* WIFI_SDIO_BASE */
