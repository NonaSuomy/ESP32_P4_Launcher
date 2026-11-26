# Mediaplayer

Native PAPP2 media player using the PAX FFmpeg 7.1.5 port. It browses `/sd/videos/` and `/sd/`, and accepts HTTP(S) media URLs. Video and audio decoding use software codecs; performance depends on codec, resolution, and source complexity. HEVC, VP9, and AV1 are experimental.

The PAPP2 manifest keeps the app ID `video` so NetSurf's existing media-link handoff can find it. The launcher displays the name **Mediaplayer**. The package is hosted as a version-pinned binary because it is larger than the repository's 4 MB inline community-app limit.
