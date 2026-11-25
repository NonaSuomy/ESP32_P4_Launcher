# fatfs (copy of ESP-IDF v5.5.6 components/fatfs, with exFAT and SD transfer recovery)

This copy overrides IDF's component (it is in EXTRA_COMPONENT_DIRS) and differs from it in two places.

## exFAT (`src/ffconf.h`)

ESP-IDF builds FatFs with exFAT off (`FF_FS_EXFAT 0`, no Kconfig option), so cards of 64 GB and more,
which come formatted as exFAT, did not mount.

- `#define FF_FS_EXFAT 1`
- `FF_USE_LABEL` is 0 or 1 instead of `CONFIG_FATFS_USE_LABEL`, which is undefined when the option is
  off: the exFAT code in ff.c uses it in a C expression, where IDF's own build (exFAT off) never does.

## SD transfer recovery (`diskio/diskio_sdmmc.c`)

After a read/write timeout or CRC error, the disk driver queries CMD13 and sends CMD12 when the card
is still in DATA/RCV. It waits for TRAN + READY and retries the failed operation. If a multi-sector
write still fails, it retries the range as single-sector CMD24 writes; this avoids the multi-block
write stop/count path that can leave affected cards in RCV. This also covers boards wired for one-bit
SD, where there is no narrower bus to fall back to. If recovery succeeds on a four-bit card, it then
switches both card and host to one-bit mode for later operations. The `sd_1bit` indicator reports only
that bus-width fallback; a recovered one-bit card remains at its existing width.

This handles cards that miss the controller's automatic STOP after a multi-block transfer. The card
can remain in the receive state (for example, status `0x400c00`) and subsequent operations otherwise
keep timing out (`0x107`). Recovery deliberately does not reinitialize the card (CMD0) or reset the
controller: the SDMMC controller is shared with the C6 SDIO host, and resetting it during SDIO traffic
can hang the board.

Recovery calls three functions from IDF's sdmmc component that its headers don't install
(`sdmmc_send_cmd`, `sdmmc_send_cmd_set_bus_width`, `sdmmc_send_cmd_send_status`, from
`sdmmc/sdmmc_common.h`). It declares them itself; check that they still exist after an IDF update.

## Moving to another ESP-IDF version

Copy the new version's `components/fatfs` here (without test_apps, host_test, test_fatfsgen), make the
two `ffconf.h` changes, and carry the diskio_sdmmc changes over (they are marked "launcher").
