#!/usr/bin/env python3
"""Quake (WinQuake via quake-go): the RetroESP32_P4 v1 PSRAM app on the v1 compatibility host.
Data: /sd/roms/quake/id1/pak0.pak; saves /sd/saves/quake, config /sd/config/quake.
    python apps/quake/build.py"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MP3_DIR = os.path.join(HERE, "third_party", "micro-mp3")
sys.path.insert(0, os.path.join(HERE, "..", "common", "v1compat"))
import v1build  # noqa: E402

SRCS = {'quake/winquake': ['chase.c', 'cmd.c', 'common.c', 'console.c', 'crc.c', 'cvar.c', 'draw.c', 'host.c', 'host_cmd.c', 'keys.c', 'mathlib.c', 'menu.c', 'model.c', 'nonintel.c', 'screen.c', 'sbar.c', 'zone.c', 'view.c', 'wad.c', 'world.c', 'cl_demo.c', 'cl_input.c', 'cl_main.c', 'cl_parse.c', 'cl_tent.c', 'd_edge.c', 'd_fill.c', 'd_init.c', 'd_modech.c', 'd_part.c', 'd_polyse.c', 'd_scan.c', 'd_sky.c', 'd_sprite.c', 'd_surf.c', 'd_vars.c', 'd_zpoint.c', 'net_loop.c', 'net_main.c', 'net_vcr.c', 'pr_cmds.c', 'pr_edict.c', 'pr_exec.c', 'r_aclip.c', 'r_alias.c', 'r_bsp.c', 'r_light.c', 'r_draw.c', 'r_efrag.c', 'r_edge.c', 'r_misc.c', 'r_main.c', 'r_sky.c', 'r_sprite.c', 'r_surf.c', 'r_part.c', 'r_vars.c', 'sv_main.c', 'sv_phys.c', 'sv_move.c', 'sv_user.c', 'cd_null.c', 'net_none.c', 'snd_mem.c']}
GLUE = ['papp_main.c', 'papp_vid.c', 'papp_snd.c', 'papp_input.c', 'papp_sys.c', 'papp_rg_stubs.c', 'papp_syscalls.c']
MP3_SRCS = ["papp_mp3.cpp"] + sorted(
    name for name in os.listdir(MP3_DIR) if name.endswith(".cpp")
)
FAST = '-DIRAM_ATTR=__attribute__((section(".fast.text")))'     # hot paths to internal SRAM

srcs = ([os.path.join(HERE, d, f) for d, files in SRCS.items() for f in files]
        + [os.path.join(HERE, f) for f in GLUE]
        + [os.path.join(HERE, "papp_mp3.cpp")]
        + [os.path.join(MP3_DIR, f) for f in MP3_SRCS[1:]])
cpp_flags = {
    source: ["-std=gnu++17", "-fno-exceptions", "-fno-rtti"]
    for source in srcs if source.endswith(".cpp")
}
v1build.build('quake', srcs, includes=[os.path.join(HERE, i) for i in ['compat', 'quake/winquake', 'quake/esp32quake', 'quake']],
              defines=['-std=gnu11', '-DESP32_QUAKE=1', '-DESP_PLATFORM=1', '-DCONFIG_IDF_TARGET_ESP32P4=1', '-DCONFIG_IDF_TARGET_ARCH_RISCV=1', '-fcommon'] + [FAST, "-O3"],
              extra_cflags=["-I" + MP3_DIR, "-I" + os.path.join(MP3_DIR, "oscl")],
              per_file_cflags=cpp_flags, menu_button='0', volume_button='0',
              rename_main=False, extra_ldflags=['-Wl,--allow-multiple-definition'], icon_dir=HERE, title='Quake')
