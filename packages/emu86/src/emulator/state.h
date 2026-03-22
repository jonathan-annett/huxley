#ifndef EMU86_STATE_H
#define EMU86_STATE_H

#include <stdint.h>
#include <string.h>

/* --- General-purpose register indices (regs[]) --- */
#define REG_AX 0
#define REG_CX 1
#define REG_DX 2
#define REG_BX 3
#define REG_SP 4
#define REG_BP 5
#define REG_SI 6
#define REG_DI 7

/* --- Segment register indices (sregs[]) --- */
#define SREG_ES 0
#define SREG_CS 1
#define SREG_SS 2
#define SREG_DS 3

/* --- FLAGS register bit masks --- */
#define FLAG_CF 0x0001
#define FLAG_PF 0x0004
#define FLAG_AF 0x0010
#define FLAG_ZF 0x0040
#define FLAG_SF 0x0080
#define FLAG_TF 0x0100
#define FLAG_IF 0x0200
#define FLAG_DF 0x0400
#define FLAG_OF 0x0800

/* --- Memory sizes --- */
#define EMU86_MEM_SIZE    0x110000  /* 1 MB + 64 KB HMA */
#define EMU86_IO_PORTS    0x10000   /* 64K I/O port space */
#define EMU86_NUM_DISKS   3         /* 0=HD, 1=FD, 2=BIOS */
#define EMU86_NUM_PIT     3         /* PIT channels 0-2 */
#define EMU86_KB_BUF_SIZE 16

/*
 * Emu86State — the complete, snapshotable machine state.
 *
 * ALL persistent state lives here. No globals. No pointers
 * (they can't survive serialisation). A snapshot is a serialised
 * copy of this struct, portable across platforms.
 */
typedef struct {
    /* --- CPU --- */
    uint16_t regs[8];      /* AX, CX, DX, BX, SP, BP, SI, DI */
    uint16_t sregs[4];     /* ES, CS, SS, DS */
    uint16_t ip;
    uint16_t flags;        /* packed FLAGS register */

    /* --- Interrupt / control --- */
    uint8_t  halted;       /* HLT state */
    uint8_t  trap_flag;    /* pending INT 1 (latched from previous instruction) */
    uint8_t  int_pending;  /* hardware interrupt waiting */
    uint8_t  int_vector;   /* which interrupt vector */

    /* --- Prefix state (spans instruction boundaries) --- */
    uint8_t  seg_override_en;  /* countdown: 2 → 1 → 0 */
    uint8_t  seg_override;     /* which segment register (SREG_xx) */
    uint8_t  rep_override_en;  /* countdown: 2 → 1 → 0 */
    uint8_t  rep_mode;         /* 0 = REPNZ, 1 = REPZ */

    /* --- Memory --- */
    uint8_t  mem[EMU86_MEM_SIZE];

    /* --- I/O ports --- */
    uint8_t  io_ports[EMU86_IO_PORTS];

    /* --- Disk --- */
    struct {
        uint32_t size;       /* image size in bytes */
        uint16_t cylinders;
        uint8_t  heads;
        uint8_t  sectors;
        uint32_t position;   /* current seek position */
    } disk[EMU86_NUM_DISKS];

    /* --- PIT (timer) --- */
    struct {
        uint16_t reload;
        uint16_t counter;
        uint8_t  mode;
        uint8_t  _pad[1];   /* explicit padding for consistent layout */
    } pit[EMU86_NUM_PIT];

    uint8_t  pit_lobyte_pending;  /* alternating hi/lo byte select */

    /* --- Keyboard --- */
    uint8_t  kb_buffer[EMU86_KB_BUF_SIZE];
    uint8_t  kb_head;
    uint8_t  kb_tail;

    /* --- Video (text mode) --- */
    uint8_t  video_mode;
    uint8_t  _pad_video;
    uint16_t cursor_row;
    uint16_t cursor_col;

    /* --- Audio --- */
    uint8_t  spkr_en;
    uint8_t  _pad_audio;
    uint16_t wave_counter;

    /* --- Graphics --- */
    uint16_t graphics_x;
    uint16_t graphics_y;

    /* --- Timing --- */
    uint64_t inst_count;
    uint8_t  int8_asap;     /* pending timer interrupt */
    uint8_t  _pad_end[7];   /* pad to 8-byte alignment */

} Emu86State;

/*
 * Initialise state to boot defaults.
 * Zeroes everything, then sets CS=0xF000, IP=0x0100.
 */
static inline void emu86_init(Emu86State *state)
{
    memset(state, 0, sizeof(Emu86State));
    state->sregs[SREG_CS] = 0xF000;
    state->ip = 0x0100;
}

#endif /* EMU86_STATE_H */
