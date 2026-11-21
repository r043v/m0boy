#include "ws.h"

enum {
    UART_ENABLE = 0x80,
    UART_FAST = 0x40,
    UART_CLEAR_OVERRUN = 0x20,
    UART_TX_READY = 4,
    UART_OVERRUN = 2,
    UART_RX_READY = 1,
    IRQ_UART_TX = 1,
    IRQ_UART_RX = 8
};

void ws_uart_refresh_irq(Ws *ws)
{
    if (!(ws->uart_control & UART_ENABLE))
        return;
    unsigned levels = ws->uart_tx_clocks ? 0 : IRQ_UART_TX;
    if (ws->uart_status & UART_RX_READY)
        levels |= IRQ_UART_RX;
    ws->irq_pending |= ws->irq_enable & levels;
}

uint8_t ws_uart_read(Ws *ws, bool status)
{
    if (status)
        return ws->uart_control | ws->uart_status |
               (ws->uart_tx_clocks ? 0 : UART_TX_READY);
    ws->uart_status &= ~UART_RX_READY;
    return ws->uart_rx;
}

void ws_uart_write(Ws *ws, bool control, uint8_t value)
{
    if (control) {
        ws->uart_control = value & (UART_ENABLE | UART_FAST);
        if (value & UART_CLEAR_OVERRUN)
            ws->uart_status &= ~UART_OVERRUN;
        ws_uart_refresh_irq(ws);
    } else if (!ws->uart_tx_clocks) {
        ws->uart_tx = value;
        // Start, eight data bits and stop at a 3.072 MHz CPU clock.
        ws->uart_tx_clocks = ws->uart_control & UART_FAST ? 800 : 3200;
    }
}

void ws_uart_receive(Ws *ws, uint8_t value)
{
    if (!(ws->uart_control & UART_ENABLE))
        return;
    if (ws->uart_status & UART_RX_READY) {
        ws->uart_status |= UART_OVERRUN;
    } else {
        ws->uart_rx = value;
        ws->uart_status |= UART_RX_READY;
    }
    ws_uart_refresh_irq(ws);
}

void ws_uart_connect(Ws *ws, WsSerialSink send, void *context)
{
    ws->uart_send = send;
    ws->uart_context = context;
}

void ws_uart_clock(Ws *ws, unsigned clocks)
{
    if (!(ws->uart_control & UART_ENABLE) || !ws->uart_tx_clocks)
        return;
    if (clocks < ws->uart_tx_clocks) {
        ws->uart_tx_clocks -= clocks;
        return;
    }
    ws->uart_tx_clocks = 0;
    ws_uart_refresh_irq(ws);
    if (ws->uart_send)
        ws->uart_send(ws->uart_context, ws->uart_tx);
}
