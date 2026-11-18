/* The T6W28, the Neo Geo Pocket's PSG: three square channels with separate
 * left/right volumes plus a 15-bit LFSR noise channel, and the two direct
 * DACs the sound CPU drives for PCM.  Register writes follow the SN76489
 * latch/data scheme through the two ports (the left port carries the left
 * volumes and the tone periods, the right one the right volumes and the
 * noise).  The chip runs at 3.072 MHz, half the colour pocket's CPU clock;
 * a period is stored shifted four bits left, exactly as the reference
 * model keeps it. */
#ifndef M0_TL
#define M0_TL
#endif
#include "ngp.h"
#include <string.h>

static const uint8_t volumes[16] = { 64, 50, 39, 31, 24, 19, 15, 12, 9, 7, 5, 4, 3, 2, 1, 0 };
static const uint16_t noise_periods[3] = { 0x100, 0x200, 0x400 };

void ngp_psg_reset(void) {
    memset(machine.sq_period, 0, sizeof machine.sq_period);
    memset(machine.sq_phase, 0, sizeof machine.sq_phase);
    memset(machine.sq_volume_left, 0, sizeof machine.sq_volume_left);
    memset(machine.sq_volume_right, 0, sizeof machine.sq_volume_right);
    machine.noise_period_select = 0;
    machine.noise_tap = 13;
    machine.noise_shifter = 0x40;
    machine.noise_phase = 0;
    machine.noise_period_extra = 0;
    machine.latch_left = machine.latch_right = 0;
    machine.dac_left = machine.dac_right = 0;
}

void ngp_psg_write_left(uint8_t v) {
    unsigned index;
    if (v & 0x80) machine.latch_left = v;
    index = (machine.latch_left >> 5) & 3;
    if (machine.latch_left & 0x10)
        machine.sq_volume_left[index] = volumes[v & 15];
    else if (index < 3) {
        if (v & 0x80) machine.sq_period[index] = (uint16_t)((machine.sq_period[index] & 0xFF00) | ((v << 4) & 0x00FF));
        else machine.sq_period[index] = (uint16_t)((machine.sq_period[index] & 0x00FF) | ((v << 8) & 0x3F00));
    }
}

void ngp_psg_write_right(uint8_t v) {
    unsigned index;
    if (v & 0x80) machine.latch_right = v;
    index = (machine.latch_right >> 5) & 3;
    if (machine.latch_right & 0x10)
        machine.sq_volume_right[index] = volumes[v & 15];
    else if (index == 2) {
        if (v & 0x80) machine.noise_period_extra = (uint16_t)((machine.noise_period_extra & 0xFF00) | ((v << 4) & 0x00FF));
        else machine.noise_period_extra = (uint16_t)((machine.noise_period_extra & 0x00FF) | ((v << 8) & 0x3F00));
    }
    else if (index == 3) {
        const unsigned select = v & 3;
        machine.noise_period_select = (uint8_t)(select < 3 ? select : 3);
        machine.noise_tap = (v & 0x04) ? 13 : 16;
        machine.noise_shifter = 0x40;
        machine.noise_phase = 0;
    }
}

uint8_t ngp_psg_read_left(void)  { return machine.latch_left; }
uint8_t ngp_psg_read_right(void) { return machine.latch_right; }

void ngp_dac_write_left(uint8_t v)  { machine.dac_left = v; }
void ngp_dac_write_right(uint8_t v) { machine.dac_right = v; }

/* the noise LFSR's next shift */
static uint16_t noise_feedback(uint16_t shifter, unsigned tap) {
    /* bit 14 <- bit 0 ^ bit (14 - tap); tap 16 (disabled) leaves only bit 0 */
    const unsigned a = (shifter >> 0) & 1;
    const unsigned b = tap < 15 ? (shifter >> (14 - tap)) & 1 : 0;
    return (uint16_t)(((a ^ b) << 14) | (shifter >> 1));
}

unsigned ngp_audio(int16_t *out, unsigned frames, unsigned rate) {
    /* per output frame the chip advances 3072000/rate chip ticks; the
     * squares toggle every stored period, the noise shifts every period*2 */
    const uint32_t dt = 3072000u / (rate ? rate : 44100);
    uint32_t sq_acc[3] = { 0, 0, 0 }, noise_acc = 0;
    for (unsigned f = 0; f < frames; f++) {
        int32_t psg = 0;
        for (unsigned ch = 0; ch < 3; ch++) {
            const uint16_t period = machine.sq_period[ch];
            if (period > 128) {
                sq_acc[ch] += dt;
                while (sq_acc[ch] >= period) {
                    sq_acc[ch] -= period;
                    machine.sq_phase[ch] ^= 1;
                }
                psg += machine.sq_phase[ch] ? machine.sq_volume_left[ch] : -(int32_t)machine.sq_volume_left[ch];
                psg += machine.sq_phase[ch] ? machine.sq_volume_right[ch] : -(int32_t)machine.sq_volume_right[ch];
            }
        }
        {
            uint16_t period = machine.noise_period_select < 3 ? noise_periods[machine.noise_period_select] : machine.noise_period_extra;
            period = (uint16_t)(period * 2);
            if (!period) period = 16;
            noise_acc += dt;
            while (noise_acc >= period) {
                noise_acc -= period;
                if (machine.noise_shifter & 1) machine.noise_phase ^= 1;
                machine.noise_shifter = noise_feedback(machine.noise_shifter, machine.noise_tap);
            }
            psg += machine.noise_phase ? machine.sq_volume_left[3] : -(int32_t)machine.sq_volume_left[3];
            psg += machine.noise_phase ? machine.sq_volume_right[3] : -(int32_t)machine.sq_volume_right[3];
        }
        {
            const int32_t dac_l = machine.dac_left - 128, dac_r = machine.dac_right - 128;
            const int32_t l = psg * 43 + dac_l * 115;
            const int32_t r = psg * 43 + dac_r * 115;
            out[f * 2] = (int16_t)(l < -32768 ? -32768 : l > 32767 ? 32767 : l);
            out[f * 2 + 1] = (int16_t)(r < -32768 ? -32768 : r > 32767 ? 32767 : r);
        }
    }
    return frames;
}
