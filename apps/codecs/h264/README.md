# h264.papp: the launcher's H.264 codec module

FFmpeg 7.1.1's H.264 decoder as a CODC codec module (docs/ARCHITECTURE.md §7.11). The launcher loads it from
`/sd/launcher/codecs/h264.papp` when an app first opens `PAPP_CODEC_H264`.

```powershell
python apps\codecs\h264\build.py            # -> build\codecs\h264\h264.papp
python tools\papp_dev.py --port COM6 put build\codecs\h264\h264.papp /sd/launcher/codecs/h264.papp
```

FFmpeg's source is not in the repo: `build.py` looks for `third_party_dl/ffmpeg-7.1.1` above the checkout
(ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz), or `--ffmpeg <dir>`. `build_ffmpeg.py --configure` regenerates
`ffmpeg_config/` and `ffmpeg_files.txt` (FFmpeg's configure under Git Bash; no make needed).

- `h264_module.c`: the module (decoder runner thread, access unit queue, Annex B parser)
- `pthread_shim.c`: the POSIX threads, `sysconf`, clocks FFmpeg uses, on THRD
- `patches/`: FFmpeg files compiled instead of the originals (see the comment at each change)

Licence: FFmpeg is LGPL 2.1 or later; so is this module as distributed. Its source is this folder plus
FFmpeg 7.1.1 with `patches/` applied.
