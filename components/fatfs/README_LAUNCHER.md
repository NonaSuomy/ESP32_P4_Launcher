# fatfs (copy of ESP-IDF v5.5.6 components/fatfs, with exFAT on)

ESP-IDF builds FatFs with exFAT off (`FF_FS_EXFAT 0` in `src/ffconf.h`, no Kconfig option), so cards
of 64 GB and more, which come formatted as exFAT, did not mount. This copy overrides IDF's component
(it is in EXTRA_COMPONENT_DIRS) and differs from it in `src/ffconf.h` only:

- `#define FF_FS_EXFAT 1`
- `FF_USE_LABEL` is 0 or 1 instead of `CONFIG_FATFS_USE_LABEL`, which is undefined when the option is
  off: the exFAT code in ff.c uses it in a C expression, where IDF's own build (exFAT off) never does.

When moving to another ESP-IDF version, copy its `components/fatfs` here again (without test_apps,
host_test, test_fatfsgen) and make the same two changes.
