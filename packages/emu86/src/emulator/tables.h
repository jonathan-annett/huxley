#ifndef EMU86_TABLES_H
#define EMU86_TABLES_H

#include <stdint.h>
#include "state.h"

/* --- Table indices into bios_table_lookup / Emu86Tables.data --- */
#define TABLE_XLAT_OPCODE          8
#define TABLE_XLAT_SUBFUNCTION     9
#define TABLE_STD_FLAGS           10
#define TABLE_PARITY_FLAG         11
#define TABLE_BASE_INST_SIZE      12
#define TABLE_I_W_SIZE            13
#define TABLE_I_MOD_SIZE          14
#define TABLE_COND_JUMP_DECODE_A  15
#define TABLE_COND_JUMP_DECODE_B  16
#define TABLE_COND_JUMP_DECODE_C  17
#define TABLE_COND_JUMP_DECODE_D  18
#define TABLE_FLAGS_BITFIELDS     19

/* Bitfields for TABLE_STD_FLAGS values */
#define FLAGS_UPDATE_SZP       1
#define FLAGS_UPDATE_AO_ARITH  2
#define FLAGS_UPDATE_OC_LOGIC  4

#define EMU86_NUM_TABLES  20
#define EMU86_TABLE_SIZE 256

/*
 * Emu86Tables — instruction decoding lookup tables.
 *
 * Derived from the BIOS image at init time. NOT snapshotted —
 * re-derive from BIOS memory on restore. Constant during execution.
 */
typedef struct {
    uint8_t data[EMU86_NUM_TABLES][EMU86_TABLE_SIZE];
} Emu86Tables;

/*
 * Load the 20 decode tables from the BIOS area in state->mem.
 * Call once after the BIOS binary has been loaded into memory at F000:0100.
 *
 * The BIOS contains an array of 20 word-sized pointers starting at
 * F000:0102 (offsets 0x81*2 from the register base). Each pointer
 * gives the offset within the BIOS where that table's 256-byte data begins.
 */
static inline void emu86_load_tables(Emu86Tables *tables, const Emu86State *state)
{
    /* Registers are no longer memory-mapped, so we read table pointers
     * directly from the BIOS area at F000:0100 in mem[].
     * The original code: regs16[0x81 + i] which is at offset 0xF0000 + 2*(0x81+i)
     * = 0xF0102 + 2*i. Each is a 16-bit LE offset within the F000 segment. */
    const uint8_t *bios_base = state->mem + 0xF0000;

    for (int i = 0; i < EMU86_NUM_TABLES; i++) {
        uint16_t ptr = (uint16_t)(bios_base[0x102 + 2*i] |
                                  (bios_base[0x103 + 2*i] << 8));
        for (int j = 0; j < EMU86_TABLE_SIZE; j++) {
            tables->data[i][j] = bios_base[ptr + j];
        }
    }
}

#endif /* EMU86_TABLES_H */
