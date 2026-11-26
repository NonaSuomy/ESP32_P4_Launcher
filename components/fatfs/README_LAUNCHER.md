# fatfs (copy of ESP-IDF v5.5.6 components/fatfs, with exFAT and SD transfer recovery)

This copy overrides IDF's component (it is in EXTRA_COMPONENT_DIRS); see the exFAT, file-lock, and
1-bit fallback notes below.

## exFAT (`src/ffconf.h`)

ESP-IDF builds FatFs with exFAT off (`FF_FS_EXFAT 0`, no Kconfig option), so cards of 64 GB and more,
which come formatted as exFAT, did not mount.

- `#define FF_FS_EXFAT 1`
- `FF_USE_LABEL` is 0 or 1 instead of `CONFIG_FATFS_USE_LABEL`, which is undefined when the option is
  off: the exFAT code in ff.c uses it in a C expression, where IDF's own build (exFAT off) never does.

## File locks (`launcher/sdkconfig.defaults`)

FatFs file locks are enabled for 32 open objects (`CONFIG_FATFS_FS_LOCK=32`). With the default of 0,
FatFs does not prevent a task from opening the same file twice or removing/renaming a file that another
task still has open. The web file manager, DevLink, and launcher services can all write the SD card, so
the launcher also uses a shared mutation lock for remote uploads, Store, online updates, settings,
Bluetooth data, saved UI lists, and ZIP extraction, with separate temporary upload names for the two
remote upload services.
File replacement renames an existing destination to a backup first and attempts to restore it if committing
the new file fails, instead of unlinking the old file before the replacement is ready.

## SD transfer recovery (`diskio/diskio_sdmmc.c`)

On a one-bit bus, the disk driver writes multiple sectors as individual CMD24 operations. The AIO's
card can time out on each multi-block CMD25 transfer, so waiting for recovery on every stdio buffer
makes uploads crawl and can outlast host transfer timeouts. On a wider bus, the driver first uses the
normal multi-block transfer. After a read/write timeout or CRC error it queries CMD13, sends CMD12
when the card is still in DATA/RCV, waits for TRAN + READY, and retries the failed write as
single-sector CMD24 operations. If recovery succeeds on a four-bit card, it then switches both card
and host to one-bit mode for later operations. The `sd_1bit` indicator reports only that bus-width
fallback; a card already wired for one-bit remains at its existing width.

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
