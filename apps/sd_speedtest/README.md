# SD Card Speed Test

A native PAPP2 tool with CrystalDiskMark-style sequential and random SD tests:
sequential 1 MiB requests, sequential 4 KiB requests, and 256 scattered 4 KiB
requests for random read/write. Each profile runs three times and reports the
average in decimal MB/s. The default quick test uses an 8 MiB working file; tap
the size control or press Left/Right to select the optional 64 MiB test. The
PAPP filesystem API is synchronous, so results are single-request Q1T1
measurements; queued Q8/Q32 tests are not available through this API.

The test writes a generated pattern to
`/sd/data/tool.sd_speedtest/benchmark.tmp`, verifies the full readback, and
removes the temporary file. It does not modify ROMs or existing user files. If
the device loses power during the test, the next run removes the leftover file.
The screen shows total/free volume space and filesystem type on launchers with
PAPP FS v3. On the Elecrow AIO it also shows the configured SDMMC bus (1-bit at
20 MHz); the card model is in the launcher boot log.

Use **Run Test** on screen or press **A**. Tap the size control or press
**Left/Right** to change test size. Press **B** to cancel and **X** to exit;
the top-right touch X also closes the app.
