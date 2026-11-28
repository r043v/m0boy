#ifndef M0BOY_S3C2400_H
#define M0BOY_S3C2400_H

#include "arm920t.h"
#include "gp32_smc.h"

#define GP32_RAM_SIZE 0x800000u
#define GP32_BIOS_SIZE 0x80000u
#define GP32_EEPROM_SIZE 4096u
#define GP32_LCD_WIDTH 240u
#define GP32_LCD_HEIGHT 320u

enum {
    GP32_LEFT = 1u << 0, GP32_DOWN = 1u << 1, GP32_RIGHT = 1u << 2,
    GP32_UP = 1u << 3, GP32_L = 1u << 4, GP32_B = 1u << 5,
    GP32_A = 1u << 6, GP32_R = 1u << 7, GP32_START = 1u << 8,
    GP32_SELECT = 1u << 9
};

typedef struct {
    void (*sample)(void *context, int16_t left, int16_t right, unsigned rate);
    void *context;
} S3c2400Audio;

typedef struct {
    void (*frame)(void *context, const uint32_t *argb, unsigned width, unsigned height);
    void *context;
} S3c2400Video;

/* A frame as the LCD controller fetched it, left for the host to decode.
   Each panel row starts with a header, followed by the framebuffer words as
   read from memory, before the byte and halfword swaps. */
enum {
    GP32_LCD_FORMAT,      /* bits per pixel | lanes << 8 | GP32_LCD_SOLID */
    GP32_LCD_COUNT,       /* visible pixels, the rest of the row is black */
    GP32_LCD_PALETTE_ROW, /* indexed formats: row in palettes */
    GP32_LCD_COLOR,       /* solid rows: the 16 bit color */
    GP32_LCD_HEADER
};
#define GP32_LCD_SOLID (1u << 16)
#define GP32_LCD_LINE_WORDS (GP32_LCD_HEADER + GP32_LCD_WIDTH / 2u)

typedef struct {
    uint32_t lines[GP32_LCD_HEIGHT][GP32_LCD_LINE_WORDS];
    /* One row per palette change within the frame. */
    uint16_t palettes[GP32_LCD_HEIGHT][256];
    unsigned palette_count;
} Gp32LcdFrame;

/* Zero initialize. The completed frame is frames[back ^ 1]. */
typedef struct {
    Gp32LcdFrame frames[2];
    uint32_t palette[256];
    unsigned back;
} Gp32LcdCapture;

typedef struct {
    uint32_t regs[7];
    bool active;
} S3c2400Dma;

typedef struct S3c2400 S3c2400;
struct S3c2400 {
    S3c2400Bus bus;
    Arm920t cpu;
    S3c2400Video video;
    /* Optional, set after boot and owned by the caller. While set, rows are
       captured raw: pixels stays untouched and video.frame is not called. */
    Gp32LcdCapture *lcd_capture;
    /* s3c2400_run accounts quiet clocks lazily: pending ones are not yet
       applied, and budget more may pass before the next peripheral event.
       Both are zero outside that call. */
    unsigned pending_clocks, idle_budget;
    /* Peripheral visits made by s3c2400_run, and instructions it did not
       interpret because a wait loop or a sleeping core only let time pass. */
    uint64_t events, skipped_steps;
    /* In a recompiled build, instructions s3c2400_run left to the interpreter. */
    uint64_t interpreted_steps;
    /* Side-effect-free register reads, and the earliest cycle at which one
       of those made in the current wait loop could return another value. */
    uint32_t quiet_reads;
    uint64_t stable_until;
    S3c2400Audio audio;
    Gp32Smc smc;
    uint8_t smc_data;
    uint8_t eeprom[GP32_EEPROM_SIZE], eeprom_page[32];
    uint64_t eeprom_ready;
    uint32_t eeprom_pending;
    uint16_t eeprom_address;
    uint8_t iic_phase, eeprom_high;
    bool eeprom_dirty, iic_ack, iic_start_pending;
    uint16_t audio_fifo[8];
    unsigned audio_head, audio_count, audio_ticks;
    /* Last L3 volume written to the DAC.  Pinball Dreams writes 0x3f, the
       data sheet's minus infinity, and nothing else, yet is a game with sound:
       how the console's board takes this register is not established, so the
       output ignores it.  The explicit mute bit is applied. */
    unsigned codec_volume;
    uint8_t l3_bits, l3_shift, l3_address, codec_status;
    bool codec_mute;
    bool unused; /* keeps the size recorded in shots */
#ifdef M0_GP32_EXTERNAL_RAM
    /* A small host: GP32_RAM_BYTES allocated by the caller and set before
       boot, and frames only through lcd_capture. */
    uint8_t *ram;
#else
    uint8_t ram[GP32_RAM_SIZE];
    uint32_t scanout[GP32_LCD_WIDTH * GP32_LCD_HEIGHT];
    uint32_t pixels[GP32_LCD_WIDTH * GP32_LCD_HEIGHT];
#endif
    uint64_t steps, cycles, frames;
    /* Approximate CPU instruction cost in FCLK cycles. Defaults to one. */
    unsigned cycles_per_instruction;
    bool (*irq_handler)(S3c2400 *soc, unsigned source);
    uint32_t hle_timer_handler[4];
    uint32_t hle_swi, hle_pc;
    unsigned hle_flip_mode;
    bool hle_exited, hle_unhandled;
    /* The HLE call just made only waits for the display: it will repeat, with
       nothing to change before the next peripheral event. */
    bool hle_waiting;
    unsigned line, line_ticks;
    unsigned rtc_ticks;
    uint16_t buttons;
    uint32_t timer_phase[5];
    S3c2400Dma dma[4];
    uint32_t memcon[13], irq[6], clock[6], lcd[21], palette[256];
    uint32_t uart[2][12], pwm[17], usb[128], watchdog[3];
    uint32_t usbhost[80]; /* OHCI registers, kept as storage: no port is connected */
    uint32_t iic[4], iis[5], gpio[23], rtc[35], adc[2], spi[6], mmc[17];
    /* Added after the first save state format; older states end before it,
       so it stays last. */
    struct {
        uint64_t rtc_second;    /* cycle at which the calendar advances */
        uint32_t watchdog_phase;
        uint32_t uart_full;     /* receivers holding a byte, one bit each */
    } late;
};

/* A calendar time, in plain binary: year 0-99, weekday 1-7. */
typedef struct {
    unsigned year, month, day, weekday, hour, minute, second;
} S3c2400Time;

/* Sets the battery backed clock; boot starts it at a fixed date. */
void s3c2400_set_time(S3c2400 *soc, S3c2400Time time);
/* Decodes the framebuffer as the LCD registers describe it now: GP32_LCD_WIDTH
   by GP32_LCD_HEIGHT panel pixels. The machine does not change. */
void s3c2400_screen(S3c2400 *soc, uint32_t *argb);
/* Completes a state that was saved without the `late` members. */
void s3c2400_upgrade_state(S3c2400 *soc, bool hle);

/* Keep the machine at a stable address. Allocate it outside the stack.
   The caller retains the BIOS and callback contexts. Pass NULL and size 0
   to initialize the hardware with the boot ROM window unmapped. */
bool s3c2400_boot(S3c2400 *soc, const uint8_t *bios, size_t size,
                  Arm920tConfig cpu_hooks, S3c2400Hooks bus_hooks, S3c2400Video video);
void s3c2400_set_buttons(S3c2400 *soc, uint16_t buttons);
Arm920tStep s3c2400_step(S3c2400 *soc);
/* The same step with the peripherals advanced in batches, for callers that
   step one instruction at a time. Call s3c2400_sync before reading or editing
   peripheral state outside the bus. */
Arm920tStep s3c2400_step_batched(S3c2400 *soc);

typedef enum {
    S3C2400_STOP_STEPS, S3C2400_STOP_FRAME, S3C2400_STOP_EXCEPTION, S3C2400_STOP_EXIT
} S3c2400Stop;

/* Same machine behaviour as up to `steps` calls of s3c2400_step, without
   visiting the peripherals between their events. Returns early on a new LCD
   frame, an unhandled exception entry or an HLE exit. */
S3c2400Stop s3c2400_run(S3c2400 *soc, uint64_t steps);
/* The CPU clock the program has set, in Hz: what soc->cycles counts. */
unsigned s3c2400_fclk(const S3c2400 *soc);
/* A callback running inside s3c2400_run calls this before reading cycles or
   peripheral phases, or before changing peripheral state directly. */
void s3c2400_sync(S3c2400 *soc);
/* A word access to the I/O port register at `offset` in the port block, as a
   load or store there would perform it, for callbacks that drive the pins. */
uint32_t s3c2400_pin_read(S3c2400 *soc, unsigned offset);
void s3c2400_pin_write(S3c2400 *soc, unsigned offset, uint32_t value);
/* One of the 13 pin routines of the card driver, arguments and result in the
   CPU registers. */
void s3c2400_card_routine(S3c2400 *soc, unsigned entry);
void s3c2400_set_irq(S3c2400 *soc, unsigned source);
void s3c2400_dma_request(S3c2400 *soc, unsigned channel, unsigned source);

#ifdef M0_GP32_EXTERNAL_RAM
/* One line of the running LCD, for a host without a frame capture: `header`
   receives the GP32_LCD_HEADER words; returns the pixel words, in guest
   memory or copied into `spare` (GP32_LCD_WIDTH / 2 words), or NULL when the
   line has none. */
const uint8_t *s3c2400_lcd_row(const S3c2400Bus *bus, const uint32_t *lcd, unsigned y,
                               uint32_t *header, uint32_t *spare);
/* The guest SDRAM bytes the running LCD fetches: false when it shows none. */
bool s3c2400_lcd_span(const S3c2400 *s, uint32_t *offset, uint32_t *length);
#endif

#endif
