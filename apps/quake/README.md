# Quake for P4 Launcher

This app uses WinQuake's CD-audio track requests to play optional MP3 music from
SD. Put tracks in `roms/quake/id1/music/` using either `track02.mp3` or
`track2.mp3` naming (and so on). Track playback follows the game's normal
`bgmvolume` setting. Sound effects continue to use the Quake mixer.

The MP3 decoder is the OpenCORE decoder, licensed under Apache-2.0. Its
upstream notices and license are retained in `third_party/micro-mp3/`.
