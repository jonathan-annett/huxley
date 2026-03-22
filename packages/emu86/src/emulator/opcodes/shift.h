#ifndef EMU86_OPCODES_SHIFT_H
#define EMU86_OPCODES_SHIFT_H

#include <stdint.h>
#include "../state.h"
#include "../decode.h"
#include "helpers.h"

/* ================================================================
 * SHL / SAL — Shift Left
 * CF = last bit shifted out. OF (count=1) = CF XOR new MSB.
 * Sets SF, ZF, PF. AF undefined (cleared).
 * ================================================================ */

static inline void
exec_shl(Emu86State *s, DecodeContext *d, uint8_t count)
{
    if (count == 0) return;

    uint8_t w = d->operand_width;
    uint8_t bits = (w + 1) * 8;
    uint32_t val = read_rm(s, d);

    /* CF = last bit shifted out (the bit at position bits-count) */
    update_flag(s, FLAG_CF, (val >> (bits - count)) & 1);

    uint32_t result = (val << count) & MASK(w);

    /* OF defined only for count=1: CF XOR MSB of result */
    if (count == 1)
        update_flag(s, FLAG_OF, get_flag(s, FLAG_CF) ^ SIGN_OF(result, w));

    set_flags_szp(s, result, w);
    clear_flag(s, FLAG_AF);
    write_rm(s, d, result);
}

/* ================================================================
 * SHR — Shift Right Logical
 * CF = last bit shifted out. OF (count=1) = original MSB.
 * Sets SF, ZF, PF.
 * ================================================================ */

static inline void
exec_shr(Emu86State *s, DecodeContext *d, uint8_t count)
{
    if (count == 0) return;

    uint8_t w = d->operand_width;
    uint32_t val = read_rm(s, d);

    /* OF (count=1) = original MSB */
    if (count == 1)
        update_flag(s, FLAG_OF, SIGN_OF(val, w));

    /* CF = last bit shifted out (bit at position count-1) */
    update_flag(s, FLAG_CF, (val >> (count - 1)) & 1);

    uint32_t result = (val >> count) & MASK(w);

    set_flags_szp(s, result, w);
    clear_flag(s, FLAG_AF);
    write_rm(s, d, result);
}

/* ================================================================
 * SAR — Shift Right Arithmetic (preserves sign bit)
 * CF = last bit shifted out. OF (count=1) = 0.
 * Sets SF, ZF, PF.
 * ================================================================ */

static inline void
exec_sar(Emu86State *s, DecodeContext *d, uint8_t count)
{
    if (count == 0) return;

    uint8_t w = d->operand_width;
    uint8_t bits = (w + 1) * 8;
    uint32_t val = read_rm(s, d);
    int32_t sval;

    /* Sign-extend to perform arithmetic shift */
    if (w)
        sval = (int32_t)(int16_t)val;
    else
        sval = (int32_t)(int8_t)(uint8_t)val;

    /* CF = last bit shifted out */
    update_flag(s, FLAG_CF, (sval >> (count - 1)) & 1);

    int32_t sresult = sval >> count;

    /* Clamp to count < bits: if count >= bits, result is all sign bits */
    uint32_t result;
    if (count >= bits)
        result = (sval < 0) ? MASK(w) : 0;
    else
        result = (uint32_t)sresult & MASK(w);

    if (count == 1)
        update_flag(s, FLAG_OF, 0);

    set_flags_szp(s, result, w);
    clear_flag(s, FLAG_AF);
    write_rm(s, d, result);
}

/* ================================================================
 * ROL — Rotate Left
 * CF = new LSB (old MSB). OF (count=1) = MSB XOR CF.
 * Does NOT affect SF, ZF, PF, AF.
 * ================================================================ */

static inline void
exec_rol(Emu86State *s, DecodeContext *d, uint8_t count)
{
    if (count == 0) return;

    uint8_t w = d->operand_width;
    uint8_t bits = (w + 1) * 8;
    uint32_t val = read_rm(s, d);

    count %= bits; /* normalize rotation */
    if (count == 0) count = bits; /* full rotation */

    uint32_t result = ((val << count) | (val >> (bits - count))) & MASK(w);

    update_flag(s, FLAG_CF, result & 1);
    if (count == 1)
        update_flag(s, FLAG_OF, SIGN_OF(result, w) ^ get_flag(s, FLAG_CF));

    write_rm(s, d, result);
}

/* ================================================================
 * ROR — Rotate Right
 * CF = new MSB (old LSB). OF (count=1) = MSB XOR (MSB-1).
 * Does NOT affect SF, ZF, PF, AF.
 * ================================================================ */

static inline void
exec_ror(Emu86State *s, DecodeContext *d, uint8_t count)
{
    if (count == 0) return;

    uint8_t w = d->operand_width;
    uint8_t bits = (w + 1) * 8;
    uint32_t val = read_rm(s, d);

    count %= bits;
    if (count == 0) count = bits;

    uint32_t result = ((val >> count) | (val << (bits - count))) & MASK(w);

    update_flag(s, FLAG_CF, SIGN_OF(result, w));
    if (count == 1)
        update_flag(s, FLAG_OF, SIGN_OF(result, w) ^ ((result >> (bits - 2)) & 1));

    write_rm(s, d, result);
}

/* ================================================================
 * RCL — Rotate through Carry Left (9-bit / 17-bit rotation)
 * CF = old MSB. OF (count=1) = MSB XOR CF.
 * Does NOT affect SF, ZF, PF, AF.
 * ================================================================ */

static inline void
exec_rcl(Emu86State *s, DecodeContext *d, uint8_t count)
{
    if (count == 0) return;

    uint8_t w = d->operand_width;
    uint8_t bits = (w + 1) * 8;
    uint32_t val = read_rm(s, d);
    int cf = get_flag(s, FLAG_CF);

    /* Effective rotation width is bits+1 (including CF) */
    count %= (bits + 1);

    for (uint8_t i = 0; i < count; i++) {
        int new_cf = (val >> (bits - 1)) & 1;
        val = ((val << 1) | cf) & MASK(w);
        cf = new_cf;
    }

    update_flag(s, FLAG_CF, cf);
    update_flag(s, FLAG_OF, SIGN_OF(val, w) ^ cf);

    write_rm(s, d, val);
}

/* ================================================================
 * RCR — Rotate through Carry Right (9-bit / 17-bit rotation)
 * CF = old LSB. OF (count=1) = MSB XOR (MSB-1) of result.
 * Does NOT affect SF, ZF, PF, AF.
 * ================================================================ */

static inline void
exec_rcr(Emu86State *s, DecodeContext *d, uint8_t count)
{
    if (count == 0) return;

    uint8_t w = d->operand_width;
    uint8_t bits = (w + 1) * 8;
    uint32_t val = read_rm(s, d);
    int cf = get_flag(s, FLAG_CF);

    count %= (bits + 1);

    for (uint8_t i = 0; i < count; i++) {
        int new_cf = val & 1;
        val = (val >> 1) | ((uint32_t)cf << (bits - 1));
        val &= MASK(w);
        cf = new_cf;
    }

    update_flag(s, FLAG_CF, cf);
    update_flag(s, FLAG_OF, SIGN_OF(val, w) ^ ((val >> (bits - 2)) & 1));

    write_rm(s, d, val);
}

/* ================================================================
 * Dispatcher: select operation based on d->extra (reg field of ModRM)
 * ================================================================ */

static inline void
exec_shift_rotate(Emu86State *s, DecodeContext *d, uint8_t count)
{
    switch (d->reg) { /* reg field from ModRM selects the operation */
        case 0: exec_rol(s, d, count); break;
        case 1: exec_ror(s, d, count); break;
        case 2: exec_rcl(s, d, count); break;
        case 3: exec_rcr(s, d, count); break;
        case 4: exec_shl(s, d, count); break;
        case 5: exec_shr(s, d, count); break;
        case 6: exec_shl(s, d, count); break; /* undefined, treat as SHL */
        case 7: exec_sar(s, d, count); break;
    }
}

#endif /* EMU86_OPCODES_SHIFT_H */
