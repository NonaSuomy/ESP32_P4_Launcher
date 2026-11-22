#!/usr/bin/env python3
"""Performance benchmark: builds apps/perf_bench, runs it on one or more boards with the panel's
scan-out on and off, and writes a comparison.

    python tools/perf_bench.py build
    python tools/perf_bench.py run COM10 COM6 [--frames 600] [--only cpu,mem,ppa,3d]
    python tools/perf_bench.py report                 # again, from the saved results

Tests (apps/perf_bench/main.c): CoreMark on 1 and 2 cores; PSRAM and internal SRAM read / write /
copy and load latency; PPA fill and 2x scale; the e3d 3D engine (from ../ESP32_P4_3D_Engine, or
E3D_DIR) on a fixed scene, into an unseen buffer and on screen. "display off" stops the LCD refresh,
which otherwise reads the whole frame buffer from PSRAM 60 times a second (1024x600x2 = ~74 MB/s
on the 7"). Results: build/perf_bench/<board>.json, report: build/perf_bench/report.md.
"""
import argparse
import datetime
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
APP = os.path.join(ROOT, "apps", "perf_bench")
OUT = os.path.join(ROOT, "build", "perf_bench")
DEV = os.path.join(HERE, "papp_dev.py")


def find_e3d():
    """E3D_DIR, else ESP32_P4_3D_Engine/e3d beside this checkout (or beside any folder above it)"""
    if os.environ.get("E3D_DIR"):
        return os.environ["E3D_DIR"]
    d = ROOT
    while os.path.dirname(d) != d:
        d = os.path.dirname(d)
        cand = os.path.join(d, "ESP32_P4_3D_Engine", "e3d")
        if os.path.isdir(cand):
            return cand
    return os.path.join(os.path.dirname(ROOT), "ESP32_P4_3D_Engine", "e3d")


E3D = find_e3d()

CM_FLAGS = ["-O3", "-funroll-all-loops", "-finline-limit=600", "-fno-common"]


# The e3d engine's SIMD code exists in two encodings, PIE for v1.x chips (-march ..._xesppie) and
# _xespv for v3.x (the same instructions, other encodings: each faults on the other generation).
# The engine builds both and picks one by the chip revision (e3d/src/e3d_simd.h, launcher >= 0.33),
# so one build runs on every board. perf_bench: the engine in plain C (-DE3D_NO_SIMD), for the
# comparison; perf_bench_simd: the engine as games build it.
SIMD_FILES = {"e3d_simd_pie.c": ["-march=rv32imafc_zicsr_zifencei_xesppie"],
              "e3d_simd_espv.c": ["-march=rv32imafc_zicsr_zifencei_xespv"]}


def papp_path(flavour=None):
    name = "perf_bench" + (f"_{flavour}" if flavour else "")
    return os.path.join(ROOT, "build", "apps", name, name + ".papp")


def build_one(flavour=None):
    """flavour None: every test, the 3D engine in plain C; "simd": the engine with its SIMD code"""
    sys.path.insert(0, os.path.join(ROOT, "papp-sdk", "tools"))
    import papp_build
    cm = os.path.join(APP, "coremark")
    cm_src = [os.path.join(cm, f) for f in ("core_list_join.c", "core_main.c", "core_matrix.c",
                                            "core_state.c", "core_util.c", "core_portme.c")]
    src = [os.path.join(APP, "main.c")] + cm_src
    per = {s: list(CM_FLAGS) for s in cm_src}
    per[os.path.join(cm, "core_main.c")].append("-Dmain=coremark_main")
    inc = [cm]
    cflags = ["-DITERATIONS=0", "-DPERFORMANCE_RUN=1"]
    if os.path.isfile(os.path.join(E3D, "src", "e3d_simd_pie.c")):
        src += [os.path.join(E3D, "src", f) for f in ("e3d.c", "e3d_render.c", *SIMD_FILES)]
        per.update({os.path.join(E3D, "src", f): list(fl) for f, fl in SIMD_FILES.items()})
        if flavour:
            cflags.append('-DPERF_3D_SUFFIX="_simd"')
        else:
            cflags.append("-DE3D_NO_SIMD")
        inc += [os.path.join(E3D, "include"), os.path.join(E3D, "src")]
        cflags.append("-DPERF_E3D")
    elif flavour:
        return None
    else:
        print(f"perf_bench: no e3d engine with e3d_simd_pie.c at {E3D} (set E3D_DIR): building without the 3D test")
    name = "perf_bench" + (f"_{flavour}" if flavour else "")
    papp_build.build(name, src, includes=inc, libc=True,
                     app_cflags=["-O2"] + cflags, per_file_cflags=per,
                     version_dir=APP if os.path.isfile(os.path.join(APP, "version.txt")) else None)
    return papp_path(flavour)


def build():
    for f in (None, "simd"):
        p = build_one(f)
        if p:
            print(p)


def dev(port, *args, timeout=60):
    r = subprocess.run([sys.executable, DEV, "--port", port, *args], capture_output=True, text=True,
                       encoding="utf-8", errors="replace", timeout=timeout + 60)
    return r.returncode, r.stdout + r.stderr


SD_BROKEN = re.compile(r"sdmmc_write_blocks failed|cannot open dir /sd|write failed \(SD full")


def run_test(port, label, args, timeout, papp=None, settle=0):
    code, out = dev(port, "deploy", papp or papp_path(), "--timeout", str(timeout), "--", *args, timeout=timeout)
    if SD_BROKEN.search(out):
        # Seen on the v3.x 7" (C6 firmware 2.3.2): an SD write while the C6 link is busy breaks
        # the card until a reboot. Reboot, wait for the network to go quiet, try once more.
        print(f"  {label}: SD card failed, rebooting the board and trying again", flush=True)
        dev(port, "reboot")
        time.sleep(60)
        code, out = dev(port, "deploy", papp or papp_path(), "--timeout", str(timeout), "--", *args, timeout=timeout)
    if settle:
        time.sleep(settle)      # the launcher resumes the Wi-Fi when the app ends
    res = {}
    for m in re.finditer(r"RESULT (\S+) (\S+) (\S+)", out):
        res[m.group(1)] = (float(m.group(2)), m.group(3))
    fails = re.findall(r"FAIL: .*", out)
    print(f"  {label:<24} exit {code}: " + ", ".join(f"{k}={v[0]:g}" for k, v in res.items())
          + ("  " + "; ".join(fails) if fails else ""), flush=True)
    if code != 0 and not res:
        print(out[-2000:])
    return res, code, out


def run(ports, frames, only, settle=0):
    os.makedirs(OUT, exist_ok=True)
    for port in ports:
        code, out = dev(port, "info")
        info = json.loads(out[out.index("{"):out.rindex("}") + 1])
        board = info["board"]
        print(f"{port}: {board} ({info['board_name']}), launcher {info['launcher']}", flush=True)
        data = {"port": port, "board": board, "board_name": info["board_name"],
                "launcher": info["launcher"], "date": datetime.datetime.now().isoformat(timespec="minutes"),
                "frames": frames, "on": {}, "off": {}, "log": {}}
        simd = papp_path("simd")
        plan = [("clock", ["clock"], 20, None)]
        if "cpu" in only: plan.append(("cpu", ["cpu"], 150, None))
        if "mem" in only: plan.append(("mem", ["mem"], 120, None))
        if "ppa" in only: plan.append(("ppa", ["ppa"], 60, None))
        if "3d" in only:
            plan.append(("3d", ["3d", "off", str(frames)], 120, None))
            plan.append(("3d_show", ["3d", "show", str(frames)], 120, None))
            if os.path.isfile(simd):
                plan.append(("3d_simd", ["3d", "off", str(frames)], 120, simd))
                plan.append(("3d_simd_show", ["3d", "show", str(frames)], 120, simd))
        try:
            for state in ("on", "off"):
                if state == "off":
                    c, o = dev(port, "display", "off")
                    print(f"  display off: {o.strip()}", flush=True)
                    if c != 0:
                        break
                for label, args, tmo, papp in plan:
                    if state == "off" and (label in ("ppa", "clock") or label.endswith("_show")):
                        continue        # these need the screen, or don't depend on it
                    res, code, out = run_test(port, f"{label} (screen {state})", args, tmo, papp, settle)
                    data[state].update({k: v[0] for k, v in res.items()})
                    data["log"][f"{label}_{state}"] = [l for l in out.splitlines()
                                                       if "[app]" in l or "FAIL" in l][-60:]
        finally:
            c, o = dev(port, "display", "on")
            print(f"  display on: {o.strip()}", flush=True)
            for n in ("perf_bench", "perf_bench_simd", "perf_bench_pie", "perf_bench_espv"):
                dev(port, "rm", f"/sd/apps/{n}/{n}.papp")
                dev(port, "rm", f"/sd/apps/{n}")
        with open(os.path.join(OUT, f"{board}.json"), "w") as f:
            json.dump(data, f, indent=1)
    report()


ROWS = [  # key, label, higher is better
    ("cpu_mhz", "CPU clock (MHz)", True),
    ("coremark_1core", "CoreMark, 1 core", True),
    ("coremark_2core", "CoreMark, 2 cores", True),
    ("coremark_1core_mhz", "CoreMark/MHz, 1 core", True),
    ("psram_read", "PSRAM read, 1 core (MB/s)", True),
    ("psram_write", "PSRAM write, 1 core (MB/s)", True),
    ("psram_copy", "PSRAM memcpy, read+write (MB/s)", True),
    ("psram_read_2core", "PSRAM read, 2 cores (MB/s)", True),
    ("psram_write_2core", "PSRAM write, 2 cores (MB/s)", True),
    ("psram_latency", "PSRAM load latency (ns)", False),
    ("sram_read", "SRAM read (MB/s)", True),
    ("sram_write", "SRAM write (MB/s)", True),
    ("sram_copy", "SRAM memcpy (MB/s)", True),
    ("sram_latency", "SRAM load latency (ns)", False),
    ("ppa_fill", "PPA fill 1024x600 (Mpix/s)", True),
    ("ppa_scale2x", "PPA scale 2x to 1024x600 (Mpix/s)", True),
    ("3d_off_fps", "3D (engine in C), unseen buffer (fps)", True),
    ("3d_off_render_ms", "3D render time, unseen (ms/frame)", False),
    ("3d_off_raster_ms", "3D raster time, unseen (ms/frame)", False),
    ("3d_show_fps", "3D (engine in C) on screen (fps)", True),
    ("3d_show_render_ms", "3D render time, on screen (ms/frame)", False),
    ("3d_off_simd_fps", "3D with the chip's SIMD, unseen buffer (fps)", True),
    ("3d_off_simd_render_ms", "3D with SIMD render time, unseen (ms/frame)", False),
    ("3d_show_simd_fps", "3D with SIMD on screen (fps)", True),
    ("3d_off_tris", "3D triangles per frame (incl. shadows)", None),
]


def report():
    boards = []
    for f in sorted(os.listdir(OUT)) if os.path.isdir(OUT) else []:
        if f.endswith(".json"):
            boards.append(json.load(open(os.path.join(OUT, f))))
    if not boards:
        sys.exit("no results")
    boards.sort(key=lambda b: b["board"].endswith("_v3"))       # v1.x first
    for b in boards:
        for st in ("on", "off"):
            d = b[st]
            mhz = b["on"].get("cpu_mhz")
            if mhz and "coremark_1core" in d:
                d["coremark_1core_mhz"] = d["coremark_1core"] / mhz
    cols = []
    for b in boards:
        for st in ("on", "off"):
            cols.append((b, st))
    hdr = "| Test | " + " | ".join(f"{b['board']} screen {st}" for b, st in cols)
    if len(boards) == 2:
        hdr += " | v3 vs v1, screen on | v3 vs v1, screen off"
    lines = ["# Performance: " + " vs ".join(f"{b['board']} ({b['board_name']})" for b in boards), "",
             "Run " + ", ".join(f"{b['board']} {b['date']} (launcher {b['launcher']})" for b in boards)
             + f". 3D: {boards[0]['frames']} frames of a fixed 400x240 scene.", "",
             hdr + " |", "|---" * (hdr.count("|")) + "|"]
    for key, label, hib in ROWS:
        vals = [b[st].get(key) for b, st in cols]
        if all(v is None for v in vals):
            continue
        cells = ["" if v is None else f"{v:,.1f}" if abs(v) >= 10 else f"{v:.2f}" for v in vals]
        row = f"| {label} | " + " | ".join(cells)
        if len(boards) == 2:
            for st in ("on", "off"):
                a, c = boards[0][st].get(key), boards[1][st].get(key)
                if a and c and hib is not None:
                    pct = (c / a - 1) * 100 if hib else (a / c - 1) * 100
                    row += f" | {pct:+.0f}%"
                else:
                    row += " | "
        lines.append(row + " |")
    lines += ["", "Percentages: how much better the v3 board does (positive = faster, for latency and times "
              "too). \"screen off\" = the panel's scan-out stopped (DevLink `display off`)."]
    path = os.path.join(OUT, "report.md")
    open(path, "w", encoding="utf-8").write("\n".join(lines) + "\n")
    print("\n".join(lines))
    print(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("build")
    p = sub.add_parser("run")
    p.add_argument("ports", nargs="+")
    p.add_argument("--frames", type=int, default=600)
    p.add_argument("--only", default="cpu,mem,ppa,3d")
    p.add_argument("--no-build", action="store_true")
    p.add_argument("--settle", type=int, default=0, help="seconds to wait after each test (Wi-Fi resume)")
    sub.add_parser("report")
    a = ap.parse_args()
    if a.cmd == "build":
        build()
    elif a.cmd == "run":
        if not a.no_build:
            build()
        run(a.ports, a.frames, a.only.split(","), a.settle)
    else:
        report()


if __name__ == "__main__":
    main()
