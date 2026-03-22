#ifndef EMU86_RUN_H
#define EMU86_RUN_H

#include "state.h"
#include "platform.h"
#include "tables.h"

typedef struct {
    int      reason;
    uint32_t cycles_used;
    uint8_t  io_type;
    uint16_t io_port;
} Emu86YieldInfo;

#define EMU86_YIELD_BUDGET     0
#define EMU86_YIELD_HALTED     1
#define EMU86_YIELD_IO_NEEDED  2
#define EMU86_YIELD_BREAKPOINT 3
#define EMU86_YIELD_ERROR      4
#define EMU86_YIELD_EXIT       5

/* Run a batch of instructions until budget exhausted or yield needed. */
void emu86_run(Emu86State *state, Emu86Platform *platform,
               Emu86Tables *tables, uint32_t cycle_budget,
               Emu86YieldInfo *yield);

/* Execute exactly one instruction. Returns approximate cycle count. */
int emu86_step_single(Emu86State *state, Emu86Platform *platform,
                      Emu86Tables *tables);

#endif /* EMU86_RUN_H */
