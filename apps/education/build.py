#!/usr/bin/env python3
"""Builds every education app (each folder here with a build.py) into SDcard/apps/Education:
    python apps/education/build.py [name ...]
apps/build_all.py runs it too. Exit code 1 if any failed."""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
only = set(sys.argv[1:])
apps = sorted(d for d in os.listdir(HERE)
              if os.path.isfile(os.path.join(HERE, d, "build.py")) and (not only or d in only))
failed = []
for d in apps:
    r = subprocess.run([sys.executable, os.path.join(HERE, d, "build.py")], capture_output=True, text=True)
    print(r.stdout.rstrip())
    if r.returncode:
        failed.append(d)
        print(r.stderr.rstrip()[-2000:])
if failed:
    print("failed: " + " ".join(failed))
sys.exit(1 if failed else 0)
