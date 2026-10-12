/*
 * wav.h - minimal WAV file reader/writer (16-bit PCM).
 *
 * A .wav file = a small header (sample rate, channels, bit depth)
 * followed by raw samples. 16-bit PCM stores each sample as a
 * signed integer from -32768 to 32767; we convert to float in [-1, 1).
 */
#ifndef WAV_H
#define WAV_H

/* Read a 16-bit PCM WAV. Stereo/multi-channel is mixed down to mono.
 * On success returns 0 and sets *samples (malloc'd, caller frees),
 * *count and *rate. Returns -1 on error (message printed to stderr). */
int wav_read(const char *path, float **samples, long *count, int *rate);

/* Write mono 16-bit PCM WAV. Values outside [-1, 1] are clipped. */
int wav_write(const char *path, const float *samples, long count, int rate);

#endif
