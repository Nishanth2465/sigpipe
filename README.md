# sigpipe — Concurrent Signal-Processing Pipeline in C

A multithreaded C pipeline that streams noisy "sensor" samples through a bounded
ring buffer to a pool of worker threads, which clean the signal with a low-pass
FIR filter in parallel.

```
 [producer thread] --chunks--> [ring buffer] --> [N worker threads: FIR filter] --> [output]
  noisy 5 Hz sine               bounded,          each filters one chunk;            written by
  + 2 kHz hum + noise           blocking          sequence number keeps order        sequence number
```

![filter result](results/signal.png)

## Real recordings (WAV)

sigpipe can clean real audio instead of the synthetic signal:

```bash
make demo
```

This (1) creates `results/noisy.wav`: a melody plus hiss plus a 7 kHz whine, (2) cleans it
with a 255-tap, 3.5 kHz low-pass filter on 4 threads, and (3) measures the result:

```
filter delay: 127 samples (2.65 ms)
SNR before:   5.2 dB
SNR after :  17.2 dB   (improvement +12.1 dB)
```

![spectrum](results/spectrum.png)

The melody (below ~1.6 kHz) passes through untouched; the 7 kHz whine and the hiss
above 3.5 kHz drop by 60-70 dB. Listen to `results/noisy.wav` and `results/cleaned.wav`.

**Use your own recording** (voice memo, music, 16-bit PCM WAV):

```bash
python3 scripts/make_noisy_wav.py my_voice.wav                         # adds hiss + whine
./sigpipe -i results/noisy.wav -w results/cleaned.wav -f 3500 -k 255 -v
python3 scripts/compare.py results/noisy.wav results/cleaned.wav results/clean.wav
```

Or clean an already-noisy file directly: `./sigpipe -i noisy.wav -w cleaned.wav -f 3500 -k 255`.
Convert other formats first: `ffmpeg -i input.m4a -ac 1 -c:a pcm_s16le input.wav`.

A low-pass filter only helps when the useful signal is low-frequency (voice, music, motion)
and the noise is higher. If the signal itself is broadband, filtering removes signal too.

## Build & run

Needs Linux (or WSL on Windows / macOS with gcc or clang), `make`, Python 3 + numpy + matplotlib for graphs.

```bash
make                 # build ./sigpipe
make test            # multithreaded output must match single-threaded reference exactly
make bench           # throughput for 1, 2, 4, 8 threads -> results/bench.csv
make plot            # graphs -> results/signal.png, results/speedup.png
```

Options:

```
./sigpipe -t 4 -n 4000000 -c 4096 -k 129 -r 16 -f 1000 -v
  -t  worker threads             (default 4)
  -n  total samples, synthetic   (default 4,000,000)
  -c  samples per chunk          (default 4096)
  -k  FIR taps (filter length)   (default 129)
  -r  ring buffer capacity       (default 16 chunks)
  -f  low-pass cutoff in Hz      (default 1000)
  -i  input WAV (16-bit PCM; stereo is mixed to mono)
  -w  output WAV
  -v  verify against a single-threaded reference run
  -o  write first 0.1 s of samples to CSV
```

## Verification

```bash
make tsan  && ./sigpipe_tsan  -t 8 -n 300000 -c 500 -r 1 -v   # data races
make debug && ./sigpipe_debug -t 4 -n 200000 -c 777 -v        # memory errors, UB
valgrind --leak-check=full ./sigpipe -t 4 -n 100000 -v        # leaks
```

- Output is **bit-for-bit identical** to a single-threaded run for any thread count, chunk size or ring capacity.
- ThreadSanitizer: no data races, even at ring capacity 1 (maximum contention).
- AddressSanitizer/UBSan and Valgrind: no errors, no leaks.

## How it works

| File | What it does |
|---|---|
| `src/ring.c` | Bounded circular queue. One mutex protects it; two condition variables (`not_full`, `not_empty`) put the producer or consumers to sleep instead of busy-waiting. `ring_close()` broadcasts so all workers exit cleanly. |
| `src/fir.c` | Designs a windowed-sinc low-pass filter (Hamming window, configurable cutoff via `-f`) and applies it: `y[n] = Σ h[k]·x[n−k]`. |
| `src/wav.c` | Minimal 16-bit PCM WAV reader/writer: parses RIFF chunks, mixes stereo to mono, clips on output. |
| `src/main.c` | Producer reads samples (synthetic or WAV) in chunks; a pool of worker threads pops chunks, filters them and writes output at `seq × chunk`. Times the run and optionally verifies it. |

### Key design decisions

- **Chunk overlap.** An FIR filter needs the previous `taps − 1` samples. Each chunk carries that history from the previous chunk, so chunks can be filtered independently on any thread and still produce exactly the same result as one long run.
- **Sequence numbers.** Workers finish out of order; each chunk's `seq` decides where its output goes, so no reordering step or extra lock is needed.
- **Backpressure.** The ring buffer has a fixed capacity. If workers fall behind, `ring_push` blocks the producer instead of letting memory grow without bound.
- **Work outside the lock.** The mutex is held only to take a chunk off the queue; the expensive filtering runs unlocked, so workers truly run in parallel.
- **`while`, not `if`, around `pthread_cond_wait`.** A woken thread re-checks the condition: wakeups can be spurious, or another thread may have taken the item first.

## Results

![speedup](results/speedup.png)

Speedup grows until the number of threads reaches the number of CPU cores, then flattens:
extra threads just take turns on the same cores. The producer thread also needs CPU time,
so it becomes the bottleneck as workers get faster (Amdahl's law).
