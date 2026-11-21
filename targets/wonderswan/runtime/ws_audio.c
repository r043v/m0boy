#include "ws.h"

static unsigned frequency(const WsAudio *audio, unsigned channel)
{
    return audio->io[channel * 2] | (audio->io[channel * 2 + 1] << 8);
}

static void advance(WsAudio *audio, unsigned clocks)
{
    static const uint8_t taps[8] = {14, 10, 13, 4, 8, 6, 9, 11};
    unsigned control = audio->io[0x10];
    for (unsigned ch = 0; ch < 4; ++ch) {
        if (!(control & (1u << ch)))
            continue;
        unsigned period = 2048 - frequency(audio, ch);
        unsigned elapsed = audio->phase[ch] + clocks;
        if (elapsed < period) {
            audio->phase[ch] = elapsed;
            continue;
        }
        unsigned steps = elapsed / period;
        audio->phase[ch] = elapsed - steps * period;
        audio->index[ch] = (audio->index[ch] + steps) & 31;
        if (ch == 3 && (audio->io[0x0e] & 16) && !(audio->io[0x15] & 12)) {
            unsigned tap = taps[audio->io[0x0e] & 7];
            while (steps--) {
                unsigned bit = 1 ^ (((audio->noise >> 7) ^ (audio->noise >> tap)) & 1);
                audio->noise = ((audio->noise << 1) | bit) & 0x7fff;
            }
        }
    }
}

static void mix(const Ws *ws, unsigned *left, unsigned *right)
{
    const WsAudio *audio = &ws->audio;
    unsigned control = audio->io[0x10];
    unsigned base = audio->io[0x0f] * 64;
    unsigned test = audio->io[0x15];
    *left = *right = 0;
    for (unsigned ch = 0; ch < 4; ++ch) {
        unsigned volume = audio->io[8 + ch];
        if (ch == 1 && (control & 0x20)) {
            unsigned voice = audio->io[0x14];
            *left += (voice & 4) ? volume : (voice & 8) ? volume / 2 : 0;
            *right += (voice & 1) ? volume : (voice & 2) ? volume / 2 : 0;
        } else if (control & (1u << ch)) {
            unsigned index = audio->index[ch];
            unsigned wave = (test >> 6) ? test >> 6 : ch;
            unsigned packed = ws->ram[base + wave * 16 + index / 2];
            unsigned sample = (packed >> ((index & 1) * 4)) & 15;
            if (ch == 3 && (control & 0x80))
                sample = (audio->noise & 1) * 15;
            *left += sample * (volume >> 4);
            *right += sample * (volume & 15);
        }
    }
    if (test & 0x20)
        *left = *right = (audio->io[9] * 5u) & 1023;
}

static void sweep(WsAudio *audio)
{
    int amount = audio->io[0x0c];
    if (amount >= 128)
        amount -= 256;
    unsigned divisor = (frequency(audio, 2) + amount) & 2047;
    audio->io[4] = divisor;
    audio->io[5] = divisor >> 8;
}

#if WS_ENABLE_COLOR
static unsigned sound_dma_period(const WsAudio *audio)
{
    static const uint16_t periods[] = {768, 512, 256, 128};
    return periods[audio->dma_control & 3];
}

static void pcm_input(WsAudio *audio, uint8_t value, unsigned mode)
{
    if (mode == 0) {
        audio->pcm_input[audio->pcm_channel] = value;
        audio->pcm_channel ^= 1;
    } else {
        if (mode & 1)
            audio->pcm_input[0] = value;
        if (mode & 2)
            audio->pcm_input[1] = value;
    }
}

static void sound_dma_transfer(Ws *ws)
{
    WsAudio *audio = &ws->audio;
    unsigned control = audio->dma_control;
    audio->dma_clocks = sound_dma_period(audio);
    if (!(control & 4) && !audio->dma_length) {
        audio->dma_control &= 0x7f;
        return;
    }

    uint8_t value = 0;
    if (!(control & 4)) {
        value = ws_read(ws, audio->dma_source);
        audio->dma_source = (audio->dma_source + ((control & 0x40) ? -1 : 1)) & 0xfffff;
        if (--audio->dma_length == 0) {
            if (control & 8) {
                audio->dma_source = audio->dma_source_reload;
                audio->dma_length = audio->dma_length_reload;
            } else {
                audio->dma_control &= 0x7f;
            }
        }
    }

    if (control & 0x10)
        pcm_input(audio, value, audio->pcm_mode >> 5);
    else
        audio->io[9] = value;
}

static int16_t headphone_sample(unsigned psg, uint16_t pcm)
{
    int sample = (int)psg * 32 + (pcm >= 0x8000 ? (int)pcm - 0x10000 : pcm);
    if (sample > INT16_MAX)
        return INT16_MAX;
    if (sample < INT16_MIN)
        return INT16_MIN;
    return sample;
}

static void pcm_latch(WsAudio *audio)
{
    static const uint8_t dividers[] = {1, 2, 3, 4, 5, 6, 8, 12};
    if (++audio->pcm_clock < dividers[(audio->pcm_control >> 4) & 7])
        return;
    audio->pcm_clock = 0;
    unsigned scale = (audio->pcm_control >> 2) & 3;
    unsigned shift = audio->pcm_control & 3;
    for (unsigned channel = 0; channel < 2; ++channel) {
        int sample = audio->pcm_input[channel];
        if (scale == 1 || (scale >= 2 && sample >= 128))
            sample -= 256;
        audio->pcm_output[channel] = sample * (256 >> (scale == 3 ? 0 : shift));
    }
}

uint8_t ws_color_audio_read(const Ws *ws, unsigned port)
{
    if (!(ws->video_mode & 0x80))
        return 0xff;
    const WsAudio *audio = &ws->audio;
    if (port >= 0x4a && port <= 0x4c)
        return audio->dma_source >> (8 * (port - 0x4a));
    if (port >= 0x4e && port <= 0x50)
        return audio->dma_length >> (8 * (port - 0x4e));
    if (port == 0x52)
        return audio->dma_control;
    if (port == 0x6a)
        return audio->pcm_control;
    if (port == 0x6b)
        return audio->pcm_mode;
    return 0;
}

static void dma_register_write(uint32_t *reg, unsigned offset, uint8_t value)
{
    unsigned shift = 8 * offset;
    *reg = ((*reg & ~(255u << shift)) | ((unsigned)value << shift)) & 0xfffff;
}

void ws_color_audio_write(Ws *ws, unsigned port, uint8_t value)
{
    if (!(ws->video_mode & 0x80))
        return;
    WsAudio *audio = &ws->audio;
    if (port >= 0x4a && port <= 0x4c) {
        dma_register_write(&audio->dma_source, port - 0x4a, value);
        dma_register_write(&audio->dma_source_reload, port - 0x4a, value);
    } else if (port >= 0x4e && port <= 0x50) {
        dma_register_write(&audio->dma_length, port - 0x4e, value);
        dma_register_write(&audio->dma_length_reload, port - 0x4e, value);
    } else if (port == 0x52) {
        audio->dma_control = value & 0xdf;
        audio->dma_clocks = sound_dma_period(audio);
    } else if (port >= 0x64 && port <= 0x67) {
        uint16_t *sample = &audio->pcm_output[(port - 0x64) >> 1];
        unsigned shift = (port & 1) * 8;
        *sample = (*sample & ~(255u << shift)) | ((unsigned)value << shift);
    } else if (port == 0x69) {
        pcm_input(audio, value, 0);
    } else if (port == 0x6a) {
        audio->pcm_control = value;
    } else if (port == 0x6b) {
        audio->pcm_mode = value & 0x60;
        if (value & 0x10)
            audio->pcm_channel = 0;
    }
}
#endif

void ws_audio_clock(Ws *ws, unsigned clocks)
{
    WsAudio *audio = &ws->audio;
    while (clocks) {
        unsigned chunk = 128 - audio->sample_clock;
        if (chunk > clocks)
            chunk = clocks;
#if WS_ENABLE_COLOR
        bool dma_active = (ws->video_mode & 0x80) && (audio->dma_control & 0x80);
        if (dma_active && chunk > audio->dma_clocks)
            chunk = audio->dma_clocks;
#endif
        bool sweeping = (audio->io[0x10] & 0x44) == 0x44;
        unsigned sweep_period = (1u + audio->io[0x0d]) *
                                ((audio->io[0x15] & 2) ? 1u : 8192u);
        if (sweeping && chunk > sweep_period - audio->sweep_clock)
            chunk = sweep_period - audio->sweep_clock;
        if (audio->io[0x10] & 15)
            advance(audio, chunk);
        clocks -= chunk;
        if (sweeping) {
            audio->sweep_clock += chunk;
            if (audio->sweep_clock == sweep_period) {
                audio->sweep_clock = 0;
                sweep(audio);
            }
        }
#if WS_ENABLE_COLOR
        if (dma_active) {
            audio->dma_clocks -= chunk;
            if (!audio->dma_clocks)
                sound_dma_transfer(ws);
        }
#endif
        audio->sample_clock += chunk;
        if (audio->sample_clock == 128) {
            audio->sample_clock = 0;
#if WS_ENABLE_COLOR
            bool hyper_voice = (ws->video_mode & 0x80) && (audio->pcm_control & 0x80);
#endif
            if (audio->sink) {
                unsigned left, right;
                mix(ws, &left, &right);
                unsigned output = audio->io[0x11];
                // Speaker DAC wraps after its programmable right shift.
                unsigned speaker = ((left + right) >> ((output >> 1) & 3)) & 255;
                speaker = speaker * audio->io[0x1e] / 3;
                int16_t headphone_left = (output & 8) ? left * 32 : 0;
                int16_t headphone_right = (output & 8) ? right * 32 : 0;
#if WS_ENABLE_COLOR
                if ((output & 8) && hyper_voice) {
                    headphone_left = headphone_sample(left, audio->pcm_output[0]);
                    headphone_right = headphone_sample(right, audio->pcm_output[1]);
                }
#endif
                audio->sink(audio->context, (output & 1) ? speaker : 0,
                            headphone_left, headphone_right);
            }
#if WS_ENABLE_COLOR
            if (hyper_voice)
                pcm_latch(audio);
#endif
        }
    }
}

uint8_t ws_audio_read(const Ws *ws, unsigned reg)
{
    const WsAudio *audio = &ws->audio;
    if (reg == 0x12 || reg == 0x13)
        return audio->noise >> ((reg - 0x12) * 8);
    if (reg >= 0x16 && reg <= 0x1b) {
        unsigned left, right;
        mix(ws, &left, &right);
        unsigned value = reg < 0x18 ? right : reg < 0x1a ? left : left + right;
        return value >> ((reg & 1) * 8);
    }
    return audio->io[reg];
}

void ws_audio_write(Ws *ws, unsigned reg, uint8_t value)
{
    WsAudio *audio = &ws->audio;
    if (reg < 8 && (reg & 1))
        value &= 7;
    switch (reg) {
    case 0x0d:
        value &= 31;
        audio->sweep_clock = 0;
        break;
    case 0x0e:
        if (value & 8)
            audio->noise = 0;
        value &= 0x17;
        break;
    case 0x10: value &= 0xef; break;
    case 0x11: value &= 15; break;
    case 0x12: case 0x13: return;
    case 0x14: value &= 15; break;
    case 0x15: audio->sweep_clock = 0; break;
    case 0x1e: value &= 3; break;
    default:
        if (reg >= 0x16)
            return;
        break;
    }
    audio->io[reg] = value;
}
