# emu86

A portable, snapshotable 8086 emulator. Runs identically on Linux CLI and in a browser Web Worker.

Clean-room refactor of [8086tiny](https://github.com/adriancable/8086tiny) by Adrian Cable (MIT License). The original source is preserved unmodified in `reference/` for comparison.

## Status

Phase 0A: Source analysis and project setup.

See `../../docs/emu86-roadmap.md` for the full build plan.

## Build

```bash
# Build the reference (original 8086tiny) for comparison
make reference-build

# Build emu86 (when implemented)
make build
```

## Architecture

See `docs/ARCHITECTURE.md` (created during implementation).

Key design decisions:
- **Single state struct** (`Emu86State`) — fully snapshotable, no globals
- **Platform abstraction** (`Emu86Platform`) — ring buffers and callbacks, identical API for Linux and browser
- **Batch execution** (`emu86_run()`) — runs ~20,000 cycles per call, yields on budget or I/O need
- **Inline opcodes** — `static inline` functions in headers, unity build for full inlining
- **Portable snapshots** — save in browser, load on Linux CLI for deterministic debugging
