#!/usr/bin/env python3
"""Test media for codec_test: the first video and audio tracks of an MP4 / M4A as packet files.

  python apps/codec_test/mp4_to_packets.py in.mp4 out_prefix

Writes, for each track found:
  <prefix>.h264.pkt / <prefix>.aac.pkt   packets as the container has them (AVC length-prefixed / raw AAC)
                                          with the avcC / AudioSpecificConfig as the decoder's extra data
  <prefix>.h264                          the video as an Annex B stream (start codes, SPS/PPS in front)
  <prefix>.adts                          the audio as an ADTS stream

.pkt format (little-endian): "PKT1", u32 codec (2 = H.264, 4 = AAC), u32 timescale, u32 extra_len, extra,
then per packet: u32 len, i64 pts (track timescale units, composition time), u8 keyframe, 3 pad, data.

Making the MP4 on Windows without extra tools: apps/codec_test/transcode.ps1 (Media Foundation).
"""
import struct
import sys


def boxes(buf, off, end):
    while off + 8 <= end:
        size, kind = struct.unpack(">I4s", buf[off:off + 8])
        hdr = 8
        if size == 1:
            size = struct.unpack(">Q", buf[off + 8:off + 16])[0]
            hdr = 16
        elif size == 0:
            size = end - off
        yield kind.decode("latin1"), off + hdr, off + size
        off += size


def find(buf, off, end, path):
    for kind, s, e in boxes(buf, off, end):
        if kind == path[0]:
            if len(path) == 1:
                return s, e
            r = find(buf, s, e, path[1:])
            if r:
                return r
    return None


def full(buf, s):
    return s + 4                      # version + flags


def table(buf, rng, fmt, has_count=True):
    s, e = rng
    p = full(buf, s)
    n = struct.unpack(">I", buf[p:p + 4])[0]
    p += 4
    sz = struct.calcsize(fmt)
    return [struct.unpack(fmt, buf[p + i * sz:p + (i + 1) * sz]) for i in range(n)]


def esds_asc(buf, s, e):
    """AudioSpecificConfig: DecoderSpecificInfo (tag 5) inside the ES descriptor"""
    p = full(buf, s)
    while p < e:
        tag = buf[p]
        p += 1
        ln = 0
        for _ in range(4):
            b = buf[p]
            p += 1
            ln = ln << 7 | (b & 0x7F)
            if not b & 0x80:
                break
        if tag == 3:
            p += 3                    # ES_ID, flags (no optional fields in practice)
            continue
        if tag == 4:
            p += 13
            continue
        if tag == 5:
            return bytes(buf[p:p + ln])
        p += ln
    raise SystemExit("esds without AudioSpecificConfig")


def track(buf, s, e):
    mdia = find(buf, s, e, ["mdia"])
    hdlr = find(buf, *mdia, ["hdlr"])
    handler = bytes(buf[hdlr[0] + 8:hdlr[0] + 12]).decode("latin1")
    mdhd = find(buf, *mdia, ["mdhd"])
    v = buf[mdhd[0]]
    timescale = struct.unpack(">I", buf[mdhd[0] + (20 if v == 1 else 12):][:4])[0]
    stbl = find(buf, *mdia, ["minf", "stbl"])
    stsd = find(buf, *stbl, ["stsd"])
    p = full(buf, stsd[0]) + 4
    esize, ekind = struct.unpack(">I4s", buf[p:p + 8])
    ekind = ekind.decode("latin1")
    if ekind == "avc1":
        codec = 2
        r = find(buf, p + 8 + 78, p + esize, ["avcC"])
        extra = bytes(buf[r[0]:r[1]])
    elif ekind == "mp4a":
        codec = 4
        r = find(buf, p + 8 + 28, p + esize, ["esds"])
        extra = esds_asc(buf, *r)
    else:
        return None
    stsz = find(buf, *stbl, ["stsz"])
    q = full(buf, stsz[0])
    fixed, n = struct.unpack(">II", buf[q:q + 8])
    sizes = [fixed] * n if fixed else list(struct.unpack(">%dI" % n, buf[q + 8:q + 8 + 4 * n]))
    co = find(buf, *stbl, ["stco"])
    chunks = [c[0] for c in table(buf, co, ">I")] if co else [c[0] for c in table(buf, find(buf, *stbl, ["co64"]), ">Q")]
    stsc = table(buf, find(buf, *stbl, ["stsc"]), ">III")
    stts = table(buf, find(buf, *stbl, ["stts"]), ">II")
    ctts_r = find(buf, *stbl, ["ctts"])
    ctts = table(buf, ctts_r, ">Ii") if ctts_r else []
    stss_r = find(buf, *stbl, ["stss"])
    keys = {k[0] - 1 for k in table(buf, stss_r, ">I")} if stss_r else None
    # sample offsets
    offs = []
    si = 0
    for ci, coff in enumerate(chunks):
        per = None
        for first, spc, _ in stsc:
            if first - 1 <= ci:
                per = spc
        o = coff
        for _ in range(per):
            if si >= n:
                break
            offs.append(o)
            o += sizes[si]
            si += 1
    dts = []
    t = 0
    for cnt, d in stts:
        for _ in range(cnt):
            dts.append(t)
            t += d
    cts = []
    for cnt, d in ctts:
        cts += [d] * cnt
    out = []
    for i in range(n):
        pts = dts[i] + (cts[i] if i < len(cts) else 0)
        out.append((bytes(buf[offs[i]:offs[i] + sizes[i]]), pts, keys is None or i in keys))
    return handler, codec, timescale, extra, out


def avcc_to_annexb(extra, pkt):
    nal_len = (extra[4] & 3) + 1
    o = b""
    p = 0
    while p + nal_len <= len(pkt):
        ln = int.from_bytes(pkt[p:p + nal_len], "big")
        o += b"\0\0\0\1" + pkt[p + nal_len:p + nal_len + ln]
        p += nal_len + ln
    return o


def avcc_params(extra):
    o = b""
    p = 5
    nsps = extra[p] & 0x1F
    p += 1
    for _ in range(nsps):
        ln = int.from_bytes(extra[p:p + 2], "big")
        o += b"\0\0\0\1" + extra[p + 2:p + 2 + ln]
        p += 2 + ln
    npps = extra[p]
    p += 1
    for _ in range(npps):
        ln = int.from_bytes(extra[p:p + 2], "big")
        o += b"\0\0\0\1" + extra[p + 2:p + 2 + ln]
        p += 2 + ln
    return o


def adts(asc, frame):
    aot = asc[0] >> 3
    idx = ((asc[0] & 7) << 1) | (asc[1] >> 7)
    ch = (asc[1] >> 3) & 15
    ln = len(frame) + 7
    h = [0xFF, 0xF1, ((aot - 1) << 6) | (idx << 2) | (ch >> 2), ((ch & 3) << 6) | (ln >> 11),
         (ln >> 3) & 0xFF, ((ln & 7) << 5) | 0x1F, 0xFC]
    return bytes(h) + frame


def main():
    src, prefix = sys.argv[1], sys.argv[2]
    buf = open(src, "rb").read()
    moov = find(buf, 0, len(buf), ["moov"])
    for kind, s, e in boxes(buf, *moov):
        if kind != "trak":
            continue
        t = track(buf, s, e)
        if not t:
            continue
        handler, codec, ts, extra, pkts = t
        name = "h264" if codec == 2 else "aac"
        with open(f"{prefix}.{name}.pkt", "wb") as f:
            f.write(b"PKT1" + struct.pack("<III", codec, ts, len(extra)) + extra)
            for data, pts, key in pkts:
                f.write(struct.pack("<IqB3x", len(data), pts, 1 if key else 0) + data)
        if codec == 2:
            with open(f"{prefix}.h264", "wb") as f:
                f.write(avcc_params(extra))
                for data, _, _ in pkts:
                    f.write(avcc_to_annexb(extra, data))
        else:
            with open(f"{prefix}.adts", "wb") as f:
                for data, _, _ in pkts:
                    f.write(adts(extra, data))
        print(f"{name}: {len(pkts)} packets, timescale {ts}, extra {extra.hex()[:48]}")


if __name__ == "__main__":
    main()
