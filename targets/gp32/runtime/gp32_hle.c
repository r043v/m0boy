#include "gp32_hle.h"
#include "gp32_exec.h"

#include <string.h>

enum {
    APP_LIMIT = 0x79c000, APP_PATH = 0x0c7b0c00, CLOCK_INFO = 0x0c7b0600, APP_HEADER = 0x0c7b0e04,
    FRAMEBUFFER = 0x0c7b4000, FRAMEBUFFER_END = 0x0c7ff800,
    SDK_TICKS = 0x0c7b0398, SDK_FLIP = 0x0c7b1400, SDK_PALETTE = 0x0c7b0800,
    IRQ_TABLE = 0x0c7ac000, IRQ_RETURN = 0x0c7b0430, IRQ_STACK = 0x0c7acc00, TICK_ISR = 0x0c7b0400,
    LCD = 0x14a00000, CLOCK = 0x14800000, PWM = 0x15100000, INTC = 0x14400000,
    GPIO = 0x15600000, DMA2 = 0x14600040, IIS = 0x15508000, PCM_IRQ = 19,
    SDK_WORK = 0x0c7b2800, SDK_SCRATCH = 0x0c7b3800, SMC_CALLBACKS = 0x0c7ab800, SMC_CALLBACK_COUNT = 13,
    SEMIHOSTING_SWI = 0x123456,
    HLE_FLIP_SWI = 0x7f0000, HLE_SMC_SWI = 0x7f0010
};

static bool guest_ram(uint32_t address, size_t size)
{
    return address >= S3C2400_SDRAM_BASE && size <= GP32_RAM_SIZE &&
           address - S3C2400_SDRAM_BASE <= GP32_RAM_SIZE - size;
}

static bool transfer_bytes(Arm920t *cpu, uint32_t address, uint8_t *data,
                           size_t size, bool from_guest)
{
    if (!guest_ram(address, size))
        return false;
    for (size_t i = 0; i < size; ++i)
        if (!arm920t_data_byte(cpu, address + i, from_guest, &data[i]))
            return false;
    return true;
}

static bool put_word(Arm920t *cpu, uint32_t addr, uint32_t value)
{
    uint8_t bytes[] = {value, value >> 8, value >> 16, value >> 24};
    return transfer_bytes(cpu, addr, bytes, sizeof(bytes), false);
}

static uint32_t read_word(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
           (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static bool get_word(Arm920t *cpu, uint32_t address, uint32_t *value)
{
    uint8_t bytes[4];
    if (!transfer_bytes(cpu, address, bytes, sizeof(bytes), true))
        return false;
    *value = read_word(bytes);
    return true;
}

static void mmio_write(S3c2400 *s, uint32_t address, uint32_t value)
{
    s3c2400_bus_write(&s->bus, S3C2400_STORE, address, 4, value);
}

static void app_entry(S3c2400 *s, uint32_t length)
{
    Arm920t *cpu = &s->cpu;
    uint32_t cpsr = cpu->cpsr;
    arm920t_set_status(cpu, 0xd2);
    cpu->r[13] = IRQ_STACK;
    arm920t_set_status(cpu, cpsr);
    /* ARM entry point used by the SDK's GpSurfaceSet. */
    put_word(cpu, SDK_FLIP, 0xef000000u | HLE_FLIP_SWI);
    put_word(cpu, SDK_FLIP + 4, 0xe12fff1eu); /* bx lr */
    static const uint32_t tick_isr[] = {
        0xe92d503f, /* push {r0-r5, ip, lr} */
        0xe59f0020, /* ldr r0, =SDK_TICKS */
        0xe5901000, /* ldr r1, [r0] */
        0xe2811001, /* add r1, r1, #1 */
        0xe5801000, /* str r1, [r0] */
        0xe5902004, /* ldr r2, [r0, #4] */
        0xe3520000, /* cmp r2, #0 */
        0x11a0e00f, /* movne lr, pc */
        0x112fff12, /* bxne r2 */
        0xe8bd503f, /* pop {r0-r5, ip, lr} */
        0xe25ef004, /* subs pc, lr, #4 */
        SDK_TICKS
    };
    for (unsigned i = 0; i < sizeof(tick_isr) / sizeof(tick_isr[0]); ++i)
        put_word(cpu, TICK_ISR + 4 * i, tick_isr[i]);
    put_word(cpu, IRQ_RETURN, 0xe25ef004u); /* subs pc, lr, #4 */
    s->cpu.r[15] = S3C2400_SDRAM_BASE;
    s->cpu.r[0] = s->cpu.r[1] = S3C2400_SDRAM_BASE;
    s->cpu.r[2] = length;
    s->cpu.r[3] = APP_HEADER;
    s->cpu.r[13] = 0x0c7afefc;
    s->cpu.r[14] = 0x0c7aff00;
    put_word(&s->cpu, s->cpu.r[14], 0xef000004u); /* Return from entry exits. */
    transfer_bytes(&s->cpu, APP_HEADER, GP32_RAM(s->ram, 4), 24, false);
}

static bool chainload(S3c2400 *s)
{
    Arm920t *cpu = &s->cpu;
    uint32_t source = cpu->r[0], path_address = cpu->r[1];
    /* The source must be directly visible in SDRAM. */
    if ((cpu->control & 1u) || !guest_ram(source, 4))
        return false;
    uint32_t at = source - S3C2400_SDRAM_BASE;
    uint8_t header[8];
    for (unsigned i = 0; i < sizeof header; ++i)
        header[i] = *GP32_RAM(s->ram, at + i);
    uint32_t length = read_word(header);
    if (length < 28 || length > APP_LIMIT || !guest_ram(source, length + 4) ||
        header[7] != 0xea)
        return false;
    uint8_t path[255] = {0};
    if (path_address) {
        unsigned n = 0;
        while (n < sizeof(path) - 1) {
            if (!transfer_bytes(cpu, path_address + n, &path[n], 1, true))
                return false;
            if (!path[n])
                break;
            ++n;
        }
    }
    size_t loaded_size;
#ifdef M0_GP32_PAGED_RAM
    /* The source spans pages which need not follow each other in the host. */
    if (gp32_exec_load_guest(s, at + 4, length, APP_LIMIT, &loaded_size))
#else
    if (gp32_exec_load_ram(s, GP32_RAM(s->ram, at + 4), length, APP_LIMIT, &loaded_size))
#endif
        return false;
    Arm920tConfig config = cpu->config;
    arm920t_reset(cpu, config, false);
    transfer_bytes(cpu, APP_PATH, path, sizeof(path), false);
    app_entry(s, loaded_size);
    return true;
}

static uint32_t eeprom_access(S3c2400 *s)
{
    Arm920t *cpu = &s->cpu;
    uint32_t start = cpu->r[0], length = cpu->r[1], address = cpu->r[2];
    bool write = cpu->r[3] != 0;
    uint8_t buffer[GP32_EEPROM_SIZE];
    if ((int32_t)start < 0)
        start = 0;
    if (!length || start >= sizeof(buffer) || length > sizeof(buffer) - start)
        return 0x21;
    if (write) {
        if (!transfer_bytes(cpu, address, buffer, length, true))
            return 0x21;
        memcpy(s->eeprom + start, buffer, length);
        s->eeprom_dirty = true;
    } else if (!transfer_bytes(cpu, address, s->eeprom + start, length, false)) {
        return 0x21;
    }
    return 0;
}

static void lcd_write(S3c2400 *s, unsigned index, uint32_t value)
{
    s3c2400_bus_write(&s->bus, S3C2400_STORE, LCD + 4 * index, 4, value);
}

static unsigned lcd_bits(const S3c2400 *s)
{
    unsigned mode = (s->lcd[0] >> 1) & 15u;
    return mode >= 8 && mode <= 12 ? 1u << (mode - 8) : 0;
}

static unsigned surface_bytes(unsigned bits)
{
    return GP32_LCD_WIDTH * GP32_LCD_HEIGHT * bits / 8;
}

static unsigned surface_stride(unsigned bits)
{
    return (surface_bytes(bits) + 255u) & ~255u;
}

static unsigned surface_count(unsigned bits)
{
    unsigned count = (FRAMEBUFFER_END - FRAMEBUFFER) / surface_stride(bits);
    return count < 4 ? count : 4;
}

static void lcd_surface(S3c2400 *s, uint32_t address, unsigned bits)
{
    lcd_write(s, 5, address / 2);
    lcd_write(s, 6, (address / 2 + surface_bytes(bits) / 2) & 0x1fffffu);
    lcd_write(s, 7, GP32_LCD_WIDTH * bits / 16);
}

static bool lcd_flip(S3c2400 *s)
{
    uint32_t address;
    unsigned bits = lcd_bits(s);
    if (!bits || !get_word(&s->cpu, s->cpu.r[0], &address) ||
        (address & 1u) || !guest_ram(address, surface_bytes(bits)) ||
        ((address & 0x3fffffu) + surface_bytes(bits) > 0x400000u))
        return false;
    /* The synchronized modes wait for the blanking lines. Re-entering the
       trap lets the SoC keep advancing and deliver IRQs during the wait. */
    unsigned visible_lines = ((s->lcd[1] >> 14) & 1023u) + 1;
    if (s->hle_flip_mode != 1 && (s->lcd[0] & 1u) && s->line < visible_lines) {
        s->cpu.r[15] = SDK_FLIP;
        s->hle_waiting = true;
        return true;
    }
    lcd_surface(s, address, bits);
    return true;
}

static bool lcd_palette(S3c2400 *s, uint32_t address)
{
    uint8_t colors[256 * 4];
    if (!transfer_bytes(&s->cpu, address ? address : SDK_PALETTE, colors, sizeof(colors), true))
        return false;
    for (unsigned i = 0; i < 256; ++i)
        mmio_write(s, LCD + 0x400 + 4 * i, read_word(colors + 4 * i));
    return true;
}

/* Move one RGB555 palette entry a step towards `target`, as the firmware does.
   A step towards the target on a side it has already passed keeps going to
   white (direction 1) or black (-1). Direction 0 closes in from both sides. */
static uint32_t palette_step(uint32_t current, uint32_t target, int direction, unsigned mask)
{
    int channel[3], goal[3];
    bool pending = false;
    for (unsigned i = 0; i < 3; ++i) {
        channel[i] = (current >> (11 - 5 * i)) & 31u;
        goal[i] = (target >> (11 - 5 * i)) & 31u;
        pending |= (mask & (4u >> i)) && direction * (goal[i] - channel[i]) > 0;
    }
    for (unsigned i = 0; i < 3; ++i) {
        if (!(mask & (4u >> i)))
            continue;
        int step = direction;
        if (!direction)
            step = (goal[i] > channel[i]) - (goal[i] < channel[i]);
        else if (pending && direction * (goal[i] - channel[i]) <= 0)
            step = 0;
        channel[i] += step;
        channel[i] = channel[i] < 0 ? 0 : channel[i] > 31 ? 31 : channel[i];
    }
    return (uint32_t)channel[0] << 11 | (uint32_t)channel[1] << 6 |
           (uint32_t)channel[2] << 1 | (current & 1u);
}

/* Palette RAM services: load the SDK palette, or fade it by one step. */
static bool palette_service(S3c2400 *s, uint32_t pc)
{
    uint32_t *r = s->cpu.r, command = r[0], source = SDK_PALETTE, mask = 7;
    int direction = 0;
    switch (command) {
    case 1: case 2:
        break;
    case 3: case 4:
        source = r[1];
        direction = command == 3 ? 1 : -1;
        break;
    case 5:
        source = r[3];
        mask = r[2] & 7u;
        direction = (int32_t)r[1] > 0 ? 1 : -1;
        if (!r[1] || !r[2]) {
            r[0] = 0;
            return true;
        }
        break;
    case 6:
        source = r[1];
        if (!source)
            return true;
        break;
    default:
        return true;
    }
    if (!source) {
        r[0] = 0;
        return true;
    }
    /* Every mode but the immediate load writes during the blanking lines. */
    unsigned visible_lines = ((s->lcd[1] >> 14) & 1023u) + 1;
    if (command != 2 && (s->lcd[0] & 1u) && s->line < visible_lines) {
        r[15] = pc;
        s->hle_waiting = true;
        return true;
    }
    if (command <= 2)
        return lcd_palette(s, source);
    uint8_t colors[256 * 4];
    if (!transfer_bytes(&s->cpu, source, colors, sizeof(colors), true))
        return false;
    uint32_t changed = 0;
    for (unsigned i = 0; i < 256; ++i) {
        uint32_t current = s->palette[i], target = read_word(colors + 4 * i);
        if (command == 6 && !((current ^ target) & 0xfffeu))
            continue;
        uint32_t next = palette_step(current, target, direction, mask);
        if (next == current)
            continue;
        mmio_write(s, LCD + 0x400 + 4 * i, next);
        if (command == 5) {
            unsigned empty = (next >> 11 & 31u ? 0 : 4u) | (next >> 6 & 31u ? 0 : 2u) |
                             (next >> 1 & 31u ? 0 : 1u);
            changed += empty != mask;
        } else {
            uint32_t limit = command == 3 ? 0xfffeu : 0;
            changed += command == 6 || next != (limit | (current & 1u));
        }
    }
    r[0] = changed;
    return true;
}

static void default_palette(Arm920t *cpu)
{
    static const unsigned levels[] = {0, 4, 9, 13, 18, 22, 26, 31};
    static const unsigned blue[] = {0, 10, 20, 31};
    for (unsigned i = 0; i < 256; ++i) {
        uint32_t color = (levels[i >> 5] << 11) | (levels[(i >> 2) & 7] << 6) |
                         (blue[i & 3] << 1);
        put_word(cpu, SDK_PALETTE + 4 * i, color);
    }
}

static bool lcd_service(S3c2400 *s)
{
    Arm920t *cpu = &s->cpu;
    uint32_t *r = cpu->r;
    unsigned bits = lcd_bits(s);
    switch (r[2]) {
    case 0: {
        unsigned bits = r[0];
        if (bits != 1 && bits != 2 && bits != 4 && bits != 8 && bits != 16)
            return false;
        if (bits != 16 && !lcd_palette(s, r[1]))
            return false;
        unsigned depth = 8;
        for (unsigned n = bits; n > 1; n >>= 1)
            ++depth;
        lcd_write(s, 0, (s->lcd[0] & ~0x1eu) | (depth << 1));
        lcd_write(s, 4, (s->lcd[4] & ~3u) | (bits == 8 ? 2u : 1u));
        lcd_surface(s, FRAMEBUFFER, bits);
        gp32_ram_fill(s->ram, FRAMEBUFFER - S3C2400_SDRAM_BASE, 0xff,
               surface_count(bits) * surface_stride(bits));
        lcd_write(s, 0, s->lcd[0] | 1u);
        r[0] = surface_count(bits);
        return true;
    }
    case 1: {
        uint32_t addr = r[0];
        if (!bits || r[1] >= surface_count(bits) || !guest_ram(addr, 7 * 4)) {
            r[0] = 2;
            return true;
        }
        uint32_t buffer = FRAMEBUFFER + r[1] * surface_stride(bits);
        uint32_t surface[] = {buffer, bits, GP32_LCD_HEIGHT, GP32_LCD_WIDTH, 0, 0, buffer};
        for (unsigned i = 0; i < sizeof(surface) / sizeof(surface[0]); ++i)
            if (!put_word(cpu, addr + 4 * i, surface[i])) {
                r[0] = 2;
                return true;
            }
        r[0] = 0;
        return true;
    }
    case 2:
        if (r[0] > 2)
            return false;
        s->hle_flip_mode = r[0];
        return true;
    case 3:
        lcd_write(s, 0, s->lcd[0] | 1u);
        return true;
    case 4:
        lcd_write(s, 0, s->lcd[0] & ~1u);
        return true;
    case 5:
        r[0] = !(s->lcd[0] & 1u) ? 0 : 0x80 |
            (((s->lcd[4] >> 17) & 3u) == 2 ? 0x20 : 0) |
            (((s->lcd[4] >> 17) & 12u) == 8 ? 0x40 : 0);
        return true;
    case 6:
        r[0] = (s->lcd[5] & 0xfffffffu) * 2;
        r[1] = s->lcd[5];
        return true;
    case 7: {
        if (!bits || !guest_ram(r[0], 8 * 4))
            return false;
        uint32_t info[] = {
            (1u << 24) | (bits << 16) | (surface_count(bits) << 8) | ((s->lcd[0] >> 8) & 255u),
            surface_bytes(bits), FRAMEBUFFER, 0, 0, 0, SDK_FLIP, SDK_PALETTE
        };
        for (unsigned i = 0; i < surface_count(bits); ++i)
            info[2 + i] = FRAMEBUFFER + i * surface_stride(bits);
        for (unsigned i = 0; i < sizeof(info) / sizeof(info[0]); ++i)
            if (!put_word(cpu, r[0] + i * 4, info[i]))
                return false;
        return true;
    }
    default:
        return false;
    }
}

static bool clock_change(S3c2400 *s)
{
    uint32_t address = s->cpu.r[0];
    uint8_t args[12];
    if (!transfer_bytes(&s->cpu, address, args, sizeof(args), true))
        return false;
    uint32_t pll = read_word(args + 4);
    if (read_word(args + 8) > 3)
        return false;
    s3c2400_bus_write(&s->bus, S3C2400_STORE, CLOCK + 4, 4, pll);
    s3c2400_bus_write(&s->bus, S3C2400_STORE, CLOCK + 20, 4, args[8]);
    return true;
}

static bool app_path(Arm920t *cpu)
{
    uint32_t *r = cpu->r;
    if (r[4] > 1)
        return false;
    uint8_t path[255];
    unsigned length = 0;
    if (!r[4]) {
        unsigned limit = r[1] && r[1] < sizeof(path) ? r[1] : sizeof(path) - 1;
        while (length < limit) {
            if (!transfer_bytes(cpu, r[0] + length, &path[length], 1, true)) {
                r[0] = 0;
                return true;
            }
            if (!path[length])
                break;
            ++length;
        }
        path[length] = 0;
        r[0] = transfer_bytes(cpu, APP_PATH, path, length + 1, false) ? length : 0;
        return true;
    }
    while (length < sizeof(path) &&
           transfer_bytes(cpu, APP_PATH + length, path, 1, true) && path[0])
        ++length;
    if (r[0] && !put_word(cpu, r[0], length)) {
        r[0] = 0;
        return true;
    }
    r[0] = APP_PATH;
    return true;
}

static uint32_t gpio_read(S3c2400 *s, unsigned offset)
{
    return s3c2400_pin_read(s, offset);
}

static void gpio_update(S3c2400 *s, unsigned offset, uint32_t mask, uint32_t value)
{
    s3c2400_pin_write(s, offset, (gpio_read(s, offset) & ~mask) | value);
}

static bool smc_callback_table(S3c2400 *s, uint32_t destination)
{
    Arm920t *cpu = &s->cpu;
    if (!guest_ram(destination, 4 * SMC_CALLBACK_COUNT))
        return false;
    for (unsigned i = 0; i < SMC_CALLBACK_COUNT; ++i) {
        uint32_t stub = SMC_CALLBACKS + 8 * i;
        if (!put_word(cpu, stub, 0xef000000u | (HLE_SMC_SWI + i)) ||
            !put_word(cpu, stub + 4, 0xe12fff1eu) ||
            !put_word(cpu, destination + 4 * i, stub))
            return false;
    }
    if (s->smc.image) {
        gpio_update(s, 8, 0xffff, 0x5555);
        gpio_update(s, 0x20, 0xffc00, 0x15000);
        gpio_update(s, 0x2c, 0xff0, 0x560);
    }
    cpu->r[0] = destination + 4 * (SMC_CALLBACK_COUNT - 1);
    return true;
}

static uint32_t peripheral_clock(const S3c2400 *s)
{
    uint32_t pll = s->clock[1];
    uint64_t fclk = (((pll >> 12) & 255u) + 8u) * UINT64_C(12000000) /
                    ((((pll >> 4) & 63u) + 2u) << (pll & 3u));
    return fclk >> ((s->clock[5] & 1u) + ((s->clock[5] >> 1) & 1u));
}

static void register_irq(S3c2400 *s, unsigned source, uint32_t handler, bool enable)
{
    put_word(&s->cpu, IRQ_TABLE + 4 * source, enable ? handler : 0);
    uint32_t bit = 1u << source;
    mmio_write(s, INTC + 8, enable ? s->irq[2] & ~bit : s->irq[2] | bit);
}

/* Prepare IIS and DMA channel 2 for 16-bit playback. Returns the rate
   obtained from the closest prescaler, in 256fs or 384fs mode. */
static uint32_t pcm_open(S3c2400 *s, uint32_t flags, uint32_t handler, uint32_t rate)
{
    uint32_t pclk = peripheral_clock(s);
    uint32_t best_rate = 0, distance = UINT32_MAX;
    unsigned mode = 0, prescale = 0;
    for (unsigned m = 0; m < 2; ++m) {
        uint32_t base = pclk / (m ? 384 : 256);
        for (unsigned i = 0; i < 32; ++i) {
            uint32_t actual = base / (i + 1);
            uint32_t error = actual > rate ? actual - rate : rate - actual;
            if (error < distance) {
                distance = error;
                best_rate = actual;
                mode = m;
                prescale = i;
            }
        }
    }
    mmio_write(s, IIS, 0x0e);
    mmio_write(s, IIS + 12, 0);
    mmio_write(s, DMA2 + 0x18, 4);
    mmio_write(s, DMA2 + 4, 0x75508010u); /* fixed APB target: IIS FIFO */
    mmio_write(s, DMA2 + 8, flags & 0x10 ? 0x50900000u : 0x50d00000u);
    register_irq(s, PCM_IRQ, handler, true);
    mmio_write(s, IIS, 0x2e);
    mmio_write(s, IIS + 4, 0x89 | mode << 2);
    mmio_write(s, IIS + 8, prescale | prescale << 5);
    mmio_write(s, IIS + 12, 0xa00);
    return best_rate;
}

static uint32_t dma_read(S3c2400 *s, unsigned offset)
{
    return s3c2400_bus_read(&s->bus, S3C2400_LOAD, DMA2 + offset, 4).value;
}

/* The command is a set of flags at the head of a guest structure:
   +8 source, +12 interrupt handler, +16 byte count, +20 sample rate. */
static bool pcm_service(S3c2400 *s)
{
    Arm920t *cpu = &s->cpu;
    uint32_t block = cpu->r[0], flags, value = 0;
    if (!get_word(cpu, block, &flags))
        return false;
    if (flags & 0x3000) {
        mmio_write(s, DMA2 + 0x18, 4);
        if (!(flags & 0x1000)) {
            mmio_write(s, IIS, 0x0e);
            mmio_write(s, IIS + 12, 0);
        }
        cpu->r[0] = 1;
        return true;
    }
    if (!(flags & 0x200)) {
        if (flags & 0xc00) {
            if ((flags & 0x800) && !get_word(cpu, block + 12, &value))
                return false;
            register_irq(s, PCM_IRQ, value, flags & 0x800);
            cpu->r[0] = 0;
            return true;
        }
        if (flags & 0x4000) {
            uint32_t control = dma_read(s, 8) & ~0x400000u;
            mmio_write(s, DMA2 + 8, control | (flags & 0x10 ? 0 : 0x400000u));
            mmio_write(s, DMA2 + 0x18, 2);
            mmio_write(s, IIS, 0x2e);
            mmio_write(s, IIS + 12, 0xa00);
            mmio_write(s, IIS, 0x27);
            cpu->r[0] = 1;
            return true;
        }
        if (flags & 0x8000) {
            uint32_t handler, rate;
            if (!get_word(cpu, block + 12, &handler) || !get_word(cpu, block + 20, &rate))
                return false;
            cpu->r[0] = pcm_open(s, flags, handler, rate);
            return true;
        }
    }
    if (flags & 0x80) {
        if (!get_word(cpu, block + 16, &value))
            return false;
        mmio_write(s, DMA2 + 8, (dma_read(s, 8) & 0xfff00000u) | ((value >> 1) & 0xfffffu));
    }
    if (flags & 0x10)
        mmio_write(s, DMA2 + 8, dma_read(s, 8) & ~0x400000u);
    if (flags & 0x20)
        mmio_write(s, DMA2 + 8, dma_read(s, 8) | 0x400000u);
    if (flags & 0x100) {
        if (!get_word(cpu, block + 8, &value))
            return false;
        mmio_write(s, DMA2, value);
    }
    if (flags & 1)
        mmio_write(s, DMA2 + 0x18, 2);
    if (flags & 2)
        mmio_write(s, DMA2 + 0x18, 4);
    if (flags & 4)
        mmio_write(s, IIS, 0x27);
    if (flags & 8) {
        mmio_write(s, IIS, 6);
        mmio_write(s, IIS + 12, 0);
    }
    cpu->r[0] = 1;
    return true;
}

static bool system_info(S3c2400 *s)
{
    uint32_t *r = s->cpu.r;
    switch (r[0]) {
    case 0:
        r[0] = SDK_FLIP;
        return true;
    case 1:
        r[0] = SDK_WORK;
        return true;
    case 2: /* Palette registers and their RAM shadow. */
        return put_word(&s->cpu, r[1], LCD + 0x400) &&
               put_word(&s->cpu, r[2], SDK_PALETTE);
    case 7:
        r[0] = 0x8000;
        return true;
    case 3:
        return smc_callback_table(s, r[1]);
    case 6:
        r[0] = SDK_TICKS;
        return true;
    case 8:
        r[0] = 32;
        return true;
    case 4: {
        uint32_t pll = s->clock[1];
        unsigned m = ((pll >> 12) & 255u) + 8, p = ((pll >> 4) & 63u) + 2;
        uint32_t fclk = m * UINT64_C(12000000) / (p << (pll & 3u));
        uint32_t hclk = fclk >> ((s->clock[5] >> 1) & 1u);
        uint32_t pclk = peripheral_clock(s);
        if (!put_word(&s->cpu, CLOCK_INFO, fclk) ||
            !put_word(&s->cpu, CLOCK_INFO + 4, hclk) ||
            !put_word(&s->cpu, CLOCK_INFO + 8, pclk))
            return false;
        r[0] = CLOCK_INFO;
        return true;
    }
    case 5:
        r[0] = S3C2400_SDRAM_BASE + APP_LIMIT;
        return true;
    default:
        r[0] = 0;
        return true;
    }
}

static void timer_clear(S3c2400 *s, unsigned timer)
{
    uint32_t bit = 1u << (10 + timer);
    mmio_write(s, INTC, bit);
    mmio_write(s, INTC + 16, bit);
}

static bool timer_service(S3c2400 *s)
{
    Arm920t *cpu = &s->cpu;
    uint32_t address = cpu->r[0], command;
    if (!get_word(cpu, address, &command))
        return false;

    if (!command) {
        mmio_write(s, PWM + 8, s->pwm[2] & ~0xfffffu);
        mmio_write(s, INTC + 8, s->irq[2] | (15u << 10));
        memset(s->hle_timer_handler, 0, sizeof(s->hle_timer_handler));
        for (unsigned timer = 0; timer < 4; ++timer)
            timer_clear(s, timer);
        if (!put_word(cpu, SDK_TICKS + 4, 0))
            return false;
        return true;
    }

    uint32_t timer;
    if (!get_word(cpu, address + 4, &timer) || timer >= 4)
        return false;
    unsigned shift = timer ? 4 * (timer + 1) : 0;
    uint32_t control = s->pwm[2] & ~(15u << shift);
    uint32_t bit = 1u << (10 + timer);
    switch (command) {
    case 1: {
        uint32_t handler, frequency, period;
        if (!get_word(cpu, address + 8, &handler) ||
            !get_word(cpu, address + 12, &frequency) ||
            !get_word(cpu, address + 16, &period) || !frequency || !period)
            return false;
        uint64_t pclk = peripheral_clock(s);
        unsigned bank_shift = timer < 2 ? 0 : 8;
        unsigned prescale = ((s->pwm[0] >> bank_shift) & 255u) + 1;
        /* Timers in each pair share one prescaler. */
        unsigned sibling = timer ^ 1u;
        if (!s->hle_timer_handler[sibling]) {
            uint64_t divisor = pclk / (2u * (uint64_t)frequency);
            prescale = divisor < 1 ? 1 : divisor > 256 ? 256 : divisor;
            mmio_write(s, PWM, (s->pwm[0] & ~(255u << bank_shift)) |
                               ((prescale - 1) << bank_shift));
        }
        unsigned mux = 0;
        while (mux < 3 && (pclk * period) / (1000000u * (prescale << (mux + 1))) > 65535)
            ++mux;
        uint64_t count = pclk * period / (1000000u * (prescale << (mux + 1)));
        if (!count)
            count = 1;
        if (count > 65535)
            count = 65535;
        unsigned mux_shift = timer * 4;
        mmio_write(s, PWM + 4, (s->pwm[1] & ~(15u << mux_shift)) | (mux << mux_shift));
        mmio_write(s, PWM + 12 + 12 * timer, count);
        mmio_write(s, PWM + 16 + 12 * timer, 0);
        mmio_write(s, PWM + 8, control | (10u << shift));
        mmio_write(s, PWM + 8, control | (8u << shift));
        if (!timer && !put_word(cpu, SDK_TICKS + 4, handler))
            return false;
        s->hle_timer_handler[timer] = timer ? handler : TICK_ISR;
        timer_clear(s, timer);
        mmio_write(s, INTC + 8, s->irq[2] & ~bit);
        return true;
    }
    case 2:
        mmio_write(s, PWM + 8, control | (9u << shift));
        return true;
    case 3:
        mmio_write(s, PWM + 8, s->pwm[2] & ~(1u << shift));
        return true;
    case 4:
        mmio_write(s, PWM + 8, s->pwm[2] & ~(1u << shift));
        mmio_write(s, INTC + 8, s->irq[2] | bit);
        timer_clear(s, timer);
        s->hle_timer_handler[timer] = 0;
        return true;
    default:
        return false;
    }
}

static bool dispatch_irq(S3c2400 *s, unsigned source)
{
    if (source >= 32)
        return false;
    uint32_t handler = read_word(GP32_RAM(s->ram, IRQ_TABLE - S3C2400_SDRAM_BASE + 4 * source));
    if (!handler && source >= 10 && source < 14)
        handler = s->hle_timer_handler[source - 10];
    uint32_t bit = 1u << source;
    mmio_write(s, INTC, bit);
    mmio_write(s, INTC + 16, bit);
    s->cpu.r[15] = handler ? handler & ~3u : IRQ_RETURN;
    return true;
}

/* The C library of some games reaches clock() and time() through ARM
   semihosting. FW157E has no handler for that SWI and stops there, so
   answering is an HLE leniency; both derive from the emulated frame count. */
static bool semihosting(S3c2400 *s)
{
    uint32_t *r = s->cpu.r;
    switch (r[0]) {
    case 0x10: /* SYS_CLOCK, in centiseconds. */
        r[0] = (uint32_t)(s->frames * 100 / 60);
        return true;
    case 0x11: /* SYS_TIME, in seconds. */
        r[0] = (uint32_t)(s->frames / 60);
        return true;
    default:
        return false;
    }
}

/* Kept apart from the other calls, whose large frame the card routines, run
   for every byte, would otherwise set up. */
#ifdef __GNUC__
__attribute__((noinline))
#endif
static bool sdk_call(S3c2400 *s, uint32_t number, uint32_t pc);

static bool dispatch(void *context, uint32_t number, uint32_t pc)
{
    S3c2400 *s = context;
    /* The card routines only drive pins: no clocked state to bring up to date. */
    if (number >= HLE_SMC_SWI && number < HLE_SMC_SWI + SMC_CALLBACK_COUNT &&
        pc == SMC_CALLBACKS + 8 * (number - HLE_SMC_SWI)) {
        s3c2400_card_routine(s, number - HLE_SMC_SWI);
        return true;
    }
    return sdk_call(s, number, pc);
}

static bool sdk_call(S3c2400 *s, uint32_t number, uint32_t pc)
{
    Arm920t *cpu = &s->cpu;
    uint32_t *r = cpu->r;
    static const uint8_t pduid_mask[] = "SANGHYUK";
    uint8_t id[16];
    s3c2400_sync(s);
    switch (number) {
    case 0x02: /* Page attributes; HLE applications run with the MMU off. */
        return true;
    case 0x0c: /* Empty in the firmware. */
        return true;
    case 0x0e: { /* Bounds of the shared scratch buffer for a requested size. */
        uint32_t end = (SDK_SCRATCH + r[1] + 0xff) & ~0xffu;
        if (put_word(cpu, r[0], SDK_SCRATCH) && put_word(cpu, r[0] + 4, end))
            return true;
        break;
    }
    case 0x14:
        if (pcm_service(s))
            return true;
        break;
    case 0x16:
        if (palette_service(s, pc))
            return true;
        break;
    case 0x00:
        arm920t_set_status(cpu, (cpu->cpsr & ~31u) | 0xd3u);
        return true;
    case 0x04:
        s->hle_exited = true;
        return true;
    case 0x05:
        if (chainload(s))
            return true;
        break;
    case 0x08:
        if (lcd_service(s))
            return true;
        break;
    case 0x09: case 0x0a:
        if (r[0] < 32) {
            register_irq(s, r[0], r[1], number == 9);
            return true;
        }
        break;
    case 0x0b:
        if (system_info(s))
            return true;
        break;
    case 0x0d:
        if (clock_change(s))
            return true;
        break;
    case 0x0f:
        if (app_path(cpu))
            return true;
        break;
    case 0x10:
        if (!r[0]) {
            r[0] = 0x1560000c;
            r[1] = 0x15600030;
            return true;
        }
        if (r[0] == 1) {
            r[0] = 0xfffffc00u | ((r[1] >> 8) & 255u) | ((r[2] & 192u) << 2);
            return true;
        }
        break;
    case 0x11:
        r[0] = s->smc.image != NULL;
        return true;
    case 0x13:
        if (timer_service(s))
            return true;
        break;
    case 0x15:
        /* The firmware leaves the guest registers unchanged on this call. */
        return true;
    case HLE_FLIP_SWI:
        if (pc == SDK_FLIP && lcd_flip(s))
            return true;
        break;
    case 0x103: {
        const uint8_t *uid = gp32_smc_uid(&s->smc);
        if (uid)
            memcpy(id, uid, sizeof(id));
        r[0] = uid && transfer_bytes(cpu, r[0], id, sizeof(id), false) ? 0 : 2;
        return true;
    }
    case 0x104:
        for (unsigned i = 0; i < sizeof(id); ++i)
            id[i] = s->eeprom[0x10 + i] ^ pduid_mask[i % 8];
        r[0] = transfer_bytes(cpu, r[0], id, sizeof(id), false) ? 0 : 0x21;
        return true;
    case 0x105:
        r[0] = eeprom_access(s);
        return true;
    case SEMIHOSTING_SWI:
        if (semihosting(s))
            return true;
        break;
    }
    s->hle_swi = number;
    s->hle_pc = pc;
    s->hle_unhandled = true;
    return false;
}

void gp32_hle_set_directory(S3c2400 *s, const char *directory)
{
    size_t length = strlen(directory);
    if (length < 256)
        transfer_bytes(&s->cpu, APP_PATH, (uint8_t *)directory, length + 1, false);
}

const char *gp32_hle_boot(S3c2400 *s, const uint8_t *data, size_t size,
                         Arm920tConfig cpu_hooks, S3c2400Hooks bus_hooks,
                         S3c2400Video video)
{
    cpu_hooks.software_interrupt = dispatch;
    cpu_hooks.software_interrupt_context = s;
    if (!s3c2400_boot(s, NULL, 0, cpu_hooks, bus_hooks, video))
        return "Unable to initialize GP32 HLE";
    s->watchdog[0] = 0; /* the boot ROM stops the watchdog */
    s->bus.absent_boot_rom_stores = true;
    /* "GP32" and the version word of firmware 1.5.7E, whose SDK-visible
       services this HLE follows. */
    static const uint32_t firmware_id[2] = {0x47503332u, 0x01030507u};
    s->bus.absent_boot_rom_id = firmware_id;
    size_t loaded_size;
    const char *error = gp32_exec_load_ram(s, data, size, APP_LIMIT, &loaded_size);
    if (error)
        return error;
    app_entry(s, loaded_size);
    char path[256];
    gp32_exec_directory(data, size, path);
    transfer_bytes(&s->cpu, APP_PATH, (uint8_t *)path, strlen(path) + 1, false);

    default_palette(&s->cpu);
    s->irq_handler = dispatch_irq;

    /* GP32 application startup clock, with PCLK at FCLK / 4. */
    mmio_write(s, CLOCK + 4, 0x69032);
    mmio_write(s, CLOCK + 20, 3);

    /* The firmware hands applications a 240 x 320 indexed TFT. */
    lcd_write(s, 1, (1u << 24) | (319u << 14) | 0x81u);
    lcd_write(s, 2, (239u << 8) | (6u << 19) | 2u);
    lcd_write(s, 3, 4);
    lcd_write(s, 4, 0x702);
    lcd_palette(s, SDK_PALETTE);
    lcd_surface(s, FRAMEBUFFER, 8);
    lcd_write(s, 0, 0x377);
    return NULL;
}
