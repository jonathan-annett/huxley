# Building and Running the Original 8086tiny

## Prerequisites

- GCC (any reasonably modern version)
- NASM (only if rebuilding the BIOS from source)
- A floppy disk image (e.g., FreeDOS) for testing

## Building (text mode, no SDL)

From `packages/emu86/`:

```bash
make reference-build
```

Or manually:

```bash
gcc -O2 -DNO_GRAPHICS -o reference/8086tiny reference/8086tiny.c
```

This compiles with `NO_GRAPHICS` defined, which disables all SDL dependencies and uses console-based keyboard input via `read(0, ...)`.

### Compiler warnings

GCC will emit warnings about:
- `ftime()` being deprecated (use `gettimeofday` or `clock_gettime` instead)
- Unused return values from `read()` and `write()`

These are expected and harmless for the reference build.

### Building with SDL (graphics mode)

```bash
sudo apt install libsdl1.2-dev
gcc -O2 -o reference/8086tiny reference/8086tiny.c -lSDL
```

Note: 8086tiny uses SDL 1.2, not SDL2.

## Running

```bash
./reference/8086tiny reference/bios test/images/freedos.img [hd_image]
```

Arguments (positional, in order):
1. **BIOS binary** — `reference/bios`
2. **Floppy disk image** — e.g., `test/images/freedos.img`
3. **Hard disk image** (optional) — prefix with `@` to boot from HD instead of FD

The emulator runs until CS:IP reaches 0000:0000, at which point it exits.

### Console I/O

In text mode (NO_GRAPHICS), the emulator:
- Reads keyboard input from stdin (fd 0) in non-blocking mode
- Writes character output via the BIOS `0F 00` custom opcode to stdout (fd 1)

For stdin to work in non-blocking mode, you may need:
```bash
stty raw -echo
./reference/8086tiny reference/bios test/images/freedos.img
stty sane
```

## BIOS binary

The pre-built `bios` binary is included in `reference/bios` (copied from the 8086tiny repository).

### Rebuilding from source

If you need to rebuild the BIOS:

```bash
sudo apt install nasm
nasm -f bin -o reference/bios reference/bios.asm
```

The BIOS assembly source is at `reference/bios.asm`. It contains:
- POST routines
- INT handlers (INT 10h video, INT 13h disk, INT 16h keyboard, etc.)
- Instruction decoding lookup tables (loaded at startup by the emulator)
- Bootstrap loader

## Floppy disk image

The repository includes `fd.img`, a FreeDOS floppy image. It has been placed at `test/images/freedos.img`.

If you need another image, FreeDOS floppy images are available from:
- https://www.freedos.org/download/
- The Rugxulo FreeDOS image referenced in the 8086tiny documentation

## Termination

The emulator's main loop terminates when `CS:IP == 0000:0000` (i.e., when `opcode_stream == mem`). There is no graceful shutdown mechanism in text mode — use Ctrl+C if needed.
