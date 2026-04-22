# EMU-34: Harness control plane (UDP, transport-agnostic core)

**You are a fresh Claude Code session.** Read this entire brief before touching any files.

**This brief is your task.** Other task files in `tasks/completed/` and triage reports in `tasks/triage/` are reference material, not your assignment.

## Context

Post-EMU-33, the harness is fast enough for interactive use. But every knob the harness exposes today — `HARNESS_FAST_COMPARE`, `HARNESS_HEARTBEAT_EVERY`, `HARNESS_STEP_LIMIT`, etc. — is fixed at process start via env vars. There's no way to toggle fast-compare mode *during* a run, inject a specific step-limit change, or read the current step count without grepping through stderr output.

Concrete missing capability: boot FreeDOS with full-compare on (paranoid correctness for the REP-heavy boot path), and once the prompt is up and interactive DOS work begins, toggle fast-compare on so typing is responsive. Today that requires killing the harness and restarting with a different env var — losing the 30-second boot each time.

### The design

A small control plane. UDP socket on a local port. Line-based text commands. Each datagram is one complete command; responses are sent back to the client's address.

Why UDP rather than TCP or Unix socket:

- One datagram = one complete message; no framing, no delimiter parsing, no connection state.
- No accept loop, no per-connection buffers, no session management.
- `nc -u` works as a trivial test client.
- Packet loss on localhost is effectively zero; even if it happened, the human retries.

Why localhost-bound: no authentication. Anyone on the box can send commands, which is fine for a developer tool. Not exposed to the network.

### Transport-agnostic core

This tool will eventually run in a browser Web Worker under WASM. The port needs to be straightforward: receive an ArrayBuffer via `postMessage`, treat it as a NUL-terminated command string, dispatch, return a NUL-terminated response ArrayBuffer.

To make that port cheap, the command-dispatch logic lives in a pure function that takes a NUL-terminated input string and writes a NUL-terminated output string into a caller-provided buffer:

```c
void harness_control_handle(const char *cmd, char *response, size_t response_size);
```

No sockets, no fds, no stdio, no allocations. This function is identical on Linux and WASM. The transport wrapper — UDP recvfrom/sendto on Linux, postMessage on WASM — calls this function and deals with moving bytes across the transport boundary.

The Linux transport layer adds the NUL terminator to the received UDP payload before calling the core. The WASM transport layer will pass the ArrayBuffer contents straight through (the browser side is responsible for NUL-terminating the payload it sends).

### Command set for v1

Keep the surface small. The protocol shape is the point; more commands are cheap to add later.

```
ping                          → OK pong
help                          → OK (multi-line list of commands)
get <name>                    → OK <value>    or    ERR unknown variable
get all                       → OK (multi-line dump of every variable)
set <name> <value>            → OK <name>=<value>    or    ERR ...
```

Variables exposed for get/set:

| Name              | Type     | R/W | Notes                                    |
|-------------------|----------|-----|------------------------------------------|
| fast_compare      | bool 0/1 | RW  | Toggle `harness_fast_compare`            |
| heartbeat_every   | uint64   | RW  | Toggle heartbeat cadence (0 = disabled)  |
| step_limit        | uint64   | RW  | Change step halt threshold mid-run       |
| step_count        | uint64   | R   | Current `harness_step_count`             |
| compare_cheap     | uint64   | R   | `compare_cheap_count`                    |
| compare_full      | uint64   | R   | `compare_full_count`                     |
| split_output      | bool 0/1 | R   | Whether split-output mode is active      |
| kbd_enabled       | bool 0/1 | R   | Whether keyboard input mode is active    |

Setting a read-only variable returns `ERR <name> is read-only`. Reading an unknown variable returns `ERR unknown variable '<name>'`. Multi-line responses are plain `\n`-separated strings terminated with a single NUL — `nc -u` renders them naturally; the WASM client will `.split('\n')` on the JS side.

### What this task does NOT do

- Does not implement a `snapshot` command. Snapshot save/restore exists, but wiring it to the control plane is a separate task.
- Does not implement a `reboot` command. Rebooting the reference requires longjmp surgery into its main sim loop that's out of scope for v1.
- Does not implement a `kbd` command to inject keyboard bytes via the control plane. The existing kbd.in fifo handles that.
- Does not introduce TCP or any connection-oriented protocol.
- Does not add authentication or access control.
- Does not change any existing harness behaviour when the control plane is disabled.
- Does not modify emulator source (`src/emulator/`) or reference (`reference/`).

### Enablement

Like the other observability features, controlled by env var:

- `HARNESS_CONTROL=1` — enable the control plane. Default off.
- `HARNESS_CONTROL_PORT=<n>` — UDP port to bind. Default 7071.

Setup happens in `main()` alongside `maybe_setup_split_output`, `maybe_setup_kbd_input`, `maybe_setup_fast_compare`. The socket is created before `reference_main()` runs so the port is bindable from startup.

## Your task

### Phase 1 — Read the current harness structure

- `harness/harness.c` setup functions (lines ~1190-1290): `maybe_setup_split_output`, `maybe_setup_kbd_input`, `maybe_setup_fast_compare`. Note the pattern: env var check, optional creation, fd stored in a static, startup banner on stderr.
- `harness/harness.c` step hooks (lines ~870-1090): `harness_step_begin` and `harness_step_end`. Note where `drain_kbd_fifo` is called from step_begin and where the heartbeat fires inside step_end.
- `harness/harness.c` global variables around the top: the knobs being exposed (`harness_fast_compare`, `harness_heartbeat_every`, `harness_step_limit`, `harness_step_count`, `compare_cheap_count`, `compare_full_count`, etc.).

### Phase 2 — Add state

Near the other EMU-XX static globals at the top of harness.c:

```c
/* EMU-34: control plane. HARNESS_CONTROL=1 enables a UDP socket bound to
 * 127.0.0.1:<HARNESS_CONTROL_PORT>. Line-based text commands ("set x 1",
 * "get step_count", etc.) are parsed through a transport-agnostic handler
 * so the WASM port is a drop-in wrapper change. */
static int      harness_control_enabled;
static int      harness_control_fd = -1;
static uint16_t harness_control_port = 7071;
```

### Phase 3 — Implement the transport-agnostic core handler

Add a section, keep it self-contained for easy porting:

```c
/* ================================================================
 * EMU-34: Control plane — transport-agnostic command dispatcher
 *
 * The following section is deliberately free of socket/fd/stdio calls.
 * It operates on NUL-terminated strings in caller-provided buffers so
 * the same code runs under the Linux UDP wrapper and the future WASM
 * postMessage wrapper without modification.
 * ================================================================ */
```

Knob table — one entry per exposed variable, with pointer, type, and read-only flag:

```c
typedef enum { VT_BOOL, VT_U64 } VarType;

typedef struct {
    const char *name;
    VarType     type;
    int         read_only;
    void       *ptr;    /* &harness_fast_compare, &harness_step_count, ... */
} ControlVar;

static const ControlVar control_vars[] = {
    { "fast_compare",    VT_BOOL, 0, &harness_fast_compare },
    { "heartbeat_every", VT_U64,  0, &harness_heartbeat_every },
    { "step_limit",      VT_U64,  0, &harness_step_limit },
    { "step_count",      VT_U64,  1, &harness_step_count },
    { "compare_cheap",   VT_U64,  1, &compare_cheap_count },
    { "compare_full",    VT_U64,  1, &compare_full_count },
    { "split_output",    VT_BOOL, 1, &harness_split_output },
    { "kbd_enabled",     VT_BOOL, 1, &kbd_enabled },
    { NULL, 0, 0, NULL }
};
```

Note that `harness_fast_compare`, `harness_split_output`, and `kbd_enabled` are declared as `int` in the current source. Treat them as 0/1 via `*(int*)ptr`. Don't change their declarations.

Helpers for reading/writing variables by name (internal to the core, no I/O):

```c
static int lookup_var(const char *name, const ControlVar **out);
static int var_get_str(const ControlVar *v, char *out, size_t outsz);
static int var_set_str(const ControlVar *v, const char *val, char *errbuf, size_t errsz);
```

Command handlers — one per verb, same signature:

```c
static void cmd_ping(const char *args, char *resp, size_t rsz);
static void cmd_help(const char *args, char *resp, size_t rsz);
static void cmd_get(const char *args, char *resp, size_t rsz);
static void cmd_set(const char *args, char *resp, size_t rsz);
```

Dispatch table:

```c
typedef struct {
    const char *verb;
    void (*handler)(const char *args, char *resp, size_t rsz);
    const char *help;
} Command;

static const Command control_commands[] = {
    { "ping", cmd_ping, "ping                  — sanity check, returns 'pong'" },
    { "help", cmd_help, "help                  — list commands and variables" },
    { "get",  cmd_get,  "get <name> | get all  — read a variable" },
    { "set",  cmd_set,  "set <name> <value>    — write a variable" },
    { NULL, NULL, NULL }
};
```

The core entry point:

```c
void harness_control_handle(const char *cmd, char *response, size_t response_size)
{
    /* Skip leading whitespace */
    while (*cmd == ' ' || *cmd == '\t') cmd++;

    /* Empty command */
    if (*cmd == '\0' || *cmd == '\n' || *cmd == '\r') {
        snprintf(response, response_size, "ERR empty command");
        return;
    }

    /* Strip trailing newline/CR if present (UDP wrapper may or may not
     * include one; WASM wrapper won't). */
    char buf[512];
    size_t len = strnlen(cmd, sizeof(buf) - 1);
    memcpy(buf, cmd, len);
    buf[len] = '\0';
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) {
        buf[--len] = '\0';
    }

    /* Split verb and args */
    char *verb = buf;
    char *args = strchr(buf, ' ');
    if (args) { *args++ = '\0'; while (*args == ' ') args++; }
    else      { args = ""; }

    /* Dispatch */
    for (const Command *c = control_commands; c->verb; c++) {
        if (strcmp(verb, c->verb) == 0) {
            c->handler(args, response, response_size);
            return;
        }
    }
    snprintf(response, response_size, "ERR unknown command '%s'", verb);
}
```

Handler implementations are straightforward. `cmd_help` walks both tables and produces a multi-line response. `cmd_get` handles both `get <name>` and `get all`. `cmd_set` parses the value according to the variable's type, range-checks if needed, and writes on success.

For VT_BOOL, accept "0" or "1" (reject anything else with `ERR expected 0 or 1`). For VT_U64, use `strtoull` with `*end == '\0'` validation (reject anything else with `ERR expected unsigned integer`).

Response size: use `snprintf` everywhere; let truncation happen cleanly. A 4KB response buffer in the transport layer is plenty for any realistic command including `get all` and `help`.

### Phase 4 — Implement the Linux UDP transport

```c
static void
maybe_setup_control(void)
{
    const char *env = getenv("HARNESS_CONTROL");
    if (!env || strcmp(env, "1") != 0) return;
    harness_control_enabled = 1;

    const char *port_env = getenv("HARNESS_CONTROL_PORT");
    if (port_env) {
        char *end;
        unsigned long p = strtoul(port_env, &end, 10);
        if (*end == '\0' && p > 0 && p < 65536)
            harness_control_port = (uint16_t)p;
    }

    harness_control_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (harness_control_fd < 0) {
        fprintf(stderr, "harness: control socket() failed: %s\n",
                strerror(errno));
        exit(1);
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(harness_control_port);

    if (bind(harness_control_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "harness: control bind(127.0.0.1:%u) failed: %s\n",
                harness_control_port, strerror(errno));
        exit(1);
    }

    fprintf(stderr, "harness: control plane active on 127.0.0.1:%u\n"
                    "         Example: printf 'get all' | nc -u -w 1 127.0.0.1 %u\n",
            harness_control_port, harness_control_port);
    fflush(stderr);
}
```

Call `maybe_setup_control()` in `main()` alongside the other `maybe_setup_*` functions.

Include additions:
```c
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
```

### Phase 5 — The tick function

Drain incoming datagrams, dispatch, reply. Called from `harness_step_end` at a fixed cadence to bound the recvfrom syscall cost:

```c
static void
harness_control_tick(void)
{
    if (!harness_control_enabled) return;

    for (;;) {
        char inbuf[512];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(harness_control_fd, inbuf, sizeof(inbuf) - 1, 0,
                             (struct sockaddr *)&from, &fromlen);
        if (n < 0) {
            /* EAGAIN = no more datagrams waiting. Anything else, log and stop. */
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                fprintf(stderr, "harness: control recvfrom: %s\n",
                        strerror(errno));
            }
            return;
        }
        inbuf[n] = '\0';

        char outbuf[4096];
        outbuf[0] = '\0';
        harness_control_handle(inbuf, outbuf, sizeof(outbuf));

        size_t outlen = strlen(outbuf);
        if (outlen > 0) {
            ssize_t w = sendto(harness_control_fd, outbuf, outlen, 0,
                               (struct sockaddr *)&from, fromlen);
            (void)w; /* best effort */
        }
    }
}
```

Call `harness_control_tick()` from `harness_step_end` every 1000 steps. A single condition in the existing step_end flow:

```c
if (harness_control_enabled && harness_step_count % 1000 == 0) {
    harness_control_tick();
}
```

1000 steps is well under the human response-latency threshold at any realistic step rate, and caps recvfrom syscall overhead at ~1/1000 per step.

### Phase 6 — Test tool

Create `tools/harness-ctl.sh`:

```bash
#!/usr/bin/env bash
# tools/harness-ctl.sh — send a control command to the harness and print
# the response. Requires HARNESS_CONTROL=1 on the harness side.
#
# Usage: bash tools/harness-ctl.sh <command> [args...]
# Examples:
#   bash tools/harness-ctl.sh ping
#   bash tools/harness-ctl.sh get step_count
#   bash tools/harness-ctl.sh set fast_compare 1
#   bash tools/harness-ctl.sh get all

set -euo pipefail

PORT="${HARNESS_CONTROL_PORT:-7071}"
HOST="${HARNESS_CONTROL_HOST:-127.0.0.1}"

if [[ $# -lt 1 ]]; then
    echo "usage: $0 <command> [args...]" >&2
    exit 1
fi

printf '%s' "$*" | nc -u -w 1 "$HOST" "$PORT"
echo
```

Make it executable (`chmod +x` after creation).

### Phase 7 — Build and baseline

```
cd packages/emu86
make clean
make all
```

Expected: no warnings, harness compiles. If the host system's `nc` doesn't support `-u`, flag it in the triage — some distros ship `ncat` instead, which supports `-u` under the same name.

### Phase 8 — Default-off behaviour

```
./harness/harness reference/bios test/images/freedos.img
```

Expected: harness behaviour unchanged from pre-EMU-34. No port bound, no banner mentioning control plane, lockstep advances as before.

### Phase 9 — Enable and smoke test

Start harness:
```
HARNESS_CONTROL=1 HARNESS_FAST_COMPARE=1 HARNESS_STEP_LIMIT=0 \
    ./harness/harness reference/bios test/images/freedos.img
```

Note: `HARNESS_STEP_LIMIT=0` is currently broken (halts after step 1 — see dormant-issues note). Use `HARNESS_STEP_LIMIT=99999999999` for this test.

Expected on startup:
```
harness: control plane active on 127.0.0.1:7071
         Example: printf 'get all' | nc -u -w 1 127.0.0.1 7071
```

From another terminal:

```
bash tools/harness-ctl.sh ping
  → OK pong

bash tools/harness-ctl.sh help
  → OK (multi-line: commands and variables)

bash tools/harness-ctl.sh get step_count
  → OK <some large number, advancing>

bash tools/harness-ctl.sh get fast_compare
  → OK 1

bash tools/harness-ctl.sh set fast_compare 0
  → OK fast_compare=0

bash tools/harness-ctl.sh get all
  → OK (multi-line dump including fast_compare=0)

bash tools/harness-ctl.sh set step_count 0
  → ERR step_count is read-only

bash tools/harness-ctl.sh get nonexistent
  → ERR unknown variable 'nonexistent'

bash tools/harness-ctl.sh nonsense
  → ERR unknown command 'nonsense'
```

### Phase 10 — Behavioural test

With the harness running post-boot at the FreeDOS prompt (use `HARNESS_KBD_INPUT=1` and a space to skip the F5/F8 banner), toggle fast_compare and observe the compare counts:

```
bash tools/harness-ctl.sh get compare_cheap
bash tools/harness-ctl.sh get compare_full
```

Note the ratio. Toggle:
```
bash tools/harness-ctl.sh set fast_compare 0
```

Wait 10 seconds. Check again:
```
bash tools/harness-ctl.sh get compare_cheap
bash tools/harness-ctl.sh get compare_full
```

Expected: after toggle, `compare_full` climbs at the same rate as `compare_cheap` (every step does both), instead of the previous 0.4% full-rate.

### Phase 11 — Lockstep preservation

With HARNESS_KBD_INPUT=1 on and the F5/F8 banner passed via space, send a series of control commands over maybe 30 seconds while the harness is running. Expected: no divergence. The control plane is harness-internal — commands don't touch either emulator's state, so lockstep must hold throughout.

If divergence appears during control-plane traffic that wouldn't appear without it, something is leaking state between the control tick and the emulators. Investigate and triage.

### Phase 12 — Unit tests

Add a small test that exercises `harness_control_handle` directly without any socket. Create `test/unit/test_control.c`:

```c
/* Test the transport-agnostic core. Links harness.c (which needs reference
 * globals stubbed for a unit test) so we can call harness_control_handle
 * without a live emulator. Or — if linking harness.c unit-test-style is
 * painful because of reference globals — factor harness_control_handle and
 * its helpers into a separate control.c that's easier to link standalone.
 * Choose whichever is cleaner. */

TEST: ping returns pong
TEST: unknown command returns ERR
TEST: get fast_compare returns OK 0 or OK 1
TEST: set fast_compare 1 then get fast_compare returns OK 1
TEST: set fast_compare frog returns ERR expected 0 or 1
TEST: set step_count 5 returns ERR (read-only)
TEST: get unknown returns ERR unknown variable
TEST: empty command returns ERR empty
TEST: help returns multi-line response mentioning every registered command
```

If linking proves painful, extracting `harness_control_handle` and its helpers into a new `harness/control.c` and `harness/control.h` is acceptable and probably better for the WASM port anyway. Use your judgement.

### Phase 13 — Full test suite

```
make clean
make all
make test
```

Expected: all existing tests pass. New control-plane tests pass. No regression anywhere.

### Phase 14 — Commit

Commit if:
- Phase 8: default behaviour (no flag) unchanged.
- Phase 9: all smoke tests produce expected responses.
- Phase 10: fast_compare toggle visibly changes compare-count ratios.
- Phase 11: no divergence caused by control plane traffic.
- Phase 12 and 13: all tests green.
- `git status` shows:
  - `harness/harness.c` modified (and possibly new `harness/control.c`, `harness/control.h`)
  - `tools/harness-ctl.sh` created (marked executable)
  - `test/unit/test_control.c` created
  - `Makefile` possibly modified if the test added a new compilation unit
  - `tasks/emu34-task.md` created
  - No emulator source changes, no reference changes.

Commit message:

```
EMU-34: Harness control plane via UDP (transport-agnostic core)

Adds HARNESS_CONTROL=1 mode. Creates a UDP socket on 127.0.0.1:7071
(configurable via HARNESS_CONTROL_PORT). Line-based text commands:
ping, help, get, set. Exposes harness knobs (fast_compare,
heartbeat_every, step_limit) for runtime toggling, and harness state
(step_count, compare counts, enable flags) for runtime observation.

Command dispatch lives in harness_control_handle, a pure function
taking a NUL-terminated input string and writing a NUL-terminated
response. The UDP wrapper is a thin transport adapter. The future
WASM port swaps the adapter for a postMessage handler — the core
handler is unchanged.

Test client at tools/harness-ctl.sh wraps `nc -u` for shell use.

Scope:
- harness/harness.c (or split harness/control.c): core handler,
  variable table, command dispatch, UDP setup, step-loop tick.
- tools/harness-ctl.sh: shell test client.
- test/unit/test_control.c: direct unit tests of the core handler.

Verification:
- Default (no flag): no regression; harness behaviour unchanged.
- Enabled: control commands work, fast_compare toggle visibly
  changes compare-count ratios in interactive runs.
- Lockstep preserved under control plane traffic.

Follow-up:
- EMU-35: disk write sync verification.
- EMU-36: DEBUG.COM → HELLO.COM acceptance procedure.
- Future: snapshot/restore commands, reboot (needs ref-side
  longjmp surgery).
- Future WASM port: swap UDP wrapper for postMessage; core unchanged.
```

Task log entry:

```
## EMU-34
Date: {today}
Status: PASS
Test results: {N} existing + {M} new control-plane tests green
Harness: adds HARNESS_CONTROL=1 UDP control plane. Commands:
ping, help, get <name>, set <name> <value>. Transport-agnostic
core handler enables drop-in WASM port via postMessage wrapper.
Notes: Tool at tools/harness-ctl.sh wraps nc -u for shell use.
Read-only knobs: step_count, compare counts, enable flags.
Read-write knobs: fast_compare, heartbeat_every, step_limit.
```

Then:
```
mv tasks/emu34-task.md tasks/completed/
git add -A
git commit
```

Do not push.

### Phase 15 — Report (on failure)

Triage to `tasks/triage/emu34-triage-report.md` if:
- Phase 8 shows any change to default behaviour.
- Phase 9 smoke tests fail with unexpected responses.
- Phase 10 fast_compare toggle doesn't visibly affect compare-count ratios.
- Phase 11 shows divergence caused by control plane traffic.
- Phase 12 or 13 tests fail.
- The host's `nc` lacks UDP support (`-u` flag) — note this is a
  host-environment issue, not a harness bug; suggest `ncat` or
  alternative tooling in the triage.

## Out of scope — do not touch

- Emulator source (`src/emulator/`).
- Reference source (`reference/`).
- Makefile changes beyond adding the new test compilation unit.
- Snapshot, reboot, keyboard-inject commands (future tasks).
- TCP or connection-oriented transport.
- Any authentication or access control.
- Dormant concerns.

## Final note

Failure modes to watch for:

1. **UDP packet ordering at the byte level.** UDP datagrams are delivered whole or not at all — no partial reads. But if the test tool sends a command larger than the recvfrom buffer (unlikely for human-typed commands, possible for script-generated ones), the excess is silently truncated. The 512-byte inbuf is generous for any realistic command; if commands ever exceed that, it's a protocol design issue, not a bug.

2. **`nc` flavour.** BSD `nc` vs GNU `nc` vs `ncat` all behave slightly differently for UDP. The `-w 1` flag (1-second wait for response then exit) is the portable-ish incantation. If `nc` on the host doesn't work, try `ncat -u --recv-only -w 1` or similar. The harness-side code doesn't care which client sends the datagram.

3. **SOCK_NONBLOCK flag.** On Linux this is supported. On older BSDs it isn't — you'd need to fcntl(F_SETFL, O_NONBLOCK) after socket(). Linux-only is fine for the current host platform.

4. **Tick cadence.** 1000 steps between recvfrom calls means up to ~150 microseconds of command latency at peak emulator speed (6.7M steps/sec), or ~10ms during REP-heavy boot sections. Both are imperceptible. If responsiveness ever matters more, tune down to 100 — the syscall overhead is negligible for a non-blocking recvfrom with nothing waiting.

5. **Variable type narrowness.** The VT_BOOL / VT_U64 distinction is enough for v1. If future variables need other types (signed, string-valued, structured), extend the enum rather than retrofitting.

6. **The core handler sees post-step state.** Because the tick runs inside step_end after the step-count increment, `get step_count` sees the count of the just-completed step. That's the intuitive answer. If a future caller needs pre-step state, that's a different entry point.

Once EMU-34 lands, the harness has a proper control plane. Fast-compare toggle alone justifies the work — boot with full-compare paranoia, switch to fast mode at the prompt, all without restarting. EMU-35 (disk sync) and EMU-36 (DEBUG.COM acceptance) follow naturally.
