/*
 * fir.h - Finite Impulse Response (FIR) filter.
 *
 * Each output sample is a weighted sum of the most recent input samples:
 *
 *     y[n] = h[0]*x[n] + h[1]*x[n-1] + ... + h[taps-1]*x[n-taps+1]
 *
 * h[] are the "coefficients" (or "taps"). With every h[k] = 1/taps this is
 * just a moving average. Here we design a proper low-pass filter instead:
 * it keeps slow changes (the real signal) and removes fast jitter (noise).
 */
#ifndef FIR_H
#define FIR_H

/* Fill h[0..taps-1] with a windowed-sinc low-pass filter.
 * cutoff = cutoff frequency as a fraction of the sample rate (0 < cutoff < 0.5). */
void fir_design_lowpass(float *h, int taps, double cutoff);

/*
 * Filter 'len' samples.
 * 'x' must point at the first NEW sample, with (taps-1) valid HISTORY samples
 * directly before it in memory (x[-1], x[-2], ...). That history is what lets
 * chunks be filtered independently and still match a single long run.
 */
void fir_apply(const float *x, float *y, int len, const float *h, int taps);

#endif
