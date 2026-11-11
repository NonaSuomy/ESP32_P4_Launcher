#!/usr/bin/env python3
"""Game Boy Advance app: gpSP (midworld's fork, RV32 dynamic recompiler) + gba_run.c, a small
libretro frontend on the odroid layer.
    python apps/gba/build.py  -> build/apps/gba/gba.papp
core/ = https://github.com/midworld/gpsp at 3f112ae (GPL-2.0-or-later), without the other CPUs' backends."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "common", "odroid"))
from emubuild import build_emu  # noqa: E402

CORE = os.path.join(HERE, "core")
LRC = "libretro/libretro-common/"
SRCS = ["../gba_run.c", "bios_data.S", "video.cc", "cpu.cc",
        "main.c", "gba_memory.c", "gba_rom_scan.c", "savestate.c", "input.c", "sound.c", "cheats.c", "memmap.c",
        "serial.c", "gbp.c", "rfu.c", "serial_proto.c", "libretro/libretro.c", "gba_cc_lut.c",
        LRC + "compat/compat_posix_string.c", LRC + "compat/compat_strl.c", LRC + "compat/fopen_utf8.c",
        LRC + "encodings/encoding_utf.c", LRC + "file/file_path.c", LRC + "file/file_path_io.c",
        LRC + "streams/file_stream.c", LRC + "string/stdstring.c", LRC + "time/rtime.c",
        LRC + "vfs/vfs_implementation.c",
        # RV32 dynamic recompiler
        "cpu_threaded.c"] + ["riscv/" + f for f in (
            "rv32_arm_alu.c", "rv32_arm_branch.c", "rv32_arm_mul.c", "rv32_codegen.c", "rv32_fallback.c",
            "rv32_platform.c", "rv32_thumb_alu.c", "rv32_stub.S", "rv32_arm_memory.c", "rv32_arm_system.c",
            "rv32_block_emit.c", "rv32_block_link.c", "rv32_ram_units.c", "rv32_thumb_memory.c",
            "rv32_flow_timing.c", "rv32_arm_flow_alu.c", "rv32_arm_flow_full.c", "rv32_thumb_flow_full.c",
            "rv32_block_runtime.cc", "rv32_arm_io.S", "rv32_arm_palette.S", "rv32_interpreter_stub.S",
            "rv32_rom_load.S", "rv32_thumb_multi_ram.S", "rv32_thumb_ram_helper.S", "rv32_thumb_ram_store.S",
            "rv32_thumb_exit_shared.S")]
DEFS = ["HAVE_STRINGS_H", "HAVE_STDINT_H", "HAVE_INTTYPES_H", "__LIBRETRO__", "INLINE=inline", "NDEBUG",
        "GPSP_RV32_JIT=1", "GPSP_RV32_CODE_CHUNK_KIB=64", 'GIT_VERSION="3f112ae"']
# Profiling build: GBA_BENCH=1 (time per part of the frame, printed every 5 s), or a comma list of
# gpSP's detail switches, e.g. GBA_BENCH=VIDEO_DETAIL,RAM_CODE (see core/riscv/README.md)
BENCH = os.environ.get("GBA_BENCH", "")
if BENCH:
    DEFS += ["GPSP_BENCHMARK=1"] + ["GPSP_BENCHMARK_" + s.strip() for s in BENCH.split(",") if s.strip() not in ("", "1")]
    SRCS.append("gpsp_benchmark_timing.c")

build_emu("gba", CORE, SRCS, dict(id="emu.gba", name="Game Boy Advance", root="/sd/roms/gba", exts=".gba,.agb",
                                  save_dir="/sd/odroid/data/gba", run="gba_run", stack=65536),
          defines=DEFS, includes=["libretro", LRC + "include", "include", "."], opt="-O2", cxx=True,
          cflags=["-fomit-frame-pointer"])
