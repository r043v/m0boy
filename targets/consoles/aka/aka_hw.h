/* Gamebuino AKA CORE v1.4: official wiring, i80 LCD, SDMMC and TAS2505. */
#ifndef AKA_HW_H
#define AKA_HW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

unsigned long millis(void); /* the Arduino core's own signature */
void delay(uint32_t ms);
void m0_watchdog_feed(void);

/* Pressed buttons in m0_menu.h's M0_MB_* order: D-pad, A, B, Home, Menu. */
uint8_t esp32_buttons(void);
/* The shoulder keys at the time of that read. */
enum { M0_SHOULDER_L = 1, M0_SHOULDER_R = 2 };
uint8_t esp32_shoulders(void);
void esp32_restart(void);

/* The 320-column row path, 1:1 on the glass: compiled when the project's
 * config asks for it (the Mega Drive's md_config.h, the Vectrex's
 * vx_config.h); the other frontends' 160-column rows take
 * m0_lcd_send_rows. */
uint32_t m0_lcd_send_wide(const void *buf, uint8_t row0, uint8_t rows);

#ifdef __cplusplus
}
#endif

#endif
