#!/usr/bin/env python3
"""Apply PAX's ESP32-P4 forced-FS root-port workaround to ESP-IDF 5.5.x."""
from __future__ import annotations

import argparse
import os
from pathlib import Path


def replace_once(path: Path, old: str, new: str) -> bool:
    text = path.read_text()
    if new in text:
        return False
    if old not in text:
        raise SystemExit(f"Could not find expected ESP-IDF 5.5 source block in {path}")
    path.write_text(text.replace(old, new, 1))
    return True


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("idf_path", nargs="?", default=os.environ.get("IDF_PATH"))
    args = parser.parse_args()
    if not args.idf_path:
        parser.error("pass the ESP-IDF checkout path or set IDF_PATH")
    root = Path(args.idf_path).resolve()
    ll = root / "components/hal/esp32p4/include/hal/usb_dwc_ll.h"
    hal = root / "components/hal/usb_dwc_hal.c"
    if not ll.is_file() or not hal.is_file():
        raise SystemExit(f"{root} does not look like an ESP-IDF checkout")

    changes = 0
    changes += replace_once(ll,
        "static inline void usb_dwc_ll_hcfg_set_fsls_supp_only(usb_dwc_dev_t *hw)\n{\n    hw->hcfg_reg.fslssupp = 1;\n}\n",
        "static inline bool usb_dwc_ll_is_hs_controller(usb_dwc_dev_t *hw)\n{\n    return hw == &USB_DWC_HS;\n}\n\n"
        "static inline void usb_dwc_ll_hcfg_init_hs_fsls_host(usb_dwc_dev_t *hw)\n{\n"
        "    hw->hcfg_reg.fslssupp = 1;\n    hw->hcfg_reg.fslspclksel = 0;\n}\n\n"
        "static inline void usb_dwc_ll_hcfg_set_utmi_fsls_phy_clock(usb_dwc_dev_t *hw)\n{\n"
        "    hw->hcfg_reg.fslspclksel = 0;\n}\n\n"
        "static inline void usb_dwc_ll_hfir_set_utmi_frame_interval(usb_dwc_dev_t *hw)\n{\n"
        "    usb_dwc_hfir_reg_t hfir;\n    hfir.val = hw->hfir_reg.val;\n"
        "    hfir.hfirrldctrl = 0;\n    hfir.frint = 29999;\n    hw->hfir_reg.val = hfir.val;\n}\n")
    changes += replace_once(ll,
        "static inline usb_dwc_speed_t usb_dwc_ll_hprt_get_speed(usb_dwc_dev_t *hw)\n{\n    return (usb_dwc_speed_t)hw->hprt_reg.prtspd;\n}\n",
        "static inline usb_dwc_speed_t usb_dwc_ll_hprt_get_speed(usb_dwc_dev_t *hw)\n{\n"
        "    usb_dwc_speed_t speed = (usb_dwc_speed_t)hw->hprt_reg.prtspd;\n"
        "    if (usb_dwc_ll_is_hs_controller(hw) && speed == USB_DWC_SPEED_HIGH) {\n"
        "        speed = USB_DWC_SPEED_FULL;\n    }\n    return speed;\n}\n")
    changes += replace_once(ll,
        "static inline void usb_dwc_ll_hprt_set_port_reset(usb_dwc_dev_t *hw, bool reset)\n{\n    usb_dwc_hprt_reg_t hprt;\n",
        "static inline void usb_dwc_ll_hprt_set_port_reset(usb_dwc_dev_t *hw, bool reset)\n{\n"
        "    if (reset && usb_dwc_ll_is_hs_controller(hw)) {\n        usb_dwc_ll_hcfg_init_hs_fsls_host(hw);\n    }\n"
        "    usb_dwc_hprt_reg_t hprt;\n")
    changes += replace_once(hal,
        "    //Enable host mode\n    usb_dwc_ll_gusbcfg_force_host_mode(hal->dev);\n",
        "    //Enable host mode\n    usb_dwc_ll_gusbcfg_force_host_mode(hal->dev);\n"
        "#if CONFIG_IDF_TARGET_ESP32P4\n    if (usb_dwc_ll_is_hs_controller(hal->dev)) {\n"
        "        usb_dwc_ll_hcfg_init_hs_fsls_host(hal->dev);\n    }\n#endif\n")
    changes += replace_once(hal,
        "    if (hal->constant_config.hsphy_type == 0) {\n        usb_dwc_ll_hcfg_set_fsls_phy_clock(hal->dev);\n        usb_dwc_ll_hfir_set_frame_interval(hal->dev);\n    }\n",
        "    if (hal->constant_config.hsphy_type == 0) {\n        usb_dwc_ll_hcfg_set_fsls_phy_clock(hal->dev);\n"
        "        usb_dwc_ll_hfir_set_frame_interval(hal->dev);\n"
        "#if CONFIG_IDF_TARGET_ESP32P4\n    } else if (usb_dwc_ll_is_hs_controller(hal->dev)) {\n"
        "        usb_dwc_ll_hcfg_set_utmi_fsls_phy_clock(hal->dev);\n"
        "        usb_dwc_ll_hfir_set_utmi_frame_interval(hal->dev);\n#endif\n    }\n")
    print("P4 USB TT HAL workaround already applied." if changes == 0 else f"Applied P4 USB TT HAL workaround ({changes} source edits).")


if __name__ == "__main__":
    main()
