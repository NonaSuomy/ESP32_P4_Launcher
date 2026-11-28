# SD Card Speed Test

A native PAPP2 tool that measures sequential SD card write and read throughput.
It writes an 8 MiB generated pattern to `/sd/data/tool.sd_speedtest/benchmark.tmp`,
reads it back, checks every byte, and removes the temporary file when finished.
It does not modify ROMs or existing user files. If the device loses power during
the test, the next run removes the leftover benchmark file before starting.

Use **Run Test** on screen or press **A**. Press **B** to cancel and **X** to exit.
The write and read rates are reported in MiB/s. A successful readback check confirms
that the bytes returned by the card match the test pattern.
