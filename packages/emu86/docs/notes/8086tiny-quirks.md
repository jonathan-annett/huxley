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
