#ifndef EMU86_DECODE_H
#define EMU86_DECODE_H

#include <stdint.h>

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
    uint8_t  operand_width;  /* 0 = byte, 1 = word */
    uint8_t  direction;      /* 0 = rm←reg, 1 = reg←rm */
    uint8_t  mod;            /* MOD field from ModRM */
    uint8_t  reg;            /* REG field from ModRM */

    uint8_t  rm;             /* R/M field from ModRM */
    uint8_t  reg4bit;        /* low 3 bits of raw opcode (register encoding) */
    uint8_t  set_flags_type; /* bitfield: which flags to update */
    uint8_t  inst_length;    /* total bytes consumed (computed) */

    /* --- Decoded data bytes --- */
    uint32_t data0;          /* instruction byte at offset +1 (sign-extended) */
    uint32_t data1;          /* instruction byte at offset +2 (sign-extended) */
    uint32_t data2;          /* instruction byte at offset +3/+4 */
    uint16_t immediate;      /* immediate value (where applicable) */
    uint16_t displacement;   /* displacement value (where applicable) */

    /* --- Effective addresses --- */
    uint32_t rm_addr;        /* resolved R/M effective address (offset into mem) */
    uint32_t op_to_addr;     /* destination operand address */
    uint32_t op_from_addr;   /* source operand address */

    /* --- ALU scratch --- */
    uint32_t op_source;      /* source operand value */
    uint32_t op_dest;        /* destination operand value (before operation) */
    int32_t  op_result;      /* result (signed for flag computation) */

    /* --- General scratch --- */
    uint32_t scratch_uint;
    uint32_t scratch2_uint;
    int32_t  scratch_int;
    uint8_t  scratch_uchar;

} DecodeContext;

#endif /* EMU86_DECODE_H */
