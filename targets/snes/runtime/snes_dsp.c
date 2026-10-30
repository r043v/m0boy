/* The S-DSP: eight BRR voices with the ADSR/GAIN envelopes, the pitch
 * counters and the volumes, rendering stereo into a ring the host drains
 * at 32000 Hz (one sample per 1024 SPC cycles, from snes_smp.c's clock).
 * The register file is what the SPC700 sees at $F2/$F3.
 *
 * The echo (its A-RAM buffer, the FIR, the feedback, the EDL delay and
 * EVOL), the noise source ($4E, the LFSR of $6C), pitch modulation (PMON,
 * the previous voice's output bent into the pitch) and the 4-point
 * gaussian interpolation follow the algorithms of the bsnes core; the
 * envelopes follow the documented shapes with the period table.  Not
 * modelled: the soft reset's exact clears, the EXTL/EXTR inputs, the
 * KON's five-sample delay.  The mute and reset bits of $6C are honoured. */
#include "snes.h"

#include <string.h>

#define ADSR_MAX 0x7FF

/* the envelope period table: the rates' samples per step, 0 = never */
static const uint16_t rate_period[32] = {
    0, 2048, 1536, 1280, 1024, 768, 640, 512,
    384, 320, 256, 192, 160, 128, 96, 80,
    64, 48, 40, 32, 24, 20, 16, 12,
    10, 8, 6, 5, 4, 3, 2, 1
};

/* the gaussian interpolation's table, the one every emulator replicates
 * from the S-SMP's reverse engineering (bsnes's gaussian_table) */
static const int16_t gauss_table[512] = {

     0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
     1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,    2,    2,    2,    2,    2,
     2,    2,    3,    3,    3,    3,    3,    4,    4,    4,    4,    4,    5,    5,    5,    5,
     6,    6,    6,    6,    7,    7,    7,    8,    8,    8,    9,    9,    9,   10,   10,   10,
    11,   11,   11,   12,   12,   13,   13,   14,   14,   15,   15,   15,   16,   16,   17,   17,
    18,   19,   19,   20,   20,   21,   21,   22,   23,   23,   24,   24,   25,   26,   27,   27,
    28,   29,   29,   30,   31,   32,   32,   33,   34,   35,   36,   36,   37,   38,   39,   40,
    41,   42,   43,   44,   45,   46,   47,   48,   49,   50,   51,   52,   53,   54,   55,   56,
    58,   59,   60,   61,   62,   64,   65,   66,   67,   69,   70,   71,   73,   74,   76,   77,
    78,   80,   81,   83,   84,   86,   87,   89,   90,   92,   94,   95,   97,   99,  100,  102,
   104,  106,  107,  109,  111,  113,  115,  117,  118,  120,  122,  124,  126,  128,  130,  132,
   134,  137,  139,  141,  143,  145,  147,  150,  152,  154,  156,  159,  161,  163,  166,  168,
   171,  173,  175,  178,  180,  183,  186,  188,  191,  193,  196,  199,  201,  204,  207,  210,
   212,  215,  218,  221,  224,  227,  230,  233,  236,  239,  242,  245,  248,  251,  254,  257,
   260,  263,  267,  270,  273,  276,  280,  283,  286,  290,  293,  297,  300,  304,  307,  311,
   314,  318,  321,  325,  328,  332,  336,  339,  343,  347,  351,  354,  358,  362,  366,  370,
   374,  378,  381,  385,  389,  393,  397,  401,  405,  410,  414,  418,  422,  426,  430,  434,
   439,  443,  447,  451,  456,  460,  464,  469,  473,  477,  482,  486,  491,  495,  499,  504,
   508,  513,  517,  522,  527,  531,  536,  540,  545,  550,  554,  559,  563,  568,  573,  577,
   582,  587,  592,  596,  601,  606,  611,  615,  620,  625,  630,  635,  640,  644,  649,  654,
   659,  664,  669,  674,  678,  683,  688,  693,  698,  703,  708,  713,  718,  723,  728,  732,
   737,  742,  747,  752,  757,  762,  767,  772,  777,  782,  787,  792,  797,  802,  806,  811,
   816,  821,  826,  831,  836,  841,  846,  851,  855,  860,  865,  870,  875,  880,  884,  889,
   894,  899,  904,  908,  913,  918,  923,  927,  932,  937,  941,  946,  951,  955,  960,  965,
   969,  974,  978,  983,  988,  992,  997, 1001, 1005, 1010, 1014, 1019, 1023, 1027, 1032, 1036,
  1040, 1045, 1049, 1053, 1057, 1061, 1066, 1070, 1074, 1078, 1082, 1086, 1090, 1094, 1098, 1102,
  1106, 1109, 1113, 1117, 1121, 1125, 1128, 1132, 1136, 1139, 1143, 1146, 1150, 1153, 1157, 1160,
  1164, 1167, 1170, 1174, 1177, 1180, 1183, 1186, 1190, 1193, 1196, 1199, 1202, 1205, 1207, 1210,
  1213, 1216, 1219, 1221, 1224, 1227, 1229, 1232, 1234, 1237, 1239, 1241, 1244, 1246, 1248, 1251,
  1253, 1255, 1257, 1259, 1261, 1263, 1265, 1267, 1269, 1270, 1272, 1274, 1275, 1277, 1279, 1280,
  1282, 1283, 1284, 1286, 1287, 1288, 1290, 1291, 1292, 1293, 1294, 1295, 1296, 1297, 1297, 1298,
  1299, 1300, 1300, 1301, 1302, 1302, 1303, 1303, 1303, 1304, 1304, 1304, 1304, 1304, 1305, 1305,
};

/* the register file's voice base */
#define VREG(v, r) machine.dsp.regs[(v) * 0x10 + (r)]
#define GREG(r) machine.dsp.regs[r]

void snes_dsp_write(uint8_t at, uint8_t v) {
    snes_dsp_t *d = &machine.dsp;
    d->regs[at & 0x7F] = v;
    switch (at & 0x7F) {
    case 0x4C: d->key_on |= v; break;  /* KON: the voices restart at the next sample */
    case 0x5C: d->key_off = v; break;  /* KOF: the voices release */
    default: break;
    }
}

uint8_t snes_dsp_read(uint8_t at) {
    snes_dsp_t *d = &machine.dsp;
    at &= 0x7F;
    if ((at & 0xF) == 0x8) { /* ENVX: the voice's current envelope */
        const unsigned v = at >> 4;
        return (uint8_t)(d->voice[v].env >> 4);
    }
    if ((at & 0xF) == 0x9) /* ENDX: the block-end flag of the voice */
        return (uint8_t)(((d->end_flags >> (at >> 4)) & 1) << 7);
    return d->regs[at];
}

/* the voice's sample source: the directory entry's two block addresses */
static void voice_load_source(unsigned v) {
    const unsigned dir = GREG(0x5D) * 0x100u;
    const unsigned src = VREG(v, 4);
    const uint16_t start = (uint16_t)(machine.smp.ram[(dir + src * 4) & 0xFFFF] |
                                      machine.smp.ram[(dir + src * 4 + 1) & 0xFFFF] << 8);
    const uint16_t loop = (uint16_t)(machine.smp.ram[(dir + src * 4 + 2) & 0xFFFF] |
                                     machine.smp.ram[(dir + src * 4 + 3) & 0xFFFF] << 8);
    machine.dsp.voice[v].start = (uint16_t)(start * 8u);
    machine.dsp.voice[v].loop = (uint16_t)(loop * 8u);
}

static int16_t voice_next_sample(unsigned v);

/* start a voice: the BRR reader at its start block, the envelope at the
 * attack or the direct gain */
static void voice_start(unsigned v) {
    snes_dsp_t *d = &machine.dsp;
    voice_load_source(v);
    d->voice[v].src = d->voice[v].start;
    d->voice[v].hdr = machine.smp.ram[d->voice[v].src];
    d->voice[v].nib = 0;
    d->voice[v].pos = 0;
    d->voice[v].prev1 = d->voice[v].prev2 = 0;
    if (VREG(v, 5) & 0x80) {
        d->voice[v].adsr_state = 1; /* attack */
        d->voice[v].env = 0;
    } else {
        d->voice[v].adsr_state = 0; /* the gain's direct value */
        d->voice[v].env = (VREG(v, 7) & 0x80) ? 0 : (VREG(v, 7) & 0x7F) << 4;
    }
    /* the first sample decodes at once and fills the gaussian's taps: the
     * four-tap read has nothing behind it yet */
    d->voice[v].out = voice_next_sample(v);
    for (unsigned k = 0; k < 4; k++) d->voice[v].hist[k] = d->voice[v].out;
    d->voice[v].buf_pos = 0;
    d->voice[v].rate_count = 0;
}

/* one BRR sample out of the current block, the filters applied; ends the
 * voice or loops when the block's flags say so */
static int16_t voice_next_sample(unsigned v) {
    snes_dsp_t *d = &machine.dsp;
    const uint8_t hdr = d->voice[v].hdr;
    const unsigned shift = hdr >> 4;
    const unsigned filter = (hdr >> 2) & 3;
    const uint8_t byte = machine.smp.ram[(d->voice[v].src + 1 + d->voice[v].nib / 2) & 0xFFFF];
    const int s = (d->voice[v].nib & 1 ? byte & 0xF : byte >> 4);
    int sample = (s & 8 ? s - 16 : s) << shift;
    sample <<= 4; sample >>= 4; /* sign-extend through the shift */
    const int16_t s1 = d->voice[v].prev1, s2 = d->voice[v].prev2;
    int out;
    switch (filter) {
    case 1: out = sample + ((s1 * 15) >> 4); break;
    case 2: out = sample + (((s1 * 61) >> 5) - ((s2 * 15) >> 4)); break;
    case 3: out = sample + (((s1 * 115) >> 6) - ((s2 * 13) >> 4)); break;
    default: out = sample; break;
    }
    if (out > 32767) out = 32767;
    if (out < -32768) out = -32768;
    d->voice[v].prev2 = s1;
    d->voice[v].prev1 = (int16_t)out;
    d->voice[v].nib++;
    if (d->voice[v].nib == 16) { /* the block is through: its flags decide */
        d->voice[v].nib = 0;
        if (hdr & 1) d->end_flags |= 1u << v;
        if (hdr & 2) d->voice[v].src = d->voice[v].loop;
        else if (hdr & 1) {
            d->voice[v].adsr_state = 4; /* end without loop: release */
            d->voice[v].rate_count = 0;
            return (int16_t)out;
        }
        d->voice[v].src = (uint16_t)(d->voice[v].src + 9);
        d->voice[v].hdr = machine.smp.ram[d->voice[v].src];
    }
    return (int16_t)out;
}

/* the envelope's one step, by the state and the register fields */
static void voice_envelope(unsigned v) {
    snes_dsp_t *d = &machine.dsp;
    d->voice[v].rate_count++;
    switch (d->voice[v].adsr_state) {
    case 1: { /* attack: linear to the top */
        const unsigned period = rate_period[(VREG(v, 5) & 0xF) * 2 + 1];
        if (!period) return;
        if (d->voice[v].rate_count < period) return;
        d->voice[v].rate_count = 0;
        d->voice[v].env = (int16_t)((d->voice[v].env + 1) & 0x7FF);
        if (d->voice[v].env >= ADSR_MAX) d->voice[v].adsr_state = 2;
        break;
    }
    case 2: { /* decay: -8 per period to the sustain level */
        const unsigned period = rate_period[((VREG(v, 5) >> 4) & 7) * 2 + 16];
        if (!period) return;
        if (d->voice[v].rate_count < period) return;
        d->voice[v].rate_count = 0;
        const int level = (((VREG(v, 6) >> 5) & 7) + 1) << 8;
        d->voice[v].env = (int16_t)(d->voice[v].env - 8);
        if (d->voice[v].env <= level) {
            d->voice[v].env = (int16_t)(level > ADSR_MAX ? ADSR_MAX : level);
            d->voice[v].adsr_state = 3;
        }
        break;
    }
    case 3: break; /* sustain: the level holds */
    case 4: { /* release: -8 per period to silence */
        const unsigned period = rate_period[VREG(v, 6) & 0x1F];
        if (!period) return;
        if (d->voice[v].rate_count < period) return;
        d->voice[v].rate_count = 0;
        d->voice[v].env = (int16_t)(d->voice[v].env - 8);
        if (d->voice[v].env <= 0) d->voice[v].env = 0;
        break;
    }
    default: { /* the gain modes */
        const uint8_t g = VREG(v, 7);
        if (!(g & 0x80)) {
            d->voice[v].env = (int16_t)((g & 0x7F) << 4);
            return;
        }
        const unsigned period = rate_period[g & 0x1F];
        if (!period) return;
        if (d->voice[v].rate_count < period) return;
        d->voice[v].rate_count = 0;
        switch ((g >> 5) & 3) {
        case 0: d->voice[v].env = (int16_t)(d->voice[v].env - 1); break;    /* linear dec */
        case 1: d->voice[v].env = (int16_t)(d->voice[v].env + 1); break;    /* linear inc */
        case 2: d->voice[v].env = (int16_t)(d->voice[v].env - 8); break;    /* bent dec */
        case 3: d->voice[v].env = (int16_t)(d->voice[v].env - (d->voice[v].env >> 8) - 1); break;
        }
        if (d->voice[v].env < 0) d->voice[v].env = 0;
        if (d->voice[v].env > ADSR_MAX) d->voice[v].env = ADSR_MAX;
        break;
    }
    }
}

/* the voice's output through the gaussian's four taps (or the noise), the
 * amplitude after the envelope, rounded even as the core does */
static int32_t voice_amplitude(unsigned v) {
    snes_dsp_t *d = &machine.dsp;
    int output;
    if (GREG(0x4E) & (1u << v)) { /* NON: the noise generator instead */
        output = (int16_t)(d->noise_lfsr << 1);
    } else {
        const unsigned frac = d->voice[v].pos & 0xFFF;
        const int16_t *fwd = gauss_table + 255 - ((frac >> 4) & 0xFF);
        const int16_t *rev = gauss_table + ((frac >> 4) & 0xFF);
        const unsigned o = d->voice[v].buf_pos; /* the oldest tap */
        output = (fwd[0] * d->voice[v].hist[o & 3]) >> 11;
        output += (fwd[256] * d->voice[v].hist[(o + 1) & 3]) >> 11;
        output += (rev[256] * d->voice[v].hist[(o + 2) & 3]) >> 11;
        output = (int16_t)output;
        output += (rev[0] * d->voice[v].hist[(o + 3) & 3]) >> 11;
        if (output > 32767) output = 32767;
        if (output < -32768) output = -32768;
        output &= ~1;
    }
    return ((int32_t)output * d->voice[v].env) >> 11 & ~1;
}

/* one output sample: the KON/KOF latches, the voices, the echo, the ring */
void snes_dsp_sample(void) {
    snes_dsp_t *d = &machine.dsp;
    if (d->key_on) {
        for (unsigned v = 0; v < 8; v++)
            if (d->key_on & (1u << v)) voice_start(v);
        d->key_on = 0;
    }
    if (d->key_off) {
        for (unsigned v = 0; v < 8; v++)
            if (d->key_off & (1u << v)) {
                d->voice[v].adsr_state = 4;
                d->voice[v].rate_count = 0;
            }
        d->key_off = 0;
    }
    /* the noise generator's rate shares the envelope period table */
    const unsigned nper = rate_period[GREG(0x6C) & 0x1F] ? rate_period[GREG(0x6C) & 0x1F] : 1;
    if (++d->noise_count >= nper) {
        d->noise_count = 0;
        const unsigned bit = (d->noise_lfsr ^ (d->noise_lfsr >> 1)) & 1;
        d->noise_lfsr = (uint16_t)((d->noise_lfsr >> 1) | bit << 14);
    }
    int32_t mix_l = 0, mix_r = 0;
    int32_t prev_amp = 0;
    const uint8_t pmon = GREG(0x4D), eon = GREG(0x4F);
    for (unsigned v = 0; v < 8; v++) {
        voice_envelope(v);
        unsigned pitch = (VREG(v, 3) << 8 | VREG(v, 2)) & 0x3FFF;
        if ((pmon & (1u << v)) && v) /* PMON: the previous voice bends it */
            pitch = (unsigned)(pitch + ((prev_amp >> 5) * pitch) / 1024);
        if (pitch && d->voice[v].env) {
            d->voice[v].pos += pitch;
            if (d->voice[v].pos > 0x7FFF) d->voice[v].pos = 0x7FFF;
            while (d->voice[v].pos >= 0x1000) { /* the next BRR sample */
                d->voice[v].pos -= 0x1000;
                d->voice[v].out = voice_next_sample(v);
                d->voice[v].hist[d->voice[v].buf_pos] = d->voice[v].out;
                d->voice[v].buf_pos = (uint8_t)((d->voice[v].buf_pos + 1) & 3);
            }
        }
        const int32_t s = voice_amplitude(v);
        prev_amp = s;
        const int8_t vl = (int8_t)VREG(v, 0), vr = (int8_t)VREG(v, 1);
        const int32_t amp_l = (s * vl) >> 7, amp_r = (s * vr) >> 7;
        mix_l = (int16_t)(mix_l + amp_l); /* the adds clamp through the cast */
        mix_r = (int16_t)(mix_r + amp_r);
        if (eon & (1u << v)) { /* the echo hears the EON voices */
            d->echo_feed[0] = (int16_t)(d->echo_feed[0] + amp_l);
            d->echo_feed[1] = (int16_t)(d->echo_feed[1] + amp_r);
        }
    }
    const uint8_t flg = GREG(0x6C);
    if (flg & 0x80) { /* the soft reset: the voices quiet until cleared */
        mix_l = mix_r = 0;
        for (unsigned v = 0; v < 8; v++) d->voice[v].env = 0;
    }
    /* mute sits on bit 6; the voices run their envelopes under it */
    /* the echo: the FIR over the delayed samples read out of the A-RAM,
     * the feedback written back (the writes stop with FLG bit 5), the EVOL
     * volumes mixing it into the output */
    d->echo_hist_pos = (uint8_t)((d->echo_hist_pos + 1) & 7);
    const unsigned edl = GREG(0x7D) & 0x0F;
    if (!d->echo_offset) d->echo_length = edl << 11;
    int32_t echo_in[2];
    for (unsigned c = 0; c < 2; c++) {
        const uint16_t at = (uint16_t)((GREG(0x6D) << 8) + d->echo_offset + c * 2);
        const int s = (int16_t)(machine.smp.ram[at] | machine.smp.ram[(uint16_t)(at + 1)] << 8);
        d->echo_hist[c][d->echo_hist_pos] = (int16_t)(s >> 1);
        int acc = 0;
        for (unsigned i = 0; i < 8; i++)
            acc += (d->echo_hist[c][(d->echo_hist_pos + i + 1) & 7] *
                    (int8_t)GREG(0x0F + i * 0x10)) >> 6;
        echo_in[c] = (int16_t)acc & ~1;
    }
    d->echo_offset = (uint16_t)(d->echo_offset + 4);
    if (d->echo_offset >= d->echo_length) d->echo_offset = 0;
    for (unsigned c = 0; c < 2; c++) {
        const int fb = (int16_t)(d->echo_feed[c] + ((echo_in[c] * (int8_t)GREG(0x0D)) >> 7)) & ~1;
        if (!(flg & 0x20)) { /* ECEN: the buffer takes the feedback */
            const uint16_t at = (uint16_t)((GREG(0x6D) << 8) + d->echo_offset - 4 + c * 2);
            machine.smp.ram[at] = (uint8_t)fb;
            machine.smp.ram[(uint16_t)(at + 1)] = (uint8_t)(fb >> 8);
        }
        d->echo_feed[c] = 0;
    }
    if (flg & 0x40) { mix_l = mix_r = 0; } /* mute */
    const int8_t ml = (int8_t)GREG(0x0C), mr = (int8_t)GREG(0x1C);
    const int8_t el = (int8_t)GREG(0x2C), er = (int8_t)GREG(0x3C);
    int32_t l = (int16_t)((mix_l * ml) >> 7) + (((echo_in[0] * el) >> 7) & ~1);
    int32_t r = (int16_t)((mix_r * mr) >> 7) + (((echo_in[1] * er) >> 7) & ~1);
    if (l > 32767) l = 32767;
    if (l < -32768) l = -32768;
    if (r > 32767) r = 32767;
    if (r < -32768) r = -32768;
    d->ring[d->ring_w] = (int16_t)l;
    d->ring[(d->ring_w + 1) & 8191] = (int16_t)r;
    d->ring_w = (uint16_t)((d->ring_w + 2) & 8191);
}
