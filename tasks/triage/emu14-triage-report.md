# EMU-14 Report
Date: 2026-04-21
Outcome: **HYPOTHESIS-FALSIFIED**

## Summary

The working hypothesis — that the silent exit at ~156,246 instructions is
caused by the BIOS accessing memory in `0xF0000–0xF00FF` expecting
register-file aliasing — is wrong. The actual cause is a different off-by-one
in the instruction-length formula: opcodes `0xC0`/`0xC1` (shift/rotate r/m,
imm) advance IP by one byte too many. This is the same family of bug as
EMU-13 fixed for `0x80–0x83`, and the brief explicitly declared the broader
"does the length formula have other edge cases" question out of scope for
this task. I characterised the cause precisely, verified it end-to-end with a
tentative one-line patch (FreeDOS advanced from silent exit to kernel banner
print), then reverted everything. Tree is clean; no commit made.

## Evidence

### Phase 1 — failure characterisation (deterministic, reproducible)

Running `./emu86 reference/bios test/images/freedos.img` under a pty
(required because the host puts the terminal into raw mode) exits silently
with no console output. Instrumentation around the exit check reported:

- `inst_count = 156246` at the moment the run loop detects `CS:IP = 0000:0000`
- Registers at exit: `AX=0101 BX=7C00 CX=0001 DX=0001 SP=F000 BP=0000
  SI=15DA DI=0FA0 ES=0000 CS=0000 SS=F000 DS=F000 FLAGS=0057`
- The last ~16,900 instructions before exit are all opcode `00 00` (ADD
  [BX+SI], AL) — the CPU is walking through zero-filled memory with IP
  incrementing by 2 each time, from `0000:FF80` up through `0000:FFFE`, then
  wrapping to `0000:0000`
- Rerunning is deterministic (same inst_count, same state)
- Only one CS transition was ever observed: `F000 → 0000 at inst=139350,
  IP=7C00`, and memory at `0000:7C00..+32` is all zero at that moment. So
  the boot sector was never actually loaded to `0:7C00`.

### Phase 2 — hypothesis test (register-memory aliasing)

The hypothesis predicted wrong values on loads/stores in `0xF0000–0xF00FF`.
I did not observe any such access triggering the failure. Instead, the
failure trail works as follows:

1. BIOS init runs normally until `inst=65719`, at `F000:0D4D`, executing
   `C1 E0 09` = `SHL AX, 9` (80186 shift-by-immediate form). This is a
   3-byte instruction. Our emulator's trace shows the next instruction
   begins at `F000:0D51` — IP advanced by **4 bytes**, skipping one byte.
2. The skipped byte is `0F` at `F000:0D50`. That byte was the first byte
   of the BIOS's extended opcode `0F 02` = `extended_read_disk`, the
   emulator-specific disk-read hook.
3. With the `0F` byte skipped, the lone `02` at `F000:0D51` is now decoded
   as a plain `ADD r8, r/m8` instruction, and the disk-read never happens.
4. No disk read → floppy boot sector never loaded into memory at
   `0000:7C00` → that region stays zero.
5. The INT 13h handler continues past the (nonexistent) read, walks through
   unused BIOS memory, eventually reaches the BIOS's `mov [cs:boot_device], dl`
   again (effectively re-entering `bios_entry` via IP wrap `F000:FFFE →
   F000:0000 → ... → F000:0100`). On this second pass, `DL = 1` (modified
   inside the int13 `i_flop_rd` branch by `mov dl, 1`), so `boot_device`
   is overwritten to `1`.
6. The second INT 13h at `F000:032B` has `DX=0001` (garbage drive), the
   handler's `cmp dl, 0 / cmp dl, 0x80` both fail, it returns with error.
7. BIOS unconditionally `jmp 0:0x7c00`. At this point `0000:7C00` is still
   zero-filled, the CPU walks through zeros until hitting `0000:0000` and
   the run loop's exit check fires.

I confirmed the cross-check against the reference emulator: running
`reference/8086tiny` on the same BIOS + disk image with an added print
of `CS:IP` each time `raw_opcode_id == 0xC1` shows the reference advancing
from `F000:0D4D` to `F000:0D50` (i.e. 3 bytes, correct), where the `0F 02`
is decoded as `extended_read_disk` and the boot sector loads as expected.

### Why the discrepancy

Our decoder (`packages/emu86/src/emulator/decode.h`) uses the linear formula

    inst_length = base_size[op] + modrm_disp + iw_size[op] * (operand_width + 1)

The BIOS tables dumped at runtime show for opcode `0xC1`:
`BASE=3, I_W=0, I_MOD=1, XLAT=12, EXTRA=1`, and for `0xC0`: same (`BASE=3,
I_W=0, I_MOD=1, XLAT=12, EXTRA=1`). The `base_size = 3` already accounts
for opcode + modrm + imm8 (the shift count), so the formula produces
`inst_length = 3` for `mod=3, rm=0` — which is correct.

Our `run.c` case 12 (shifts/rotates) then does

    if (d->extra) {
        count = (uint8_t)(d->data1 & 0xFF);
        d->inst_length++;               // <-- over-advances by 1
    }

That extra `++` makes `inst_length = 4`, so IP skips a byte.

The reference `8086tiny.c` also has a `++reg_ip` in its `OPCODE 12` block
for the `extra` path, but it avoids the overshoot by calling
`set_opcode(0x10)` later inside case 12 (only for shifts with `i_reg > 3`);
this re-maps `raw_opcode_id` to `0x10` (ADC r/m8, r8), whose
`base_size[0x10] = 2`, so the main IP-advance formula at line 684 uses 2
instead of 3. Net: `2 (ADC base) + 1 (++reg_ip) = 3`. Correct.

Our refactor dropped the `set_opcode(0x10)` remap but kept the `++`, so the
numbers no longer balance. (Aside: the reference appears to carry a latent
bug for `0xC0`/`0xC1` *rotate* forms — `i_reg < 4` skips `set_opcode(0x10)`,
so those advance by 4 bytes too. The BIOS never uses those forms, so it
never surfaces.)

### Independent verification (kept separate from the diagnostic)

To confirm the cause hypothesis *about the real bug* (not the original
reg-mem hypothesis), I tentatively removed the offending `d->inst_length++;`
line and re-ran `./emu86 reference/bios test/images/freedos.img`. Result:
FreeDOS boot sector relocates to `1FE0:7C5E` (CS transitions `F000 → 0000`
at `IP=7C00 op=EB`, then `0000 → 1FE0` a few instructions later), the
FreeDOS kernel loads from disk, prints its startup banner ("FreeDOS kernel
- SVN (build 2040 OEM:0xfd) [compiled Apr 7 2012]" etc.), and runs until a
new failure ("Interrupt divide by zero"). That is Phase 4 outcome **(b)** —
a new, different bug surfaces, implying the `C0/C1` length overshoot was
indeed what was blocking boot at 156,246, and the reg-mem aliasing did
*not* bite. I then reverted the change; the tree is back to the pre-task
state (verified by `git diff`).

## Root cause

Single-line instruction-length off-by-one in
`packages/emu86/src/emulator/run.c` case 12, at the `if (d->extra)` branch
(C0/C1 shift/rotate r/m, imm form). The BIOS's `base_size[0xC0/0xC1]` of 3
already includes the immediate byte, so the extra `d->inst_length++;` is
wrong. Dropping the line brings the behaviour in line with the actual
instruction length of 3 (+ mod displacement).

This is a direct analogue of the EMU-13 bug in case 8 (ALU r/m, imm for
`0x80-0x83`): both are cases where `run.c` adds an adjustment to
`d->inst_length` that was correct when the refactor's decoder had a
different base-size convention but is wrong given the actual BIOS-table
values. Both stem from mismatched assumptions between decoder and dispatch.

## Scope assessment

- The **original hypothesis fix** (register-memory aliasing) is not
  applicable — no evidence of reg-mem aliasing being exercised along the
  failing path. I cannot assess whether that fix would be localised or
  architectural because the problem it would solve is not the active bug.
- The **actual bug's fix** is trivially localised: one line deleted in
  `run.c` case 12. It does not require any change to state structure,
  snapshot format, opcode helpers, or platform interface. It would not
  break any existing unit test (all 1560 still pass against the
  unmodified-except-for-the-one-line tree I tested).
- However, this fix is **out of scope for EMU-14**. Per the task brief:
  > The decoder's linear length formula (the `CASE8` fix in EMU-13 was the
  > specific bug; the broader "does the length formula have other edge
  > cases" question is not for this task)
  0xC0/0xC1 is exactly such an edge case — another opcode group where the
  linear formula plus per-case adjustment double-counts the immediate.

Per the Phase-2 instruction for contradicted hypotheses ("characterise the
actual cause precisely, skip to Phase 5, and report"), I have not
committed the fix. It belongs in its own task, ideally with a reviewed
audit of **every** opcode group whose `base_size` value might already
include an immediate that the dispatch code then redundantly adjusts.

## Changes made

None. The tree is clean:

```
$ git diff --stat   # (no output)
$ git status        # only untracked: tasks/emu14-task.md
```

Diagnostic code added during investigation (ring buffer of last 64
instructions gated on `-DEMU14_DEBUG`, printf calls at CS transitions,
INT/IRET/0F opcode logging, one-shot BIOS-table dump) was removed before
this report was written. The candidate one-line fix applied during Phase
2's independent verification was also reverted. (A `packages/emu86/emu86-dbg`
binary was produced during the debug session; `rm` is not permitted by my
current sandbox so I could not delete it — please remove it manually.)

## Remaining concerns

- **The C0/C1 instruction-length bug itself.** Needs its own task. The fix
  is one line; I suggest a companion test in `test/unit/test_run.c`
  modelled on `run_push_pop_sreg_dispatch`: drive `C1 E0 09 F4`
  (`SHL AX, 9; HLT`) through `emu86_run` and assert IP advanced by the
  correct amount.
- **Potential siblings of the same bug.** The symptom is "case N in
  `run.c` adds an `inst_length++/+=` adjustment that the BIOS tables
  already count in `base_size`/`iw_size`". Cases worth auditing in a
  follow-up:
  - case 8 (`0x80-0x83`): already audited and fixed in EMU-13.
  - case 12 (`0xC0/0xC1/D0/D1/D2/D3`): the subject of this report.
  - case 6 sub 0 (`TEST r/m, imm`) — adds `d->operand_width + 1` to
    `d->inst_length` for the immediate. Needs checking against
    `base_size[0xF6/0xF7]` and `iw_size[0xF6/0xF7]`. I did not verify this
    during the current task.
  - case 14 (`JMP/CALL near/far`) — sets `d->inst_length = 3 - d->direction`
    manually, overriding the decoder's value. Worth checking the cases
    don't also rely on a base_size entry.
  - case 1 (`MOV reg, imm`) — explicitly recomputes `d->inst_length` from
    tables, looks deliberate.
- **The second BIOS-run-through pattern we saw during Phase 1 debugging
  (the CPU walked off the end of the INT 13h handler into zero memory,
  IP-wrapped F000:FFFF → F000:0000 → F000:0100, and silently re-ran
  bios_entry) is itself a sign that a malformed instruction stream has
  no fault-handling path.** The emulator just churns through zero bytes
  as `ADD [BX+SI], AL` until something lands at `0:0`. This matches a
  concern already listed in the EMU-12 triage ("Silent exit on CS:IP=0:0
  with no output"). Not fixable from inside the emulator cleanly — the
  host could detect "exit before any console output" and warn, as the
  EMU-12 triage suggested.
- **The "Interrupt divide by zero" surfaced during Phase 2 independent
  verification** indicates another bug (likely in flag handling, or in
  DIV/IDIV, or an unrelated kernel expectation), but does not affect
  this report's conclusion. It is a downstream problem to be diagnosed
  after C0/C1 is fixed.
- **The original EMU-12 latent concern about register-memory mapping is
  not disproven by this task.** The reg-mem aliasing hypothesis simply
  was not what was blocking boot *at 156,246 instructions*. After the
  C0/C1 fix, FreeDOS runs long enough (through kernel load and copyright
  print) that such a mismatch could still surface later. Keep the concern
  open; re-assess after the C0/C1 fix is in.

## Recommended next step

Open a new task, e.g. **EMU-15: Fix `0xC0/0xC1` instruction-length
overshoot in `run.c` case 12**, scoped exactly like EMU-13:

1. Remove the `d->inst_length++;` line from case 12's `if (d->extra)`
   branch.
2. Add a targeted unit test that drives `C1 E0 09 F4` (`SHL AX, 9; HLT`)
   through `emu86_run` starting from a known IP, and asserts IP advanced
   by exactly 3 bytes before the HLT. Also add one test for `C1` with a
   memory operand (e.g. `C1 06 34 12 09 F4`, `SHL word [0x1234], 9; HLT`)
   to catch any interaction with the mod/rm displacement.
3. Do the revert-and-re-test discipline: confirm the new test fails
   against unpatched code, then re-patch and confirm it passes.
4. Attempt FreeDOS boot; expect to progress at least to the kernel banner
   (what I observed during Phase 2 verification). The divide-by-zero that
   follows becomes its own follow-up task.

Once that lands, the original EMU-14 register-memory-aliasing concern can
be re-evaluated against how far FreeDOS actually gets, which is a better
signal than speculating from the 156,246 failure point.
