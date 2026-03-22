## Task: EMU-01

### Context
You are building a clean-room refactored 8086 emulator called "emu86". It lives at `packages/emu86/` within a monorepo. The full roadmap is at `docs/emu86-roadmap.md` (relative to the repo root). You are working on the first task.

This project takes the original 8086tiny source (MIT licensed, by Adrian Cable) and refactors it into a portable, maintainable, snapshotable emulator that runs identically on Linux CLI and in a browser Web Worker.

**You are working inside `packages/emu86/`.** All paths in this task are relative to that directory unless explicitly stated otherwise.

### Previous tasks completed
None — this is the first task.

### Your task

**Goal:** Clone 8086tiny, populate the reference directory, read the source line by line, and produce a comprehensive annotation document. Get a native build of the original running.

**The directory structure already exists.** You don't need to create it. Verify it looks like this:

```
packages/emu86/
├── Makefile                 # already present with stub targets
├── README.md
├── reference/               # already present with LICENSE and README
│   └── LICENSE              # Adrian Cable's MIT license
├── src/
│   ├── emulator/
│   │   ├── opcodes/
│   │   └── devices/
│   └── hosts/
│       ├── linux/
│       └── browser/
├── test/
│   ├── unit/
│   ├── integration/
│   └── images/
├── docs/
├── bios/
└── elks-driver/
```

**Steps:**

1. Clone https://github.com/adriancable/8086tiny into a temp directory. Copy the following files into `reference/`:
   - `8086tiny.c` — the emulator source
   - `bios_source/bios.asm` — the BIOS assembly source
   - The pre-built `bios` binary (check the repo root and any subdirectories)
   
   Do NOT overwrite the existing `reference/LICENSE` and `reference/README.md`.
   Clean up the temp directory after copying.

2. Read `8086tiny.c` thoroughly. Produce `docs/ORIGINAL-ANALYSIS.md` containing:

   a. **Global variables inventory** — every global, classified as:
      - CPU state (registers, flags, IP)
      - Machine state (memory, I/O ports, disk, timer, video)
      - Scratch/temporary (used during decode/execute, NOT persistent)
      
      For each variable, note: name, type, what it holds, and whether it belongs in a snapshot.

   b. **Macro inventory** — every #define macro that implements logic (not just constants), with:
      - Name
      - Plain English description of what it does
      - Which opcodes use it
      - Whether it reads/writes globals (and which ones)

   c. **External dependency inventory** — every point where the code calls the OS:
      - `open()`, `read()`, `write()`, `lseek()` calls (disk and keyboard)
      - SDL calls (graphics, keyboard, audio)
      - `time()` / clock calls
      - Any other libc/POSIX calls
      
      For each, note: what it does, where the data comes from/goes, and what the platform abstraction replacement will be.

   d. **Main loop analysis** — describe the structure of the main execution loop:
      - How instructions are fetched and decoded
      - How the opcode dispatch works (the xlat table)
      - How interrupts are checked and serviced
      - How the timer/keyboard/video are serviced
      - The execution flow from one instruction to the next

   e. **Instruction encoding tables** — describe the lookup tables in the BIOS data area that 8086tiny uses for instruction decoding (the bios_table_lookup system).

3. Get the original building and running:
   - Compile `reference/8086tiny.c` with gcc, NO SDL (text mode only):
     `gcc -O2 -o reference/8086tiny reference/8086tiny.c`
   - Or simply: `make reference-build` (the Makefile already has this target)
   - You'll need the bios binary. If it's not pre-built in the repo, note this and document how to build it with NASM.
   - Note where to get a FreeDOS floppy image for testing (https://www.freedos.org/ or the Rugxulo image mentioned in the 8086tiny docs). If you can download one, place it in `test/images/freedos.img`. If not, document the URL.
   - Document the build and run steps in `docs/BUILD-REFERENCE.md`.

4. Update the `Makefile` if needed — the existing one has `reference-build`, `clean`, and stub targets. Add anything missing.

5. Stage and commit everything from the **repo root** (not from `packages/emu86/`):
   ```
   cd ../../
   git add -A
   git commit -m "EMU-01: Reference source and analysis"
   ```

### Deliverables
1. Reference source files in `reference/` (8086tiny.c, bios.asm, bios binary)
2. `docs/ORIGINAL-ANALYSIS.md` — comprehensive, as described above
3. `docs/BUILD-REFERENCE.md` — how to build and run the original
4. Makefile updated if needed
5. Committed to git

### Rules
- Do NOT modify anything already in `reference/` (LICENSE, README.md) — only ADD files
- The analysis document is the most important deliverable. Take your time with it. Read every line of 8086tiny.c.
- If something in 8086tiny.c is unclear, document it as "UNCLEAR:" with your best guess
- Classify every global variable. This classification directly feeds the Emu86State struct design in the next task.
- Commit with message: "EMU-01: Reference source and analysis"
- The commit should be made from the repo root, not from packages/emu86/
