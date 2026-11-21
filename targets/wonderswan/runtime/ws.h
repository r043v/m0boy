#ifndef M0BOY_WS_H
#define M0BOY_WS_H

#include <stdbool.h>
#include <stdint.h>

#ifndef WS_ENABLE_COLOR
#define WS_ENABLE_COLOR 1
#endif
#ifndef WS_ENABLE_BIZHAWK_TIMING
#define WS_ENABLE_BIZHAWK_TIMING 0
#endif

enum { WS_MONO_RAM_SIZE = 16 * 1024,
       WS_RAM_SIZE = WS_ENABLE_COLOR ? 64 * 1024 : WS_MONO_RAM_SIZE,
       WS_MONO_BIOS_SIZE = 4096, WS_COLOR_BIOS_SIZE = 8192,
       WS_MONO_EEPROM_SIZE = 128, WS_COLOR_EEPROM_SIZE = 2048,
       WS_MAX_ROM_SIZE = 64 * 1024 * 1024,
       WS_WIDTH = 224, WS_HEIGHT = 144 };

typedef struct {
    // The cartridge can be read from host memory or from a device cache.
    uint8_t (*read)(void *context, uint32_t offset);
    void *context;
    uint32_t size;
} WsRom;

typedef struct {
    uint8_t *data;
    uint16_t size, command, read_data, write_data;
    bool read_done, write_enabled, write_protected, dirty;
} WsEeprom;

typedef void (*WsSerialSink)(void *context, uint8_t value);

enum { WS_AUDIO_RATE = 24000 };
typedef void (*WsAudioSink)(void *context, uint8_t speaker, int16_t left, int16_t right);
typedef void (*WsInstructionSink)(void *context, uint32_t pc, uint32_t rom_offset);
typedef void (*WsMemoryWriteSink)(void *context, uint32_t address, uint8_t value);
typedef void (*WsMemoryReadSink)(void *context, uint32_t address, uint8_t value);
typedef void (*WsIoSink)(void *context, uint16_t port, uint8_t value, int write);

typedef struct {
    WsAudioSink sink;
    void *context;
    uint32_t sweep_clock;
    uint16_t phase[4], noise;
    uint8_t index[4], io[0x1f], sample_clock;
#if WS_ENABLE_COLOR
    uint32_t dma_source, dma_length, dma_source_reload, dma_length_reload;
    uint16_t dma_clocks, pcm_output[2];
    uint8_t dma_control, pcm_control, pcm_mode, pcm_clock, pcm_input[2], pcm_channel;
#endif
} WsAudio;

typedef struct {
    uint32_t second_clocks;
    uint8_t date[7], alarm[2], payload[7];
    uint8_t status, command, data, index, length, delay;
    bool present, busy, ready;
} WsRtc;

enum { WS_RTC_SAVE_SIZE = 12 };
void ws_rtc_export(const WsRtc *rtc, uint8_t bytes[WS_RTC_SAVE_SIZE]);
bool ws_rtc_import(WsRtc *rtc, const uint8_t bytes[WS_RTC_SAVE_SIZE]);

enum { WS_FLASH_SIZE = 512 * 1024 };
typedef void (*WsFlashStore)(void *context, uint32_t offset, uint32_t count, uint8_t value);
typedef struct {
    WsFlashStore store;
    void *context;
    uint8_t phase;
    bool identify, fast, exit_fast;
} WsFlash;

typedef struct {
    WsRom rom;
    WsInstructionSink instruction_sink;
    void *instruction_context;
    WsMemoryWriteSink memory_write_sink;
    void *memory_write_context;
    WsMemoryReadSink memory_read_sink;
    void *memory_read_context;
    WsIoSink io_sink;
    void *io_context;
    WsFlash flash;
    const uint8_t *boot_rom;
    uint8_t system_control;
#if WS_ENABLE_COLOR
    uint8_t video_mode;
    uint8_t dma_control;
    uint32_t dma_source;
    uint16_t dma_target, dma_length;
#endif
    WsRtc rtc;
    WsAudio audio;
    WsSerialSink uart_send;
    void *uart_context;
    uint16_t uart_tx_clocks;
    uint8_t uart_control, uart_status, uart_tx, uart_rx;
    WsEeprom internal_eeprom, cart_eeprom;
    uint8_t internal_nv[WS_ENABLE_COLOR ? WS_COLOR_EEPROM_SIZE : WS_MONO_EEPROM_SIZE];
    uint8_t cart_nv[2048];
    uint32_t cart_sram_size;
    uint8_t *sram;
    void *sram_context;
    uint8_t (*sram_read)(void *context, uint32_t offset);
    void (*sram_write)(void *context, uint32_t offset, uint8_t value);
    uint32_t sram_size;
    uint16_t sram_bank;
    uint8_t ram[WS_RAM_SIZE];
    uint8_t display[0x40];
    uint8_t sprites[512];
    uint8_t line, line_clock;
    uint8_t line_shown;
    bool line_override;
#if WS_ENABLE_BIZHAWK_TIMING
    bool bizhawk_timing;
#endif
    uint32_t frames;
    uint16_t keys;
    uint8_t key_select;
    bool mapper_2003, rom_in_sram_window;
    uint8_t gpo_direction, gpo_data, rom_linear;
    uint16_t rom_bank[2], rom_bank_mask;
    uint32_t rom_window[16];
    uint16_t reg[8]; // AX, CX, DX, BX, SP, BP, SI, DI
    uint16_t cs, ip, ds, es, ss, flags;
    uint16_t timer_reload[2], timer_count[2];
    uint8_t timer_control;
    uint8_t irq_base, irq_enable, irq_pending;
    bool halted, irq_shadow, stack_shadow;
    bool nmi_enabled, nmi_pending, low_battery;
} Ws;

typedef enum { WS_STEP_OK, WS_STEP_HALTED, WS_STEP_UNSUPPORTED } WsStep;

uint8_t ws_eeprom_read(const WsEeprom *chip, unsigned reg, bool internal);
void ws_eeprom_write(WsEeprom *chip, unsigned reg, uint8_t value, bool internal);
bool ws_boot(Ws *ws, WsRom rom);
// Optional 4096-byte ASWAN boot ROM, retained by the caller.
bool ws_boot_with_bios(Ws *ws, WsRom rom, const uint8_t bios[WS_MONO_BIOS_SIZE]);
// The optional boot ROM must be the size specified for the selected model.
bool ws_boot_model(Ws *ws, WsRom rom, bool color, const uint8_t *bios);
// The ROM reader and store must use the same backing image.
bool ws_attach_flash(Ws *ws, WsFlashStore store, void *context);
uint8_t ws_flash_read(const Ws *ws, uint32_t offset, bool word_bus);
void ws_flash_write(Ws *ws, uint32_t offset, uint8_t value);
bool ws_attach_sram(Ws *ws, uint8_t *data, uint32_t size);
bool ws_attach_sram_bus(Ws *ws, uint32_t size, void *context,
                        uint8_t (*read)(void *, uint32_t),
                        void (*write)(void *, uint32_t, uint8_t));
uint32_t ws_address(uint16_t segment, uint16_t offset);
// Returns UINT32_MAX for addresses without a backing cartridge ROM byte.
uint32_t ws_rom_offset(const Ws *ws, uint32_t address);
uint8_t ws_read(const Ws *ws, uint32_t address);
// The memory view without firing memory_read_sink (opcode fetches, probes).
uint8_t ws_peek(const Ws *ws, uint32_t address);
void ws_write(Ws *ws, uint32_t address, uint8_t value);
void ws_set_low_battery(Ws *ws, bool low);
void ws_set_keys(Ws *ws, uint16_t keys);
bool ws_can_wake(const Ws *ws);
uint8_t ws_in(Ws *ws, uint16_t port);
void ws_out(Ws *ws, uint16_t port, uint8_t value);
// Receive a completed byte from a peer or frontend.
void ws_uart_receive(Ws *ws, uint8_t value);
void ws_uart_connect(Ws *ws, WsSerialSink send, void *context);
uint8_t ws_uart_read(Ws *ws, bool status);
void ws_uart_write(Ws *ws, bool control, uint8_t value);
void ws_uart_clock(Ws *ws, unsigned clocks);
void ws_uart_refresh_irq(Ws *ws);
uint8_t ws_rtc_read(Ws *ws, bool status);
void ws_rtc_write(Ws *ws, bool command, uint8_t value);
void ws_rtc_clock(Ws *ws, unsigned clocks);
void ws_rtc_refresh_irq(Ws *ws);
void ws_audio_clock(Ws *ws, unsigned clocks);
uint8_t ws_audio_read(const Ws *ws, unsigned reg);
void ws_audio_write(Ws *ws, unsigned reg, uint8_t value);
typedef void (*WsLineSink)(void *context, const Ws *ws, unsigned line);
#if WS_ENABLE_COLOR
uint8_t ws_color_audio_read(const Ws *ws, unsigned port);
void ws_color_audio_write(Ws *ws, unsigned port, uint8_t value);
void ws_dma_run(Ws *ws, WsLineSink sink, void *context);
#endif
WsStep ws_step(Ws *ws);
WsStep ws_step_with_video(Ws *ws, WsLineSink sink, void *context);
void ws_video_clock(Ws *ws, unsigned clocks, WsLineSink sink, void *context);
void ws_render_line(const Ws *ws, unsigned line, uint8_t pixels[WS_WIDTH]);
#if WS_ENABLE_COLOR
void ws_render_line_rgb(const Ws *ws, unsigned line, uint16_t pixels[WS_WIDTH]);
#endif

#endif
