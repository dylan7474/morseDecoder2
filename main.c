#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>

/*
 * Simple multi-channel Morse decoder
 *
 * The program expects pairs of arguments on the command line:
 *   ./sinewave_detector <wav_file> <tone_hz> [<wav_file> <tone_hz> ...]
 * Each pair describes a channel.  A dedicated thread is started for each
 * channel and the decoded text is printed with a channel identifier.
 *
 * The implementation is intentionally lightweight.  It focuses on the core
 * stages required for Morse decoding and mirrors the architecture used in
 * sample.c: normalisation, band limited detection and timing based
 * classification.  The audio front-end works on 16-bit PCM WAV files to keep
 * dependencies to a minimum.
 */

/* -------------------------- Morse lookup table -------------------------- */

typedef struct {
    const char *code;
    char        ch;
} MorseEntry;

static const MorseEntry MORSE_TABLE[] = {
    {".-", 'A'},    {"-...", 'B'},  {"-.-.", 'C'}, {"-..", 'D'},
    {".", 'E'},     {"..-.", 'F'},  {"--.", 'G'}, {"....", 'H'},
    {"..", 'I'},    {".---", 'J'},  {"-.-", 'K'}, {".-..", 'L'},
    {"--", 'M'},    {"-.", 'N'},    {"---", 'O'}, {".--.", 'P'},
    {"--.-", 'Q'},  {".-.", 'R'},   {"...", 'S'}, {"-", 'T'},
    {"..-", 'U'},   {"...-", 'V'},  {".--", 'W'}, {"-..-", 'X'},
    {"-.--", 'Y'},  {"--..", 'Z'},
    {".----", '1'}, {"..---", '2'}, {"...--", '3'}, {"....-", '4'},
    {".....", '5'}, {"-....", '6'}, {"--...", '7'}, {"---..", '8'},
    {"----.", '9'}, {"-----", '0'},
    {NULL, 0}
};

static char lookup_morse(const char *code)
{
    for (const MorseEntry *e = MORSE_TABLE; e->code; ++e) {
        if (strcmp(e->code, code) == 0)
            return e->ch;
    }
    return '?';
}

/* ----------------------------- WAV utilities ---------------------------- */

typedef struct {
    float  *samples;      /* normalised samples (-1.0 .. 1.0) */
    size_t  length;       /* number of samples */
    int     sample_rate;  /* samples per second */
} AudioData;

static int read_wav(const char *filename, AudioData *out)
{
    FILE *f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "Failed to open %s\n", filename);
        return -1;
    }

    /* Minimal WAV header parsing (PCM 16-bit mono) */
    char riff[4];
    uint32_t chunk_size;
    char wave[4];
    fread(riff, 1, 4, f);
    fread(&chunk_size, 4, 1, f);
    fread(wave, 1, 4, f);
    if (strncmp(riff, "RIFF", 4) || strncmp(wave, "WAVE", 4)) {
        fprintf(stderr, "%s is not a WAV file\n", filename);
        fclose(f);
        return -1;
    }

    char fmt[4];
    uint32_t fmt_size;
    fread(fmt, 1, 4, f);
    fread(&fmt_size, 4, 1, f);
    if (strncmp(fmt, "fmt ", 4) || fmt_size < 16) {
        fprintf(stderr, "Invalid fmt chunk in %s\n", filename);
        fclose(f);
        return -1;
    }

    uint16_t audio_format, num_channels, bits_per_sample;
    uint32_t sample_rate, byte_rate;
    uint16_t block_align;
    fread(&audio_format, 2, 1, f);
    fread(&num_channels, 2, 1, f);
    fread(&sample_rate, 4, 1, f);
    fread(&byte_rate, 4, 1, f);
    fread(&block_align, 2, 1, f);
    fread(&bits_per_sample, 2, 1, f);

    if (audio_format != 1 || num_channels != 1 || bits_per_sample != 16) {
        fprintf(stderr, "%s must be mono 16-bit PCM\n", filename);
        fclose(f);
        return -1;
    }

    /* Skip any remaining fmt bytes */
    if (fmt_size > 16)
        fseek(f, fmt_size - 16, SEEK_CUR);

    /* Read data chunk header */
    char data_hdr[4];
    uint32_t data_size;
    while (fread(data_hdr, 1, 4, f) == 4) {
        fread(&data_size, 4, 1, f);
        if (strncmp(data_hdr, "data", 4) == 0)
            break;
        /* Skip unknown chunk */
        fseek(f, data_size, SEEK_CUR);
    }
    if (strncmp(data_hdr, "data", 4) != 0) {
        fprintf(stderr, "No data chunk in %s\n", filename);
        fclose(f);
        return -1;
    }

    size_t sample_count = data_size / sizeof(int16_t);
    int16_t *tmp = malloc(data_size);
    if (!tmp) {
        fclose(f);
        return -1;
    }
    fread(tmp, sizeof(int16_t), sample_count, f);
    fclose(f);

    float *samples = malloc(sample_count * sizeof(float));
    if (!samples) {
        free(tmp);
        return -1;
    }

    float max = 0.0f;
    for (size_t i = 0; i < sample_count; ++i) {
        float s = (float)tmp[i];
        if (fabsf(s) > max) max = fabsf(s);
        samples[i] = s;
    }
    free(tmp);

    if (max == 0.0f) max = 1.0f;
    for (size_t i = 0; i < sample_count; ++i)
        samples[i] /= max; /* normalise to -1..1 */

    out->samples = samples;
    out->length = sample_count;
    out->sample_rate = (int)sample_rate;
    return 0;
}

static void free_audio(AudioData *a)
{
    free(a->samples);
    a->samples = NULL;
    a->length = 0;
    a->sample_rate = 0;
}

/* --------------------- Signal processing utilities ---------------------- */

/* Compute the Goertzel power at target frequency for blocks of size B. */
static void compute_envelope(const float *samples, size_t length,
                             int sample_rate, float freq, size_t B,
                             float *out)
{
    size_t blocks = length / B;
    float w = 2.0f * (float)M_PI * freq / (float)sample_rate;
    float coeff = 2.0f * cosf(w);
    for (size_t b = 0; b < blocks; ++b) {
        float s_prev = 0.0f, s_prev2 = 0.0f;
        const float *blk = samples + b * B;
        for (size_t i = 0; i < B; ++i) {
            float s = blk[i] + coeff * s_prev - s_prev2;
            s_prev2 = s_prev;
            s_prev = s;
        }
        float power = s_prev2 * s_prev2 + s_prev * s_prev - coeff * s_prev * s_prev2;
        out[b] = power;
    }

    /* normalise envelope */
    float max = 0.0f;
    for (size_t i = 0; i < blocks; ++i)
        if (out[i] > max) max = out[i];
    if (max == 0.0f) max = 1.0f;
    for (size_t i = 0; i < blocks; ++i)
        out[i] /= max;
}

/* Decode Morse from a normalised envelope (0..1). */
static void decode_envelope(const float *env, size_t blocks, float threshold,
                            char *out, size_t out_size)
{
    const int dot_units = 1;
    const int dash_units = 3;
    const int letter_gap_units = 3;
    const int word_gap_units = 7;

    int prev = env[0] > threshold;
    int count = 0;
    char symbol[16];
    int sym_len = 0;
    size_t out_pos = 0;

    for (size_t i = 0; i < blocks; ++i) {
        int cur = env[i] > threshold;
        if (cur == prev) {
            count++;
            continue;
        }

        if (prev) { /* tone */
            symbol[sym_len++] = (count < dash_units) ? '.' : '-';
        } else {     /* silence */
            if (count >= word_gap_units) {
                if (sym_len) {
                    symbol[sym_len] = '\0';
                    out[out_pos++] = lookup_morse(symbol);
                    sym_len = 0;
                }
                out[out_pos++] = ' ';
            } else if (count >= letter_gap_units) {
                symbol[sym_len] = '\0';
                out[out_pos++] = lookup_morse(symbol);
                sym_len = 0;
            }
        }

        prev = cur;
        count = 1;
    }

    /* Process final run */
    if (prev) {
        symbol[sym_len++] = (count < dash_units) ? '.' : '-';
    } else {
        if (count >= letter_gap_units) {
            symbol[sym_len] = '\0';
            out[out_pos++] = lookup_morse(symbol);
            sym_len = 0;
        }
    }

    if (sym_len) {
        symbol[sym_len] = '\0';
        out[out_pos++] = lookup_morse(symbol);
    }

    out[out_pos] = '\0';
}

/* --------------------------- Channel handling --------------------------- */

typedef struct {
    int   id;
    float freq;
    AudioData audio;
} ChannelData;

static void *channel_thread(void *arg)
{
    ChannelData *cd = arg;
    size_t block = 1024; /* processing block size */
    size_t blocks = cd->audio.length / block;
    if (blocks == 0) {
        printf("Channel %d: no data\n", cd->id);
        return NULL;
    }

    float *env = malloc(blocks * sizeof(float));
    if (!env) {
        fprintf(stderr, "Channel %d: allocation failed\n", cd->id);
        return NULL;
    }

    compute_envelope(cd->audio.samples, cd->audio.length,
                     cd->audio.sample_rate, cd->freq, block, env);

    char text[1024];
    decode_envelope(env, blocks, 0.5f, text, sizeof(text));

    printf("Channel %d: %s\n", cd->id, text);

    free(env);
    return NULL;
}

/* ------------------------------- main ---------------------------------- */

int main(int argc, char **argv)
{
    if (argc < 3 || ((argc - 1) % 2) != 0) {
        fprintf(stderr, "Usage: %s <wav> <freq> [<wav> <freq> ...]\n", argv[0]);
        return 1;
    }

    int channels = (argc - 1) / 2;
    pthread_t *threads = malloc(sizeof(pthread_t) * channels);
    ChannelData *cdata = malloc(sizeof(ChannelData) * channels);

    for (int i = 0; i < channels; ++i) {
        const char *file = argv[1 + i * 2];
        float freq = strtof(argv[2 + i * 2], NULL);
        cdata[i].id = i;
        cdata[i].freq = freq;
        if (read_wav(file, &cdata[i].audio) != 0) {
            fprintf(stderr, "Failed to read %s\n", file);
            return 1;
        }
        pthread_create(&threads[i], NULL, channel_thread, &cdata[i]);
    }

    for (int i = 0; i < channels; ++i) {
        pthread_join(threads[i], NULL);
        free_audio(&cdata[i].audio);
    }

    free(threads);
    free(cdata);
    return 0;
}

