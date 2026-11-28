# SD Card Speed Test

A native PAPP2 tool with CrystalDiskMark-style sequential and random SD tests:
sequential 1 MiB requests, sequential 4 KiB requests, and 256 scattered 4 KiB
requests for random read/write. It reports decimal MB/s for comparison with PC
tools. The PAPP filesystem API is synchronous, so results are single-request
Q1T1 measurements; queued Q8/Q32 tests are not available through this API.

The test writes an 8 MiB generated pattern to
`/sd/data/tool.sd_speedtest/benchmark.tmp`, verifies the full readback, and
removes the temporary file. It does not modify ROMs or existing user files. If
the device loses power during the test, the next run removes the leftover file.
On the Elecrow AIO the interface label shows its configured SDMMC bus (1-bit at
20 MHz); the exact model and capacity are in the launcher boot log.

Use **Run Test** on screen or press **A**. Press **B** to cancel and **X** to exit.
A successful readback check confirms that the bytes returned by the card match
the test pattern.
