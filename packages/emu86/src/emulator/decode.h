#ifndef EMU86_DECODE_H
#define EMU86_DECODE_H

#include <stdint.h>
#include <string.h>
#include "state.h"
#include "tables.h"

/*
 * DecodeContext — transient instruction decode state.
 *
 * Lives on the stack during instruction execution. Never snapshotted.
 * These correspond to the scratch/temporary globals in the original
 * 8086tiny, plus the ModRM decode fields.
 */
typedef struct {
    /* --- Raw / translated opcode --- */
    uint8_t  opcode;         /* raw opcode byte */
    uint8_t  xlat_id;        /* translated opcode (switch case number) */
    uint8_t  extra;          /* sub-function index (TABLE_XLAT_SUBFUNCTION) */
    uint8_t  has_modrm;      /* whether this opcode uses ModRM */

    /* --- Instruction fields --- */
    uint8_t  operand_width;  /* 0 = byte, 1 = word (i_w) */
    uint8_t  direction;      /* 0 = rm←reg, 1 = reg←rm (i_d) */
    uint8_t  mod;            /* MOD field from ModRM */
    uint8_t  reg;            /* REG field from ModRM */

    uint8_t  rm;             /* R/M field from ModRM */
    uint8_t  reg4bit;        /* low 3 bits of raw opcode (register encoding) */
    uint8_t  set_flags_type; /* bitfield: which flags to update */
    uint8_t  inst_length;    /* total bytes consumed (computed) */

    /* --- Decoded data bytes (matching original semantics) --- */
    uint32_t data0;          /* 16-bit LE read at opcode+1, sign-extended to 32 */
    uint32_t data1;          /* 16-bit LE read at opcode+2, sign-extended to 32 */
    uint32_t data2;          /* 16-bit LE read at opcode+3 (may be adjusted) */
    uint16_t immediate;      /* immediate value (where applicable) */
    uint16_t displacement;   /* displacement value (where applicable) */

    /* --- Effective addresses --- */
    uint32_t rm_addr;        /* for mod<3: linear mem addr; for mod==3: reg index */
    uint32_t op_to_addr;     /* destination operand address */
    uint32_t op_from_addr;   /* source operand address (reg index) */

    /* --- ALU scratch (used by execution, populated here as 0) --- */
    uint32_t op_source;
    uint32_t op_dest;
    int32_t  op_result;

    /* --- General scratch --- */
    uint32_t scratch_uint;
    uint32_t scratch2_uint;
    int32_t  scratch_int;
    uint8_t  scratch_uchar;

} DecodeContext;

/* ================================================================
 * Memory access helpers
 * ================================================================ */

/* Convert segment:offset to linear address */
static inline uint32_t __attribute__((always_inline))
segoff_to_linear(uint16_t segment, uint16_t offset)
{
    return ((uint32_t)segment << 4) + offset;
}

/* Read 8-bit value from emulated memory */
static inline uint8_t __attribute__((always_inline))
mem_read8(const Emu86State *s, uint32_t addr)
{
    return s->mem[addr];
}

/* Write 8-bit value to emulated memory */
static inline void __attribute__((always_inline))
mem_write8(Emu86State *s, uint32_t addr, uint8_t val)
{
    s->mem[addr] = val;
}

/* Read 16-bit little-endian value from emulated memory */
static inline uint16_t __attribute__((always_inline))
mem_read16(const Emu86State *s, uint32_t addr)
{
    return (uint16_t)s->mem[addr] | ((uint16_t)s->mem[addr + 1] << 8);
}

/* Write 16-bit little-endian value to emulated memory */
static inline void __attribute__((always_inline))
mem_write16(Emu86State *s, uint32_t addr, uint16_t val)
{
    s->mem[addr] = (uint8_t)val;
    s->mem[addr + 1] = (uint8_t)(val >> 8);
}

/* ================================================================
 * Register access helpers
 *
 * 8-bit register index mapping (matches 8086 encoding):
 *   0=AL, 1=CL, 2=DL, 3=BL  (low byte of AX,CX,DX,BX)
 *   4=AH, 5=CH, 6=DH, 7=BH  (high byte of AX,CX,DX,BX)
 * ================================================================ */

static inline uint8_t __attribute__((always_inline))
read_reg8(const Emu86State *s, uint8_t index)
{
    if (index < 4)
        return (uint8_t)s->regs[index];           /* AL, CL, DL, BL */
    else
        return (uint8_t)(s->regs[index - 4] >> 8); /* AH, CH, DH, BH */
}

static inline void __attribute__((always_inline))
write_reg8(Emu86State *s, uint8_t index, uint8_t val)
{
    if (index < 4)
        s->regs[index] = (s->regs[index] & 0xFF00) | val;
    else
        s->regs[index - 4] = (s->regs[index - 4] & 0x00FF) | ((uint16_t)val << 8);
}

static inline uint16_t __attribute__((always_inline))
read_reg16(const Emu86State *s, uint8_t index)
{
    return s->regs[index];
}

static inline void __attribute__((always_inline))
write_reg16(Emu86State *s, uint8_t index, uint16_t val)
{
    s->regs[index] = val;
}

static inline uint16_t __attribute__((always_inline))
read_sreg(const Emu86State *s, uint8_t index)
{
    return s->sregs[index];
}

static inline void __attribute__((always_inline))
write_sreg(Emu86State *s, uint8_t index, uint16_t val)
{
    s->sregs[index] = val;
}

/* ================================================================
 * Operand access helpers (use decode context)
 *
 * read_rm / write_rm: access the R/M operand.
 *   - mod < 3: memory operand at rm_addr
 *   - mod == 3: register operand (rm is register index)
 *
 * read_reg / write_reg: access the REG field operand (always a register).
 * ================================================================ */

static inline uint8_t
read_rm8(const Emu86State *s, const DecodeContext *d)
{
    if (d->mod == 3)
        return read_reg8(s, d->rm);
    return mem_read8(s, d->rm_addr);
}

static inline uint16_t
read_rm16(const Emu86State *s, const DecodeContext *d)
{
    if (d->mod == 3)
        return read_reg16(s, d->rm);
    return mem_read16(s, d->rm_addr);
}

static inline void
write_rm8(Emu86State *s, const DecodeContext *d, uint8_t val)
{
    if (d->mod == 3)
        write_reg8(s, d->rm, val);
    else
        mem_write8(s, d->rm_addr, val);
}

static inline void
write_rm16(Emu86State *s, const DecodeContext *d, uint16_t val)
{
    if (d->mod == 3)
        write_reg16(s, d->rm, val);
    else
        mem_write16(s, d->rm_addr, val);
}

/* Generic dispatchers based on operand_width */
static inline uint16_t
read_rm(const Emu86State *s, const DecodeContext *d)
{
    return d->operand_width ? read_rm16(s, d) : read_rm8(s, d);
}

static inline void
write_rm(Emu86State *s, const DecodeContext *d, uint16_t val)
{
    if (d->operand_width)
        write_rm16(s, d, val);
    else
        write_rm8(s, d, (uint8_t)val);
}

/* REG field operand (always a register) */
static inline uint16_t
read_reg(const Emu86State *s, const DecodeContext *d)
{
    return d->operand_width ? read_reg16(s, d->reg) : read_reg8(s, d->reg);
}

static inline void
write_reg(Emu86State *s, const DecodeContext *d, uint16_t val)
{
    if (d->operand_width)
        write_reg16(s, d->reg, val);
    else
        write_reg8(s, d->reg, (uint8_t)val);
}

/* ================================================================
 * Internal helpers for EA calculation
 * ================================================================ */

/*
 * Read a general register by table index (0-7 = AX-DI, 12 = ZERO).
 * Tables 0-7 in the BIOS encode register indices in the original's
 * numbering where REG_ZERO=12 means "no register / zero".
 */
static inline uint16_t __attribute__((always_inline))
read_table_reg(const Emu86State *s, uint8_t table_idx)
{
    if (table_idx < 8)
        return s->regs[table_idx];
    return 0; /* REG_ZERO (12) or any unused index */
}

/*
 * Get segment register value from a table index.
 * Tables use original numbering: 8=ES, 9=CS, 10=SS, 11=DS.
 */
static inline uint16_t __attribute__((always_inline))
read_table_sreg(const Emu86State *s, uint8_t table_idx)
{
    if (table_idx >= 8 && table_idx < 12)
        return s->sregs[table_idx - 8];
    return 0;
}

/*
 * Compute the register address offset (for op_from_addr / op_to_addr).
 * Matches the original GET_REG_ADDR but returns a register index
 * instead of a memory offset.
 *
 * For 16-bit: just the register index (0-7).
 * For 8-bit: the 8-bit register index (0-7, with the AL/AH mapping).
 *
 * This is the register index as used by read_reg8/write_reg8 or
 * read_reg16/write_reg16.
 */
static inline uint8_t __attribute__((always_inline))
get_reg_index(uint8_t reg_id, uint8_t operand_width)
{
    (void)operand_width;
    return reg_id; /* Same index for both 8-bit and 16-bit modes */
}

/* ================================================================
 * Main decode function
 *
 * Populates DecodeContext from the instruction at CS:IP.
 * Does NOT modify Emu86State (no IP advancement, no override decrement).
 * ================================================================ */

static inline void
decode_instruction(const Emu86State *state,
                   const Emu86Tables *tables,
                   DecodeContext *d)
{
    /* Zero the context */
    memset(d, 0, sizeof(DecodeContext));

    /* Compute linear address of instruction */
    uint32_t cs_base = segoff_to_linear(state->sregs[SREG_CS], state->ip);
    const uint8_t *stream = state->mem + cs_base;

    /* 1. Fetch and translate the opcode byte */
    d->opcode = stream[0];
    d->xlat_id = tables->data[TABLE_XLAT_OPCODE][d->opcode];
    d->extra = tables->data[TABLE_XLAT_SUBFUNCTION][d->opcode];
    d->set_flags_type = tables->data[TABLE_STD_FLAGS][d->opcode];
    d->has_modrm = tables->data[TABLE_I_MOD_SIZE][d->opcode];

    /* 2. Extract encoding bits from raw opcode */
    d->reg4bit = d->opcode & 7;
    d->operand_width = d->reg4bit & 1;       /* i_w */
    d->direction = (d->reg4bit >> 1) & 1;    /* i_d */

    /* 3. Read data bytes (16-bit LE reads, sign-extended to 32-bit) */
    d->data0 = (uint32_t)(int16_t)(stream[1] | ((uint16_t)stream[2] << 8));
    d->data1 = (uint32_t)(int16_t)(stream[2] | ((uint16_t)stream[3] << 8));
    d->data2 = (uint32_t)(int16_t)(stream[3] | ((uint16_t)stream[4] << 8));

    /* 4. ModRM decode (if this opcode uses it) */
    if (d->has_modrm) {
        uint8_t modrm_byte = (uint8_t)(d->data0 & 0xFF);
        d->mod = modrm_byte >> 6;
        d->rm  = modrm_byte & 7;
        d->reg = (modrm_byte >> 3) & 7;

        /*
         * Adjust data1/data2 based on mod and rm, matching the original:
         *   - mod==0, rm==6 or mod==2: data2 reads from offset +4
         *   - mod==0, rm!=6: data2 = data1 (immediate follows ModRM directly)
         *   - mod==1: data1 sign-extended from 8 bits
         */
        if ((!d->mod && d->rm == 6) || (d->mod == 2))
            d->data2 = (uint32_t)(int16_t)(stream[4] | ((uint16_t)stream[5] << 8));
        else if (d->mod != 1)
            d->data2 = d->data1;
        else /* mod == 1: sign-extend displacement from byte */
            d->data1 = (uint32_t)(int32_t)(int8_t)(d->data1 & 0xFF);

        /* 5. Resolve effective address */
        if (d->mod < 3) {
            /* Memory operand: compute linear address using BIOS tables.
             *
             * Table set selection (matches original scratch2_uint = 4 * !i_mod):
             *   mod == 0 → tables 4,5,6,7
             *   mod != 0 → tables 0,1,2,3
             */
            uint8_t tbase = (d->mod == 0) ? 4 : 0;

            uint8_t base_reg_idx  = tables->data[tbase + 0][d->rm];
            uint8_t index_reg_idx = tables->data[tbase + 1][d->rm];
            uint8_t disp_mult     = tables->data[tbase + 2][d->rm];
            uint8_t seg_reg_idx   = tables->data[tbase + 3][d->rm];

            /* Effective address offset (16-bit, wraps) */
            uint16_t offset = read_table_reg(state, base_reg_idx)
                            + read_table_reg(state, index_reg_idx)
                            + disp_mult * (uint16_t)d->data1;

            /* Segment: use override if active, otherwise default from table */
            uint16_t segment;
            if (state->seg_override_en)
                segment = state->sregs[state->seg_override];
            else
                segment = read_table_sreg(state, seg_reg_idx);

            d->rm_addr = segoff_to_linear(segment, offset);
        } else {
            /* Register operand: rm_addr stores the register index */
            d->rm_addr = d->rm;
        }

        /* Set op_to_addr (R/M) and op_from_addr (REG), swap if direction=1 */
        d->op_to_addr = d->rm_addr;
        d->op_from_addr = d->reg;  /* always a register index */

        if (d->direction) {
            uint32_t tmp = d->op_from_addr;
            d->op_from_addr = d->op_to_addr;
            d->op_to_addr = tmp;
        }
    }

    /* 6. Compute instruction length */
    {
        uint8_t base_size = tables->data[TABLE_BASE_INST_SIZE][d->opcode];
        uint8_t iw_size   = tables->data[TABLE_I_W_SIZE][d->opcode];

        /* ModRM displacement contribution */
        uint8_t modrm_disp = 0;
        if (d->has_modrm) {
            /* mod * (mod != 3) gives 1 for mod=1, 2 for mod=2, 0 for mod=0,3 */
            modrm_disp = d->mod * (d->mod != 3);
            /* Special case: mod=0, rm=6 → direct 16-bit address = 2 extra bytes */
            if (!d->mod && d->rm == 6)
                modrm_disp = 2;
        }

        d->inst_length = base_size
                       + modrm_disp
                       + iw_size * (d->operand_width + 1);
    }
}

#endif /* EMU86_DECODE_H */
