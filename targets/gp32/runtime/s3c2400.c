#include "s3c2400.h"

#include <stddef.h>
#include <string.h>

#define IRQ 0x14400000u
#define DMA 0x14600000u
#define LCD 0x14a00000u
#define PWM 0x15100000u
#define RTC 0x15700000u
#define RTC_TICNT (RTC + 0x44u)
#define UART 0x15000000u
#define UART_PORT 0x4000u
#define SPI 0x15900000u
#define ADC 0x15800000u
#define WATCHDOG 0x15300000u
#define IIC 0x15400000u
#define IIS 0x15508000u
#define GPIO 0x15600000u
#define DMA_ADDRESS 0x1fffffffu
#define DMA_COUNT 0xfffffu

/* Interrupt sources. */
enum {
    IRQ_TICK = 8, IRQ_WATCHDOG = 9, IRQ_SPI = 22, IRQ_UART_RX = 23,
    IRQ_UART_TX = 28, IRQ_ALARM = 30, IRQ_ADC = 31
};
/* Register indexes within their blocks. */
enum { UCON = 1, UTRSTAT = 4, UTXH = 8, URXH = 9 };
enum { RTCALM = 20, ALMSEC = 21, BCDSEC = 28, BCDMIN, BCDHOUR, BCDDATE, BCDDAY, BCDMON, BCDYEAR };

/* Kept out of the instruction loop, which inlines the peripheral tick. */
#if defined(__GNUC__)
#define RARE __attribute__((noinline, cold))
#else
#define RARE
#endif

static const unsigned timer_control[] = {0, 8, 12, 16, 20};
#define TIMERS_ON 0x111101u /* the start bit of each timer in TCON */
static const unsigned timer_buffer[] = {3, 6, 9, 12, 15};
static const unsigned timer_observe[] = {5, 8, 11, 14, 16};

static uint32_t *register_at(S3c2400 *s, uint32_t address)
{
#define BLOCK(base, field) \
    if (address >= (base) && address - (base) < sizeof(s->field)) \
        return &s->field[(address - (base)) / 4]
    BLOCK(0x14000000u, memcon);
    BLOCK(IRQ, irq);
    if (address >= DMA && address < DMA + 0x80 && (address & 31u) < 28u)
        return &s->dma[(address - DMA) / 32].regs[(address & 31u) / 4];
    BLOCK(0x14800000u, clock);
    BLOCK(LCD, lcd);
    BLOCK(LCD + 0x400, palette);
    BLOCK(0x15000000u, uart[0]);
    BLOCK(0x15004000u, uart[1]);
    BLOCK(PWM, pwm);
    BLOCK(0x15200000u, usbhost);
    BLOCK(0x15200140u, usb);
    BLOCK(0x15300000u, watchdog);
    BLOCK(IIC, iic);
    BLOCK(IIS, iis);
    BLOCK(GPIO, gpio);
    BLOCK(RTC, rtc);
    BLOCK(0x15800000u, adc);
    BLOCK(0x15900000u, spi);
    BLOCK(0x15a00000u, mmc);
#undef BLOCK
    return NULL;
}

static void button_irqs(S3c2400 *s, uint16_t previous)
{
    for (unsigned pin = 6; pin <= 7; ++pin) {
        if ((s->gpio[11] >> (2 * pin) & 3u) != 2)
            continue;
        unsigned mode = (s->gpio[22] >> (4 * pin)) & 7u;
        unsigned mask = 1u << (pin + 2);
        bool pressed = (s->buttons & mask) != 0;
        bool changed = ((s->buttons ^ previous) & mask) != 0;
        if ((mode == 0 && pressed) || (mode == 1 && !pressed) ||
            (changed && (mode >= 6 || (mode >= 2 && pressed == (mode < 4)))))
            s->irq[0] |= 1u << pin;
    }
}

/* A level triggered serial request stays pending while its cause lasts. */
static void uart_irqs(S3c2400 *s)
{
    for (unsigned port = 0; port < 2; ++port) {
        uint32_t control = s->uart[port][UCON];
        if ((control & 0x20cu) == 0x204u)
            s->irq[0] |= 1u << (IRQ_UART_TX + port);
        if ((control & 0x103u) == 0x101u && (s->late.uart_full & (1u << port)))
            s->irq[0] |= 1u << (IRQ_UART_RX + port);
    }
}

static void irq_update(S3c2400 *s)
{
    uint32_t pending = s->irq[0] & ~s->irq[2];
    uint32_t normal = pending & ~s->irq[1];
    if (!s->irq[4] && normal) {
        unsigned bit = 0;
        while (!(normal & (1u << bit)))
            ++bit;
        s->irq[4] = 1u << bit;
        s->irq[5] = bit;
    }
    s->cpu.irq = s->irq[4] != 0;
    s->cpu.fiq_line = (pending & s->irq[1]) != 0;
}

void s3c2400_set_irq(S3c2400 *s, unsigned source)
{
    if (source < 32)
        s->irq[0] |= 1u << source;
    irq_update(s);
}


/* Without division, the machine divides only where a value is set up: here by
   small numbers through their inverses, all computed when this file is
   compiled. */
#if M0_GP32_DIVIDE
#define INVERSE(d) 0
#else
#define INVERSE(d) inverses[(d) - 1]
#define R1(d) (((UINT64_C(1) << 32) + (d) - 1) / (d))
#define R4(d) R1(d), R1((d) + 1), R1((d) + 2), R1((d) + 3)
#define R16(d) R4(d), R4((d) + 4), R4((d) + 8), R4((d) + 12)
#define R64(d) R16(d), R16((d) + 16), R16((d) + 32), R16((d) + 48)
static const uint64_t inverses[256] = {R64(1), R64(65), R64(129), R64(193)};
#endif

/* A peripheral period: `count`, from 1 to 256, times 2^`shift`. */
typedef struct {
    unsigned count, shift;
} Period;

static inline uint32_t periods(uint32_t clocks, Period period)
{
    return arm920t_quotient(clocks >> period.shift, period.count, INVERSE(period.count));
}

static Period iis_scale(const S3c2400 *s)
{
    unsigned pclk_shift = ((s->clock[5] >> 1) & 1u) + (s->clock[5] & 1u);
    unsigned prescale = s->iis[0] & 2u ? ((s->iis[2] >> 5) & 31u) + 1 : 1;
    /* 384 or 256 periods of the master clock: 3 * 128, or 256. */
    bool wide = (s->iis[1] & 4u) != 0;
    return (Period){wide ? 3 * prescale : prescale, pclk_shift + (wide ? 7 : 8)};
}

static unsigned iis_period(const S3c2400 *s)
{
    Period period = iis_scale(s);
    return period.count << period.shift;
}

static void apply_pending(S3c2400 *s);
static unsigned watchdog_period(const S3c2400 *s);
static unsigned lcd_line_period(const S3c2400 *s);
static unsigned fclk(const S3c2400 *s);
static unsigned rtc_tick_period(const S3c2400 *s);
static void note_read(S3c2400 *s, const uint32_t *reg);

/* Phase of the current line (0 sync, 1 back porch, 2 active, 3 front porch)
   and, in ticks, the position where it ends. A line starts with its active
   part, as a frame starts with its first active line. */
static unsigned lcd_horizontal_status(const S3c2400 *s, unsigned *end)
{
    unsigned period = lcd_line_period(s);
    unsigned clocks = 4 + (s->lcd[3] & 255u) + ((s->lcd[2] >> 8) & 2047u) +
                      (s->lcd[2] & 255u) + ((s->lcd[2] >> 19) & 127u);
    unsigned tick = (2u + 2u * ((s->lcd[0] >> 8) & 1023u)) << ((s->clock[5] >> 1) & 1u);
    *end = (((s->lcd[2] >> 8) & 2047u) + 1) * tick;
    if (s->line_ticks < *end)
        return 2;
    *end += ((s->lcd[2] & 255u) + 1) * tick;
    if (s->line_ticks < *end)
        return 3;
    *end += ((s->lcd[3] & 255u) + 1) * tick;
    if (s->line_ticks < *end)
        return 0;
    *end = period;
    return 1;
}

static unsigned lcd_vertical_status(const S3c2400 *s)
{
    unsigned active = ((s->lcd[1] >> 14) & 1023u) + 1;
    unsigned front = active + ((s->lcd[1] >> 6) & 255u) + 1;
    unsigned sync = front + (s->lcd[1] & 63u) + 1;
    return s->line < active ? 2 : s->line < front ? 3 : s->line < sync ? 0 : 1;
}

static unsigned lcd_frame_lines(const S3c2400 *s)
{
    return 4 + ((s->lcd[1] >> 14) & 1023u) + (s->lcd[1] & 63u) +
           ((s->lcd[1] >> 6) & 255u) + (s->lcd[1] >> 24);
}

static unsigned lcd_active_lines(const S3c2400 *s)
{
    unsigned lines = ((s->lcd[1] >> 14) & 1023u) + 1;
    return lines < GP32_LCD_HEIGHT ? lines : GP32_LCD_HEIGHT;
}

/* Clocks before the next line end which has to be an event.  Only two a frame
   do anything but count: the first blanking line, where the picture is taken
   and which the HLE waits for, and the last, which ends the frame. */
static unsigned lcd_idle_clocks(const S3c2400 *s)
{
    unsigned period = lcd_line_period(s);
    if (period <= s->line_ticks)
        return 0;
    unsigned active = lcd_active_lines(s);
    unsigned target = s->line < active ? active : lcd_frame_lines(s);
    unsigned lines = target > s->line ? target - s->line : 1;
    uint64_t left = (uint64_t)lines * period - s->line_ticks - 1;
    return left > 1u << 30 ? 1u << 30 : (unsigned)left;
}

/* What the port registers read as, from the latches and what drives the pins. */
static inline uint32_t pin_levels(const S3c2400 *s, uint32_t aligned, uint32_t result)
{
    if (aligned == GPIO + 0x0c) {
        result = (result & ~0xff00u) | ((~(uint32_t)s->buttons & 0xffu) << 8);
        if ((s->gpio[2] & 3u) == 0) {
            bool driving = s->smc.image && !(s->gpio[9] & 0x180u);
            result = (result & ~0xffu) | (driving ? s->smc_data : 0xffu);
        }
    } else if (aligned == GPIO + 0x24) {
        result |= 0x200u;
    } else if (aligned == GPIO + 0x30) {
        result = (result & ~0xc4u) | (s->smc.image ? 0 : 4u) |
                 ((~(uint32_t)s->buttons >> 2) & 0xc0u);
    }
    return result;
}

static bool mmio_read(void *context, uint32_t address, uint8_t width,
                      bool side_effects, uint32_t *value)
{
    S3c2400 *s = context;
    uint32_t aligned = address & ~3u;
    uint32_t *reg = register_at(s, aligned);
    unsigned lane = address & 3u;
    (void)side_effects;
    apply_pending(s);
    if (!reg || lane + width > 4)
        return false;
    uint32_t result = *reg;
    if (aligned >= DMA && aligned < DMA + 0x80 && (aligned & 31u) == 12u &&
        s->dma[(aligned - DMA) / 32].active)
        result |= 1u << 20;
    switch (aligned) {
    case LCD:
        result &= 0x3ffffu;
        if ((result & 1u) && s->line <= ((s->lcd[1] >> 14) & 1023u))
            result |= (((s->lcd[1] >> 14) & 1023u) - s->line) << 18;
        break;
    case LCD + 0x10:
        if (s->lcd[0] & 1u) {
            unsigned end;
            result = (result & ~(15u << 17)) | lcd_horizontal_status(s, &end) << 17 |
                     lcd_vertical_status(s) << 19;
        }
        break;
    case GPIO + 0x0c:
    case GPIO + 0x24:
    case GPIO + 0x30:
        result = pin_levels(s, aligned, result);
        break;
    case IIS:
        result = (result & 63u) | 0x40u | (s->audio_count ? 0x80u : 0);
        if (!(result & 1u) || s->audio_ticks >= iis_period(s) / 2)
            result |= 0x100u;
        break;
    case IIS + 12:
        result = (result & ~255u) | (s->audio_count << 4);
        break;
    /* Nothing is wired to the serial ports: transmission is immediate. */
    case UART + 4 * UTRSTAT:
    case UART + UART_PORT + 4 * UTRSTAT:
        result = 6u | ((s->late.uart_full >> (aligned != UART + 4 * UTRSTAT)) & 1u);
        break;
    case SPI + 4: result |= 1u; break;      /* transfer ready */
    case SPI + 0x14: result = 0xffu; break; /* nothing answers */
    }
    *value = result >> (lane * 8);
    if (side_effects) {
        /* Taking a received byte changes the machine, unlike other reads. */
        for (unsigned port = 0; port < 2; ++port)
            if (aligned == UART + port * UART_PORT + 4 * URXH &&
                (s->late.uart_full & (1u << port))) {
                s->late.uart_full &= ~(1u << port);
                return true;
            }
        note_read(s, reg);
    }
    return true;
}

static inline bool bus_observed(const S3c2400 *s)
{
    return M0_OBSERVER(s->bus.hooks.access) || M0_OBSERVER(s->bus.hooks.probe);
}

uint32_t s3c2400_pin_read(S3c2400 *s, unsigned offset)
{
    if (bus_observed(s))
        return s3c2400_bus_read(&s->bus, S3C2400_LOAD, GPIO + offset, 4).value;
    apply_pending(s);
    uint32_t *reg = &s->gpio[offset / 4];
    uint32_t value = pin_levels(s, GPIO + offset, *reg);
    note_read(s, reg);
    return value;
}

static void dma_start(S3c2400Dma *d)
{
    if (!(d->regs[6] & 2u) || d->active)
        return;
    if (!(d->regs[3] & DMA_COUNT)) {
        d->regs[3] = d->regs[2] & DMA_COUNT;
        d->regs[4] = d->regs[0] & DMA_ADDRESS;
        d->regs[5] = d->regs[1] & DMA_ADDRESS;
    }
    d->active = d->regs[3] != 0;
}

void s3c2400_dma_request(S3c2400 *s, unsigned channel, unsigned source)
{
    if (channel < 4 && source < 4) {
        s3c2400_sync(s);
        S3c2400Dma *d = &s->dma[channel];
        if ((d->regs[2] & (1u << 23)) && ((d->regs[2] >> 24) & 3u) == source)
            dma_start(d);
    }
}

static void dma_tick(S3c2400 *s, unsigned channel)
{
    S3c2400Dma *d = &s->dma[channel];
    if (!d->active)
        return;
    uint32_t control = d->regs[2];
    unsigned size = (control >> 20) & 3u;
    unsigned width = 1u << size;
    unsigned beats = control & (1u << 27) ? 4 : 1;
    for (unsigned beat = 0; beat < beats; ++beat) {
        if (size == 3 || !s3c2400_bus_dma_unit(&s->bus, d->regs[4], d->regs[5], width)) {
            d->regs[6] = 0;
            d->active = false;
            return;
        }
        for (unsigned port = 0; port < 2; ++port)
            if (!(d->regs[port] & (1u << 29)))
                d->regs[port + 4] = (d->regs[port + 4] + width) & DMA_ADDRESS;
    }
    if (--d->regs[3] == 0) {
        if (control & (1u << 22))
            d->regs[6] = 0;
        if (control & (1u << 28))
            s3c2400_set_irq(s, 17 + channel);
        d->active = false;
    } else if (!(control & (1u << 26))) {
        d->active = false;
    }
}

static unsigned fclk(const S3c2400 *s)
{
    uint32_t pll = s->clock[1];
    /* At most 263 * 12 MHz, which 32 bits hold. */
    Period divider = {((pll >> 4) & 63u) + 2u, pll & 3u};
    return periods((((pll >> 12) & 255u) + 8u) * 12000000u, divider);
}

unsigned s3c2400_fclk(const S3c2400 *s) { return fclk(s); }

static void iic_stop(S3c2400 *s)
{
    if (s->eeprom_pending) {
        unsigned base = s->eeprom_address & ~31u;
        for (unsigned i = 0; i < 32; ++i)
            if (s->eeprom_pending & (1u << i))
                s->eeprom[base + i] = s->eeprom_page[i];
        s->eeprom_pending = 0;
        s->eeprom_dirty = true;
        s->eeprom_ready = s->cycles + (uint64_t)fclk(s) * 3 / 1000;
    }
}

static void iic_advance(S3c2400 *s, bool start)
{
    unsigned mode = s->iic[1] >> 6 & 3u;
    uint8_t data = s->iic[3];
    if (start) {
        s->iic_phase = 0;
        s->eeprom_pending = 0;
        s->iic_ack = mode >= 2 && (data & 0xfeu) == 0xa0u &&
                     s->cycles >= s->eeprom_ready;
    } else if (s->iic_ack && mode == 2) {
        s->iic[3] = s->eeprom[s->eeprom_address];
        s->eeprom_address = (s->eeprom_address + 1) % GP32_EEPROM_SIZE;
        s->iic_ack = (s->iic[0] & 0x80u) != 0;
    } else if (s->iic_ack && mode == 3) {
        if (s->iic_phase == 0) {
            s->eeprom_high = data;
            ++s->iic_phase;
        } else if (s->iic_phase == 1) {
            s->eeprom_address = ((s->eeprom_high << 8) | data) % GP32_EEPROM_SIZE;
            ++s->iic_phase;
        } else {
            unsigned offset = s->eeprom_address & 31u;
            s->eeprom_page[offset] = data;
            s->eeprom_pending |= 1u << offset;
            s->eeprom_address = (s->eeprom_address & ~31u) | ((offset + 1) & 31u);
        }
    }
    s->iic[1] = (s->iic[1] & ~1u) | !s->iic_ack;
    s->iic[0] |= 0x10u;
    if (s->iic[0] & 0x20u)
        s3c2400_set_irq(s, 27);
}

void s3c2400_set_buttons(S3c2400 *s, uint16_t buttons)
{
    uint16_t previous = s->buttons;
    s->buttons = buttons & 0x3ffu;
    if (previous != s->buttons) {
        button_irqs(s, previous);
        irq_update(s);
    }
}

static void smc_gpio(S3c2400 *s, uint32_t address, uint32_t old, uint32_t next)
{
    if (!s->smc.image || (s->gpio[9] & 0x80u))
        return;
    bool write_enable = s->smc.writable && (s->gpio[9] & 0x40u);
    if (address == GPIO + 0x30 && (next & ~old & 8u) && (s->gpio[2] & 3u) == 1) {
        uint8_t data = s->gpio[3];
        if (next & 0x20u)
            gp32_smc_command(&s->smc, data, write_enable);
        else if (next & 0x10u)
            gp32_smc_address(&s->smc, data);
        else
            gp32_smc_write(&s->smc, data);
    } else if (address == GPIO + 0x24 && (old & ~next & 0x100u) &&
               (s->gpio[2] & 3u) == 0 && !(s->gpio[12] & 0x30u)) {
        s->smc_data = gp32_smc_read(&s->smc, write_enable);
    }
}

static void codec_gpio(S3c2400 *s, uint32_t old, uint32_t next)
{
    const unsigned clock = 1u << 9, mode = 1u << 10, data = 1u << 11;
    if (((s->gpio[11] >> 18) & 63u) != 0x15)
        return;
    if ((old ^ next) & mode) {
        if ((next & mode) && s->l3_bits >= 8)
            s->l3_address = s->l3_shift;
        s->l3_bits = s->l3_shift = 0;
        return;
    }
    if (!(next & ~old & clock))
        return;
    s->l3_shift = (s->l3_shift >> 1) | (next & data ? 0x80 : 0);
    if (s->l3_bits < 8)
        ++s->l3_bits;
    if (!(next & mode) || s->l3_bits < 8)
        return;

    uint8_t value = s->l3_shift;
    s->l3_bits = 0;
    if (s->l3_address == 0x16 && !(value & 0xc0)) {
        s->codec_status = value;
    } else if (s->l3_address == 0x14 && !(value & 0xc0)) {
        s->codec_volume = value; /* recorded, not applied: see s3c2400.h */
    } else if (s->l3_address == 0x14 && (value & 0xc0) == 0x80) {
        s->codec_mute = (value & 4) != 0;
    }
}

static void audio_push(S3c2400 *s, uint16_t value)
{
    if ((s->iis[3] & 0x200u) && s->audio_count < 8) {
        unsigned tail = (s->audio_head + s->audio_count) % 8;
        s->audio_fifo[tail] = value;
        ++s->audio_count;
    }
}

static int16_t audio_pop(S3c2400 *s)
{
    if (!s->audio_count)
        return 0;
    uint16_t value = s->audio_fifo[s->audio_head];
    s->audio_head = (s->audio_head + 1) % 8;
    --s->audio_count;
    return s->iis[1] & 8u ? (int16_t)value : (int16_t)((int8_t)value * 256);
}

static bool audio_running(const S3c2400 *s)
{
    return (s->iis[0] & 9u) == 1 && (s->iis[1] & 0x180u) == 0x80u;
}

/* The transmit FIFO asks DMA channel 2 for data. */
static bool audio_wants_data(const S3c2400 *s)
{
    return (s->iis[0] & 32u) && (s->iis[3] & 0xa00u) == 0xa00u && s->audio_count <= 4;
}

/* Sends one pair of samples to the codec. */
static void audio_sample(S3c2400 *s)
{
    int16_t left = audio_pop(s);
    int16_t right = audio_pop(s);
    if (s->iis[1] & 32u) {
        int16_t first = left;
        left = right;
        right = first;
    }
    if (s->audio.sample) {
        if (s->codec_mute)
            left = right = 0;
        Period scale = iis_scale(s);
        s->audio.sample(s->audio.context, left, right,
                        periods(fclk(s), scale));
    }
}

static void audio_tick(S3c2400 *s)
{
    if (!audio_running(s))
        return;
    if (audio_wants_data(s))
        s3c2400_dma_request(s, 2, 0);
    unsigned period = iis_period(s);
    if (++s->audio_ticks >= period) {
        s->audio_ticks = 0;
        audio_sample(s);
    }
}

/* The port registers whose write can raise an interrupt. */
static inline bool pin_interrupts(uint32_t aligned)
{
    return aligned == GPIO + 0x2c || aligned == GPIO + 0x58;
}

static inline void pins_written(S3c2400 *s, uint32_t aligned, uint32_t old, uint32_t next)
{
    if (aligned == GPIO + 0x24 || aligned == GPIO + 0x30)
        smc_gpio(s, aligned, old, next);
    if (aligned == GPIO + 0x30)
        codec_gpio(s, old, next);
    if (pin_interrupts(aligned)) {
        button_irqs(s, s->buttons);
        irq_update(s);
    }
}

static bool mmio_write(void *context, uint32_t address, uint8_t width, uint32_t value)
{
    S3c2400 *s = context;
    uint32_t aligned = address & ~3u;
    uint32_t *reg = register_at(s, aligned);
    unsigned lane = address & 3u;
    if (!reg || lane + width > 4)
        return false;
    uint32_t mask = width == 4 ? UINT32_MAX : ((1u << (8 * width)) - 1);
    value = (value & mask) << (lane * 8);
    mask <<= lane * 8;
    /* The audio FIFO, the interrupt controller and the pins hold nothing
       clocked: a write there leaves the next peripheral event where it was.
       The pins reach the codec, so the clocks already spent are applied. */
    if (aligned >= GPIO && aligned < GPIO + 0x60 && !pin_interrupts(aligned))
        apply_pending(s);
    else if (aligned == IIS + 16)
        apply_pending(s); /* the samples already due leave the FIFO first */
    else if (!(aligned >= IRQ && aligned < IRQ + 24))
        s3c2400_sync(s);
    uint32_t old = *reg;
    uint32_t next = (old & ~mask) | value;
    if (aligned >= IRQ && aligned < IRQ + 24) {
        if (aligned == IRQ || aligned == IRQ + 16)
            *reg &= ~value;
        else if (aligned != IRQ + 20)
            *reg = next;
        if (aligned == IRQ) {
            button_irqs(s, s->buttons);
            uart_irqs(s);
        }
        irq_update(s);
        return true;
    }
    if (aligned >= DMA && aligned < DMA + 0x80) {
        unsigned index = (aligned & 31u) / 4;
        S3c2400Dma *d = &s->dma[(aligned - DMA) / 32];
        if (index < 3)
            *reg = next;
        if (index == 6) {
            *reg = next & 2u;
            if ((value & 4u) || !*reg) {
                d->active = false;
                *reg = 0;
                d->regs[3] = 0;
            } else if ((value & 1u) && !(d->regs[2] & (1u << 23))) {
                dma_start(d);
            }
        }
        return true;
    }
    if (aligned == IIC + 4)
        next = (next & ~15u) | (old & 15u);
    *reg = next;
    if (aligned >= GPIO && aligned < GPIO + 0x60)
        pins_written(s, aligned, old, next);
    if (aligned == IIS + 16) {
        if (mask & 0xffffu)
            audio_push(s, (uint16_t)value);
        if (mask & 0xffff0000u)
            audio_push(s, (uint16_t)(value >> 16));
    } else if (aligned == IIS + 12 && !(next & 0x200u)) {
        s->audio_count = s->audio_head = 0;
    } else if (aligned == IIS && ((old ^ next) & 1u)) {
        s->audio_ticks = 0;
    } else if (aligned == PWM + 8) {
        for (unsigned i = 0; i < 5; ++i) {
            if (next & (2u << timer_control[i])) {
                s->pwm[timer_observe[i]] = s->pwm[timer_buffer[i]] & 0xffffu;
                s->timer_phase[i] = 0;
            }
        }
    } else if (aligned == IIC + 4 && (mask & 0x20u)) {
        s->iic_start_pending = false;
        if (!(next & 0x20u)) {
            iic_stop(s);
        } else if (next & 0x10u) {
            /* A repeated START written while the previous byte's IRQ is
               pending takes effect when software releases that transfer. */
            s->iic_start_pending = (old & 0xf0u) == 0xf0u &&
                                   (next & 0xf0u) == 0xb0u && (s->iic[0] & 0x10u);
            if (!s->iic_start_pending)
                iic_advance(s, true);
        }
    } else if (aligned == IIC && (old & ~next & 0x10u) && (s->iic[1] & 0x30u) == 0x30u) {
        iic_advance(s, s->iic_start_pending);
        s->iic_start_pending = false;
    } else if (aligned == RTC_TICNT && (mask & 0xffu)) {
        s->rtc_ticks = 0;
    } else if (aligned == LCD && !(old & 1u) && (next & 1u)) {
        s->line = s->line_ticks = 0;
    } else if (aligned == WATCHDOG && (old ^ next) & 0xff38u) {
        s->late.watchdog_phase = 0;
    } else if (aligned == ADC) {
        /* The end of conversion flag is read only; a start completes at once,
           with a full battery on every channel. */
        s->adc[0] = (next & ~0x8000u) | (old & 0x8000u);
        if (next & 1u) {
            s->adc[0] = (next & ~1u) | 0x8000u;
            s->adc[1] = 0x3ffu;
            s3c2400_set_irq(s, IRQ_ADC);
        }
    } else if (aligned == SPI + 0x10 && (s->spi[0] & 0x60u) == 0x20u) {
        s3c2400_set_irq(s, IRQ_SPI);
    } else {
        for (unsigned port = 0; port < 2; ++port) {
            uint32_t base = UART + port * UART_PORT;
            uint32_t control = s->uart[port][UCON];
            if (aligned == base + 4 * UTXH && !lane) {
                if (control & 0x20u) { /* loopback */
                    s->uart[port][URXH] = value & 255u;
                    s->late.uart_full |= 1u << port;
                    if ((control & 3u) == 1)
                        s3c2400_set_irq(s, IRQ_UART_RX + port);
                }
                if ((control & 0xcu) == 4)
                    s3c2400_set_irq(s, IRQ_UART_TX + port);
            } else if (aligned == base + 4 * UCON && (next & 0xcu) == 4 && (old & 0xcu) != 4) {
                s3c2400_set_irq(s, IRQ_UART_TX + port);
            }
        }
    }
    return true;
}

void s3c2400_pin_write(S3c2400 *s, unsigned offset, uint32_t value)
{
    uint32_t aligned = GPIO + offset;
    if (bus_observed(s) || pin_interrupts(aligned)) {
        s3c2400_bus_write(&s->bus, S3C2400_STORE, aligned, 4, value);
        return;
    }
    apply_pending(s);
    uint32_t old = s->gpio[offset / 4];
    s->gpio[offset / 4] = value;
    pins_written(s, aligned, old, value);
}

/* The pin accesses of a card routine follow each other without any time
   between them: the clocks spent are applied once, before the first. */
static inline uint32_t card_get(S3c2400 *s, unsigned offset)
{
    if (bus_observed(s))
        return s3c2400_pin_read(s, offset);
    ++s->quiet_reads;       /* all note_read() does for a port register */
    return pin_levels(s, GPIO + offset, s->gpio[offset / 4]);
}

/* None of the registers these routines write can raise an interrupt. */
static inline void card_set(S3c2400 *s, unsigned offset, uint32_t mask, uint32_t value)
{
    uint32_t next = (card_get(s, offset) & ~mask) | value;
    if (bus_observed(s)) {
        s3c2400_pin_write(s, offset, next);
        return;
    }
    uint32_t old = s->gpio[offset / 4];
    s->gpio[offset / 4] = next;
    pins_written(s, GPIO + offset, old, next);
}

void s3c2400_card_routine(S3c2400 *s, unsigned entry)
{
    enum { DATA_CONFIG = 8, DATA = 12, CONTROL = 0x24, LATCH = 0x30 };
    uint32_t *r = s->cpu.r;
    if (!bus_observed(s))
        apply_pending(s);
    switch (entry) {
    case 0: /* Write one byte and pulse WE. */
        card_set(s, DATA, 0xff, r[0] & 0xff);
        card_set(s, LATCH, 8, 0);
        card_set(s, LATCH, 8, 8);
        break;
    case 1: /* Pulse RE around the data read. */
        card_set(s, CONTROL, 0x100, 0);
        r[0] = card_get(s, DATA) & 0xff;
        card_set(s, CONTROL, 0x100, 0x100);
        break;
    case 2: /* The NAND model completes operations synchronously. */
        r[0] = card_get(s, LATCH);
        if (!(r[0] & 4))
            r[0] = card_get(s, CONTROL);
        break;
    case 3: case 4:
        card_set(s, DATA_CONFIG, 0xffff, entry == 4 ? 0x5555 : 0);
        r[0] = card_get(s, DATA_CONFIG);
        break;
    case 5: case 6:
        card_set(s, LATCH, 0x10, entry == 5 ? 0x10 : 0);
        r[0] = card_get(s, LATCH);
        break;
    case 7: case 8:
        card_set(s, LATCH, 0x20, entry == 7 ? 0x20 : 0);
        r[0] = card_get(s, LATCH);
        break;
    case 9: case 10:
        card_set(s, CONTROL, 0x80, entry == 10 ? 0x80 : 0);
        r[0] = card_get(s, CONTROL);
        break;
    case 11: case 12:
        card_set(s, CONTROL, 0x40, entry == 11 ? 0x40 : 0);
        r[0] = card_get(s, CONTROL);
        break;
    }
}

static uint32_t color(uint32_t value)
{
    unsigned intensity = value & 1u;
    unsigned r = ((value >> 10) & 62u) | intensity;
    unsigned g = ((value >> 5) & 62u) | intensity;
    unsigned b = (value & 62u) | intensity;
    return 0xff000000u | ((r << 2 | r >> 4) << 16) |
           ((g << 2 | g >> 4) << 8) | (b << 2 | b >> 4);
}

typedef struct {
    unsigned bits, lanes, count, page_width;
    uint32_t row, end; /* halfword addresses */
    bool solid;
} LcdLine;

static LcdLine lcd_line(const uint32_t *lcd, unsigned y)
{
    LcdLine line = {0};
    unsigned mode = (lcd[0] >> 1) & 15u;
    if (!(lcd[0] & 1u) || mode < 8 || mode > 12)
        return line;

    line.bits = 1u << (mode - 8);
    unsigned width = ((lcd[2] >> 8) & 2047u) + 1;
    unsigned height = ((lcd[1] >> 14) & 1023u) + 1;
    line.page_width = lcd[7] & 2047u;
    unsigned stride = line.page_width + ((lcd[7] >> 11) & 2047u);
    line.lanes = (lcd[4] & 1u ? 2u : 0u) ^ (lcd[4] & 2u ? 3u : 0u);
    uint32_t base = lcd[5] & 0xfffffffu;
    line.end = (base & ~0x1fffffu) | (lcd[6] & 0x1fffffu);
    if (line.end == (base & ~0x1fffffu))
        line.end += 0x200000u;

    if (y >= height)
        return line;
    line.row = base + y * stride;
    line.solid = (lcd[20] & 0x10000u) != 0;
    unsigned count = line.solid ? width : line.page_width * 16 / line.bits;
    if (count > width) count = width;
    if (count > GP32_LCD_WIDTH) count = GP32_LCD_WIDTH;
    if (!line.solid) {
        /* The fetch stops at the first word starting past the buffer end. */
        uint32_t words = line.row < line.end ? (line.end - line.row + 1) / 2 : 0;
        if (words < (count * line.bits + 31) / 32)
            count = words * 32 / line.bits;
    }
    line.count = count;
    return line;
}

static uint32_t lcd_word(const S3c2400Bus *bus, const LcdLine *line, unsigned word)
{
    uint32_t addr = line->row + word * 2;
    uint32_t data = s3c2400_bus_read(bus, S3C2400_LCD_LOAD, addr * 2, 2).value;
    if (word * 2 + 1 < line->page_width && addr + 1 < line->end)
        data |= s3c2400_bus_read(bus, S3C2400_LCD_LOAD, (addr + 1) * 2, 2).value << 16;
    return data;
}

static void lcd_scanline(const S3c2400 *s, unsigned y, uint32_t *pixels)
{
    for (unsigned x = 0; x < GP32_LCD_WIDTH; ++x)
        pixels[x] = 0xff000000u;

    LcdLine line = lcd_line(s->lcd, y);
    uint32_t data = 0;
    for (unsigned x = 0; x < line.count; ++x) {
        unsigned offset = x * line.bits;
        if (!(offset & 31u) && !line.solid) {
            data = lcd_word(&s->bus, &line, offset / 32);
            uint32_t swapped = 0;
            for (unsigned i = 0; i < 4; ++i)
                swapped |= ((data >> ((i ^ line.lanes) * 8)) & 255u) << (i * 8);
            data = swapped;
        }
        unsigned shift = 32 - line.bits - (offset & 31u);
        uint32_t pixel = (data >> shift) & ((1u << line.bits) - 1);
        if (line.solid) {
            pixel = s->lcd[20] & 0xffffu;
        } else if (line.bits != 16) {
            pixel = s->palette[pixel];
        }
        pixels[x] = color(pixel);
    }
}

/* Copy a whole row from SDRAM when nobody observes the LCD reads. */
static bool lcd_copy(const S3c2400Bus *bus, const LcdLine *line, uint32_t *words, unsigned count)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return false;
#endif
    if (M0_OBSERVER(bus->hooks.access) || !count)
        return false;
    unsigned halves = count * 2;
    if (halves > line->page_width) halves = line->page_width;
    if (halves > line->end - line->row) halves = line->end - line->row;
    /* Only a span held entirely by the SDRAM is copied in one piece. */
    uint32_t offset = line->row * 2 - S3C2400_SDRAM_BASE;
    size_t held = bus->ram_size < S3C2400_BANK_SIZE ? bus->ram_size : S3C2400_BANK_SIZE;
    if (!bus->ram || line->row * 2 < S3C2400_SDRAM_BASE || offset >= held ||
        halves * 2 > held - offset || !gp32_ram_whole(offset, halves * 2))
        return false;
    words[count - 1] = 0;
    memcpy(words, GP32_RAM_SHOWN(bus->ram, offset), halves * 2);
    return true;
}

static void lcd_capture_line(S3c2400 *s, unsigned y)
{
    Gp32LcdCapture *capture = s->lcd_capture;
    Gp32LcdFrame *frame = &capture->frames[capture->back];
    uint32_t *out = frame->lines[y];
    LcdLine line = lcd_line(s->lcd, y);
    if (!y)
        frame->palette_count = 0;
    out[GP32_LCD_FORMAT] = line.bits | line.lanes << 8 | (line.solid ? GP32_LCD_SOLID : 0);
    out[GP32_LCD_COUNT] = line.count;
    out[GP32_LCD_PALETTE_ROW] = 0;
    out[GP32_LCD_COLOR] = s->lcd[20] & 0xffffu;
    if (!line.count || line.solid)
        return;
    if (line.bits != 16) {
        if (!frame->palette_count ||
            memcmp(capture->palette, s->palette, sizeof(capture->palette))) {
            memcpy(capture->palette, s->palette, sizeof(capture->palette));
            for (unsigned i = 0; i < 256; ++i)
                frame->palettes[frame->palette_count][i] = (uint16_t)s->palette[i];
            ++frame->palette_count;
        }
        out[GP32_LCD_PALETTE_ROW] = frame->palette_count - 1;
    }
    unsigned count = (line.count * line.bits + 31) / 32;
    uint32_t *words = out + GP32_LCD_HEADER;
    if (!lcd_copy(&s->bus, &line, words, count))
        for (unsigned i = 0; i < count; ++i)
            words[i] = lcd_word(&s->bus, &line, i);
}

#ifdef M0_GP32_EXTERNAL_RAM
const uint8_t *s3c2400_lcd_row(const S3c2400Bus *bus, const uint32_t *lcd, unsigned y,
                               uint32_t *header, uint32_t *spare)
{
    LcdLine line = lcd_line(lcd, y);
    header[GP32_LCD_FORMAT] = line.bits | line.lanes << 8 | (line.solid ? GP32_LCD_SOLID : 0);
    header[GP32_LCD_COUNT] = line.count;
    header[GP32_LCD_PALETTE_ROW] = 0;
    header[GP32_LCD_COLOR] = lcd[20] & 0xffffu;
    if (!line.count || line.solid)
        return NULL;
    unsigned count = (line.count * line.bits + 31) / 32;
    uint32_t offset = line.row * 2 - S3C2400_SDRAM_BASE;
    if (line.row * 2 >= S3C2400_SDRAM_BASE && offset < GP32_RAM_SIZE &&
        count * 4 <= GP32_RAM_SIZE - offset && !(offset & 3u) &&
        gp32_ram_whole(offset, count * 4))
        return GP32_RAM_SHOWN(bus->ram, offset);
    if (!lcd_copy(bus, &line, spare, count))
        for (unsigned i = 0; i < count; ++i)
            spare[i] = lcd_word(bus, &line, i);
    return (const uint8_t *)spare;
}
#endif

#ifdef M0_GP32_EXTERNAL_RAM
bool s3c2400_lcd_span(const S3c2400 *s, uint32_t *offset, uint32_t *length)
{
    LcdLine first = lcd_line(s->lcd, 0);
    if (!first.count || first.solid)
        return false;
    unsigned height = ((s->lcd[1] >> 14) & 1023u) + 1;
    if (height > GP32_LCD_HEIGHT)
        height = GP32_LCD_HEIGHT;
    LcdLine last = lcd_line(s->lcd, height - 1);
    uint32_t start = first.row * 2, end = (last.row + last.page_width) * 2;
    if (start < S3C2400_SDRAM_BASE || end <= start ||
        end - S3C2400_SDRAM_BASE > GP32_RAM_SIZE)
        return false;
    *offset = start - S3C2400_SDRAM_BASE;
    *length = end - start;
    return true;
}
#endif

static void lcd_render_line(S3c2400 *s, unsigned y)
{
#ifdef M0_GP32_EXTERNAL_RAM
    if (s->lcd_capture)
        lcd_capture_line(s, y);
#else
    if (s->lcd_capture)
        lcd_capture_line(s, y);
    else
        lcd_scanline(s, y, &s->scanout[y * GP32_LCD_WIDTH]);
#endif
}

void s3c2400_screen(S3c2400 *s, uint32_t *argb)
{
    S3c2400Hooks hooks = s->bus.hooks;
    s->bus.hooks = (S3c2400Hooks){0};
    for (unsigned y = 0; y < GP32_LCD_HEIGHT; ++y)
        lcd_scanline(s, y, argb + y * GP32_LCD_WIDTH);
    s->bus.hooks = hooks;
}

/* Reset values of the members that are not zero. */
static void reset_registers(S3c2400 *s)
{
    s->irq[2] = UINT32_MAX;
    s->clock[0] = 0xffffffu;
    s->gpio[0] = 0x3ffffu;
    s->gpio[2] = 0xaaaaaaaau;
    s->gpio[3] = 0xffu;
    s->gpio[9] = 0x1c0u;
    s->gpio[12] = 8u;
    s->smc_data = 0xff;
    s->clock[1] = 0x5c080u;
    s->watchdog[0] = 0x8021u;
    s->watchdog[1] = s->watchdog[2] = 0x8000u;
    s->usbhost[0] = 0x10u; /* HcRevision: OHCI 1.0 */
}

static unsigned to_bcd(unsigned value)
{
    return (value / 10 % 10) << 4 | value % 10;
}

void s3c2400_set_time(S3c2400 *s, S3c2400Time time)
{
    s->rtc[BCDSEC] = to_bcd(time.second);
    s->rtc[BCDMIN] = to_bcd(time.minute);
    s->rtc[BCDHOUR] = to_bcd(time.hour);
    s->rtc[BCDDATE] = to_bcd(time.day);
    s->rtc[BCDDAY] = time.weekday;
    s->rtc[BCDMON] = to_bcd(time.month);
    s->rtc[BCDYEAR] = to_bcd(time.year);
}

/* Adds one to a BCD field; true when it wrapped to `first`. */
static bool bcd_next(uint32_t *field, unsigned first, unsigned last)
{
    unsigned value = ((*field >> 4) & 15u) * 10 + (*field & 15u) + 1;
    bool wrapped = value > last;
    *field = to_bcd(wrapped ? first : value);
    return wrapped;
}

static RARE void rtc_second(S3c2400 *s)
{
    uint32_t *rtc = s->rtc;
    if (bcd_next(&rtc[BCDSEC], 0, 59) && bcd_next(&rtc[BCDMIN], 0, 59) &&
        bcd_next(&rtc[BCDHOUR], 0, 23)) {
        rtc[BCDDAY] = rtc[BCDDAY] % 7 + 1;
        unsigned month = ((rtc[BCDMON] >> 4) & 1u) * 10 + (rtc[BCDMON] & 15u);
        unsigned year = ((rtc[BCDYEAR] >> 4) & 15u) * 10 + (rtc[BCDYEAR] & 15u);
        unsigned days = month == 2 ? (year % 4 ? 28 : 29)
                      : month == 4 || month == 6 || month == 9 || month == 11 ? 30 : 31;
        if (bcd_next(&rtc[BCDDATE], 1, days) && bcd_next(&rtc[BCDMON], 1, 12))
            bcd_next(&rtc[BCDYEAR], 0, 99);
    }
    /* RTCALM: a global enable, then one bit per compared field. */
    uint32_t alarm = rtc[RTCALM];
    if (!(alarm & 0x40u) || !(alarm & 0x3fu))
        return;
    static const unsigned field[] = {BCDSEC, BCDMIN, BCDHOUR, BCDDATE, BCDMON, BCDYEAR};
    for (unsigned i = 0; i < 6; ++i)
        if ((alarm & (1u << i)) && (rtc[ALMSEC + i] & 255u) != rtc[field[i]])
            return;
    s3c2400_set_irq(s, IRQ_ALARM);
}

void s3c2400_upgrade_state(S3c2400 *s, bool hle)
{
    memset(&s->late, 0, sizeof(s->late));
    s->late.rtc_second = s->cycles + fclk(s);
    /* The first format never ran the watchdog, so HLE states left it on. */
    if (hle)
        s->watchdog[0] = 0;
}

/* FCLK cycles per watchdog count; zero while stopped. */
static Period watchdog_scale(const S3c2400 *s)
{
    uint32_t control = s->watchdog[0];
    if (!(control & 0x20u))
        return (Period){0, 0};
    unsigned pclk_shift = ((s->clock[5] >> 1) & 1u) + (s->clock[5] & 1u);
    return (Period){((control >> 8) & 255u) + 1, ((control >> 3) & 3u) + 4 + pclk_shift};
}

static unsigned watchdog_period(const S3c2400 *s)
{
    Period period = watchdog_scale(s);
    return period.count << period.shift;
}

/* A watchdog reset restarts the processor and the peripherals. Memory, the
   card, the EEPROM and the calendar keep their contents. Without a boot ROM
   there is nothing to restart: the application has ended. */
static RARE void watchdog_reset(S3c2400 *s)
{
    if (!s->bus.boot_rom) {
        s->watchdog[0] = 0;
        s->hle_exited = true;
        return;
    }
    memset(&s->line, 0, offsetof(S3c2400, rtc) - offsetof(S3c2400, line));
    memset(s->adc, 0, offsetof(S3c2400, late) - offsetof(S3c2400, adc));
    s->audio_head = s->audio_count = s->audio_ticks = 0;
    s->iic_phase = s->eeprom_pending = 0;
    s->iic_ack = s->iic_start_pending = false;
    s->eeprom_ready = 0;
    s->l3_bits = s->l3_shift = s->l3_address = s->codec_status = 0;
    s->codec_mute = false;
    s->late.watchdog_phase = s->late.uart_full = 0;
    uint16_t buttons = s->buttons;
    reset_registers(s);
    s->buttons = buttons;
    Arm920tConfig config = s->cpu.config;
    arm920t_reset(&s->cpu, config, false);
}

static RARE void watchdog_tick(S3c2400 *s)
{
    unsigned period = watchdog_period(s);
    if (!period || ++s->late.watchdog_phase < period)
        return;
    s->late.watchdog_phase = 0;
    uint32_t *count = &s->watchdog[2];
    if ((*count & 0xffffu) > 1) {
        --*count;
        return;
    }
    *count = s->watchdog[1] & 0xffffu;
    if (s->watchdog[0] & 4u)
        s3c2400_set_irq(s, IRQ_WATCHDOG);
    if (s->watchdog[0] & 1u)
        watchdog_reset(s);
}

bool s3c2400_boot(S3c2400 *s, const uint8_t *bios, size_t size,
                  Arm920tConfig cpu_hooks, S3c2400Hooks bus_hooks, S3c2400Video video)
{
    if (!s || size != (bios ? GP32_BIOS_SIZE : 0))
        return false;
#ifdef M0_GP32_EXTERNAL_RAM
    uint8_t *ram = s->ram;
    if (!ram)
        return false;
    memset(s, 0, sizeof(*s));
    gp32_ram_fill(ram, 0, 0, GP32_RAM_SIZE);
    s->ram = ram;
#else
    memset(s, 0, sizeof(*s));
#endif
    s->bus = (S3c2400Bus){
        .boot_rom = bios, .boot_rom_size = size, .ram = s->ram, .ram_size = GP32_RAM_SIZE,
        .mmio = {mmio_read, mmio_write, s}, .hooks = bus_hooks, .absent_sdram_open_bus = true
    };
    s->video = video;
#ifndef M0_GP32_EXTERNAL_RAM
    for (unsigned i = 0; i < GP32_LCD_WIDTH * GP32_LCD_HEIGHT; ++i)
        s->pixels[i] = s->scanout[i] = 0xff000000u;
#endif
    s->cycles_per_instruction = 1;
    memset(s->eeprom, 0xff, sizeof(s->eeprom));
    reset_registers(s);
    s3c2400_set_time(s, (S3c2400Time){.year = 1, .month = 1, .day = 1, .weekday = 2});
    s->late.rtc_second = fclk(s);
    cpu_hooks.bus = &s->bus;
    arm920t_reset(&s->cpu, cpu_hooks, false);
    return true;
}

static Period timer_scale(const S3c2400 *s, unsigned timer)
{
    if (!(s->pwm[2] & (1u << timer_control[timer])))
        return (Period){0, 0};
    unsigned mux = (s->pwm[1] >> (4 * timer)) & 15u;
    if (mux > 3)
        return (Period){0, 0};
    unsigned prescale = (s->pwm[0] >> (timer < 2 ? 0 : 8)) & 255u;
    unsigned pclk_shift = ((s->clock[5] >> 1) & 1u) + (s->clock[5] & 1u);
    return (Period){prescale + 1, mux + 1 + pclk_shift};
}

static unsigned timer_period(const S3c2400 *s, unsigned timer)
{
    Period period = timer_scale(s, timer);
    return period.count << period.shift;
}

/* The periods of the running timers, derived from four registers.  The machine
   asks for them around every event: they are worked out again only when one of
   those registers differs.  A function of the values alone, so not part of the
   machine's state. */
typedef struct {
    uint32_t key[4];
    unsigned running;           /* one bit per timer which counts */
    unsigned period[5];
    Period scale[5];
} Timers;
/* One per thread: a program may run one machine per thread. A build which runs
   a single machine defines M0_GP32_ONE_MACHINE and gets a plain variable,
   cheaper where thread local storage is emulated (MinGW). */
#ifdef M0_GP32_ONE_MACHINE
static Timers timers;
#else
static _Thread_local Timers timers;
#endif

static RARE void timers_derive(const S3c2400 *s)
{
    memcpy(timers.key, s->pwm, 3 * sizeof *timers.key);
    timers.key[3] = s->clock[5];
    timers.running = 0;
    for (unsigned i = 0; i < 5; ++i) {
        timers.scale[i] = timer_scale(s, i);
        timers.period[i] = timers.scale[i].count << timers.scale[i].shift;
        if (timers.period[i])
            timers.running |= 1u << i;
    }
}

static inline const Timers *timers_of(const S3c2400 *s)
{
    if (__builtin_expect(((s->pwm[0] ^ timers.key[0]) | (s->pwm[1] ^ timers.key[1]) |
                          (s->pwm[2] ^ timers.key[2]) | (s->clock[5] ^ timers.key[3])) != 0, 0))
        timers_derive(s);
    return &timers;
}

static void timers_tick(S3c2400 *s)
{
    const Timers *t = timers_of(s);
    for (unsigned i = 0; i < 5; ++i) {
        unsigned period = t->period[i];
        if (!period || ++s->timer_phase[i] < period)
            continue;
        unsigned control = s->pwm[2] >> timer_control[i];
        s->timer_phase[i] = 0;
        uint32_t *count = &s->pwm[timer_observe[i]];
        if (*count && --*count)
            continue;
        unsigned dma_select = (s->pwm[1] >> 20) & 15u;
        if (dma_select == i + 1) {
            s3c2400_dma_request(s, 0, 3);
            s3c2400_dma_request(s, 2, 3);
            s3c2400_dma_request(s, 3, 3);
        } else {
            s3c2400_set_irq(s, 10 + i);
        }
        if (control & (i == 4 ? 4u : 8u))
            *count = s->pwm[timer_buffer[i]] & 0xffffu;
        else
            s->pwm[2] &= ~(1u << timer_control[i]);
        /* An expiry, and the transfers it requests, may change the periods. */
        timers_of(s);
    }
}

static unsigned lcd_line_period(const S3c2400 *s)
{
    unsigned clocks = 4 + (s->lcd[3] & 255u) + ((s->lcd[2] >> 8) & 2047u) +
                      (s->lcd[2] & 255u) + ((s->lcd[2] >> 19) & 127u);
    unsigned ticks = clocks * (2u + 2u * ((s->lcd[0] >> 8) & 1023u));
    return ticks << ((s->clock[5] >> 1) & 1u);
}

/* The RTC tick interrupt fires every (count + 1) / 128 second while TICNT
   enables it. The period is expressed in FCLK cycles; zero means disabled. */
static unsigned rtc_tick_period(const S3c2400 *s)
{
    uint32_t control = s->rtc[(RTC_TICNT - RTC) / 4];
    if (!(control & 0x80u))
        return 0;
    uint64_t period = (uint64_t)fclk(s) * ((control & 127u) + 1) / 128;
    return period ? (unsigned)period : 1;
}

/* Programs show a new picture by changing buffers, not by writing the one on
   screen or the LCD registers while it is scanned: the panel is fetched at
   once. */
static void lcd_render_panel(S3c2400 *s)
{
    for (unsigned y = 0; y < GP32_LCD_HEIGHT; ++y)
        lcd_render_line(s, y);
}

static void peripheral_tick(S3c2400 *s)
{
    ++s->cycles;
    if (s->iis[0] & 1u)
        audio_tick(s);
    for (unsigned i = 0; i < 4; ++i)
        dma_tick(s, i);
    if (s->pwm[2] & TIMERS_ON)
        timers_tick(s);
    if (s->rtc[(RTC_TICNT - RTC) / 4] & 0x80u) {
        if (++s->rtc_ticks >= rtc_tick_period(s)) {
            s->rtc_ticks = 0;
            s3c2400_set_irq(s, IRQ_TICK);
        }
    }
    if (s->cycles >= s->late.rtc_second) {
        s->late.rtc_second += fclk(s);
        rtc_second(s);
    }
    if (s->lcd[0] & 1u) {
        if (++s->line_ticks >= lcd_line_period(s)) {
            s->line_ticks = 0;
            if (++s->line == lcd_active_lines(s))
                lcd_render_panel(s);
            if (s->line >= lcd_frame_lines(s)) {
                s->line = 0;
                ++s->frames;
                if (s->lcd_capture) {
                    s->lcd_capture->back ^= 1u;
                }
#ifndef M0_GP32_EXTERNAL_RAM
                else {
                    memcpy(s->pixels, s->scanout, sizeof(s->pixels));
                    if (s->video.frame)
                        s->video.frame(s->video.context, s->pixels, GP32_LCD_WIDTH, GP32_LCD_HEIGHT);
                }
#endif
            }
        }
    }
    /* Last: a reset leaves the other peripherals nothing to do this cycle. */
    if (s->watchdog[0] & 0x20u)
        watchdog_tick(s);
}

/* No register read changes the machine. A timer count and the transmit
   status of the audio port move between events: bound their validity. */
static void note_read(S3c2400 *s, const uint32_t *reg)
{
    ++s->quiet_reads;
    /* Only these registers move on their own; any other read stays valid. */
    bool timer = (uintptr_t)reg - (uintptr_t)s->pwm < sizeof s->pwm;
    /* The audio port and the count and addresses of its DMA channel. */
    bool audio = reg == &s->iis[0] || reg == &s->iis[3] ||
                 (uintptr_t)reg - (uintptr_t)&s->dma[2].regs[3] < 3 * sizeof *reg;
    if (!timer && !audio && reg != &s->lcd[0] && reg != &s->lcd[4] &&
        reg != &s->watchdog[2])
        return;
    uint64_t change = UINT64_MAX;
    for (unsigned i = 0; timer && i < 5; ++i) {
        unsigned period;
        if (reg != &s->pwm[timer_observe[i]] || !(period = timer_period(s, i)))
            continue;
        change = s->cycles + (s->timer_phase[i] < period ? period - s->timer_phase[i] : 1);
    }
    if (audio && audio_running(s)) {
        /* The next transfer, sample or change of the half period flag. */
        unsigned period = iis_period(s), ticks = s->audio_ticks;
        unsigned at = reg == &s->iis[0] && ticks < period / 2 ? period / 2 : period;
        change = s->cycles + (audio_wants_data(s) || at <= ticks ? 1 : at - ticks);
    }
    if (reg == &s->lcd[4] && (s->lcd[0] & 1u)) {
        unsigned end;
        lcd_horizontal_status(s, &end);
        change = s->cycles + (end > s->line_ticks ? end - s->line_ticks : 1);
    }
    /* The line counter, which may move without an event. */
    if (reg == &s->lcd[0] && (s->lcd[0] & 1u)) {
        unsigned period = lcd_line_period(s);
        change = s->cycles + (period > s->line_ticks ? period - s->line_ticks : 1);
    }
    unsigned watchdog = watchdog_period(s);
    if (reg == &s->watchdog[2] && watchdog)
        change = s->cycles + (s->late.watchdog_phase < watchdog
                              ? watchdog - s->late.watchdog_phase : 1);
    if (change < s->stable_until)
        s->stable_until = change;
}

/* Samples processed without an event, at most.  Above 1, a transfer may read
   memory the program wrote after the transfer's clock: for slow targets only. */
#ifndef M0_GP32_AUDIO_BATCH
#define M0_GP32_AUDIO_BATCH 1u
#endif

/* Whether the audio port is fed one transfer at a time from memory by DMA
   channel 2, with nobody watching the accesses: the transfers and samples can
   then be done late, in the same order, as long as the count does not run out.
   A register access applies what is due first; only the memory a transfer reads
   may by then hold what the program wrote after the transfer's clock. */
static inline bool audio_fed(const S3c2400 *s)
{
    const S3c2400Dma *d = &s->dma[2];
    uint32_t control = d->regs[2], size = (control >> 20) & 3u;
    return !bus_observed(s) && !M0_OBSERVER(s->bus.hooks.copy) &&
           (control & (1u << 23)) && !((control >> 24) & 3u) && !(control & (3u << 26)) &&
           (size == 1 || size == 2) && (d->regs[1] & (1u << 29)) &&
           d->regs[5] == IIS + 16 && (d->regs[6] & 2u) && !d->active;
}

/* The clocks of audio_tick() and of the transfers it requests. */
static void audio_elapse(S3c2400 *s, unsigned clocks)
{
    unsigned period = iis_period(s);
    while (clocks) {
        unsigned run = 1;
        bool transfer = audio_wants_data(s);
        if (transfer) {
            dma_start(&s->dma[2]);
        } else {
            run = period - s->audio_ticks;
            if (run > clocks)
                run = clocks;
        }
        clocks -= run;
        if ((s->audio_ticks += run) >= period) {
            s->audio_ticks = 0;
            audio_sample(s);
        }
        if (transfer)
            dma_tick(s, 2); /* after the sample, as in peripheral_tick() */
    }
}

/* Clocks that can elapse before a peripheral does anything but count. */
static GP32_HOT unsigned idle_clocks(const S3c2400 *s)
{
    unsigned idle = 1u << 30;
#define UNTIL(period, phase) do { unsigned p_ = (period), t_ = (phase), \
    left_ = p_ > t_ ? p_ - t_ - 1 : 0; if (left_ < idle) idle = left_; } while (0)
    if (s->lcd[0] & 1u) {
        unsigned left = lcd_idle_clocks(s);
        if (left < idle)
            idle = left;
    }
    if (audio_running(s)) {
        /* A sample takes two transfers at most, and up to five may be owed:
           while more remain, the channel's interrupt is not among them. */
        uint32_t remaining = s->dma[2].regs[3] & DMA_COUNT, batch = 1;
        if (remaining > 8 && audio_fed(s))
            batch = (remaining - 8) / 2 + 1;
        else if (audio_wants_data(s) && (s->dma[2].regs[2] & (1u << 23)) &&
                 !((s->dma[2].regs[2] >> 24) & 3u) && (s->dma[2].regs[6] & 2u) &&
                 !s->dma[2].active)
            return 0;
        if (batch > M0_GP32_AUDIO_BATCH)
            batch = M0_GP32_AUDIO_BATCH;
        unsigned period = iis_period(s);
        uint64_t span = (uint64_t)period * batch;
        UNTIL(span > 1u << 30 ? period : (unsigned)span, s->audio_ticks);
    }
    for (unsigned i = 0; i < 4; ++i)
        if (s->dma[i].active)
            return 0;
    unsigned tick = rtc_tick_period(s);
    if (tick)
        UNTIL(tick, s->rtc_ticks);
    const Timers *t = timers_of(s);
    for (unsigned mask = t->running; mask; mask &= mask - 1) {
        /* Counting down is not an event: only reaching zero is. */
        unsigned i = (unsigned)__builtin_ctz(mask), period = t->period[i];
        {
            uint32_t count = s->pwm[timer_observe[i]];
            uint64_t span = s->timer_phase[i] < period && count > 1
                ? (uint64_t)period * count : period;
            UNTIL(span > 1u << 30 ? 1u << 30 : (unsigned)span, s->timer_phase[i]);
        }
    }
    unsigned watchdog = watchdog_period(s);
    if (watchdog) {
        uint32_t count = s->watchdog[2] & 0xffffu;
        uint64_t span = s->late.watchdog_phase < watchdog && count > 1
            ? (uint64_t)watchdog * count : watchdog;
        UNTIL(span > 1u << 30 ? 1u << 30 : (unsigned)span, s->late.watchdog_phase);
    }
    uint64_t second = s->late.rtc_second > s->cycles ? s->late.rtc_second - s->cycles - 1 : 0;
    if (second < idle)
        idle = (unsigned)second;
#undef UNTIL
    return idle;
}

static GP32_HOT void apply_pending(S3c2400 *s)
{
    unsigned clocks = s->pending_clocks;
    if (!clocks)
        return;
    s->pending_clocks = 0;
    s->cycles += clocks;
    if (s->lcd[0] & 1u) {
        s->line_ticks += clocks;
        /* Lines which ended without an event; never one of those two. */
        for (unsigned period = lcd_line_period(s); s->line_ticks >= period; ++s->line)
            s->line_ticks -= period;
    }
    if (audio_running(s)) {
        if (audio_fed(s))
            audio_elapse(s, clocks);
        else
            s->audio_ticks += clocks;
    }
    if (s->rtc[(RTC_TICNT - RTC) / 4] & 0x80u)
        s->rtc_ticks += clocks;
    const Timers *t = timers_of(s);
    for (unsigned left = t->running; left; left &= left - 1) {
        unsigned i = (unsigned)__builtin_ctz(left), period = t->period[i];
        Period scale = t->scale[i];
        /* A phase and the clocks of one span: far below 2^32. */
        uint32_t phase = s->timer_phase[i] + clocks;
        if (phase >= period) {
            uint32_t elapsed = periods(phase, scale);
            s->pwm[timer_observe[i]] -= elapsed;
            phase -= elapsed * period;
        }
        s->timer_phase[i] = phase;
    }
    Period scale = watchdog_scale(s);
    unsigned watchdog = scale.count << scale.shift;
    if (watchdog) {
        uint32_t phase = s->late.watchdog_phase + clocks;
        if (phase >= watchdog) {
            uint32_t elapsed = periods(phase, scale);
            s->watchdog[2] -= elapsed;
            phase -= elapsed * watchdog;
        }
        s->late.watchdog_phase = phase;
    }
}

void s3c2400_sync(S3c2400 *s)
{
    apply_pending(s);
    s->idle_budget = 0;
}

/* Spans without a peripheral event are counted at once. The clock of an
   event takes the ordinary tick, which retains bus ordering across DMA, timer
   and LCD events. Returns the clocks that may follow without an event; the
   clocks after the last event stay pending, to be applied with later ones.
   What is left of the last budget is a span already known to be without an
   event: it is looked for again only when none is left. */
static GP32_HOT unsigned elapse(S3c2400 *s, unsigned clocks)
{
    unsigned span = s->idle_budget;
    s->idle_budget = 0;
    if (!span) {
        apply_pending(s);
        span = idle_clocks(s);
    }
    for (;;) {
        if (span >= clocks) {
            s->pending_clocks += clocks;
            return span - clocks;
        }
        s->pending_clocks += span;
        clocks -= span;
        apply_pending(s);
        /* The clock after an idle span carries an event: it takes the ordinary
           tick without looking again. */
        peripheral_tick(s);
        span = idle_clocks(s);
        if (!--clocks)
            return span;
    }
}

static inline Arm920tStep completed(S3c2400 *s, Arm920tStep result)
{
    if (result == ARM920T_EXCEPTION &&
        (s->cpu.r[15] == 0x18 || s->cpu.r[15] == 0xffff0018u) && s->irq_handler) {
        s3c2400_sync(s);
        if (s->irq_handler(s, s->irq[5]))
            result = ARM920T_INSTRUCTION;
    }
    ++s->steps;
    return result;
}

static inline Arm920tStep instruction(S3c2400 *s)
{
    return completed(s, arm920t_step(&s->cpu));
}

Arm920tStep s3c2400_step(S3c2400 *s)
{
    Arm920tStep result = instruction(s);
    elapse(s, s->cycles_per_instruction);
    apply_pending(s);
    return result;
}

Arm920tStep s3c2400_step_batched(S3c2400 *s)
{
    Arm920tStep result = instruction(s);
    unsigned cost = s->cycles_per_instruction;
    if (cost <= s->idle_budget) {
        s->idle_budget -= cost;
        s->pending_clocks += cost;
    } else {
        ++s->events;
        s->idle_budget = elapse(s, cost);
    }
    return result;
}

/* State at the last arrival on the target of a short backward branch. */
typedef struct {
    uint32_t r[16], cpsr, effects, device_reads, quiet_reads;
    uint64_t steps, events;
#if !M0_GP32_DIVIDE
    uint32_t span;              /* clocks of the last long loop skipped, */
    uint64_t span_inverse;      /* and 2^32 over them */
#endif
    bool valid;
} Loop;

/* A loop that returns to the same registers without any effect, with no
   peripheral visit in between, repeats identically until the next event:
   let whole iterations elapse without interpreting them. */
static void skip_loop(S3c2400 *s, Loop *loop, uint64_t *steps, unsigned cost)
{
    const Arm920t *cpu = &s->cpu;
    if (loop->valid && loop->r[15] == cpu->r[15] && loop->events == s->events &&
        loop->effects == cpu->effects && loop->cpsr == cpu->cpsr && !cpu->write_count &&
        cpu->device_reads - loop->device_reads == s->quiet_reads - loop->quiet_reads &&
        !memcmp(loop->r, cpu->r, sizeof loop->r)) {
        uint64_t length = s->steps - loop->steps;
        if (length && cost) {
            uint64_t now = s->cycles + s->pending_clocks;
            uint64_t valid = s->stable_until > now ? s->stable_until - now - 1 : 0;
            /* The clocks whole turns may fill: the budget, the validity of
               what was read, and the instructions the caller accepts. */
            uint64_t room = s->idle_budget < valid ? s->idle_budget : valid;
            if (*steps < room && *steps * cost < room)
                room = *steps * cost;
            uint64_t turn = length * cost, count = 0;
#if M0_GP32_DIVIDE
            if (turn <= room)
                count = (uint32_t)room / (uint32_t)turn;
#else
            if (turn <= 256 && turn <= room) {
                count = arm920t_quotient((uint32_t)room, (uint32_t)turn, INVERSE(turn));
            } else if (turn <= room) {
                if (turn != loop->span) {
                    loop->span = (uint32_t)turn;
                    loop->span_inverse = ((UINT64_C(1) << 32) + turn - 1) / turn;
                }
                count = arm920t_quotient((uint32_t)room, loop->span, loop->span_inverse);
            }
#endif
            uint64_t skipped = count * length;
            s->idle_budget -= (unsigned)(skipped * cost);
            s->pending_clocks += (unsigned)(skipped * cost);
            s->steps += skipped;
            s->skipped_steps += skipped;
            *steps -= skipped;
        }
    }
    memcpy(loop->r, cpu->r, sizeof loop->r);
    loop->cpsr = cpu->cpsr;
    loop->effects = cpu->effects;
    loop->device_reads = cpu->device_reads;
    loop->quiet_reads = s->quiet_reads;
    s->stable_until = UINT64_MAX;
    loop->steps = s->steps;
    loop->events = s->events;
    loop->valid = true;
}

/* Count the steps which fit before the next event, up to those accepted, as
   done without running anything. */
static inline void skip_steps(S3c2400 *s, uint64_t *steps, unsigned cost, uint64_t inverse)
{
    uint64_t skip = arm920t_quotient(s->idle_budget, cost, inverse);
    if (skip > *steps)
        skip = *steps;
    s->idle_budget -= (unsigned)skip * cost;
    s->pending_clocks += (unsigned)skip * cost;
    s->steps += skip;
    s->skipped_steps += skip;
    *steps -= skip;
}

/* The HLE routine reading a byte from the card (see gp32_hle.c). */
#define CARD_READ 0x0c7ab808u

/* A guest address range, as an offset in SDRAM, or false when outside it. */
static bool in_sdram(uint32_t address, uint32_t length, uint32_t *offset)
{
    *offset = address - S3C2400_SDRAM_BASE;
    return *offset < GP32_RAM_SIZE && length <= GP32_RAM_SIZE - *offset;
}

static uint32_t ram_word(S3c2400 *s, uint32_t offset)
{
    uint32_t value;
    __builtin_memcpy(&value, GP32_RAM(s->ram, offset), 4);
    return value;
}

/* The SDK reads a card page with a loop calling the routine above for every
   byte.  Just after such a call, do the following turns directly, as long as no
   peripheral event falls within them: the same registers, flags, memory, card
   state, instructions and clocks. */
static __attribute__((noinline)) void card_stream(S3c2400 *s, uint64_t *steps, unsigned cost)
{
    /* mov lr, pc; ldr pc, [sl]; strb r0, [r7]; add r8, r8, #1; cmp r8, r6;
       add r7, r7, #1; blo, back to the first */
    static const uint32_t loop[] = {0xe1a0e00fu, 0xe59af000u, 0xe5c70000u, 0xe2888001u,
                                    0xe1580006u, 0xe2877001u, 0x3afffff8u};
    enum { TURN = 9 }; /* the loop, the call and the return */
    Arm920t *cpu = &s->cpu;
    uint32_t *r = cpu->r;
    uint32_t code, pointer, stub, to;
    unsigned turn = TURN * cost;
    if (r[15] != CARD_READ + 4 || s->idle_budget < turn || *steps < TURN ||
        r[8] + 1u >= r[6] || (cpu->control & 0x1087u) || cpu->write_count ||
        (cpu->irq && !(cpu->cpsr & 0x80u)) || (cpu->fiq_line && !(cpu->cpsr & 0x40u)))
        return;
    uint32_t left = r[6] - r[8] - 1u;
    if ((r[14] & 3u) || !in_sdram(r[14] - 8u, sizeof loop, &code) ||
        (r[10] & 3u) || !in_sdram(r[10], 4, &pointer) || !in_sdram(CARD_READ, 8, &stub) ||
        !in_sdram(r[7], left, &to))
        return;
    for (unsigned i = 0; i < sizeof loop / sizeof *loop; ++i)
        if (ram_word(s, code + 4 * i) != loop[i])
            return;
    if (ram_word(s, pointer) != CARD_READ || ram_word(s, stub) != 0xef7f0011u ||
        ram_word(s, stub + 4) != 0xe12fff1eu)
        return;
    /* The bytes must not change what runs the loop. */
    if ((to < code + sizeof loop && code < to + left) || (to < stub + 8 && stub < to + left) ||
        (to < pointer + 4 && pointer < to + left))
        return;
    /* The pins as the routine leaves them, selecting a card which drives the
       data pins while its read pulse is low. */
    if (!s->smc.image || (s->gpio[2] & 3u) || (s->gpio[12] & 0x30u) ||
        (s->gpio[9] & 0x180u) != 0x100u || pin_interrupts(GPIO + 0x24))
        return;
    bool write_enable = s->smc.writable && (s->gpio[9] & 0x40u);
    do {
        *GP32_RAM(s->ram, to++) = (uint8_t)r[0];
        ++r[7];
        ++r[8];
        s->smc_data = gp32_smc_read(&s->smc, write_enable);
        r[0] = s->smc_data;
        s->idle_budget -= turn;
        s->pending_clocks += turn;
        s->steps += TURN;
        *steps -= TURN;
        s->quiet_reads += 3;
    } while (r[8] + 1u < r[6] && s->idle_budget >= turn && *steps >= TURN);
    /* cmp r8, r6, with r8 below r6 */
    uint32_t difference = r[8] - r[6];
    uint32_t overflow = ((r[8] ^ r[6]) & (r[8] ^ difference)) >> 31;
    cpu->cpsr = (cpu->cpsr & 0x0fffffffu) | (difference & 0x80000000u) | overflow << 28;
    ++cpu->effects;
}

GP32_HOT S3c2400Stop s3c2400_run(S3c2400 *s, uint64_t steps)
{
    Loop loop = {.valid = false};
    const Arm920tConfig *hooks = &s->cpu.config;
    bool observed = M0_OBSERVER(hooks->trace) || M0_OBSERVER(hooks->predicate) ||
                    M0_OBSERVER(hooks->access) || bus_observed(s);
    unsigned cost = s->cycles_per_instruction;
#if M0_GP32_DIVIDE
    const uint64_t inverse = 0;
#else
    const uint64_t inverse = !cost ? 0 : cost <= 256 ? INVERSE(cost)
        : ((UINT64_C(1) << 32) + cost - 1) / cost;
#endif
    uint64_t frame = s->frames;
    S3c2400Stop stop = S3C2400_STOP_STEPS;
    s->hle_waiting = false;
    while (steps) {
        uint32_t pc = s->cpu.r[15];
        Arm920tStep result;
        unsigned extra = 0;
#ifdef M0_GP32_AOT
        if (!observed && cost) {
            Arm920tAot run = {&s->steps, steps, &s->idle_budget, &s->pending_clocks,
                              cost, &s->interpreted_steps, pc, 0, s->cpu.effects, inverse};
            result = arm920t_aot_run(&s->cpu, &run);
            pc = run.pc;
            steps = run.left;
            extra = run.extra;
            result = completed(s, result);
        } else
#endif
            result = instruction(s);
        --steps;
        if (cost <= s->idle_budget && !extra) {
            s->idle_budget -= cost;
            s->pending_clocks += cost;
            /* A sleeping core repeats this step until an interrupt, which
               only a peripheral event or the caller can raise. */
            if (result == ARM920T_WAITING && cost && !s->cpu.irq && !s->cpu.fiq_line)
                skip_steps(s, &steps, cost, inverse);
            if (pc - s->cpu.r[15] < 128u && result == ARM920T_INSTRUCTION && !observed)
                skip_loop(s, &loop, &steps, cost);
        } else {
            ++s->events;
            s->idle_budget = elapse(s, cost + extra);
        }
        if (s->hle_waiting) {
            s->hle_waiting = false;
            /* Only an interrupt the core accepts would run something else
               before the next event. */
            /* The call saw the machine up to its own step: the next ones
               would see the same only if that step crossed no event, leaving
               all its clocks pending.  Every step also drains a buffered
               write. */
            if (cost && s->pending_clocks == cost + extra && !observed && !s->cpu.write_count &&
                (!s->cpu.irq || (s->cpu.cpsr & 0x80u)) &&
                (!s->cpu.fiq_line || (s->cpu.cpsr & 0x40u)))
                skip_steps(s, &steps, cost, inverse);
        }
        /* Not past the end of a frame: the caller gets it without delay. */
        if (pc == CARD_READ && result == ARM920T_INSTRUCTION && !observed && cost &&
            s->frames == frame)
            card_stream(s, &steps, cost);
        if (result == ARM920T_EXCEPTION) {
            stop = S3C2400_STOP_EXCEPTION;
            break;
        }
        if (s->hle_exited) {
            stop = S3C2400_STOP_EXIT;
            break;
        }
        if (s->frames != frame) {
            stop = S3C2400_STOP_FRAME;
            break;
        }
    }
    s3c2400_sync(s);
    return stop;
}

