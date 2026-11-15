"""
Builds a RetroESP32_P4 v1 PSRAM app (game + its own glue) as a v2 .papp on top
of v1_host.c (the v1 service table implemented with the v2 SDK).

The game's newlib glue (papp_syscalls.c: --wrap malloc, fds, locks) replaces
the SDK's libc_glue.c, so it is linked with libc but without the SDK glue.
At compile time the game's `papp_exit_requested` flag becomes
`v1_exit_requested` (the SDK has a function of that name) and, with
rename_main, its `main` becomes `v1_game_main` (crt0 calls our main).
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(ROOT, "papp-sdk", "tools"))
import papp_build  # noqa: E402

QUIET = ["-Wno-format", "-Wno-unused-variable", "-Wno-unused-function", "-Wno-unused-but-set-variable",
         "-Wno-maybe-uninitialized", "-Wno-implicit-function-declaration", "-Wno-pointer-sign",
         "-Wno-int-conversion", "-Wno-incompatible-pointer-types", "-Wno-dangling-pointer",
         "-Wno-array-bounds", "-Wno-address", "-Wno-restrict", "-Wno-builtin-declaration-mismatch",
         "-Wno-char-subscripts", "-Wno-sign-compare", "-Wno-parentheses", "-Wno-missing-braces",
         "-Wno-misleading-indentation", "-Wno-unused-value", "-Wno-stringop-truncation",
         "-Wno-stringop-overflow", "-Wno-format-overflow", "-Wno-return-type", "-Wno-switch",
         "-Wno-dangling-else", "-Wno-unused-label", "-Wno-implicit-int", "-Wno-discarded-qualifiers",
         "-Wno-enum-conversion", "-Wno-overflow", "-Wno-shift-negative-value", "-Wno-attributes",
         "-Wno-format-truncation", "-Wno-unused-const-variable", "-Wno-nonnull", "-Wno-use-after-free"]


def build(name, game_srcs, includes, defines=(), menu_button="PAPP_BTN_X", volume_button="0",
          rename_main=False, extra_cflags=(), extra_ldflags=(), per_file_cflags=None,
          icon_dir=None, title=None, stack=32768):
    """game_srcs: engine + glue .c files. includes: the game's compat dir first."""
    defs = ["-DPAPP_APP_SIDE=1", "-Dpapp_exit_requested=v1_exit_requested"] + list(defines)
    if rename_main:
        defs.append("-Dmain=v1_game_main")
    host = os.path.join(HERE, "v1_host.c")
    # the host is an app source too: undo the game-only renames for it
    host_flags = [f"-DV1_MENU_BUTTON={menu_button}", f"-DV1_VOLUME_BUTTON={volume_button}",
                  f'-DV1_ID="game.{name}"', f'-DV1_NAME="{title or name}"', f"-DV1_STACK={stack}",
                  "-Upapp_exit_requested", "-Umain"]
    return papp_build.build(
        name, [host] + list(game_srcs), includes=list(includes) + [HERE], libc=True, libc_glue=False,
        app_cflags=QUIET + defs + list(extra_cflags),
        per_file_cflags={host: host_flags, **(per_file_cflags or {})},
        extra_ldflags=list(extra_ldflags), icon_dir=icon_dir)
