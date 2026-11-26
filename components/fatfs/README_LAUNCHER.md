# fatfs (copy of ESP-IDF v5.5.6 components/fatfs, with exFAT on and a 1-bit SD fallback)

This copy overrides IDF's component (it is in EXTRA_COMPONENT_DIRS) and differs from it in two places.

## exFAT (`src/ffconf.h`)

ESP-IDF builds FatFs with exFAT off (`FF_FS_EXFAT 0`, no Kconfig option), so cards of 64 GB and more,
which come formatted as exFAT, did not mount.

- `#define FF_FS_EXFAT 1`
- `FF_USE_LABEL` is 0 or 1 instead of `CONFIG_FATFS_USE_LABEL`, which is undefined when the option is
  off: the exFAT code in ff.c uses it in a C expression, where IDF's own build (exFAT off) never does.

## 1-bit fallback (`diskio/diskio_sdmmc.c`, `diskio/diskio_sdmmc.h`)

When a 4-bit read or write of an SD card fails with a timeout or CRC error, `ff_sdmmc_fall_back()`
moves the card to the 1-bit bus, which needs only D0, and repeats the transfer once. The card then
stays 1-bit until the next boot. `ff_sdmmc_bus_fell_back()` reports it; Settings and DevLink `info`
(`sd_1bit`) show it.

Seen on a Guition 7" with a v3.2 chip, probably a bad contact on D1–D3. After a multi-block write the
card doesn't answer the controller's automatic STOP and stays in the receive state. Every later write
then times out (0x107, card status 0x...d00) until a reboot. The recovery:

1. Send STOP (CMD12) by hand until CMD13 reports the transfer state.
2. Send ACMD6 for 1 bit.
3. Set the host slot to 1 bit.

It does neither a card re-init (CMD0) nor a controller reset: the controller is shared with the C6
on SDIO slot 1. A reset hit the C6's running transfers and hung the board. A re-init left the
controller "hardware locked" (0x1000) for every later command.

The fallback calls three functions from IDF's sdmmc component that its headers don't install
(`sdmmc_send_cmd`, `sdmmc_send_cmd_set_bus_width`, `sdmmc_send_cmd_send_status`, from
`sdmmc/sdmmc_common.h`). It declares them itself; check that they still exist after an IDF update.

## Moving to another ESP-IDF version

Copy the new version's `components/fatfs` here (without test_apps, host_test, test_fatfsgen), make the
two `ffconf.h` changes, and carry the diskio_sdmmc changes over (they are marked "launcher").
