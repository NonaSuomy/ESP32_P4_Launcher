# Roadmap: features and upgrades to pick from

A living list of what could come next. Look here when deciding what to build; nothing in it is promised.
`docs/WORKPLAN.md` is the history of how the launcher got here (phases, milestones up to 0.21) and is not kept current.

**How to use it**
- Pick items from the tables; when one starts, mark it **in progress** and name the branch or session.
- When it ships, move it to [Done](#done) with the launcher version, in one line.
- New ideas go in the table of their area with a size. "Parked" holds what was decided against, and why, so it
  isn't proposed again without new facts.
- Sizes are rough: **S** = up to 2 days, **M** = about a week, **L** = 2+ weeks.

Last reviewed: 2026-11-26 (launcher 0.37.1).

## Suggested next

My short list, in order. Change it freely.

1. **Bigger app slots** (Platform): flash is getting tight. The Tab5 image has 257 KB (8%) left in its 3 MB slot,
   so the next big feature may not fit. The longer this waits, the more boards there are to migrate.
2. **Community PR queue** (Review queue): 17 PRs are open. Most are community apps, which reach the Store
   without a launcher release once merged.
3. **MP4 demuxer for apps** (Media): CODC decodes packets, but every player has to parse MP4 itself. A shared
   demuxer is what makes the media player (#70) and future video apps easy.
4. **Signed updates** (Store and updates): updates are checked against a SHA-256 from the same release, so
   anyone who can publish a release can push any firmware. A signature closes that before the user base grows.

## Platform

| Item | Size | Why / notes |
|---|---|---|
| Bigger app slots (~6.5 MB) | L | 16 MB flash, two OTA slots, factory dropped. Settings → Update can't change the partition table, so the launcher must rewrite it itself once. Worst case, users flash the full image once over USB; say so in the release notes |
| ABI freeze 1.0 | M | Drop "2.0-preview", write the versioning policy (interface minor/major rules). Needs a pass over every interface for what we'd regret |
| Conformance test app | M | Calls every interface and reports per board. Speeds up porting and catches a broken board early |
| Regression run covers more | M | `tests/board/smoke.py` runs the relocation test and 4 apps. Add emulators, forced stop, the codec test and a Store install from a local catalogue |
| PC simulator | L | Run apps on Windows or Linux over SDL2, built as native DLLs: fast iteration without a board |

## Media and codecs

| Item | Size | Why / notes |
|---|---|---|
| MP4 demuxer for apps | M | A CODC or SDK helper that reads MP4 (moov/stsz/stco) and hands out packets with timestamps. `apps/codec_test/mp4_to_packets.py` shows the logic, on the PC |
| H.264 on 2 cores: find the bottleneck | M | 2 cores give only 1.2x (36 → 44 fps at 640x360). FFmpeg's frame threads overlap poorly; profile before choosing slice threads or another split |
| Codec test: results on screen | S | Today it only logs over DevLink, so a Store user sees nothing. Draw pass/fail lines, and say where the test media goes |
| Store: an app whose codec went missing | S | It shows "Up to date"; Reinstall fixes it. Count a missing codec as an update |
| Camera + hardware encode (`CAM`) | L | MIPI-CSI camera, hardware JPEG/H.264 encode. Board-dependent: which boards have a camera connector? |
| More codec modules | M each | The module mechanism works for any decoder (HEVC, VP8, Opus, FLAC). Add one only when an app needs it |

## Store and updates

| Item | Size | Why / notes |
|---|---|---|
| Signed updates | M | Verify a signature on `latest.json` and the images, not only a hash from the same release |
| CI release builds | M | GitHub Actions builds the launcher for every board, and the apps, then drafts the release. Today the release is built on one PC, board by board |
| Store: screenshots on app pages | S–M | community/ already allows a `screenshot.png`; the Store doesn't show it |
| Store: ratings or download counts | M | Needs a server, not just GitHub releases. Probably not worth it yet |

## Launcher UI

| Item | Size | Why / notes |
|---|---|---|
| Sleep and wake | M | Only dimming exists. Screen off + light sleep after idle, wake by touch or button; matters on battery boards (Tab5) |
| Brightness in the in-game menu | S | The HOME menu has volume but not brightness |
| File associations | S | `.papp` manifests list the file types they open (`.nes`, `.mp4`, ...); Files and a library use it |
| Game library with cover art | L | Browse games across systems in one place; needs file associations |
| Files app: rename, copy, open with | M | It browses, previews and deletes only |
| Languages and themes | L | All launcher text is English and in the code |

## Input and Bluetooth

| Item | Size | Why / notes |
|---|---|---|
| Realtek USB Bluetooth dongles | M | They need a firmware download at start, which isn't done. Common, cheap dongles |
| Xbox One / Series pads (GIP) | M | XInput (Xbox 360) works; GIP is another protocol |
| Wired Switch Pro pad | S–M | PR #22 (NonaSuomy) has a version; review it |
| CherryUSB (issue #58) | ? | Requested. Evaluate what it would give over IDF's USB host (hub TT? classes?) before anything else |

## Network and developer tools

| Item | Size | Why / notes |
|---|---|---|
| DevLink over Wi-Fi | M | Develop without the cable; also lets a board in a case be tested |
| NET v2 for apps | L | WebSocket client, mDNS, ESP-NOW, IPv6, server TLS (BLE v2 pairing is done) |
| Save sync over Wi-Fi | L | Game saves to a PC or a cloud folder |

## Boards

| Item | Size | Why / notes |
|---|---|---|
| Elecrow AIO: test on hardware | S | The port has never run on the board here; NonaSuomy has one |
| Waveshare Nano DSI | M | PR #25 (NonaSuomy) |
| A board with HDMI | M | First real test of portability beyond DSI panels |
| Battery calibration | S | Check the battery readings against a measured cell, per board |

## Review queue

Open external PRs and issues (2026-11-26). Community apps (`community/`) reach the Store through CI with no launcher
release; project apps and launcher changes wait for one.

| What | Notes |
|---|---|
| Community apps: #71 NetSurf, #72 MicroSSH, #73 Scratch, #74 LED Switch (AboZaher), #75 Tulip CC, #76 Workspace | Check with `tools/community.py`, run each on a board |
| Emulators and interpreters: #59 C64, #60 Game & Watch, #61 Neo Geo Pocket, #62 AZIP, #63 WonderSwan, #64 Arduboy | Same questions as #69: does it build from this repo, licence, id, tested on which board? |
| #69 ScummVM | Waiting for NonaSuomy: build from this repo, or move to `community/` (comment posted) |
| #70 Media player | Black screen and no sound on the 7" v3.2; drop the 4 MB `.papp` from the PR (comment posted). Could move to CODC |
| #24 `.pax` suffix, #22 Switch Pro, #25 Nano DSI, #2 Elecrow AIO | Older PRs; #2 is probably superseded by the Elecrow port in the tree |
| Issue #1 "PSRAM APP's" | NonaSuomy's list of ported apps; a source of candidates |

## Parked

| Item | Decided | Why |
|---|---|---|
| Codec in the 3D games (Driftrock, Dustline, Gorgewing, Shardwall, Voidmarch) | 2026-11-26 | They synthesize all sound and draw procedurally, so there's nothing to decode. Revisit if they get music or picture assets |
| Global SD write lock (#68) | 2026-11-26 | Held across a whole DevLink upload: a stalled transfer blocks every later write, and a blocked app can't be stopped. A narrower per-path lock with a timeout would be considered |
| `CONFIG_FATFS_FS_LOCK` (#68) | 2026-11-26 | Changes behaviour on every board (a second open of a file being written fails). Needs its own PR with board tests |

## Done

Recent items from this list, newest first. Older history: `docs/WORKPLAN.md` and the release notes.

- 0.37.1: the Store installs codec modules with the apps that need them.
- 0.37.0: CODC decoders (MP3, AAC, H.264 module, hardware JPEG), DISP v5 `blit_yuv`; time zone on boards
  without an RTC; file lists that follow a finger (#67); SD write recovery (from #68).
