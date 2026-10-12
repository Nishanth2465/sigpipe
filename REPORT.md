# sigpipe — Project Report

## 1. What the project is

sigpipe is a multithreaded signal-processing pipeline written in C11 with POSIX threads.
It takes a noisy signal, either a synthetic "sensor" reading or a real WAV recording,
and removes the noise with a low-pass FIR filter that runs on several CPU cores at once.

It brings three topics together:

- **Concurrency:** a producer/consumer design built on a bounded ring buffer, a mutex and condition variables.
- **Digital signal processing:** windowed-sinc FIR filter design (Hamming window) and convolution.
- **Verification:** the parallel output is checked bit-for-bit against a single-threaded reference, and the code is run under ThreadSanitizer, AddressSanitizer/UBSan and Valgrind.

```
 [producer thread] --chunks--> [ring buffer] --> [N worker threads: FIR filter] --> [output]
  reads / generates            bounded,          each filters one chunk              written at
  samples in chunks            blocking          independently                       seq x chunk
```

### Code size

| Part | Files | Lines |
|---|---|---|
| C core (`src/`) | `main.c`, `ring.c/h`, `fir.c/h`, `wav.c/h` | 620 |
| Scripts + Makefile | `bench.sh`, `plot.py`, `compare.py`, `make_noisy_wav.py`, `Makefile` | 254 |
| **Total** | 12 | **874** |

No external C libraries: only libc, libm and pthreads.

## 2. How it works

| Component | Responsibility |
|---|---|
| `ring.c` | Fixed-capacity circular queue. One mutex guards it; `not_full` and `not_empty` condition variables put threads to sleep instead of busy-waiting. `ring_close()` broadcasts so every worker exits cleanly. |
| `fir.c` | Designs the filter, `h[k] = sinc(k) · hamming(k)` normalised to unity DC gain, and applies it: `y[n] = Σ h[k]·x[n−k]`. |
| `wav.c` | 16-bit PCM WAV reader/writer. Walks RIFF chunks, mixes stereo to mono and clips on output. |
| `main.c` | Option parsing, the producer loop, the worker pool, timing, verification, CSV/WAV output. |

### Key design decisions

1. **Overlapping chunks.** An FIR filter with `k` taps needs the previous `k − 1` input samples. Every chunk carries that history from the chunk before it, so any thread can filter any chunk on its own and still get exactly the same result as one long run.
2. **Sequence numbers instead of reordering.** Workers finish out of order. Each chunk's `seq` gives its output position (`out[seq × chunk]`), so writes never overlap and no lock or reorder buffer is needed.
3. **Backpressure.** The ring has a fixed capacity, so if the workers fall behind, `ring_push` blocks the producer. Memory stays bounded however large the input is.
4. **Expensive work outside the lock.** The mutex is held only while a pointer is taken off the queue, and the filtering runs unlocked.
5. **`while` around `pthread_cond_wait`.** A woken thread re-checks its condition, which protects against spurious wakeups and against another thread taking the item first.

## 3. Metrics

### Test environment

| | |
|---|---|
| CPU | Intel Xeon @ 2.10 GHz, **4 cores** (cloud VM) |
| Compiler | gcc 13.3.0, `-O2 -std=c11` |
| OS | Linux (Ubuntu 24.04) |

Timings on a shared VM vary by roughly ±10% from run to run. The benchmark script keeps
the best of 3 runs, and the sweeps below are single runs. Absolute numbers will differ on
other machines, but the trends should hold.

### 3.1 Correctness

| Check | Command | Result |
|---|---|---|
| 1 thread vs reference | `./sigpipe -t 1 -n 1000000 -v` | **OK, 0 mismatches** |
| 4 threads vs reference | `./sigpipe -t 4 -n 1000000 -v` | **OK, 0 mismatches** |
| 8 threads, odd sizes, tiny ring | `./sigpipe -t 8 -n 1000003 -c 1000 -r 2 -v` | **OK, 0 mismatches** |
| Real WAV, 255 taps | `make demo` | **OK, 0 mismatches** |
| Data races | ThreadSanitizer, 8 threads, ring capacity 1 | **0 warnings** |
| Memory errors / UB | AddressSanitizer + UBSan, chunk size 777 | **0 errors** |
| Leaks | Valgrind `--leak-check=full` | **0 errors, all heap blocks freed** |

Verification uses exact float equality (`out[i] != ref[i]`), not a tolerance, so the
multithreaded result is bit-for-bit identical to the single-threaded one.

### 3.2 Throughput vs thread count (`make bench`)

8,000,000 samples, 129 taps, chunk 4096, best of 3 runs.

| Threads | Best time | Throughput | Speedup | Parallel efficiency |
|---|---|---|---|---|
| 1 | 0.531 s | 15.07 Msamples/s | 1.00× | 100% |
| 2 | 0.326 s | 24.54 Msamples/s | 1.63× | 81% |
| 4 | 0.266 s | 30.08 Msamples/s | 2.00× | 50% |
| 8 | 0.258 s | 31.01 Msamples/s | 2.06× | 26% |

Scaling flattens at 4 threads, which matches the 4 cores on this machine. Beyond that,
extra threads only take turns on the same cores. Even at 4 threads the speedup is only
2×, because the producer also needs a core: it computes two `sin()` calls and a random
number for every synthetic sample. With 129 taps, the workers are fast enough that the
producer becomes the bottleneck (Amdahl's law).

### 3.3 Throughput vs filter length

4,000,000 samples, chunk 4096. More taps means more work per sample, which shifts the
bottleneck from the producer to the workers, so parallel speedup improves.

| Taps | 1 thread | 4 threads | Speedup |
|---|---|---|---|
| 31 | 31.38 Msamples/s | 39.09 Msamples/s | 1.25× |
| 129 | 14.59 Msamples/s | 33.51 Msamples/s | 2.30× |
| 255 | 5.50 Msamples/s | 18.08 Msamples/s | 3.29× |
| 511 | 2.81 Msamples/s | 10.06 Msamples/s | **3.58×** |

With 511 taps the pipeline reaches 3.58× on 4 cores (90% efficiency). That shows the
threading design scales well once the per-chunk work dominates.

### 3.4 Throughput vs chunk size

4,000,000 samples, 129 taps, 4 threads.

| Chunk size | Throughput |
|---|---|
| 256 | 14.48 Msamples/s |
| 1,024 | 25.23 Msamples/s |
| 4,096 (default) | 25.71 Msamples/s |
| 16,384 | 30.62 Msamples/s |
| 65,536 | 37.54 Msamples/s |

Small chunks are expensive because each one costs a `malloc`, a lock round-trip and a copy
of 128 history samples. At 256 samples the copied history is half the size of the new data.
Larger chunks spread that fixed cost over more samples.

### 3.5 Filter quality: synthetic signal

The default input is a 5 Hz sine (the "real" signal) plus a 2 kHz hum and uniform random
noise, filtered at 1 kHz with 129 taps. The first 0.1 s was compared against the clean
sine, with the filter's 64-sample delay removed.

| | RMS error vs clean signal | SNR |
|---|---|---|
| Before filtering | 0.3174 | 7.0 dB |
| After filtering | 0.0449 | 24.0 dB |
| **Improvement** | **7.1× smaller error** | **+17.0 dB** |

### 3.6 Filter quality: audio demo (`make demo`)

A 6-second synthesised melody (262–523 Hz notes plus harmonics), with broadband hiss and a
7 kHz whine added. It was cleaned with a 255-tap, 3.5 kHz low-pass filter on 4 threads.

| Metric | Value |
|---|---|
| Input | 288,000 samples, 48 kHz, 6.00 s |
| Processing time | 0.015 s (~400× faster than real time) |
| Filter delay | 127 samples (2.65 ms) |
| SNR before | 5.2 dB |
| SNR after | 17.2 dB |
| **SNR improvement** | **+12.1 dB** |
| Gain at 440 Hz (melody) | 0.0 dB (untouched) |
| Gain at 1 kHz | 0.0 dB (untouched) |
| Gain at 7 kHz (whine) | **−72.6 dB** |
| Total energy above 5 kHz | **−55.6 dB** |

The melody passes through unchanged, while the whine is attenuated by a factor of more
than 4,000 in amplitude.

## 4. Limitations and possible improvements

- **Producer bottleneck.** A single producer generates or copies all samples. Splitting
  generation across threads, or precomputing the signal, would raise the 129-tap speedup.
- **Allocation per chunk.** Each chunk is `malloc`ed and `free`d. A fixed pool of chunk
  buffers recycled through a second ring would remove that cost and suit firmware better.
- **Scalar inner loop.** `fir_apply` is a plain nested loop. SIMD (SSE/AVX/NEON), or
  FFT-based convolution for long filters, would give large single-thread gains.
- **Low-pass only.** High-pass, band-pass or notch filters (for example for 50/60 Hz mains
  hum) would need only a different `fir_design_*` function.
- **WAV support** is limited to 16-bit PCM input and mono output.

## 5. Reproducing these numbers

```bash
make test                       # correctness (section 3.1)
make bench                      # thread scaling (3.2) -> results/bench.csv
make demo                       # audio demo (3.6)
make plot                       # graphs -> results/signal.png, results/speedup.png
make tsan  && ./sigpipe_tsan  -t 8 -n 300000 -c 500 -r 1 -v
make debug && ./sigpipe_debug -t 4 -n 200000 -c 777 -v
valgrind --leak-check=full ./sigpipe -t 4 -n 100000 -v

# filter-length sweep (3.3)
for k in 31 129 255 511; do ./sigpipe -t 1 -n 4000000 -k $k; ./sigpipe -t 4 -n 4000000 -k $k; done
# chunk-size sweep (3.4)
for c in 256 1024 4096 16384 65536; do ./sigpipe -t 4 -n 4000000 -c $c; done
```

Python scripts need `numpy` and `matplotlib`.
