# `.pax` filenames for PAPP2 apps

The launcher can list and launch a PAPP2 container named with either `.papp` or
`.pax`. Both suffixes go through the same PAPP2 loader and magic/header checks;
this change does not implement or load legacy PAPP1 binaries. `.papp` remains the
usual build and Store output name.

`papp_dev.py deploy` preserves the suffix supplied on the host, so it can be used
to deploy an already-built PAPP2 file named `.pax` without renaming it first.
