extern "C" {
#include "ws.h"
}
#include "m0_target.h"
#include "m0_audio.h"
#include "m0_card.h"
#include "m0_clock.h"
#include "m0_lcd.h"
#include "m0_menu.h"
#include "m0_pad.h"
#include "m0_scale.h"
#include "meta_hw.h"
#include "meta_usb.h"

#include "ws_memory_config.h"

#ifdef WS_FLASH_ROM
extern const uint16_t ws_flash_map[];
extern const uint8_t ws_flash_bytes[];
extern const uint32_t ws_flash_rom_size, ws_flash_crc;
#endif

static Ws machine;
static meta_extent extents[32];
static int extent_count;
static bool read_failed;
extern "C" {
volatile uint32_t ws_meta_error[4];
}

enum WsMetaError : uint32_t {
    CARD_INIT = 1, ROM_LOOKUP, ROM_MISMATCH, ROM_BOOT,
    SD_READ, SAVE_IO, CPU_OPCODE, CPU_HALTED
};
static struct {
    uint32_t sector;
    alignas(4) uint8_t bytes[512];
} cache[WS_ROM_CACHE_SLOTS];
static uint8_t shades[WS_WIDTH];
static uint16_t row[160];

static uint8_t read_rom(void *, uint32_t offset)
{
    uint32_t sector = offset >> 9;
#ifdef WS_FLASH_ROM
    unsigned resident = ws_flash_map[sector];
    if (resident != UINT16_MAX)
        return ws_flash_bytes[resident * 512u + (offset & 511)];
#endif
    auto &slot = cache[sector % WS_ROM_CACHE_SLOTS];
    if (slot.sector != sector) {
        slot.sector = UINT32_MAX;
        if (!m0_card_read(extents, extent_count, sector, slot.bytes)) {
            read_failed = true;
            ws_meta_error[3] = sector;
            return 0xff;
        }
        slot.sector = sector;
    }
    return slot.bytes[offset & 511];
}

#ifdef WS_FLASH_ROM
static bool match_rom(uint32_t size)
{
    if (size != ws_flash_rom_size)
        return false;
    uint32_t crc = UINT32_MAX;
    for (uint32_t sector = 0; sector < size / 512; ++sector) {
        if (!m0_card_read(extents, extent_count, sector, cache[0].bytes))
            return false;
        for (uint8_t byte : cache[0].bytes) {
            crc ^= byte;
            for (unsigned bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
        }
        m0_watchdog_feed();
    }
    return ~crc == ws_flash_crc;
}
#endif

static meta_extent save_extents[16];
static int save_extent_count;
static uint32_t save_sector = UINT32_MAX;
static bool save_dirty, save_failed, save_has_rtc;
alignas(4) static uint8_t save_block[512];

static bool flush_save_block()
{
    if (!save_dirty)
        return true;
    if (!m0_card_write(save_extents, save_extent_count, save_sector, save_block)) {
        save_failed = true;
        return false;
    }
    save_dirty = false;
    return true;
}

static bool prepare_save_byte(uint32_t offset)
{
    uint32_t sector = offset >> 9;
    if (sector == save_sector)
        return true;
    if (!flush_save_block())
        return false;
    save_sector = UINT32_MAX;
    if (!m0_card_read(save_extents, save_extent_count, sector, save_block)) {
        save_failed = true;
        return false;
    }
    save_sector = sector;
    return true;
}

static uint8_t save_read(uint32_t offset)
{
    return prepare_save_byte(offset) ? save_block[offset & 511] : 0xff;
}

static void save_write(uint32_t offset, uint8_t value)
{
    if (prepare_save_byte(offset) && save_block[offset & 511] != value) {
        save_block[offset & 511] = value;
        save_dirty = true;
    }
}

static uint8_t read_sram(void *, uint32_t offset)
{
    return save_read(sizeof(machine.internal_nv) + offset);
}

static void write_sram(void *, uint32_t offset, uint8_t value)
{
    save_write(sizeof(machine.internal_nv) + offset, value);
}

static void transfer_eeprom(WsEeprom &chip, uint32_t offset, bool write)
{
    for (unsigned i = 0; i < chip.size; ++i) {
        if (write)
            save_write(offset + i, chip.data[i]);
        else
            chip.data[i] = save_read(offset + i);
    }
    if (!save_failed)
        chip.dirty = false;
}

static uint32_t rtc_save_offset()
{
    return sizeof(machine.internal_nv) + machine.cart_eeprom.size + machine.cart_sram_size;
}

static void transfer_rtc(bool write)
{
    uint8_t bytes[WS_RTC_SAVE_SIZE];
    uint32_t offset = rtc_save_offset();
    if (write) {
        ws_rtc_export(&machine.rtc, bytes);
        for (uint8_t byte : bytes)
            save_write(offset++, byte);
    } else {
        for (uint8_t &byte : bytes)
            byte = save_read(offset++);
        ws_rtc_import(&machine.rtc, bytes);
    }
}

static void flush_nvram(bool flush_rtc)
{
    if (!save_extent_count)
        return;
    if (machine.internal_eeprom.dirty)
        transfer_eeprom(machine.internal_eeprom, 0, true);
    if (machine.cart_eeprom.dirty)
        transfer_eeprom(machine.cart_eeprom, sizeof(machine.internal_nv), true);
    if (flush_rtc && save_has_rtc)
        transfer_rtc(true);
    flush_save_block();
}

static void draw_line(void *, const Ws *ws, unsigned line)
{
    constexpr unsigned rows = WS_HEIGHT * 160 / WS_WIDTH;
    unsigned y = m0_row_of(line, WS_HEIGHT, rows);
    if (line + 1 < WS_HEIGHT && y == m0_row_of(line + 1, WS_HEIGHT, rows))
        return;

    ws_render_line(ws, line, shades);
    for (unsigned x = 0; x < 160; ++x) {
        unsigned shade = shades[x * WS_WIDTH / 160];
        row[x] = M0_COLOR(shade * 0x010101u);
    }
    m0_lcd_send_rows(row, y + (128 - rows) / 2, 1);
    m0_lcd_wait();
}

static unsigned read_keys()
{
    unsigned pad = M0_PAD(meta_pad_read());
    unsigned keys = 0;
    if (pad & M0_MB_MENU)  keys |= 0x002;
    if (pad & M0_MB_A)     keys |= 0x004;
    if (pad & M0_MB_B)     keys |= 0x008;
    if (pad & M0_MB_UP)    keys |= 0x010;
    if (pad & M0_MB_RIGHT) keys |= 0x020;
    if (pad & M0_MB_DOWN)  keys |= 0x040;
    if (pad & M0_MB_LEFT)  keys |= 0x080;
    return keys;
}

static int32_t speaker_dc;

static void queue_sound(void *, uint8_t speaker, int16_t, int16_t)
{
    // AC coupling removes the unsigned hardware mix's DC offset.
    int32_t input = (int32_t)speaker << 8;
    speaker_dc += (input - speaker_dc) >> 8;
    int sample = ((input - speaker_dc) >> 9) + 128;
    m0_audio_push((uint8_t)sample);
}

[[noreturn]] static void stop(WsMetaError reason, uint16_t color)
{
    ws_meta_error[0] = reason;
    ws_meta_error[1] = uint32_t(machine.cs) << 16 | machine.ip;
    ws_meta_error[2] = machine.frames;
    m0_lcd_fill(color);
    m0_watchdog_stop();
    for (;;) {}
}

int main()
{
    m0_clock_init();
    meta_usb_init();
    m0_lcd_init();
    m0_lcd_fill(0);
    uint32_t size;
    if (!m0_sd_init())
        stop(CARD_INIT, M0_COLOR(0x0000ff));
    for (auto &slot : cache)
        slot.sector = UINT32_MAX;

    uint32_t dir = M0_FAT_ANY_DIR;
    if (m0_fat_find_in(m0_sd_read, cache[0].bytes, "ROM     WS ", nullptr,
                       nullptr, &dir, extents, 32, &extent_count, &size))
        stop(ROM_LOOKUP, M0_COLOR(0xff0000));
#ifdef WS_FLASH_ROM
    if (!match_rom(size))
        stop(ROM_MISMATCH, M0_COLOR(0xff0000));
#endif
    if (!ws_boot(&machine, {read_rom, nullptr, size}) || read_failed)
        stop(ROM_BOOT, M0_COLOR(0xff0000));

    uint32_t save_size;
    int save_result = m0_fat_find_in(m0_sd_read, save_block, "ROM     SAV",
                                    nullptr, nullptr, &dir, save_extents, 16,
                                    &save_extent_count, &save_size);
    uint32_t base_save_size = rtc_save_offset();
    if (save_result == M0_FAT_OK &&
        (save_size == base_save_size ||
         (machine.rtc.present && save_size == base_save_size + WS_RTC_SAVE_SIZE))) {
        save_has_rtc = save_size != base_save_size;
        transfer_eeprom(machine.internal_eeprom, 0, false);
        transfer_eeprom(machine.cart_eeprom, sizeof(machine.internal_nv), false);
        if (machine.cart_sram_size)
            ws_attach_sram_bus(&machine, machine.cart_sram_size, nullptr, read_sram, write_sram);
        if (save_has_rtc)
            transfer_rtc(false);
    } else if (save_result == M0_FAT_NOT_FOUND) {
        save_extent_count = 0;
    } else {
        stop(SAVE_IO, M0_COLOR(0xffff00));
    }
    if (save_failed)
        stop(SAVE_IO, M0_COLOR(0xffff00));

    static_assert(META_AUDIO_PUSH_RATE == WS_AUDIO_RATE, "WonderSwan DAC sample rate");
    m0_audio_init();
    machine.audio.sink = queue_sound;

    unsigned frame = 0;
    for (;;) {
        WsStep result = ws_step_with_video(&machine, draw_line, nullptr);
        if (result != WS_STEP_OK || read_failed) {
            flush_nvram(true);
            m0_audio_stop();
            if (read_failed)
                stop(SD_READ, M0_COLOR(0x00ffff));
            if (result == WS_STEP_HALTED)
                stop(CPU_HALTED, M0_COLOR(0xff8000));
            ws_meta_error[3] = ws_read(&machine, ws_address(machine.cs, machine.ip));
            if (read_failed)
                stop(SD_READ, M0_COLOR(0x00ffff));
            stop(CPU_OPCODE, M0_COLOR(0xff00ff));
        }
        if (save_failed) {
            m0_audio_stop();
            stop(SAVE_IO, M0_COLOR(0xffff00));
        }
        if (machine.frames != frame) {
            frame = machine.frames;
            m0_lcd_end_frame();
            ws_set_keys(&machine, read_keys());
            if (frame % 30 == 0)
                flush_nvram(frame % 300 == 0);
            m0_watchdog_feed();
        }
    }
}
