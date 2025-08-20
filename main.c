#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <signal.h>
#include <SDL2/SDL.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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

/* ------------------------- Goertzel computation ------------------------- */
static float goertzel_power(const float *samples, size_t length,
                            int sample_rate, float freq)
{
    float w = 2.0f * (float)M_PI * freq / (float)sample_rate;
    float coeff = 2.0f * cosf(w);
    float s_prev = 0.0f, s_prev2 = 0.0f;
    for (size_t i = 0; i < length; ++i) {
        float s = samples[i] + coeff * s_prev - s_prev2;
        s_prev2 = s_prev;
        s_prev = s;
    }
    return s_prev2 * s_prev2 + s_prev * s_prev - coeff * s_prev * s_prev2;
}

/* ------------------------ Real-time channel state ----------------------- */
typedef struct {
    int   id;
    float freq;
    int   sample_rate;
    float threshold;
    float max_power;
    int   prev;
    int   count;
    char  symbol[16];
    int   sym_len;
} ChannelState;

static void channel_init(ChannelState *c, int id, float freq, int sample_rate)
{
    c->id = id;
    c->freq = freq;
    c->sample_rate = sample_rate;
    c->threshold = 0.5f;
    c->max_power = 1e-9f;
    c->prev = 0;
    c->count = 0;
    c->sym_len = 0;
}

static void channel_process(ChannelState *c, const float *samples, size_t len)
{
    float p = goertzel_power(samples, len, c->sample_rate, c->freq);
    if (p > c->max_power)
        c->max_power = p;
    float env = p / c->max_power;
    int cur = env > c->threshold;

    if (c->count == 0) {
        c->prev = cur;
        c->count = 1;
        return;
    }

    if (cur == c->prev) {
        c->count++;
        return;
    }

    const int dash_units = 3;
    const int letter_gap_units = 3;
    const int word_gap_units = 7;

    if (c->prev) {
        c->symbol[c->sym_len++] = (c->count < dash_units) ? '.' : '-';
        printf("Channel %d symbol: %c\n", c->id, c->symbol[c->sym_len - 1]);
    } else {
        if (c->count >= word_gap_units) {
            if (c->sym_len) {
                c->symbol[c->sym_len] = '\0';
                char ch = lookup_morse(c->symbol);
                printf("Channel %d: %c\n", c->id, ch);
                c->sym_len = 0;
            }
            printf("Channel %d: [space]\n", c->id);
        } else if (c->count >= letter_gap_units) {
            if (c->sym_len) {
                c->symbol[c->sym_len] = '\0';
                char ch = lookup_morse(c->symbol);
                printf("Channel %d: %c\n", c->id, ch);
                c->sym_len = 0;
            }
            printf("Channel %d: [space]\n", c->id);
        }
    }

    c->prev = cur;
    c->count = 1;
}

/* --------------------------- Signal handling ---------------------------- */
static volatile int keep_running = 1;

static void handle_sigint(int sig)
{
    (void)sig;
    keep_running = 0;
}

/* -------------------------------- main --------------------------------- */
int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <freq> [<freq> ...]\n", argv[0]);
        return 1;
    }

    int channel_count = argc - 1;
    int sample_rate = 44100;
    size_t block = 1024;

    ChannelState *channels = malloc(sizeof(ChannelState) * channel_count);
    if (!channels) {
        fprintf(stderr, "Allocation failed\n");
        return 1;
    }
    for (int i = 0; i < channel_count; ++i) {
        float f = strtof(argv[i + 1], NULL);
        channel_init(&channels[i], i, f, sample_rate);
    }

    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        free(channels);
        return 1;
    }

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = sample_rate;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = (Uint16)block;
    want.callback = NULL;

    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 1, &want, &have, 0);
    if (!dev) {
        fprintf(stderr, "Failed to open audio device: %s\n", SDL_GetError());
        SDL_Quit();
        free(channels);
        return 1;
    }

    SDL_PauseAudioDevice(dev, 0);
    signal(SIGINT, handle_sigint);

    size_t bytes_per_sample = SDL_AUDIO_BITSIZE(have.format) / 8;
    int16_t *ibuf = malloc(block * bytes_per_sample);
    float *fbuf = malloc(block * sizeof(float));
    if (!ibuf || !fbuf) {
        fprintf(stderr, "Buffer allocation failed\n");
        SDL_CloseAudioDevice(dev);
        SDL_Quit();
        free(channels);
        free(ibuf);
        free(fbuf);
        return 1;
    }

    while (keep_running) {
        if (SDL_GetQueuedAudioSize(dev) >= block * bytes_per_sample) {
            SDL_DequeueAudio(dev, ibuf, block * bytes_per_sample);
            for (size_t i = 0; i < block; ++i)
                fbuf[i] = (float)ibuf[i] / 32768.0f;
            for (int c = 0; c < channel_count; ++c)
                channel_process(&channels[c], fbuf, block);
        } else {
            SDL_Delay(10);
        }
    }

    SDL_CloseAudioDevice(dev);
    SDL_Quit();
    free(channels);
    free(ibuf);
    free(fbuf);
    return 0;
}

