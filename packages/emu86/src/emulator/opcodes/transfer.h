#ifndef EMU86_OPCODES_TRANSFER_H
#define EMU86_OPCODES_TRANSFER_H

#include <stdint.h>
#include "../state.h"
#include "../decode.h"
#include "helpers.h"

/* ================================================================
 * Stack helpers (reused by CALL/RET/INT/IRET in later tasks)
 * ================================================================ */

/* Push 16-bit value: SP -= 2, write to SS:SP */
static inline void __attribute__((always_inline))
stack_push(Emu86State *s, uint16_t value)
{
    s->regs[REG_SP] -= 2;
    uint32_t addr = segoff_to_linear(s->sregs[SREG_SS], s->regs[REG_SP]);
    mem_write16(s, addr, value);
}

/* Pop 16-bit value: read from SS:SP, SP += 2 */
static inline uint16_t __attribute__((always_inline))
stack_pop(Emu86State *s)
{
    uint32_t addr = segoff_to_linear(s->sregs[SREG_SS], s->regs[REG_SP]);
    uint16_t val = mem_read16(s, addr);
    s->regs[REG_SP] += 2;
    return val;
}

/* ================================================================
 * FLAGS pack/unpack
 *
 * On 8086: bit 1 is always 1, bits 12-15 are always 1.
 * ================================================================ */

static inline uint16_t
flags_pack(const Emu86State *s)
{
    return (s->flags & 0x0FFF) | 0xF002; /* bits 12-15 = 1, bit 1 = 1 */
}

static inline void
flags_unpack(Emu86State *s, uint16_t val)
{
    /* Preserve bit 1 = 1, clear reserved bits */
    s->flags = (val & 0x0FD5) | 0x0002; /* mask valid flag bits, set bit 1 */
}

/* ================================================================
 * MOV variants — no flags affected
 * ================================================================ */

/* MOV r/m, reg or MOV reg, r/m (direction-aware) */
static inline void
exec_mov_rm_reg(Emu86State *s, DecodeContext *d)
{
    if (d->direction) {
        /* reg ← r/m */
        write_reg(s, d, read_rm(s, d));
    } else {
        /* r/m ← reg */
        write_rm(s, d, read_reg(s, d));
    }
}

/* MOV reg, imm (B0-BF) — register from reg4bit, data from data0 */
static inline void
exec_mov_reg_imm(Emu86State *s, DecodeContext *d)
{
    if (d->operand_width)
        write_reg16(s, d->reg4bit, (uint16_t)d->data0);
    else
        write_reg8(s, d->reg4bit, (uint8_t)(d->data0 & 0xFF));
}

/* MOV r/m, imm (C6/C7) — data from data2 */
static inline void
exec_mov_rm_imm(Emu86State *s, DecodeContext *d)
{
    write_rm(s, d, (uint16_t)d->data2);
}

/* MOV AL/AX, moffs (A0/A1) — direct memory read */
static inline void
exec_mov_al_moffs(Emu86State *s, DecodeContext *d)
{
    uint16_t seg = d->operand_width ? /* dummy */ 0 : 0;
    (void)seg;
    /* Address from data0, segment from DS (or override) */
    uint16_t segment = s->seg_override_en ? s->sregs[s->seg_override] : s->sregs[SREG_DS];
    uint32_t addr = segoff_to_linear(segment, (uint16_t)d->data0);
    if (d->operand_width)
        s->regs[REG_AX] = mem_read16(s, addr);
    else
        s->regs[REG_AX] = (s->regs[REG_AX] & 0xFF00) | mem_read8(s, addr);
}

/* MOV moffs, AL/AX (A2/A3) — direct memory write */
static inline void
exec_mov_moffs_al(Emu86State *s, DecodeContext *d)
{
    uint16_t segment = s->seg_override_en ? s->sregs[s->seg_override] : s->sregs[SREG_DS];
    uint32_t addr = segoff_to_linear(segment, (uint16_t)d->data0);
    if (d->operand_width)
        mem_write16(s, addr, s->regs[REG_AX]);
    else
        mem_write8(s, addr, (uint8_t)s->regs[REG_AX]);
}

/* MOV sreg, r/m (8E) */
static inline void
exec_mov_sreg_rm(Emu86State *s, DecodeContext *d)
{
    s->sregs[d->reg] = read_rm16(s, d);
}

/* MOV r/m, sreg (8C) */
static inline void
exec_mov_rm_sreg(Emu86State *s, DecodeContext *d)
{
    write_rm16(s, d, s->sregs[d->reg]);
}

/* ================================================================
 * PUSH / POP — no flags affected
 * ================================================================ */

static inline void exec_push_reg(Emu86State *s, uint8_t reg)
{
    stack_push(s, s->regs[reg]);
}

static inline void exec_pop_reg(Emu86State *s, uint8_t reg)
{
    s->regs[reg] = stack_pop(s);
}

static inline void exec_push_rm(Emu86State *s, DecodeContext *d)
{
    stack_push(s, read_rm16(s, d));
}

static inline void exec_pop_rm(Emu86State *s, DecodeContext *d)
{
    write_rm16(s, d, stack_pop(s));
}

static inline void exec_push_sreg(Emu86State *s, uint8_t sreg)
{
    stack_push(s, s->sregs[sreg]);
}

static inline void exec_pop_sreg(Emu86State *s, uint8_t sreg)
{
    s->sregs[sreg] = stack_pop(s);
}

static inline void exec_pushf(Emu86State *s)
{
    stack_push(s, flags_pack(s));
}

static inline void exec_popf(Emu86State *s)
{
    flags_unpack(s, stack_pop(s));
}

/* ================================================================
 * XCHG — no flags affected
 * ================================================================ */

static inline void exec_xchg(Emu86State *s, DecodeContext *d)
{
    uint16_t a = read_rm(s, d);
    uint16_t b = read_reg(s, d);
    write_rm(s, d, b);
    write_reg(s, d, a);
}

static inline void exec_xchg_ax_reg(Emu86State *s, uint8_t reg)
{
    uint16_t tmp = s->regs[REG_AX];
    s->regs[REG_AX] = s->regs[reg];
    s->regs[reg] = tmp;
}

/* ================================================================
 * LDS / LES — Load far pointer (no flags)
 * ================================================================ */

static inline void exec_lds(Emu86State *s, DecodeContext *d)
{
    uint16_t offset = mem_read16(s, d->rm_addr);
    uint16_t seg = mem_read16(s, d->rm_addr + 2);
    write_reg16(s, d->reg, offset);
    s->sregs[SREG_DS] = seg;
}

static inline void exec_les(Emu86State *s, DecodeContext *d)
{
    uint16_t offset = mem_read16(s, d->rm_addr);
    uint16_t seg = mem_read16(s, d->rm_addr + 2);
    write_reg16(s, d->reg, offset);
    s->sregs[SREG_ES] = seg;
}

/* ================================================================
 * Conversion — no flags
 * ================================================================ */

/* CBW: sign-extend AL → AX */
static inline void exec_cbw(Emu86State *s)
{
    s->regs[REG_AX] = (uint16_t)(int16_t)(int8_t)(uint8_t)s->regs[REG_AX];
}

/* CWD: sign-extend AX → DX:AX */
static inline void exec_cwd(Emu86State *s)
{
    s->regs[REG_DX] = (s->regs[REG_AX] & 0x8000) ? 0xFFFF : 0x0000;
}

/* XLAT: AL = mem[DS:BX + unsigned AL] */
static inline void exec_xlat(Emu86State *s)
{
    uint16_t seg = s->seg_override_en ? s->sregs[s->seg_override] : s->sregs[SREG_DS];
    uint16_t offset = s->regs[REG_BX] + (uint8_t)s->regs[REG_AX];
    uint32_t addr = segoff_to_linear(seg, offset);
    s->regs[REG_AX] = (s->regs[REG_AX] & 0xFF00) | mem_read8(s, addr);
}

/* ================================================================
 * LAHF / SAHF
 * ================================================================ */

/* LAHF: AH = low byte of FLAGS (SF, ZF, -, AF, -, PF, -, CF) */
static inline void exec_lahf(Emu86State *s)
{
    uint8_t ah = (uint8_t)(s->flags & 0xFF);
    s->regs[REG_AX] = (s->regs[REG_AX] & 0x00FF) | ((uint16_t)ah << 8);
}

/* SAHF: low byte of FLAGS = AH. Only affects SF,ZF,AF,PF,CF (bits 7,6,4,2,0) */
static inline void exec_sahf(Emu86State *s)
{
    uint8_t ah = (uint8_t)(s->regs[REG_AX] >> 8);
    /* Mask: bits 7(SF), 6(ZF), 4(AF), 2(PF), 0(CF) = 0xD5.
     * Preserve bits 8-15 of flags (OF, DF, IF, TF, etc.) */
    s->flags = (s->flags & 0xFF00) | (ah & 0xD5) | 0x02; /* keep bit 1 = 1 */
}

#endif /* EMU86_OPCODES_TRANSFER_H */
