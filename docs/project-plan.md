# Lite Editor — Project Plan

**Status:** Draft for review  
**Date:** March 2026  
**Version:** 1.0  

---

## Executive Summary

Lite Editor is a lightweight, single-user code editor that runs as a Node.js server with a browser-based client. It provides the core editing experience of VS Code — file tree, tabbed editor, integrated terminal — without the extension ecosystem, Electron overhead, or framework complexity. The editor connects to a remote server over a single multiplexed WebSocket, giving the user a full development environment accessible from any browser.

The project has a clear v1 scope (functional editor with file management, terminal, and a command REPL) and a well-defined path to v2 features including AI agent integration, a WASM-based ELKs virtual machine, WebRTC transport upgrade, and version control with time-machine history.

A foundational design principle — **Editor Buffer Sovereignty** — ensures that the user's text is never modified without their explicit consent, establishing the trust model required before any AI or automation layer can be added.

---

## Table of Contents

1. [What We're Building](#1-what-were-building)
2. [Core Design Principles](#2-core-design-principles)
3. [Architecture Overview](#3-architecture-overview)
4. [Editor Buffer Sovereignty](#4-editor-buffer-sovereignty)
5. [V1 Feature Scope](#5-v1-feature-scope)
6. [Technology Choices](#6-technology-choices)
7. [Build Phases & Timeline](#7-build-phases--timeline)
8. [Testing Strategy](#8-testing-strategy)
9. [What V1 Does NOT Include](#9-what-v1-does-not-include)
10. [V2+ Roadmap](#10-v2-roadmap)
11. [Open Questions](#11-open-questions)
12. [Risks & Mitigations](#12-risks--mitigations)
13. [Future Architecture — Time Machine](#13-future-architecture--time-machine)
14. [Future Architecture — AI Tooling Philosophy](#14-future-architecture--ai-tooling-philosophy)
15. [Design Philosophy Note](#15-design-philosophy-note)

---

## 1. What We're Building

A browser-based code editor with four main panels:

```
┌──────────────────────────────────────────────────┐
│  Tab Bar (open files)                            │
├─────────────┬────────────────────────────────────┤
│             │                                    │
│  File Tree  │         Ace Editor                 │
│  (left)     │         (main area)                │
│             │                                    │
│  browse,    │   syntax highlighting, tabs,       │
│  create,    │   multi-file editing, AI           │
│  rename,    │   annotation overlays              │
│  delete     │                                    │
│             │                                    │
├─────────────┴────────────────────────────────────┤
│  Terminal Panel                                  │
│  [REPL] [Shell] [AI Chat*]                       │
│                                                  │
│  REPL: editor commands (open, search, replace)   │
│  Shell: full remote terminal via tmux            │
│  AI Chat: placeholder for v2                     │
└──────────────────────────────────────────────────┘
```

The editor is designed for a single user working on a remote server. There is no collaboration with other humans. However, the architecture anticipates an AI agent layer that will observe and annotate the editor — never modifying it directly — in a future version.

---

## 2. Core Design Principles

### 2.1 User Sovereignty Over Text

The editor buffer is the user's space. Nothing — not AI, not file sync, not remote changes — alters what the user sees in the editor without their explicit action. External changes are surfaced as annotations, indicators, and pending notifications. The user decides when to accept them. This is the single most important design decision in the project and is detailed fully in Section 4.

### 2.2 Minimal Dependencies, Maximum Composability

V1 uses no frontend framework (no React, Vue, or Angular). The UI is vanilla TypeScript with direct DOM manipulation. This keeps the bundle small, avoids abstraction layers between us and the libraries that need direct DOM access (Ace, xterm.js), and makes the codebase understandable without framework-specific knowledge.

If UI complexity warrants it in v2+, Preact (3KB) is the upgrade path.

### 2.3 Transport Abstraction

All communication between browser and server flows through a single multiplexed WebSocket with a pluggable transport interface. This means WebRTC, alternative protocols, or additional channels can be added later without touching application code. The transport is an implementation detail that the rest of the system never sees.

### 2.4 Session Persistence

The remote shell runs inside tmux. Browser refreshes, device switches, and network interruptions don't kill running processes or lose shell history. The user can close their laptop, open a different browser, and pick up exactly where they left off.

### 2.5 REPL as Extension Point

Rather than a command palette or plugin API, Lite Editor uses a local REPL (command line in the terminal panel) for editor manipulation. This composes naturally with the shell, is scriptable, and is the surface where the ELKs WASM runtime will hook in for v2.

---

## 3. Architecture Overview

### 3.1 System Diagram

```
┌──────────────────── Browser ────────────────────┐
│                                                  │
│  File Tree ←→ Editor State Manager ←→ Ace Editor │
│                      ↕                           │
│             Terminal Panel (xterm.js)             │
│              REPL │ Shell │ AI Chat              │
│                      ↕                           │
│          Transport Layer (multiplexed)            │
│  ch:0 control │ ch:1 fs │ ch:2 shell │ ch:3 ai  │
│  ──────────────── WebSocket ─────────────────── │
└───────────────────────┬──────────────────────────┘
                        │ wss://
┌───────────────────────┼──────────────────────────┐
│                  Node Server                     │
│          Transport Layer (demux)                  │
│              ↕         ↕          ↕              │
│          FS API    Shell Mgr   File Watcher      │
│           ↕          ↕              ↕            │
│         disk    tmux → bash      chokidar        │
└──────────────────────────────────────────────────┘
```

### 3.2 Multiplexed Binary Protocol

A single WebSocket connection carries all traffic. Each message has a 4-byte binary header:

| Byte | Purpose |
|------|---------|
| 0 | Channel ID (0x00–0x04) |
| 1 | Flags (JSON, binary, chunk) |
| 2–3 | Payload length (big-endian, max 64KB) |

Channels:

| Channel | Purpose | Encoding |
|---------|---------|----------|
| 0x00 | Control (session, resize, keepalive) | JSON |
| 0x01 | File system operations | JSON + binary |
| 0x02 | Shell I/O (PTY data) | Raw bytes |
| 0x03 | AI annotations | JSON |
| 0x04 | File watch events | JSON |

Binary framing (rather than wrapping everything in JSON) is essential because shell I/O is high-frequency raw bytes. Base64-encoding every PTY output chunk would double bandwidth and add parse latency.

### 3.3 Session Management

On first connect, the server assigns a session ID and creates a tmux session. On disconnect, the server holds the session in memory for 5 minutes. On reconnect, the client sends its session ID; the server reattaches the tmux session and replays any buffered watch events. If the session times out, the tmux session is killed and a fresh one is created on next connect.

---

## 4. Editor Buffer Sovereignty

This section describes the single most important constraint in the system. It is not a feature to be toggled — it is an invariant that every component must respect.

### 4.1 The Rule

**The Ace editor buffer can only be modified by:**
1. The user typing (Ace's native input handling)
2. Initial file load when opening a file (buffer was empty)
3. Programmatic edits from the REPL or ELKs, which must go through the undo system

**The Ace editor buffer is NEVER modified by:**
- File watcher events (remote/external file changes)
- AI agent actions or suggestions
- WebSocket messages of any kind
- Any future subsystem, plugin, or automation layer

### 4.2 How External Changes Are Handled

When a file changes on disk while it's open in the editor:

```
Disk change detected (chokidar)
        ↓
Watch event sent to client (ch:0x04)
        ↓
Client reads new file content via FS API
        ↓
Content stored as "pending remote" on the tab
        ↓
  ┌─────────────────────────────────────┐
  │  Buffer UNCHANGED. Visual indicators │
  │  show that an external change exists │
  └─────────────────────────────────────┘
        ↓
User explicitly runs "sync" / "accept" in REPL
or clicks the change indicator
        ↓
ONLY NOW does the buffer update
(as a programmatic edit, fully undoable)
```

### 4.3 Conflict States

Each open file tab tracks a conflict state:

| State | Meaning | What the user sees |
|-------|---------|-------------------|
| **clean** | Buffer matches disk | Normal editing |
| **local-only** | User has unsaved edits, disk unchanged | Modified dot on tab |
| **remote-only** | Disk changed, buffer untouched | Change indicator icon + gutter highlights |
| **conflict** | Both user edits AND disk changes exist | Conflict warning icon |

In the **remote-only** case, the user can see what changed (highlighted in the gutter) and accept with a single command. In the **conflict** case, the editor warns but does not auto-resolve — the user must choose.

### 4.4 Programmatic Edits (REPL / ELKs)

When the REPL executes a command like `replace foo bar`, it modifies the editor buffer through a controlled path:

1. The edit is described as a `ProgrammaticEdit` object (source, description, list of text ranges)
2. The editor wraps all changes in a single Ace UndoManager group
3. The changes are applied atomically
4. The REPL reports what changed: `Replaced 3 occurrences in /src/app.ts`
5. The user can press Ctrl+Z once to reverse the entire operation

This same mechanism will be used by the ELKs WASM runtime when it gains the ability to script editor operations in v2.

### 4.5 Why This Matters

This constraint exists because the editor will eventually have an AI agent layer that can observe and annotate files. Without sovereignty, there is no trust boundary — the user cannot be confident that what they see is what they typed. With sovereignty, the AI can suggest, highlight, and annotate freely, and the user always knows their buffer is theirs.

This also lays the groundwork for v2's version control and time-machine mode: if every buffer modification goes through a controlled path (user keystrokes, programmatic edits with undo groups), then building a full operational history is an additive feature, not a rewrite.

---

## 5. V1 Feature Scope

### 5.1 File Tree (left panel)

- Lazy-loaded directory listing (only expanded directories are fetched)
- Single-click to preview a file (italic tab, replaced by next preview)
- Double-click to open persistently
- Right-click context menu: New File, New Folder, Rename, Delete
- Visual indicator for modified files (dot)
- Visual indicator for files with pending remote changes (icon)
- Auto-refresh when file watcher detects changes

### 5.2 Ace Editor (main area)

- Syntax highlighting via Ace's built-in mode detection
- Tab management with preview/persistent distinction
- Undo/redo per tab (Ace UndoManager preserved across tab switches)
- Cursor position and scroll preserved per tab
- Ctrl+S / Cmd+S to save
- AI annotation layer (gutter decorations and range highlights — display only, v1 uses mock data)
- Pending remote change indicators (gutter highlights showing diff without modifying buffer)

### 5.3 Terminal Panel (bottom)

Three tabs:

**REPL tab** — local command interpreter, not a remote shell. Commands manipulate the editor:

| Command | Action |
|---------|--------|
| `open <path>` | Open file in editor |
| `new <path>` | Create and open new file |
| `save` | Save current file |
| `close` | Close current tab |
| `files [path]` | Print file tree |
| `search <pattern>` | Grep across project |
| `replace <old> <new>` | Find-and-replace in current file (via programmatic edit, undoable) |
| `goto <line>` | Jump to line |
| `theme <name>` | Switch editor theme |
| `set <key> <value>` | Change editor setting |
| `sync` | Accept pending remote changes if clean, warn on conflict |
| `accept` | Force-accept pending remote changes |
| `dismiss` | Discard pending remote changes |
| `help` | List commands |

**Shell tab** — full remote terminal. xterm.js connected to a tmux session on the server via the WebSocket tunnel. Persistent across browser refreshes.

**AI Chat tab** — stubbed in v1. UI exists (tab button), but non-functional. Placeholder for v2.

### 5.4 File System Operations

All file operations are RPC calls over the WebSocket: readdir, readFile, writeFile, mkdir, rename, unlink, stat. All paths are sandboxed to a configurable project root — no path traversal outside the project.

### 5.5 File Watching

The server watches the project directory (via chokidar) and pushes change events to the client. Events include: add, change, unlink, addDir, unlinkDir. Ignored directories: node_modules, .git, dist. Events use relative paths and are debounced (100ms window for rapid changes to the same file).

---

## 6. Technology Choices

| Component | Choice | Rationale |
|-----------|--------|-----------|
| **Editor** | Ace | Mature, lightweight, no VS Code/Monaco dependency chain. Good API for markers, gutter decorations, and programmatic control. |
| **Terminal** | xterm.js + addon-fit + addon-webgl | Industry standard browser terminal. GPU-accelerated rendering. |
| **Server runtime** | Node.js + TypeScript | Native PTY support (node-pty), fast WebSocket (ws), file watching (chokidar). |
| **Shell persistence** | tmux | Battle-tested session management. Detach/reattach handles all reconnection cases. |
| **WebSocket** | ws (server), native WebSocket (client) | Minimal, fast, binary-capable. |
| **Build** | tsup (server), esbuild (client) | Fast builds, ESM output, minimal config. |
| **Test** | vitest | Fast, TypeScript-native, compatible test runner. Every task produces automated tests. |
| **Frontend framework** | None (vanilla TS) | Three panels + a tab bar doesn't justify a framework. Direct DOM access needed for Ace and xterm.js. Preact (3KB) is the upgrade path if complexity warrants it. |

### 6.1 What About Rsync-in-Browser?

We considered implementing rsync's rolling-checksum algorithm in JavaScript for efficient file sync over the tunnel. For v1, this adds too much complexity for the benefit — direct read/write over WebSocket is simple and sufficient for single-user editing.

The file system abstraction includes a cache-layer seam so delta-sync can be added transparently in v2, either as a JavaScript implementation or (more likely) by running rsync inside the ELKs WASM instance where it's a native binary.

### 6.2 ELKs / WASM Integration

A working proof of concept exists: 8086tiny compiled to WASM, running in a Web Worker, capable of booting ELKs from a zipped disk image. The I/O currently uses BIOS-level putchar/keystroke polling. For production integration, the plan is to implement a virtual network adapter in the emulator, giving ELKs a real TCP/IP stack. This enables NFS, HTTP, or any standard protocol for file access, eliminating the need for custom serial bridges.

This is deferred to v2. The v1 architecture accommodates it: the transport has a spare channel, the terminal panel supports additional tabs, and the REPL command system is extensible.

---

## 7. Build Phases & Timeline

The project is divided into 13 tasks across 5 phases. Each task is self-contained, testable in isolation, and produces automated tests as a deliverable. Tasks are fed sequentially to a Claude Code agent, with human review between tasks.

### Phase 1 — Foundation (days 1–2)

| Task | Description | Key Output |
|------|-------------|------------|
| SCAFFOLD-01 | Project skeleton, build tooling, test runner | Working npm build + test pipeline |
| PROTO-01 | Binary frame protocol, message types, sovereignty types | Shared contract between server and client |
| CONFIG-01 | Server configuration (CLI args, env vars, validation) | `lite-editor /path/to/project` works |

### Phase 2 — Transport (days 3–5)

| Task | Description | Key Output |
|------|-------------|------------|
| TRANSPORT-SERVER-01 | WebSocket server, mux/demux, session management | Server accepts connections, routes channels |
| TRANSPORT-CLIENT-01 | WebSocket client, auto-reconnect, RPC helper | Client can call server, survives disconnects |

### Phase 3 — Server APIs (days 6–8)

| Task | Description | Key Output |
|------|-------------|------------|
| FS-SERVER-01 | File system RPC (read, write, mkdir, rename, delete) | Full file CRUD over WebSocket |
| WATCH-01 | File watcher (chokidar), debounced event push | Client notified of external changes |
| SHELL-01 | PTY + tmux management, shell I/O piping | Working remote shell in browser |

### Phase 4 — Client UI (days 9–12)

| Task | Description | Key Output |
|------|-------------|------------|
| UI-LAYOUT-01 | HTML/CSS grid layout, resizable panels, theme | Visual chrome, all panels visible |
| UI-TREE-01 | File tree with lazy loading, context menu | Browse and open files |
| UI-EDITOR-01 | Ace editor, tabs, sovereignty model, annotations | Edit files with full undo, remote change handling |
| UI-TERMINAL-01 | xterm.js panel, REPL commands, shell adapter | REPL and shell working in tabs |

### Phase 5 — Integration (days 13–14)

| Task | Description | Key Output |
|------|-------------|------------|
| INTEGRATION-01 | Wire all components, bootstrap, keyboard shortcuts | Complete working editor |

### Estimated Total: ~14 working days

This assumes a single Claude Code agent working sequentially with human review at phase boundaries. Parallel work within phases could compress the timeline.

---

## 8. Testing Strategy

Every task produces automated tests as a deliverable. The agent cannot mark a task complete until all tests pass.

### 8.1 Test Layers

| Layer | What it tests | How it runs | Example |
|-------|---------------|-------------|---------|
| **Unit** | Individual functions, types, parsers | In-memory, no I/O | Frame encode/decode, REPL parser, mode detection |
| **Component** | Single module with real dependencies | Temp dirs, real WebSockets on random ports | FS API with temp files, transport with real connections |
| **Integration** | Multiple modules working together | Full server stack, real client | Create file via shell → read via FS API → watch event arrives |
| **Sovereignty** | Buffer invariant holds under all conditions | Mock + component tests | Remote change → buffer unchanged; programmatic edit → single undo group |

### 8.2 What's Automated vs Manual

**Automated (vitest):** All protocol tests, server API tests, transport tests, REPL logic, tab management, file tree logic, sovereignty invariant tests, integration round-trips.

**Manual / Playwright (future):** Ace rendering, xterm.js rendering, drag-resize handles, visual theme appearance. These are marked as `describe.skip` in the test suite with notes on what to verify.

### 8.3 Regression Protection

After each task, the full test suite runs. If a previous task's tests break, the agent fixes the regression before proceeding. A CI script (`scripts/ci.sh`) runs type checking, builds, and all tests as a single gate.

---

## 9. What V1 Does NOT Include

To keep scope clear, here is what is explicitly deferred:

| Feature | Why deferred | When |
|---------|-------------|------|
| **AI agent integration** | Requires sovereignty model to be solid first | v2 |
| **AI chat interface** | Tab exists but is non-functional | v2 |
| **WebRTC upgrade** | Adds ICE/STUN/TURN complexity for marginal v1 gain | v2 |
| **ELKs WASM runtime** | Virtual NIC needs design work; PoC exists | v2 |
| **Version control integration** | Depends on operational history infrastructure | v2 |
| **Time-machine mode** | Depends on version control | v2 |
| **Rsync / delta sync** | Direct read/write is sufficient for single user | v2 |
| **Authentication** | V1 assumes trusted single-user environment | Pre-deployment |
| **Multi-file search/replace** | Single-file replace in v1; project-wide in v2 | v2 |
| **Drag-and-drop file move** | Nice-to-have, not essential | v1.1 |
| **Collaboration** | This is a single-user editor by design | Not planned |

---

## 10. V2+ Roadmap

These features are designed-for but not built in v1. The v1 architecture includes specific seams where each plugs in.

### 10.1 AI Agent Layer

The AI agent is a separate process that communicates over channel 0x03. It can push annotations to the editor (gutter icons, range highlights, ghost text) but cannot modify the buffer. The user interacts with the AI via the chat tab in the terminal panel.

**V1 seam:** Annotation types are defined in the protocol. The editor renders them. Channel 0x03 is reserved. The chat tab exists as a stub.

### 10.2 ELKs WASM Virtual Machine

A real 8086 PC running ELKs (or FreeDOS) in the browser, with a virtual network adapter bridging to the editor's file system. Users can run native UNIX tools (grep, sed, awk, rsync) against editor files through a mounted filesystem.

**V1 seam:** Terminal panel supports additional tabs. Transport has spare channels. REPL command system is extensible. A working PoC (8086tiny on WASM + xterm.js) exists.

### 10.3 WebRTC Transport

Upgrade from WebSocket to WebRTC DataChannel for lower latency on shell I/O. The server acts as the signaling channel for ICE negotiation. Falls back to WebSocket if DataChannel fails.

**V1 seam:** The `Transport` interface is abstract. WebRTC is a new implementation of the same interface.

### 10.4 Version Control & Time Machine

Covered in depth in Section 13: Future Architecture — Time Machine.

---

## 11. Open Questions

These are areas where input from reviewers would be valuable:

### 11.1 Initial File Load for Already-Open Files

When the user reconnects after a browser crash, should the editor re-fetch all previously open files from disk and restore tabs? If so, how do we handle the case where a file changed on disk during the disconnection? Current plan: re-fetch all, but treat any changes since last known mtime as "pending remote" — never silently overwrite the user's remembered state.

### 11.2 Large File Handling

What's the threshold for "too large to load in the editor"? Ace struggles with files over ~100K lines. Should we warn, truncate, or refuse? Current thinking: warn at 50K lines, refuse at 200K lines, with an override flag.

### 11.3 Binary File Handling

The FS API detects binary files and returns base64. Should the editor show a hex view, a "binary file" placeholder, or refuse to open? Current plan: show a placeholder with file size and type info, with a "view as hex" option in v2.

### 11.4 Conflict Resolution UI

When both local edits and remote changes exist (conflict state), the REPL commands `accept` (force-accept remote, losing local edits) and `dismiss` (keep local, discard remote) are available. Should there be a visual diff/merge tool in v1, or is REPL-based resolution sufficient? Current plan: REPL-only in v1, visual diff in v2.

### 11.5 ELKs Virtual NIC Architecture

The proof-of-concept ELKs instance uses BIOS-level I/O (putchar/keystroke). The production plan is a virtual network adapter. This needs design work: should it emulate an NE2000 (simple, well-supported in ELKs), or use a custom virtio-style device? What protocol runs over it — raw Ethernet frames with a JS-side TCP/IP stack, or something lighter? Note that if the AI tooling model (Section 14) is adopted, this NIC also serves as the deployment pipeline for AI-crafted tools — it must handle bidirectional binary transfer, not just filesystem mounting.

### 11.6 Security Before Deployment

V1 assumes a trusted single-user environment. Before any network-accessible deployment, we need: TLS (via reverse proxy), token-based authentication, and rate limiting. How urgent is this? Is v1 strictly local-only, or will it be network-accessible early?

---

## 12. Risks & Mitigations

| Risk | Impact | Likelihood | Mitigation |
|------|--------|-----------|------------|
| **node-pty build issues** | Shell doesn't work | Medium | node-pty requires native compilation. Pin version, test on target OS early. Fallback to plain child_process if needed. |
| **tmux not installed** | Shell persistence lost | Low | Detect at startup, warn user, fall back to plain bash without persistence. |
| **Ace performance on large files** | Editor becomes sluggish | Medium | Set line count thresholds, warn user. Ace's virtual renderer handles most cases. |
| **chokidar reliability** | Watch events missed or duplicated | Low-Medium | Debounce aggressively. Node 20+ native recursive watch as fallback. Integration tests verify events arrive. |
| **WebSocket disconnects in poor networks** | Session state lost | Medium | Exponential backoff reconnect. tmux preserves shell. Pending FS operations queued and retried. |
| **Sovereignty invariant violation** | User trust broken, blocks AI integration | High impact, Low likelihood | Extensive automated tests. No public API on EditorManager that sets buffer content. Code review enforcement. |
| **Scope creep into v2 features** | v1 delayed | Medium | This document defines the boundary. Tasks are modular and self-contained. |
| **WASM state loss on tab close** | AI agent loses work in progress | High impact, Medium likelihood | Three-tier storage architecture (Section 13.1) with event-driven checkpointing to IndexedDB. |

---

## 13. Future Architecture — Time Machine

This section describes the time machine system planned for v2–v3. None of this is built in v1, but the v1 design decisions — editor buffer sovereignty, controlled edit paths, programmatic edit types with source attribution — are the foundation it rests on. This section is included so reviewers can evaluate whether the v1 architecture adequately supports the future direction.

### 13.1 Three-Tier Storage Architecture

State in the system lives in one of three tiers, each with different performance, persistence, and cost characteristics.

```
┌─────────────────────────────────────────────────────────┐
│                    HOT — WASM Memory                    │
│                                                         │
│  Active computation: ASTs, partial solutions, agent     │
│  working context, ELKs instance RAM.                    │
│                                                         │
│  Speed: nanoseconds                                     │
│  Persistence: NONE — dies with the browser tab          │
│  Size: bounded by WASM linear memory (typically 16–64MB)│
│  Backed by: WebAssembly.Memory ArrayBuffer              │
└──────────────────────┬──────────────────────────────────┘
                       │ checkpoint (event-driven)
                       ▼
┌─────────────────────────────────────────────────────────┐
│               WARM — Browser Storage                    │
│                                                         │
│  WASM memory snapshots, compiled tool binaries,         │
│  editor session state, AI working context dumps.        │
│                                                         │
│  Speed: milliseconds                                    │
│  Persistence: survives tab close, browser restart        │
│  Size: hundreds of MB (IndexedDB / OPFS)                │
│  Bound to: this browser on this device                  │
└──────────────────────┬──────────────────────────────────┘
                       │ sync (on save, on signoff)
                       ▼
┌─────────────────────────────────────────────────────────┐
│               COLD — Server Filesystem                  │
│                                                         │
│  Canonical codebase, time machine history (git object   │
│  store), tool source projects, documentation.           │
│                                                         │
│  Speed: network round-trip                              │
│  Persistence: survives everything, backupable            │
│  Size: unbounded (disk)                                 │
│  Backed by: git object model (blobs, trees, commits)    │
└─────────────────────────────────────────────────────────┘
```

**Why three tiers, not two.** A two-tier model (memory + server) means every checkpoint requires a network round-trip. This makes aggressive checkpointing prohibitively slow. The warm tier (IndexedDB/OPFS) sits in between: it's fast enough for frequent writes (a 16MB WASM memory dump to IndexedDB takes ~50ms) and persistent enough to survive tab closes. The server is the source of truth, but the warm tier is the safety net for the most common failure mode — the user closing or refreshing the tab.

**Checkpointing strategy (hot → warm):**

Checkpoints are event-driven, not continuous. A checkpoint is triggered by:

- User saves a file (Ctrl+S)
- REPL/ELKs completes a programmatic edit
- AI agent completes a task or sub-task
- Timer: every 30 seconds during active WASM computation
- User explicitly requests: `checkpoint "about to try something risky"`
- Lint/compile status transitions (see Section 13.3)

Each checkpoint captures: the WASM linear memory (as a typed array), the editor's open tab state, and a delta of changed project files since the last checkpoint. Deltas keep checkpoint size proportional to what changed, not to the total project size.

**Sync strategy (warm → cold):**

The warm tier syncs to the server on user save, on signoff (Section 13.4), and on a background timer. This is not time-critical — the warm tier is the crash recovery mechanism, the cold tier is the permanent record.

**Resumption (warm → hot):**

When the user reopens the editor after a tab close, the system checks IndexedDB for a warm checkpoint. If one exists and is newer than the server state, it offers to resume: "You had unsaved work in progress. Resume where you left off?" This restores the ELKs WASM memory, the open tabs, and the editor state. The user is back to where they were, including any AI agent computation that was in flight.

### 13.2 Time Machine Storage Model

The time machine history is stored on the server (cold tier) using git's internal object model. This is not an abstraction or an analogy — it is literally git, used programmatically:

- **Blobs** store file contents, content-addressed by SHA hash. Identical files across checkpoints are stored once.
- **Trees** store directory listings, mapping filenames to blob hashes.
- **Commits** are checkpoints: a tree hash (the project state), a parent commit hash (the previous checkpoint), a timestamp, and metadata.

This gives us deduplication for free. If only 2 files changed between checkpoints, only 2 new blobs are created. The tree is rewritten to point to the new blobs, but unchanged subtrees share references. A project with 10,000 files where 3 changed between checkpoints costs approximately the size of those 3 file diffs, not a full copy.

Branches in the time machine are git branches. Forking at a checkpoint creates a new branch from that commit. The developer's main line of work is the `main` branch. Each fork the AI explores is a named branch (`attempt/recursive-approach`, `attempt/iterative-approach`).

**Why git and not a custom format?** Because git is a solved problem with 20 years of optimisation, garbage collection handles unreferenced objects automatically, the object model is well-documented, and the entire time machine history is interoperable with standard git tools. A developer who wants to inspect the history outside the editor can just use `git log`.

### 13.3 Checkpoint Quality Tagging

Every checkpoint carries metadata beyond the timestamp. This metadata is what makes the time machine *queryable* rather than just *navigable*.

When a checkpoint is created, a lightweight pipeline runs against the changed files:

```
Files changed
    ↓
Lint (fast — seconds)
    ↓
Compile check (fast — seconds for type checking)
    ↓
Optionally: test suite (slower — only on save-triggered checkpoints)
    ↓
Tag the checkpoint with results
```

A checkpoint record looks like this:

```
checkpoint: a1b2c3d4
parent: e5f6g7h8
timestamp: 2026-03-21T14:32:15Z
trigger: user-save
files_changed: [src/parser.ts, src/lexer.ts]
lint: { status: pass, errors: 0, warnings: 2 }
compile: { status: pass }
tests: { status: pass, passed: 47, failed: 0, skipped: 3 }
sovereignty: { source: user-keystroke }
user_label: null
ai_notes: null
```

This enables queries that would otherwise require scrubbing through history manually:

| Query | Implementation |
|-------|---------------|
| "Go back to last clean compile" | Find most recent checkpoint where `compile.status === pass` |
| "When did this test start failing?" | Binary search over checkpoints for `tests` status transition on the specific test |
| "Show me all checkpoints where the linter was clean" | Index scan on `lint.status === pass AND lint.warnings === 0` |
| "What was the state before the AI agent started working?" | Find checkpoint before the first `sovereignty.source === ai-agent` entry |

The lint/compile pipeline also serves as a **natural checkpoint boundary detector**. Rather than checkpointing on a fixed timer during periods of heavy editing (which captures many broken intermediate states), the system can checkpoint *when the lint status transitions* — the moment a broken file becomes valid again, or vice versa. These transition points are inherently meaningful: they mark the boundaries between "the developer is mid-edit" and "the developer has completed a thought."

### 13.4 Checkpoint Lifecycle & Compaction

Checkpoints move through three lifecycle phases:

**Active** — the developer (or AI agent) is working on something. All checkpoints are retained. The safety net is at maximum strength. A function being written might generate 200 checkpoints over 30 minutes of work. Every one is kept because the developer might need to roll back to any of them.

**Completed** — the unit of work is done. The developer signals this explicitly (`signoff parser-refactor` in the REPL) or the system infers it (all tests pass after a sustained editing session on related files). Checkpoints enter a grace period. They're candidates for compaction but haven't been compacted yet, because the developer might reopen the work.

**Archived** — after the grace period, compaction runs. The checkpoint sequence is reduced to a curated subset that tells the story of the work without the noise.

#### The Compaction Problem

Naive compaction — keep every Nth checkpoint, or keep only the first and last — destroys information. In a sequence of 200 checkpoints where a developer went from an empty function to a working module, the mechanically interesting checkpoints aren't evenly distributed. The pivot points — where the approach changed, where a bug was found, where the design crystallised — are clustered unpredictably. A rule that keeps every 20th checkpoint will almost certainly miss the most important ones.

This is where checkpoint quality tags become load-bearing infrastructure. A rule-based compactor can use them to identify structurally meaningful checkpoints:

- **Lint/compile transitions**: the checkpoint where the code first compiled, where it broke, where it was fixed. These mark phase boundaries.
- **Test transitions**: the checkpoint where tests first passed, where a regression appeared.
- **User-labelled checkpoints**: any checkpoint the developer explicitly marked (`mark "switching to recursive approach"`).
- **First and last**: the starting state and the signed-off state are always kept.

A rule-based compactor applying these heuristics might reduce 200 checkpoints to 15–20. This is good. But it's not good enough, because the most important moments in a development session are often *not* the ones where lint status changed.

#### The AI Curator

An AI agent reviewing the checkpoint sequence can do something a rule-based system cannot: it can read the actual code at each checkpoint and understand *intent*.

Consider: the developer is writing a parser. Checkpoint 47 has broken syntax everywhere — half the function is deleted, the type signatures are wrong, nothing compiles. A rule-based compactor sees a failing lint check and marks it for deletion. But the AI curator reads the diff and recognises that checkpoint 47 is where the developer abandoned a top-down parsing approach and started writing a Pratt parser instead. The deleted code reveals *why* — the precedence handling was becoming unmanageable. Checkpoint 47 is the most important moment in the entire sequence. The AI curator keeps it and annotates it:

```
checkpoint: 47 [AI-CURATED: RETAINED]
curator_notes: "Developer abandoned recursive descent parser here 
  and switched to Pratt parsing. The deleted code in parse_expression() 
  shows the precedence logic had become unmanageable — 6 levels of 
  nested if/else with no clear extension path. The Pratt approach 
  adopted after this point resolved the issue. This checkpoint 
  documents the architectural decision."
```

The human never asked for this analysis. The AI curator produced it autonomously as part of the compaction process, because understanding *why* a checkpoint matters is inseparable from deciding *whether* to keep it.

This is not hypothetical capability. Current language models are good at reading code diffs and explaining what changed and why. The curator doesn't need to be brilliant — it needs to read 200 diffs and flag the 4–5 that represent actual decisions rather than mechanical edits. That's squarely within what models can do today.

The compaction pipeline becomes:

```
Rule-based pass (fast, mechanical)
    ↓
  Retain: lint/compile transitions, test transitions,
  user-labelled, first/last
  Tentatively discard: everything else
    ↓
AI curator pass (slower, intelligent)
    ↓
  Review tentatively discarded checkpoints
  Retain any that represent: design decisions, approach changes,
  bug discoveries, abandoned alternatives worth documenting
  Annotate retained checkpoints with explanations
    ↓
Final compacted history
```

The developer sees the compacted history and can inspect the AI curator's notes. If they disagree with a retention decision, they can override it — promote a discarded checkpoint or discard a retained one. The AI curator is an advisor, not an authority. But in practice, an AI that has read every single intermediate state of the code is going to catch significant moments that the developer has forgotten about by the time compaction runs.

### 13.5 Branching & Speculative Execution

The time machine supports forking: rewinding to a previous checkpoint and taking a different path from that point. This is the foundation for AI-assisted speculative development.

The model is **sequential, not parallel**. The AI agent runs, hits a wall (a test fails, a lint error it can't resolve, a design that isn't working). The evaluator — which might be the AI itself reflecting on its failure, or a separate "wiser" agent reviewing the work, or the human developer — identifies the checkpoint where things went wrong and decides on an alternative approach.

The system rewinds to that checkpoint, creates a branch, modifies the AI's instructions for the new attempt, and the agent runs again. If the first attempt took a wrong turn at checkpoint 30, the branch starts at checkpoint 30 with different instructions. The agent produces a new sequence of checkpoints on the branch.

Sometimes one attempt is sufficient. Sometimes the evaluator identifies two or three plausible alternative approaches and runs them sequentially: branch A from checkpoint 30 with approach X, branch B from checkpoint 30 with approach Y, branch C from checkpoint 30 with approach Z. Each branch runs to completion or failure independently.

```
main:      ─── 1 ── 2 ── ... ── 30 ── 31 ── 32 (stuck)
                                 │
branch/approach-x:               └── 31' ── 32' ── 33' (tests pass)
                                 │
branch/approach-y:               └── 31" ── 32" (still failing, abandoned)
                                 │
branch/approach-z:               └── 31‴ ── 32‴ ── 33‴ (tests pass, cleaner)
```

The developer reviews the branches that succeeded. They might pick approach-z because the code is cleaner, even though approach-x also passed tests. Or they might take pieces from both. The time machine's diff view shows what each branch did differently from the fork point.

**Why sequential, not parallel:**

Running three agents simultaneously requires three isolated environments — three filesystem snapshots, three WASM instances, three times the computation. Sequential execution reuses the same environment, just reset to the fork point between attempts. It's cheaper, simpler, and matches how the system will actually be used: the evaluator examines the result of one attempt before deciding what to try next. Sometimes the first attempt succeeds and there's nothing else to try. Sometimes the failure mode of attempt A informs what to try in attempt B. Parallel execution forfeits that learning.

**State capture at fork points is about the codebase, not running processes.** A fork point checkpoint contains: every file in the project, the editor's tab state, and the checkpoint metadata. It does not attempt to capture running server processes, open network connections, or database state. The AI agent on the new branch starts from a clean codebase state and can spin up whatever it needs. This keeps forking cheap (it's a git branch, not a VM snapshot) and reliable (no fragile process restoration).

### 13.6 Implications for V1

None of this changes v1's implementation. But it means certain v1 decisions are more important than they might appear:

| V1 Decision | Why it matters for the time machine |
|-------------|-------------------------------------|
| Editor buffer sovereignty | Every mutation goes through a controlled path → those paths are where the time machine records history |
| `ProgrammaticEdit` type with source attribution | The time machine log knows *who* made each change (user, REPL, AI agent) and *why* (the description field) |
| Controlled edit paths (no public `setContent`) | Guarantees no untracked mutations can bypass the history log |
| File watcher events as notifications, not auto-applies | External changes are recorded as events in the log, distinct from user edits |
| Git on the server | The time machine's storage model is literally git. If the project is already in a git repo, the time machine extends it. |

**One small v1 addition worth considering:** add an optional `branchId` field (nullable) to `ProgrammaticEdit`. V1 never sets it. V2's time machine uses it to tag which branch an edit belongs to. This costs one nullable field now and avoids a schema migration later.

---

## 14. Future Architecture — AI Tooling Philosophy

This section describes the model for AI-created tools planned for v3+. It is included because it affects the design of the ELKs integration (v2) and because it represents a distinctive aspect of the project's vision.

### 14.1 The Transparency Contract

When the AI agent creates a tool — a linter, a test harness, a code analyser, a speech-to-text processor — it does not produce an opaque binary. It produces a **project**: source code, documentation, test suite, build instructions, and a design rationale. The tool is treated as if a human colleague had been assigned to build it.

This is not overhead. It is the mechanism by which the human maintains trust in a system where an AI agent has significant autonomy. If the AI is running wild in the time machine sandbox, iterating across branches with tools it built itself, the human must be able to audit those tools. A test harness with a subtle bias could taint an entire evolutionary tree of development. The defence is transparency: the human reads the source, understands the methodology, and either trusts it or fixes it.

### 14.2 The C Mandate

AI-crafted tools must be written in C (or C++, or any language the ELKs toolchain can compile to an 8086 target). The rationale is:

**Legibility at the right level.** C is low enough that there's no hidden runtime, no garbage collector, no framework doing things behind the scenes. What you read is what executes. When the AI writes a string search algorithm in C, the human can see exactly what it does in 40 lines.

**The open source ecosystem.** The AI doesn't write everything from scratch. There are mature, battle-tested C libraries for virtually every domain: signal processing (kiss_fft), compression (miniz), image manipulation (stb_image), parsing (re2c), data structures (uthash). The AI's job becomes composition and adaptation: selecting known components, gluing them together for the specific use case, and explaining the choices. The human can verify: "it's using miniz for compression, that's well-known, the glue code looks reasonable."

**The 8086 constraint breeds discipline.** An 8086 target means no threads, no virtual memory tricks, no SIMD intrinsics. The AI must write straightforward, sequential code that fits in a constrained environment. This is the same reason embedded systems code tends to be more reliable than desktop applications — constraints prevent the complexity hiding that causes bugs in larger environments.

**Browser-local execution inverts the cost model.** A WASM tool compiled from C runs on the user's hardware at near-native speed with zero marginal cost. No API calls, no per-token billing, no network latency. The user will use these tools freely and casually in ways they'd never use a paid cloud service. Speech-to-text (Whisper compiled to WASM) becomes something you leave running continuously. A custom linter runs on every checkpoint without anyone worrying about cost.

The mandate is not C-exclusive. Anything the ELKs toolchain can build is acceptable — a Forth interpreter for certain scripting tasks, assembly for performance-critical inner loops, or a small domain-specific language if the AI determines one would be clearer. The principle is: must compile in the ELKs environment, must be source-available, must be documented and tested. C is the default and the right choice for 90% of cases.

### 14.3 Tool Lifecycle

The AI agent creates a tool as a project within the ELKs instance:

```
tools/
└── custom-linter/
    ├── README.md          # Design rationale, usage, limitations
    ├── CHANGELOG.md       # Version history
    ├── Makefile           # Build instructions
    ├── src/
    │   ├── main.c         # Entry point
    │   ├── rules.c        # Lint rules, documented individually
    │   └── ast.c          # AST traversal
    ├── test/
    │   ├── test_rules.c   # Unit tests for each rule
    │   └── fixtures/      # Test input files
    └── bin/
        └── linter.wasm    # Compiled binary
```

The AI writes the source, compiles it in ELKs, runs the tests, packages the binary, and deploys it to the editor via the virtual network. The editor's tool registry picks it up. The user can:

- `tools list` — see all installed tools
- `tools inspect custom-linter` — opens the tool's source project in the editor
- `tools disable custom-linter` — turns it off without deleting
- `tools rebuild custom-linter` — recompile from source (after the user has modified it)
- `tools remove custom-linter` — delete the tool

### 14.4 The Human in the Loop

The AI produces a tool and documents it meticulously. The human reads the README, looks at the source if they want to, and decides whether to trust it. Maybe they make a suggestion: "the linter should also check for unused imports." The AI updates the source, the tests, the documentation, rebuilds, and redeploys. This is a normal development workflow — it just happens to be between a human and an AI rather than two humans.

The documentation isn't just for the human's benefit. It forces the AI to articulate its design decisions, which makes them auditable. If the AI can't clearly explain why it chose a particular algorithm, that's a signal the human can act on. And because the tool is a full project with tests, the human can verify that the tool does what it claims by running the test suite, not by trusting the AI's description.

### 14.5 Tools the AI Would Build

Examples of tools the AI would likely create for a typical development project:

| Tool | Purpose | Why local matters |
|------|---------|-------------------|
| **Project-specific linter** | Custom static analysis rules for the codebase's patterns and conventions | Runs on every checkpoint, zero cost. Cloud linting would be prohibitively frequent. |
| **Fast test runner** | Executes lightweight unit tests in a WASM sandbox | Instant feedback loop for the AI agent without server round-trips. |
| **Code search index** | Semantic embedding index over the codebase for search-by-concept | Embeddings stay local. No codebase uploaded to third-party vector DB. |
| **Speech-to-text** | Whisper model for voice commands and dictation | Always-on, zero per-minute cost. Private — audio never leaves the device. |
| **Image/asset processor** | Resize, optimise, convert images | No upload to a cloud service for a simple resize operation. |
| **Documentation generator** | Extracts doc comments and generates formatted output | Runs locally against the full codebase without API limits. |
| **Dependency analyser** | Maps module dependencies, detects cycles | Integrates with the time machine to show how the dependency graph evolved. |

The common thread: computationally worthwhile to automate, cheap enough to run on consumer hardware, and private enough that the user would prefer not to send the data to a cloud.

### 14.6 ELKs as the AI's Workshop

The ELKs instance isn't just a curiosity or a tool for the user. It's the AI agent's *development environment*. The AI writes C code, compiles it with the ELKs toolchain, runs tests, debugs failures, iterates — all inside a sandboxed virtual machine that can't affect the user's project until the tool is explicitly deployed.

This means the ELKs disk image needs to ship with a real development toolchain:

- A C compiler (Smaller C, or a port of an older GCC that targets 8086)
- make or equivalent build tool
- A test runner (can be minimal — even a shell script that runs test binaries and checks exit codes)
- Core utilities (grep, sed, diff) for the AI to use during development

The virtual NIC design (Open Question 11.5) becomes more important in this context. The NIC is the deployment pipeline: the AI compiles a tool in ELKs, sends the .wasm binary over the virtual network, and the editor's tool registry loads it. This must support bidirectional binary transfer efficiently.

### 14.7 Implications for V1

The AI tooling model doesn't change v1, but it reinforces two v1 decisions:

The **REPL command system** must be designed for extensibility. When the AI deploys a tool, it registers new REPL commands. The `tools` namespace is reserved.

The **ELKs integration** (v2) should be designed with tool development as a primary use case, not just user scripting. The disk image, the NIC, and the deployment pipeline are all shaped by this.

---

## 15. Design Philosophy Note

The UI of this project is intentionally lean. There is no extension marketplace, no theme store, no settings UI with 400 toggleable options. The editor gets out of the way so the developer can focus on the work.

But the system behind the UI is not lightweight. The storage architecture, the time machine, the AI tooling pipeline, and the speculative execution model represent significant infrastructure. The leanness of the interface is a deliberate choice to keep the human's attention on their code, not a reflection of the system's actual complexity.

This is an important distinction for anyone reviewing the project. V1 looks like a simple editor. It is a simple editor. But it is also the foundation for a system where an AI agent can autonomously explore solution spaces, build its own tools, document its methodology, and present the results to a human who has full transparency into what happened and full control over what to keep.

The human stays in the driver's seat. The AI does the heavy lifting. The editor is the cockpit.

---

## Appendices

### A. Detailed Build Specification

The full build specification with task-by-task implementation details, TypeScript interfaces, and automated test suites is maintained as a separate document (`lite-editor-build-plan.md`). It is designed to be fed directly to a Claude Code agent for implementation.

### B. Proof of Concept: ELKs in Browser

A working prototype exists at [retro.sophtwhere.com](https://retro.sophtwhere.com) demonstrating 8086tiny compiled to WASM, running in a Web Worker with xterm.js rendering. Originally built to boot FreeDOS, it successfully boots ELKs disk images. Current I/O is BIOS-level (INT 10h/16h); production integration will require a virtual NIC.

### C. Related Prior Art

| Project | Relationship |
|---------|-------------|
| VS Code (Monaco) | Inspiration for layout; we avoid its weight and extension complexity |
| Theia | Similar concept but framework-heavy; we go minimal |
| code-server | VS Code in browser; different philosophy — we build from scratch |
| ttyd | Web terminal only; our shell tab is similar but integrated |
| Ace Editor | Our chosen editor component |
| git | Internal storage model for the time machine — not just inspiration, literal reuse |

---

*This document is a living draft. Please add comments, questions, and concerns directly. The build specification will be updated to reflect any approved changes before implementation begins.*
