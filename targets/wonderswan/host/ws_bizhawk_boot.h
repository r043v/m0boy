// Boot state matching BizHawk's WonderSwan core, so a BizHawk-recorded TAS
// replays in lockstep. Apply right after ws_boot_model().
#ifndef WS_BIZHAWK_BOOT_H
#define WS_BIZHAWK_BOOT_H

#include "ws.h"
#include <string.h>
#include "ieeprom.h"
#include "startio.h"

// The movie's sync settings: RTC at 2010-01-01 00:00:00. BizHawk's Cygne RTC
// reports January as BCD 0 (tm_mon is 0-based).
static const uint8_t ws_bizhawk_date[7] = {10, 1, 1, 5, 0, 0, 0};

static void ws_boot_bizhawk(Ws *ws)
{
    memcpy(ws->internal_nv, ieeprom_init, 0x400);
    // BizHawk formats a fresh cartridge EEPROM to zeroes, not the 0xff blank state.
    memset(ws->cart_nv, 0, sizeof(ws->cart_nv));
    memcpy(ws->rtc.date, ws_bizhawk_date, sizeof(ws_bizhawk_date));
    ws->rtc.date[1] = 0;
    // BizHawk's core seeds every IO register from Cygne's startio table on reset.
    for (unsigned port = 0; port < 0xc9; ++port) {
        if (port == 0xba || port == 0xbb || port == 0xc4 || port == 0xc5)
            continue;
        ws_out(ws, port, startio[port]);
    }
}

#endif
