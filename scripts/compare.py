"""Measure and visualise what the filter did to a recording.

usage:
  python3 scripts/compare.py noisy.wav cleaned.wav [clean_reference.wav]

- Always: plots the frequency spectrum before vs after -> results/spectrum.png
- If a clean reference is given: computes SNR (signal-to-noise ratio) before and after.
"""
import os
import sys
import wave

import matplotlib.pyplot as plt
import numpy as np


def read_wav(path):
    with wave.open(path, "rb") as w:
        rate, ch = w.getframerate(), w.getnchannels()
        x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64)
    return x.reshape(-1, ch).mean(axis=1) / 32768.0, rate


def snr_db(reference, test):
    """How much louder the real signal is than the error, in decibels. Higher = cleaner."""
    noise = test - reference
    return 10 * np.log10(np.sum(reference ** 2) / max(1e-20, np.sum(noise ** 2)))


def spectrum(x, rate):
    """Average magnitude spectrum (dB) over 4096-sample windows."""
    n = 4096
    win = np.hanning(n)
    frames = [x[i:i + n] * win for i in range(0, len(x) - n, n // 2)]
    mag = np.mean([np.abs(np.fft.rfft(f)) for f in frames], axis=0)
    return np.fft.rfftfreq(n, 1 / rate), 20 * np.log10(mag + 1e-12)


if len(sys.argv) < 3:
    sys.exit(__doc__)

noisy, rate = read_wav(sys.argv[1])
cleaned, _ = read_wav(sys.argv[2])
os.makedirs("results", exist_ok=True)

if len(sys.argv) > 3:
    ref, _ = read_wav(sys.argv[3])
    # A symmetric FIR delays the signal by (taps-1)/2 samples. Find that delay
    # by testing shifts, and compare like with like.
    best = max(range(0, 600), key=lambda d: -np.sum((cleaned[d:d + len(ref) - 600] - ref[:len(ref) - 600]) ** 2))
    n = len(ref) - 600
    before = snr_db(ref[:n], noisy[:n])
    after = snr_db(ref[:n], cleaned[best:best + n])
    print(f"filter delay: {best} samples ({1000 * best / rate:.2f} ms)")
    print(f"SNR before: {before:5.1f} dB")
    print(f"SNR after : {after:5.1f} dB   (improvement {after - before:+.1f} dB)")

f, a = spectrum(noisy, rate)
_, b = spectrum(cleaned, rate)
plt.figure(figsize=(9, 4))
plt.plot(f / 1000, a, lw=0.8, label="noisy input")
plt.plot(f / 1000, b, lw=0.8, label="filtered output")
plt.xlabel("frequency (kHz)")
plt.ylabel("magnitude (dB)")
plt.title("Frequency spectrum before and after the low-pass FIR filter")
plt.legend()
plt.tight_layout()
plt.savefig("results/spectrum.png", dpi=150)
print("saved results/spectrum.png")
