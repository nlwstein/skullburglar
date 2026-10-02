#!/usr/bin/env python3
"""Convert a REX_AUDIO_DUMP file (SDK patch 0002) to a stereo WAV and print loudness.

Dump format: frames of 6 channels x 256 samples, channel-sequential, big-endian float32,
48 kHz. Stereo = FL/FR (channels 0/1). Prints per-second RMS so silence vs sound is obvious.
  python3 tools/audio_dump.py run/audio.raw [out.wav]
"""
import array, math, struct, sys, wave

src = sys.argv[1]
out = sys.argv[2] if len(sys.argv) > 2 else src.rsplit(".", 1)[0] + ".wav"
raw = open(src, "rb").read()
FRAME = 6 * 256
n = len(raw) // (FRAME * 4)
f = array.array("f", raw[: n * FRAME * 4])
f.byteswap()  # BE -> host LE
pcm = array.array("h")
sec_sq, sec_n, rms = 0.0, 0, []
peak = 0.0
for i in range(n):
    base = i * FRAME
    for s in range(256):
        l, r = f[base + s], f[base + 256 + s]
        for v in (l, r):
            v = max(-1.0, min(1.0, v if v == v else 0.0))
            peak = max(peak, abs(v))
            pcm.append(int(v * 32767))
            sec_sq += v * v
        sec_n += 2
        if sec_n >= 48000 * 2:
            rms.append(math.sqrt(sec_sq / sec_n)); sec_sq, sec_n = 0.0, 0
with wave.open(out, "wb") as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000); w.writeframes(pcm.tobytes())
print(f"{n} frames = {n*256/48000:.1f} s, peak {peak:.3f} -> {out}")
print("per-second RMS dBFS:", " ".join(f"{20*math.log10(x):.0f}" if x > 0 else "-inf" for x in rms))
