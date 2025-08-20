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
    float avg_power;
    float on_threshold;
    float off_threshold;
    int   prev;
    int   count;
    char  symbol[16];
    int   sym_len;
    float dit;
} ChannelState;

static void channel_init(ChannelState *c, int id, float freq, int sample_rate)
{
    c->id = id;
    c->freq = freq;
    c->sample_rate = sample_rate;
    c->avg_power = 0.0f;
    c->on_threshold = 1.8f;
    c->off_threshold = 1.2f;
    c->prev = 0;
    c->count = 0;
    c->sym_len = 0;
    c->dit = 1.2f / 15.0f; /* start at 15 WPM */
}

static void channel_process(ChannelState *c, const float *samples, size_t len)
{
    const float ALPHA = 0.01f;
    float p = goertzel_power(samples, len, c->sample_rate, c->freq);
    if (c->avg_power == 0.0f)
        c->avg_power = p;
    else
        c->avg_power = (1.0f - ALPHA) * c->avg_power + ALPHA * p;

    float ratio = (c->avg_power > 0.0f) ? p / c->avg_power : 0.0f;
    int cur = c->prev;
    if (ratio > c->on_threshold)
        cur = 1;
    else if (ratio < c->off_threshold)
        cur = 0;

    if (c->count == 0) {
        c->prev = cur;
        c->count = 1;
        return;
    }

    if (cur == c->prev) {
        c->count++;
        return;
    }

    float block_time = (float)len / (float)c->sample_rate;
    float duration = c->count * block_time;

    if (c->prev) {
        const float DIT_ALPHA = 0.2f;
        if (duration < c->dit * 2.0f) {
            c->symbol[c->sym_len++] = '.';
            c->dit = (1.0f - DIT_ALPHA) * c->dit + DIT_ALPHA * duration;
        } else {
            c->symbol[c->sym_len++] = '-';
        }
        printf("Channel %d symbol: %c\n", c->id, c->symbol[c->sym_len - 1]);
    } else {
        if (duration >= c->dit * 7.0f) {
            if (c->sym_len) {
                c->symbol[c->sym_len] = '\0';
                char ch = lookup_morse(c->symbol);
                printf("Channel %d: %c\n", c->id, ch);
                c->sym_len = 0;
            }
            printf("Channel %d: [space]\n", c->id);
        } else if (duration >= c->dit * 3.0f) {
            if (c->sym_len) {
                c->symbol[c->sym_len] = '\0';
                char ch = lookup_morse(c->symbol);
                printf("Channel %d: %c\n", c->id, ch);
                c->sym_len = 0;
            }
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

    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        free(channels);
        return 1;
    }

    /* create small window to receive keyboard events */
    SDL_Window *win = SDL_CreateWindow("morsed", SDL_WINDOWPOS_UNDEFINED,
                                      SDL_WINDOWPOS_UNDEFINED, 200, 100, 0);
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
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

    SDL_AudioDeviceID in_dev = SDL_OpenAudioDevice(NULL, 1, &want, &have, 0);
    if (!in_dev) {
        fprintf(stderr, "Failed to open capture device: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        free(channels);
        return 1;
    }

    SDL_AudioDeviceID out_dev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (!out_dev) {
        fprintf(stderr, "Failed to open playback device: %s\n", SDL_GetError());
        SDL_CloseAudioDevice(in_dev);
        SDL_DestroyWindow(win);
        SDL_Quit();
        free(channels);
        return 1;
    }

    SDL_PauseAudioDevice(in_dev, 0);
    SDL_PauseAudioDevice(out_dev, 0);
    signal(SIGINT, handle_sigint);

    size_t bytes_per_sample = SDL_AUDIO_BITSIZE(have.format) / 8;
    int16_t *ibuf = malloc(block * bytes_per_sample);
    float *fbuf = malloc(block * sizeof(float));
    if (!ibuf || !fbuf) {
        fprintf(stderr, "Buffer allocation failed\n");
        SDL_CloseAudioDevice(in_dev);
        SDL_Quit();
        free(channels);
        free(ibuf);
        free(fbuf);
        return 1;
    }

    bool key_down = false;
    float phase = 0.0f;
    float test_freq = channels[0].freq; /* use first channel for test tone */
    Uint32 block_ms = (Uint32)((block * 1000) / sample_rate);

    while (keep_running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT ||
                (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)) {
                keep_running = 0;
            } else if (e.type == SDL_KEYDOWN) {
                SDL_Keycode k = e.key.keysym.sym;
                if (k == SDLK_PERIOD || k == SDLK_COMMA ||
                    k == SDLK_KP_PERIOD || k == SDLK_SPACE) {
                    key_down = true;
                }
            } else if (e.type == SDL_KEYUP) {
                SDL_Keycode k = e.key.keysym.sym;
                if (k == SDLK_PERIOD || k == SDLK_COMMA ||
                    k == SDLK_KP_PERIOD || k == SDLK_SPACE) {
                    key_down = false;
                }
            }
        }

        if (key_down) {
            SDL_ClearQueuedAudio(in_dev);
            for (size_t i = 0; i < block; ++i) {
                float sample = sinf(phase);
                phase += 2.0f * (float)M_PI * test_freq / (float)sample_rate;
                if (phase > 2.0f * (float)M_PI)
                    phase -= 2.0f * (float)M_PI;
                fbuf[i] = sample;
                ibuf[i] = (int16_t)(sample * 32767.0f);
            }
            SDL_QueueAudio(out_dev, ibuf, block * bytes_per_sample);
            for (int c = 0; c < channel_count; ++c)
                channel_process(&channels[c], fbuf, block);
            SDL_Delay(block_ms);
        } else if (SDL_GetQueuedAudioSize(in_dev) >= block * bytes_per_sample) {
            SDL_DequeueAudio(in_dev, ibuf, block * bytes_per_sample);
            for (size_t i = 0; i < block; ++i)
                fbuf[i] = (float)ibuf[i] / 32768.0f;
            for (int c = 0; c < channel_count; ++c)
                channel_process(&channels[c], fbuf, block);
        } else {
            SDL_Delay(10);
        }
    }

    SDL_CloseAudioDevice(in_dev);
    SDL_CloseAudioDevice(out_dev);
    SDL_DestroyWindow(win);
    SDL_Quit();
    free(channels);
    free(ibuf);
    free(fbuf);
    return 0;
}

