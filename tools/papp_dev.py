#!/usr/bin/env python3
"""
papp_dev.py — DevLink host tool for the ESP32-P4 Launcher.

Talks the framed DevLink protocol (components/lx_devlink/include/devlink_proto.h)
over the USB-Serial-JTAG port.

  python tools/papp_dev.py [--port COM9] info
  python tools/papp_dev.py ls /sd/apps
  python tools/papp_dev.py put build/apps/hello/hello.papp /sd/apps/hello/hello.papp
  python tools/papp_dev.py get /sd/data/x.txt local.txt
  python tools/papp_dev.py rm /sd/apps/hello/hello.papp
  python tools/papp_dev.py run /sd/apps/hello/hello.papp [args...]
  python tools/papp_dev.py stop [--grace 2000]
  python tools/papp_dev.py status | crash | crash-clear | reboot
  python tools/papp_dev.py shot [screen.png]
  python tools/papp_dev.py log [--seconds N]
  python tools/papp_dev.py deploy build/apps/hello/hello.papp [--timeout 30] [-- args...]

`deploy` preserves the input `.papp` or `.pax` filename in /sd/apps/<name>/,
runs it, and streams logs until the
app exits (or timeout → force stop) and exits with the app's return code
(124 on timeout, 125 on DevLink error) — so it can be used from scripts/CI.

Port: --port, else $PAPP_PORT, else auto-detect the Espressif USB-Serial-JTAG (VID 303A).
"""

import argparse
import json
import os
import struct
import sys
import threading
import time
import zlib

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.exit("pyserial missing: pip install pyserial")

SOF = b"\xA5\x44"
HDR = struct.Struct("<BBHI")            # type, flags, seq, len
F_ERROR, F_APP = 0x01, 0x02
CHUNK = 16 * 1024

PING, INFO, TIME = 0x01, 0x02, 0x03
LS, STAT, MKDIR, RM, RENAME = 0x10, 0x11, 0x12, 0x13, 0x14
PUT_BEGIN, PUT_DATA, PUT_END, GET = 0x20, 0x21, 0x22, 0x28
RUN, STOP, STATUS = 0x30, 0x31, 0x32
REBOOT, CRASH, CRASH_CLEAR, BOOTLOG = 0x40, 0x41, 0x42, 0x43
NET = 0x52
SHOT, INPUT = 0x50, 0x51
INPUT_PAD, INPUT_KEY, INPUT_STATE, INPUT_UNMAP = 1, 2, 3, 4

# PAPP_BTN_* bit positions (papp_abi.h)
BUTTONS = {"A": 0, "B": 1, "X": 2, "Y": 3, "L1": 4, "R1": 5, "L2": 6, "R2": 7, "SELECT": 8,
           "START": 9, "L3": 10, "R3": 11, "HOME": 12, "MISC": 13,
           "UP": 16, "DOWN": 17, "LEFT": 18, "RIGHT": 19}
# HID keyboard usages for named keys; letters/digits are derived
KEYS = {"ENTER": 0x28, "ESC": 0x29, "BACKSPACE": 0x2A, "TAB": 0x2B, "SPACE": 0x2C,
        "RIGHT": 0x4F, "LEFT": 0x50, "DOWN": 0x51, "UP": 0x52, "F1": 0x3A, "F12": 0x45}
US_PLAIN = "abcdefghijklmnopqrstuvwxyz1234567890\r\x1b\b\t -=[]\\#;'`,./"
US_SHIFT = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\r\x1b\b\t _+{}|~:\"~<>?"


def buttons_mask(spec):
    mask = 0
    for name in spec.upper().replace(",", "+").split("+"):
        if name not in BUTTONS:
            raise DevLinkError(f"unknown button {name}; one of {', '.join(BUTTONS)}")
        mask |= 1 << BUTTONS[name]
    return mask


def buttons_names(mask):
    return " ".join(n for n, b in BUTTONS.items() if mask & (1 << b)) or "-"


def key_usage(name):
    """-> (usage, mods) for a key name, a single character or a hex usage."""
    if name.upper() in KEYS:
        return KEYS[name.upper()], 0
    if len(name) == 1:
        non_us = 0x32 - 0x04                 # the non-US '#' key: never the first choice
        for table, mods in ((US_PLAIN, 0), (US_SHIFT, 0x02)):
            hits = [i for i, c in enumerate(table) if c == name and i != non_us]
            if hits:
                return 0x04 + hits[0], mods
    return int(name, 16), 0
SHOT_HDR = struct.Struct("<HHHHI")     # width, height, format, rotation, total
LOG, EVENT = 0x70, 0x71


class DevLinkError(Exception):
    pass


def find_port():
    if os.environ.get("PAPP_PORT"):
        return os.environ["PAPP_PORT"]
    for p in serial.tools.list_ports.comports():
        if p.vid == 0x303A:
            return p.device
    raise DevLinkError("no Espressif USB-Serial-JTAG port found; use --port")


class DevLink:
    """Frame-level client. A reader thread splits the byte stream into frames;
    responses go to a queue keyed by seq, LOG/EVENT frames to callbacks."""

    def __init__(self, port, on_log=None, on_event=None):
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = 115200
        self.ser.timeout = 0.05
        # Don't toggle DTR/RTS on open: on USB-Serial-JTAG RTS high with DTR low resets the chip.
        if os.name == "nt":
            self.ser.dtr = False
            self.ser.rts = False
            self.ser.open()
        else:
            # Linux/macOS raise both lines on open; pyserial would then drop DTR before RTS
            # (a reset). Keep both high through open, then drop RTS first.
            self.ser.open()
            self.ser.rts = False
            self.ser.dtr = False
        self.on_log = on_log or (lambda app, text: None)
        self.on_event = on_event or (lambda ev: None)
        self.seq = 0
        self.pending = {}
        self.cond = threading.Condition()
        self.alive = True
        self.reader = threading.Thread(target=self._read_loop, daemon=True)
        self.reader.start()

    def close(self):
        self.alive = False
        self.reader.join(timeout=1)
        self.ser.close()

    # ── RX ────────────────────────────────────────────────────────────────
    def _read_loop(self):
        buf = bytearray()
        while self.alive:
            try:
                data = self.ser.read(4096)
            except serial.SerialException:
                self.alive = False
                with self.cond:
                    self.cond.notify_all()
                return
            if data:
                buf += data
            while True:
                i = buf.find(SOF)
                if i < 0:
                    del buf[:-1]
                    break
                if i:
                    del buf[:i]
                if len(buf) < 2 + HDR.size:
                    break
                typ, flags, seq, length = HDR.unpack_from(buf, 2)
                if length > 64 * 1024:
                    del buf[:2]          # garbage header — resync
                    continue
                total = 2 + HDR.size + length + 4
                if len(buf) < total:
                    break
                body = bytes(buf[2:2 + HDR.size + length])
                (crc,) = struct.unpack_from("<I", buf, 2 + HDR.size + length)
                if zlib.crc32(body) & 0xFFFFFFFF != crc:
                    del buf[:2]          # bad frame: rescan from the next byte, don't trust length
                    continue
                del buf[:total]
                self._dispatch(typ, flags, seq, body[HDR.size:])

    def _dispatch(self, typ, flags, seq, payload):
        if typ == LOG:
            self.on_log(bool(flags & F_APP), payload.decode("utf-8", "replace"))
        elif typ == EVENT:
            try:
                self.on_event(json.loads(payload))
            except ValueError:
                pass
        else:
            with self.cond:
                self.pending[seq] = (typ, flags, payload)
                self.cond.notify_all()

    # ── TX ────────────────────────────────────────────────────────────────
    def request(self, typ, payload=b"", timeout=5.0):
        self.seq = (self.seq % 0xFFFF) + 1
        seq = self.seq
        body = HDR.pack(typ, 0, seq, len(payload)) + payload
        frame = SOF + body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)
        self.ser.write(frame)
        deadline = time.time() + timeout
        with self.cond:
            while seq not in self.pending:
                left = deadline - time.time()
                if left <= 0 or not self.alive:
                    raise DevLinkError(f"no response to command 0x{typ:02x} (is the launcher running?)")
                self.cond.wait(left)
            rtyp, flags, resp = self.pending.pop(seq)
        if flags & F_ERROR:
            raise DevLinkError(resp.decode("utf-8", "replace"))
        return resp

    def json(self, typ, payload=b"", timeout=5.0):
        return json.loads(self.request(typ, payload, timeout))

    # ── High level ────────────────────────────────────────────────────────
    def ping(self):
        return self.request(PING, timeout=2.0) == b"pong"

    def put(self, local, remote, progress=True):
        data = open(local, "rb").read()
        crc = zlib.crc32(data) & 0xFFFFFFFF
        self.request(PUT_BEGIN, struct.pack("<II", len(data), crc) + remote.encode() + b"\0")
        t0 = time.time()
        for off in range(0, len(data), CHUNK):
            self.request(PUT_DATA, data[off:off + CHUNK], timeout=10.0)
            if progress:
                pct = min(100, (off + CHUNK) * 100 // max(1, len(data)))
                print(f"\r  {remote}: {pct:3d}%", end="", file=sys.stderr, flush=True)
        self.request(PUT_END, timeout=10.0)
        dt = max(time.time() - t0, 1e-3)
        if progress:
            print(f"\r  {remote}: {len(data):,} bytes in {dt:.1f}s ({len(data) / dt / 1024:.0f} KB/s)",
                  file=sys.stderr)

    def shot(self):
        """Returns (landscape_width, landscape_height, rgb888 bytes) of the visible screen."""
        data = bytearray()
        total = None
        while total is None or len(data) < total:
            resp = self.request(SHOT, struct.pack("<II", len(data), CHUNK), timeout=10.0)
            w, h, fmt, rot, total = SHOT_HDR.unpack_from(resp)
            if fmt != 1:
                raise DevLinkError(f"unknown screenshot format {fmt}")
            chunk = resp[SHOT_HDR.size:]
            if not chunk and len(data) < total:
                raise DevLinkError("screenshot truncated")
            data += chunk
        px = rle16_decode(bytes(data), w * h)
        return unrotate(px, w, h, rot)

    def get(self, remote):
        out = bytearray()
        while True:
            resp = self.request(GET, struct.pack("<II", len(out), CHUNK) + remote.encode() + b"\0",
                                timeout=10.0)
            (size,) = struct.unpack_from("<I", resp)
            out += resp[4:]
            if len(resp) <= 4 or len(out) >= size:
                return bytes(out)


# ── Screenshot helpers ───────────────────────────────────────────────────

def rle16_decode(buf, count):
    import array
    out = array.array("H")
    i = 0
    while i < len(buf) and len(out) < count:
        (n,) = struct.unpack_from("<H", buf, i)
        i += 2
        if n & 0x8000:
            (v,) = struct.unpack_from("<H", buf, i)
            i += 2
            out.extend([v] * (n & 0x7FFF))
        else:
            out.frombytes(buf[i:i + 2 * n])
            i += 2 * n
    if len(out) != count:
        raise DevLinkError(f"screenshot decode: {len(out)} pixels, expected {count}")
    return out


def unrotate(px, pw, ph, rot):
    """Panel-native pixels -> logical landscape RGB888 (same mapping as lx_gfx/bsp.h)."""
    w, h = (ph, pw) if rot in (90, 270) else (pw, ph)
    rgb = bytearray(w * h * 3)
    o = 0
    for y in range(h):
        for x in range(w):
            if rot == 270:
                v = px[x * pw + (h - 1 - y)]
            elif rot == 90:
                v = px[(w - 1 - x) * pw + y]
            else:
                v = px[y * pw + x]
            r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
            rgb[o] = (r << 3) | (r >> 2)
            rgb[o + 1] = (g << 2) | (g >> 4)
            rgb[o + 2] = (b << 3) | (b >> 2)
            o += 3
    return w, h, bytes(rgb)


def write_png(path, w, h, rgb):
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


# ── CLI ──────────────────────────────────────────────────────────────────

def print_log(app, text):
    prefix = "[app] " if app else ""
    sys.stdout.write(prefix + text if text.endswith("\n") else prefix + text + "\n")
    sys.stdout.flush()


def keepalive(dl, stop_evt):
    """Logs are only forwarded while the host pings regularly."""
    while not stop_evt.wait(2.0):
        try:
            dl.request(PING, timeout=2.0)
        except DevLinkError:
            pass


def symbolize(elf, offsets):
    """addr2line over image offsets (apps are linked at 0, so offset == link address)."""
    import glob
    import shutil
    import subprocess
    tool = shutil.which("riscv32-esp-elf-addr2line") or (sorted(glob.glob(os.path.join(
        os.path.expanduser("~"), ".espressif", "tools", "riscv32-esp-elf", "*", "riscv32-esp-elf",
        "bin", "riscv32-esp-elf-addr2line*"))) or [None])[-1]
    if not tool:
        return ["?"] * len(offsets)
    out = subprocess.run([tool, "-f", "-p", "-C", "-e", elf] + [hex(o) for o in offsets],
                         capture_output=True, text=True).stdout.strip().splitlines()
    return out + ["?"] * (len(offsets) - len(out))


def print_crash(r, elf=None):
    names = {1: "power-on", 3: "software reset", 4: "panic", 5: "interrupt WDT", 6: "task WDT", 7: "WDT"}
    print(f"last reset: {names.get(r['reset_reason'], r['reset_reason'])}")
    app, cd = r.get("app"), r.get("coredump")
    if not app and not cd:
        print("no crash recorded")
        return
    if app:
        print(f"app running at crash: {app['path']} (exec base {app['exec_base']:#010x}, {app['size']:,} B)")
    if not cd:
        return
    causes = {0: "instruction misaligned", 1: "instruction access fault", 2: "illegal instruction",
              3: "breakpoint", 4: "load misaligned", 5: "load access fault", 6: "store misaligned",
              7: "store access fault"}
    print(f"task {cd['task']}: {causes.get(cd['mcause'], f'mcause {cd['mcause']}')}, "
          f"mtval={cd['mtval']:#010x}")
    base = app["exec_base"] if app else 0
    size = app["size"] if app else 0
    fbase, fsize = (app.get("fast_base", 0), app.get("fast_size", 0)) if app else (0, 0)
    in_fast = lambda a: fsize and fbase <= a < fbase + fsize
    in_app = lambda a: app and (base <= a < base + size or in_fast(a))
    link = lambda a: a - fbase + 0x10000000 if in_fast(a) else a - base     # ELF address
    frames = [("pc", cd["pc"]), ("ra", cd["ra"])]
    frames += [(f"stack[{i}]", w) for i, w in enumerate(cd["stack"]) if in_app(w)]
    syms = symbolize(elf, [link(a) for _, a in frames]) if elf and app else None
    for i, (label, a) in enumerate(frames):
        where = (f"FAST+{a - fbase:#x}" if in_fast(a) else f"app+{a - base:#x}") if in_app(a) else "launcher/ROM"
        sym = f"  {syms[i]}" if syms and in_app(a) else ""
        print(f"  {label:>10} {a:#010x}  {where}{sym}")


def send_input(dl, what, value, player=0, hold=100):
    """what: pad (A+START), key (ENTER, a, 0x28) or text (string; "\\n" = Enter)."""
    if what == "pad":
        dl.request(INPUT, struct.pack("<BBII", INPUT_PAD, player, buttons_mask(value), hold))
    elif what == "key":
        usage, mods = key_usage(value)
        dl.request(INPUT, struct.pack("<BHHI", INPUT_KEY, usage, mods, hold))
    elif what == "text":
        for ch in value.replace("\\n", "\r"):
            usage, mods = key_usage(ch)
            dl.request(INPUT, struct.pack("<BHHI", INPUT_KEY, usage, mods, 30))
            time.sleep(0.06)
    else:
        raise DevLinkError(f"unknown input kind {what}")


def run_injections(dl, specs, done):
    """specs: "MS:pad:A+B", "MS:key:ENTER", "MS:text:hello" — MS after the app starts."""
    t0 = time.time()
    for spec in specs:
        ms, what, value = spec.split(":", 2)
        delay = t0 + int(ms) / 1000 - time.time()
        if delay > 0 and done.wait(delay):
            return
        try:
            send_input(dl, what, value)
            print(f"[inject] {what} {value}", file=sys.stderr)
        except DevLinkError as e:
            print(f"[inject] {spec}: {e}", file=sys.stderr)


def app_remote_path(dl, name, suffix=".papp"):
    """Where the app belongs: preserve its container suffix and any home folder."""
    def subdirs(path):
        return [e["n"] for e in json.loads(dl.request(LS, path.encode()).decode().split("\n#")[0]) if e["d"]]
    top = subdirs("/sd/apps")
    if name not in top:
        for folder in top:
            if name in subdirs(f"/sd/apps/{folder}"):
                return f"/sd/apps/{folder}/{name}/{name}{suffix}"
    return f"/sd/apps/{name}/{name}{suffix}"


FOLDER_ICONS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "apps", "common", "icons")


def put_folder_icon(dl, remote):
    """An app deployed into a home screen folder (/sd/apps/<folder>/<dir>/...): give the folder
    its icon (apps/common/icons/folder_<folder>.png) if it has none, as the starter card does"""
    parts = remote.split("/")                      # "", "sd", "apps", folder, dir, file
    if len(parts) != 6 or parts[:3] != ["", "sd", "apps"]:
        return
    folder = parts[3]
    icon = os.path.join(FOLDER_ICONS, f"folder_{folder.lower().replace(' ', '_')}.png")
    if not os.path.isfile(icon):
        return
    entries = json.loads(dl.request(LS, f"/sd/apps/{folder}".encode()).decode().split("\n#")[0])
    if not any(e["n"] == "icon.png" and not e["d"] for e in entries):
        dl.put(icon, f"/sd/apps/{folder}/icon.png")
        print(f"folder icon -> /sd/apps/{folder}/icon.png", file=sys.stderr)


def cmd_deploy(dl, args, exit_box):
    name = os.path.splitext(os.path.basename(args.file))[0]
    suffix = os.path.splitext(args.file)[1].lower()
    if suffix not in (".papp", ".pax"):
        raise DevLinkError("deploy expects a .papp or .pax PAPP2 container")
    remote = args.dest or app_remote_path(dl, name, suffix)
    status = dl.json(STATUS)
    if status["state"] != "idle":
        print(f"stopping running app {status['app']}", file=sys.stderr)
        dl.request(STOP, struct.pack("<I", 1000))
        time.sleep(1.5)
    dl.put(args.file, remote)
    icon = os.path.join(os.path.dirname(os.path.abspath(args.file)), "icon.png")
    if os.path.isfile(icon):                       # home screen icon (papp_build copies it here)
        dl.put(icon, remote.rsplit("/", 1)[0] + "/icon.png")
    put_folder_icon(dl, remote)

    done = threading.Event()

    def on_event(ev):
        if ev.get("ev") == "app_exit":
            exit_box["ev"] = ev
            done.set()
    dl.on_event = on_event

    payload = remote.encode() + b"\0" + b"".join(a.encode() + b"\0" for a in args.args)
    dl.request(RUN, payload, timeout=15.0)
    print(f"running {remote}", file=sys.stderr)

    stop_evt = threading.Event()
    threading.Thread(target=keepalive, args=(dl, stop_evt), daemon=True).start()
    if args.inject:
        threading.Thread(target=run_injections, args=(dl, args.inject, done), daemon=True).start()
    if not done.wait(args.timeout):
        print(f"timeout after {args.timeout}s - stopping app", file=sys.stderr)
        try:
            dl.request(STOP, struct.pack("<I", 1000))
        except DevLinkError as e:
            print(f"stop failed: {e}", file=sys.stderr)
        done.wait(5)
        stop_evt.set()
        return 124
    stop_evt.set()
    ev = exit_box["ev"]
    print(f"app exited: code={ev['code']} forced={ev['forced']} "
          f"leaked_allocs={ev['leaked_allocs']} leaked_files={ev['leaked_files']}", file=sys.stderr)
    code = ev["code"]
    return code if 0 <= code < 124 else 1


def main():
    # App logs may hold any character; a piped Windows console is cp1252 and would raise
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description="DevLink host tool")
    ap.add_argument("--port")
    ap.add_argument("--no-time", action="store_true", help="don't set the board's clock from this PC")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info")
    p = sub.add_parser("net", help="network self-test on the board"); p.add_argument("--quick", action="store_true", help="link and gateway ping only"); p.add_argument("--scan", action="store_true", help="also list networks")
    sub.add_parser("ping")
    p = sub.add_parser("ls"); p.add_argument("path", nargs="?", default="/sd")
    p = sub.add_parser("stat"); p.add_argument("path")
    p = sub.add_parser("mkdir"); p.add_argument("path")
    p = sub.add_parser("rm"); p.add_argument("path")
    p = sub.add_parser("mv"); p.add_argument("src"); p.add_argument("dst")
    p = sub.add_parser("put"); p.add_argument("local"); p.add_argument("remote")
    p = sub.add_parser("get"); p.add_argument("remote"); p.add_argument("local")
    p = sub.add_parser("run"); p.add_argument("path"); p.add_argument("args", nargs="*")
    p = sub.add_parser("stop"); p.add_argument("--grace", type=int, default=2000)
    sub.add_parser("status")
    p = sub.add_parser("crash"); p.add_argument("--elf", help="app ELF to symbolize the crash with")
    sub.add_parser("crash-clear")
    sub.add_parser("bootlog", help="messages logged while the board was starting")
    sub.add_parser("reboot")
    p = sub.add_parser("shot"); p.add_argument("out", nargs="?", default="screenshot.png")
    p = sub.add_parser("log"); p.add_argument("--seconds", type=float, default=0)
    p = sub.add_parser("input", help="inject input or read the input state")
    p.add_argument("what", choices=["state", "pad", "key", "text", "unmap"])
    p.add_argument("value", nargs="?", help="pad: A+START ...; key: ENTER, a, 0x28; text: string")
    p.add_argument("--player", type=int, default=0)
    p.add_argument("--hold", type=int, default=100, help="ms")
    p.add_argument("--watch", type=float, default=0, help="state: print changes for N seconds")
    p = sub.add_parser("deploy")
    p.add_argument("file"); p.add_argument("--dest"); p.add_argument("--timeout", type=float, default=30)
    p.add_argument("--inject", action="append", metavar="MS:KIND:VALUE",
                   help="send input MS ms after start: 500:pad:A+START, 800:key:ENTER, 900:text:hi (repeatable)")
    p.add_argument("args", nargs="*")
    args = ap.parse_args()

    try:
        dl = DevLink(args.port or find_port(), on_log=print_log,
                     on_event=lambda ev: print(f"EVENT {json.dumps(ev)}", file=sys.stderr))
    except (DevLinkError, serial.SerialException) as e:
        print(f"error: {e}", file=sys.stderr)
        return 125
    try:
        # The board may have no clock source: give it ours (local wall-clock time)
        now = time.time()
        if not args.no_time:
            dl.request(TIME, struct.pack("<qq", int(now + time.localtime(now).tm_gmtoff), int(now)), timeout=2.0)
    except DevLinkError:
        pass                    # older launcher

    try:
        c = args.cmd
        if c == "info":
            print(json.dumps(dl.json(INFO), indent=2))
        elif c == "net":
            r = dl.request(NET, bytes([3 if args.scan else 2 if args.quick else 1])).decode("utf-8", "replace")
            while r[:1] == "1":
                time.sleep(1)
                r = dl.request(NET).decode("utf-8", "replace")
            print(r[1:].strip(chr(0)))
        elif c == "ping":
            print("pong" if dl.ping() else "?")
        elif c == "ls":
            raw = dl.request(LS, args.path.encode()).decode()
            truncated = raw.endswith("#truncated")
            for e in json.loads(raw.split("\n#")[0]):
                print(f"{'<dir>' if e['d'] else e['s']:>12}  {e['n']}{'/' if e['d'] else ''}")
            if truncated:
                print("(listing truncated)")
        elif c == "stat":
            print(dl.request(STAT, args.path.encode()).decode())
        elif c == "mkdir":
            dl.request(MKDIR, args.path.encode())
        elif c == "rm":
            dl.request(RM, args.path.encode())
        elif c == "mv":
            dl.request(RENAME, args.src.encode() + b"\0" + args.dst.encode())
        elif c == "put":
            dl.put(args.local, args.remote)
        elif c == "get":
            data = dl.get(args.remote)
            open(args.local, "wb").write(data)
            print(f"{len(data):,} bytes → {args.local}", file=sys.stderr)
        elif c == "run":
            payload = args.path.encode() + b"\0" + b"".join(a.encode() + b"\0" for a in args.args)
            print(dl.request(RUN, payload, timeout=15.0).decode())
        elif c == "stop":
            print(dl.request(STOP, struct.pack("<I", args.grace)).decode())
        elif c == "status":
            print(json.dumps(dl.json(STATUS), indent=2))
        elif c == "crash":
            report = dl.json(CRASH)
            print_crash(report, args.elf)
        elif c == "crash-clear":
            dl.request(CRASH_CLEAR)
        elif c == "shot":
            t0 = time.time()
            w, h, rgb = dl.shot()
            write_png(args.out, w, h, rgb)
            print(f"{w}x{h} -> {args.out} ({time.time() - t0:.1f}s)", file=sys.stderr)
        elif c == "input":
            if args.what == "state":
                last, end = None, time.time() + args.watch
                while True:
                    st = dl.json(INPUT, bytes([INPUT_STATE]))
                    line = f"devices: {st['devices'] or '-'}"
                    for i, pl in enumerate(st["players"]):
                        if pl["connected"]:
                            line += (f" | P{i + 1} {pl['name']}: {buttons_names(pl['buttons'])} "
                                     f"L({pl['lx']},{pl['ly']}) R({pl['rx']},{pl['ry']}) T({pl['lt']},{pl['rt']})")
                    if st.get("paddle", -1) >= 0:
                        line += f" | paddle {st['paddle'] >> 10}"    # 0..63: hides ADC noise
                    if line != last:
                        print(line, flush=True)
                        last = line
                    if time.time() >= end:
                        break
                    time.sleep(0.05)
            elif args.what == "unmap":                  # forget the pad's saved button mapping
                dl.request(INPUT, bytes([INPUT_UNMAP, args.player]))
                print(f"player {args.player + 1}: default buttons")
            else:
                send_input(dl, args.what, args.value, args.player, args.hold)
        elif c == "bootlog":
            print(dl.request(BOOTLOG).decode("utf-8", "replace"))
        elif c == "reboot":
            print(dl.request(REBOOT).decode())
        elif c == "log":
            stop_evt = threading.Event()
            dl.request(PING)
            threading.Thread(target=keepalive, args=(dl, stop_evt), daemon=True).start()
            try:
                if args.seconds:
                    time.sleep(args.seconds)
                else:
                    while dl.alive:
                        time.sleep(0.5)
            except KeyboardInterrupt:
                pass
            stop_evt.set()
        elif c == "deploy":
            return cmd_deploy(dl, args, {})
        return 0
    except DevLinkError as e:
        print(f"error: {e}", file=sys.stderr)
        return 125
    finally:
        dl.close()


if __name__ == "__main__":
    sys.exit(main())
