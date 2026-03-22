#ifndef EMU86_OPCODES_HELPERS_H
#define EMU86_OPCODES_HELPERS_H

#include <stdint.h>
#include "../state.h"

/* ================================================================
 * Width helpers
 * ================================================================ */

/* Mask for operand width: 0xFF for byte, 0xFFFF for word */
static inline uint16_t __attribute__((always_inline))
MASK(uint8_t width)
{
    return width ? 0xFFFF : 0x00FF;
}

/* Top bit for operand width: 0x80 for byte, 0x8000 for word */
static inline uint16_t __attribute__((always_inline))
TOP_BIT(uint8_t width)
{
    return width ? 0x8000 : 0x0080;
}

/* Extract sign bit: 1 if negative, 0 if positive */
static inline int __attribute__((always_inline))
SIGN_OF(uint32_t val, uint8_t width)
{
    return (val & TOP_BIT(width)) ? 1 : 0;
}

/* ================================================================
 * Flag manipulation
 * ================================================================ */

static inline int __attribute__((always_inline))
get_flag(const Emu86State *s, uint16_t flag)
{
    return (s->flags & flag) ? 1 : 0;
}

static inline void __attribute__((always_inline))
set_flag(Emu86State *s, uint16_t flag)
{
    s->flags |= flag;
}

static inline void __attribute__((always_inline))
clear_flag(Emu86State *s, uint16_t flag)
{
    s->flags &= ~flag;
}

static inline void __attribute__((always_inline))
update_flag(Emu86State *s, uint16_t flag, int condition)
{
    if (condition)
        s->flags |= flag;
    else
        s->flags &= ~flag;
}

/* ================================================================
 * Parity computation
 *
 * Even parity of low 8 bits: PF=1 if even number of 1-bits.
 * This matches the 8086 behavior where PF always checks the
 * low byte only, even for 16-bit operations.
 * ================================================================ */

static inline int __attribute__((always_inline))
parity8(uint8_t val)
{
    val ^= val >> 4;
    val ^= val >> 2;
    val ^= val >> 1;
    return (~val) & 1; /* 1 if even parity */
}

/* ================================================================
 * Flag group setters
 * ================================================================ */

/*
 * Set SF, ZF, PF based on result.
 * PF always checks the low 8 bits only.
 */
static inline void __attribute__((always_inline))
set_flags_szp(Emu86State *s, uint32_t result, uint8_t width)
{
    uint16_t masked = result & MASK(width);
    update_flag(s, FLAG_SF, SIGN_OF(masked, width));
    update_flag(s, FLAG_ZF, masked == 0);
    update_flag(s, FLAG_PF, parity8((uint8_t)masked));
}

/*
 * Set CF, AF, OF for addition (ADD, ADC).
 * dest and src should be the original operand values (pre-operation).
 * result should be computed as (uint32_t)dest + (uint32_t)src [+ CF for ADC].
 *
 * CF = carry out of MSB (result exceeds operand width)
 * AF = carry out of bit 3 (for BCD)
 * OF = signed overflow (both operands same sign, result different sign)
 */
static inline void __attribute__((always_inline))
set_flags_add(Emu86State *s, uint32_t dest, uint32_t src,
              uint32_t result, uint8_t width)
{
    uint16_t mask = MASK(width);
    uint16_t top = TOP_BIT(width);

    update_flag(s, FLAG_CF, result > mask);
    update_flag(s, FLAG_AF, ((dest ^ src ^ result) & 0x10) != 0);
    update_flag(s, FLAG_OF, ((dest ^ result) & (src ^ result) & top) != 0);
}

/*
 * Set CF, AF, OF for subtraction (SUB, SBB, CMP, NEG).
 * dest and src should be the original operand values.
 * result = (uint32_t)dest - (uint32_t)src [- CF for SBB].
 *
 * CF = borrow (dest < src, or result wraps in unsigned)
 * AF = borrow from bit 4
 * OF = signed overflow (operands different sign, result sign differs from dest)
 */
static inline void __attribute__((always_inline))
set_flags_sub(Emu86State *s, uint32_t dest, uint32_t src,
              uint32_t result, uint8_t width)
{
    uint16_t top = TOP_BIT(width);

    /* CF: borrow occurred if result wrapped (exceeds mask in uint32_t) */
    update_flag(s, FLAG_CF, result > MASK(width));
    update_flag(s, FLAG_AF, ((dest ^ src ^ result) & 0x10) != 0);
    update_flag(s, FLAG_OF, ((dest ^ src) & (dest ^ result) & top) != 0);
}

/*
 * Set flags for logic operations (AND, OR, XOR, TEST).
 * CF and OF are always cleared. SF, ZF, PF set normally.
 * AF is undefined per Intel docs but we clear it for consistency.
 */
static inline void __attribute__((always_inline))
set_flags_logic(Emu86State *s, uint32_t result, uint8_t width)
{
    set_flags_szp(s, result, width);
    clear_flag(s, FLAG_CF);
    clear_flag(s, FLAG_OF);
    /* AF is undefined for logic ops; clear it */
    clear_flag(s, FLAG_AF);
}

/*
 * Set flags for INC. Same as ADD except CF is NOT modified.
 */
static inline void __attribute__((always_inline))
set_flags_inc(Emu86State *s, uint32_t dest, uint32_t result, uint8_t width)
{
    uint16_t top = TOP_BIT(width);
    /* src=1 for INC */
    update_flag(s, FLAG_AF, ((dest ^ 1 ^ result) & 0x10) != 0);
    update_flag(s, FLAG_OF, ((dest ^ result) & (1 ^ result) & top) != 0);
    set_flags_szp(s, result, width);
    /* CF is NOT modified */
}

/*
 * Set flags for DEC. Same as SUB except CF is NOT modified.
 */
static inline void __attribute__((always_inline))
set_flags_dec(Emu86State *s, uint32_t dest, uint32_t result, uint8_t width)
{
    uint16_t top = TOP_BIT(width);
    /* src=1 for DEC */
    update_flag(s, FLAG_AF, ((dest ^ 1 ^ result) & 0x10) != 0);
    update_flag(s, FLAG_OF, ((dest ^ 1) & (dest ^ result) & top) != 0);
    set_flags_szp(s, result, width);
    /* CF is NOT modified */
}

#endif /* EMU86_OPCODES_HELPERS_H */
