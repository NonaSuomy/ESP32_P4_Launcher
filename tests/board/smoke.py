#!/usr/bin/env python3
"""
Quick board regression: run the test apps through DevLink and fail on a bad exit
code or a stored crash. Doesn't change the home screen, apart from the test apps
it deploys. Run after a launcher flash, with the board idle on COM9:

    python tests/board/smoke.py [--port COM9]
"""
import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TESTS = [
    ("hello", ["arg1"], 20),
    ("libc_test", [], 30),
    ("hid_test", [], 30),
    ("thread_test", ["all"], 40),
    ("irq_test", [], 60),              # toggles the lowest free header pin: nothing may be wired to it
]


def dev(port, *args, timeout=60):
    cmd = [sys.executable, os.path.join(ROOT, "tools", "papp_dev.py"), "--port", port, *args]
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, cwd=ROOT)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM9")
    args = ap.parse_args()
    failed = 0

    reloc = subprocess.run([sys.executable, os.path.join(ROOT, "tests", "host", "test_reloc.py")],
                           capture_output=True, text=True)
    print(f"{'ok  ' if reloc.returncode == 0 else 'FAIL'} host relocation test")
    failed += reloc.returncode != 0

    for name, app_args, limit in TESTS:
        papp = os.path.join(ROOT, "build", "apps", name, f"{name}.papp")
        if not os.path.isfile(papp):
            print(f"skip {name}: not built")
            continue
        r = dev(args.port, "deploy", papp, "--timeout", str(limit), "--", *app_args, timeout=limit + 30)
        print(f"{'ok  ' if r.returncode == 0 else 'FAIL'} {name} (exit {r.returncode})")
        failed += r.returncode != 0

    crash = dev(args.port, "crash").stdout
    clean = "no crash recorded" in crash
    print(f"{'ok  ' if clean else 'FAIL'} crash record")
    failed += not clean
    print("PASSED" if not failed else f"{failed} FAILED")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
