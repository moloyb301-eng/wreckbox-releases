"""Makes the player test fixtures: a stereo test tone (WAV), a tiny ProTracker module, and a playlist.
The WAV is then encoded to the other formats by make_audio_fixtures.ps1."""
import math, struct, sys, wave
from pathlib import Path

out = Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)

# 2.5 s stereo, 44.1 kHz: 100 Hz + 1 kHz + 5 kHz, so equalizer tests have energy in low / mid / high bands.
rate, secs = 44100, 2.5
frames = bytearray()
for i in range(int(rate * secs)):
    t = i / rate
    s = 0.3 * math.sin(2 * math.pi * 100 * t) + 0.25 * math.sin(2 * math.pi * 1000 * t) + 0.2 * math.sin(2 * math.pi * 5000 * t)
    v = int(s * 32767)
    frames += struct.pack('<hh', v, v)
with wave.open(str(out / 'tone.wav'), 'wb') as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(rate); w.writeframes(bytes(frames))

# ProTracker "M.K." module: one looping square-wave sample, one pattern of 16 rows (then a pattern break), ~2 s.
title = b'wreckbox test'.ljust(20, b'\0')
sample = bytes((64 if (i // 16) % 2 == 0 else 192) for i in range(64))  # signed 8-bit square: +64 / -64
headers = b''
for n in range(31):
    if n == 0:
        headers += b'square'.ljust(22, b'\0') + struct.pack('>HBBHH', len(sample) // 2, 0, 64, 0, len(sample) // 2)
    else:
        headers += b'\0' * 22 + struct.pack('>HBBHH', 0, 0, 0, 0, 1)
order = bytes([1, 127]) + bytes([0] * 128) + b'M.K.'
periods = [428, 381, 339, 320]  # C-2 D-2 E-2 F-2
pattern = bytearray()
for row in range(64):
    for ch in range(4):
        if ch == 0 and row % 4 == 0 and row < 16:
            p = periods[(row // 4) % 4]
            pattern += bytes([(1 >> 4) << 4 | (p >> 8), p & 0xFF, (1 & 0xF) << 4, 0])
        elif ch == 1 and row == 15:
            pattern += bytes([0, 0, 0x0D, 0])  # Dxx: pattern break → the song ends
        else:
            pattern += bytes(4)
(out / 'tone.mod').write_bytes(title + headers + order + bytes(pattern) + sample)

# A playlist with two entries (paths relative to the playlist).
(out / 'two.m3u').write_text('#EXTM3U\n#EXTINF:2,Test - Ogg tone\ntone.ogg\n#EXTINF:2,Test - FLAC tone\ntone.flac\n', encoding='utf-8')
print('wrote', sorted(p.name for p in out.iterdir()))
