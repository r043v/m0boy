#include "s3c2400.h"
#include "gp32_exec.h"
#include "gp32_hle.h"

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t bios[GP32_BIOS_SIZE] = {
    0xfe, 0xff, 0xff, 0xea, /* ARM branch to reset vector */
    0x00, 0xf8, 0xc0, 0x07,
    0x3e, 0x00, 0xff, 0xff
};

static void put_word(uint8_t *data, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        data[i] = value >> (i * 8);
}

/* Synthetic package: an image longer than one stride, whose first `prefix`
   bytes live in the GXE and whose stride heads are XORed with the table. */
static void test_zpk(size_t gxe_size, size_t relative, size_t prefix,
                     const uint8_t *header, size_t header_size)
{
    enum { GXE = 0x175, STRIDE = 0x400, LENGTH = 0x900 };
    const size_t gxc = GXE + 20 + gxe_size, content = gxc + 40;
    const size_t total = prefix + LENGTH, shift = relative - 0x410;
    const size_t size = content + LENGTH;
    uint8_t image[0x100 + LENGTH], key[96];
    assert(header_size <= sizeof(image));
    for (size_t i = 0; i < total; ++i)
        image[i] = i * 7 + 3;
    memcpy(image, header, header_size);
    for (unsigned i = 0; i < sizeof(key); ++i)
        key[i] = i * 5 + 0x87;
    uint8_t *zpk = calloc(1, size), *packed = malloc(total);
    assert(zpk && packed);
    memcpy(packed, image, total);
    for (size_t offset = 0; offset < total; offset += STRIDE)
        for (size_t i = 0; i < 256 && i < total - offset; ++i)
            packed[offset + i] ^= key[i % sizeof(key)];
    memcpy(zpk, "ZPK ", 4);
    memcpy(zpk + GXE, "GXE ", 4);
    put_word(zpk + GXE + 16, gxe_size);
    uint8_t *gxe = zpk + GXE + 20;
    put_word(gxe + 0x70, relative);
    put_word(gxe + 0x74 + relative + 44, 0x228 + prefix);
    put_word(gxe + 0x560 + shift, STRIDE);
    memcpy(gxe + 0x5dc + shift, packed, prefix);
    memcpy(gxe + 0x5dc + shift + prefix, key, sizeof(key));
    memcpy(zpk + gxc, "GXC ", 4);
    put_word(zpk + gxc + 16, 20 + LENGTH);
    put_word(zpk + gxc + 20, LENGTH);
    memcpy(zpk + content, packed + prefix, LENGTH);

    uint8_t *rom;
    size_t rom_size;
    assert(!gp32_exec_boot_image(bios, zpk, size, &rom, &rom_size));
    assert(rom_size >= 0x8004 + total && !memcmp(rom + 0x8004, image, total));
    free(rom);
    memset(zpk + gxc, 0, 4); /* a blank tag is tolerated */
    assert(!gp32_exec_boot_image(bios, zpk, size, &rom, &rom_size));
    free(rom);
    assert(gp32_exec_boot_image(bios, zpk, size - 1, &rom, &rom_size));
    put_word(gxe + 0x74 + relative + 44, 0x428);
    assert(gp32_exec_boot_image(bios, zpk, size, &rom, &rom_size));
    free(packed);
    free(zpk);
}

typedef struct {
    unsigned count;
    char log[128];
} Resources;

static bool log_resource(void *context, const char *path, const uint8_t *data, size_t size)
{
    Resources *resources = context;
    size_t used = strlen(resources->log);
    snprintf(resources->log + used, sizeof(resources->log) - used, "%s:%.*s;",
             path, (int)size, data ? (const char *)data : "");
    ++resources->count;
    return true;
}

/* An SZDD stream made of literals, after two empty chunks. */
static const char *unpack_archive(const uint8_t *archive, size_t length, Resources *resources)
{
    enum { GXE = 0x175, SZDD = GXE + 40 };
    uint8_t zpk[SZDD + 14 + 256] = {'Z', 'P', 'K', ' '};
    memcpy(zpk + GXE, "GXE ", 4);
    memcpy(zpk + GXE + 20, "GXC ", 4);
    memcpy(zpk + SZDD, "SZDD\x88\xf0\x27\x33", 8);
    put_word(zpk + SZDD + 10, length);
    size_t at = SZDD + 14;
    for (size_t i = 0; i < length; ++i) {
        if (!(i % 8))
            zpk[at++] = 0xff;
        zpk[at++] = archive[i];
    }
    assert(at <= sizeof(zpk));
    *resources = (Resources){0};
    return gp32_exec_resources(zpk, at, log_resource, resources);
}

static void test_resources(void)
{
    static const char good[] = "\x30\x09gp:\\GAME\0"
        "\x31\x09gp:\\GAME\0" "\x06" "A.DAT\0" "\x02\0\0\0" "hi" "\x32";
    static const char parent[] = "\x30\x0dgp:\\GAME\\..\\\0";
    static const char slash[] = "\x31\x05gp:\\\0" "\x05" "../x\0" "\0\0\0\0";
    Resources resources;
    assert(!unpack_archive((const uint8_t *)good, sizeof(good) - 1, &resources));
    assert(resources.count == 2 && !strcmp(resources.log, "GAME:;GAME/A.DAT:hi;"));
    assert(unpack_archive((const uint8_t *)parent, sizeof(parent) - 1, &resources) && !resources.count);
    assert(unpack_archive((const uint8_t *)slash, sizeof(slash) - 1, &resources) && !resources.count);
    assert(unpack_archive((const uint8_t *)good, sizeof(good) - 5, &resources));
    assert(!gp32_exec_resources((const uint8_t *)"GXB", 3, log_resource, &resources));
    uint8_t header[0x175] = "ZPK ";
    char directory[256];
    memcpy(header + 0x6d, "gp:\\GAME\\x", 11);
    gp32_exec_directory(header, sizeof(header), directory);
    assert(!strcmp(directory, "gp:\\GAME\\x"));
    memset(header + 0x6d, 'a', 255);
    gp32_exec_directory(header, sizeof(header), directory);
    assert(!*directory);
}

static void test_executable(void)
{
    test_resources();
    static const uint8_t gxb[] = {
        5, 0, 0, 0xea, 0, 0, 0, 0x0c,
        32, 0, 0, 0x0c, 32, 0, 0, 0x0c,
        40, 0, 0, 0x0c, 32, 0, 0, 0x0c,
        40, 0, 0, 0x0c, 0xfe, 0xff, 0xff, 0xea,
        0x23, 0x42
    };
    uint8_t fxe[0x464 + sizeof(gxb)] = {
        'f', 'x', 'e', ' ', [8] = 0x54, [9] = 4,
        [0x45c] = 32, [0x460] = 2
    };
    for (unsigned i = 0; i < 2; ++i)
        fxe[0x464 + i] = gxb[32 + i] ^ 0xff;
    for (unsigned i = 0; i < 32; ++i)
        fxe[0x466 + i] = gxb[i] ^ gxb[32 + i % 2];

    uint8_t *image;
    size_t size;
    assert(!gp32_exec_boot_image(bios, fxe, sizeof(fxe), &image, &size));
    assert(size == sizeof(bios) && !memcmp(image, bios, 0x8000));
    assert(image[0x8000] == 36 && !memcmp(image + 0x8004, gxb, sizeof(gxb)));
    assert(image[0x8004 + sizeof(gxb)] == 0);
    free(image);
    assert(gp32_exec_boot_image(bios, fxe, sizeof(fxe) - 1, &image, &size));
    assert(!gp32_exec_boot_image(bios, gxb, sizeof(gxb), &image, &size));
    free(image);
    assert(!gp32_exec_boot_image(bios, bios, sizeof(bios), &image, &size));
    assert(size == sizeof(bios) && !memcmp(image, bios, size));
    free(image);

    /* The three observed profiles: reference padding, longer padding, and
       the head of the image kept in the GXE. */
    test_zpk(0x6dc, 0x410, 0, gxb, sizeof(gxb));
    test_zpk(0x12dc, 0x1010, 0, gxb, sizeof(gxb));
    test_zpk(0x7dc, 0x410, 0x100, gxb, sizeof(gxb));

    uint8_t gxe[0x74 + 52 + 8] = {
        0x6c, [0x70] = 4, [0x78 + 48] = 1, [0x78 + 50] = 4
    };
    assert(gp32_exec_identity(gxb, sizeof(gxb)) == GP32_EXEC_ID_NONE);
    assert(gp32_exec_identity(gxe, sizeof(gxe)) == GP32_EXEC_ID_SMC);
    gxe[0x78 + 48] = 2;
    assert(gp32_exec_identity(gxe, sizeof(gxe)) == GP32_EXEC_ID_MACHINE);
    assert(gp32_exec_identity(gxe, sizeof(gxe) - 1) == GP32_EXEC_ID_UNKNOWN);

    static const char hex[] = "00112233445566778899aAbBcCdDeEfF";
    static const char mask[] = "TASTERSCHOICE";
    uint8_t record[64] = {0}, id[16];
    for (unsigned i = 0; i < 32; ++i)
        record[32 + i] = hex[i] ^ mask[(32 + i) % 13];
    assert(gp32_exec_read_identity(record, id));
    for (unsigned i = 0; i < sizeof(id); ++i)
        assert(id[i] == i * 17);
    record[63] = (uint8_t)mask[63 % 13];
    assert(!gp32_exec_read_identity(record, id));
}

/* The loader clears ZI behind the loaded bytes, also when the RW limit of
   the header covers it. */
static void test_load_ram(void)
{
    static const uint8_t gxb[] = {
        5, 0, 0, 0xea, 0, 0, 0, 0x0c,
        32, 0, 0, 0x0c, 32, 0, 0, 0x0c,
        48, 0, 0, 0x0c, 40, 0, 0, 0x0c,
        48, 0, 0, 0x0c, 0xfe, 0xff, 0xff, 0xea,
        0x23, 0x42
    };
    S3c2400 *s = malloc(sizeof(*s));
    assert(s && s3c2400_boot(s, bios, sizeof(bios), (Arm920tConfig){0},
                             (S3c2400Hooks){0}, (S3c2400Video){0}));
    memset(s->ram, 0xaa, 64);
    size_t loaded;
    assert(!gp32_exec_load_ram(s, gxb, sizeof(gxb), 1024, &loaded));
    assert(loaded == sizeof(gxb) && s->ram[33] == 0x42 && s->ram[39] == 0xaa);
    for (unsigned i = 40; i < 48; ++i)
        assert(!s->ram[i]);
    assert(s->ram[48] == 0xaa);
    free(s);
}

typedef struct {
    unsigned copies, lcd_reads, frames, audio_frames;
} Events;

static void access_event(void *context, const S3c2400Access *access)
{
    Events *events = context;
    if (access->kind == S3C2400_LCD_LOAD) {
        assert(access->accepted && access->region == S3C2400_SDRAM && access->width == 2);
        ++events->lcd_reads;
    }
}

static void copy_event(void *context, const S3c2400Access *source,
                       const S3c2400Access *target)
{
    Events *events = context;
    assert(source->region == S3C2400_BOOT_ROM);
    assert(target->region == S3C2400_SDRAM || target->address == 0x15508010);
    assert(source->value == target->value);
    ++events->copies;
}

static void frame_event(void *context, const uint32_t *pixels, unsigned width, unsigned height)
{
    Events *events = context;
    assert(width == 240 && height == 320);
    assert(pixels[0] == 0xfffb0000u && pixels[1] == 0xff00fb00u);
    assert(pixels[width] == 0xff0000fbu && pixels[width + 1] == 0xffffffffu);
    ++events->frames;
}

static void write_register(S3c2400 *s, uint32_t address, uint32_t value)
{
    bool accepted = s3c2400_bus_write(&s->bus, S3C2400_STORE, address, 4, value).accepted;
    assert(accepted);
    (void)accepted;
}

static uint32_t read_register(S3c2400 *s, uint32_t address)
{
    S3c2400Access result = s3c2400_bus_peek(&s->bus, address, 4);
    assert(result.accepted);
    return result.value;
}

/* Raw rows carry the same fetches as the converted image, with or without a
   bus observer. */
static void test_lcd_capture(S3c2400 *s, Events *events)
{
    Gp32LcdCapture *capture = calloc(1, sizeof(*capture));
    assert(capture);
    s->lcd_capture = capture;
    S3c2400Hooks hooks = s->bus.hooks;
    uint32_t first = s3c2400_bus_peek(&s->bus, S3C2400_SDRAM_BASE + GP32_RAM_SIZE - 8, 4).value;
    uint32_t second = s3c2400_bus_peek(&s->bus, S3C2400_SDRAM_BASE + GP32_RAM_SIZE - 4, 4).value;
    for (unsigned observed = 0; observed < 2; ++observed) {
        s->bus.hooks = observed ? hooks : (S3c2400Hooks){0};
        unsigned reads = events->lcd_reads;
        uint64_t frame = s->frames;
        for (unsigned step = 0; step < 100 && s->frames == frame; ++step)
            s3c2400_step(s);
        assert(s->frames == frame + 1 && events->frames == 1);
        assert(events->lcd_reads == reads + (observed ? 4 : 0));
        const Gp32LcdFrame *done = &capture->frames[capture->back ^ 1u];
        assert((done->lines[0][GP32_LCD_FORMAT] & 255u) == 16);
        assert(done->lines[0][GP32_LCD_COUNT] == 2 && done->lines[2][GP32_LCD_COUNT] == 0);
        assert(done->lines[0][GP32_LCD_HEADER] == first);
        assert(done->lines[1][GP32_LCD_HEADER] == second);
    }
    /* Indexed rows share one palette row until the palette changes. */
    write_register(s, 0x14a00000, (11u << 1) | (3u << 5) | 1);
    s->palette[7] = 0x1234;
    uint64_t frame = s->frames;
    for (unsigned step = 0; step < 100 && s->frames < frame + 2; ++step)
        s3c2400_step(s);
    const Gp32LcdFrame *done = &capture->frames[capture->back ^ 1u];
    assert(done->palette_count == 1 && done->palettes[0][7] == 0x1234);
    assert(done->lines[1][GP32_LCD_PALETTE_ROW] == 0);
    s->lcd_capture = NULL;
    free(capture);
}

/* LCDCON5 reports the vertical and horizontal phase of the scan. */
static void test_lcd_status(S3c2400 *s)
{
    unsigned lines = 4 + ((s->lcd[1] >> 14) & 1023u) + (s->lcd[1] & 63u) +
                     ((s->lcd[1] >> 6) & 255u) + (s->lcd[1] >> 24);
    unsigned active = ((s->lcd[1] >> 14) & 1023u) + 1, vertical = 0, horizontal = 0;
    write_register(s, 0x14a00000, read_register(s, 0x14a00000) | 1);
    S3c2400Video video = s->video;
    s->video.frame = NULL;
    uint64_t frame = s->frames;
    s->cycles_per_instruction = 1;
    for (unsigned step = 0; step < 2000000 && s->frames < frame + 2; ++step) {
        s3c2400_step(s);
        uint32_t status = read_register(s, 0x14a00010);
        unsigned v = (status >> 19) & 3u;
        assert(s->line < lines && (v == 2) == (s->line < active));
        vertical |= 1u << v;
        horizontal |= 1u << ((status >> 17) & 3u);
    }
    assert(vertical == 15 && horizontal == 15);
    s->video = video;
    s->cycles_per_instruction = 1;
}

/* TICNT raises the tick interrupt every (count + 1) / 128 second, with or
   without idle spans. */
static void test_rtc_tick(void)
{
    static const uint32_t program[] = {0xeafffffe}; /* b . */
    uint64_t when[2];
    for (unsigned m = 0; m < 2; ++m) {
        S3c2400 *s = malloc(sizeof(*s));
        assert(s && s3c2400_boot(s, bios, sizeof(bios), (Arm920tConfig){0},
                                 (S3c2400Hooks){0}, (S3c2400Video){0}));
        write_register(s, S3C2400_SDRAM_BASE, program[0]);
        s->cpu.r[15] = S3C2400_SDRAM_BASE;
        s->cycles_per_instruction = 16;
        write_register(s, 0x15700044, 0x81);
        while (!(s->irq[0] & (1u << 8))) {
            if (m)
                assert(s3c2400_run(s, 100000) == S3C2400_STOP_STEPS);
            else
                s3c2400_step(s);
            assert(s->cycles < 100000000u);
        }
        when[m] = s->cycles;
        write_register(s, 0x15700044, 0);
        free(s);
    }
    assert(when[0] > 16 && when[1] >= when[0] && when[1] - when[0] <= 1600000);
}

/* A machine spinning in RAM, for the timed peripherals. */
static S3c2400 *idle_machine(bool rom)
{
    S3c2400 *s = malloc(sizeof(*s));
    assert(s && s3c2400_boot(s, rom ? bios : NULL, rom ? sizeof(bios) : 0, (Arm920tConfig){0},
                             (S3c2400Hooks){0}, (S3c2400Video){0}));
    write_register(s, 0x15300000, 0);
    write_register(s, S3C2400_SDRAM_BASE, 0xeafffffe); /* b . */
    s->cpu.r[15] = S3C2400_SDRAM_BASE;
    s->cycles_per_instruction = 16;
    return s;
}

static void run_until_pending(S3c2400 *s, unsigned source, uint64_t limit)
{
    while (!(s->irq[0] & (1u << source))) {
        assert(s3c2400_run(s, 100000) == S3C2400_STOP_STEPS);
        assert(s->cycles < limit);
    }
}

static void test_watchdog(void)
{
    /* Interrupt mode: 100 counts of 16 PCLK, then the reload value. */
    S3c2400 *s = idle_machine(true);
    write_register(s, 0x15300004, 50);
    write_register(s, 0x15300008, 100);
    write_register(s, 0x15300000, 0x24);
    uint64_t start = s->cycles;
    while (!(s->irq[0] & (1u << 9)))
        s3c2400_step(s);
    assert(s->cycles - start >= 1600 && s->cycles - start <= 1616);
    assert(read_register(s, 0x15300008) <= 50 && read_register(s, 0x15300008) >= 49);
    write_register(s, 0x14400000, 1u << 9);
    start = s->cycles;
    run_until_pending(s, 9, start + 10000000);
    free(s);

    /* The batched loop and single steps expire together. */
    uint32_t count[2];
    for (unsigned m = 0; m < 2; ++m) {
        s = idle_machine(true);
        write_register(s, 0x15300008, 1000);
        write_register(s, 0x15300000, 0x0120);
        if (m)
            assert(s3c2400_run(s, 700) == S3C2400_STOP_STEPS);
        else
            for (unsigned i = 0; i < 700; ++i)
                s3c2400_step(s);
        count[m] = read_register(s, 0x15300008);
        free(s);
    }
    assert(count[0] == count[1] && count[0] == 1000 - 700 * 16 / 32);

    /* Reset mode restarts the boot ROM and keeps the memory. */
    s = idle_machine(true);
    write_register(s, S3C2400_SDRAM_BASE + 64, 0x12345678);
    write_register(s, 0x15300008, 10);
    write_register(s, 0x15300000, 0x21);
    for (unsigned i = 0; i < 11; ++i)
        s3c2400_step(s);
    assert(s->cpu.r[15] < S3C2400_SDRAM_BASE && !s->hle_exited);
    assert(read_register(s, S3C2400_SDRAM_BASE + 64) == 0x12345678);
    assert(read_register(s, 0x15300000) == 0x8021);
    free(s);

    /* Without a boot ROM the application has ended. */
    s = idle_machine(false);
    write_register(s, 0x15300008, 10);
    write_register(s, 0x15300000, 0x21);
    assert(s3c2400_run(s, 100000) == S3C2400_STOP_EXIT);
    free(s);
}

static void test_calendar(void)
{
    S3c2400 *s = idle_machine(true);
    s3c2400_set_time(s, (S3c2400Time){.year = 4, .month = 2, .day = 28, .weekday = 7,
                                      .hour = 23, .minute = 59, .second = 59});
    assert(read_register(s, 0x15700070) == 0x59 && read_register(s, 0x15700084) == 2);
    write_register(s, 0x15700054, 0x00);      /* alarm on second 0 */
    write_register(s, 0x15700050, 0x41);
    /* A short second keeps the test quick. */
    s->late.rtc_second = s->cycles + 5000;
    run_until_pending(s, 30, s->cycles + 100000000);
    assert(read_register(s, 0x15700070) == 0 && read_register(s, 0x15700074) == 0);
    assert(read_register(s, 0x15700078) == 0 && read_register(s, 0x1570007c) == 0x29);
    assert(read_register(s, 0x15700080) == 1 && read_register(s, 0x15700088) == 4);
    assert(s->late.rtc_second > s->cycles + 1000000);
    free(s);
}

static void test_serial_and_converters(void)
{
    S3c2400 *s = idle_machine(true);
    assert(read_register(s, 0x15004010) == 6);
    write_register(s, 0x15004004, 0x204);     /* level transmit requests */
    assert(s->irq[0] & (1u << 29));
    write_register(s, 0x14400000, 1u << 29);
    assert(s->irq[0] & (1u << 29));
    write_register(s, 0x15004004, 0);
    write_register(s, 0x14400000, 1u << 29);
    assert(!(s->irq[0] & (1u << 29)));

    write_register(s, 0x15000004, 0x21);      /* loopback, receive requests */
    write_register(s, 0x15000020, 'g');
    assert((s->irq[0] & (1u << 23)) && read_register(s, 0x15000010) == 7);
    S3c2400Access byte = s3c2400_bus_read(&s->bus, S3C2400_LOAD, 0x15000024, 4);
    assert(byte.accepted && byte.value == 'g' && read_register(s, 0x15000010) == 6);

    write_register(s, 0x15800000, 0x4001);
    assert(read_register(s, 0x15800000) == 0xc000 && read_register(s, 0x15800004) == 0x3ff);
    assert(s->irq[0] & (1u << 31));
    assert(read_register(s, 0x15900004) & 1);
    free(s);
}

static void nand_send(S3c2400 *s, unsigned latch, uint8_t value)
{
    write_register(s, 0x15600008, 0x5555);
    write_register(s, 0x1560000c, value);
    write_register(s, 0x15600030, latch);
    write_register(s, 0x15600030, latch | 8);
}

static void nand_page(S3c2400 *s, unsigned page, unsigned column)
{
    nand_send(s, 16, column);
    nand_send(s, 16, page & 255);
    nand_send(s, 16, page >> 8);
}

static unsigned nand_read(S3c2400 *s)
{
    write_register(s, 0x15600008, 0);
    write_register(s, 0x15600030, 8);
    write_register(s, 0x15600024, 0x40);
    unsigned value = read_register(s, 0x1560000c) & 255;
    assert((read_register(s, 0x1560000c) & 255) == value);
    write_register(s, 0x15600024, 0x140);
    return value;
}

static void test_smartmedia(S3c2400 *s)
{
    size_t size = 8192 * 528;
    uint8_t *image = malloc(size);
    assert(image);
    memset(image, 0xff, size);
    assert(read_register(s, 0x15600030) & 4);
    assert(gp32_smc_attach(&s->smc, image, size, true));
    assert(!(read_register(s, 0x15600030) & 4));
    write_register(s, 0x15600024, 0x140);
    nand_send(s, 32, 0x90);
    nand_send(s, 16, 0);
    assert(nand_read(s) == 0xec && nand_read(s) == 0xe3);
    nand_send(s, 32, 0x50);
    nand_send(s, 32, 0x80);
    nand_page(s, 16, 1);
    nand_send(s, 0, 0x7b);
    nand_send(s, 32, 0x10);
    assert(image[16 * 528 + 513] == 0x7b && s->smc.dirty);
    nand_send(s, 32, 0x50);
    nand_page(s, 16, 0);
    assert(nand_read(s) == 0xff && nand_read(s) == 0x7b);
    s->smc.writable = false;
    nand_send(s, 32, 0x60);
    nand_send(s, 16, 17);
    nand_send(s, 16, 0);
    nand_send(s, 32, 0xd0);
    nand_send(s, 32, 0x70);
    assert(nand_read(s) == 0x41 && image[16 * 528 + 513] == 0x7b);
    s->smc.writable = true;
    nand_send(s, 32, 0x60);
    nand_send(s, 16, 17);
    nand_send(s, 16, 0);
    nand_send(s, 32, 0xd0);
    nand_send(s, 32, 0x70);
    assert(nand_read(s) == 0xc0 && image[16 * 528 + 513] == 0xff);
    const uint8_t uid[16] = {0x12, 0x34};
    assert(gp32_smc_set_uid(&s->smc, uid));
    assert(!memcmp(gp32_smc_uid(&s->smc), uid, sizeof(uid)));
    nand_send(s, 32, 0x90);
    nand_send(s, 16, 0);
    assert(nand_read(s) == 0xec && nand_read(s) == 0xe3 && nand_read(s) == 0xa5);
    nand_send(s, 32, 0x30);
    nand_send(s, 32, 0x65);
    nand_send(s, 32, 0x00);
    nand_page(s, 0, 0);
    for (unsigned i = 0; i < 32; ++i)
        assert(nand_read(s) == (uid[i % 16] ^ (i < 16 ? 0 : 255)));
    nand_send(s, 32, 0x50);
    nand_page(s, 0, 0);
    assert(nand_read(s) == 0x55 && nand_read(s) == 0xaa);
    assert(gp32_smc_attach(&s->smc, NULL, 0, false));
    free(image);
}

static void iic_tx(S3c2400 *s, unsigned value)
{
    write_register(s, 0x1540000c, value);
    write_register(s, 0x15400000, 0xa0);
    assert(read_register(s, 0x15400000) & 0x10);
}

static void test_eeprom(S3c2400 *s)
{
    write_register(s, 0x15400000, 0xa0);
    write_register(s, 0x1540000c, 0xa0);
    write_register(s, 0x15400004, 0xf0);
    assert(!(read_register(s, 0x15400004) & 1));
    iic_tx(s, 0);
    iic_tx(s, 31);
    iic_tx(s, 0x5a);
    iic_tx(s, 0xa5);
    assert(!s->eeprom_dirty && s->eeprom[31] == 0xff);
    write_register(s, 0x15400004, 0xd0);
    assert(s->eeprom_dirty && s->eeprom[31] == 0x5a && s->eeprom[0] == 0xa5);
    write_register(s, 0x1540000c, 0xa0);
    write_register(s, 0x15400004, 0xf0);
    assert(read_register(s, 0x15400004) & 1);
    write_register(s, 0x15400004, 0xd0);
    s->cycles = s->eeprom_ready;
    write_register(s, 0x15400004, 0xf0);
    iic_tx(s, 0);
    iic_tx(s, 0);
    write_register(s, 0x1540000c, 0xa1);
    write_register(s, 0x15400004, 0xb0);
    write_register(s, 0x15400000, 0xa0);
    assert(!(read_register(s, 0x15400004) & 1));
    assert(read_register(s, 0x1540000c) == 0xa1);
    write_register(s, 0x15400000, 0x20);
    assert(read_register(s, 0x1540000c) == 0xa5);
    write_register(s, 0x15400004, 0x90);
}

static void sample_event(void *context, int16_t left, int16_t right, unsigned rate)
{
    Events *events = context;
    assert(rate == 468750);
    if (events->audio_frames != 1) /* only the explicit mute silences */
        assert(left == (int16_t)0xf800 && right == 0x07c0);
    else
        assert(left == 0 && right == 0);
    ++events->audio_frames;
}

static void codec_byte(S3c2400 *s, unsigned mode, unsigned value)
{
    for (unsigned i = 0; i < 8; ++i) {
        unsigned data = mode | ((value >> i & 1u) << 11);
        write_register(s, 0x15600030, data);
        write_register(s, 0x15600030, data | 0x200);
    }
}

static void test_audio(S3c2400 *s, Events *events)
{
    s->audio = (S3c2400Audio){sample_event, events};
    write_register(s, 0x15508004, 0x88);
    write_register(s, 0x1550800c, 0xa00);
    write_register(s, 0x14600040, 4);
    write_register(s, 0x14600044, (1u << 29) | 0x15508010);
    write_register(s, 0x14600048, (1u << 23) | (1u << 22) | (1u << 20) | 2);
    write_register(s, 0x14600058, 2);
    write_register(s, 0x15508000, 0x21);
    for (unsigned step = 0; step < 256; ++step)
        s3c2400_step(s);
    assert(events->audio_frames == 1 && events->copies == 4);
    write_register(s, 0x1560002c, 0x15u << 18);
    codec_byte(s, 0, 0x14);
    write_register(s, 0x15600030, 0x600);
    codec_byte(s, 0x400, 0x84);
    assert(s->codec_mute);
    write_register(s, 0x14600058, 2);
    for (unsigned step = 0; step < 256; ++step)
        s3c2400_step(s);
    assert(events->audio_frames == 2 && events->copies == 6);
    codec_byte(s, 0x400, 0x80);
    codec_byte(s, 0x400, 0x3f);
    assert(!s->codec_mute && s->codec_volume == 0x3f);

    s->cycles_per_instruction = 16;
    uint64_t cycles = s->cycles;
    write_register(s, 0x14600058, 2);
    for (unsigned step = 0; step < 16; ++step)
        s3c2400_step(s);
    assert(events->audio_frames == 3 && events->copies == 8);
    write_register(s, 0x14600058, 2);
    for (unsigned step = 0; step < 16; ++step)
        s3c2400_step(s);
    assert(events->audio_frames == 4 && events->copies == 10);
    assert(s->cycles == cycles + 512);

}

static uint32_t hle_smc_call(S3c2400 *s, uint32_t table, unsigned entry, uint32_t arg)
{
    s->cpu.r[15] = read_register(s, table + 4 * entry);
    s->cpu.r[14] = S3C2400_SDRAM_BASE + 0x84;
    s->cpu.r[0] = arg;
    assert(s3c2400_step(s) == ARM920T_INSTRUCTION);
    assert(s3c2400_step(s) == ARM920T_INSTRUCTION &&
           s->cpu.r[15] == S3C2400_SDRAM_BASE + 0x84);
    return s->cpu.r[0];
}

/* s3c2400_run does not repeat a display wait one step at a time, yet must
   leave it on the same step as s3c2400_step.  The machine is put back. */
static void test_flip_wait(S3c2400 *s, uint32_t resume)
{
    S3c2400 *before = malloc(sizeof(*s)), *stepped = malloc(sizeof(*s));
    assert(before && stepped);
    write_register(s, resume, 0xeafffffeu); /* b . */
    memcpy(before, s, sizeof(*s));
    const uint64_t first = s->steps, skipped = s->skipped_steps;
    while (s->cpu.r[15] != resume) {
        assert(s->steps - first < 10000000);
        s3c2400_step(s);
    }
    const uint64_t length = s->steps - first;
    memcpy(stepped, s, sizeof(*s));
    memcpy(s, before, sizeof(*s));
    while (s->steps - first < length)
        assert(s3c2400_run(s, length - (s->steps - first)) != S3C2400_STOP_EXCEPTION);
    assert(!memcmp(s->cpu.r, stepped->cpu.r, sizeof(s->cpu.r)));
    assert(s->cpu.cpsr == stepped->cpu.cpsr && s->steps == stepped->steps);
    assert(s->cycles == stepped->cycles && s->frames == stepped->frames);
    assert(s->line == stepped->line && s->line_ticks == stepped->line_ticks);
    assert(s->skipped_steps - skipped > length / 2);
    memcpy(s, before, sizeof(*s));
    free(before);
    free(stepped);
}

static void test_hle(S3c2400 *s)
{
    static const uint8_t app[] = {
        5, 0, 0, 0xea, 0, 0, 0, 0x0c,
        44, 0, 0, 0x0c, 44, 0, 0, 0x0c,
        48, 0, 0, 0x0c, 44, 0, 0, 0x0c,
        48, 0, 0, 0x0c,
        0x11, 0, 0, 0xef, /* SWI: card inserted */
        0x05, 1, 0, 0xef, /* SWI: EEPROM access */
        0x03, 1, 0, 0xef, /* SWI: SmartMedia ID */
        0x04, 0, 0, 0xef  /* SWI: exit */
    };
    const uint32_t base = S3C2400_SDRAM_BASE, buffer = base + 0x100;
    const Gp32Smc fake_card = {.image = s->ram + 0x200, .uid_override = true, .uid = {0x51}};
    assert(!gp32_hle_boot(s, app, sizeof(app), (Arm920tConfig){0},
                         (S3c2400Hooks){0}, (S3c2400Video){0}));
    assert((s->lcd[0] & 31) == 23 && s->palette[255] == 0xfffe);
    assert(!s->bus.boot_rom && s->cpu.r[15] == base && s->cpu.r[2] == sizeof(app));
    assert(s3c2400_step(s) == ARM920T_INSTRUCTION && s->cpu.r[15] == base + 28);
    assert(s3c2400_step(s) == ARM920T_INSTRUCTION && s->cpu.r[0] == 0);
    s->smc = fake_card;
    s->cpu.r[15] = base + 28;
    assert(s3c2400_step(s) == ARM920T_INSTRUCTION && s->cpu.r[0] == 1);
    s->cpu.r[0] = 3;
    s->cpu.r[1] = 1;
    s->cpu.r[2] = buffer;
    s->cpu.r[3] = 1;
    s->ram[0x100] = 0x75;
    assert(s3c2400_step(s) == ARM920T_INSTRUCTION && s->cpu.r[0] == 0);
    assert(s->eeprom_dirty && s->eeprom[3] == 0x75);
    s->cpu.r[0] = buffer;
    assert(s3c2400_step(s) == ARM920T_INSTRUCTION && s->cpu.r[0] == 0);
    assert(s->ram[0x100] == 0x51);
    assert(s3c2400_step(s) == ARM920T_INSTRUCTION && s->hle_exited);

    /* A packed startup stub can chainload from overlapping SDRAM. */
    assert(!gp32_hle_boot(s, app, sizeof(app), (Arm920tConfig){0},
                         (S3c2400Hooks){0}, (S3c2400Video){0}));
    write_register(s, base + 4, sizeof(app));
    memcpy(s->ram + 8, app, sizeof(app));
    write_register(s, base + 0x80, 0xef000005u);
    s->cpu.r[0] = base + 4;
    s->cpu.r[1] = 0;
    s->cpu.r[15] = base + 0x80;
    assert(s3c2400_step(s) == ARM920T_INSTRUCTION && s->cpu.r[15] == base);
    assert(!memcmp(s->ram, app, sizeof(app)));

    /* Chainload initializes separated RW/ZI even if the decompressor used them. */
    memcpy(s->ram + 0x304, app, sizeof(app));
    write_register(s, base + 0x300, sizeof(app));
    write_register(s, base + 0x30c, base + 40);
    write_register(s, base + 0x310, base + 0x200);
    write_register(s, base + 0x314, base + 0x208);
    write_register(s, base + 0x318, base + 0x208);
    write_register(s, base + 0x31c, base + 0x210);
    memset(s->ram + 0x200, 0xa5, 16);
    s->cpu.r[0] = base + 0x300;
    s->cpu.r[15] = base + 0x80;
    assert(s3c2400_step(s) == ARM920T_INSTRUCTION && s->cpu.r[15] == base);
    assert(read_register(s, base + 0x200) == 0xef000004u);
    for (unsigned i = 0x204; i < 0x210; ++i)
        assert(s->ram[i] == 0);

    /* Palette fades move one step per call, outside the visible lines. */
    write_register(s, base + 0x80, 0xef000016u);
    uint32_t lcd_control = s->lcd[0];
    s->lcd[0] &= ~1u;
    s->palette[0] = 0x0843; /* 1, 1, 1, intensity */
    for (unsigned i = 0; i < 256; ++i)
        write_register(s, buffer + 4 * i, i ? 0 : 0x1801); /* red 3 */
    static const struct { uint32_t command, mask, target, expected, changed; } fades[] = {
        {3, 0, 0, 0x1043, 1}, /* only the channel below its target rises */
        {4, 0, 0, 0x1001, 1},
        {6, 0, 0, 0x1801, 1},
        {5, 1, 0, 0x1803, 1}, /* blue only, past its target */
    };
    for (unsigned i = 0; i < sizeof(fades) / sizeof(fades[0]); ++i) {
        s->cpu.r[15] = base + 0x80;
        s->cpu.r[0] = fades[i].command;
        s->cpu.r[1] = fades[i].command == 5 ? 1 : buffer;
        s->cpu.r[2] = fades[i].mask;
        s->cpu.r[3] = buffer;
        assert(s3c2400_step(s) == ARM920T_INSTRUCTION);
        assert(s->palette[0] == fades[i].expected);
        assert(fades[i].command == 3 || fades[i].command == 6 || s->cpu.r[0] >= fades[i].changed);
    }
    s->lcd[0] = lcd_control;

    /* Page attributes and the empty service leave registers alone. */
    static const uint32_t no_ops[] = {0xef000002u, 0xef00000cu};
    for (unsigned i = 0; i < 2; ++i) {
        write_register(s, base + 0x80, no_ops[i]);
        s->cpu.r[15] = base + 0x80;
        s->cpu.r[0] = 0x55;
        assert(s3c2400_step(s) == ARM920T_INSTRUCTION && s->cpu.r[0] == 0x55);
    }
    /* Scratch buffer bounds. */
    write_register(s, base + 0x80, 0xef00000eu);
    s->cpu.r[15] = base + 0x80;
    s->cpu.r[0] = buffer;
    s->cpu.r[1] = 1;
    assert(s3c2400_step(s) == ARM920T_INSTRUCTION);
    assert(read_register(s, buffer) == 0x0c7b3800u && read_register(s, buffer + 4) == 0x0c7b3900u);
    /* Semihosted time functions follow the frame count. */
    write_register(s, base + 0x80, 0xef123456u);
    uint64_t frames = s->frames;
    s->frames = 150;
    for (unsigned i = 0; i < 2; ++i) {
        s->cpu.r[15] = base + 0x80;
        s->cpu.r[0] = 0x10 + i;
        assert(s3c2400_step(s) == ARM920T_INSTRUCTION && s->cpu.r[0] == (i ? 2u : 250u));
    }
    s->frames = frames;
    /* A PCM stop request is accepted. */
    write_register(s, base + 0x80, 0xef000014u);
    write_register(s, buffer, 0x1000);
    s->cpu.r[15] = base + 0x80;
    s->cpu.r[0] = buffer;
    assert(s3c2400_step(s) == ARM920T_INSTRUCTION && s->cpu.r[0] == 1);
    /* A null pointer store is dropped, as the boot ROM drops it. */
    write_register(s, base + 0x80, 0xe5801000u); /* str r1, [r0] */
    s->cpu.r[15] = base + 0x80;
    s->cpu.r[0] = 0;
    assert(s3c2400_step(s) == ARM920T_INSTRUCTION && s->cpu.r[15] == base + 0x84);

    /* 8-bit scanout uses the SDK's default palette. */
    write_register(s, base + 0x80, 0xef000008u);
    s->cpu.r[15] = base + 0x80;
    s->cpu.r[0] = 8;
    s->cpu.r[1] = 0;
    s->cpu.r[2] = 0;
    assert(s3c2400_step(s) == ARM920T_INSTRUCTION && s->cpu.r[0] == 4);
    assert(s->palette[3] == 0x003e && s->palette[255] == 0xfffe);
    assert((s->lcd[0] & 31) == 23 && (s->lcd[4] & 3) == 2);

    s->cpu.r[15] = base + 0x80;
    s->cpu.r[0] = buffer;
    s->cpu.r[1] = 1;
    s->cpu.r[2] = 1;
    assert(s3c2400_step(s) == ARM920T_INSTRUCTION && s->cpu.r[0] == 0);
    uint32_t second_surface = read_register(s, buffer);
    assert(second_surface > 2 * s->lcd[5]);
    write_register(s, base + 0x80, 0xef00000bu);
    s->cpu.r[15] = base + 0x80;
    assert(s3c2400_step(s) == ARM920T_INSTRUCTION);
    uint32_t flip_entry = s->cpu.r[0];
    s->cpu.r[15] = flip_entry;
    s->cpu.r[0] = buffer;
    s->cpu.r[14] = base + 0x84;
    assert(s3c2400_step(s) == ARM920T_INSTRUCTION && s->cpu.r[15] == flip_entry);
    test_flip_wait(s, base + 0x84);
    s->line = GP32_LCD_HEIGHT;
    assert(s3c2400_step(s) == ARM920T_INSTRUCTION && 2 * s->lcd[5] == second_surface);
    assert(s3c2400_step(s) == ARM920T_INSTRUCTION && s->cpu.r[15] == base + 0x84);

    /* Timer 0 runs a guest callback through an IRQ and updates SDK ticks. */
    write_register(s, base + 0x80, 0xef000013u);
    write_register(s, base + 0x84, 0xeafffffeu); /* b . */
    write_register(s, base + 0x88, 0xe5801008u); /* str r1, [r0, #8] */
    write_register(s, base + 0x8c, 0xe12fff1eu); /* bx lr */
    write_register(s, buffer, 1);
    write_register(s, buffer + 4, 0);
    write_register(s, buffer + 8, base + 0x88);
    write_register(s, buffer + 12, 20000);
    write_register(s, buffer + 16, 1000);
    s->cpu.r[0] = buffer;
    s->cpu.r[15] = base + 0x80;
    arm920t_set_status(&s->cpu, 0x53);
    assert(s3c2400_step(s) == ARM920T_INSTRUCTION);
    write_register(s, buffer, 2);
    s->cpu.r[15] = base + 0x80;
    assert(s3c2400_step(s) == ARM920T_INSTRUCTION);
    s3c2400_set_irq(s, 10);
    for (unsigned i = 0; i < 14; ++i)
        assert(s3c2400_step(s) == ARM920T_INSTRUCTION);
    assert(s->cpu.r[15] == base + 0x84 && s->cpu.cpsr == 0x53);
    assert(read_register(s, 0x0c7b0398) == 1 && read_register(s, 0x0c7b03a0) == 1);
    assert(!s->cpu.irq);

    /* Games can install a raw IRQ handler for timer 4. */
    write_register(s, base + 0x80, 0xef000009u);
    write_register(s, base + 0x88, 0xe25ef004u); /* subs pc, lr, #4 */
    s->cpu.r[15] = base + 0x80;
    s->cpu.r[0] = 14;
    s->cpu.r[1] = base + 0x88;
    assert(s3c2400_step(s) == ARM920T_INSTRUCTION);
    s3c2400_set_irq(s, 14);
    assert(s3c2400_step(s) == ARM920T_INSTRUCTION && s->cpu.r[15] == base + 0x88);
    assert(s3c2400_step(s) == ARM920T_INSTRUCTION && s->cpu.r[15] == base + 0x84);
    assert(s->cpu.cpsr == 0x53 && !s->cpu.irq);
    write_register(s, base + 0x80, 0xef00000au);
    s->cpu.r[15] = base + 0x80;
    assert(s3c2400_step(s) == ARM920T_INSTRUCTION && (s->irq[2] & (1u << 14)));

    /* The SDK callback table drives the same NAND as direct GPIO access. */
    size_t card_size = 8192 * 528;
    uint8_t *card = malloc(card_size);
    assert(card && gp32_smc_attach(&s->smc, card, card_size, false));
    write_register(s, base + 0x80, 0xef00000bu);
    s->cpu.r[15] = base + 0x80;
    s->cpu.r[0] = 3;
    s->cpu.r[1] = buffer;
    assert(s3c2400_step(s) == ARM920T_INSTRUCTION);
    hle_smc_call(s, buffer, 9, 0);  /* Select the card. */
    hle_smc_call(s, buffer, 7, 0);  /* CLE high. */
    hle_smc_call(s, buffer, 0, 0x90);
    hle_smc_call(s, buffer, 8, 0);  /* CLE low. */
    hle_smc_call(s, buffer, 5, 0);  /* ALE high. */
    hle_smc_call(s, buffer, 0, 0);
    hle_smc_call(s, buffer, 6, 0);  /* ALE low. */
    hle_smc_call(s, buffer, 3, 0);  /* Read direction. */
    assert(hle_smc_call(s, buffer, 1, 0) == 0xec);
    assert(hle_smc_call(s, buffer, 1, 0) == 0xe3);
    assert(gp32_smc_attach(&s->smc, NULL, 0, false));
    free(card);

    /* Thumb calls return to the next halfword. Unknown calls take the vector. */
    write_register(s, base + 0x80, 0xdffedf11u);
    s->cpu.r[15] = base + 0x80;
    arm920t_set_status(&s->cpu, s->cpu.cpsr | 32u);
    assert(s3c2400_step(s) == ARM920T_INSTRUCTION && s->cpu.r[15] == base + 0x82);
    assert(s3c2400_step(s) == ARM920T_EXCEPTION && s->cpu.r[15] == 8);
    assert(s->hle_unhandled && s->hle_swi == 0xfe && s->hle_pc == base + 0x82);
}

/* s3c2400_run must reach the state s3c2400_step reaches, while skipping a
   loop that waits for a timer count to change. */
static void test_run(void)
{
    static const uint32_t program[] = {
        0xe59f0018, /* ldr r0, =TCNTO0 */
        0xe5901000, /* 1: ldr r1, [r0] */
        0xe1510002, /* cmp r1, r2 */
        0x0afffffc, /* beq 1b */
        0xe1a02001, /* mov r2, r1 */
        0xe2833001, /* add r3, r3, #1 */
        0xeafffff9, /* b 1b */
        0xe1a00000,
        0x15100014
    };
    static const unsigned costs[] = {1, 2, 16};
    for (unsigned c = 0; c < 3; ++c) {
        S3c2400 *machine[2];
        for (unsigned m = 0; m < 2; ++m) {
            S3c2400 *s = machine[m] = malloc(sizeof(*s));
            assert(s && s3c2400_boot(s, bios, sizeof(bios), (Arm920tConfig){0},
                                     (S3c2400Hooks){0}, (S3c2400Video){0}));
            for (unsigned i = 0; i < sizeof(program) / 4; ++i)
                write_register(s, S3C2400_SDRAM_BASE + 4 * i, program[i]);
            s->cpu.r[15] = S3C2400_SDRAM_BASE;
            s->cycles_per_instruction = costs[c];
            write_register(s, 0x15100000, 99);
            write_register(s, 0x1510000c, 50);
            write_register(s, 0x15100008, 2);
            write_register(s, 0x15100008, 9);
        }
        const uint64_t steps = 200000;
        for (uint64_t i = 0; i < steps; ++i)
            s3c2400_step(machine[0]);
        while (machine[1]->steps < steps)
            assert(s3c2400_run(machine[1], steps - machine[1]->steps) !=
                   S3C2400_STOP_EXCEPTION);
        assert(!memcmp(machine[0]->cpu.r, machine[1]->cpu.r, sizeof(machine[0]->cpu.r)));
        assert(machine[0]->cpu.cpsr == machine[1]->cpu.cpsr);
        assert(machine[0]->steps == machine[1]->steps);
        assert(machine[0]->cycles == machine[1]->cycles);
        assert(!memcmp(machine[0]->pwm, machine[1]->pwm, sizeof(machine[0]->pwm)));
        assert(!memcmp(machine[0]->timer_phase, machine[1]->timer_phase,
                       sizeof(machine[0]->timer_phase)));
        assert(machine[0]->cpu.r[3] > 10);
        assert(!machine[0]->skipped_steps);
        /* At 16 cycles per instruction a count lasts too few iterations. */
        assert(costs[c] == 16 || machine[1]->skipped_steps > steps / 2);
        free(machine[0]);
        free(machine[1]);
    }
}

/* A loop whose stores change nothing is skipped; one changing memory while
   coming back to the same registers never is. */
static void test_run_stores(void)
{
    static const uint32_t silent[] = {
        0xe3a0440c, /* mov r4, #0x0c000000 */
        0xe2844c01, /* add r4, r4, #0x100 */
        0xe5844000, /* 1: str r4, [r4] */
        0xeafffffd  /* b 1b */
    };
    static const uint32_t counting[] = {
        0xe3a0440c, /* mov r4, #0x0c000000 */
        0xe2844c01, /* add r4, r4, #0x100 */
        0xe5941000, /* 1: ldr r1, [r4] */
        0xe2811001, /* add r1, r1, #1 */
        0xe5841000, /* str r1, [r4] */
        0xe3a01000, /* mov r1, #0 */
        0xeafffffa  /* b 1b */
    };
    for (unsigned kind = 0; kind < 2; ++kind) {
        S3c2400 *machine[2];
        for (unsigned m = 0; m < 2; ++m) {
            S3c2400 *s = machine[m] = malloc(sizeof(*s));
            assert(s && s3c2400_boot(s, bios, sizeof(bios), (Arm920tConfig){0},
                                     (S3c2400Hooks){0}, (S3c2400Video){0}));
            for (unsigned i = 0; i < (kind ? sizeof(counting) : sizeof(silent)) / 4; ++i)
                write_register(s, S3C2400_SDRAM_BASE + 4 * i, (kind ? counting : silent)[i]);
            write_register(s, S3C2400_SDRAM_BASE + 0x100, 0);
            s->cpu.r[15] = S3C2400_SDRAM_BASE;
        }
        const uint64_t steps = 200000;
        for (uint64_t i = 0; i < steps; ++i)
            s3c2400_step(machine[0]);
        while (machine[1]->steps < steps)
            assert(s3c2400_run(machine[1], steps - machine[1]->steps) !=
                   S3C2400_STOP_EXCEPTION);
        assert(!memcmp(machine[0]->cpu.r, machine[1]->cpu.r, sizeof(machine[0]->cpu.r)));
        assert(machine[0]->steps == machine[1]->steps);
        assert(machine[0]->cycles == machine[1]->cycles);
        assert(read_register(machine[0], S3C2400_SDRAM_BASE + 0x100) ==
               read_register(machine[1], S3C2400_SDRAM_BASE + 0x100));
        assert(kind ? !machine[1]->skipped_steps
                    : machine[1]->skipped_steps > steps / 2);
        free(machine[0]);
        free(machine[1]);
    }
}

int main(void)
{
    test_executable();
    test_load_ram();
    test_rtc_tick();
    test_run();
    test_run_stores();
    S3c2400 *s = malloc(sizeof(*s));
    Events events = {0};
    bool booted = s3c2400_boot(s, bios, sizeof(bios), (Arm920tConfig){0},
                              (S3c2400Hooks){.access = access_event, .copy = copy_event, .context = &events},
                              (S3c2400Video){frame_event, &events});
    if (!booted)
        return EXIT_FAILURE;
    uint32_t fb = S3C2400_SDRAM_BASE + GP32_RAM_SIZE - 8;
    assert(s3c2400_bus_peek(&s->bus, fb + 4, 4).accepted);
    S3c2400Access absent = s3c2400_bus_peek(&s->bus, fb + 8, 1);
    assert(absent.region == S3C2400_OPEN_BUS && absent.accepted && absent.value == 255);
    write_register(s, 0x14600000, 4);
    write_register(s, 0x14600004, fb);
    write_register(s, 0x14600008, (1u << 28) | (1u << 26) | (1u << 22) | (2u << 20) | 2);
    write_register(s, 0x14400008, ~(1u << 17));
    write_register(s, 0x14600018, 3);
    s3c2400_step(s);
    assert(events.copies == 1 && !s->cpu.irq);
    s3c2400_step(s);
    assert(events.copies == 2 && s->cpu.irq && s->irq[5] == 17);
    write_register(s, 0x14400000, 1u << 17);
    write_register(s, 0x14400010, 1u << 17);
    assert(!s->cpu.irq);

    write_register(s, 0x14a00004, 1u << 14);
    write_register(s, 0x14a00008, 1u << 8);
    write_register(s, 0x14a00010, 1);
    write_register(s, 0x14a00014, fb >> 1);
    write_register(s, 0x14a00018, (fb + 8) >> 1);
    write_register(s, 0x14a0001c, 2);
    write_register(s, 0x14a00000, (12u << 1) | (3u << 5) | 1);
    for (unsigned step = 0; step < 100 && !events.frames; ++step)
        s3c2400_step(s);
    assert(events.frames == 1 && events.lcd_reads == 4 && s->cpu.r[15] == 0);
    test_lcd_capture(s, &events);
    test_lcd_status(s);
    write_register(s, 0x14a00000, 0);
    test_smartmedia(s);
    test_eeprom(s);
    write_register(s, 0x1560002c, 2u << 12);
    write_register(s, 0x15600058, 2u << 24);
    s3c2400_set_buttons(s, GP32_A | GP32_UP | GP32_START);
    assert((read_register(s, 0x1560000c) & 0xff00) == 0xb700);
    assert((read_register(s, 0x15600030) & 0xc0) == 0x80);
    assert(read_register(s, 0x14400000) & (1u << 6));
    s3c2400_set_buttons(s, 0);
    assert((read_register(s, 0x1560000c) & 0xff00) == 0xff00);
    test_audio(s, &events);
    test_watchdog();
    test_calendar();
    test_serial_and_converters();
    test_hle(s);
    free(s);
    return EXIT_SUCCESS;
}
