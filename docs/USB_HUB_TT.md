# ESP32-P4 USB hub transaction-translator support

This PR vendors the ESP-IDF USB host component with hub support and enables
`CONFIG_USB_HOST_HUBS_SUPPORTED` in launcher defaults. It also applies the P4
HS-port workaround needed to enumerate full-speed devices behind a hub.

The workaround is specific to ESP-IDF 5.5.x. After sourcing that toolchain's
`export.sh`, run:

```sh
python tools/apply_p4_usb_tt_fix.py "$IDF_PATH"
```

The helper checks for the expected upstream source blocks, is idempotent, and
stops if it sees an unfamiliar version. It edits the selected ESP-IDF checkout,
so keep a clean toolchain copy or review the changes before applying. Remove the
helper once the upstream ESP-IDF USB host supports this P4 configuration.

The copied `components/espressif__usb` component and its associated upstream
license are kept together. This change is isolated from Switch controller
parsing; a controller can be tested directly before adding a hub.
