#include "fir.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void fir_design_lowpass(float *h, int taps, double cutoff)
{
    int mid = (taps - 1) / 2;
    double sum = 0.0;

    for (int k = 0; k < taps; k++) {
        int m = k - mid;
        /* ideal low-pass response: sinc */
        double sinc = (m == 0) ? 2.0 * cutoff
                               : sin(2.0 * M_PI * cutoff * m) / (M_PI * m);
        /* Hamming window: tapers the ends so the filter behaves well */
        double window = 0.54 - 0.46 * cos(2.0 * M_PI * k / (taps - 1));
        h[k] = (float)(sinc * window);
        sum += h[k];
    }

    /* normalise so a constant input passes through unchanged (gain = 1) */
    for (int k = 0; k < taps; k++)
        h[k] = (float)(h[k] / sum);
}

void fir_apply(const float *x, float *y, int len, const float *h, int taps)
{
    for (int n = 0; n < len; n++) {
        float acc = 0.0f;
        for (int k = 0; k < taps; k++)
            acc += h[k] * x[n - k];      /* reaches back into history for small n */
        y[n] = acc;
    }
}
