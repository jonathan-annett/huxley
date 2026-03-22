#ifndef EMU86_OPCODES_LOGIC_H
#define EMU86_OPCODES_LOGIC_H

#include <stdint.h>
#include "../state.h"
#include "../decode.h"
#include "helpers.h"

/* Re-use read_dest/write_dest/read_src from arithmetic.h if included,
 * otherwise define them here. Guard with a check. */
#ifndef EMU86_OPCODES_ARITHMETIC_H
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
    if (d->direction) write_reg(s, d, val);
    else write_rm(s, d, val);
}
#endif

/* ================================================================
 * AND — dest = dest & src
 * Flags: CF=0, OF=0, SF/ZF/PF from result, AF=0
 * ================================================================ */

static inline void
exec_and(Emu86State *s, DecodeContext *d)
{
    uint16_t result = read_dest(s, d) & read_src(s, d);
    set_flags_logic(s, result, d->operand_width);
    write_dest(s, d, result);
}

/* ================================================================
 * OR — dest = dest | src
 * ================================================================ */

static inline void
exec_or(Emu86State *s, DecodeContext *d)
{
    uint16_t result = read_dest(s, d) | read_src(s, d);
    set_flags_logic(s, result, d->operand_width);
    write_dest(s, d, result);
}

/* ================================================================
 * XOR — dest = dest ^ src
 * ================================================================ */

static inline void
exec_xor(Emu86State *s, DecodeContext *d)
{
    uint16_t result = read_dest(s, d) ^ read_src(s, d);
    set_flags_logic(s, result, d->operand_width);
    write_dest(s, d, result);
}

/* ================================================================
 * NOT — dest = ~dest
 * NO flags affected.
 * ================================================================ */

static inline void
exec_not(Emu86State *s, DecodeContext *d)
{
    uint16_t val = read_rm(s, d);
    write_rm(s, d, ~val & MASK(d->operand_width));
}

/* ================================================================
 * TEST — temp = dest & src, set flags, discard result
 * Flags: same as AND (CF=0, OF=0, SF/ZF/PF from result)
 * ================================================================ */

static inline void
exec_test(Emu86State *s, DecodeContext *d)
{
    uint16_t result = read_dest(s, d) & read_src(s, d);
    set_flags_logic(s, result, d->operand_width);
    /* Do NOT write result back */
}

#endif /* EMU86_OPCODES_LOGIC_H */
