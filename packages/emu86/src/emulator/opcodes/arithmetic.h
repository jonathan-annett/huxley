#ifndef EMU86_OPCODES_ARITHMETIC_H
#define EMU86_OPCODES_ARITHMETIC_H

#include <stdint.h>
#include "../state.h"
#include "../decode.h"
#include "helpers.h"

/* ================================================================
 * Helper: read dest/src based on direction, write dest back
 * ================================================================ */

static inline uint16_t __attribute__((always_inline))
read_dest(const Emu86State *s, const DecodeContext *d)
{
    return d->direction ? read_reg(s, d) : read_rm(s, d);
}

static inline uint16_t __attribute__((always_inline))
read_src(const Emu86State *s, const DecodeContext *d)
{
    return d->direction ? read_rm(s, d) : read_reg(s, d);
}

static inline void __attribute__((always_inline))
write_dest(Emu86State *s, const DecodeContext *d, uint16_t val)
{
    if (d->direction)
        write_reg(s, d, val);
    else
        write_rm(s, d, val);
}

/* ================================================================
 * ADD
 * dest = dest + src
 * Sets: CF, PF, AF, ZF, SF, OF
 * ================================================================ */

static inline void
exec_add(Emu86State *s, DecodeContext *d)
{
    uint32_t dest = read_dest(s, d);
    uint32_t src  = read_src(s, d);
    uint32_t result = dest + src;

    set_flags_add(s, dest, src, result, d->operand_width);
    set_flags_szp(s, result, d->operand_width);
    write_dest(s, d, result & MASK(d->operand_width));
}

/* ================================================================
 * ADC — add with carry
 * dest = dest + src + CF
 * Sets: CF, PF, AF, ZF, SF, OF
 * ================================================================ */

static inline void
exec_adc(Emu86State *s, DecodeContext *d)
{
    uint32_t dest = read_dest(s, d);
    uint32_t src  = read_src(s, d);
    uint32_t cf   = get_flag(s, FLAG_CF);
    uint32_t result = dest + src + cf;

    set_flags_add(s, dest, src, result, d->operand_width);
    set_flags_szp(s, result, d->operand_width);
    write_dest(s, d, result & MASK(d->operand_width));
}

/* ================================================================
 * SUB
 * dest = dest - src
 * Sets: CF, PF, AF, ZF, SF, OF
 * ================================================================ */

static inline void
exec_sub(Emu86State *s, DecodeContext *d)
{
    uint32_t dest = read_dest(s, d);
    uint32_t src  = read_src(s, d);
    uint32_t result = dest - src;

    set_flags_sub(s, dest, src, result, d->operand_width);
    set_flags_szp(s, result, d->operand_width);
    write_dest(s, d, result & MASK(d->operand_width));
}

/* ================================================================
 * SBB — subtract with borrow
 * dest = dest - src - CF
 * Sets: CF, PF, AF, ZF, SF, OF
 * ================================================================ */

static inline void
exec_sbb(Emu86State *s, DecodeContext *d)
{
    uint32_t dest = read_dest(s, d);
    uint32_t src  = read_src(s, d);
    uint32_t cf   = get_flag(s, FLAG_CF);
    uint32_t result = dest - src - cf;

    set_flags_sub(s, dest, src, result, d->operand_width);
    set_flags_szp(s, result, d->operand_width);
    write_dest(s, d, result & MASK(d->operand_width));
}

/* ================================================================
 * CMP — compare (SUB without writeback)
 * temp = dest - src, set flags, discard result
 * Sets: CF, PF, AF, ZF, SF, OF
 * ================================================================ */

static inline void
exec_cmp(Emu86State *s, DecodeContext *d)
{
    uint32_t dest = read_dest(s, d);
    uint32_t src  = read_src(s, d);
    uint32_t result = dest - src;

    set_flags_sub(s, dest, src, result, d->operand_width);
    set_flags_szp(s, result, d->operand_width);
    /* Do NOT write result back */
}

/* ================================================================
 * NEG — two's complement negation
 * dest = 0 - dest
 * CF = 1 unless operand was 0
 * OF = 1 if operand was TOP_BIT (0x80 / 0x8000)
 * Sets: CF, PF, AF, ZF, SF, OF
 * ================================================================ */

static inline void
exec_neg(Emu86State *s, DecodeContext *d)
{
    uint32_t dest = read_rm(s, d);
    uint32_t result = (uint32_t)0 - dest;

    set_flags_sub(s, 0, dest, result, d->operand_width);
    set_flags_szp(s, result, d->operand_width);
    write_rm(s, d, result & MASK(d->operand_width));
}

/* ================================================================
 * INC — increment by 1
 * dest = dest + 1
 * Sets: PF, AF, ZF, SF, OF — does NOT affect CF
 * ================================================================ */

static inline void
exec_inc(Emu86State *s, DecodeContext *d)
{
    uint32_t dest = read_rm(s, d);
    uint32_t result = dest + 1;

    set_flags_inc(s, dest, result, d->operand_width);
    write_rm(s, d, result & MASK(d->operand_width));
}

/* ================================================================
 * DEC — decrement by 1
 * dest = dest - 1
 * Sets: PF, AF, ZF, SF, OF — does NOT affect CF
 * ================================================================ */

static inline void
exec_dec(Emu86State *s, DecodeContext *d)
{
    uint32_t dest = read_rm(s, d);
    uint32_t result = dest - 1;

    set_flags_dec(s, dest, result, d->operand_width);
    write_rm(s, d, result & MASK(d->operand_width));
}

/* ================================================================
 * MUL — unsigned multiply
 * Byte: AX = AL * r/m8.  CF=OF=1 if AH != 0
 * Word: DX:AX = AX * r/m16.  CF=OF=1 if DX != 0
 * ================================================================ */

static inline void
exec_mul(Emu86State *s, DecodeContext *d)
{
    uint32_t src = read_rm(s, d);

    if (d->operand_width) {
        /* Word: DX:AX = AX * src */
        uint32_t result = (uint32_t)s->regs[REG_AX] * src;
        s->regs[REG_AX] = (uint16_t)result;
        s->regs[REG_DX] = (uint16_t)(result >> 16);
        update_flag(s, FLAG_CF, s->regs[REG_DX] != 0);
        update_flag(s, FLAG_OF, s->regs[REG_DX] != 0);
    } else {
        /* Byte: AX = AL * src */
        uint16_t result = (uint16_t)(uint8_t)s->regs[REG_AX] * (uint8_t)src;
        s->regs[REG_AX] = result;
        update_flag(s, FLAG_CF, (result >> 8) != 0);
        update_flag(s, FLAG_OF, (result >> 8) != 0);
    }
}

/* ================================================================
 * IMUL — signed multiply
 * Byte: AX = (int8_t)AL * (int8_t)r/m8.
 *       CF=OF=1 if result doesn't fit in signed byte
 * Word: DX:AX = (int16_t)AX * (int16_t)r/m16.
 *       CF=OF=1 if result doesn't fit in signed word
 * ================================================================ */

static inline void
exec_imul(Emu86State *s, DecodeContext *d)
{
    uint32_t src = read_rm(s, d);

    if (d->operand_width) {
        /* Word: DX:AX = (int16_t)AX * (int16_t)src */
        int32_t result = (int32_t)(int16_t)s->regs[REG_AX] * (int16_t)src;
        s->regs[REG_AX] = (uint16_t)result;
        s->regs[REG_DX] = (uint16_t)((uint32_t)result >> 16);
        /* CF=OF=1 if sign-extending AX doesn't reproduce the full result */
        int fits = (result == (int32_t)(int16_t)s->regs[REG_AX]);
        update_flag(s, FLAG_CF, !fits);
        update_flag(s, FLAG_OF, !fits);
    } else {
        /* Byte: AX = (int8_t)AL * (int8_t)src */
        int16_t result = (int16_t)(int8_t)(uint8_t)s->regs[REG_AX] * (int8_t)(uint8_t)src;
        s->regs[REG_AX] = (uint16_t)result;
        /* CF=OF=1 if sign-extending AL doesn't reproduce the full result */
        int fits = (result == (int16_t)(int8_t)(uint8_t)s->regs[REG_AX]);
        update_flag(s, FLAG_CF, !fits);
        update_flag(s, FLAG_OF, !fits);
    }
}

/* ================================================================
 * DIV — unsigned divide
 * Byte: AL = AX / r/m8, AH = AX % r/m8
 * Word: AX = DX:AX / r/m16, DX = DX:AX % r/m16
 * Returns 0 on success, 1 on divide error (div by zero or overflow)
 * ================================================================ */

static inline int
exec_div(Emu86State *s, DecodeContext *d)
{
    uint32_t divisor = read_rm(s, d);

    if (divisor == 0)
        return 1; /* divide by zero */

    if (d->operand_width) {
        /* Word: DX:AX / src */
        uint32_t dividend = ((uint32_t)s->regs[REG_DX] << 16) | s->regs[REG_AX];
        uint32_t quotient = dividend / divisor;
        uint32_t remainder = dividend % divisor;
        if (quotient > 0xFFFF)
            return 1; /* quotient overflow */
        s->regs[REG_AX] = (uint16_t)quotient;
        s->regs[REG_DX] = (uint16_t)remainder;
    } else {
        /* Byte: AX / src */
        uint16_t dividend = s->regs[REG_AX];
        uint16_t quotient = dividend / (uint8_t)divisor;
        uint16_t remainder = dividend % (uint8_t)divisor;
        if (quotient > 0xFF)
            return 1; /* quotient overflow */
        /* AL = quotient, AH = remainder */
        s->regs[REG_AX] = ((remainder & 0xFF) << 8) | (quotient & 0xFF);
    }
    return 0;
}

/* ================================================================
 * IDIV — signed divide
 * Byte: AL = (int16_t)AX / (int8_t)r/m8, AH = remainder
 * Word: AX = (int32_t)DX:AX / (int16_t)r/m16, DX = remainder
 * Returns 0 on success, 1 on divide error
 * ================================================================ */

static inline int
exec_idiv(Emu86State *s, DecodeContext *d)
{
    uint32_t raw_divisor = read_rm(s, d);

    if (d->operand_width) {
        int16_t divisor = (int16_t)raw_divisor;
        if (divisor == 0)
            return 1;
        int32_t dividend = (int32_t)(((uint32_t)s->regs[REG_DX] << 16) | s->regs[REG_AX]);
        int32_t quotient = dividend / divisor;
        int32_t remainder = dividend % divisor;
        if (quotient > 32767 || quotient < -32768)
            return 1; /* overflow */
        s->regs[REG_AX] = (uint16_t)quotient;
        s->regs[REG_DX] = (uint16_t)remainder;
    } else {
        int8_t divisor = (int8_t)(uint8_t)raw_divisor;
        if (divisor == 0)
            return 1;
        int16_t dividend = (int16_t)s->regs[REG_AX];
        int16_t quotient = dividend / divisor;
        int16_t remainder = dividend % divisor;
        if (quotient > 127 || quotient < -128)
            return 1; /* overflow */
        s->regs[REG_AX] = ((uint16_t)(uint8_t)remainder << 8) | (uint8_t)quotient;
    }
    return 0;
}

/* ================================================================
 * BCD arithmetic
 * ================================================================ */

/*
 * DAA — Decimal Adjust after Addition
 * Adjusts AL for packed BCD after ADD/ADC.
 */
static inline void
exec_daa(Emu86State *s)
{
    uint8_t al = (uint8_t)s->regs[REG_AX];
    uint8_t old_al = al;
    int old_cf = get_flag(s, FLAG_CF);
    int new_cf = 0;

    if ((al & 0x0F) > 9 || get_flag(s, FLAG_AF)) {
        al += 6;
        new_cf = old_cf || (al < old_al); /* carry from +6 */
        update_flag(s, FLAG_AF, 1);
    } else {
        update_flag(s, FLAG_AF, 0);
    }

    if (old_al > 0x99 || old_cf) {
        al += 0x60;
        new_cf = 1;
    }

    update_flag(s, FLAG_CF, new_cf);
    s->regs[REG_AX] = (s->regs[REG_AX] & 0xFF00) | al;
    set_flags_szp(s, al, 0);
}

/*
 * DAS — Decimal Adjust after Subtraction
 * Adjusts AL for packed BCD after SUB/SBB.
 */
static inline void
exec_das(Emu86State *s)
{
    uint8_t al = (uint8_t)s->regs[REG_AX];
    uint8_t old_al = al;
    int old_cf = get_flag(s, FLAG_CF);
    int new_cf = 0;

    if ((al & 0x0F) > 9 || get_flag(s, FLAG_AF)) {
        al -= 6;
        new_cf = old_cf || (al > old_al); /* borrow from -6 */
        update_flag(s, FLAG_AF, 1);
    } else {
        update_flag(s, FLAG_AF, 0);
    }

    if (old_al > 0x99 || old_cf) {
        al -= 0x60;
        new_cf = 1;
    }

    update_flag(s, FLAG_CF, new_cf);
    s->regs[REG_AX] = (s->regs[REG_AX] & 0xFF00) | al;
    set_flags_szp(s, al, 0);
}

/*
 * AAA — ASCII Adjust after Addition
 * Adjusts AL for unpacked BCD, increments AH if needed.
 */
static inline void
exec_aaa(Emu86State *s)
{
    uint8_t al = (uint8_t)s->regs[REG_AX];

    if ((al & 0x0F) > 9 || get_flag(s, FLAG_AF)) {
        s->regs[REG_AX] += 0x106; /* AL += 6, AH += 1 */
        update_flag(s, FLAG_AF, 1);
        update_flag(s, FLAG_CF, 1);
    } else {
        update_flag(s, FLAG_AF, 0);
        update_flag(s, FLAG_CF, 0);
    }
    s->regs[REG_AX] &= 0xFF0F; /* mask AL to low nibble */
}

/*
 * AAS — ASCII Adjust after Subtraction
 * Adjusts AL for unpacked BCD, decrements AH if needed.
 */
static inline void
exec_aas(Emu86State *s)
{
    uint8_t al = (uint8_t)s->regs[REG_AX];

    if ((al & 0x0F) > 9 || get_flag(s, FLAG_AF)) {
        s->regs[REG_AX] -= 0x106; /* AL -= 6, AH -= 1 */
        update_flag(s, FLAG_AF, 1);
        update_flag(s, FLAG_CF, 1);
    } else {
        update_flag(s, FLAG_AF, 0);
        update_flag(s, FLAG_CF, 0);
    }
    s->regs[REG_AX] &= 0xFF0F; /* mask AL to low nibble */
}

/*
 * AAM — ASCII Adjust AX after Multiply
 * AH = AL / base, AL = AL % base (base is usually 10)
 * Returns 0 on success, 1 on divide-by-zero (base == 0)
 */
static inline void
exec_aam(Emu86State *s, uint8_t base)
{
    if (base == 0) return; /* caller should trigger INT 0 */
    uint8_t al = (uint8_t)s->regs[REG_AX];
    uint8_t ah = al / base;
    al = al % base;
    s->regs[REG_AX] = ((uint16_t)ah << 8) | al;
    set_flags_szp(s, al, 0);
}

/*
 * AAD — ASCII Adjust AX before Division
 * AL = AH * base + AL, AH = 0 (base is usually 10)
 */
static inline void
exec_aad(Emu86State *s, uint8_t base)
{
    uint8_t al = (uint8_t)s->regs[REG_AX];
    uint8_t ah = (uint8_t)(s->regs[REG_AX] >> 8);
    al = (uint8_t)(ah * base + al);
    s->regs[REG_AX] = al; /* AH = 0 implicitly */
    set_flags_szp(s, al, 0);
}

#endif /* EMU86_OPCODES_ARITHMETIC_H */
