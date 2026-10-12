#include "wav.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* WAV is little-endian. Assemble bytes by hand so this works on any CPU. */
static uint32_t le32(const unsigned char *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }
static uint16_t le16(const unsigned char *b) { return (uint16_t)(b[0] | b[1] << 8); }

static void put32(unsigned char *b, uint32_t v) { b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24; }
static void put16(unsigned char *b, uint16_t v) { b[0] = v; b[1] = v >> 8; }

int wav_read(const char *path, float **samples, long *count, int *rate)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }

    unsigned char hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        fprintf(stderr, "%s: not a WAV file\n", path);
        fclose(f);
        return -1;
    }

    int channels = 0, bits = 0, format = 0;
    *rate = 0;

    /* A WAV is a list of chunks: [4-byte id][4-byte size][data].
     * We need "fmt " (the format) and "data" (the samples); skip anything else. */
    for (;;) {
        unsigned char ch[8];
        if (fread(ch, 1, 8, f) != 8) {
            fprintf(stderr, "%s: no data chunk found\n", path);
            fclose(f);
            return -1;
        }
        uint32_t size = le32(ch + 4);

        if (!memcmp(ch, "fmt ", 4)) {
            unsigned char fmt[16];
            if (size < 16 || fread(fmt, 1, 16, f) != 16) break;
            format   = le16(fmt);          /* 1 = PCM, 0xFFFE = extensible */
            channels = le16(fmt + 2);
            *rate    = (int)le32(fmt + 4);
            bits     = le16(fmt + 14);
            fseek(f, (long)(size - 16 + (size & 1)), SEEK_CUR);
        } else if (!memcmp(ch, "data", 4)) {
            if ((format != 1 && format != 0xFFFE) || bits != 16 || channels < 1) {
                fprintf(stderr, "%s: only 16-bit PCM WAV is supported "
                                "(got format %d, %d-bit). Convert with:\n"
                                "  ffmpeg -i in.wav -ac 1 -c:a pcm_s16le out.wav\n",
                        path, format, bits);
                fclose(f);
                return -1;
            }
            long frames = (long)(size / (2u * channels));
            int16_t *raw = malloc((size_t)frames * channels * sizeof *raw);
            float *out = malloc((size_t)frames * sizeof *out);
            if (!raw || !out) { free(raw); free(out); fclose(f); return -1; }

            frames = (long)fread(raw, 2 * channels, frames, f);   /* tolerate truncated files */
            for (long i = 0; i < frames; i++) {
                long sum = 0;
                for (int c = 0; c < channels; c++) {
                    const unsigned char *b = (const unsigned char *)&raw[i * channels + c];
                    sum += (int16_t)le16(b);
                }
                out[i] = (float)sum / channels / 32768.0f;        /* mix to mono, scale to [-1, 1) */
            }
            free(raw);
            fclose(f);
            *samples = out;
            *count = frames;
            return 0;
        } else {
            fseek(f, (long)(size + (size & 1)), SEEK_CUR);         /* chunks are padded to even size */
        }
    }
    fprintf(stderr, "%s: malformed fmt chunk\n", path);
    fclose(f);
    return -1;
}

int wav_write(const char *path, const float *samples, long count, int rate)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }

    uint32_t data_bytes = (uint32_t)count * 2;
    unsigned char h[44];
    memcpy(h, "RIFF", 4);      put32(h + 4, 36 + data_bytes);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4); put32(h + 16, 16);
    put16(h + 20, 1);          /* PCM            */
    put16(h + 22, 1);          /* mono           */
    put32(h + 24, (uint32_t)rate);
    put32(h + 28, (uint32_t)rate * 2);   /* bytes per second */
    put16(h + 32, 2);          /* bytes per frame */
    put16(h + 34, 16);         /* bits per sample */
    memcpy(h + 36, "data", 4); put32(h + 40, data_bytes);
    fwrite(h, 1, 44, f);

    for (long i = 0; i < count; i++) {
        float v = samples[i];
        if (v > 1.0f) v = 1.0f;                          /* clip instead of wrapping around */
        if (v < -1.0f) v = -1.0f;
        int s = (int)(v * 32767.0f);
        unsigned char b[2];
        put16(b, (uint16_t)(int16_t)s);
        fwrite(b, 1, 2, f);
    }
    fclose(f);
    return 0;
}
