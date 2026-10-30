/* Bus mirrors, WRAM port and 65C816 interrupt stack frames. */
#include "../targets/snes/runtime/snes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static uint8_t rom[0x10000];
static uint8_t image[256 * 224 * 3];

static void video_test(void) {
    snes_init(rom, sizeof rom, 0, 0);
    /* DMA mode 1 interleaves VMDATAL/VMDATAH and advances the word address. */
    machine.wram[0x100] = 0x80;
    machine.wram[0x101] = 0x00;
    machine.wram[0x102] = 0x33;
    machine.wram[0x103] = 0x44;
    snes_bus_write(0x2115, 0x80);
    snes_bus_write(0x2116, 0x10);
    snes_bus_write(0x2117, 0);
    snes_bus_write(0x4300, 1);
    snes_bus_write(0x4301, 0x18);
    snes_bus_write(0x4302, 0);
    snes_bus_write(0x4303, 1);
    snes_bus_write(0x4304, 0x7e);
    snes_bus_write(0x4305, 4);
    snes_bus_write(0x4306, 0);
    snes_bus_write(0x420b, 1);
    CHECK(machine.vram[32] == 0x80 && machine.vram[33] == 0);
    CHECK(machine.vram[34] == 0x33 && machine.vram[35] == 0x44);
    CHECK(machine.dma[0][2] == 4 && machine.dma[0][3] == 1);
    CHECK(machine.dma[0][5] == 0 && machine.dma[0][6] == 0);
    snes_bus_write(0x2116, 0x10);
    snes_bus_write(0x2117, 0);
    CHECK(snes_bus_read(0x2139) == 0x80);
    CHECK(snes_bus_read(0x213a) == 0);
    /* Mode 1 BG1, tile 1 at the first map entry, palette color 1 red. */
    snes_bus_write(0x2116, 0);
    snes_bus_write(0x2117, 4);
    snes_bus_write(0x2118, 1);
    snes_bus_write(0x2119, 0);
    snes_bus_write(0x2121, 1);
    snes_bus_write(0x2122, 0x1f);
    snes_bus_write(0x2122, 0);
    snes_bus_write(0x2105, 1);
    snes_bus_write(0x2107, 4);
    snes_bus_write(0x212c, 1);
    snes_bus_write(0x2100, 15);
    snes_render(image);
    CHECK(image[0] == 255 && image[1] == 0 && image[2] == 0);
    CHECK(image[3] == 0 && image[4] == 0 && image[5] == 0);
    snes_bus_write(0x2100, 0x8f);
    snes_render(image);
    CHECK(image[0] == 0 && image[1] == 0 && image[2] == 0);
}

static void object_test(void) {
    snes_init(rom, sizeof rom, 0, 0);
    for (unsigned i = 0; i < 128; i++) machine.oam[i * 4 + 1] = 224;

    /* Low OAM commits only after both writes; the high table mirrors. */
    snes_bus_write(0x2102, 0);
    snes_bus_write(0x2103, 0);
    snes_bus_write(0x2104, 7);
    CHECK(machine.oam[0] == 0);
    snes_bus_write(0x2104, 9);
    CHECK(machine.oam[0] == 7 && machine.oam[1] == 9);
    snes_bus_write(0x2102, 0x10);
    snes_bus_write(0x2103, 1);
    snes_bus_write(0x2104, 0x5a);
    CHECK(machine.oam[512] == 0x5a);
    snes_bus_write(0x2102, 0);
    snes_bus_write(0x2103, 1);
    CHECK(snes_bus_read(0x2138) == 0x5a);
    snes_bus_write(0x2102, 0);
    snes_bus_write(0x2103, 1);
    snes_bus_write(0x2104, 0); /* two small, positive-X objects */

    /* DMA two objects to OAM. Sprite 0 covers sprite 1 despite lower
     * background priority; both use a one-pixel 4bpp tile. */
    const uint8_t objects[] = {0,0,1,0x00, 0,0,1,0x32};
    for (unsigned i = 0; i < sizeof objects; i++) machine.wram[0x100 + i] = objects[i];
    snes_bus_write(0x2102, 0); snes_bus_write(0x2103, 0);
    snes_bus_write(0x4300, 0); snes_bus_write(0x4301, 4);
    snes_bus_write(0x4302, 0); snes_bus_write(0x4303, 1);
    snes_bus_write(0x4304, 0x7e); snes_bus_write(0x4305, 8); snes_bus_write(0x4306, 0);
    snes_bus_write(0x420b, 1);
    CHECK(machine.oam[0] == 0 && machine.oam[1] == 0 && machine.oam[6] == 1);
    machine.vram[32] = 0x80;
    machine.cgram[129 * 2] = 0x1f; /* red */
    machine.cgram[145 * 2] = 0xe0; machine.cgram[145 * 2 + 1] = 3; /* green */
    snes_bus_write(0x2100, 15); snes_bus_write(0x2105, 1); snes_bus_write(0x212c, 0x10);
    snes_render(image);
    CHECK(image[0] == 255 && image[1] == 0 && image[2] == 0);
    /* Rotation selects sprite 1 first. */
    snes_bus_write(0x2102, 2); snes_bus_write(0x2103, 0x80);
    snes_render(image);
    CHECK(image[0] == 0 && image[1] == 255 && image[2] == 0);
    /* Horizontal flip moves the sole pixel from x=0 to x=7. */
    machine.oam[7] |= 0x40;
    snes_render(image);
    CHECK(image[0] == 255 && image[1] == 0 && image[2] == 0);
    CHECK(image[7 * 3] == 0 && image[7 * 3 + 1] == 255);

    /* BG1 low priority hides OBJ priority 0; OBJ priority 3 hides BG1. */
    machine.oam[7] = 0x02;
    machine.oam[3] = 0;
    machine.vram[0x800] = 1;
    machine.cgram[2] = 0; machine.cgram[3] = 0x7c; /* blue */
    snes_bus_write(0x2107, 4); snes_bus_write(0x212c, 0x11);
    snes_bus_write(0x2102, 0); snes_bus_write(0x2103, 0);
    snes_render(image);
    CHECK(image[0] == 0 && image[1] == 0 && image[2] == 255);
    machine.oam[3] = 0x30;
    snes_render(image);
    CHECK(image[0] == 255 && image[1] == 0 && image[2] == 0);

    /* Signed X and large 16x16 objects use the high OAM table. */
    machine.oam[5] = 224;
    machine.oam[0] = 252; machine.oam[1] = 0; machine.oam[2] = 1;
    machine.oam[512] = 1; /* x = -4 */
    machine.vram[32] = 0x08; /* pixel 4 */
    snes_bus_write(0x212c, 0x10);
    snes_render(image);
    CHECK(image[0] == 255 && image[1] == 0 && image[2] == 0);
    machine.oam[0] = 8; machine.oam[512] = 2; /* large, x = 8 */
    machine.vram[32] = 0;
    machine.vram[18 * 32] = 0x80; /* bottom-right tile */
    snes_render(image);
    unsigned pixel = (8 * 256 + 16) * 3;
    CHECK(image[pixel] == 255 && image[pixel + 1] == 0 && image[pixel + 2] == 0);
}

/* The per-line object limits: 32 sprites, 34 tile slots, and the $213F
 * flags of the last evaluated line. */
static void object_limits_test(void) {
    snes_init(rom, sizeof rom, 0, 0);
    machine.vram[32] = 0x80;       /* the one-pixel 4bpp tile */
    machine.cgram[129 * 2] = 0x1f; /* red, palette 1 */
    snes_bus_write(0x2100, 15); snes_bus_write(0x2105, 1); snes_bus_write(0x212c, 0x10);
    for (unsigned i = 0; i < 33; i++) {
        machine.oam[i * 4 + 0] = (uint8_t)i; /* X = 0..32 */
        machine.oam[i * 4 + 1] = 8;          /* Y: the line 8 */
        machine.oam[i * 4 + 2] = 1;
        machine.oam[i * 4 + 3] = 0x00;
    }
    snes_render_line(8, image); /* one row: the pixels sit at image[x * 3] */
    unsigned pixel = 32 * 3; /* the 33rd stays invisible */
    CHECK(image[pixel] == 0 && image[pixel + 1] == 0 && image[pixel + 2] == 0);
    pixel = 31 * 3; /* the 32nd is in */
    CHECK(image[pixel] == 255 && image[pixel + 1] == 0 && image[pixel + 2] == 0);
    CHECK((snes_bus_read(0x213f) & 0x60) == 0x20); /* range over */

    /* 16-pixel objects spend two tile slots: 17 fit the 34, the 18th
     * hits the time over */
    snes_bus_write(0x2101, 0x60); /* 16x16 objects, name base 0 */
    for (unsigned i = 0; i < 18; i++) {
        machine.oam[i * 4 + 0] = (uint8_t)(i * 8);
        machine.oam[i * 4 + 1] = 8;
        machine.oam[i * 4 + 2] = 1;
        machine.oam[i * 4 + 3] = 0x00;
    }
    snes_render_line(8, image);
    pixel = 8 * 17 * 3; /* the 18th is out */
    CHECK(image[pixel] == 0 && image[pixel + 1] == 0 && image[pixel + 2] == 0);
    pixel = 8 * 16 * 3; /* the 17th is in */
    CHECK(image[pixel] == 255 && image[pixel + 1] == 0 && image[pixel + 2] == 0);
    CHECK((snes_bus_read(0x213f) & 0x60) == 0x40); /* time over */
}

/* Modes 3 (direct color), 2 (offset-per-tile) and 7 (the identity
 * matrix fetches the map and tileset bytes). */
static void modes_test(void) {
    snes_init(rom, sizeof rom, 0, 0);
    /* Mode 3, BG1 8bpp: direct color turns the $FF index into RRRGGGBB.
     * The tileset sits at NBA 1 so the map at 0 stays empty; every plane's
     * bit 7 set gives the index $FF. */
    memset(machine.vram + 0x2000, 0x80, 64);
    machine.cgram[0xff * 2] = 0x1f; /* the CGRAM path shows red */
    snes_bus_write(0x2105, 3); snes_bus_write(0x210b, 1); snes_bus_write(0x212c, 1); snes_bus_write(0x2100, 15);
    snes_render_line(0, image);
    CHECK(image[0] == 255 && image[1] == 0 && image[2] == 0); /* CGRAM path: red */
    snes_bus_write(0x2130, 1); /* CGWSEL direct color */
    snes_render_line(0, image);
    CHECK(image[0] == 25 && image[1] == 58 && image[2] == 58);

    /* Mode 2: the offset-per-tile entry of tile (0,0) shifts the source
     * right by one tile, so tile 1's pixel moves from x=8 to x=0. */
    snes_init(rom, sizeof rom, 0, 0);
    machine.vram[32] = 0x80;      /* tile 1: one pixel at (0,0) */
    machine.vram[2] = 0x01; machine.vram[3] = 0x00; /* map word 1 = tile 1 */
    machine.cgram[2] = 0x1f;
    snes_bus_write(0x2105, 2); snes_bus_write(0x212c, 1); snes_bus_write(0x2100, 15);
    snes_render_line(0, image);
    CHECK(image[8 * 3] == 255 && image[0] == 0); /* unshifted: x=8 shows it */
    machine.vram[0x400] = 0x08; machine.vram[0x401] = 0; /* the opt map sits one map past the base: +8 */
    snes_render_line(0, image);
    CHECK(image[0] == 255 && image[16 * 3] == 0); /* tile (0,0) shifted, (1,0) not */

    /* Mode 7 with the identity matrix: the map's low byte at (0,0) picks
     * tile 1, whose (0,0) pixel sits in the high byte at $4081. */
    snes_init(rom, sizeof rom, 0, 0);
    machine.vram[0] = 1;                 /* map (0,0) = tile 1 */
    machine.vram[0x4081] = 5;            /* tile 1's (0,0) = index 5 */
    machine.cgram[10] = 0x00; machine.cgram[11] = 0x7c; /* blue */
    snes_bus_write(0x2105, 7); snes_bus_write(0x212c, 1); snes_bus_write(0x2100, 15);
    snes_bus_write(0x211b, 0x00); snes_bus_write(0x211b, 0x01); /* M7A = 1.0 */
    snes_bus_write(0x211e, 0x00); snes_bus_write(0x211e, 0x01); /* M7D = 1.0 */
    snes_render_line(0, image);
    CHECK(image[0] == 0 && image[1] == 0 && image[2] == 255);
    CHECK(image[8 * 3] == 0 && image[8 * 3 + 2] == 0); /* map (1,0) is empty */
}

/* The APU: the boot handshake uploads a program and starts it, the
 * program keys a voice whose BRR block reaches the ring, the timers tick
 * at their documented rates. */
static void apu_test(void) {
    snes_init(rom, sizeof rom, 0, 0);
    /* the boot: the IPL's ready signature, the $CC command with the
     * block's page, the bytes with their counter on port 0, the go */
    CHECK(machine.smp.ipl_state == 0 && machine.smp.ports_out[0] == 0xaa);
    CHECK(machine.smp.ports_out[1] == 0xbb);
    snes_bus_write(0x2142, 0x00); snes_bus_write(0x2143, 0x02);
    snes_bus_write(0x2140, 0xcc); /* the upload command, the block at $0200 */
    CHECK(machine.smp.ipl_state == 2 && machine.smp.ports_out[0] == 0xcc);
    static const uint8_t prog[] = {
        0xe8, 0x4c,             /* MOV A,#$4C */
        0xc4, 0xf2,             /* MOV $F2,A */
        0xe8, 0x01,             /* MOV A,#$01 */
        0xc4, 0xf3,             /* MOV $F3,A: KON = voice 0 */
        0x80, 0xfe,             /* BRA $FE */
    };
    for (unsigned i = 0; i < sizeof prog; i++) {
        snes_bus_write(0x2140, (uint8_t)i);   /* the byte's counter */
        snes_bus_write(0x2141, prog[i]);      /* the byte */
        CHECK(machine.smp.ports_out[0] == i); /* the counter echoed */
    }
    CHECK(machine.smp.ram[0x200] == prog[0] && machine.smp.ram[0x208] == prog[8]);
    snes_bus_write(0x2142, 0x00); snes_bus_write(0x2143, 0x02);
    snes_bus_write(0x2141, 0x00);
    snes_bus_write(0x2140, 0x80); /* terminate: counter ahead, port 1 = 0: jump */
    CHECK(machine.smp.ipl_state == 3 && machine.smp.pc == 0x0200);

    /* the voice's sample source: the directory at $0300, the BRR block at
     * $0400 (block addresses in 8-byte units), one block of two $7
     * samples at shift 12, then an end-and-loop block */
    machine.smp.ram[0x300] = 0x80; machine.smp.ram[0x301] = 0x00; /* start $0400 */
    machine.smp.ram[0x302] = 0x80; machine.smp.ram[0x303] = 0x00; /* loop $0400 */
    machine.smp.ram[0x400] = 0xc0; /* shift 12, filter 0 */
    for (unsigned i = 1; i < 9; i++) machine.smp.ram[0x400 + i] = 0x77;
    machine.smp.ram[0x409] = 0x03; /* the next block: end + loop */
    machine.dsp.regs[0x5d] = 0x03; /* DIR */
    machine.dsp.regs[0x02] = 0x00; machine.dsp.regs[0x03] = 0x10; /* pitch 1.0 */
    machine.dsp.regs[0x00] = machine.dsp.regs[0x01] = 0x7f;       /* volumes */
    machine.dsp.regs[0x04] = 0x00;                                /* SRCN 0 */
    machine.dsp.regs[0x07] = 0x7f;                                /* GAIN direct */
    machine.dsp.regs[0x0c] = machine.dsp.regs[0x1c] = 0x7f;       /* main volume */
    snes_frame();
    CHECK(machine.dsp.regs[0x4c] == 0x01); /* the program keyed the voice */
    CHECK(machine.dsp.voice[0].adsr_state == 0); /* the direct gain */

    /* two samples of $7 << 12 through env $7F0 and the volumes */
    const int s0 = machine.dsp.ring[0];
    const int expected = (7 * 4096) * 0x7f0 / 2048 * 0x7f / 128 * 0x7f / 128;
    CHECK(s0 > expected - 500 && s0 < expected + 500);

    /* the timers: T0 at 8000/(7+1) = 1000 Hz, T2 at 64000/(7+1) */
    machine.smp.control = 0x07;
    machine.smp.ram[0xfa] = 7; machine.smp.ram[0xfc] = 7;
    const uint8_t t0 = machine.smp.timer_out[0], t2 = machine.smp.timer_out[2];
    snes_frame();
    const uint8_t d0 = (uint8_t)(machine.smp.timer_out[0] - t0);
    const uint8_t d2 = (uint8_t)(machine.smp.timer_out[2] - t2);
    CHECK(d0 >= 14 && d0 <= 19);   /* ~1000 Hz over one frame */
    CHECK(d2 >= 128 && d2 <= 138); /* ~8000 Hz over one frame */
    machine.smp.control = 0;
}

static void interrupt_test(int emulation, int interpreter) {
    snes_init(rom, sizeof rom, 0, 0);
    machine.cpu.e = emulation;
    machine.cpu.k = 0x81;
    machine.cpu.pc = 0x9234;
    machine.cpu.s = emulation ? 0x01ff : 0x1fff;
    machine.cpu.p = SNES_P_X | SNES_P_D;
    if (interpreter) {
        machine.cpu.nmi_pending = 1;
        snes_run(&machine.cpu, emulation ? 7 : 8);
    } else snes_take_interrupt(&machine.cpu, emulation ? 0xfffa : 0xffea);
    CHECK(machine.cpu.k == 0);
    CHECK(machine.cpu.pc == 0x8000);
    CHECK(machine.cpu.s == (emulation ? 0x01fc : 0x1ffb));
    CHECK(machine.wram[emulation ? 0x01ff : 0x1ffe] == 0x92);
    CHECK(machine.wram[emulation ? 0x01fe : 0x1ffd] == 0x34);
    CHECK(machine.wram[emulation ? 0x01fd : 0x1ffc] == (emulation ? 0x28 : 0x18));
    if (!emulation) CHECK(machine.wram[0x1fff] == 0x81);
    CHECK((machine.cpu.p & (SNES_P_I | SNES_P_D)) == SNES_P_I);
}

static void cpu_flow_test(void) {
    /* Fetch all JML operands from the original bank before switching. */
    rom[0] = 0x5c; rom[1] = 0x34; rom[2] = 0x92; rom[3] = 0x81;
    rom[0x8001] = 0x78; rom[0x8002] = 0x56;
    snes_init(rom, sizeof rom, 0, 0);
    snes_run(&machine.cpu, 4);
    CHECK(machine.cpu.k == 0x81 && machine.cpu.pc == 0x9234);
    /* JSL from bank 00 into its 80 mirror, then RTL returns to bank 00. */
    rom[0] = 0x22; rom[1] = 0x10; rom[2] = 0x80; rom[3] = 0x80;
    rom[0x10] = 0x6b;
    snes_init(rom, sizeof rom, 0, 0);
    snes_run(&machine.cpu, 8);
    CHECK(machine.cpu.k == 0x80 && machine.cpu.pc == 0x8010);
    CHECK(machine.cpu.s == 0x1fc);
    CHECK(machine.wram[0x1ff] == 0 && machine.wram[0x1fe] == 0x80 && machine.wram[0x1fd] == 3);
    snes_run(&machine.cpu, 14);
    CHECK(machine.cpu.k == 0 && machine.cpu.pc == 0x8004 && machine.cpu.s == 0x1ff);

    /* WAI remains asleep across slices and resumes after NMI at the next byte. */
    rom[0] = 0xcb; rom[1] = 0xea;
    snes_init(rom, sizeof rom, 0, 0);
    snes_run(&machine.cpu, 20);
    CHECK(machine.cpu.waiting && machine.cpu.pc == 0x8001);
    snes_run(&machine.cpu, 40);
    CHECK(machine.cpu.waiting && machine.cpu.pc == 0x8001);
    machine.cpu.nmi_pending = 1;
    snes_run(&machine.cpu, 47);
    CHECK(!machine.cpu.waiting && machine.cpu.pc == 0x8000);
    CHECK(machine.wram[0x1ff] == 0x80 && machine.wram[0x1fe] == 1);
}

static void joypad_test(void) {
    snes_init(rom, sizeof rom, 0, 0);
    snes_set_joypad(0x8940); /* B, up, right and X */
    snes_bus_write(0x4016, 1);
    CHECK((snes_bus_read(0x4016) & 1) == 1);
    CHECK((snes_bus_read(0x4016) & 1) == 1); /* strobe holds B */
    snes_bus_write(0x4016, 0);
    for (unsigned i = 0; i < 16; i++)
        CHECK((snes_bus_read(0x4016) & 1) == ((0x8940u >> (15 - i)) & 1));
    CHECK((snes_bus_read(0x4016) & 1) == 1);
    CHECK((snes_bus_read(0x4017) & 3) == 0);
    CHECK(snes_bus_read(0x4218) == 0 && snes_bus_read(0x4219) == 0);
    machine.cpu.halted = 1;
    snes_bus_write(0x4200, 1);
    snes_frame();
    CHECK(snes_bus_read(0x4218) == 0x40 && snes_bus_read(0x4219) == 0x89);
    CHECK((snes_bus_read(0x4212) & 1) == 0);
    snes_set_joypad(0x1000);
    CHECK(snes_bus_read(0x4219) == 0x89); /* auto result stays latched */
    snes_frame();
    CHECK(snes_bus_read(0x4218) == 0 && snes_bus_read(0x4219) == 0x10);
    /* Enabling NMI while the VBlank flag is set raises its edge. */
    snes_bus_write(0x4200, 0x81);
    CHECK(machine.cpu.nmi_pending);
}

static void cpu_index_test(void) {
    rom[0] = 0x9b; rom[1] = 0xbb; /* TXY, TYX */
    snes_init(rom, sizeof rom, 0, 0);
    machine.cpu.e = 0; machine.cpu.p = 0;
    machine.cpu.x = 0x8001; machine.cpu.y = 0x1234;
    snes_run(&machine.cpu, 2);
    CHECK(machine.cpu.x == 0x8001 && machine.cpu.y == 0x8001 && (machine.cpu.p & SNES_P_N));
    machine.cpu.y = 0;
    snes_run(&machine.cpu, 4);
    CHECK(machine.cpu.x == 0 && machine.cpu.y == 0 && (machine.cpu.p & SNES_P_Z));

    rom[0] = 0xb5; rom[1] = 0xf0; /* LDA $f0,X */
    snes_init(rom, sizeof rom, 0, 0);
    machine.cpu.e = 0; machine.cpu.p = SNES_P_M | SNES_P_X;
    machine.cpu.x = 0x30; machine.cpu.d = 0;
    machine.wram[0x20] = 0x11; machine.wram[0x120] = 0x22;
    snes_run(&machine.cpu, 4);
    CHECK((machine.cpu.a & 0xff) == 0x22); /* native crosses the page */
    snes_init(rom, sizeof rom, 0, 0);
    machine.cpu.x = 0x30;
    machine.wram[0x20] = 0x11; machine.wram[0x120] = 0x22;
    snes_run(&machine.cpu, 4);
    CHECK((machine.cpu.a & 0xff) == 0x11); /* emulation with aligned D wraps */
    snes_init(rom, sizeof rom, 0, 0);
    machine.cpu.x = 0x30; machine.cpu.d = 1;
    machine.wram[0x21] = 0x11; machine.wram[0x121] = 0x22;
    snes_run(&machine.cpu, 5);
    CHECK((machine.cpu.a & 0xff) == 0x22); /* unaligned D crosses */

    /* Wide read-modify-write must address the requested word. */
    rom[0] = 0x46; rom[1] = 0x18; rom[2] = 0x06; rom[3] = 0x1c;
    snes_init(rom, sizeof rom, 0, 0);
    machine.cpu.e = 0; machine.cpu.p = 0;
    machine.wram[0x18] = 4; machine.wram[0x19] = 1; /* $0104 */
    machine.wram[0x1c] = 0x80; machine.wram[0x1d] = 1; /* $0180 */
    snes_run(&machine.cpu, 6);
    CHECK(machine.wram[0x18] == 0x82 && machine.wram[0x19] == 0);
    snes_run(&machine.cpu, 12);
    CHECK(machine.wram[0x1c] == 0 && machine.wram[0x1d] == 3);

    rom[0] = 0x04; rom[1] = 0x10; /* TSB: Z from A & old memory */
    rom[2] = 0x14; rom[3] = 0x10; /* TRB: same test before clearing */
    rom[4] = 0x89; rom[5] = 0;    /* BIT immediate leaves N and V */
    snes_init(rom, sizeof rom, 0, 0);
    machine.cpu.a = 1; machine.cpu.p |= SNES_P_N | SNES_P_V;
    machine.wram[0x10] = 2;
    snes_run(&machine.cpu, 5);
    CHECK(machine.wram[0x10] == 3 && (machine.cpu.p & SNES_P_Z));
    snes_run(&machine.cpu, 10);
    CHECK(machine.wram[0x10] == 2 && !(machine.cpu.p & SNES_P_Z));
    snes_run(&machine.cpu, 12);
    CHECK((machine.cpu.p & (SNES_P_N | SNES_P_V | SNES_P_Z)) == (SNES_P_N | SNES_P_V | SNES_P_Z));

    rom[0] = 0x54; rom[1] = 0x7e; rom[2] = 0; /* MVN destination, source */
    rom[0x10] = 0x61; rom[0x11] = 0x72;
    snes_init(rom, sizeof rom, 0, 0);
    machine.cpu.e = 0; machine.cpu.p = 0;
    machine.cpu.a = 1; machine.cpu.x = 0x8010; machine.cpu.y = 0x234;
    snes_run(&machine.cpu, 14);
    CHECK(machine.wram[0x234] == 0x61 && machine.wram[0x235] == 0x72);
    CHECK(machine.cpu.dbr == 0x7e && machine.cpu.a == 0xffff);
    CHECK(machine.cpu.x == 0x8012 && machine.cpu.y == 0x236 && machine.cpu.pc == 0x8003);
}

/* The HDMA feeds one unit per line: a direct-mode table paints three red
 * colors then one blue into CGRAM, the repeat entry carrying three units. */
static void hdma_test(void) {
    static const uint8_t table[] = {
        0x83, 0x1f, 0x00, 0x1f, 0x00, 0x1f, 0x00, /* repeat, 3 lines of red */
        0x01, 0x00, 0x7c,                          /* one line of blue */
        0x00,                                      /* end */
    };
    memcpy(rom + 0x100, table, sizeof table);
    snes_init(rom, sizeof rom, 0, 0);
    uint8_t *d = machine.dma[0];
    d[0] = 0x02;              /* mode 2: two bytes to one register, A to B */
    d[1] = 0x22;              /* CGDATA */
    d[2] = 0x00; d[3] = 0x81; /* the table at $008100 */
    d[4] = 0x00;
    snes_bus_write(0x2121, 0); /* CGADD = 0 */
    snes_bus_write(0x420c, 1); /* channel 0 on */
    snes_frame();
    CHECK(machine.hdma_done[0] && !machine.hdma_run[0]);
    CHECK(d[8] == 0x0b && d[9] == 0x81); /* the table pointer ran through */
    CHECK(machine.cgram[0] == 0x1f && machine.cgram[1] == 0x00);
    CHECK(machine.cgram[2] == 0x1f && machine.cgram[3] == 0x00);
    CHECK(machine.cgram[4] == 0x1f && machine.cgram[5] == 0x00);
    CHECK(machine.cgram[6] == 0x00 && machine.cgram[7] == 0x7c);
    CHECK(machine.cgram[8] == 0 && machine.cgram[9] == 0);
    snes_bus_write(0x420c, 0);
}

/* The H/V timer IRQs: an H-IRQ on every line, a V-IRQ on one line, the
 * handler counts in WRAM and clears $4211. */
static void irq_test(void) {
    rom[0] = 0x80; rom[1] = 0xfe; /* the reset code idles at $8000 */
    /* the handler at $008100: PHA, read $4211, INC $30, PLA, RTI */
    const uint8_t handler[] = {0x48, 0xad, 0x11, 0x42, 0xee, 0x30, 0x00, 0x68, 0x40};
    memcpy(rom + 0x100, handler, sizeof handler);
    rom[0x7fee] = rom[0x7ffe] = 0x00; /* IRQ and BRK vectors -> $008100 */
    rom[0x7fef] = rom[0x7fff] = 0x81;
    snes_init(rom, sizeof rom, 0, 0);
    machine.cpu.e = 0;
    machine.cpu.p &= (uint8_t)~SNES_P_I;
    snes_bus_write(0x4207, 100); /* HTIME = 100: the pair's latch composes */
    snes_bus_write(0x4208, 0);
    snes_bus_write(0x4200, 0x10); /* H-IRQ enabled */
    snes_frame();
    CHECK(machine.wram[0x30] == 262u % 256u); /* one match per line, 8-bit INC */
    CHECK(machine.timeup == 0);       /* the last handler read cleared it */

    machine.wram[0x30] = 0;
    snes_bus_write(0x4200, 0x00);
    snes_bus_write(0x4207, 44); /* HTIME = 300: past the visible cut, the
                                 * match waits for the HDMA boundary */
    snes_bus_write(0x4208, 1);
    snes_bus_write(0x4200, 0x10);
    snes_frame();
    CHECK(machine.wram[0x30] == 262u % 256u);
    snes_bus_write(0x4200, 0x00);
    snes_bus_write(0x4209, 100); /* VTIME = 100 */
    snes_bus_write(0x420a, 0);
    snes_bus_write(0x4200, 0x20); /* V-IRQ enabled */
    machine.wram[0x30] = 0;
    snes_frame();
    CHECK(machine.wram[0x30] == 1);
    machine.wram[0x30] = 0;
    snes_bus_write(0x4200, 0x00);
    /* without the $4211 read the level stays asserted: the handler loops
     * until the line's budget is spent */
    rom[0x100] = 0x40; /* the handler is just RTI */
    snes_init(rom, sizeof rom, 0, 0);
    machine.cpu.e = 0;
    machine.cpu.p &= (uint8_t)~SNES_P_I;
    snes_bus_write(0x4207, 100);
    snes_bus_write(0x4200, 0x10);
    snes_frame();
    CHECK(machine.timeup == 1 && machine.cpu.irq_line == 1);
    snes_bus_write(0x4200, 0x00);
}

/* The bus time of DMA and HDMA comes off the CPU's slices: the transfers
 * book into dma_stolen (16 cycles of channel start-up, 8 a byte), and a
 * frame of idle loops spends the frame's cycles less what the HDMA took. */
static void timing_test(void) {
    rom[0] = 0x80; rom[1] = 0xfe;
    snes_init(rom, sizeof rom, 0, 0);
    /* one channel, 256 bytes, mode 0 to the WRAM port, A fixed on the
     * ROM's first byte ($00:0000 is the WRAM mirror, $00:8000 is ROM) */
    snes_bus_write(0x4300, 0x08);
    snes_bus_write(0x4301, 0x80); /* B = $2180 */
    snes_bus_write(0x4302, 0x00);
    snes_bus_write(0x4303, 0x80); /* A = $00:8000 */
    snes_bus_write(0x4304, 0x00);
    snes_bus_write(0x4305, 0x00);
    snes_bus_write(0x4306, 0x01); /* 256 bytes */
    snes_bus_write(0x420b, 0x01);
    CHECK(machine.dma_stolen == 16 + 8u * 256);
    CHECK(machine.wram[0] == 0x80); /* the transfer happened */
    /* one HDMA channel, mode 0, direct: a one-unit table at $00:8100 */
    snes_bus_write(0x4300, 0x00);
    snes_bus_write(0x4301, 0x80);
    snes_bus_write(0x4304, 0x00); /* the table's bank */
    snes_bus_write(0x4302, 0x00);
    snes_bus_write(0x4303, 0x81);
    memset(rom + 0x100, 0, 0x10); /* the earlier tests left tables there */
    rom[0x100] = 0x01; /* one unit */
    rom[0x101] = 0x00; /* then done */
    snes_bus_write(0x420c, 0x01);
    const uint32_t before = machine.cpu.cycles;
    snes_frame();
    /* the frame's full cycles, less what the HDMA took (a table read and
     * one unit): the frame's CPU spend and the stolen time add up */
    const uint32_t spent = machine.cpu.cycles - before;
    CHECK(machine.dma_stolen == 24); /* the table read and unit, then the
                                      * next line's zero count byte */
    CHECK(spent == SNES_FRAME_CYCLES - machine.dma_stolen);
    snes_bus_write(0x420c, 0x00);
}

/* The hardware multiply/divide unit: signed 8x8 -> $4216/7, unsigned
 * 16/8 -> quotient $4214/5 and remainder $4216, /0 = $FFFF/$FF. */
static void muldiv_test(void) {
    snes_bus_write(0x4202, 15);
    snes_bus_write(0x4203, 17);
    CHECK(snes_bus_read(0x4216) == 255 && snes_bus_read(0x4217) == 0);
    snes_bus_write(0x4202, 0xFF); /* -1 times -13 = 13 */
    snes_bus_write(0x4203, 0xF3);
    CHECK(snes_bus_read(0x4216) == 13 && snes_bus_read(0x4217) == 0);
    snes_bus_write(0x4204, 0x39); snes_bus_write(0x4205, 0x05); /* $0539 = 1337 */
    snes_bus_write(0x4206, 42);
    const unsigned quot = snes_bus_read(0x4214) | snes_bus_read(0x4215) << 8;
    CHECK(quot == 1337 / 42 && snes_bus_read(0x4216) == 1337 % 42);
    snes_bus_write(0x4204, 0x00); snes_bus_write(0x4205, 0x01);
    snes_bus_write(0x4206, 0);
    CHECK(snes_bus_read(0x4214) == 0xFF && snes_bus_read(0x4215) == 0xFF);
    CHECK(snes_bus_read(0x4216) == 0xFF); /* the /0 remainder */
}

int main(void) {
    rom[0] = 0x42;
    rom[0x7ffa] = rom[0x7fea] = rom[0x7ffc] = 0;
    rom[0x7ffb] = rom[0x7feb] = rom[0x7ffd] = 0x80;
    snes_init(rom, sizeof rom, 0, 0);
    CHECK(snes_bus_read(0x808000) == 0x42);
    hdma_test();
    muldiv_test();
    irq_test();
    timing_test();
    object_limits_test();
    modes_test();
    apu_test();
    /* the SRAM window: mirrored through the $6000-$7FFF offsets of the
     * LoROM banks, open bus when nothing is declared */
    snes_init(rom, sizeof rom, 0, 8192);
    CHECK(snes_bus_read(0x7f7000) == 0);
    snes_bus_write(0x006001, 0x5a);
    CHECK(machine.sram[1] == 0x5a && machine.sram_dirty);
    CHECK(snes_bus_read(0x806001) == 0x5a); /* the mirror of bank $80 */
    machine.sram_dirty = 0;
    snes_bus_write(0x7e6001, 0x99); /* bank $7E is WRAM, never the SRAM */
    CHECK(!machine.sram_dirty && machine.wram[0x6001] == 0x99);
    snes_init(rom, sizeof rom, 0, 2048);
    snes_bus_write(0x006801, 0x77);
    CHECK(machine.sram[1] == 0x77);
    CHECK(snes_bus_read(0x006001) == 0x77); /* the 2 KiB window mirrors */
    snes_init(rom, sizeof rom, 0, 0);
    snes_bus_write(0x006001, 0x44);
    CHECK(!machine.sram_dirty && machine.sram[1] == 0);
    snes_bus_write(0x800123, 0x56);
    CHECK(snes_bus_read(0x000123) == 0x56);
    snes_bus_write(0x7f0123, 0x78);
    CHECK(snes_bus_read(0x7f0123) == 0x78);
    CHECK(snes_bus_read(0x000123) == 0x56);
    snes_bus_write(0x2181, 0xff);
    snes_bus_write(0x2182, 0xff);
    snes_bus_write(0x2183, 1);
    snes_bus_write(0x2180, 0xa5);
    snes_bus_write(0x2180, 0x5a);
    CHECK(machine.wram[0x1ffff] == 0xa5 && machine.wram[0] == 0x5a);
    CHECK(machine.cpu.wram_addr == 1);
    interrupt_test(0, 0);
    interrupt_test(1, 0);
    interrupt_test(0, 1);
    interrupt_test(1, 1);
    cpu_flow_test();
    video_test();
    object_test();
    joypad_test();
    cpu_index_test();
    return 0;
}
