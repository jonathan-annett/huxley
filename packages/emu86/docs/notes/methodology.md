# Methodology — how we validate emulator correctness

This document records the current approach to checking that our emulator
behaves correctly, and the known limitations of that approach. It is
intended to be read by anyone — future self, future agent — before
proposing changes to the validation strategy.

## The oracle

Our primary correctness reference is the `8086tiny` emulator by Adrian
Cable, committed to this repo at `packages/emu86/reference/8086tiny.c`.
When our emulator behaves differently from 8086tiny on the same input,
we treat our emulator as wrong by default and fix ours.

This is pragmatic rather than principled: 8086tiny has been run against
real DOS software for over a decade, and any bug that affected widely-
used software would have been found and reported by now. For the
workloads we care about (FreeDOS boot, ELKs boot, small test programs),
8086tiny is an adequate authority.

## The harness

`packages/emu86/harness/` runs both emulators in lockstep, comparing
CPU state after every instruction. See EMU-16 / EMU-17 / EMU-18 commits
and `tasks/completed/` for development history.

The harness is the primary tool for detecting correctness regressions.
When divergence is detected, the harness reports:

- The instruction count at which divergence occurred
- Which fields differ (registers, flags, `int8_asap`, etc.)
- The CS:IP on each side

This converts "boot FreeDOS and hope a bug shows up" into "run N steps
and the harness tells you which instruction diverged and how."

## Known limitations of the oracle model

Using 8086tiny as the sole oracle has two specific blind spots:

**Bugs in code paths real software does not exercise.** 8086tiny has
been shaken out against FreeDOS, Windows 3.0, Alley Cat, and similar.
It has *not* been exhaustively verified against every instruction and
every modrm form. Specific latent issues already identified are
documented in `8086tiny-quirks.md`.

**Bugs where both 8086tiny and the workload are wrong in the same
direction.** If 8086tiny and FreeDOS both mis-implement (say) a flag
semantic for a particular instruction, and they happen to be wrong
*consistently* with each other, nothing in our current methodology
would detect it. This is rare but not zero.

## When to stop trusting 8086tiny

Per project-level decision: continue matching 8086tiny until we find
something that is *clearly* wrong (i.e., demonstrably at odds with
Intel documentation, or producing observably broken behaviour that
can't be attributed to our own emulator). At that point, bring in a
secondary reference — candidates include PCem, 86Box, DOSBox-X, or
QEMU's x86 interpreter. Each has trade-offs; the choice should be
deferred until we know which specific disagreement we need to resolve.

Until that threshold is reached, multi-oracle validation is not worth
the engineering cost.

## Validation tiers (for future reference)

The validation strategy has natural tiers of increasing ambition:

1. **Lockstep against reference** (current). Catches everything that
   differs from 8086tiny.
2. **Existing 8086 test programs.** Small `.COM` files written to
   exercise specific instructions or quirks. Deferred until needed.
3. **Cross-oracle validation.** Run against 8086tiny *and* a second
   reference; report which one is outlying on disagreements. Deferred.
4. **Property-based testing.** Generate random valid instruction
   sequences, run through multiple emulators, find divergences.
   Research-project territory; deferred.

## Non-goals

- Cycle-accurate timing. Neither 8086tiny nor our emulator tries to
  reproduce 8086 instruction timings. See `8086tiny-quirks.md` under
  "Timer tick cadence" for what we do instead and why.
- Bit-exact match to real 8086 silicon. We match the oracle; the
  oracle matches its target workloads; that is enough.
