#include "ws.h"
#include <string.h>

enum { RTC_SECOND = 3072000, RTC_BYTE = 64, RTC_IRQ = 4 };

static unsigned bcd_decode(unsigned value)
{
    return (value >> 4) * 10 + (value & 15);
}

static uint8_t bcd_encode(unsigned value)
{
    return (value / 10) * 16 + value % 10;
}

static uint8_t hour_encode(unsigned hour, unsigned status)
{
    return status & 0x40 ? bcd_encode(hour)
                        : bcd_encode(hour % 12) | (hour >= 12 ? 0x80 : 0);
}

static unsigned hour_decode(uint8_t hour, unsigned status)
{
    unsigned value = bcd_decode(hour & 0x3f);
    return status & 0x40 ? value % 24 : value % 12 + ((hour & 0x80) ? 12 : 0);
}

static unsigned month_days(unsigned year, unsigned month)
{
    static const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return days[month - 1] + (month == 2 && year % 4 == 0);
}

void ws_rtc_export(const WsRtc *rtc, uint8_t bytes[WS_RTC_SAVE_SIZE])
{
    bytes[0] = 'W';
    bytes[1] = 1;
    memcpy(bytes + 2, rtc->date, sizeof(rtc->date));
    bytes[9] = rtc->status;
    memcpy(bytes + 10, rtc->alarm, sizeof(rtc->alarm));
}

bool ws_rtc_import(WsRtc *rtc, const uint8_t bytes[WS_RTC_SAVE_SIZE])
{
    const uint8_t *date = bytes + 2;
    if (!rtc->present || bytes[0] != 'W' || bytes[1] != 1 ||
        date[0] > 99 || date[1] < 1 || date[1] > 12 ||
        date[2] < 1 || date[2] > month_days(date[0], date[1]) ||
        date[3] > 6 || date[4] > 23 || date[5] > 59 || date[6] > 59)
        return false;
    memcpy(rtc->date, date, sizeof(rtc->date));
    rtc->status = bytes[9] & 0xea;
    memcpy(rtc->alarm, bytes + 10, sizeof(rtc->alarm));
    rtc->second_clocks = 0;
    return true;
}

static void reset_clock(WsRtc *rtc)
{
    memset(rtc->date, 0, sizeof(rtc->date));
    rtc->date[1] = rtc->date[2] = 1;
    rtc->status = 0;
    rtc->alarm[0] = rtc->alarm[1] = 0;
    rtc->second_clocks = 0;
}

static void next_second(WsRtc *rtc)
{
    uint8_t *date = rtc->date; // binary year, month, day, weekday, hour, minute, second
    if (++date[6] < 60)
        return;
    date[6] = 0;
    if (++date[5] < 60)
        return;
    date[5] = 0;
    if (++date[4] < 24)
        return;
    date[4] = 0;
    if (++date[3] == 7)
        date[3] = 0;
    if (++date[2] <= month_days(date[0], date[1]))
        return;
    date[2] = 1;
    if (++date[1] <= 12)
        return;
    date[1] = 1;
    if (++date[0] == 100)
        date[0] = 0;
}

void ws_rtc_refresh_irq(Ws *ws)
{
    const WsRtc *rtc = &ws->rtc;
    if (!rtc->present || !(ws->irq_enable & RTC_IRQ))
        return;
    bool level;
    if (rtc->status & 8) {
        level = rtc->date[6] < ((rtc->status & 2) ? 30 : 1);
    } else if (rtc->status & 2) {
        // The selected divider taps are ORed onto the active interrupt line.
        uint32_t mask = rtc->alarm[0] | ((unsigned)rtc->alarm[1] << 8);
        uint32_t divider = (rtc->second_clocks * 128u) / 12000u;
        level = (~divider & mask) != 0;
    } else {
        level = (rtc->status & 0x20) &&
                rtc->date[4] == hour_decode(rtc->alarm[0], rtc->status) &&
                rtc->date[5] == bcd_decode(rtc->alarm[1] & 0x7f);
    }
    if (level)
        ws->irq_pending |= RTC_IRQ;
}

void ws_rtc_clock(Ws *ws, unsigned clocks)
{
    WsRtc *rtc = &ws->rtc;
    if (rtc->delay)
        rtc->delay = clocks >= rtc->delay ? 0 : rtc->delay - clocks;
    if (!rtc->present)
        return;
    rtc->second_clocks += clocks;
    if (rtc->second_clocks >= RTC_SECOND) {
        rtc->second_clocks -= RTC_SECOND;
        next_second(rtc);
        ws_rtc_refresh_irq(ws);
    } else if ((rtc->status & 0x0a) == 2) {
        ws_rtc_refresh_irq(ws);
    }
}

static void date_payload(WsRtc *rtc, unsigned first)
{
    for (unsigned i = first; i < 7; ++i)
        rtc->payload[i - first] = i == 4 ? hour_encode(rtc->date[i], rtc->status)
                                        : bcd_encode(rtc->date[i]);
}

static void apply_payload(WsRtc *rtc)
{
    switch (rtc->command) {
    case 2:
        rtc->status = (rtc->status & 0x80) | (rtc->payload[0] & 0x6a);
        break;
    case 4:
        rtc->date[0] = bcd_decode(rtc->payload[0]) % 100;
        rtc->date[1] = bcd_decode(rtc->payload[1]);
        if (!rtc->date[1] || rtc->date[1] > 12)
            rtc->date[1] = 1;
        rtc->date[2] = bcd_decode(rtc->payload[2]);
        if (!rtc->date[2] || rtc->date[2] > month_days(rtc->date[0], rtc->date[1]))
            rtc->date[2] = 1;
        rtc->date[3] = rtc->payload[3] % 7;
        /* fall through */
    case 6: {
        unsigned offset = rtc->command == 4 ? 4 : 0;
        rtc->date[4] = hour_decode(rtc->payload[offset], rtc->status);
        rtc->date[5] = bcd_decode(rtc->payload[offset + 1]) % 60;
        rtc->date[6] = bcd_decode(rtc->payload[offset + 2] & 0x7f) % 60;
        rtc->second_clocks = 0;
        break;
    }
    case 8:
        memcpy(rtc->alarm, rtc->payload, sizeof(rtc->alarm));
        break;
    }
}

static void write_payload(Ws *ws, uint8_t value)
{
    WsRtc *rtc = &ws->rtc;
    rtc->payload[rtc->index++] = value;
    if (rtc->index == rtc->length) {
        rtc->busy = false;
        apply_payload(rtc);
        ws_rtc_refresh_irq(ws);
    }
}

uint8_t ws_rtc_read(Ws *ws, bool status)
{
    WsRtc *rtc = &ws->rtc;
    if (status)
        return rtc->command | (rtc->busy || rtc->delay ? 0x10 : 0) |
               (!rtc->delay && rtc->ready ? 0x80 : 0);
    if (rtc->busy && rtc->length && !rtc->delay && (rtc->command & 1)) {
        rtc->data = rtc->payload[rtc->index++];
        rtc->busy = rtc->index != rtc->length;
        rtc->delay = RTC_BYTE;
    } else if (!rtc->busy && !rtc->delay) {
        rtc->ready = false;
    }
    return rtc->present ? rtc->data : 0xff;
}

void ws_rtc_write(Ws *ws, bool command, uint8_t value)
{
    static const uint8_t lengths[] = {0, 1, 7, 3, 2, 2};
    WsRtc *rtc = &ws->rtc;
    if (!command) {
        rtc->data = value;
        if (rtc->busy && rtc->length && !rtc->delay && !(rtc->command & 1)) {
            if (rtc->present)
                write_payload(ws, value);
            else
                rtc->busy = ++rtc->index != rtc->length;
            rtc->delay = RTC_BYTE;
        } else if (!rtc->busy && !rtc->delay) {
            rtc->ready = false;
        }
        return;
    }

    rtc->command = value & 15;
    rtc->busy = (value & 16) != 0;
    rtc->ready = false;
    rtc->delay = 0;
    rtc->length = 0;
    unsigned group = rtc->command >> 1;
    if (!(value & 16) || group >= sizeof(lengths))
        return;
    rtc->delay = RTC_BYTE;
    rtc->ready = true;
    rtc->length = lengths[group];
    rtc->index = 0;
    if (!group) {
        rtc->busy = false;
        if (rtc->present)
            reset_clock(rtc);
        return;
    }

    if (!(rtc->command & 1)) {
        if (rtc->present)
            write_payload(ws, rtc->data);
        else
            rtc->busy = ++rtc->index != rtc->length;
    } else if (!rtc->present) {
        memset(rtc->payload, 0xff, sizeof(rtc->payload));
    } else if (group == 1) {
        rtc->payload[0] = rtc->status;
        rtc->status &= 0x7f;
    } else if (group == 2 || group == 3) {
        date_payload(rtc, group == 2 ? 0 : 4);
    } else if (group == 4) {
        memcpy(rtc->payload, rtc->alarm, sizeof(rtc->alarm));
    } else {
        memset(rtc->payload, 0xff, sizeof(rtc->payload));
    }
}
