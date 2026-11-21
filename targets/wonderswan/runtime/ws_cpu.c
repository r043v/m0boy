#include "ws.h"
#include <stddef.h>

static uint8_t fetch(Ws *ws)
{
    return ws_read(ws, ws_address(ws->cs, ws->ip++));
}

static uint16_t fetch_word(Ws *ws)
{
    uint8_t low = fetch(ws);
    return low | ((uint16_t)fetch(ws) << 8);
}

static void set_reg_byte(Ws *ws, unsigned index, uint8_t value)
{
    unsigned shift = (index & 4) ? 8 : 0;
    uint16_t *reg = &ws->reg[index & 3];
    *reg = (*reg & ~(0xffu << shift)) | ((uint16_t)value << shift);
}

enum {
    CF = 1, PF = 4, AF = 16, ZF = 64, SF = 128,
    TF = 256, IF = 512, DF = 1024, OF = 2048,
    FLAGS_FIXED = 0xf002, FLAGS_WRITABLE = 0x0fd5
};

typedef struct {
    unsigned mode, rm;
    uint32_t address;
    uint16_t offset;
} Operand;

static Operand decode_operand(Ws *ws, unsigned modrm, const uint16_t *override)
{
    Operand operand = { .mode = modrm >> 6, .rm = modrm & 7 };
    if (operand.mode == 3)
        return operand;
    uint16_t bx = ws->reg[3], bp = ws->reg[5], si = ws->reg[6], di = ws->reg[7];
    const uint16_t bases[] = {bx + si, bx + di, bp + si, bp + di, si, di, bp, bx};
    bool direct = !operand.mode && operand.rm == 6;
    bool stack = operand.rm == 2 || operand.rm == 3 || operand.rm == 6;
    uint16_t offset = direct ? fetch_word(ws) : bases[operand.rm];
    if (operand.mode == 1) {
        unsigned displacement = fetch(ws);
        offset += displacement < 128 ? displacement : displacement - 256;
    } else if (operand.mode == 2) {
        offset += fetch_word(ws);
    }
    operand.offset = offset;
    operand.address = ws_address(override ? *override : stack && !direct ? ws->ss : ws->ds,
                                 offset);
    return operand;
}

static unsigned read_reg(const Ws *ws, unsigned reg, bool word)
{
    if (word)
        return ws->reg[reg];
    return (ws->reg[reg & 3] >> ((reg & 4) ? 8 : 0)) & 255;
}

static void write_reg(Ws *ws, unsigned reg, bool word, unsigned value)
{
    if (word)
        ws->reg[reg] = value;
    else
        set_reg_byte(ws, reg, value);
}

static unsigned read_mem(const Ws *ws, uint32_t address, bool word)
{
    unsigned value = ws_read(ws, address);
    return word ? value | ((unsigned)ws_read(ws, address + 1) << 8) : value;
}

static void write_mem(Ws *ws, uint32_t address, bool word, unsigned value)
{
    ws_write(ws, address, value);
    if (word)
        ws_write(ws, address + 1, value >> 8);
}

static unsigned read_operand(const Ws *ws, Operand operand, bool word)
{
    return operand.mode == 3 ? read_reg(ws, operand.rm, word)
                             : read_mem(ws, operand.address, word);
}

static void write_operand(Ws *ws, Operand operand, bool word, unsigned value)
{
    if (operand.mode == 3)
        write_reg(ws, operand.rm, word, value);
    else
        write_mem(ws, operand.address, word, value);
}

static unsigned alu(Ws *ws, unsigned left, unsigned right, bool word, unsigned operation)
{
    unsigned sign = word ? 0x8000 : 0x80;
    unsigned mask = sign * 2 - 1;
    unsigned result;
    unsigned flags = 0;
    if (operation == 1) {
        result = left | right;
    } else if (operation == 4) {
        result = left & right;
    } else if (operation == 6) {
        result = left ^ right;
    } else {
        bool subtract = operation == 3 || operation == 5 || operation == 7;
        unsigned carry = (operation == 2 || operation == 3) && (ws->flags & CF);
        result = subtract ? left - right - carry : left + right + carry;
        if (result & (mask + 1))
            flags |= CF;
        if ((left ^ right ^ result) & 16)
            flags |= AF;
        if (((subtract ? left ^ right : ~(left ^ right)) & (left ^ result)) & sign)
            flags |= OF;
    }
    result &= mask;
    unsigned parity = result & 255;
    parity ^= parity >> 4;
    parity ^= parity >> 2;
    parity ^= parity >> 1;
    if (!(parity & 1))
        flags |= PF;
    if (!result)
        flags |= ZF;
    if (result & sign)
        flags |= SF;
    ws->flags = (ws->flags & ~(CF | PF | AF | ZF | SF | OF)) | flags;
    return result;
}

static void decimal_adjust(Ws *ws, bool subtract, bool ascii)
{
    unsigned al = read_reg(ws, 0, false);
    bool adjust_low = (al & 15) > 9 || (ws->flags & AF);
    int direction = subtract ? -1 : 1;
    if (ascii) {
        if (adjust_low) {
            set_reg_byte(ws, 4, read_reg(ws, 4, false) + direction);
            al += direction * 6;
        }
        set_reg_byte(ws, 0, al & 15);
        ws->flags &= ~(AF | CF);
        if (adjust_low)
            ws->flags |= AF | CF;
        return;
    }

    bool old_carry = ws->flags & CF;
    bool adjust_high = old_carry || al > 0x99;
    bool low_borrow = subtract && adjust_low && al < 6;
    unsigned result = al;
    if (adjust_low)
        result += direction * 6;
    if (adjust_high)
        result += direction * 0x60;
    unsigned overflow = ws->flags & OF;
    alu(ws, result & 255, 0, false, 6);
    ws->flags |= overflow | (adjust_low ? AF : 0) |
                 (adjust_high || low_borrow ? CF : 0);
    set_reg_byte(ws, 0, result);
}

static int32_t signed_value(uint32_t value, unsigned bits)
{
    uint32_t sign = UINT32_C(1) << (bits - 1);
    return (int32_t)(value & (sign - 1)) - (value & sign ? (int32_t)(sign - 1) : 0)
           - ((value & sign) != 0);
}

static void multiply_flags(Ws *ws, bool overflow)
{
    ws->flags &= ~(CF | OF);
    if (overflow)
        ws->flags |= CF | OF;
}

static void store_division(Ws *ws, bool word, unsigned quotient, unsigned remainder)
{
    if (word) {
        ws->reg[0] = quotient;
        ws->reg[2] = remainder;
    } else {
        ws->reg[0] = (quotient & 255) | ((remainder & 255) << 8);
    }
}

static bool condition(unsigned flags, unsigned cc)
{
    bool result;
    switch (cc >> 1) {
    case 0: result = flags & OF; break;
    case 1: result = flags & CF; break;
    case 2: result = flags & ZF; break;
    case 3: result = flags & (CF | ZF); break;
    case 4: result = flags & SF; break;
    case 5: result = flags & PF; break;
    case 6: result = !!(flags & SF) != !!(flags & OF); break;
    default: result = (flags & ZF) || (!!(flags & SF) != !!(flags & OF)); break;
    }
    return result != (bool)(cc & 1);
}

static void relative_byte(Ws *ws, unsigned displacement)
{
    ws->ip += displacement < 128 ? displacement : displacement - 256;
}

static unsigned read_port(Ws *ws, uint16_t port, bool word)
{
    unsigned value = ws_in(ws, port);
    if (word)
        value |= (unsigned)ws_in(ws, (uint16_t)(port + 1)) << 8;
    return value;
}

static void write_port(Ws *ws, uint16_t port, bool word, unsigned value)
{
    ws_out(ws, port, value);
    if (word)
        ws_out(ws, (uint16_t)(port + 1), value >> 8);
}

static bool string_opcode(unsigned op)
{
    return (op >= 0x6c && op <= 0x6f) ||
           (op >= 0xa4 && op <= 0xaf && op != 0xa8 && op != 0xa9);
}

static unsigned string_step(Ws *ws, unsigned op, const uint16_t *segment,
                            unsigned repeat, uint16_t start_ip)
{
    if (repeat && !ws->reg[1])
        return 1;

    bool word = op & 1;
    int delta = word ? 2 : 1;
    if (ws->flags & DF)
        delta = -delta;
    unsigned source = ws_address(segment ? *segment : ws->ds, ws->reg[6]);
    unsigned destination = ws_address(ws->es, ws->reg[7]);
    bool compare = false;
    unsigned clocks = 3;

    switch (op & ~1u) {
    case 0x6c: // INS
        write_mem(ws, destination, word, read_port(ws, ws->reg[2], word));
        ws->reg[7] += delta;
        clocks = 6;
        break;
    case 0x6e: // OUTS
        write_port(ws, ws->reg[2], word, read_mem(ws, source, word));
        ws->reg[6] += delta;
        clocks = 6;
        break;
    case 0xa4: // MOVS
        write_mem(ws, destination, word, read_mem(ws, source, word));
        ws->reg[6] += delta;
        ws->reg[7] += delta;
        clocks = 5;
        break;
    case 0xa6: // CMPS
        alu(ws, read_mem(ws, source, word), read_mem(ws, destination, word), word, 7);
        ws->reg[6] += delta;
        ws->reg[7] += delta;
        compare = true;
        clocks = 6;
        break;
    case 0xaa: // STOS
        write_mem(ws, destination, word, ws->reg[0]);
        ws->reg[7] += delta;
        break;
    case 0xac: // LODS
        write_reg(ws, 0, word, read_mem(ws, source, word));
        ws->reg[6] += delta;
        break;
    case 0xae: // SCAS
        alu(ws, read_reg(ws, 0, word), read_mem(ws, destination, word), word, 7);
        ws->reg[7] += delta;
        compare = true;
        clocks = 4;
        break;
    }

    if (repeat && --ws->reg[1] &&
        (!compare || ((ws->flags & ZF) != 0) == (repeat == 0xf3)))
        ws->ip = start_ip;
    return clocks;
}

static void push(Ws *ws, unsigned value)
{
    ws->reg[4] -= 2;
    write_mem(ws, ws_address(ws->ss, ws->reg[4]), true, value);
}

static unsigned pop(Ws *ws)
{
    unsigned value = read_mem(ws, ws_address(ws->ss, ws->reg[4]), true);
    ws->reg[4] += 2;
    return value;
}

static void interrupt(Ws *ws, unsigned vector)
{

    push(ws, ws->flags | FLAGS_FIXED);
    push(ws, ws->cs);
    push(ws, ws->ip);
    ws->flags &= ~(IF | TF);
    ws->ip = read_mem(ws, vector * 4, true);
    ws->cs = read_mem(ws, vector * 4 + 2, true);
}

bool ws_can_wake(const Ws *ws)
{
    if (ws->nmi_pending)
        return true;
    unsigned sources = ws->irq_pending | 0x50;
    if (ws->rtc.present && (ws->rtc.status & 0x2a))
        sources |= 4;
    if (ws->uart_control & 0x80)
        sources |= 9;
    if (ws->timer_count[0] == 1 || ((ws->timer_control & 1) && ws->timer_count[0]))
        sources |= 0x80;
    if (ws->timer_count[1] == 1 || ((ws->timer_control & 4) && ws->timer_count[1]))
        sources |= 0x20;
    return (ws->irq_enable & sources) != 0;
}

WsStep ws_step_with_video(Ws *ws, WsLineSink sink, void *context)
{
    bool shadow = ws->irq_shadow;
    bool stack_shadow = ws->stack_shadow;
    ws->irq_shadow = ws->stack_shadow = false;
    if (!stack_shadow && ws->nmi_pending) {
        ws->nmi_pending = false;
        interrupt(ws, 2);
        ws->halted = false;
        ws_video_clock(ws, 2, sink, context);
        return WS_STEP_OK;
    }
    if (!shadow && !stack_shadow && (ws->flags & IF) &&
        (ws->irq_pending & ws->irq_enable)) {
        unsigned source = 7;
        while (!(ws->irq_pending & ws->irq_enable & (1u << source)))
            --source;
        interrupt(ws, ws->irq_base | source);
        ws->halted = false;
        ws_video_clock(ws, 2, sink, context);
        return WS_STEP_OK;
    }
    // INT releases standby even when IF prevents taking the vector.
    if (ws->halted && (ws->irq_pending & ws->irq_enable))
        ws->halted = false;
    if (ws->halted) {
        if (!ws_can_wake(ws))
            return WS_STEP_HALTED;
        unsigned clocks = 256 - ws->line_clock;
        if ((ws->uart_control & 0x80) && ws->uart_tx_clocks &&
            ws->uart_tx_clocks < clocks)
            clocks = ws->uart_tx_clocks;
        ws_video_clock(ws, clocks, sink, context);
        return WS_STEP_OK;
    }

    if (ws->instruction_sink) {
        uint32_t pc = ws_address(ws->cs, ws->ip);
        ws->instruction_sink(ws->instruction_context, pc, ws_rom_offset(ws, pc));
    }

    unsigned start_flags = ws->flags;
    uint16_t start_ip = ws->ip;
    const uint16_t *segment = NULL;
    unsigned repeat = 0;
    uint8_t op;
    unsigned prefixes = 0;
    for (;;) {
        op = fetch(ws);
        switch (op) {
        case 0x26: segment = &ws->es; break;
        case 0x2e: segment = &ws->cs; break;
        case 0x36: segment = &ws->ss; break;
        case 0x3e: segment = &ws->ds; break;
        case 0xf0: break; // No other bus master executes mid-instruction.
        case 0xf2: case 0xf3: repeat = op; break;
        default: goto decoded;
        }
        if (++prefixes == 15)
            goto unsupported;
    }
decoded:
    if (repeat && !string_opcode(op))
        goto unsupported;

    unsigned clocks = 1;
    if (op >= 0xb0 && op <= 0xb7) {
        set_reg_byte(ws, op - 0xb0, fetch(ws));
    } else if (op >= 0xb8 && op <= 0xbf) {
        ws->reg[op - 0xb8] = fetch_word(ws);
    } else if (op >= 0x40 && op <= 0x4f) {
        unsigned reg = op & 7;
        unsigned carry = ws->flags & CF;
        ws->reg[reg] = alu(ws, ws->reg[reg], 1, true, (op & 8) ? 7 : 0);
        ws->flags = (ws->flags & ~CF) | carry;
    } else if (op >= 0x50 && op <= 0x57) {
        push(ws, ws->reg[op - 0x50]);
        clocks = 1;
    } else if (op >= 0x58 && op <= 0x5f) {
        unsigned value = pop(ws);
        ws->reg[op - 0x58] = value;
        clocks = 1;
    } else if (op >= 0x70 && op <= 0x7f) {
        unsigned displacement = fetch(ws);
        if (condition(ws->flags, op & 15)) {
            relative_byte(ws, displacement);
            clocks = 1;
        }
    } else switch (op) {
    case 0x06: push(ws, ws->es); clocks = 2; break;
    case 0x07: ws->es = pop(ws); clocks = 3; break;
    case 0x0e: push(ws, ws->cs); clocks = 2; break;
    case 0x16: push(ws, ws->ss); clocks = 2; break;
    case 0x17: ws->ss = pop(ws); ws->stack_shadow = true; clocks = 3; break;
    case 0x1e: push(ws, ws->ds); clocks = 2; break;
    case 0x1f: ws->ds = pop(ws); clocks = 3; break;
    case 0x60: {
        uint16_t sp = ws->reg[4];
        for (unsigned reg = 0; reg < 8; ++reg)
            push(ws, reg == 4 ? sp : ws->reg[reg]);
        clocks = 9;
        break;
    }
    case 0x61:
        for (int reg = 7; reg >= 0; --reg) {
            unsigned value = pop(ws);
            if (reg != 4)
                ws->reg[reg] = value;
        }
        clocks = 8;
        break;
    case 0x27: case 0x2f: case 0x37: case 0x3f:
        decimal_adjust(ws, op & 8, op & 16);
        clocks = (op & 16) ? 9 : 10;
        break;
    case 0x62: {
        unsigned modrm = fetch(ws);
        Operand operand = decode_operand(ws, modrm, segment);
        if (operand.mode == 3)
            goto unsupported;
        unsigned value = ws->reg[(modrm >> 3) & 7];
        unsigned low = read_mem(ws, operand.address, true);
        unsigned high = read_mem(ws, operand.address + 2, true);
        clocks = 13;
        if (value < low || value > high) {
            interrupt(ws, 5);
            clocks = 20;
        }
        break;
    }
    case 0xc8: {
        unsigned size = fetch_word(ws);
        unsigned level = fetch(ws) & 31;
        uint16_t bp = ws->reg[5];
        push(ws, bp);
        uint16_t frame = ws->reg[4];
        for (unsigned i = 1; i < level; ++i) {
            bp -= 2;
            push(ws, read_mem(ws, ws_address(ws->ss, bp), true));
        }
        if (level)
            push(ws, frame);
        ws->reg[5] = frame;
        ws->reg[4] -= size;
        clocks = 19 + (level ? (level - 1) * 4 : 0);
        break;
    }
    case 0xc9:
        ws->reg[4] = ws->reg[5];
        ws->reg[5] = pop(ws);
        clocks = 2;
        break;
    case 0xd4: case 0xd5: {
        fetch(ws); // The V30MZ uses base ten.
        unsigned al = read_reg(ws, 0, false);
        if (op == 0xd4) {
            ws->reg[0] = ((al / 10) << 8) | (al % 10);
            alu(ws, ws->reg[0], 0, true, 6);
            clocks = 17;
        } else {
            ws->reg[0] = (al + read_reg(ws, 4, false) * 10) & 255;
            alu(ws, ws->reg[0], 0, false, 6);
            clocks = 6;
        }
        break;
    }
    case 0xd6:
        set_reg_byte(ws, 0, (ws->flags & CF) ? 255 : 0);
        clocks = 3;
        break;
    case 0xd7: {
        uint16_t offset = ws->reg[3] + read_reg(ws, 0, false);
        set_reg_byte(ws, 0, ws_read(ws, ws_address(segment ? *segment : ws->ds, offset)));
        clocks = 5;
        break;
    }
    case 0x9b: // The WonderSwan holds POLLB low.
        break;
    case 0x68: case 0x6a: {
        unsigned immediate = op == 0x68 ? fetch_word(ws)
                                      : (uint16_t)signed_value(fetch(ws), 8);
        push(ws, immediate);
        break;
    }
    case 0x69: case 0x6b: {
        unsigned modrm = fetch(ws);
        Operand operand = decode_operand(ws, modrm, segment);
        int32_t immediate = op == 0x69 ? signed_value(fetch_word(ws), 16)
                                      : signed_value(fetch(ws), 8);
        int32_t result = signed_value(read_operand(ws, operand, true), 16) * immediate;
        ws->reg[(modrm >> 3) & 7] = result;
        multiply_flags(ws, result < -32768 || result > 32767);
        clocks = operand.mode == 3 ? 3 : 4;
        break;
    }
    case 0x9c: push(ws, ws->flags | FLAGS_FIXED); clocks = 2; break;
    case 0x9d:
        ws->flags = (pop(ws) & FLAGS_WRITABLE) | FLAGS_FIXED;
        ws->irq_shadow = !(start_flags & IF) && (ws->flags & IF);
        clocks = 3;
        break;
    case 0x9e:
        ws->flags = (ws->flags & ~0xffu) | (read_reg(ws, 4, false) & 0xd5) | 2;
        clocks = 4;
        break;
    case 0x9f: set_reg_byte(ws, 4, ws->flags | FLAGS_FIXED); clocks = 2; break;
    case 0x86: case 0x87: {
        unsigned modrm = fetch(ws);
        Operand operand = decode_operand(ws, modrm, segment);
        unsigned reg = (modrm >> 3) & 7;
        bool word = op & 1;
        unsigned value = read_operand(ws, operand, word);
        write_operand(ws, operand, word, read_reg(ws, reg, word));
        write_reg(ws, reg, word, value);
        clocks = operand.mode == 3 ? 3 : 5;
        break;
    }
    case 0x8d: case 0xc4: case 0xc5: {
        unsigned modrm = fetch(ws);
        Operand operand = decode_operand(ws, modrm, segment);
        if (operand.mode == 3)
            goto unsupported;
        unsigned reg = (modrm >> 3) & 7;
        if (op == 0x8d) {
            ws->reg[reg] = operand.offset;
        } else {
            ws->reg[reg] = read_mem(ws, operand.address, true);
            uint16_t *target = op == 0xc4 ? &ws->es : &ws->ds;
            *target = read_mem(ws, operand.address + 2, true);
            clocks = 6;
        }
        break;
    }
    case 0x8f: {
        unsigned modrm = fetch(ws);
        if ((modrm >> 3) & 7)
            goto unsupported;
        Operand operand = decode_operand(ws, modrm, segment);
        unsigned value = pop(ws);
        write_operand(ws, operand, true, value);
        clocks = operand.mode == 3 ? 1 : 3;
        break;
    }
    case 0x00: case 0x01: case 0x02: case 0x03:
    case 0x08: case 0x09: case 0x0a: case 0x0b:
    case 0x10: case 0x11: case 0x12: case 0x13:
    case 0x18: case 0x19: case 0x1a: case 0x1b:
    case 0x20: case 0x21: case 0x22: case 0x23:
    case 0x28: case 0x29: case 0x2a: case 0x2b:
    case 0x30: case 0x31: case 0x32: case 0x33:
    case 0x38: case 0x39: case 0x3a: case 0x3b:
    case 0x84: case 0x85:
    case 0x88: case 0x89: case 0x8a: case 0x8b:
    case 0x8c: case 0x8e:
    case 0x80: case 0x81: case 0x82: case 0x83:
    case 0xc6: case 0xc7: {
        unsigned modrm = fetch(ws);
        unsigned reg = (modrm >> 3) & 7;
        if ((op == 0xc6 || op == 0xc7) && reg)
            goto unsupported;
        uint16_t *segments[] = {&ws->es, &ws->cs, &ws->ss, &ws->ds};
        if ((op == 0x8c || op == 0x8e) && (reg >= 4 || (op == 0x8e && reg == 1)))
            goto unsupported;
        Operand operand = decode_operand(ws, modrm, segment);
        bool word = (op & 1) || op == 0x8c || op == 0x8e;
        // BizHawk's core follows the V30MZ timings; register operands are cheaper
        // than memory only for the read-modify ALU, test and stack groups.
        if ((op >= 0x88 && op <= 0x8c) || op == 0xc6 || op == 0xc7)
            clocks = 1;
        else if (op == 0x8e)
            clocks = operand.mode == 3 ? 3 : 2;
        else if ((op & 2) || op == 0x84 || op == 0x85 || (op >= 0x38 && op <= 0x3b))
            clocks = operand.mode == 3 ? 2 : 1;
        else
            clocks = operand.mode == 3 ? 3 : 1;
        switch (op) {
        case 0x88: case 0x89:
            write_operand(ws, operand, word, read_reg(ws, reg, word));
            break;
        case 0x8a: case 0x8b:
            write_reg(ws, reg, word, read_operand(ws, operand, word));
            break;
        case 0x8c:
            write_operand(ws, operand, true, *segments[reg]);
            break;
        case 0x8e:
            *segments[reg] = read_operand(ws, operand, true);
            if (reg == 2)
                ws->stack_shadow = true;
            break;
        case 0xc6: case 0xc7:
            write_operand(ws, operand, word, word ? fetch_word(ws) : fetch(ws));
            break;
        case 0x80: case 0x81: case 0x82: case 0x83: {
            unsigned right = op == 0x81 ? fetch_word(ws) : fetch(ws);
            if (op == 0x83 && right >= 128)
                right |= 0xff00;
            unsigned result = alu(ws, read_operand(ws, operand, word), right, word, reg);
            if (reg != 7)
                write_operand(ws, operand, word, result);
            break;
        }
        default: {
            unsigned operation = op >= 0x84 ? 4 : op >> 3;
            unsigned rm_value = read_operand(ws, operand, word);
            unsigned reg_value = read_reg(ws, reg, word);
            if (op & 2) {
                unsigned value = alu(ws, reg_value, rm_value, word, operation);
                if (operation != 7)
                    write_reg(ws, reg, word, value);
            } else {
                unsigned value = alu(ws, rm_value, reg_value, word, operation);
                if (operation != 7 && op < 0x84)
                    write_operand(ws, operand, word, value);
            }
            break;
        }
        }
        break;
    }
    case 0x04: case 0x05: case 0x0c: case 0x0d:
    case 0x14: case 0x15: case 0x1c: case 0x1d:
    case 0x24: case 0x25: case 0x2c: case 0x2d:
    case 0x34: case 0x35: case 0x3c: case 0x3d:
    case 0xa8: case 0xa9: {
        bool word = op & 1;
        unsigned right = word ? fetch_word(ws) : fetch(ws);
        unsigned operation = op >= 0xa8 ? 4 : op >> 3;
        unsigned value = alu(ws, read_reg(ws, 0, word), right, word, operation);
        if (operation != 7 && op < 0xa8)
            write_reg(ws, 0, word, value);
        break;
    }
    case 0xe0: case 0xe1: case 0xe2: case 0xe3: {
        unsigned displacement = fetch(ws);
        bool taken = op == 0xe3 ? ws->reg[1] == 0 : --ws->reg[1] != 0;
        if (op < 0xe2)
            taken &= ((ws->flags & ZF) != 0) == (op == 0xe1);
        if (taken)
            relative_byte(ws, displacement);
        clocks = 1 + (taken ? 3 : 0);
        if (op != 0xe3)
            clocks += op == 0xe2 ? 1 : 2;
        break;
    }
    case 0x90: clocks = 3; break;
    case 0x91: case 0x92: case 0x93: case 0x94:
    case 0x95: case 0x96: case 0x97: {
        unsigned reg = op & 7, value = ws->reg[reg];
        ws->reg[reg] = ws->reg[0];
        ws->reg[0] = value;
        clocks = 3;
        break;
    }
    case 0x98:
        ws->reg[0] = (ws->reg[0] & 0x80) ? ws->reg[0] | 0xff00 : ws->reg[0] & 0xff;
        break;
    case 0x99:
        ws->reg[2] = (ws->reg[0] & 0x8000) ? 0xffff : 0;
        break;
    case 0xa0: case 0xa1: case 0xa2: case 0xa3: {
        uint16_t offset = fetch_word(ws);
        uint32_t address = ws_address(segment ? *segment : ws->ds, offset);
        bool word = op & 1;
        if (op & 2)
            write_mem(ws, address, word, ws->reg[0]);
        else
            write_reg(ws, 0, word, read_mem(ws, address, word));
        clocks = 1;
        break;
    }
    case 0x6c: case 0x6d: case 0x6e: case 0x6f:
    case 0xa4: case 0xa5: case 0xa6: case 0xa7:
    case 0xaa: case 0xab: case 0xac: case 0xad: case 0xae: case 0xaf:
        clocks = string_step(ws, op, segment, repeat, start_ip);
        break;
    case 0xcc: interrupt(ws, 3); clocks = 9; break;
    case 0xcd: interrupt(ws, fetch(ws)); clocks = 10; break;
    case 0xce:
        clocks = 6;
        if (ws->flags & OF) {
            interrupt(ws, 4);
            clocks = 13;
        }
        break;
    case 0xcf:
        ws->ip = pop(ws);
        ws->cs = pop(ws);
        ws->flags = (pop(ws) & FLAGS_WRITABLE) | FLAGS_FIXED;
        clocks = 10;
        break;
    case 0xc2: case 0xc3: case 0xca: case 0xcb: {
        unsigned discard = (op & 1) ? 0 : fetch_word(ws);
        ws->ip = pop(ws);
        if (op & 8)
            ws->cs = pop(ws);
        ws->reg[4] += discard;
        clocks = (op & 8) ? 8 : 6;
        break;
    }
    case 0xd0: case 0xd1: case 0xd2: case 0xd3:
    case 0xc0: case 0xc1: {
        unsigned modrm = fetch(ws);
        unsigned operation = (modrm >> 3) & 7;
        if (operation == 6)
            goto unsupported;
        Operand operand = decode_operand(ws, modrm, segment);
        bool word = op & 1;
        unsigned count = op < 0xd0 ? fetch(ws) : (op & 2) ? ws->reg[1] : 1;
        count &= 31;
        // BizHawk's core charges a flat cost per shift, whatever the count.
        if (op < 0xd0 || op >= 0xd2)
            clocks = operand.mode == 3 ? 5 : 3;
        else
            clocks = operand.mode == 3 ? 1 : 3;
        if (!count)
            break;

        unsigned value = read_operand(ws, operand, word);
        unsigned sign = word ? 0x8000 : 0x80;
        unsigned mask = sign * 2 - 1;
        unsigned result = value;
        bool carry = (ws->flags & CF) != 0;
        for (unsigned bit = 0; bit < count; ++bit) {
            if (operation == 2) {
                bool next_carry = (result & sign) != 0;
                result = ((result << 1) & mask) | carry;
                carry = next_carry;
            } else if (operation == 3) {
                bool next_carry = (result & 1) != 0;
                result = (result >> 1) | (carry ? sign : 0);
                carry = next_carry;
            } else if (operation == 0) {
                carry = (result & sign) != 0;
                result = ((result << 1) & mask) | carry;
            } else if (operation == 1) {
                carry = (result & 1) != 0;
                result = (result >> 1) | (carry ? sign : 0);
            } else if (operation == 4) {
                carry = (result & sign) != 0;
                result = (result << 1) & mask;
            } else {
                carry = (result & 1) != 0;
                result = (result >> 1) | (operation == 7 ? result & sign : 0);
            }
        }
        if (operation >= 4)
            alu(ws, result, 0, word, 6);
        else
            ws->flags &= ~(CF | OF);
        if (carry)
            ws->flags |= CF;
        unsigned overflow = operation == 0 || operation == 2 || operation == 4
                            ? result ^ (carry ? sign : 0)
                            : operation == 1 || operation == 3
                              ? result ^ (result << 1) : value ^ result;
        if (count == 1 && (overflow & sign))
            ws->flags |= OF;
        write_operand(ws, operand, word, result);
        break;
    }
    case 0xf6: case 0xf7: {
        unsigned modrm = fetch(ws);
        unsigned operation = (modrm >> 3) & 7;
        if (operation == 1)
            goto unsupported;
        bool word = op & 1;
        Operand operand = decode_operand(ws, modrm, segment);
        if (!operation) {
            unsigned immediate = word ? fetch_word(ws) : fetch(ws);
            alu(ws, read_operand(ws, operand, word), immediate, word, 4);
        } else if (operation <= 3) {
            unsigned value = read_operand(ws, operand, word);
            unsigned result = operation == 2 ? ~value : alu(ws, 0, value, word, 7);
            write_operand(ws, operand, word, result);
        } else {
            uint32_t value = read_operand(ws, operand, word);
            unsigned bits = word ? 16 : 8;
            unsigned mask = (1u << bits) - 1;
            if (operation == 4 || operation == 5) {
                unsigned accumulator = read_reg(ws, 0, word);
                uint32_t result;
                if (operation == 5) {
                    int32_t product = signed_value(accumulator, bits) * signed_value(value, bits);
                    result = product;
                    multiply_flags(ws, product != signed_value(result, bits));
                } else {
                    result = accumulator * value;
                    multiply_flags(ws, result > mask);
                }
                ws->reg[0] = result;
                if (word)
                    ws->reg[2] = result >> 16;
            } else {
                uint32_t dividend = ws->reg[0];
                if (word)
                    dividend |= (uint32_t)ws->reg[2] << 16;
                bool fault = !value;
                if (!fault && operation == 6) {
                    uint32_t quotient = dividend / value;
                    fault = quotient > mask;
                    if (!fault)
                        store_division(ws, word, quotient, dividend % value);
                } else if (!fault) {
                    int32_t numerator = signed_value(dividend, 2 * bits);
                    int32_t denominator = signed_value(value, bits);
                    fault = numerator == INT32_MIN && denominator == -1;
                    if (!fault) {
                        int32_t quotient = numerator / denominator;
                        int32_t limit = 1 << (bits - 1);
                        fault = quotient < -limit || quotient >= limit;
                        if (!fault)
                            store_division(ws, word, quotient, numerator % denominator);
                    }
                }
                if (fault) {
                    ws->ip = start_ip;
                    interrupt(ws, 0);
                }
            }
        }
        if (operation == 4 || operation == 5)
            clocks = operand.mode == 3 ? 3 : 4;
        else if (operation >= 6) {
            if (word)
                clocks = operand.mode == 3 ? (operation == 6 ? 23 : 24)
                                           : (operation == 6 ? 24 : 25);
            else
                clocks = operand.mode == 3 ? (operation == 6 ? 15 : 17)
                                           : (operation == 6 ? 16 : 18);
        } else if (operation == 0)
            clocks = operand.mode == 3 ? 1 : 2;
        else
            clocks = 3;
        break;
    }
    case 0xfe: case 0xff: {
        unsigned modrm = fetch(ws);
        unsigned operation = (modrm >> 3) & 7;
        if ((op == 0xfe && operation > 1) || operation == 7)
            goto unsupported;
        Operand operand = decode_operand(ws, modrm, segment);
        bool far = operation == 3 || operation == 5;
        if (far && operand.mode == 3)
            goto unsupported;
        bool word = op & 1;
        unsigned value = read_operand(ws, operand, word);
        if (operation < 2) {
            unsigned carry = ws->flags & CF;
            value = alu(ws, value, 1, word, operation ? 7 : 0);
            ws->flags = (ws->flags & ~CF) | carry;
            write_operand(ws, operand, word, value);
        } else if (operation == 6)
            push(ws, value);
        else {
            uint16_t target = far ? read_mem(ws, operand.address + 2, true) : ws->cs;
            if (operation == 3)
                push(ws, ws->cs);
            if (operation == 2 || operation == 3)
                push(ws, ws->ip);
            ws->cs = target;
            ws->ip = value;
        }
        if (far)
            clocks = operation == 3 ? 12 : 10;
        else if (operation == 2)
            clocks = operand.mode == 3 ? 5 : 6;
        else if (operation == 4)
            clocks = operand.mode == 3 ? 4 : 5;
        else if (operation == 6)
            clocks = operand.mode == 3 ? 1 : 2;
        else
            clocks = operand.mode == 3 ? 1 : 3;
        break;
    }
    case 0xe4: case 0xe5: case 0xe6: case 0xe7:
    case 0xec: case 0xed: case 0xee: case 0xef: {
        uint16_t port = (op & 8) ? ws->reg[2] : fetch(ws);
        bool word = op & 1;
        if (op & 2)
            write_port(ws, port, word, ws->reg[0]);
        else
            write_reg(ws, 0, word, read_port(ws, port, word));
        clocks = 6;
        break;
    }
    case 0xe8: case 0xe9: {
        uint16_t displacement = fetch_word(ws);
        if (op == 0xe8)
            push(ws, ws->ip);
        ws->ip += displacement;
        clocks = op == 0xe8 ? 5 : 4;
        break;
    }
    case 0x9a: case 0xea: {
        uint16_t offset = fetch_word(ws);
        uint16_t target = fetch_word(ws);
        if (op == 0x9a) {
            push(ws, ws->cs);
            push(ws, ws->ip);
        }
        ws->cs = target;
        ws->ip = offset;
        clocks = op == 0x9a ? 10 : 7;
        break;
    }
    case 0xeb:
        relative_byte(ws, fetch(ws));
        clocks = 4;
        break;
    case 0xf4: ws->halted = true; clocks = 0; break;
    case 0xf5: ws->flags ^= CF; clocks = 4; break;
    case 0xf8: ws->flags &= ~CF; clocks = 4; break;
    case 0xf9: ws->flags |= CF; clocks = 4; break;
    case 0xfa: ws->flags &= ~IF; clocks = 4; break;
    case 0xfb:
        ws->irq_shadow = !(ws->flags & IF);
        ws->flags |= IF;
        clocks = 4;
        break;
    case 0xfc: ws->flags &= ~DF; clocks = 4; break;
    case 0xfd: ws->flags |= DF; clocks = 4; break;
    default: goto unsupported;
    }
    ws_video_clock(ws, clocks, sink, context);
#if WS_ENABLE_COLOR
    if (ws->dma_control & 0x80 && !ws->bizhawk_timing)
        ws_dma_run(ws, sink, context);
#endif
    if ((start_flags & ws->flags & TF) && !ws->stack_shadow) {
        interrupt(ws, 1);
        ws->halted = false;
        ws_video_clock(ws, 2, sink, context);
    }
    return ws->halted && !ws_can_wake(ws) ? WS_STEP_HALTED : WS_STEP_OK;

unsupported:
    ws->irq_shadow = shadow;
    ws->stack_shadow = stack_shadow;
    ws->ip = start_ip;
    return WS_STEP_UNSUPPORTED;
}


WsStep ws_step(Ws *ws)
{
    return ws_step_with_video(ws, NULL, NULL);
}
