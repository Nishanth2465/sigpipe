"""Create a noisy test recording for sigpipe.

usage:
  python3 scripts/make_noisy_wav.py                 # synthesise a short melody, then add noise
  python3 scripts/make_noisy_wav.py my_voice.wav    # add noise to YOUR recording

Writes:
  results/clean.wav   the original (used to measure how well the filter works)
  results/noisy.wav   original + hiss + a high-pitched 7 kHz whine
"""
import os
import sys
import wave

import numpy as np

RATE = 48000
os.makedirs("results", exist_ok=True)


def read_wav(path):
    with wave.open(path, "rb") as w:
        if w.getsampwidth() != 2:
            sys.exit(f"{path}: need 16-bit PCM WAV "
                     "(convert: ffmpeg -i in.wav -ac 1 -c:a pcm_s16le out.wav)")
        rate, ch = w.getframerate(), w.getnchannels()
        x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float32)
    x = x.reshape(-1, ch).mean(axis=1) / 32768.0          # mix to mono, scale to [-1, 1)
    return x, rate


def write_wav(path, x, rate):
    pcm = (np.clip(x, -1, 1) * 32767).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm.tobytes())


def melody(rate, seconds=6.0):
    """Simple piano-like melody: notes with a few harmonics and a decay."""
    notes = [262, 294, 330, 349, 392, 440, 494, 523, 494, 440, 392, 330]   # C major-ish, in Hz
    note_len = seconds / len(notes)
    t = np.arange(int(rate * note_len)) / rate
    env = np.exp(-3.0 * t)                                 # each note fades out
    out = []
    for f in notes:
        tone = (np.sin(2 * np.pi * f * t)
                + 0.5 * np.sin(2 * np.pi * 2 * f * t)
                + 0.25 * np.sin(2 * np.pi * 3 * f * t))
        out.append(0.3 * env * tone)
    return np.concatenate(out).astype(np.float32)


if len(sys.argv) > 1:
    clean, rate = read_wav(sys.argv[1])
    clean = 0.7 * clean / max(1e-9, np.max(np.abs(clean)))   # normalise loudness
    print(f"using {sys.argv[1]} ({len(clean) / rate:.1f} s, {rate} Hz)")
else:
    rate = RATE
    clean = melody(rate)
    print("synthesised a 6 s melody")

rng = np.random.default_rng(7)
t = np.arange(len(clean)) / rate
hiss = 0.05 * rng.standard_normal(len(clean))              # broadband hiss
whine = 0.08 * np.sin(2 * np.pi * 7000 * t)                # high-pitched electronic whine
noisy = (clean + hiss + whine).astype(np.float32)

write_wav("results/clean.wav", clean, rate)
write_wav("results/noisy.wav", noisy, rate)
print("wrote results/clean.wav and results/noisy.wav")
