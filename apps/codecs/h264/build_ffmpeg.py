#!/usr/bin/env python3
"""Build FFmpeg 7.1.1's H.264 decoder (libavcodec + libavutil) for the ESP32-P4 as libffh264.a.

    python apps/codecs/h264/build_ffmpeg.py                 # compile ffmpeg_files.txt -> libffh264.a
    python apps/codecs/h264/build_ffmpeg.py -O3 --out build/codecs/h264/ffmpeg_obj_o3
    python apps/codecs/h264/build_ffmpeg.py --linktest      # also link linktest.c, list undefined symbols
    python apps/codecs/h264/build_ffmpeg.py --configure     # re-run FFmpeg's configure (needs bash),
                                                            # refresh ffmpeg_config/ and ffmpeg_files.txt

FFmpeg root: --ffmpeg, default <main checkout>/third_party_dl/ffmpeg-7.1.1 (searched upward from here).
No make needed: --configure evaluates the OBJS lines of FFmpeg's Makefiles against ffbuild/config.mak.
"""
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))       # repo (worktree) root
CONFIG_DIR = os.path.join(HERE, "ffmpeg_config")
PATCH_DIR = os.path.join(HERE, "patches")
FILE_LIST = os.path.join(HERE, "ffmpeg_files.txt")
FFMPEG_DIR_NAME = "ffmpeg-7.1.1"

ARCH = ["-march=rv32imafc_zicsr_zifencei", "-mabi=ilp32f"]
# FFmpeg's own CPPFLAGS/CFLAGS from ffbuild/config.mak (warnings trimmed), plus the app flags.
CPPFLAGS = ["-D_ISOC11_SOURCE", "-D_FILE_OFFSET_BITS=64", "-D_LARGEFILE_SOURCE", "-U__STRICT_ANSI__",
            "-D_XOPEN_SOURCE=600", "-DHAVE_AV_CONFIG_H"]
CFLAGS = ARCH + ["-mcmodel=medany", "-ffunction-sections", "-fdata-sections", "-fno-common",
                 "-std=c17", "-fomit-frame-pointer", "-fno-math-errno", "-fno-signed-zeros",
                 "-fno-tree-vectorize", "-g",
                 "-Wall", "-Wno-parentheses", "-Wno-switch", "-Wno-format-zero-length",
                 "-Wno-pointer-sign", "-Wno-unused-const-variable", "-Wno-bool-operation",
                 "-Wno-char-subscripts", "-Wno-maybe-uninitialized",
                 "-Werror=implicit-function-declaration", "-Werror=return-type", "-Werror=vla"]

# configure line (see --configure). --target-os=none: newlib, no OS. --disable-asm: no RVV on the P4.
CONFIGURE_ARGS = [
    "--enable-cross-compile", "--cross-prefix=riscv32-esp-elf-", "--arch=riscv", "--cpu=generic",
    "--target-os=none",
    # no host compiler on this PC: the cross gcc stands in (only used for checks, nothing host-built)
    "--host-cc=riscv32-esp-elf-gcc", "--host-ld=riscv32-esp-elf-gcc",
    "--host-cflags=" + " ".join(ARCH), "--host-ldflags=" + " ".join(ARCH) + " -nostartfiles",
    "--disable-everything", "--enable-decoder=h264", "--enable-parser=h264",
    "--disable-programs", "--disable-doc", "--disable-avdevice", "--disable-avformat",
    "--disable-swscale", "--disable-swresample", "--disable-postproc", "--disable-avfilter",
    "--disable-network", "--disable-iconv", "--disable-zlib", "--disable-bzlib", "--disable-lzma",
    "--disable-autodetect", "--enable-pthreads", "--disable-runtime-cpudetect", "--disable-asm",
    "--disable-debug", "--enable-static", "--disable-shared",
    "--extra-cflags=" + " ".join(ARCH) + " -mcmodel=medany",
    # without ARCH here the link checks pick the soft-float multilib and every libm check fails
    "--extra-ldflags=" + " ".join(ARCH) + " -nostartfiles",
    "--extra-libs=-lm",
]
# config.h edits after configure (configure's link checks see newlib's stubs / miss our shim).
CONFIG_H_EDITS = {
    "HAVE_PTHREAD_CANCEL": 0,   # newlib stub only; FFmpeg's libs don't use it
    "HAVE_SYSCONF": 1,          # shim: sysconf(_SC_NPROCESSORS_ONLN) = 2 -> av_cpu_count() (threads=0 = auto)
    "HAVE_ISATTY": 0,           # av_log's default callback: no isatty(2)
}
# Sources FFmpeg 7.1.1's Makefiles miss for an h264-only build: h2645_sei.c's ff_h2645_sei_reset()
# calls ff_aom_uninit_film_grain_params() unconditionally, but aom_film_grain.o is only listed
# under CONFIG_HEVC_SEI (upstream bug; an h264-only static build has the same undefined symbol).
EXTRA_SOURCES = ["libavcodec/aom_film_grain.c"]
GENERATED = ["config.h", "config_components.h", "libavutil/avconfig.h",
             "libavcodec/codec_list.c", "libavcodec/parser_list.c", "libavcodec/bsf_list.c"]


def find_tool(name):
    exe = shutil.which(name)
    if exe:
        return exe
    pattern = os.path.join(os.path.expanduser("~"), ".espressif", "tools", "riscv32-esp-elf", "*",
                           "riscv32-esp-elf", "bin", name + (".exe" if os.name == "nt" else ""))
    hits = sorted(glob.glob(pattern))
    if not hits:
        sys.exit(f"build_ffmpeg: {name} not found (install ESP-IDF tools or add the toolchain to PATH)")
    return hits[-1]


def find_ffmpeg():
    d = HERE
    while True:
        cand = os.path.join(d, "third_party_dl", FFMPEG_DIR_NAME)
        if os.path.isdir(cand):
            return cand
        parent = os.path.dirname(d)
        if parent == d:
            sys.exit(f"build_ffmpeg: third_party_dl/{FFMPEG_DIR_NAME} not found above {HERE}; use --ffmpeg")
        d = parent


# ---------------------------------------------------------------- configure + file list

def read_config_mak(path):
    on = set()
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"^(!?[A-Za-z0-9_]+)=yes\s*$", line)
        if m:
            on.add(m.group(1))
    return on


def makefile_objs(path, on, subdir, objs):
    """Minimal evaluation of FFmpeg's 'OBJS[-$(X)] [+]= a.o dir/b.o' lines and includes."""
    text = open(path, encoding="utf-8").read().replace("\\\n", " ")
    for line in text.splitlines():
        m = re.match(r"^-?include\s+\$\(SRC_PATH\)/(\S+)", line)
        if m:
            inc = m.group(1)
            if "$(ARCH)" in inc:
                continue                         # ARCH=c: no arch subdir
            makefile_objs(os.path.join(SRC_ROOT, inc), on, subdir, objs)
            continue
        m = re.match(r"^OBJS(?:-\$\((!?\w+)\))?\s*\+?=\s*(.*)$", line)
        if not m:
            continue
        cond, rest = m.group(1), m.group(2)
        if cond and cond not in on:
            continue
        for tok in rest.split():
            if tok.endswith(".o") and "$" not in tok:
                objs.append(subdir + "/" + tok[:-2])


def gen_file_list(build_dir, src):
    global SRC_ROOT
    SRC_ROOT = src
    on = read_config_mak(os.path.join(build_dir, "ffbuild", "config.mak"))
    files = []
    for lib in ("libavutil", "libavcodec"):
        objs = []
        makefile_objs(os.path.join(src, lib, "Makefile"), on, lib, objs)
        for o in dict.fromkeys(objs):            # keep order, drop duplicates
            c = os.path.join(src, o + ".c")
            if not os.path.isfile(c):
                sys.exit(f"build_ffmpeg: {o}.c listed by the Makefile but missing")
            files.append(o + ".c")
    files += [e for e in EXTRA_SOURCES if e not in files]
    with open(FILE_LIST, "w", newline="\n") as f:
        f.write("# FFmpeg sources for the H.264 decoder, relative to the FFmpeg root.\n"
                "# Generated by build_ffmpeg.py --configure from libavutil/libavcodec Makefiles + config.mak.\n")
        f.write("\n".join(files) + "\n")
    print(f"build_ffmpeg: {len(files)} sources -> {FILE_LIST}")


def find_bash():
    for cand in (shutil.which("bash"), r"C:\Program Files\Git\bin\bash.exe"):
        if cand and os.path.isfile(cand) and "System32" not in cand:
            return cand
    sys.exit("build_ffmpeg: bash not found (Git Bash) for configure")


def configure(src, build_dir):
    os.makedirs(os.path.join(build_dir, "tmp"), exist_ok=True)
    env = dict(os.environ)
    tmp = os.path.join(build_dir, "tmp")
    env.update(TMPDIR=tmp, TMP=tmp, TEMP=tmp)    # configure can't use the Windows %TEMP% path
    env["PATH"] = os.path.dirname(find_tool("riscv32-esp-elf-gcc")) + os.pathsep + env["PATH"]
    def posix(p):
        p = os.path.abspath(p).replace("\\", "/")
        return "/" + p[0].lower() + p[2:] if re.match(r"^[A-Za-z]:", p) else p
    args = " ".join("'" + a + "'" for a in CONFIGURE_ARGS)
    cmd = f"cd '{posix(build_dir)}' && '{posix(src)}/configure' {args}"
    print("build_ffmpeg: running configure in", build_dir)
    r = subprocess.run([find_bash(), "-c", cmd], env=env)
    if r.returncode:
        sys.exit("build_ffmpeg: configure failed (see ffbuild/config.log)")
    for g in GENERATED:
        dst = os.path.join(CONFIG_DIR, g)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        data = open(os.path.join(build_dir, g), encoding="utf-8").read()
        if g == "config.h":
            for k, v in CONFIG_H_EDITS.items():
                data, n = re.subn(rf"^#define {k} \d+$", f"#define {k} {v}", data, flags=re.M)
                if n != 1:
                    sys.exit(f"build_ffmpeg: {k} not found in config.h")
            # absolute build paths out of the copied config.h
            data = re.sub(r'^#define FFMPEG_CONFIGURATION .*$', '#define FFMPEG_CONFIGURATION "h264 decoder, ESP32-P4"',
                          data, flags=re.M)
        with open(dst, "w", newline="\n") as f:
            f.write(data)
    version = open(os.path.join(src, "RELEASE")).read().strip()
    with open(os.path.join(CONFIG_DIR, "libavutil", "ffversion.h"), "w", newline="\n") as f:
        f.write("/* Automatically generated by version.sh, do not manually edit! */\n"
                "#ifndef AVUTIL_FFVERSION_H\n#define AVUTIL_FFVERSION_H\n"
                f'#define FFMPEG_VERSION "{version}"\n#endif /* AVUTIL_FFVERSION_H */\n')
    gen_file_list(build_dir, src)


# ---------------------------------------------------------------- compile + archive

def compile_all(src, out_dir, opt, jobs):
    gcc = find_tool("riscv32-esp-elf-gcc")
    files = [l.strip() for l in open(FILE_LIST) if l.strip() and not l.startswith("#")]
    os.makedirs(out_dir, exist_ok=True)
    incs = ["-I" + CONFIG_DIR, "-I" + src, "-I" + os.path.join(src, "compat", "stdbit")]

    def one(rel):
        patched = os.path.join(PATCH_DIR, rel)
        source = patched if os.path.isfile(patched) else os.path.join(src, rel)
        lib = rel.split("/")[0]                   # libavcodec / libavutil
        obj = os.path.join(out_dir, rel[:-2].replace("/", "__") + ".o")
        if os.path.isfile(obj) and os.path.getmtime(obj) >= max(os.path.getmtime(source), OPT_STAMP):
            return obj, None
        # -iquote <dir of the original file> (not -I: libavutil/time.h would hide <time.h>) so a patched copy still finds its neighbours' "x.h"
        cmd = [gcc] + CPPFLAGS + ["-DBUILDING_" + lib[3:]] + CFLAGS + [opt] + incs + \
              ["-iquote", os.path.dirname(os.path.join(src, rel)), "-c", source, "-o", obj]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            return None, f"{rel}:\n{r.stderr}"
        return obj, (r.stderr if r.stderr.strip() else None)

    stamp = os.path.join(out_dir, "opt.txt")
    flags_id = opt + " " + " ".join(CFLAGS)
    global OPT_STAMP
    if not os.path.isfile(stamp) or open(stamp).read() != flags_id:
        with open(stamp, "w") as f:
            f.write(flags_id)
    OPT_STAMP = os.path.getmtime(stamp)

    with ThreadPoolExecutor(jobs) as ex:
        results = list(ex.map(one, files))
    errors = [e for o, e in results if o is None]
    warns = [e for o, e in results if o is not None and e]
    if warns:
        with open(os.path.join(out_dir, "warnings.txt"), "w") as f:
            f.write("\n".join(warns))
        print(f"build_ffmpeg: {len(warns)} files with warnings (warnings.txt)")
    if errors:
        print("\n".join(errors))
        sys.exit(f"build_ffmpeg: {len(errors)} files failed")
    objs = [o for o, e in results]
    lib = os.path.join(out_dir, "libffh264.a")
    if os.path.exists(lib):
        os.remove(lib)
    rsp = os.path.join(out_dir, "objs.rsp")
    with open(rsp, "w") as f:
        f.write("\n".join('"' + o.replace("\\", "/") + '"' for o in objs))
    r = subprocess.run([find_tool("riscv32-esp-elf-ar"), "rcs", lib, "@" + rsp])
    if r.returncode:
        sys.exit("build_ffmpeg: ar failed")
    print(f"build_ffmpeg: {len(objs)} objects -> {lib}")
    return lib


def linktest(src, out_dir, lib, opt):
    """Link linktest.c against libffh264.a; report what FFmpeg needs from outside."""
    gcc = find_tool("riscv32-esp-elf-gcc")
    elf = os.path.join(out_dir, "linktest.elf")
    incs = ["-I" + CONFIG_DIR, "-I" + src]
    base = [gcc] + ARCH + ["-mcmodel=medany", opt, "-ffunction-sections", "-fdata-sections", "-fno-common"] +         incs + [os.path.join(HERE, "linktest.c"), "-nostartfiles", "-Wl,--gc-sections", "-Wl,-e,main"]
    # 1) FFmpeg alone (-nostdlib, libgcc only): every symbol it needs from libc/libm/the shim, with a
    #    cross reference (who uses it) in linktest_nolibc.map
    nolibc = elf[:-4] + "_nolibc.elf"
    r = subprocess.run(base + ["-nostdlib", lib, "-lgcc", "-Wl,--unresolved-symbols=ignore-all",
                               "-Wl,--cref", "-Wl,-Map=" + nolibc[:-4] + ".map", "-o", nolibc],
                       capture_output=True, text=True)
    if r.returncode:
        print(r.stderr)
    und = subprocess.run([find_tool("riscv32-esp-elf-nm"), "-u", nolibc], capture_output=True, text=True).stdout.split()
    und = sorted(s for s in und if s != "U")
    print(f"== needed from libc/libm/shim ({len(und)}):\n   " + " ".join(und))
    # 2) with newlib: real undefined references, and the newlib stubs ("not implemented") that got pulled in
    r = subprocess.run(base + [lib, "-lc", "-lm", "-lgcc", "-Wl,-Map=" + elf[:-4] + ".map", "-o", elf],
                       capture_output=True, text=True)
    missing = sorted(set(re.findall(r"undefined reference to `([^']+)'", r.stderr)))
    stubs = sorted(set(re.findall(r"warning: (\S+) is not implemented", r.stderr)))
    print(f"== undefined with -lc -lm -lgcc ({len(missing)}): " + " ".join(missing))
    print(f"== newlib 'not implemented' stubs linked ({len(stubs)}): " + " ".join(stubs))
    if r.returncode and not missing:
        print(r.stderr)
    # the size: link once more with the missing ones ignored
    if missing:
        subprocess.run(base + [lib, "-lc", "-lm", "-lgcc", "-Wl,--unresolved-symbols=ignore-all", "-o", elf],
                       capture_output=True)
    sys.stdout.flush()
    subprocess.run([find_tool("riscv32-esp-elf-size"), elf, nolibc])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ffmpeg", help="FFmpeg source root")
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "codecs", "h264", "ffmpeg_obj"))
    ap.add_argument("-O", dest="opt", default="2", help="optimisation level (2, 3, s)")
    ap.add_argument("-j", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--configure", action="store_true", help="re-run configure first")
    ap.add_argument("--build-dir", default=None, help="configure's scratch dir (default <ffmpeg>/../ffbuild)")
    ap.add_argument("--linktest", action="store_true")
    a = ap.parse_args()
    src = os.path.abspath(a.ffmpeg or find_ffmpeg())
    out = os.path.abspath(a.out)
    if a.configure:
        configure(src, os.path.abspath(a.build_dir or os.path.join(os.path.dirname(src), "ffbuild")))
    lib = compile_all(src, out, "-O" + a.opt, a.j)
    if a.linktest:
        linktest(src, out, lib, "-O" + a.opt)


OPT_STAMP = 0
SRC_ROOT = ""
if __name__ == "__main__":
    main()
