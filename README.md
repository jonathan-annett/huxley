# Lite

A lightweight browser-based code editor with an integrated 8086 emulator running ELKs, designed for AI-assisted development with full user sovereignty over the editing experience.

## Repository Structure

```
lite/
├── docs/              # Project plans and architecture documents
├── packages/
│   ├── emu86/         # 8086 emulator — standalone, builds first
│   └── editor/        # Browser-based editor — depends on emu86
├── images/            # Disk images (gitignored, fetched by script)
└── tasks/             # Agent task prompts and history
```

## Getting Started

### Prerequisites

- gcc (or clang)
- make
- NASM (for BIOS assembly)
- git
- Node.js 20+ (for editor and browser wrapper, later)

### Fetch disk images

```bash
./images/fetch-images.sh
```

### Build the emulator

```bash
cd packages/emu86
make
```

## Project Status

**Current phase:** Phase 0 — Building the emulator (`packages/emu86`)

See `docs/project-plan.md` for the full vision and `docs/emu86-roadmap.md` for the current build plan.

## License

This project is licensed under [TODO: choose license].

The `packages/emu86/reference/` directory contains source from [8086tiny](https://github.com/adriancable/8086tiny) by Adrian Cable, used under the MIT License. See `packages/emu86/reference/LICENSE`.
