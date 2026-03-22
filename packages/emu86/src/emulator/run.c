/*
 * run.c — Unity build for the emu86 emulator hot path.
 *
 * This file #includes all opcode headers so the compiler sees the entire
 * emulator as one translation unit and inlines everything aggressively.
 */

#include "run.h"
#include "state.h"
#include "platform.h"
#include "tables.h"
#include "decode.h"
#include "opcodes/helpers.h"
#include "opcodes/arithmetic.h"
#include "opcodes/logic.h"
#include "opcodes/shift.h"
#include "opcodes/transfer.h"
#include "opcodes/string.h"
#include "opcodes/control.h"
#include "opcodes/flags_io.h"

#include <string.h>

/* ================================================================
 * ALU immediate helper — handles cases 7, 8, and case 9 extra=8
 * Performs ALU operation on rm operand with an immediate source.
 * op: 0=ADD, 1=OR, 2=ADC, 3=SBB, 4=AND, 5=SUB, 6=XOR, 7=CMP, 8=MOV
 * ================================================================ */

static inline void
exec_alu_imm(Emu86State *s, DecodeContext *d, uint8_t op, uint16_t imm)
{
    uint32_t dest = read_rm(s, d);
    uint32_t src = imm & MASK(d->operand_width);
    uint32_t result;
    uint8_t w = d->operand_width;

    switch (op) {
    case 0: /* ADD */
        result = dest + src;
        set_flags_add(s, dest, src, result, w);
        set_flags_szp(s, result, w);
        write_rm(s, d, result & MASK(w));
        break;
    case 1: /* OR */
        result = dest | src;
        set_flags_logic(s, result, w);
        write_rm(s, d, result & MASK(w));
        break;
    case 2: /* ADC */ {
        uint32_t cf = get_flag(s, FLAG_CF);
        result = dest + src + cf;
        set_flags_add(s, dest, src, result, w);
        set_flags_szp(s, result, w);
        write_rm(s, d, result & MASK(w));
        break;
    }
    case 3: /* SBB */ {
        uint32_t cf = get_flag(s, FLAG_CF);
        result = dest - src - cf;
        set_flags_sub(s, dest, src, result, w);
        set_flags_szp(s, result, w);
        write_rm(s, d, result & MASK(w));
        break;
    }
    case 4: /* AND */
        result = dest & src;
        set_flags_logic(s, result, w);
        write_rm(s, d, result & MASK(w));
        break;
    case 5: /* SUB */
        result = dest - src;
        set_flags_sub(s, dest, src, result, w);
        set_flags_szp(s, result, w);
        write_rm(s, d, result & MASK(w));
        break;
    case 6: /* XOR */
        result = dest ^ src;
        set_flags_logic(s, result, w);
        write_rm(s, d, result & MASK(w));
        break;
    case 7: /* CMP */
        result = dest - src;
        set_flags_sub(s, dest, src, result, w);
        set_flags_szp(s, result, w);
        /* no writeback */
        break;
    case 8: /* MOV */
        write_rm(s, d, src);
        break;
    }
}

/* ================================================================
 * Instruction dispatch
 *
 * Maps xlat_id (from TABLE_XLAT_OPCODE) to exec functions.
 * See ORIGINAL-ANALYSIS.md Section D, Phase 2 for the complete mapping.
 * ================================================================ */

static inline void
execute_instruction(Emu86State *s, Emu86Platform *p,
                    Emu86Tables *t, DecodeContext *d)
{
    switch (d->xlat_id) {

    case 0: /* Conditional jumps (Jcc, 70-7F) */
        exec_jcc(s, d);
        break;

    case 1: /* MOV reg, imm (B0-BF) */
        /* Override operand_width: bit 3 of opcode distinguishes byte/word */
        d->operand_width = !!(d->opcode & 8);
        /* Fix inst_length for the overridden width */
        d->inst_length = t->data[TABLE_BASE_INST_SIZE][d->opcode]
                       + t->data[TABLE_I_W_SIZE][d->opcode] * (d->operand_width + 1);
        exec_mov_reg_imm(s, d);
        break;

    case 2: /* INC/DEC regs16 (40-4F) */
        d->operand_width = 1;
        d->mod = 3;
        d->rm = d->reg4bit;
        if (d->extra == 0)
            exec_inc(s, d);
        else
            exec_dec(s, d);
        break;

    case 3: /* PUSH regs16 (50-57) */
        exec_push_reg(s, d->reg4bit);
        break;

    case 4: /* POP regs16 (58-5F) */
        exec_pop_reg(s, d->reg4bit);
        break;

    case 5: /* INC/DEC/JMP/CALL/PUSH r/m (FE/FF group) */
        switch (d->reg) {
        case 0: exec_inc(s, d); break;
        case 1: exec_dec(s, d); break;
        case 2: exec_call_rm(s, d); break;
        case 3: exec_call_far_mem(s, d); break;
        case 4: exec_jmp_rm(s, d); break;
        case 5: exec_jmp_far_mem(s, d); break;
        case 6: exec_push_rm(s, d); break;
        }
        break;

    case 6: /* TEST/NOT/NEG/MUL/IMUL/DIV/IDIV (F6/F7 group) */
        switch (d->reg) {
        case 0: { /* TEST r/m, imm */
            uint32_t rm_val = read_rm(s, d);
            uint16_t imm = (uint16_t)d->data2 & MASK(d->operand_width);
            set_flags_logic(s, rm_val & imm, d->operand_width);
            d->inst_length += d->operand_width + 1;
            break;
        }
        case 2: exec_not(s, d); break;
        case 3: exec_neg(s, d); break;
        case 4: exec_mul(s, d); break;
        case 5: exec_imul(s, d); break;
        case 6:
            if (exec_div(s, d)) {
                s->ip += d->inst_length;
                pc_interrupt(s, 0);
                d->ip_changed = 1;
            }
            break;
        case 7:
            if (exec_idiv(s, d)) {
                s->ip += d->inst_length;
                pc_interrupt(s, 0);
                d->ip_changed = 1;
            }
            break;
        }
        break;

    case 7: /* ALU AL/AX, imm (04-05, 0C-0D, etc.) */
        d->mod = 3;
        d->rm = REG_AX;
        exec_alu_imm(s, d, d->extra, (uint16_t)d->data0);
        break;

    case 8: { /* ALU r/m, imm (80-83 group) */
        uint8_t sign_ext = d->direction | !d->operand_width;
        uint16_t imm;
        if (sign_ext)
            imm = (uint16_t)(int16_t)(int8_t)(d->data2 & 0xFF);
        else
            imm = (uint16_t)d->data2;
        d->inst_length += sign_ext ? 1 : 2;
        exec_alu_imm(s, d, d->reg, imm);
        break;
    }

    case 9: /* ALU/MOV reg, r/m (main ALU dispatch) */
        switch (d->extra) {
        case 0: exec_add(s, d); break;
        case 1: exec_or(s, d); break;
        case 2: exec_adc(s, d); break;
        case 3: exec_sbb(s, d); break;
        case 4: exec_and(s, d); break;
        case 5: exec_sub(s, d); break;
        case 6: exec_xor(s, d); break;
        case 7: exec_cmp(s, d); break;
        case 8: exec_mov_rm_reg(s, d); break;
        }
        break;

    case 10: /* MOV sreg / POP r/m / LEA */
        if (!d->operand_width) {
            /* MOV involving segment registers (8C/8E) */
            d->operand_width = 1;
            if (d->direction)
                exec_mov_sreg_rm(s, d);
            else
                exec_mov_rm_sreg(s, d);
        } else if (!d->direction) {
            /* LEA (8D) — compute offset without segment */
            if (d->mod < 3) {
                uint8_t tbase = (d->mod == 0) ? 4 : 0;
                uint8_t seg_reg_idx = t->data[tbase + 3][d->rm];
                uint16_t seg;
                if (s->seg_override_en)
                    seg = s->sregs[s->seg_override];
                else
                    seg = read_table_sreg(s, seg_reg_idx);
                write_reg16(s, d->reg,
                    (uint16_t)(d->rm_addr - ((uint32_t)seg << 4)));
            }
        } else {
            /* POP r/m (8F) */
            exec_pop_rm(s, d);
        }
        break;

    case 11: /* MOV AL/AX, [moffs] / MOV [moffs], AL/AX (A0-A3) */
        if (d->direction)
            exec_mov_moffs_al(s, d);
        else
            exec_mov_al_moffs(s, d);
        break;

    case 12: { /* Shifts and rotates (C0/C1/D0/D1/D2/D3) */
        uint8_t count;
        if (d->extra) {
            /* Immediate shift count (C0/C1, 80186+) */
            count = (uint8_t)(d->data1 & 0xFF);
            d->inst_length++;
        } else if (d->direction) {
            /* Shift by CL (D2/D3) */
            count = (uint8_t)s->regs[REG_CX]; /* CL */
        } else {
            /* Shift by 1 (D0/D1) */
            count = 1;
        }
        exec_shift_rotate(s, d, count);
        break;
    }

    case 13: /* LOOPxx / JCXZ (E0-E3) */
        switch (d->reg4bit) {
        case 0: exec_loopnz(s, d); break;
        case 1: exec_loopz(s, d); break;
        case 2: exec_loop(s, d); break;
        case 3: exec_jcxz(s, d); break;
        }
        break;

    case 14: { /* JMP / CALL short/near/far (E8-EB) */
        /* Original does: reg_ip += 3 - i_d, then adds offset.
         * Since base_size=0 in the tables, we must compute the
         * instruction's base length ourselves: 3 - i_d */
        uint8_t base = 3 - d->direction;
        d->inst_length = base;
        if (d->direction && d->operand_width) {
            /* EB: JMP short */
            exec_jmp_short(s, d);
        } else if (!d->direction && !d->operand_width) {
            /* E8: CALL near */
            exec_call_near(s, d);
        } else if (!d->direction && d->operand_width) {
            /* E9: JMP near */
            exec_jmp_near(s, d);
        } else {
            /* EA: JMP far */
            exec_jmp_far(s, d);
        }
        break;
    }

    case 15: /* TEST reg, r/m (84/85) */
        exec_test(s, d);
        break;

    case 16: /* XCHG AX, regs16 (90-97) */
        d->operand_width = 1;
        exec_xchg_ax_reg(s, d->reg4bit);
        break;

    case 17: /* MOVS/STOS/LODS (A4/A5/AA/AB/AC/AD) */
        exec_string_op(s, d);
        break;

    case 18: /* CMPS/SCAS (A6/A7/AE/AF) */
        exec_string_op(s, d);
        break;

    case 19: { /* RET/RETF/IRET */
        uint8_t has_imm = !d->operand_width;
        if (d->extra & 2) {
            exec_iret(s, d);
        } else if (d->extra) {
            if (has_imm) exec_retf_imm(s, d);
            else exec_retf(s, d);
        } else {
            if (has_imm) exec_ret_near_imm(s, d);
            else exec_ret_near(s, d);
        }
        break;
    }

    case 20: /* MOV r/m, imm (C6/C7) */
        exec_mov_rm_imm(s, d);
        break;

    case 21: { /* IN AL/AX, port */
        uint16_t port = d->extra ? s->regs[REG_DX] : (uint8_t)d->data0;
        /* Simulate PIC/PIT/CGA reads (matching original) */
        s->io_ports[0x20] = 0; /* PIC EOI */
        s->io_ports[0x42] = --s->io_ports[0x40]; /* PIT read placeholder */
        s->io_ports[0x3DA] ^= 9; /* CGA refresh toggle */
        if (port == 0x60) s->io_ports[0x64] = 0; /* Scancode read flag */
        if (d->operand_width)
            exec_in_ax_dx(s); /* uses DX or we fake it */
        else
            s->regs[REG_AX] = (s->regs[REG_AX] & 0xFF00) | s->io_ports[port];
        if (d->operand_width) {
            s->regs[REG_AX] = s->io_ports[port] | ((uint16_t)s->io_ports[port + 1] << 8);
        }
        break;
    }

    case 22: { /* OUT port, AL/AX */
        uint16_t port = d->extra ? s->regs[REG_DX] : (uint8_t)d->data0;
        s->io_ports[port] = (uint8_t)s->regs[REG_AX];
        if (d->operand_width)
            s->io_ports[port + 1] = (uint8_t)(s->regs[REG_AX] >> 8);
        io_port_write_hook(s, port, s->io_ports[port]);
        /* Speaker control */
        if (port == 0x61)
            s->spkr_en |= (uint8_t)s->regs[REG_AX] & 3;
        /* PIT rate programming */
        if ((port == 0x40 || port == 0x42) && (s->io_ports[0x43] & 6))
            s->mem[0x469 + port - (s->pit_lobyte_pending ^= 1)] = (uint8_t)s->regs[REG_AX];
        break;
    }

    case 23: /* REP/REPNZ/REPZ prefix (F2/F3) */
        exec_rep_prefix(s, d->operand_width);
        if (s->seg_override_en)
            s->seg_override_en++;
        break;

    case 24: /* NOP / XCHG reg, r/m (86/87) */
        exec_xchg(s, d);
        break;

    case 25: /* PUSH segment register */
        exec_push_sreg(s, d->extra);
        break;

    case 26: /* POP segment register */
        exec_pop_sreg(s, d->extra);
        break;

    case 27: /* Segment override prefixes (26/2E/36/3E) */
        /* extra from table is 8=ES, 9=CS, 10=SS, 11=DS (original numbering) */
        exec_segment_override(s, d->extra - 8);
        if (s->rep_override_en)
            s->rep_override_en++;
        break;

    case 28: /* DAA/DAS */
        if (d->extra)
            exec_das(s);
        else
            exec_daa(s);
        break;

    case 29: /* AAA/AAS */
        if (d->extra)
            exec_aas(s);
        else
            exec_aaa(s);
        break;

    case 30: /* CBW */
        exec_cbw(s);
        break;

    case 31: /* CWD */
        exec_cwd(s);
        break;

    case 32: /* CALL far imm (9A) */
        exec_call_far(s, d);
        break;

    case 33: /* PUSHF */
        exec_pushf(s);
        break;

    case 34: /* POPF */
        exec_popf(s);
        break;

    case 35: /* SAHF */
        exec_sahf(s);
        break;

    case 36: /* LAHF */
        exec_lahf(s);
        break;

    case 37: /* LES/LDS */
        d->operand_width = 1;
        d->direction = 1;
        /* Re-decode for the far pointer load */
        if ((d->opcode & 1) == 0)
            exec_les(s, d);
        else
            exec_lds(s, d);
        break;

    case 38: /* INT 3 (CC) */
        d->inst_length = 1; /* single-byte INT 3 */
        exec_int3(s, d);
        break;

    case 39: /* INT imm8 (CD) */
        d->inst_length = 2; /* CD xx */
        exec_int(s, d, (uint8_t)(d->data0 & 0xFF));
        break;

    case 40: /* INTO (CE) */
        d->inst_length = 1;
        exec_into(s, d);
        break;

    case 41: /* AAM */
        if ((d->data0 & 0xFF) == 0) {
            s->ip += d->inst_length;
            pc_interrupt(s, 0);
            d->ip_changed = 1;
        } else {
            exec_aam(s, (uint8_t)(d->data0 & 0xFF));
        }
        break;

    case 42: /* AAD */
        d->operand_width = 0;
        exec_aad(s, (uint8_t)(d->data0 & 0xFF));
        break;

    case 43: /* SALC (D6) */
        exec_salc(s);
        break;

    case 44: /* XLAT (D7) */
        exec_xlat(s);
        break;

    case 45: /* CMC (F5) */
        exec_cmc(s);
        break;

    case 46: { /* CLC/STC/CLI/STI/CLD/STD (F8-FD) */
        /* extra encodes: extra/2 = flag bit position in original scheme,
         * extra & 1 = value to set. The original does regs8[extra/2] = extra&1.
         * We map from the original's flag indices to our FLAG_xx constants. */
        static const uint16_t flag_map[] = {
            [40] = FLAG_CF, [41] = FLAG_PF, [42] = FLAG_AF, [43] = FLAG_ZF,
            [44] = FLAG_SF, [45] = FLAG_TF, [46] = FLAG_IF, [47] = FLAG_DF,
            [48] = FLAG_OF
        };
        uint8_t idx = d->extra / 2;
        uint8_t val = d->extra & 1;
        if (idx >= 40 && idx <= 48)
            update_flag(s, flag_map[idx], val);
        break;
    }

    case 47: { /* TEST AL/AX, imm (A8/A9) */
        uint32_t acc = d->operand_width ? s->regs[REG_AX]
                                        : (uint8_t)s->regs[REG_AX];
        uint32_t imm = (uint16_t)d->data0 & MASK(d->operand_width);
        set_flags_logic(s, acc & imm, d->operand_width);
        break;
    }

    case 48: /* Emulator-specific 0F xx BIOS calls */
        switch ((uint8_t)(d->data0 & 0xFF)) {
        case 0: exec_bios_putchar(s, p); break;
        case 1: exec_bios_get_rtc(s, p); break;
        case 2: exec_bios_disk_read(s, p); break;
        case 3: exec_bios_disk_write(s, p); break;
        }
        break;

    case 53: /* HLT (F4) and LOCK prefix (F0) */
        if (d->opcode == 0xF4)
            exec_hlt(s);
        /* LOCK (F0) = no-op on 8086 */
        break;

    default:
        /* Unknown xlat_id — ignore for now */
        break;
    }
}

/* ================================================================
 * emu86_run — batch execution loop
 * ================================================================ */

void emu86_run(Emu86State *s, Emu86Platform *p,
               Emu86Tables *t, uint32_t budget,
               Emu86YieldInfo *yield)
{
    uint32_t cycles = 0;
    DecodeContext d;

    while (cycles < budget) {
        /* 1. Check exit condition: CS:IP == 0000:0000 */
        if (s->sregs[SREG_CS] == 0 && s->ip == 0) {
            yield->reason = EMU86_YIELD_EXIT;
            yield->cycles_used = cycles;
            return;
        }

        /* 2. Handle pending hardware interrupts */
        if (s->int_pending && (s->flags & FLAG_IF) &&
            s->seg_override_en == 0 && s->rep_override_en == 0) {
            pc_interrupt(s, s->int_vector);
            s->int_pending = 0;
        }

        /* 3. Check HLT state */
        if (s->halted) {
            yield->reason = EMU86_YIELD_HALTED;
            yield->cycles_used = cycles;
            return;
        }

        /* 4. Decode instruction at CS:IP */
        decode_instruction(s, t, &d);

        /* 5. Decrement prefix override counters */
        if (s->seg_override_en)
            s->seg_override_en--;
        if (s->rep_override_en)
            s->rep_override_en--;

        /* 6. Execute */
        execute_instruction(s, p, t, &d);

        /* 7. Advance IP */
        if (!d.ip_changed)
            s->ip += d.inst_length;

        /* 8. Cycle accounting (approximate: 4 per instruction) */
        cycles += 4;
        s->inst_count++;

        /* 9. Trap flag handling */
        if (s->trap_flag) {
            pc_interrupt(s, 1);
            s->trap_flag = 0;
        }
        if (s->flags & FLAG_TF)
            s->trap_flag = 1;

        /* 10. Timer polling (~every 20000 instructions) */
        if ((s->inst_count & 0x4FFF) == 0)
            s->int8_asap = 1;

        /* Service pending timer interrupt */
        if (s->int8_asap && (s->flags & FLAG_IF) &&
            s->seg_override_en == 0 && s->rep_override_en == 0) {
            pc_interrupt(s, 0xA);
            s->int8_asap = 0;

            /* Poll keyboard */
            if (p->console_in.buf) {
                uint8_t key;
                if (ringbuf_read(&p->console_in, &key) == 0) {
                    s->mem[0x4A6] = key;
                    if (key == 0x1B) s->int8_asap = 1;
                    pc_interrupt(s, 7);
                }
            }
        }

        /* 11. Check console output buffer fill */
        if (p->console_out.buf &&
            ringbuf_available(&p->console_out) > (p->console_out.size * 3 / 4)) {
            yield->reason = EMU86_YIELD_IO_NEEDED;
            yield->io_type = 0;
            yield->cycles_used = cycles;
            return;
        }
    }

    yield->reason = EMU86_YIELD_BUDGET;
    yield->cycles_used = cycles;
}

/* ================================================================
 * emu86_step_single — execute exactly one instruction
 * ================================================================ */

int emu86_step_single(Emu86State *s, Emu86Platform *p, Emu86Tables *t)
{
    DecodeContext d;

    /* Decode */
    decode_instruction(s, t, &d);

    /* Decrement prefix counters */
    if (s->seg_override_en)
        s->seg_override_en--;
    if (s->rep_override_en)
        s->rep_override_en--;

    /* Execute */
    execute_instruction(s, p, t, &d);

    /* Advance IP */
    if (!d.ip_changed)
        s->ip += d.inst_length;

    /* Cycle accounting */
    s->inst_count++;

    /* Trap flag */
    if (s->trap_flag) {
        pc_interrupt(s, 1);
        s->trap_flag = 0;
    }
    if (s->flags & FLAG_TF)
        s->trap_flag = 1;

    return 4; /* approximate cycles */
}
