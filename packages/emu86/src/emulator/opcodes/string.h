#ifndef EMU86_OPCODES_STRING_H
#define EMU86_OPCODES_STRING_H

#include <stdint.h>
#include "../state.h"
#include "../decode.h"
#include "helpers.h"

/* ================================================================
 * Index advancement: advance SI or DI by operand size, respecting DF
 * ================================================================ */

static inline void __attribute__((always_inline))
index_advance(Emu86State *s, uint8_t reg, uint8_t width)
{
    int delta = width ? 2 : 1;
    if (get_flag(s, FLAG_DF))
        s->regs[reg] -= delta;
    else
        s->regs[reg] += delta;
}

/* Source segment helper: DS or segment override */
static inline uint16_t __attribute__((always_inline))
string_src_seg(const Emu86State *s)
{
    return s->seg_override_en ? s->sregs[s->seg_override] : s->sregs[SREG_DS];
}

/* ================================================================
 * MOVSB/MOVSW — Move String (no flags)
 * mem[ES:DI] = mem[src_seg:SI], advance SI and DI
 * ================================================================ */

static inline void exec_movsb(Emu86State *s)
{
    uint32_t src = segoff_to_linear(string_src_seg(s), s->regs[REG_SI]);
    uint32_t dst = segoff_to_linear(s->sregs[SREG_ES], s->regs[REG_DI]);
    mem_write8(s, dst, mem_read8(s, src));
    index_advance(s, REG_SI, 0);
    index_advance(s, REG_DI, 0);
}

static inline void exec_movsw(Emu86State *s)
{
    uint32_t src = segoff_to_linear(string_src_seg(s), s->regs[REG_SI]);
    uint32_t dst = segoff_to_linear(s->sregs[SREG_ES], s->regs[REG_DI]);
    mem_write16(s, dst, mem_read16(s, src));
    index_advance(s, REG_SI, 1);
    index_advance(s, REG_DI, 1);
}

/* ================================================================
 * CMPSB/CMPSW — Compare String (sets flags like SUB)
 * temp = mem[src_seg:SI] - mem[ES:DI], set flags
 * ================================================================ */

static inline void exec_cmpsb(Emu86State *s)
{
    uint32_t src_addr = segoff_to_linear(string_src_seg(s), s->regs[REG_SI]);
    uint32_t dst_addr = segoff_to_linear(s->sregs[SREG_ES], s->regs[REG_DI]);
    uint32_t src_val = mem_read8(s, src_addr);
    uint32_t dst_val = mem_read8(s, dst_addr);
    uint32_t result = src_val - dst_val;
    set_flags_sub(s, src_val, dst_val, result, 0);
    set_flags_szp(s, result, 0);
    index_advance(s, REG_SI, 0);
    index_advance(s, REG_DI, 0);
}

static inline void exec_cmpsw(Emu86State *s)
{
    uint32_t src_addr = segoff_to_linear(string_src_seg(s), s->regs[REG_SI]);
    uint32_t dst_addr = segoff_to_linear(s->sregs[SREG_ES], s->regs[REG_DI]);
    uint32_t src_val = mem_read16(s, src_addr);
    uint32_t dst_val = mem_read16(s, dst_addr);
    uint32_t result = src_val - dst_val;
    set_flags_sub(s, src_val, dst_val, result, 1);
    set_flags_szp(s, result, 1);
    index_advance(s, REG_SI, 1);
    index_advance(s, REG_DI, 1);
}

/* ================================================================
 * STOSB/STOSW — Store String (no flags)
 * mem[ES:DI] = AL/AX, advance DI
 * ================================================================ */

static inline void exec_stosb(Emu86State *s)
{
    uint32_t dst = segoff_to_linear(s->sregs[SREG_ES], s->regs[REG_DI]);
    mem_write8(s, dst, (uint8_t)s->regs[REG_AX]);
    index_advance(s, REG_DI, 0);
}

static inline void exec_stosw(Emu86State *s)
{
    uint32_t dst = segoff_to_linear(s->sregs[SREG_ES], s->regs[REG_DI]);
    mem_write16(s, dst, s->regs[REG_AX]);
    index_advance(s, REG_DI, 1);
}

/* ================================================================
 * LODSB/LODSW — Load String (no flags)
 * AL/AX = mem[src_seg:SI], advance SI
 * ================================================================ */

static inline void exec_lodsb(Emu86State *s)
{
    uint32_t src = segoff_to_linear(string_src_seg(s), s->regs[REG_SI]);
    s->regs[REG_AX] = (s->regs[REG_AX] & 0xFF00) | mem_read8(s, src);
    index_advance(s, REG_SI, 0);
}

static inline void exec_lodsw(Emu86State *s)
{
    uint32_t src = segoff_to_linear(string_src_seg(s), s->regs[REG_SI]);
    s->regs[REG_AX] = mem_read16(s, src);
    index_advance(s, REG_SI, 1);
}

/* ================================================================
 * SCASB/SCASW — Scan String (sets flags like SUB)
 * temp = AL/AX - mem[ES:DI], set flags, advance DI
 * ================================================================ */

static inline void exec_scasb(Emu86State *s)
{
    uint32_t dst_addr = segoff_to_linear(s->sregs[SREG_ES], s->regs[REG_DI]);
    uint32_t al = (uint8_t)s->regs[REG_AX];
    uint32_t mem_val = mem_read8(s, dst_addr);
    uint32_t result = al - mem_val;
    set_flags_sub(s, al, mem_val, result, 0);
    set_flags_szp(s, result, 0);
    index_advance(s, REG_DI, 0);
}

static inline void exec_scasw(Emu86State *s)
{
    uint32_t dst_addr = segoff_to_linear(s->sregs[SREG_ES], s->regs[REG_DI]);
    uint32_t ax = s->regs[REG_AX];
    uint32_t mem_val = mem_read16(s, dst_addr);
    uint32_t result = ax - mem_val;
    set_flags_sub(s, ax, mem_val, result, 1);
    set_flags_szp(s, result, 1);
    index_advance(s, REG_DI, 1);
}

/* ================================================================
 * REP-aware string operation dispatcher
 *
 * d->extra encodes the string op type (from TABLE_XLAT_SUBFUNCTION):
 *   In the original: xlat_id 17 = MOVS(0)/STOS(1)/LODS(2)
 *                    xlat_id 18 = CMPS(0)/SCAS(1)
 *
 * For this dispatcher, we use d->opcode directly:
 *   A4/A5 = MOVS, A6/A7 = CMPS, AA/AB = STOS, AC/AD = LODS, AE/AF = SCAS
 *   d->operand_width: 0=byte, 1=word
 * ================================================================ */

static inline void
exec_string_op(Emu86State *s, DecodeContext *d)
{
    uint8_t w = d->operand_width;
    uint8_t op = d->opcode & 0xFE; /* mask off width bit */
    int is_rep = s->rep_override_en > 0;
    int rep_mode = s->rep_mode; /* 0=REPNZ, 1=REPZ */

    /* Determine if this is a flag-testing string op (CMPS/SCAS) */
    int is_cmp = (op == 0xA6 || op == 0xAE);

    if (!is_rep) {
        /* Single iteration */
        switch (op) {
            case 0xA4: w ? exec_movsw(s) : exec_movsb(s); break;
            case 0xA6: w ? exec_cmpsw(s) : exec_cmpsb(s); break;
            case 0xAA: w ? exec_stosw(s) : exec_stosb(s); break;
            case 0xAC: w ? exec_lodsw(s) : exec_lodsb(s); break;
            case 0xAE: w ? exec_scasw(s) : exec_scasb(s); break;
        }
        return;
    }

    /* REP loop */
    while (s->regs[REG_CX] != 0) {
        switch (op) {
            case 0xA4: w ? exec_movsw(s) : exec_movsb(s); break;
            case 0xA6: w ? exec_cmpsw(s) : exec_cmpsb(s); break;
            case 0xAA: w ? exec_stosw(s) : exec_stosb(s); break;
            case 0xAC: w ? exec_lodsw(s) : exec_lodsb(s); break;
            case 0xAE: w ? exec_scasw(s) : exec_scasb(s); break;
        }
        s->regs[REG_CX]--;

        /* For CMPS/SCAS with REPZ/REPNZ, check termination condition */
        if (is_cmp && s->regs[REG_CX] > 0) {
            if (rep_mode == 1 && !get_flag(s, FLAG_ZF)) break;  /* REPZ: stop if ZF=0 */
            if (rep_mode == 0 && get_flag(s, FLAG_ZF)) break;   /* REPNZ: stop if ZF=1 */
        }
    }
}

#endif /* EMU86_OPCODES_STRING_H */
