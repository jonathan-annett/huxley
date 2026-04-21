# 8086tiny quirks

Specific behaviours of our reference emulator (`packages/emu86/reference/8086tiny.c`)
that we have encountered and should be aware of. These are not
necessarily *bugs* — some are deliberate design choices, some are
documented limitations, some are latent issues in code paths that
never get exercised. What they share is: *if you encounter one as a
divergence in the harness, the answer is not necessarily "fix our
emulator."*

## Timer tick cadence (inst_counter % 20000)

**Location:** `reference/8086tiny.c` around line 710-711.

**Behaviour:** The reference fires INT 8 (the BIOS timer interrupt,
expected at ~18.2Hz on real PC hardware) every 20,000 executed
instructions rather than on wall-clock time. The condition is
`inst_counter % KEYBOARD_TIMER_UPDATE_DELAY == 0` where
`KEYBOARD_TIMER_UPDATE_DELAY = 20000`.

The reference's `time()` / `ftime()` / `localtime()` calls are *only*
used inside the GET_RTC BIOS call (opcode 0x0F 0x01) to populate guest
memory when software requests the real-time clock. They do not drive
interrupt cadence.

**Design rationale (from Cable's own documentation):** *"A countdown
timer on I/O port 0x40 is simulated in an overly simplistic way which
is good enough for most software. On a real PC this has a default
period of 55ms and is programmable. No programmability is supported in
the emulator and the period may be about right or completely wrong
depending on the actual speed of your computer... On a real PC,
interrupt 8 is fired every 55ms. The emulator tries to do the same but
again, the delay period is uncalibrated."*

In practice the heuristic works well enough for the software 8086tiny
targets, including the Alley Cat game bundled with the emulator. On
very fast modern hardware, timer-driven software ticks faster than
real-PC pacing; on very slow hardware, slower.

**What we do:** Match the reference's `% 20000` cadence exactly
(EMU-19). Reasons:
- Lockstep with the reference requires identical cadence.
- Replacing with wall-clock timing is meaningful additional rework
  and is not required by any current project goal.

**When to revisit:** If we ever need wall-clock-accurate timing (e.g.,
for running timing-sensitive software at true 18.2Hz, or for
validating timer behaviour against real hardware), this inheritance
becomes the blocker. Until then, `% 20000` stands.

**Historical note:** Informally referred to in our project
conversations as "the alleycat clock" (a nod to Cable's bundling of
Alley Cat as his favourite game and demo workload). This is not a
name Cable used or a specific labelled fix in upstream — it's a
mnemonic of convenience. Don't cite it as canonical.

---

## 0xC0 / 0xC1 rotate-form instruction length (latent)

**Location:** `reference/8086tiny.c` — the OPCODE 12 block.

**Behaviour:** For opcodes 0xC0 and 0xC1 (shift/rotate r/m, imm), the
reference's post-execute IP advance uses `set_opcode(0x10)` to re-map
through ADC's base_size, producing the correct 3-byte advance for
shift forms (where `i_reg < 4`). For rotate forms (where `i_reg >= 4`)
this remapping is skipped, and the post-execute `++reg_ip` plus the
original `base_size[0xC0/0xC1] = 3` produces an IP advance of 4 bytes
for what should be a 3-byte instruction.

**Why it hasn't surfaced:** The BIOS and FreeDOS do not appear to use
the 0xC0/0xC1 rotate-by-immediate forms. Only shift forms are
exercised in the workloads 8086tiny has been run against, so the
rotate-path bug has never manifested.

**Identified during:** EMU-15 (our corresponding bug was different but
in the same opcode family; we noticed the reference's mirror-image
latent bug while fixing ours).

**Implication for our work:** Our emulator currently does *not* have
the same rotate-path bug (the EMU-15 fix handles rotate and shift
forms uniformly). If the harness ever surfaces a divergence at a 0xC0
or 0xC1 rotate instruction, the answer is likely "the reference is
wrong, ours is right" — unusual compared to our normal default of
"fix ours."

**When to revisit:** If any workload ever triggers this path, we need
a policy on how to handle the divergence (probably: document and
continue, treating our emulator's correct behaviour as ground truth
for that specific case).

---

## AF flag after shift instructions (Intel-undefined)

**Location:** `reference/8086tiny.c` shift dispatch (case 12) and our
`packages/emu86/src/emulator/opcodes/shift.h`.

**Behaviour:** Intel's 8086 specification explicitly says "The AF flag
is undefined" after SHL, SHR, and SAR. Different implementations may
produce different AF values without being wrong per the spec. The
reference emulator preserves AF across shift instructions (never
calls set_AF, leaves the existing value untouched). Specifically:
`std_flags[0xD2] = 0`, so the post-dispatch flag block does not run
for opcode 0xD2; inside the shift dispatch `set_opcode(0x10)` re-maps
to opcode 0x10 whose `std_flags` entry is SZP only (no AO_ARITH), so
`set_AF_OF_arith()` is never called. Our emulator previously cleared
AF to 0 at the end of these operations.

**Design rationale:** Both are defensible per Intel. We align with the
reference to keep the differential harness clean. Per our
methodology (see `methodology.md`), matching the oracle is the
correctness criterion.

**What we do:** EMU-20 removed the `clear_flag(s, FLAG_AF)` calls
from `exec_shl`, `exec_shr`, and `exec_sar`. AF now carries its
previous value across shifts, matching the reference.

**Identified during:** EMU-20 investigation (following a harness
divergence at step 65,679 during FreeDOS boot — a `SHR BH, CL` at
`F000:10F8` that produced `AF: ref=1 ours=0`).

**Historical note:** Similar behaviour for rotate instructions
(ROL, ROR, RCL, RCR) was already correct — none of these touched AF
in either emulator. Our regression tests now explicitly guard
against regression in that area as well.

---

## LEA and the transient seg_override idiom (Intel-behaviourally-equivalent)

**Location:** `reference/8086tiny.c` LEA handling (case 10, `!i_d`
branch), our `packages/emu86/src/emulator/run.c` case 10, and our
`packages/emu86/harness/harness.c` comparator.

**Behaviour:** The reference implements LEA's offset computation by
reusing the segment-override machinery. It sets:
- `seg_override_en = 1` (a counter that decrements at the top of the
  next main-loop iteration, before any instruction consumes it)
- `seg_override = REG_ZERO` (value 12, a pseudo-register at
  `regs16[REG_ZERO]` that is initialised to 0 and never written to)

After these are set, `SEGREG(REG_ZERO, ...)` evaluates to
`0*16 + offset = offset`, which is what LEA needs in rm_addr.

At the step boundary after LEA executes, the reference has both fields
set. The next main-loop iteration decrements seg_override_en to 0,
and seg_override is never consulted. CPU-observable behaviour is
identical to a conventional LEA implementation.

**Our implementation** (`run.c` case 10) reconstructs the offset
directly from `rm_addr - segment*16`, without using the segment-
override machinery. To align the internal state visible at step
boundaries, we set `seg_override_en = 1` at the end of LEA as a
side effect (matching the reference's flag setting). We do *not*
set `seg_override` to any sentinel value, because our state
vocabulary has no REG_ZERO equivalent — our `seg_override` is an
SREG enum in [0, 3].

**Harness compensation:** The comparator in `harness/harness.c`
treats `ref seg_override == REG_ZERO (12)` as an equivalence-class
match: when the reference uses this sentinel, the comparator skips
the seg_override value comparison. This is a principled exception
because REG_ZERO is an implementation-specific marker with no
guest-observable meaning.

**Category:** Intel-behaviourally-equivalent divergence (like the
AF-on-shift alignment). Both implementations produce identical CPU
behaviour but differ in internal state visible at step boundaries.
In this case, unlike AF-on-shift, the alignment required a two-part
fix: emulator side (set the flag) and harness side (don't compare
the sentinel value).

**Why not add a zero-segment register to our state?** That was the
alternative approach (extend `Emu86State.sregs` to 5 entries, add
`SREG_ZERO = 4`, etc.). Rejected because it adds permanent state
for a single-instruction idiom. The harness-side equivalence class
achieves the same lockstep property without carrying that weight.
Future LEA-adjacent alignments that need the same equivalence
benefit from this foundation; future alignments that don't can
ignore it.

**Identified during:** EMU-21 (partial, emulator side) and EMU-22
(completed, harness side). EMU-21 triage report at
`tasks/triage/emu21-triage-report.md` has the full investigation.

**Related:** `exec_lea` in `opcodes/transfer.h` is dead code on the
runtime path — the runtime LEA is inlined in `run.c` case 10. This
was discovered during EMU-21. Resolving the DRY violation is
deferred to EMU-23.

---

## Template for future entries

Each entry should include:
- **Location** in the reference source
- **Behaviour** — what it does, precisely
- **Design rationale** if known (or "unknown — inferred from context")
- **Why we hadn't noticed** (if latent)
- **What we do** about it in our emulator
- **When to revisit**
- **Identified during** which task

Keep entries self-contained. The goal is that reading a single entry
tells you everything you need to know without cross-referencing other
documents.
