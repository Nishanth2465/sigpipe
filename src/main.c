/*
 * sigpipe - concurrent signal-processing pipeline
 *
 *   [producer] --chunks--> [ring buffer] --> [N worker threads: FIR] --> [output]
 *
 * Input is either a synthetic noisy "sensor" signal (default) or a real
 * .wav recording (-i). The producer streams it in chunks; worker threads pull
 * chunks from the ring buffer and low-pass filter them in parallel. Each chunk
 * carries a sequence number, so results land in the right place even though
 * workers finish out of order.
 */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "fir.h"
#include "ring.h"
#include "wav.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---------------------------------------------------------------- config */
typedef struct {
    int threads;         /* worker threads                          */
    long samples;        /* total samples (synthetic mode)          */
    int chunk;           /* samples per chunk                       */
    int taps;            /* FIR length                              */
    int ring_cap;        /* ring buffer capacity (chunks)           */
    double cutoff_hz;    /* low-pass cutoff frequency               */
    int verify;          /* compare against single-threaded run     */
    const char *csv;     /* optional: dump first samples to CSV     */
    const char *in_wav;  /* optional: read samples from this WAV    */
    const char *out_wav; /* optional: write filtered result as WAV  */
} config_t;

/* One unit of work. data[] = (taps-1) history samples followed by 'len' new ones. */
typedef struct {
    long seq;            /* chunk index -> where the output goes    */
    int len;
    float data[];        /* flexible array member                   */
} chunk_t;

/* Everything the worker threads share. */
typedef struct {
    ring_t ring;
    const float *h;
    int taps;
    int chunk;
    float *out;          /* final output, written at out[seq * chunk] */
} shared_t;

/* ---------------------------------------------------------------- input sources */
/*
 * Where samples come from. The producer just asks for "the next len samples";
 * it doesn't care whether they're generated or read from a recording.
 */
typedef struct {
    const float *file;   /* WAV samples, or NULL for synthetic mode */
    int rate;            /* sample rate in Hz                       */
    unsigned rng;        /* random state for synthetic noise        */
} source_t;

/* Synthetic "sensor": 5 Hz sine + 2 kHz hum + random noise. */
static float synth_sample(long i, int rate, unsigned *rng)
{
    double t = i / (double)rate;
    double clean = sin(2.0 * M_PI * 5.0 * t);
    double hum   = 0.3 * sin(2.0 * M_PI * 2000.0 * t);
    *rng = *rng * 1103515245u + 12345u;                 /* tiny LCG, reproducible */
    double noise = ((*rng >> 8) / (double)(1u << 24) - 0.5) * 0.8;
    return (float)(clean + hum + noise);
}

static float clean_sample(long i, int rate)
{
    return (float)sin(2.0 * M_PI * 5.0 * (i / (double)rate));
}

static void source_init(source_t *src, const float *file, int rate)
{
    src->file = file;
    src->rate = rate;
    src->rng = 42;       /* same seed every time -> identical runs */
}

/* Copy the next 'len' samples, starting at sample index 'start', into dst. */
static void source_read(source_t *src, float *dst, long start, int len)
{
    if (src->file)
        memcpy(dst, src->file + start, len * sizeof(float));
    else
        for (int i = 0; i < len; i++)
            dst[i] = synth_sample(start + i, src->rate, &src->rng);
}

/* ---------------------------------------------------------------- worker */
static void *worker(void *arg)
{
    shared_t *s = arg;
    chunk_t *c;

    /* ring_pop returns NULL only when the producer is done and the ring is empty */
    while ((c = ring_pop(&s->ring)) != NULL) {
        const float *x = c->data + (s->taps - 1);       /* skip past history */
        float *y = s->out + c->seq * (long)s->chunk;
        fir_apply(x, y, c->len, s->h, s->taps);         /* the heavy work, outside any lock */
        free(c);
    }
    return NULL;
}

/* ---------------------------------------------------------------- helpers */
static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [-t threads] [-n samples] [-c chunk] [-k taps] [-r ring_cap]\n"
            "          [-f cutoff_hz] [-i in.wav] [-w out.wav] [-v] [-o out.csv]\n"
            "  -i  read a real recording (16-bit PCM WAV) instead of the synthetic signal\n"
            "  -w  write the filtered result as a WAV file\n"
            "  -f  low-pass cutoff in Hz (default 1000)\n"
            "  -v  verify output against a single-threaded reference\n"
            "  -o  write first 0.1 s of samples (noisy, filtered[, clean]) to CSV\n",
            prog);
}

static int parse_args(int argc, char **argv, config_t *cfg)
{
    *cfg = (config_t){ .threads = 4, .samples = 4000000, .chunk = 4096, .taps = 129,
                       .ring_cap = 16, .cutoff_hz = 1000.0 };
    int opt;
    while ((opt = getopt(argc, argv, "t:n:c:k:r:f:i:w:vo:h")) != -1) {
        switch (opt) {
        case 't': cfg->threads   = atoi(optarg); break;
        case 'n': cfg->samples   = atol(optarg); break;
        case 'c': cfg->chunk     = atoi(optarg); break;
        case 'k': cfg->taps      = atoi(optarg); break;
        case 'r': cfg->ring_cap  = atoi(optarg); break;
        case 'f': cfg->cutoff_hz = atof(optarg); break;
        case 'i': cfg->in_wav    = optarg;       break;
        case 'w': cfg->out_wav   = optarg;       break;
        case 'v': cfg->verify    = 1;            break;
        case 'o': cfg->csv       = optarg;       break;
        default:  usage(argv[0]); return -1;
        }
    }
    if (cfg->threads < 1 || cfg->samples < 1 || cfg->chunk < 1 ||
        cfg->taps < 3 || cfg->ring_cap < 1 || cfg->cutoff_hz <= 0) {
        usage(argv[0]);
        return -1;
    }
    return 0;
}

/* Single-threaded reference: same filter over the whole signal in one go. */
static float *reference_run(long n, const float *file, int rate, const float *h, int taps)
{
    int hist = taps - 1;
    float *in  = calloc(n + hist, sizeof *in);           /* leading zeros = history */
    float *out = malloc(n * sizeof *out);
    if (!in || !out) { free(in); free(out); return NULL; }

    source_t src;
    source_init(&src, file, rate);
    source_read(&src, in + hist, 0, (int)n);

    fir_apply(in + hist, out, (int)n, h, taps);
    free(in);
    return out;
}

static void write_csv(const char *path, long n_total, const float *file, int rate, const float *out)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return; }
    long n = rate / 10;                                   /* first 0.1 s */
    if (n > n_total) n = n_total;

    float *noisy = malloc(n * sizeof *noisy);
    if (!noisy) { fclose(f); return; }
    source_t src;
    source_init(&src, file, rate);
    source_read(&src, noisy, 0, (int)n);

    fprintf(f, file ? "i,noisy,filtered,rate\n" : "i,noisy,filtered,clean,rate\n");
    for (long i = 0; i < n; i++) {
        if (file)
            fprintf(f, "%ld,%f,%f,%d\n", i, noisy[i], out[i], rate);
        else
            fprintf(f, "%ld,%f,%f,%f,%d\n", i, noisy[i], out[i], clean_sample(i, rate), rate);
    }
    free(noisy);
    fclose(f);
}

/* ---------------------------------------------------------------- main */
int main(int argc, char **argv)
{
    config_t cfg;
    if (parse_args(argc, argv, &cfg) != 0)
        return 1;

    /* input: a real recording, or the synthetic sensor */
    float *file = NULL;
    int rate = 48000;
    if (cfg.in_wav) {
        if (wav_read(cfg.in_wav, &file, &cfg.samples, &rate) != 0)
            return 1;
        printf("input: %s (%ld samples, %d Hz, %.2f s)\n",
               cfg.in_wav, cfg.samples, rate, cfg.samples / (double)rate);
    }
    if (cfg.cutoff_hz >= rate / 2.0) {
        fprintf(stderr, "cutoff must be below half the sample rate (%d Hz)\n", rate / 2);
        return 1;
    }

    int hist = cfg.taps - 1;
    long nchunks = (cfg.samples + cfg.chunk - 1) / cfg.chunk;

    float *h = malloc(cfg.taps * sizeof *h);
    float *out = malloc(cfg.samples * sizeof *out);
    pthread_t *tids = malloc(cfg.threads * sizeof *tids);
    if (!h || !out || !tids) { fprintf(stderr, "out of memory\n"); return 1; }

    /* low-pass: keep frequencies below cutoff_hz, remove those above */
    fir_design_lowpass(h, cfg.taps, cfg.cutoff_hz / rate);

    shared_t s = { .h = h, .taps = cfg.taps, .chunk = cfg.chunk, .out = out };
    if (ring_init(&s.ring, cfg.ring_cap) != 0) { fprintf(stderr, "ring init failed\n"); return 1; }

    double t0 = now_sec();

    /* start the worker pool */
    for (int i = 0; i < cfg.threads; i++)
        pthread_create(&tids[i], NULL, worker, &s);

    /* producer (this thread): stream samples into chunks.
     * 'history' remembers the last (taps-1) samples of the previous chunk. */
    float *history = calloc(hist, sizeof *history);         /* zeros before t = 0 */
    source_t src;
    source_init(&src, file, rate);
    long next = 0;

    for (long seq = 0; seq < nchunks; seq++) {
        int len = (int)((cfg.samples - next) < cfg.chunk ? (cfg.samples - next) : cfg.chunk);
        chunk_t *c = malloc(sizeof *c + (hist + len) * sizeof(float));
        if (!c) { fprintf(stderr, "out of memory\n"); return 1; }
        c->seq = seq;
        c->len = len;

        memcpy(c->data, history, hist * sizeof(float));      /* overlap from previous chunk */
        source_read(&src, c->data + hist, next, len);        /* new samples */
        next += len;

        /* save the tail of this chunk (history + new samples) for the next one */
        memcpy(history, c->data + len, hist * sizeof(float));

        ring_push(&s.ring, c);                               /* blocks if workers fall behind */
    }

    ring_close(&s.ring);                                     /* tell workers we're done */
    for (int i = 0; i < cfg.threads; i++)
        pthread_join(tids[i], NULL);

    double elapsed = now_sec() - t0;

    printf("threads=%d samples=%ld chunk=%d taps=%d cutoff=%.0fHz time=%.3fs throughput=%.2f Msamples/s\n",
           cfg.threads, cfg.samples, cfg.chunk, cfg.taps, cfg.cutoff_hz, elapsed,
           cfg.samples / elapsed / 1e6);

    int status = 0;
    if (cfg.verify) {
        float *ref = reference_run(cfg.samples, file, rate, h, cfg.taps);
        if (!ref) { fprintf(stderr, "out of memory\n"); return 1; }
        long mismatches = 0;
        for (long i = 0; i < cfg.samples; i++)
            if (out[i] != ref[i])                            /* exact, bit-for-bit */
                mismatches++;
        printf("verify: %s (%ld mismatches)\n", mismatches ? "FAIL" : "OK", mismatches);
        status = mismatches ? 2 : 0;
        free(ref);
    }

    if (cfg.csv)
        write_csv(cfg.csv, cfg.samples, file, rate, out);

    if (cfg.out_wav && wav_write(cfg.out_wav, out, cfg.samples, rate) == 0)
        printf("wrote %s\n", cfg.out_wav);

    ring_destroy(&s.ring);
    free(history);
    free(tids);
    free(out);
    free(h);
    free(file);
    return status;
}
