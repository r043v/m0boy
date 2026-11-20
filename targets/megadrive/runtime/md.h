#ifndef M0BOY_MD_H
#define M0BOY_MD_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Bounded Mega Drive profile: reset vectors in ROM, upper 4 KiB of work
 * RAM, VDP control/data ports with register writes and CRAM colour zero.
 * A bus fault or unsupported opcode stops execution; no silent emulation. */
typedef struct {
    uint32_t d[8], a[8], pc;
#ifdef MD_FULL_VDP
    uint8_t ram[65536];
    uint8_t vram[65536];
#else
    uint8_t ram[4096];
#endif
    uint16_t cram[64];
    uint16_t vdp_reg[24];
    uint16_t vdp_addr;
    uint8_t vdp_target, vdp_pending;
    uint16_t vdp_first;
    uint8_t z, fault;
    uint8_t pad, pad_th, pad_control;
    uint32_t instructions, frames;
} Md;

enum { MD_UP = 1u, MD_DOWN = 2u, MD_LEFT = 4u, MD_RIGHT = 8u,
       MD_A = 16u, MD_B = 32u, MD_C = 64u, MD_START = 128u };

#ifdef MD_ROM_EXTERNAL   /* PC build whose ROM is loaded at run time (md_rom_ext.c) */
extern const uint8_t *md_rom;
extern uint32_t md_rom_size;
int md_rom_load(const char *path);
#else
extern const uint8_t md_rom[];
extern const uint32_t md_rom_size;
#endif
void md_reset(Md *m);
uint8_t md_read8(Md *m, uint32_t addr);
uint16_t md_read16(Md *m, uint32_t addr);
uint32_t md_read32(Md *m, uint32_t addr);
void md_write8(Md *m, uint32_t addr, uint8_t value);
void md_write16(Md *m, uint32_t addr, uint16_t value);
void md_write32(Md *m, uint32_t addr, uint32_t value);
void md_set_word(Md *m, unsigned n, uint16_t v);
void md_set_long(Md *m, unsigned n, uint32_t v);
void md_run_native(Md *m, uint32_t steps);
void md_run_interp(Md *m, uint32_t steps);
uint16_t md_background_rgb565(const Md *m);
void md_render_rgb565(const Md *m, uint16_t *pixels, unsigned stride);
uint32_t md_hash(const Md *m);
#ifdef __cplusplus
}
#endif
#endif
