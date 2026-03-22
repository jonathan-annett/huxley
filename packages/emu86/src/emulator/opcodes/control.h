#ifndef EMU86_OPCODES_CONTROL_H
#define EMU86_OPCODES_CONTROL_H

#include <stdint.h>
#include "../state.h"
#include "../decode.h"
#include "helpers.h"
#include "transfer.h" /* stack_push, stack_pop, flags_pack, flags_unpack */

/* ================================================================
 * Interrupt helper
 * Push FLAGS, CS, IP; clear IF and TF; load CS:IP from IVT
 * ================================================================ */

static inline void
pc_interrupt(Emu86State *s, uint8_t vector_num)
{
    stack_push(s, flags_pack(s));
    stack_push(s, s->sregs[SREG_CS]);
    stack_push(s, s->ip);
    clear_flag(s, FLAG_IF);
    clear_flag(s, FLAG_TF);
    s->ip = mem_read16(s, (uint32_t)vector_num * 4);
    s->sregs[SREG_CS] = mem_read16(s, (uint32_t)vector_num * 4 + 2);
}

/* ================================================================
 * Conditional jump evaluation
 * condition_code: 0-15 (low nibble of opcode 70-7F)
 * ================================================================ */

static inline int
eval_condition(const Emu86State *s, uint8_t cc)
{
    int result;
    switch (cc & 0x0E) { /* even condition code (positive test) */
        case 0x00: result = get_flag(s, FLAG_OF); break;                     /* JO */
        case 0x02: result = get_flag(s, FLAG_CF); break;                     /* JB */
        case 0x04: result = get_flag(s, FLAG_ZF); break;                     /* JZ */
        case 0x06: result = get_flag(s, FLAG_CF) || get_flag(s, FLAG_ZF); break; /* JBE */
        case 0x08: result = get_flag(s, FLAG_SF); break;                     /* JS */
        case 0x0A: result = get_flag(s, FLAG_PF); break;                     /* JP */
        case 0x0C: result = get_flag(s, FLAG_SF) != get_flag(s, FLAG_OF); break; /* JL */
        case 0x0E: result = get_flag(s, FLAG_ZF) ||
                            (get_flag(s, FLAG_SF) != get_flag(s, FLAG_OF)); break; /* JLE */
        default: result = 0;
    }
    /* Odd opcodes invert the condition (JNO, JNB, JNZ, etc.) */
    return (cc & 1) ? !result : result;
}

/* ================================================================
 * JMP instructions — no flags affected
 * ================================================================ */

/* JMP short rel8 (EB) */
static inline void exec_jmp_short(Emu86State *s, DecodeContext *d)
{
    s->ip = s->ip + d->inst_length + (int8_t)(d->data0 & 0xFF);
    d->ip_changed = 1;
}

/* JMP near rel16 (E9) */
static inline void exec_jmp_near(Emu86State *s, DecodeContext *d)
{
    s->ip = s->ip + d->inst_length + (int16_t)(uint16_t)d->data0;
    d->ip_changed = 1;
}

/* JMP far ptr16:16 (EA) */
static inline void exec_jmp_far(Emu86State *s, DecodeContext *d)
{
    s->ip = (uint16_t)d->data0;
    s->sregs[SREG_CS] = (uint16_t)d->data2;
    d->ip_changed = 1;
}

/* JMP near r/m16 (FF/4) */
static inline void exec_jmp_rm(Emu86State *s, DecodeContext *d)
{
    s->ip = read_rm16(s, d);
    d->ip_changed = 1;
}

/* JMP far m16:16 (FF/5) */
static inline void exec_jmp_far_mem(Emu86State *s, DecodeContext *d)
{
    s->ip = mem_read16(s, d->rm_addr);
    s->sregs[SREG_CS] = mem_read16(s, d->rm_addr + 2);
    d->ip_changed = 1;
}

/* ================================================================
 * Conditional jumps (Jcc) 70-7F — short rel8
 * ================================================================ */

static inline void exec_jcc(Emu86State *s, DecodeContext *d)
{
    uint8_t cc = d->opcode & 0x0F;
    if (eval_condition(s, cc)) {
        s->ip = s->ip + d->inst_length + (int8_t)(d->data0 & 0xFF);
        d->ip_changed = 1;
    }
    /* If not taken, normal IP advance applies (ip_changed stays 0) */
}

/* ================================================================
 * CALL instructions — no flags affected
 * ================================================================ */

/* CALL near rel16 (E8) */
static inline void exec_call_near(Emu86State *s, DecodeContext *d)
{
    uint16_t return_ip = s->ip + d->inst_length;
    stack_push(s, return_ip);
    s->ip = return_ip + (int16_t)(uint16_t)d->data0;
    d->ip_changed = 1;
}

/* CALL far ptr16:16 (9A) */
static inline void exec_call_far(Emu86State *s, DecodeContext *d)
{
    uint16_t return_ip = s->ip + d->inst_length;
    stack_push(s, s->sregs[SREG_CS]);
    stack_push(s, return_ip);
    s->ip = (uint16_t)d->data0;
    s->sregs[SREG_CS] = (uint16_t)d->data2;
    d->ip_changed = 1;
}

/* CALL near r/m16 (FF/2) */
static inline void exec_call_rm(Emu86State *s, DecodeContext *d)
{
    uint16_t return_ip = s->ip + d->inst_length;
    stack_push(s, return_ip);
    s->ip = read_rm16(s, d);
    d->ip_changed = 1;
}

/* CALL far m16:16 (FF/3) */
static inline void exec_call_far_mem(Emu86State *s, DecodeContext *d)
{
    uint16_t return_ip = s->ip + d->inst_length;
    stack_push(s, s->sregs[SREG_CS]);
    stack_push(s, return_ip);
    s->ip = mem_read16(s, d->rm_addr);
    s->sregs[SREG_CS] = mem_read16(s, d->rm_addr + 2);
    d->ip_changed = 1;
}

/* ================================================================
 * RET instructions — no flags affected
 * ================================================================ */

static inline void exec_ret_near(Emu86State *s, DecodeContext *d)
{
    s->ip = stack_pop(s);
    d->ip_changed = 1;
}

static inline void exec_ret_near_imm(Emu86State *s, DecodeContext *d)
{
    s->ip = stack_pop(s);
    s->regs[REG_SP] += (uint16_t)d->data0;
    d->ip_changed = 1;
}

static inline void exec_retf(Emu86State *s, DecodeContext *d)
{
    s->ip = stack_pop(s);
    s->sregs[SREG_CS] = stack_pop(s);
    d->ip_changed = 1;
}

static inline void exec_retf_imm(Emu86State *s, DecodeContext *d)
{
    s->ip = stack_pop(s);
    s->sregs[SREG_CS] = stack_pop(s);
    s->regs[REG_SP] += (uint16_t)d->data0;
    d->ip_changed = 1;
}

/* ================================================================
 * INT / IRET
 * ================================================================ */

static inline void exec_int(Emu86State *s, DecodeContext *d, uint8_t vector)
{
    s->ip += d->inst_length; /* push IP of NEXT instruction */
    pc_interrupt(s, vector);
    d->ip_changed = 1;
}

static inline void exec_int3(Emu86State *s, DecodeContext *d)
{
    exec_int(s, d, 3);
}

static inline void exec_into(Emu86State *s, DecodeContext *d)
{
    if (get_flag(s, FLAG_OF))
        exec_int(s, d, 4);
    /* If OF=0, do nothing — normal IP advance */
}

static inline void exec_iret(Emu86State *s, DecodeContext *d)
{
    s->ip = stack_pop(s);
    s->sregs[SREG_CS] = stack_pop(s);
    flags_unpack(s, stack_pop(s));
    d->ip_changed = 1;
}

/* ================================================================
 * LOOP / LOOPZ / LOOPNZ / JCXZ — no flags affected
 * ================================================================ */

static inline void exec_loop(Emu86State *s, DecodeContext *d)
{
    s->regs[REG_CX]--;
    if (s->regs[REG_CX] != 0) {
        s->ip = s->ip + d->inst_length + (int8_t)(d->data0 & 0xFF);
        d->ip_changed = 1;
    }
}

static inline void exec_loopz(Emu86State *s, DecodeContext *d)
{
    s->regs[REG_CX]--;
    if (s->regs[REG_CX] != 0 && get_flag(s, FLAG_ZF)) {
        s->ip = s->ip + d->inst_length + (int8_t)(d->data0 & 0xFF);
        d->ip_changed = 1;
    }
}

static inline void exec_loopnz(Emu86State *s, DecodeContext *d)
{
    s->regs[REG_CX]--;
    if (s->regs[REG_CX] != 0 && !get_flag(s, FLAG_ZF)) {
        s->ip = s->ip + d->inst_length + (int8_t)(d->data0 & 0xFF);
        d->ip_changed = 1;
    }
}

static inline void exec_jcxz(Emu86State *s, DecodeContext *d)
{
    if (s->regs[REG_CX] == 0) {
        s->ip = s->ip + d->inst_length + (int8_t)(d->data0 & 0xFF);
        d->ip_changed = 1;
    }
}

#endif /* EMU86_OPCODES_CONTROL_H */
