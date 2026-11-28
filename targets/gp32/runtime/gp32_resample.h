/* Stereo rate conversion that keeps its phase from one block to the next.
   SDL 2 restarts its converter on every block put in a stream, which clicks
   at each boundary: the front end converts the game's rate itself. */
#ifndef M0_GP32_RESAMPLE_H
#define M0_GP32_RESAMPLE_H

#include <math.h>
#include <stdint.h>
#include <string.h>

enum { GP32_RESAMPLE_TAPS = 16, GP32_RESAMPLE_PHASES = 256 };

typedef struct {
    /* One filter per output position between two input frames, and one more
       for the position rounded up to the next frame. */
    float kernel[GP32_RESAMPLE_PHASES + 1][GP32_RESAMPLE_TAPS];
    /* The last input frames, written twice so that they read in one piece. */
    float history[2][2 * GP32_RESAMPLE_TAPS];
    unsigned next;
    uint64_t position, step;    /* in input frames, 32.32 fixed point */
} Gp32Resample;

static inline void gp32_resample_clear(Gp32Resample *r)
{
    memset(r->history, 0, sizeof(r->history));
    r->next = 0;
    r->position = 0;
}

static inline void gp32_resample_init(Gp32Resample *r, unsigned from, unsigned to)
{
    const double pi = 3.14159265358979323846;
    /* Cut below the lower of the two Nyquist frequencies. */
    double cut = 0.92 * (to < from ? (double)to / from : 1.0);
    for (unsigned phase = 0; phase <= GP32_RESAMPLE_PHASES; ++phase) {
        double sum = 0;
        for (unsigned tap = 0; tap < GP32_RESAMPLE_TAPS; ++tap) {
            double x = (double)tap - (GP32_RESAMPLE_TAPS / 2 - 1) -
                       (double)phase / GP32_RESAMPLE_PHASES;
            double sinc = x == 0 ? cut : sin(pi * cut * x) / (pi * x);
            double w = pi * x / (GP32_RESAMPLE_TAPS / 2);       /* Blackman */
            double value = sinc * (0.42 + 0.5 * cos(w) + 0.08 * cos(2 * w));
            r->kernel[phase][tap] = (float)value;
            sum += value;
        }
        for (unsigned tap = 0; tap < GP32_RESAMPLE_TAPS; ++tap)
            r->kernel[phase][tap] = (float)(r->kernel[phase][tap] / sum);
    }
    r->step = ((uint64_t)from << 32) / to;
    gp32_resample_clear(r);
}

static inline int16_t gp32_resample_clip(float value)
{
    return value >= 32767 ? 32767 : value <= -32768 ? -32768 : (int16_t)lrintf(value);
}

/* Takes one input frame and writes the output frames it completes.  `out` needs
   room for to / from + 1 frames.  Returns their number. */
static inline unsigned gp32_resample_frame(Gp32Resample *r, int16_t left, int16_t right,
                                           int16_t *out)
{
    const int16_t in[2] = {left, right};
    for (unsigned channel = 0; channel < 2; ++channel)
        r->history[channel][r->next] =
            r->history[channel][r->next + GP32_RESAMPLE_TAPS] = in[channel];
    r->next = (r->next + 1) % GP32_RESAMPLE_TAPS;

    unsigned count = 0;
    while (r->position < (UINT64_C(1) << 32)) {
        unsigned phase = (unsigned)((r->position * GP32_RESAMPLE_PHASES + (UINT64_C(1) << 31)) >> 32);
        for (unsigned channel = 0; channel < 2; ++channel) {
            float sum = 0;
            for (unsigned tap = 0; tap < GP32_RESAMPLE_TAPS; ++tap)
                sum += r->kernel[phase][tap] * r->history[channel][r->next + tap];
            out[2 * count + channel] = gp32_resample_clip(sum);
        }
        ++count;
        r->position += r->step;
    }
    r->position -= UINT64_C(1) << 32;
    return count;
}

#endif
