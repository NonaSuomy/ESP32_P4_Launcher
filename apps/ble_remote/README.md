# BLE Remote (PAPP2)

This app advertises a Bluetooth LE HID peripheral named **PAX BLE Remote**. Pair it from the receiving device's Bluetooth settings, then use Keyboard, Mouse or Gamepad controls. The matching **BLE Input Test** app scans for this HID service and displays received reports.

The app uses PAPP2 BLE GATT, report-reference descriptors and bonded LE Secure Connections. It advertises while the app is open; closing the app stops advertising and drops the active link. The PAPP2 app has one active host connection and does not implement saved host slots or the older PAX launcher service bridge.

Build it with `python3 apps/ble_remote/build_papp2.py`. The script writes its build tree below `~/code/pax/pax-dev/build/` and the store artifact to `community/ble-remote/ble-remote.papp` by default.

Hardware interoperability should be checked with the BLE Input Test PAPP2 app on a second P4. This PR has a reproducible package build but no paired-device test result yet.
