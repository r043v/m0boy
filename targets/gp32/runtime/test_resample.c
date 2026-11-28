/* The rate conversion must carry a tone through any number of blocks without a click. */
#include "gp32_resample.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    static Gp32Resample resample;
    static int16_t out[2 * 60000];
    const double pi = 3.14159265358979323846;
    const unsigned from = 22070, to = 48000;
    const double tones[] = {440, 1000, 3000};
    for (unsigned t = 0; t < 3; ++t) {
        gp32_resample_init(&resample, from, to);
        unsigned count = 0;
        for (unsigned i = 0; i < from; ++i) {
            int16_t in = (int16_t)lrint(12000 * sin(2 * pi * tones[t] * i / from));
            count += gp32_resample_frame(&resample, in, (int16_t)-in, out + 2 * count);
        }
        assert(count >= to - 1 && count <= to + 2);
        /* The filter delays by half its length less one input frame. */
        double delay = (double)(GP32_RESAMPLE_TAPS / 2) / from, worst = 0;
        for (unsigned i = 100; i < count; ++i) {
            double want = 12000 * sin(2 * pi * tones[t] * ((double)i / to - delay));
            double error = fabs(out[2 * i] - want);
            if (error > worst)
                worst = error;
            assert(abs(out[2 * i] + out[2 * i + 1]) <= 1);
        }
        printf("%.0f Hz: largest error %.1f of 12000\n", tones[t], worst);
        assert(worst < 120);
    }
    return 0;
}
