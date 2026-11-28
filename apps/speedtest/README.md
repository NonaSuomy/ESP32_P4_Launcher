# Speedtest

A native PAPP2 network diagnostic for the ESP32-P4 Launcher. It discovers nearby
Ookla Speedtest servers, then measures latency, download throughput, and upload
throughput using the Speedtest.net web-client endpoints. The single-stream test is
intended for device diagnostics; results can differ from the multi-stream
Speedtest.net apps.

## Controls

- **A / touch:** Start, change server, or toggle the server label, depending on focus.
- **D-pad / left stick:** Move focus and navigate the server picker.
- **B:** Cancel a running test or close the server picker.
- **X:** Exit to the launcher.
- **Y:** Change server.

The device must have working Wi-Fi and Internet access. Server discovery and test
traffic go directly to Speedtest.net and the selected server; no results are sent
to a PAX or project server.
