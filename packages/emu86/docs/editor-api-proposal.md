# Editor API — ELKs Integration Design

> **Status: PROPOSAL — not yet ratified**
>
> This document was produced by the EMU-12 agent during implementation
> of the Linux host platform. It proposes a substantive change to the
> project's architecture: promoting ELKs from a v2 power-user feature to
> the primary command interface, and demoting the REPL to a diagnostic
> fallback.
>
> The proposal is preserved here for review but has **not** been folded
> into `docs/project-plan.md` or the editor build plan. Implementation
> decisions should continue to follow the canonical plan until this
> proposal is explicitly ratified.
>
> Open questions to resolve before ratification:
>
> - Can the REPL handle file open/save and sovereignty commands
>   (`pending`, `accept`, `dismiss`) without ELKs running? If not,
>   ELKs becomes a hard dependency for conflict recovery.
> - Does shipping a C toolchain (Smaller C, make, core utils) inside
>   the ELKs disk image port cleanly, or is it a project in itself?
> - Do we maintain two transports (raw frames for the Linux host mock,
>   real TCP for the browser) or invest in a single TCP implementation
>   on both sides?
> - What's the delta in v2 scope this proposal introduces, and is the
>   project plan's v2 roadmap still accurate if we accept it?
>
> Ratification — if it happens — should be a deliberate commit to the
> project plan, not a silent adoption.

---

## Overview

ELKs replaces the custom REPL as the primary command interface for the editor. Standard UNIX tools (grep, sed, find) work directly on project files. Editor-specific operations (open file in tab, search across project, set annotations) are provided by small C utilities in ELKs that talk to the editor over the virtual NIC.

The REPL becomes a diagnostic fallback — available if ELKs isn't running, but not the primary workflow.

## Architecture

```
┌─────────── Browser Main Thread ──────────────┐
│                                               │
│  Ace Editor ←→ Editor API Handler             │
│                    ↕                          │
│              Virtual Switch                   │
│                    ↕                          │
│              NIC RX/TX Ring Buffers           │
└────────────────────┬──────────────────────────┘
                     │ SharedArrayBuffer / postMessage
┌────────────────────┴──────────────────────────┐
│              Web Worker                        │
│                                               │
│  emu86_run() ←→ NIC device                    │
│                    ↕                          │
│              ELKs TCP/IP stack                 │
│                    ↕                          │
│  /usr/bin/eopen, /usr/bin/esearch, etc.       │
│  (or any user script / AI tool)               │
└───────────────────────────────────────────────┘
```

On Linux CLI (for testing):

```
┌─────────── Linux Process ────────────────────┐
│                                               │
│  Mock Editor API ←→ NIC loopback              │
│       ↕                                       │
│  emu86_run() ←→ NIC device                    │
│       ↕                                       │
│  ELKs TCP/IP stack                            │
│       ↕                                       │
│  /usr/bin/eopen, /usr/bin/esearch, etc.       │
└───────────────────────────────────────────────┘
```

The mock editor API runs in the same Linux process as the emulator, reading/writing the NIC ring buffers directly. It simulates the editor's behaviour well enough to test the ELKs utilities without building the browser UI.

## Protocol

### Transport

TCP connection over the virtual NIC. The editor API listens on a fixed IP and port inside the virtual network (e.g., `10.0.0.1:7070`). ELKs utilities connect, send a request, read the response, disconnect. Simple request/response — no persistent connections needed for v1.

### Message Format

Line-based text protocol. Easy to implement in C (no JSON parser needed), easy to debug with `cat` and `echo`, easy to mock.

```
REQUEST:  VERB [args...]\n
RESPONSE: STATUS [data...]\n
          [additional lines...]\n
          \n                        (blank line terminates multi-line response)
```

### Commands

#### File Operations

```
OPEN /src/foo.c
  → OK opened /src/foo.c
  → ERR file not found: /src/foo.c

CLOSE /src/foo.c
  → OK closed /src/foo.c
  → ERR not open: /src/foo.c

SAVE
  → OK saved /src/foo.c
  → ERR no file open

SAVE /src/foo.c
  → OK saved /src/foo.c

SAVEAS /src/foo.c /src/bar.c
  → OK saved as /src/bar.c

NEW /src/newfile.c
  → OK created /src/newfile.c
```

#### Navigation

```
GOTO 42
  → OK line 42 col 0

GOTO 42 10
  → OK line 42 col 10

CURSOR
  → OK /src/foo.c 42 10

ACTIVE
  → OK /src/foo.c
```

#### Search & Replace

```
SEARCH TODO
  → OK 3 results
  /src/foo.c:14: // TODO fix this
  /src/foo.c:42: // TODO refactor
  /src/bar.c:7: // TODO add tests

SEARCH TODO /src/foo.c
  → OK 2 results
  /src/foo.c:14: // TODO fix this
  /src/foo.c:42: // TODO refactor

REPLACE TODO FIXME
  → OK 2 replacements in /src/foo.c

REPLACE TODO FIXME /src/foo.c
  → OK 2 replacements in /src/foo.c

REPLACE TODO FIXME --all
  → OK 3 replacements in 2 files
```

#### Editor State

```
LIST
  → OK 3 files
  /src/foo.c *
  /src/bar.c
  /src/baz.h *
  (* = modified)

STATUS
  → OK
  active: /src/foo.c
  modified: true
  line: 42
  col: 10
  files: 3

THEME monokai
  → OK theme set to monokai

SET tabSize 4
  → OK tabSize = 4
```

#### Buffer Sovereignty Commands

```
PENDING
  → OK /src/foo.c has pending remote changes (mtime 1711000000)
  → OK no pending changes

ACCEPT
  → OK accepted remote changes for /src/foo.c

ACCEPT /src/foo.c
  → OK accepted remote changes for /src/foo.c

DISMISS
  → OK dismissed remote changes for /src/foo.c

DIFF
  → OK 3 changed lines
  - line 14: old content
  + line 14: new content
  - line 20: old stuff
  + line 20: new stuff
  ~ line 30: modified
```

#### Annotation (for AI agent)

```
ANNOTATE /src/foo.c 10 25 ai-active "Refactoring function"
  → OK annotation set

ANNOTATE-CLEAR /src/foo.c
  → OK annotations cleared

ANNOTATE-LIST /src/foo.c
  → OK 1 annotation
  10-25 ai-active "Refactoring function"
```

#### Meta

```
PING
  → OK pong

VERSION
  → OK claudette 0.1.0

HELP
  → OK commands:
  OPEN CLOSE SAVE SAVEAS NEW
  GOTO CURSOR ACTIVE
  SEARCH REPLACE
  LIST STATUS THEME SET
  PENDING ACCEPT DISMISS DIFF
  ANNOTATE ANNOTATE-CLEAR ANNOTATE-LIST
  PING VERSION HELP
```

## ELKs Utilities

Small C programs installed in `/usr/bin/` on the ELKs disk image. Each is a thin client for one or more editor API commands.

| Utility | Maps to | Example |
|---------|---------|---------|
| `eopen` | OPEN | `eopen /src/foo.c` |
| `eclose` | CLOSE | `eclose /src/foo.c` |
| `esave` | SAVE | `esave` or `esave /src/foo.c` |
| `enew` | NEW | `enew /src/utils.c` |
| `egoto` | GOTO | `egoto 42` or `egoto 42 10` |
| `ecursor` | CURSOR | `ecursor` (prints position) |
| `esearch` | SEARCH | `esearch "TODO"` |
| `ereplace` | REPLACE | `ereplace "TODO" "FIXME"` |
| `elist` | LIST | `elist` (prints open files) |
| `estatus` | STATUS | `estatus` |
| `eaccept` | ACCEPT | `eaccept` (accept pending changes) |
| `edismiss` | DISMISS | `edismiss` |
| `ediff` | DIFF | `ediff` (show pending changes) |
| `eping` | PING | `eping` (connectivity test) |

Each utility follows the same pattern:

```c
// eopen.c — ~30 lines
#include "edclient.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: eopen <path>\n");
        return 1;
    }
    int fd = ed_connect();
    if (fd < 0) {
        fprintf(stderr, "eopen: cannot connect to editor\n");
        return 1;
    }
    ed_send(fd, "OPEN %s\n", argv[1]);
    char *response = ed_recv(fd);
    printf("%s\n", response);
    ed_close(fd);
    return strncmp(response, "OK", 2) != 0;
}
```

The `edclient.h` library (~100 lines) handles TCP connection to the editor API endpoint, sending commands, and reading responses.

Because these are real UNIX programs, users can compose them with shell features:

```bash
# Open all .c files in src/
find /mnt/editor/src -name "*.c" | xargs -I{} eopen {}

# Search and pipe to grep for further filtering
esearch "function" | grep "static"

# Replace in all files, save all
ereplace "old_name" "new_name" --all && esave --all

# Script: open a file, goto line, show status
#!/bin/sh
eopen "$1"
egoto "$2"
estatus
```

## Mock Editor API

For testing the ELKs utilities and the NIC communication before the browser editor exists.

### Implementation

`src/hosts/linux/mock_editor.c` — a simple state machine that:

1. Listens on the NIC ring buffer for incoming TCP connections
2. Parses the line-based protocol
3. Maintains a mock editor state:
   - List of "open files" (just paths in an array)
   - Current "active file"
   - Cursor position
   - Modified flags
   - Pending remote changes (simulated)
4. Responds according to the protocol spec
5. Logs all commands received (for test verification)

```c
typedef struct {
    // Open files
    struct {
        char path[256];
        int  modified;
        int  has_pending;
    } files[32];
    int file_count;
    int active_file;    // index into files[]

    // Cursor
    int cursor_line;
    int cursor_col;

    // Command log (for test assertions)
    struct {
        char command[512];
    } log[256];
    int log_count;

} MockEditorState;

// Process one request, produce response
void mock_editor_handle(MockEditorState *state,
                        const char *request,
                        char *response, int response_size);
```

### How the mock plugs in

On the Linux host, the mock editor runs in the same process as the emulator. The host's NIC loopback implementation routes frames between the emulator's NIC and the mock editor's "network stack." Since we're in the same process, this can be simplified:

```
emu86 NIC TX ring buffer
    ↓ (host reads frame)
Simple TCP reassembly (extract payload from TCP/IP frames)
    ↓
mock_editor_handle(request) → response
    ↓
Wrap response in TCP/IP frames
    ↓
emu86 NIC RX ring buffer
```

The TCP/IP handling in the host can be very minimal — we control both sides of the connection, so we don't need a full stack. A simple state machine that tracks the TCP handshake and extracts/wraps payloads is sufficient.

Alternatively, if full TCP/IP in the host is too complex for the test phase, we could use a simpler transport (raw frames with a 2-byte length prefix) and have the ELKs client library use raw sockets instead of TCP. But TCP is better long-term because it means the editor API works with any TCP client, including `curl` or `nc` inside ELKs.

## Test Plan

### Phase 1: Protocol Unit Tests (no ELKs, no NIC)

Test the mock editor API in isolation — just function calls.

**`test/unit/test_mock_editor.c`**

```
TEST: ping
  - Send "PING\n" → response starts with "OK"

TEST: open_file
  - Send "OPEN /src/foo.c\n"
  - Response: "OK opened /src/foo.c"
  - Mock state: files[0].path = "/src/foo.c", active = 0

TEST: open_multiple_files
  - Open /src/foo.c, /src/bar.c, /src/baz.c
  - Send "LIST\n"
  - Response contains all 3 paths

TEST: close_file
  - Open /src/foo.c
  - Send "CLOSE /src/foo.c\n"
  - Response: "OK closed"
  - File no longer in LIST

TEST: close_nonexistent
  - Send "CLOSE /nope.c\n"
  - Response starts with "ERR"

TEST: save_active
  - Open /src/foo.c, mark modified
  - Send "SAVE\n"
  - Response: "OK saved"
  - Modified flag cleared

TEST: goto_line
  - Send "GOTO 42\n"
  - Response: "OK line 42 col 0"
  - cursor_line = 42

TEST: goto_line_col
  - Send "GOTO 42 10\n"
  - cursor_line = 42, cursor_col = 10

TEST: cursor_query
  - Set cursor to 42, 10
  - Send "CURSOR\n"
  - Response contains "42 10"

TEST: active_query
  - Open /src/foo.c
  - Send "ACTIVE\n"
  - Response contains "/src/foo.c"

TEST: search_basic
  - Send "SEARCH TODO\n"
  - Response: "OK 0 results" (mock has no file content, just tracks the command)
  - Log shows "SEARCH TODO" was received

TEST: replace_basic
  - Send "REPLACE foo bar\n"
  - Log shows command received, response is OK

TEST: status_query
  - Open a file, goto line 10
  - Send "STATUS\n"
  - Response contains file path, line number, modified state

TEST: new_file
  - Send "NEW /src/new.c\n"
  - Response: "OK created"
  - File appears in LIST

TEST: pending_none
  - Send "PENDING\n"
  - Response: "OK no pending changes"

TEST: pending_with_changes
  - Simulate pending remote change on active file
  - Send "PENDING\n"
  - Response mentions the file

TEST: accept_pending
  - Simulate pending change
  - Send "ACCEPT\n"
  - Pending cleared

TEST: dismiss_pending
  - Simulate pending change
  - Send "DISMISS\n"
  - Pending cleared

TEST: unknown_command
  - Send "FOOBAR\n"
  - Response starts with "ERR unknown command"

TEST: help_lists_commands
  - Send "HELP\n"
  - Response contains "OPEN", "CLOSE", "SAVE", etc.

TEST: command_log
  - Send OPEN, GOTO, SEARCH
  - Assert log contains all 3 commands in order
```

### Phase 2: NIC Round-Trip Tests (ELKs + mock editor over NIC)

These are integration tests that run after the virtual NIC (EMU-14) is working. They boot ELKs, run an editor utility, and verify the mock editor received the correct command and sent the right response.

**`test/integration/test_editor_api.sh`**

```bash
# Boot ELKs with mock editor on the NIC
# Run eopen inside ELKs (send keystrokes via console_in)
# Check mock editor log for "OPEN /src/test.c"
# Check ELKs console output for "OK opened"

# Boot ELKs
./emu86-test --mock-editor --elks-image test/images/elks.img <<'INPUT'
eopen /src/test.c
elist
esearch "hello"
eping
INPUT

# Verify mock editor log
grep "OPEN /src/test.c" /tmp/mock-editor.log
grep "LIST" /tmp/mock-editor.log
grep "SEARCH hello" /tmp/mock-editor.log
grep "PING" /tmp/mock-editor.log
```

### Phase 3: Browser Integration Tests (after editor exists)

These test the same API but with the real Ace editor on the other end. Deferred to the editor build phase.

## Changes to the Project Plan

### REPL becomes diagnostic fallback

The REPL (from the editor build plan, UI-TERMINAL-01) is retained but simplified:
- Available when ELKs isn't running
- Provides basic commands: `open`, `save`, `close`, `help`
- No search, replace, or advanced features — those live in ELKs
- Primary purpose: bootstrapping (first boot before ELKs is configured) and diagnostics

### ELKs tab is primary

The terminal panel's default tab becomes the ELKs console, not the REPL. Tab order:
1. **ELKs** — primary command interface (shell + editor utilities)
2. **Shell** — remote server shell (tmux, as before)
3. **REPL** — diagnostic fallback
4. **AI Chat** — future

### Editor API is a first-class component

The editor API handler becomes a core part of the editor architecture, not an afterthought:

```
packages/editor/
├── server/
│   ├── editor-api.ts    # API protocol handler
│   └── ...
├── client/
│   ├── api-handler.ts   # Browser-side API handler (virtual switch ↔ editor state)
│   └── ...
```

### ELKs disk image includes editor utilities

The ELKs disk image built for production includes:
- `/usr/bin/eopen`, `eclose`, `esave`, `enew` etc.
- `/usr/lib/edclient.h` and `libedclient.a` — client library
- `/etc/editor.conf` — API endpoint configuration (IP, port)
- Standard UNIX tools (grep, sed, awk, find)
- C compiler and toolchain (for AI tool building)

### AI agent uses the same API

The AI agent doesn't get a special privileged interface. It writes C programs that call the editor API, compiles them in ELKs, and runs them. The same API, the same tools, the same constraints. The only difference is the AI can create new tools — but they all talk through the same protocol.

## Impact on Build Plan

### Emulator phase (current)

No changes. EMU-12 through EMU-17 proceed as planned. The mock editor API becomes part of EMU-14 (NIC testing).

### Editor phase

The editor build plan (docs/editor-build-plan.md) needs these updates:

1. **UI-TERMINAL-01** — REPL is simplified. ELKs tab added as primary.
2. **New task: EDITOR-API-01** — Implement the editor API handler (line protocol parser, command dispatch, integration with EditorManager).
3. **New task: EDITOR-API-02** — Wire the API handler to the virtual switch (NIC ring buffers ↔ API handler).
4. **INTEGRATION-01** — Add tests that boot ELKs, run editor utilities, verify editor state changes.

### ELKs utilities phase (new)

After the NIC is working (EMU-14):

1. **ELKS-UTIL-01** — Build `edclient.h`/`libedclient.a` (TCP client library for ELKs)
2. **ELKS-UTIL-02** — Build the core utilities (eopen, eclose, esave, esearch, ereplace, egoto, elist, estatus, eping)
3. **ELKS-UTIL-03** — Build sovereignty utilities (eaccept, edismiss, ediff, epending)
4. **ELKS-UTIL-04** — Build the ELKs disk image with all utilities and toolchain installed
5. **ELKS-UTIL-05** — Integration test: boot ELKs, run utilities against mock editor, verify round-trip
