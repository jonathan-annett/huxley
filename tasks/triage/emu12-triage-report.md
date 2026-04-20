# EMU-12 Triage Report

Date: 2026-04-21
Outcome: **BLOCKED** (partial fix applied; a second bug remains that is outside the triage scope)

## Summary

The prior agent was chasing a boot failure. I found two separate bugs. The first
— PUSH/POP segment-register dispatch passing the wrong register index — is
localised and now fixed, with a regression test added. The second — instruction
length miscalculation for the 0x80–0x83 ALU-with-immediate opcode group — is
what the leftover `CASE8` debug print was investigating. It is still broken,
but it is an opcode-family bug and per the brief's scope rules I did not touch it.
FreeDOS still does not boot.

## Evidence

**Unit tests (Phase 1):** all pass, 1560 assertions, 0 failures. The earlier
figure of 2876 in the prior agent's notes was wrong — the real number is 1560
across all modules (including the new platform tests).

**First boot trace (debug still in place, bug-as-found):**
Execution runs 174 instructions in the BIOS init path and reaches `CD 13` (INT
13h, BIOS disk read for the boot sector) at F000:032B. pc_interrupt then jumps
to CS:IP = 0000:0000 and the run loop exits. This means IVT[13h] is zero — no
interrupt handlers were installed.

Digging back: the BIOS's IVT-setup code runs correctly for the `rep stosw` that
zeros the IVT (at F000:0259), but the following `rep movsb` copy of the handler
table is a no-op because CX is loaded as zero from `mov cx, [0x1753]` (itbl_size).
mem[F1753] contains 0x7A as expected — the memory is fine; the load returned
zero because DS was 0, not 0xF000. The `8B 0E 53 17` decode produced rm_addr =
0x01753 rather than 0xF1753.

Root cause of the DS=0: the BIOS's earlier `push cs; pop ds` had silently
failed. Both `PUSH CS` (opcode 0x0E, extra=9) and `POP DS` (opcode 0x1F,
extra=11) were dispatched in run.c cases 25 and 26 with `exec_push_sreg(s, d->extra)`
and `exec_pop_sreg(s, d->extra)`. Those functions index `s->sregs[...]`, which
is a 4-element array (0..3). Passing 9 or 11 is out-of-bounds; the writes
landed on unrelated fields of `Emu86State` and the reads returned garbage.
Case 27 (segment-override prefix) already does `extra - 8`; cases 25 and 26 did
not.

**Second boot trace (after the PUSH/POP sreg fix):**
Now execution reaches 209 instructions, INT 13h correctly dispatches to the
BIOS's own handler at F000:0CAF, and the handler runs through to its
`reach_stack_stc` tail at F000:1249. That sequence is `xchg bp,sp; or word
[bp+4], 1; xchg bp,sp; iret`. We advance IP by 6 bytes for the 4-byte
`83 4E 04 01` (OR [BP+4], 1), land on the wrong instruction, and IRET pops
garbage from a wrong stack offset, landing at CS:IP = 0:0.

That 6-vs-4 discrepancy is the bug the prior `CASE8` debug was investigating.

**Reference cross-check:** `reference/8086tiny reference/bios test/images/freedos.img`
boots FreeDOS correctly under timeout, reaching the `A:\>` prompt. Same BIOS,
same image, so the BIOS binary and disk image are fine — the bugs are ours.

## Root cause

### Bug 1 (FIXED): PUSH/POP sreg dispatch mis-indexes sregs[]

`packages/emu86/src/emulator/run.c`, case 25 and case 26.

The decoder's `extra` field for sreg-push/pop opcodes carries the original
8086tiny numbering where ES=8, CS=9, SS=10, DS=11. Our `Emu86State.sregs` is
indexed 0..3 with ES=0, CS=1, SS=2, DS=3. Cases 25 and 26 passed `extra`
straight through to `exec_push_sreg` / `exec_pop_sreg`, so reads/writes went
to `sregs[8..11]` — out-of-bounds, trampling adjacent struct fields. Case 27
already did `extra - 8`; cases 25 and 26 did not. Two-character fix.

### Bug 2 (NOT FIXED — out of scope): 0x80–0x83 ALU-imm length is off by one to two

`packages/emu86/src/emulator/run.c`, case 8 (ALU r/m, imm — opcodes 0x80–0x83).

The decoder computes `inst_length = base + modrm_disp + iw_size * (w+1)`, and
for 0x80–0x83 the tables give `base=2, iw_size=1, i_mod_size=1`. So the decoder
always counts 2 bytes for the immediate (via `iw_size * (w+1)` with w=1). The
real immediate is 1 byte for 0x80/0x82/0x83 and 2 bytes for 0x81.

Case 8 then runs `d->inst_length += sign_ext ? 1 : 2` (where
`sign_ext = direction | !operand_width`). This *adds* on top of the already-
counted immediate, producing:

    op 0x80 (w=0): decoder=3+disp, +1  → 4+disp  (correct: 3+disp)
    op 0x81 (w=1): decoder=4+disp, +2  → 6+disp  (correct: 4+disp)
    op 0x82 (w=0): decoder=3+disp, +1  → 4+disp  (correct: 3+disp)
    op 0x83 (w=1): decoder=4+disp, +1  → 5+disp  (correct: 4+disp)

All four are wrong; all advance too far. The reference 8086tiny avoids this
by calling `set_opcode(0x08 * (extra = i_reg))` during its OPCODE 8 case, which
re-maps the final `reg_ip +=` to use the ALU-r/m-r opcode's table entries
(iw_size=0), and then does `reg_ip += !i_d + 1` to account for the immediate.
Our decoder has no equivalent rewiring.

The fix (which I did **not** apply) looks like one of:

```c
/* replace `d->inst_length += sign_ext ? 1 : 2` with: */
d->inst_length += (sign_ext ? 1 : 2) - (d->operand_width + 1);
```

which neutralises the decoder's overcount and adds the correct immediate
size. Under that adjustment: 0x80/0x81/0x82 gain no delta, 0x83 loses one.

Why I did not apply it: the brief's Phase 3b rule says "If the bug is anywhere
else — decoder, opcode family, run loop structure — **do not fix it here**."
0x80–0x83 is explicitly an opcode family. Bug 1 I judged as a dispatch-wiring
bug (a two-character index map), which is strictly in the case-dispatch layer
and has a targeted regression test; Bug 2 is a semantic bug in how an opcode
family computes instruction length. That line seems clearer on the family
side of the fence.

## Changes made

- `packages/emu86/src/emulator/run.c`: fixed cases 25/26 to pass `d->extra - 8`
  to `exec_push_sreg` / `exec_pop_sreg`, matching case 27's existing mapping.
  Removed all debug traces added by the prior agent: the `TRACE` block in
  `emu86_run()`, the `xlat_id=…` print, both `IP advance` / `IP jumped` prints,
  and the `CASE8` print inside `execute_instruction`. Removed `#include <stdio.h>`
  since nothing else in the file needs it.
- `packages/emu86/src/hosts/linux/main.c`: removed the three `DEBUG:` prints
  (initial-state dump and per-yield dump). Left all other `fprintf(stderr, …)`
  calls alone (usage message, error reporting, snapshot I/O) — those are
  legitimate.
- `packages/emu86/test/unit/test_run.c`: added `run_push_pop_sreg_dispatch`,
  which drives actual bytes `06 1F 1E 07 F4` (PUSH ES; POP DS; PUSH DS; POP ES;
  HLT) through `emu86_run` and asserts DS and ES both end up at the sentinel
  value 0x1234. This test would have failed with the pre-fix dispatch (DS
  would have stayed at its prior value because the POP went out-of-bounds).
  Also wired it into the RUN_TEST list in `main()`.

No other files touched. `test-unit` runs clean: 1560 passed, 0 failed
(including the new regression test, which reports `run_push_pop_sreg_dispatch... ok`).

## Remaining concerns

- **Bug 2 blocks boot.** Until 0x80–0x83 length is corrected, FreeDOS will not
  reach the `A:\>` prompt and ELKs will not boot either. Every `reach_stack_stc`
  / `reach_stack_clc` path in the BIOS — called on nearly every INT return — hits
  this bug, so almost nothing past the first disk INT completes cleanly.
- **The existing unit tests pass despite Bug 2** because none of them exercise
  the full dispatch path for the 0x80–0x83 group. Several tests use opcodes
  0x3D (`CMP AX, imm16` — xlat_id=7, a different case) and 0x80-style modrm=3
  cases apparently aren't present. A targeted test that drives `83 C0 01 F4`
  (ADD AX, 1; HLT) through `emu86_run` and asserts IP advance would catch it.
- **Prior task-log entry is misleading.** The task log's `PASS (unit tests) —
  manual boot test pending` line implies the boot test simply wasn't run, when
  in fact the prior agent was actively instrumenting the boot path. Per brief
  instruction I have not edited `tasks/completed/task-log.md`, but whoever
  reviews this triage should.
- **`i_mod_size` multiplier missing from decoder.** The reference's linear
  `reg_ip +=` formula has `... * i_mod_size + base + iw_size*(i_w+1)`; our
  decoder uses the same `TABLE_I_MOD_SIZE` table only as a 0/1 has-modrm flag
  (`d->has_modrm`). For every opcode in our BIOS tables `i_mod_size` is 0 or
  1, so this happens to be harmless today, but if any opcode had
  `i_mod_size > 1` our decoder would underestimate the length. This is a
  latent issue, not related to the current boot failure.
- **Register-memory mapping gap.** The reference maps registers into `mem[]`
  at 0xF0000 (`regs8 = mem + REGS_BASE`); we do not. The BIOS's INT-13 handler
  reads this area as ordinary memory in a few places. This did not actually
  bite in my reproduction — the handler got far enough to call
  `reach_stack_stc` before it would have mattered — but it may surface once
  Bug 2 is fixed. Flagging it so nobody is blindsided.
- **Silent-exit on CS:IP=0:0 is not an error.** When a miscomputed IP lands the
  CPU at 0:0 the emulator cleanly yields `EMU86_YIELD_EXIT` and `main()`
  returns 0. That matches the reference's behaviour (`opcode_stream != mem` is
  its loop condition), but it means a broken boot looks indistinguishable from
  a deliberate shutdown. Consider whether the host should warn when the exit
  happens before any console output was produced.

## Recommended next step

Escalate to the user.

1. Review and accept the PUSH/POP sreg fix — it's small, local, has a
   regression test, and clears one of the two blockers.
2. Decide whether the 0x80–0x83 length fix belongs in a new task (EMU-13 or a
   EMU-11 follow-up) or is acceptable to fold into this one. My read of the
   triage brief is that this is outside the triage's scope and wants a separate
   task; your call.
3. Either way, do not commit the current tree as "EMU-12 complete" — that
   would repeat the prior agent's failure mode of claiming PASS when the boot
   still breaks. A commit labelled e.g. "EMU-12: Linux host (boot still
   blocked — see tasks/triage/emu12-triage-report.md)" would be honest.
4. Update `tasks/completed/task-log.md` to reflect the real state once a
   direction is chosen.
