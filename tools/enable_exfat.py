#!/usr/bin/env python3
"""Enable FatFs exFAT in the selected ESP-IDF 5.5 checkout.

ESP-IDF 5.5 hard-codes FF_FS_EXFAT to 0 in components/fatfs/src/ffconf.h;
there is no Kconfig option in that release. Newer ESP-IDF releases should use
their CONFIG_FATFS_FS_EXFAT option instead and do not need this patch.
"""

from __future__ import annotations

import os
import re
from pathlib import Path


def main() -> None:
    idf_path = os.environ.get("IDF_PATH")
    if not idf_path:
        raise SystemExit("IDF_PATH is not set. Source the ESP-IDF export script first.")

    config = Path(idf_path) / "components/fatfs/src/ffconf.h"
    if not config.is_file():
        raise SystemExit(f"FatFs config not found: {config}")

    source = config.read_text()
    updated = source
    if re.search(r"^\s*#define\s+FF_FS_EXFAT\s+1\s*$", updated, re.MULTILINE):
        pass
    else:
        updated, count = re.subn(
            r"^\s*#define\s+FF_FS_EXFAT\s+0\s*$",
            "#define FF_FS_EXFAT\t\t1",
            updated,
            count=1,
            flags=re.MULTILINE,
        )
        if count != 1:
            raise SystemExit(
                f"Expected ESP-IDF 5.5's single FF_FS_EXFAT=0 setting in {config}; "
                "leaving the toolchain unchanged. Use CONFIG_FATFS_FS_EXFAT on a "
                "newer ESP-IDF release."
            )

    # ESP-IDF 5.5 may reference this Kconfig symbol even when it is absent
    # from sdkconfig. PAX's known-compatible fallback keeps exFAT builds valid.
    plain_label = "#define FF_USE_LABEL\tCONFIG_FATFS_USE_LABEL"
    guarded_label = (
        "#ifdef CONFIG_FATFS_USE_LABEL\n"
        "#define FF_USE_LABEL\tCONFIG_FATFS_USE_LABEL\n"
        "#else\n"
        "#define FF_USE_LABEL\t0\n"
        "#endif"
    )
    nested_guarded_label = (
        "#ifdef CONFIG_FATFS_USE_LABEL\n" + guarded_label +
        "\n#else\n#define FF_USE_LABEL\t0\n#endif"
    )
    if nested_guarded_label in updated:
        updated = updated.replace(nested_guarded_label, guarded_label, 1)
    elif guarded_label in updated:
        pass
    elif plain_label in updated:
        updated = updated.replace(plain_label, guarded_label, 1)
    else:
        raise SystemExit(
            f"Could not recognize the FF_USE_LABEL setting in {config}; "
            "leaving the toolchain unchanged."
        )

    if updated != source:
        config.write_text(updated)
        print(f"Enabled FatFs exFAT and its IDF 5.5 compatibility fallback in {config}.")
    else:
        print(f"FatFs exFAT is already enabled in {config}.")


if __name__ == "__main__":
    main()
