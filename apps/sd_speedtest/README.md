# SD Card Speed Test

A native PAPP2 tool with CrystalDiskMark-style sequential and random SD tests:
sequential 1 MiB requests, sequential 4 KiB requests, and 256 scattered 4 KiB
requests for random read/write. Select one pass for a quick check or three passes
for an average in decimal MB/s. The default test uses an 8 MiB working file; tap
the size control or press Left/Right to select the optional 64 MiB test. The
PAPP filesystem API is synchronous, so results are single-request Q1T1
measurements; queued Q8/Q32 tests are not available through this API.

Choose an 8 MiB or 64 MiB test file and either a quick single pass or a more
repeatable three-pass average. Touch the selectors, use Left/Right to change
size, or Up to switch between one and three passes. The test writes a generated
pattern to
`/sd/data/tool.sd_speedtest/benchmark.tmp`, verifies the full readback, and
removes the temporary file. It does not modify ROMs or existing user files. If
the device loses power during the test, the next run removes the leftover file.
The screen shows total/free volume space and filesystem type on launchers with
PAPP FS v3. On the Elecrow AIO it also shows the configured SDMMC bus (1-bit at
20 MHz); the card model is in the launcher boot log.

Use **Run Test** on screen or press **A**. Tap a selector or use
**Left/Right** for size and **Up** for pass count. Press **B** to cancel and
**X** to exit; the top-right touch X also closes the app.
