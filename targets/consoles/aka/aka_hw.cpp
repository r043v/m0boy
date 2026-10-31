/* AKA CORE v1.4 adapter. Wiring and register values documented by
 * jmp42/Gamebuino_AKA_lib (gb_ll_common/lcd/audio); see docs/AKA.fr.md.
 * The LCD is i80, the buttons are I2C expanders, the audio codec is TAS2505. */
#include <Arduino.h>
#include <Wire.h>
#include <string.h>
#include <driver/i2s.h>
#include <esp_lcd_panel_io.h>
#include <freertos/semphr.h>
#include "esp32_hw.h"
#include "aka_hw.h"
#include "esp32_config.h"
#include "m0_target.h"

/* MADCTL bit 3 (ST7789: 1 = the panel reads BGR words).  The official library
 * sets it because its lcd_color_rgb() puts red in the low bits; m0boy sends
 * RGB565 (red high), so it stays 0 — confirmed on the board (Alex Kidd came out
 * BGR with the bit set).  AKA_LCD_BGR=1 for a frontend that sends BGR words. */
#ifndef AKA_LCD_BGR
#define AKA_LCD_BGR 0
#endif
#include "m0_scale.h"
extern "C" {
#include "m0_lcd.h"
#include "m0_menu.h"
}

/* The rows the frontend sends span this many screen rows (the Sega
 * frontends' 120 visible rows zoom x2 onto the panel; the default is the
 * whole M0_SCREEN_ROWS picture). */
#ifndef M0_PANEL_SRC_ROWS
#define M0_PANEL_SRC_ROWS M0_SCREEN_ROWS
#endif

static uint8_t outputs = 0xf7; /* inputs high; 3V3, LCD reset/RD high; amp reset low */
static void expander(uint8_t value) {
    outputs = value | 0xc6; /* RUN, MENU, L1, R1 must stay inputs */
    Wire.beginTransmission(0x38);
    Wire.write(outputs);
    ESP_ERROR_CHECK(Wire.endTransmission() == 0 ? ESP_OK : ESP_FAIL);
}
static void codec(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(0x18);
    Wire.write(reg);
    Wire.write(value);
    ESP_ERROR_CHECK(Wire.endTransmission() == 0 ? ESP_OK : ESP_FAIL);
}
extern "C" uint8_t esp32_buttons(void) {
    uint16_t raw = 0;
    for (unsigned i = 0; i < 2; i++) {
        if (Wire.requestFrom(uint8_t(i ? 0x3f : 0x38), uint8_t(1)) != 1) return 0;
        raw |= uint16_t(Wire.read()) << (8 * i);
    }
    uint16_t keys = (raw ^ 0xffc4) & 0xffc6; /* RUN active high, others active low */
    uint8_t out = 0;
    if (keys & 0x0400) out |= M0_MB_DOWN;
    if (keys & 0x0800) out |= M0_MB_LEFT;
    if (keys & 0x0100) out |= M0_MB_RIGHT;
    if (keys & 0x0200) out |= M0_MB_UP;
    if (keys & 0x8000) out |= M0_MB_A;
    if (keys & 0x2000) out |= M0_MB_B;
    if (keys & (0x0002 | 0x1000)) out |= M0_MB_MENU; /* RUN or D: Start */
    if (keys & (0x0004 | 0x4000)) out |= M0_MB_HOME; /* MENU or C: options */
    /* CORE v1.4 joystick: ADC1 channels 5/4, i.e. GPIO6/5.
     * Positive X is right and positive Y is up in the official library. */
    unsigned x = analogReadMilliVolts(6), y = analogReadMilliVolts(5);
    if (x < 825) out |= M0_MB_LEFT;
    if (x > 2475) out |= M0_MB_RIGHT;
    if (y < 825) out |= M0_MB_DOWN;
    if (y > 2475) out |= M0_MB_UP;
    return out;
}
extern "C" void esp32_restart(void) { ESP.restart(); for (;;) {} }

/* LCD: wait for each DMA before reusing the line buffer.  The pixels are
 * native RGB565; the i80 peripheral swaps the bytes of each one on the wire
 * (swap_color_bytes, as the official library does) — no software swap. */
static esp_lcd_panel_io_handle_t panel;
/* What the panel is set to: the init below sends these very values, and gm0
 * reads them to model the glass (MADCTL bit 3 = BGR, i80 byte swap). */
extern "C" const struct m0_aka_lcd_setup {
    uint8_t madctl, colmod, swap_color_bytes, inversion;
} m0_aka_lcd __attribute__((used)) = {uint8_t(0x60u | (AKA_LCD_BGR ? 0x08u : 0u)), 0x55, 1, 1};
static SemaphoreHandle_t color_done;
/* free slots of the async wide path: the producer takes one per queued
 * batch, the transfer-done ISR gives it back.  The count matches the
 * frontends' ring of batches exactly: taking a slot means the oldest
 * transfer in flight has completed, so the batch buffer the caller is
 * about to refill is free again (transfers complete in queue order). */
static SemaphoreHandle_t wide_slots;
#define WIDE_QUEUE 4
static uint32_t lcd_seq;
static uint16_t line[320] __attribute__((aligned(64)));
static bool color_sent(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *) {
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(color_done, &wake);
    if (wide_slots) xSemaphoreGiveFromISR(wide_slots, &wake);
    return wake == pdTRUE;
}
static void command(int cmd, const void *data = nullptr, size_t n = 0) {
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(panel, cmd, data, n));
}
static void draw_line(unsigned y) {
    const uint8_t x[] = {0, 0, 1, 63};
    const uint8_t row[] = {uint8_t(y >> 8), uint8_t(y), uint8_t(y >> 8), uint8_t(y)};
    command(0x2a, x, sizeof x);
    command(0x2b, row, sizeof row);
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(panel, 0x2c, line, sizeof line));
    xSemaphoreTake(color_done, portMAX_DELAY);
}
extern "C" void m0_lcd_init(void) {
    Wire.begin(AKA_I2C_SDA, AKA_I2C_SCL, 400000);
    Wire.setTimeOut(20);
    expander(0xc7); /* power on; reset LCD and amplifier */
    delay(100);
    expander(0xf7);
    delay(100);
    pinMode(AKA_LCD_TE, INPUT);
    analogSetPinAttenuation(5, ADC_11db);
    analogSetPinAttenuation(6, ADC_11db);
    ledcSetup(0, 50000, 8);
    ledcAttachPin(AKA_LCD_BL, 0);
    ledcWrite(0, 64);
    color_done = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(color_done ? ESP_OK : ESP_ERR_NO_MEM);
    wide_slots = xSemaphoreCreateCounting(WIDE_QUEUE, WIDE_QUEUE);
    ESP_ERROR_CHECK(wide_slots ? ESP_OK : ESP_ERR_NO_MEM);
    esp_lcd_i80_bus_handle_t bus;
    esp_lcd_i80_bus_config_t config = {};
    config.dc_gpio_num = AKA_LCD_DC;
    config.wr_gpio_num = AKA_LCD_WR;
    config.clk_src = LCD_CLK_SRC_PLL160M;
    const int data[] = AKA_LCD_DATA;
    for (unsigned i = 0; i < 8; i++) config.data_gpio_nums[i] = data[i];
    config.bus_width = 8;
    /* the largest transfer is a batch of eight 320-pixel rows (md_esp32.cpp,
     * vx): esp_lcd asserts when one transfer outgrows this */
    config.max_transfer_bytes = 8 * sizeof line;
    config.psram_trans_align = 64;
    config.sram_trans_align = 64;
    ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&config, &bus));
    esp_lcd_panel_io_i80_config_t io = {};
    io.cs_gpio_num = -1; /* CS tied low on CORE v1.4 */
    /* the panel's write cycle allows more than the official library's
     * 20 MHz; 40 MHz halves the bus time of a full 240-row frame
     * (7.8 -> 3.9 ms).  Back to 20000000 if a board shows artifacts. */
#ifndef AKA_LCD_PCLK
#define AKA_LCD_PCLK 40000000
#endif
    io.pclk_hz = AKA_LCD_PCLK;
    io.trans_queue_depth = WIDE_QUEUE + 2;
    io.on_color_trans_done = color_sent;
    io.lcd_cmd_bits = io.lcd_param_bits = 8;
    io.flags.swap_color_bytes = m0_aka_lcd.swap_color_bytes;
    io.dc_levels.dc_idle_level = io.dc_levels.dc_data_level = io.dc_levels.dc_dummy_level = 1;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(bus, &io, &panel));
    command(0x01);
    delay(150);
    command(0x11);
    delay(120);
    /* The official library's sequence: COLMOD 16 bits, INVON, NORON, rotation 3
     * (MX | MV) with MADCTL's bit 3 clear: the pixels are RGB. */
    const uint8_t te = 0;
    command(0x3a, &m0_aka_lcd.colmod, 1);
    command(0x21);
    command(0x13);
    command(0x36, &m0_aka_lcd.madctl, 1);
    command(0x29);
    command(0x35, &te, 1);
}
extern "C" uint32_t m0_lcd_send_rows(const void *buf, uint8_t row0, uint8_t rows) {
    const uint16_t *p = (const uint16_t *)buf;
    for (unsigned y = 0; y < rows && row0 + y < M0_SCREEN_ROWS; y++, p += 160) {
        /* m0_nearest(x, 160, 320) is (x + 1) / 2 */
        for (unsigned x = 0; x < 320; x++) {
            const unsigned i = (x + 1) >> 1;
            line[x] = p[i < 160 ? i : 159];
        }
        unsigned py = (row0 + y) * 240 / M0_PANEL_SRC_ROWS;
        unsigned end = (row0 + y + 1) * 240 / M0_PANEL_SRC_ROWS;
        while (py < end) draw_line(py++);
    }
    return ++lcd_seq;
}
extern "C" uint32_t m0_lcd_done_seq(void) { return lcd_seq; }
#if __has_include("md_config.h") || __has_include("vx_config.h") || __has_include("pce_config.h")
/* The Mega Drive's and the Vectrex's 320-column rows go out 1:1 (no
 * 160-column rescale); the rows already sit in the panel's RGB565 wire
 * order.  The async path sets the address window once per frame, then
 * streams whole batches of rows: the producer renders into a small ring
 * of them and waits on a slot only when every queued transfer is still
 * in flight. */
extern "C" uint32_t m0_lcd_send_wide(const void *buf, uint8_t row0, uint8_t rows) {
    const uint16_t *p = (const uint16_t *)buf;
    for (unsigned y = 0; y < rows; y++) {
        memcpy(line, p + 320u * y, sizeof line);
        draw_line(row0 + y);
    }
    return ++lcd_seq;
}
static bool wide_continue; /* the window's first batch is RAMWR, the next ones continue it */
extern "C" void m0_lcd_wide_begin(uint8_t row0, uint8_t rows) {
    wide_continue = false;
    const uint8_t x[] = {0, 0, 1, 63};
    const uint8_t r[] = {uint8_t(row0 >> 8), uint8_t(row0),
                         uint8_t((row0 + rows - 1) >> 8), uint8_t(row0 + rows - 1)};
    command(0x2a, x, sizeof x);
    command(0x2b, r, sizeof r);
}
/* Reserve the batch buffer before filling it (the transfer that still
 * reads it is then known to be done), submit once it is full. */
extern "C" void m0_lcd_wide_reserve(void) {
    xSemaphoreTake(wide_slots, portMAX_DELAY);
}
extern "C" void m0_lcd_wide_submit(const void *batch, uint8_t rows) {
    /* every RAMWR (0x2C) rewinds the write pointer to the window's corner: only
     * the first batch of a window may use it, the others are 0x3C (continue) */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(panel, wide_continue ? 0x3c : 0x2c, batch, 320u * 2u * rows));
    wide_continue = true;
}
extern "C" void m0_lcd_wide_flush(void) {
    while (uxSemaphoreGetCount(wide_slots) < WIDE_QUEUE) vTaskDelay(1);
}
#endif
extern "C" int m0_lcd_busy(void) { return 0; }
extern "C" int m0_lcd_release(void) { return 1; }
extern "C" void m0_lcd_wait(void) {}
extern "C" void m0_lcd_end_frame(void) {}
extern "C" void m0_lcd_fill(uint16_t color) {
    for (auto &pixel : line) pixel = color;
    for (unsigned y = 0; y < 240; y++) draw_line(y);
}

extern "C" void esp32_audio_pull(uint8_t *, unsigned);
static TaskHandle_t audio_task_handle;
static volatile bool audio_enabled;
static void audio_task(void *) {
    uint8_t source[128];
    int16_t wire[512];
    for (;;) {
        esp32_audio_pull(source, sizeof source);
        /* Shared runtime produces 11025 Hz. Keep the codec at 44100 Hz. */
        for (unsigned i = 0; i < sizeof source; i++)
            for (unsigned j = 0; j < 4; j++)
                wire[4*i+j] = audio_enabled ? int16_t((int(source[i]) - 128) * 256) : 0;
        size_t written;
        ESP_ERROR_CHECK(i2s_write(I2S_NUM_0, wire, sizeof wire, &written, portMAX_DELAY));
    }
}
extern "C" void esp32_audio_start(unsigned hz) {
    ESP_ERROR_CHECK(hz == 11025 ? ESP_OK : ESP_ERR_INVALID_ARG);
    if (!audio_task_handle) {
        expander(outputs & ~8);
        delay(100);
        expander(outputs | 8);
        delay(100);
        i2s_config_t config = {};
        config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
        config.sample_rate = 44100;
        config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
        config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
        config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
        config.dma_buf_count = 4;
        config.dma_buf_len = 256;
        ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_0, &config, 0, nullptr));
        i2s_pin_config_t pins = {};
        pins.bck_io_num = AKA_I2S_BCLK;
        pins.ws_io_num = AKA_I2S_WS;
        pins.data_out_num = AKA_I2S_DOUT;
        pins.data_in_num = pins.mck_io_num = I2S_PIN_NO_CHANGE;
        ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM_0, &pins));
        /* TAS2505: reference clock tree, mono DAC, headphone and speaker path. */
        const uint8_t regs[][2] = {
            {0,0},{1,1},{0,1},{2,0},{0,0},{4,7},{5,0x93},{6,20},{7,0},{8,0}
        };
        for (const auto &r : regs) codec(r[0], r[1]);
        delay(15);
        const uint8_t path[][2] = {
            {11,0x84},{12,0x82},{13,0},{14,0x80},{27,0},{28,0},{60,2},
            {63,0x90},{65,0},{64,4},{0,1},{1,0x10},{10,0},{12,4},
            {22,0},{24,0},{25,0},{9,0x20},{16,0},{46,0},{48,0x20},{45,2}
        };
        for (const auto &r : path) codec(r[0], r[1]);
        ESP_ERROR_CHECK(xTaskCreate(audio_task, "m0audio", 3072, nullptr, 5,
                                   &audio_task_handle) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    }
    audio_enabled = true;
}
extern "C" void esp32_audio_stop(void) { audio_enabled = false; }
extern "C" void esp32_audio_sample(uint8_t) {}
size_t getArduinoLoopTaskStackSize(void) { return 32768; }

void setup(void) {
    setCpuFrequencyMhz(240);
    m0boy_console_main();
}
void loop(void) {}
