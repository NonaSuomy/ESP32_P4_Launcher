"""Test media for the media player: a 10 s stereo WAV tone and a 10 s 640x360 MJPEG AVI (25 fps, with PCM audio)."""
import io, math, struct, sys, wave
from PIL import Image, ImageDraw

out = sys.argv[1]
RATE, SECS = 44100, 10

def tone(n0, n):
    b = bytearray()
    for i in range(n0, n0 + n):
        t = i / RATE
        f = 440 if int(t) % 2 == 0 else 660
        v = int(8000 * math.sin(2 * math.pi * f * t))
        b += struct.pack('<hh', v, v)
    return bytes(b)

with wave.open(f"{out}/tone.wav", "wb") as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(RATE)
    w.writeframes(tone(0, RATE * SECS))

W, H, FPS = 640, 360, 25
frames = []
for k in range(FPS * SECS):
    im = Image.new("RGB", (W, H), (20, 20, 60))
    d = ImageDraw.Draw(im)
    x = int((k * 8) % (W + 80)) - 80
    d.rectangle([x, 140, x + 80, 220], fill=(240, 180, 40))
    d.text((20, 20), f"frame {k}  t={k / FPS:.2f}s", fill=(255, 255, 255))
    bio = io.BytesIO(); im.save(bio, "JPEG", quality=80)
    frames.append(bio.getvalue())

def chunk(fourcc, data):
    pad = b"\0" if len(data) & 1 else b""
    return fourcc + struct.pack("<I", len(data)) + data + pad

def lst(kind, data):
    return b"LIST" + struct.pack("<I", len(data) + 4) + kind + data

apf = RATE // FPS
movi, idx, off = b"", b"", 4
for k, f in enumerate(frames):
    c = chunk(b"00dc", f); idx += b"00dc" + struct.pack("<III", 0x10, off, len(f)); movi += c; off += len(c)
    a = tone(k * apf, apf)
    c = chunk(b"01wb", a); idx += b"01wb" + struct.pack("<III", 0x10, off, len(a)); movi += c; off += len(c)

maxf = max(len(f) for f in frames)
avih = struct.pack("<IIIIIIIIIIIIII", 1000000 // FPS, 0, 0, 0x10, len(frames), 0, 2, maxf, W, H, 0, 0, 0, 0)
vstrh = struct.pack("<4s4sIHH8I4h", b"vids", b"MJPG", 0, 0, 0, 0, 1, FPS, 0, len(frames), maxf, 0xFFFFFFFF, 0, 0, 0, W, H)
vstrf = struct.pack("<IiiHH4sIiiII", 40, W, H, 1, 24, b"MJPG", W * H * 3, 0, 0, 0, 0)
astrh = struct.pack("<4s4sIHH8I4h", b"auds", b"\0\0\0\0", 0, 0, 0, 0, 1, RATE, 0, RATE * SECS, apf * 4, 0xFFFFFFFF, 4, 0, 0, 0, 0)
astrf = struct.pack("<HHIIHH", 1, 2, RATE, RATE * 4, 4, 16)
hdrl = lst(b"hdrl", chunk(b"avih", avih) + lst(b"strl", chunk(b"strh", vstrh) + chunk(b"strf", vstrf))
           + lst(b"strl", chunk(b"strh", astrh) + chunk(b"strf", astrf)))
body = b"AVI " + hdrl + lst(b"movi", movi) + chunk(b"idx1", idx)
open(f"{out}/test_mjpeg.avi", "wb").write(b"RIFF" + struct.pack("<I", len(body)) + body)
print("ok", len(body))
