# Lite Editor — Build Specification

## For: Claude Code Agent
## Managed by: Human operator feeding tasks sequentially

---

## 0. Conventions

- Language: TypeScript (strict mode) throughout, both server and client
- Module system: ESM (`"type": "module"` in package.json)
- Build: `tsup` for server, `esbuild` for client bundle
- Test framework: `vitest` — every task MUST produce a test file as a deliverable
- Style: No framework. Vanilla TS + DOM APIs. Minimal dependencies.
- Error handling: All async ops return typed errors, never throw across module boundaries
- Naming: `kebab-case` files, `PascalCase` types, `camelCase` functions/variables

### Design Constraint: Editor Buffer Sovereignty

**THIS IS AN INVIOLABLE INVARIANT. Every task must respect it. Every test suite must verify it where applicable.**

The Ace editor buffer is sovereign. No external process — AI agent, file watcher, remote sync, WebSocket message, or any future subsystem — may alter the text content of an open editor buffer. The user's keystrokes are the only input that changes editor text, with one controlled exception (REPL/ELKs commands, under strict undo rules).

**The rules:**

1. **Remote file changes (watch events) NEVER modify the editor buffer.** If a file changes on disk while open in the editor, the editor MUST:
   - Store the incoming remote content as a `pending` version on the tab state (never applied to Ace)
   - Show a visual indicator on the tab (e.g. icon, colour change) that an external change exists
   - If the file is unmodified locally, show an inline annotation/diff gutter highlighting what changed
   - If the file IS modified locally (dirty), show a conflict indicator instead
   - The user explicitly triggers sync via a REPL command (`sync`, `accept`, `diff`) or a UI action (click the indicator)
   - Until the user acts, the editor buffer is untouched

2. **AI annotations NEVER modify the editor buffer.** AI activity is rendered exclusively through:
   - Gutter decorations (icons, colour bands)
   - Semi-transparent overlay markers (range highlights)
   - Inline "ghost text" that is visually distinct and NOT part of the document model
   - The annotation layer reads the buffer; it never writes to it

3. **REPL and ELKs commands CAN modify the editor buffer** (e.g. `replace foo bar`), but:
   - Every programmatic edit MUST go through the Ace `UndoManager`
   - Edits must be recorded as a single undo group (user presses Ctrl+Z once to reverse the entire operation)
   - The REPL must report what it changed: `Replaced 3 occurrences in /src/app.ts`
   - A future "time machine" mode (v2) will provide full operational history, but v1's UndoManager coverage is the foundation

4. **No silent writes.** Any subsystem that writes to the FS (save, REPL `new`, shell `touch`) must notify the editor state manager so it can reconcile. The editor never polls; it reacts to events, and always preserves user buffer content.

**Architectural enforcement:**

The `EditorManager` class must NOT expose any public method that directly sets buffer content from external sources. The only write paths are:
- `open(path)` — initial load (buffer was empty)
- User keystrokes (Ace's native input handling)
- `applyProgrammaticEdit(path, editFn)` — for REPL/ELKs, wraps in undo group

Remote content goes to `TabState.pendingRemote: { content: string; mtime: number } | null`, never to Ace.

This constraint lays the foundation for v2's version control integration and time-machine mode, where the full edit history (user edits, programmatic edits, remote changes) is tracked as an operational log. Getting the sovereignty model right now means v2 is an additive feature, not a rewrite.

### Testing Conventions

Every task produces test files alongside the source. The structure mirrors source:

```
test/
├── shared/
│   └── protocol.test.ts
├── server/
│   ├── transport.test.ts
│   ├── fs-api.test.ts
│   ├── file-watcher.test.ts
│   ├── shell-manager.test.ts
│   └── config.test.ts
├── client/
│   ├── transport.test.ts
│   ├── tree.test.ts
│   ├── editor.test.ts
│   ├── terminal.test.ts
│   └── repl.test.ts
└── integration/
    ├── fs-roundtrip.test.ts
    ├── shell-roundtrip.test.ts
    ├── watch-roundtrip.test.ts
    └── full-boot.test.ts
```

**Rules for test files:**

1. Each test file must be runnable in isolation: `npx vitest run test/server/fs-api.test.ts`
2. Tests that need a server instance must spin one up in `beforeAll` and tear it down in `afterAll`
3. Tests that need filesystem state must use a temp directory (`fs.mkdtemp`) cleaned up in `afterAll`
4. Tests that need a WebSocket client must create real connections (not mocks) for integration tests
5. Unit tests use mocks/stubs; integration tests use real components
6. Every `describe` block must have a comment referencing the task ID: `// TASK: PROTO-01`
7. Use `vitest` built-in `vi.fn()`, `vi.spyOn()`, `vi.useFakeTimers()` — no extra mock libraries
8. Client-side tests that need DOM: use `jsdom` environment via vitest config per-file: `// @vitest-environment jsdom`
9. For xterm.js and Ace tests that can't run headless, create a thin adapter interface and test the adapter's logic, not the terminal/editor DOM. Mark anything that truly needs a browser as `describe.skip` with a comment explaining it requires manual or Playwright verification.

**Test categories (npm scripts):**

```json
{
  "test": "vitest",
  "test:run": "vitest run",
  "test:unit": "vitest run --testPathPattern='test/(shared|server|client)/'",
  "test:integration": "vitest run --testPathPattern='test/integration/'",
  "test:watch": "vitest --watch"
}
```

---

## 1. Project Scaffold

### Task ID: `SCAFFOLD-01`

**Goal**: Create the project skeleton with build tooling, directory structure, dev scripts, and verify the test runner itself works.

**Create these files:**

```
lite-editor/
├── package.json
├── tsconfig.json
├── tsconfig.server.json
├── tsconfig.client.json
├── vitest.config.ts
├── tsup.config.ts              # server build
├── esbuild.config.ts           # client build
├── server/
│   └── index.ts                # placeholder: console.log("server")
├── client/
│   ├── index.html
│   └── app.ts                  # placeholder: console.log("client")
├── shared/
│   ├── protocol.ts             # channel constants only (stubs)
│   └── types.ts                # shared type definitions (stubs)
├── test/
│   ├── setup.ts                # global vitest setup
│   └── scaffold.test.ts        # tests for the scaffold itself
└── scripts/
    └── dev.ts                  # runs server + watches client
```

**vitest.config.ts:**

```typescript
import { defineConfig } from 'vitest/config';

export default defineConfig({
  test: {
    globals: true,
    include: ['test/**/*.test.ts'],
    testTimeout: 30000,         // shell tests can be slow
    hookTimeout: 15000,
    setupFiles: ['test/setup.ts'],
    pool: 'forks',              // isolate tests that spawn processes
  },
  resolve: {
    alias: {
      '@shared': './shared',
      '@server': './server',
      '@client': './client',
    },
  },
});
```

**package.json dependencies** (add to previous list):

```json
{
  "devDependencies": {
    "vitest": "^1.x",
    "jsdom": "^24.x",
    "@vitest/coverage-v8": "^1.x"
  }
}
```

### Deliverables

**Source files:** everything listed above.

**Test file: `test/scaffold.test.ts`**

```typescript
import { describe, it, expect } from 'vitest';
import { existsSync, readFileSync } from 'fs';
import { execSync } from 'child_process';
import path from 'path';

// TASK: SCAFFOLD-01
describe('SCAFFOLD-01: Project scaffold', () => {
  const root = path.resolve(__dirname, '..');

  it('has package.json with correct type', () => {
    const pkg = JSON.parse(
      readFileSync(path.join(root, 'package.json'), 'utf-8')
    );
    expect(pkg.type).toBe('module');
  });

  it('has required directories', () => {
    for (const dir of ['server', 'client', 'shared', 'test', 'scripts']) {
      expect(existsSync(path.join(root, dir))).toBe(true);
    }
  });

  it('typescript compiles without errors', () => {
    // Should not throw
    execSync('npx tsc --noEmit', { cwd: root, stdio: 'pipe' });
  });

  it('server builds successfully', () => {
    execSync('npx tsup', { cwd: root, stdio: 'pipe' });
  });

  it('client builds successfully', () => {
    execSync('node esbuild.config.ts', { cwd: root, stdio: 'pipe' });
  });

  it('vitest is configured correctly', () => {
    // This test existing and passing proves vitest works
    expect(true).toBe(true);
  });
});
```

**Pass criteria:** `npx vitest run test/scaffold.test.ts` — all green.

---

## 2. Shared Protocol

### Task ID: `PROTO-01`

**Goal**: Define the binary mux framing protocol and all message types, with thorough encode/decode tests.

**Source file: `shared/protocol.ts`**

```typescript
// --- Channel IDs ---
export const CH = {
  CONTROL: 0x00,
  FS:      0x01,
  SHELL:   0x02,
  AI:      0x03,
  WATCH:   0x04,
} as const;

export type ChannelId = (typeof CH)[keyof typeof CH];

// --- Frame encoding ---
// [1 byte channel] [1 byte flags] [2 bytes payload length BE] [payload]
// Max payload: 65535 bytes. Larger messages must be chunked by the caller.

export const FRAME_HEADER_SIZE = 4;
export const MAX_PAYLOAD = 65535;

export const FLAGS = {
  NONE:     0x00,
  JSON:     0x01,
  BINARY:   0x02,
  CHUNK:    0x04,
  FINAL:    0x08,
} as const;

export function encodeFrame(
  channel: ChannelId, 
  flags: number, 
  payload: Uint8Array
): Uint8Array {
  // Implementation: allocate header + payload, write BE length
}

export function decodeFrame(
  data: Uint8Array
): { channel: ChannelId; flags: number; payload: Uint8Array } {
  // Implementation: read header, slice payload
}

// Helper: encode a JSON object as a frame
export function encodeJsonFrame(channel: ChannelId, obj: unknown): Uint8Array {
  const json = new TextEncoder().encode(JSON.stringify(obj));
  return encodeFrame(channel, FLAGS.JSON, json);
}

// Helper: decode a JSON frame's payload
export function decodeJsonPayload<T>(payload: Uint8Array): T {
  return JSON.parse(new TextDecoder().decode(payload));
}

// --- RPC messages (JSON, used on CH.CONTROL and CH.FS) ---
export interface RpcRequest {
  id: number;
  method: string;
  params: Record<string, unknown>;
}

export interface RpcResponse {
  id: number;
  result?: unknown;
  error?: { code: number; message: string };
}

// --- FS types ---
export interface DirEntry {
  name: string;
  type: 'file' | 'directory' | 'symlink';
  size: number;
  mtime: number;
}

export interface FileStat {
  size: number;
  mtime: number;
  type: 'file' | 'directory' | 'symlink';
}

// --- Watch events (CH.WATCH) ---
export interface WatchEvent {
  event: 'add' | 'change' | 'unlink' | 'addDir' | 'unlinkDir';
  path: string;
  stat?: FileStat;
}

// --- Control messages (CH.CONTROL) ---
export type ControlMessage = SessionInit | SessionResume | ResizeMessage | PingPong;

export interface SessionInit {
  type: 'session-init';
  sessionId: string;
  projectRoot: string;
  resumed: boolean;
}

export interface SessionResume {
  type: 'session-resume';
  sessionId: string;
}

export interface ResizeMessage {
  type: 'resize';
  cols: number;
  rows: number;
}

export interface PingPong {
  type: 'ping' | 'pong';
  ts: number;
}

// --- AI annotation types (CH.AI) ---
export interface AiAnnotation {
  file: string;
  annotations: Array<{
    startRow: number;
    endRow: number;
    type: 'ai-active' | 'ai-suggestion' | 'ai-complete';
    label: string;
  }>;
}

// --- Editor Buffer Sovereignty types ---
// These live in the protocol so both server and client agree on the shape.

// Stored on TabState when a remote change arrives for an open file.
// NEVER applied to the Ace buffer automatically.
export interface PendingRemote {
  content: string;
  mtime: number;
  detectedAt: number;    // client timestamp when the change was detected
}

// Result of diffing local buffer against pending remote content.
export type ConflictState = 
  | 'clean'              // buffer matches disk — no action needed
  | 'remote-only'        // buffer unmodified locally, remote has changes — safe to sync
  | 'local-only'         // buffer modified locally, disk unchanged — normal editing
  | 'conflict';          // both local and remote modified — user must resolve

// Programmatic edit request (REPL/ELKs → editor).
// Must always go through UndoManager as a single group.
export interface ProgrammaticEdit {
  source: 'repl' | 'elks' | 'plugin';
  description: string;   // human-readable, shown in REPL output
  branchId: string | null; // null in v1. Time machine branch ID in v2+.
  edits: Array<{
    startRow: number;
    startCol: number;
    endRow: number;
    endCol: number;
    text: string;          // replacement text
  }>;
}
```

**Source file: `shared/types.ts`**

```typescript
export type {
  ChannelId, RpcRequest, RpcResponse, DirEntry, FileStat,
  WatchEvent, ControlMessage, SessionInit, SessionResume,
  ResizeMessage, AiAnnotation, PendingRemote, ConflictState,
  ProgrammaticEdit
} from './protocol.js';

export type Result<T, E = Error> =
  | { ok: true; value: T }
  | { ok: false; error: E };

export function ok<T>(value: T): Result<T, never> {
  return { ok: true, value };
}

export function err<E>(error: E): Result<never, E> {
  return { ok: false, error };
}
```

### Deliverables

**Test file: `test/shared/protocol.test.ts`**

```typescript
import { describe, it, expect } from 'vitest';
import {
  CH, FLAGS, FRAME_HEADER_SIZE, MAX_PAYLOAD,
  encodeFrame, decodeFrame, encodeJsonFrame, decodeJsonPayload,
} from '@shared/protocol';
import type { RpcRequest, RpcResponse, PendingRemote, ProgrammaticEdit, ConflictState } from '@shared/protocol';

// TASK: PROTO-01
describe('PROTO-01: Frame encoding', () => {

  it('round-trips a JSON payload', () => {
    const payload = new TextEncoder().encode('{"hello":"world"}');
    const frame = encodeFrame(CH.FS, FLAGS.JSON, payload);
    const decoded = decodeFrame(frame);
    expect(decoded.channel).toBe(CH.FS);
    expect(decoded.flags).toBe(FLAGS.JSON);
    expect(decoded.payload).toEqual(payload);
  });

  it('round-trips binary data', () => {
    const payload = new Uint8Array(1000);
    crypto.getRandomValues(payload);
    const frame = encodeFrame(CH.SHELL, FLAGS.BINARY, payload);
    const decoded = decodeFrame(frame);
    expect(decoded.channel).toBe(CH.SHELL);
    expect(decoded.flags).toBe(FLAGS.BINARY);
    expect(decoded.payload).toEqual(payload);
  });

  it('encodes correct header bytes', () => {
    const payload = new Uint8Array([0x41, 0x42]); // "AB"
    const frame = encodeFrame(CH.CONTROL, FLAGS.JSON, payload);
    expect(frame[0]).toBe(CH.CONTROL);       // channel
    expect(frame[1]).toBe(FLAGS.JSON);        // flags
    expect(frame[2]).toBe(0x00);              // length high byte
    expect(frame[3]).toBe(0x02);              // length low byte
    expect(frame.slice(4)).toEqual(payload);
  });

  it('handles empty payload', () => {
    const frame = encodeFrame(CH.CONTROL, FLAGS.NONE, new Uint8Array(0));
    expect(frame.length).toBe(FRAME_HEADER_SIZE);
    const decoded = decodeFrame(frame);
    expect(decoded.payload.length).toBe(0);
  });

  it('handles max payload (65535 bytes)', () => {
    const payload = new Uint8Array(MAX_PAYLOAD);
    const frame = encodeFrame(CH.FS, FLAGS.BINARY, payload);
    const decoded = decodeFrame(frame);
    expect(decoded.payload.length).toBe(MAX_PAYLOAD);
  });

  it('rejects payload exceeding max size', () => {
    const payload = new Uint8Array(MAX_PAYLOAD + 1);
    expect(() => encodeFrame(CH.FS, FLAGS.BINARY, payload)).toThrow();
  });

  it('rejects truncated frame (< 4 bytes)', () => {
    expect(() => decodeFrame(new Uint8Array([0x00, 0x01]))).toThrow();
  });

  it('rejects frame with length mismatch', () => {
    // Header says 10 bytes, but only 5 provided
    const bad = new Uint8Array([CH.FS, FLAGS.JSON, 0x00, 0x0A, 1, 2, 3, 4, 5]);
    expect(() => decodeFrame(bad)).toThrow();
  });

  it('preserves all channel IDs', () => {
    for (const ch of Object.values(CH)) {
      const frame = encodeFrame(ch, FLAGS.NONE, new Uint8Array([0xFF]));
      const decoded = decodeFrame(frame);
      expect(decoded.channel).toBe(ch);
    }
  });

  it('preserves combined flags', () => {
    const flags = FLAGS.JSON | FLAGS.CHUNK;
    const frame = encodeFrame(CH.FS, flags, new Uint8Array([1]));
    const decoded = decodeFrame(frame);
    expect(decoded.flags).toBe(flags);
  });
});

// TASK: PROTO-01
describe('PROTO-01: JSON helpers', () => {

  it('encodeJsonFrame produces valid JSON frame', () => {
    const obj = { id: 1, method: 'readdir', params: { path: '/' } };
    const frame = encodeJsonFrame(CH.FS, obj);
    const decoded = decodeFrame(frame);
    expect(decoded.flags & FLAGS.JSON).toBeTruthy();
    const parsed = decodeJsonPayload(decoded.payload);
    expect(parsed).toEqual(obj);
  });

  it('handles nested objects', () => {
    const obj = { a: { b: { c: [1, 2, 3] } } };
    const frame = encodeJsonFrame(CH.CONTROL, obj);
    const decoded = decodeFrame(frame);
    expect(decodeJsonPayload(decoded.payload)).toEqual(obj);
  });

  it('handles unicode strings', () => {
    const obj = { text: '日本語テスト 🎉' };
    const frame = encodeJsonFrame(CH.AI, obj);
    const decoded = decodeFrame(frame);
    expect(decodeJsonPayload<typeof obj>(decoded.payload).text).toBe('日本語テスト 🎉');
  });
});

// TASK: PROTO-01
describe('PROTO-01: RPC message shapes', () => {

  it('RpcRequest round-trips through JSON', () => {
    const req: RpcRequest = { id: 42, method: 'readdir', params: { path: '/src' } };
    const json = JSON.stringify(req);
    const parsed: RpcRequest = JSON.parse(json);
    expect(parsed.id).toBe(42);
    expect(parsed.method).toBe('readdir');
    expect(parsed.params.path).toBe('/src');
  });

  it('RpcResponse with result', () => {
    const res: RpcResponse = { id: 42, result: [{ name: 'foo', type: 'file' }] };
    const parsed: RpcResponse = JSON.parse(JSON.stringify(res));
    expect(parsed.result).toBeDefined();
    expect(parsed.error).toBeUndefined();
  });

  it('RpcResponse with error', () => {
    const res: RpcResponse = { id: 42, error: { code: 404, message: 'Not found' } };
    const parsed: RpcResponse = JSON.parse(JSON.stringify(res));
    expect(parsed.result).toBeUndefined();
    expect(parsed.error?.code).toBe(404);
  });
});

// TASK: PROTO-01
describe('PROTO-01: Buffer sovereignty types', () => {

  it('PendingRemote shape round-trips through JSON', () => {
    const pending: PendingRemote = { content: 'new stuff', mtime: 1700000000, detectedAt: Date.now() };
    const parsed = JSON.parse(JSON.stringify(pending));
    expect(parsed.content).toBe('new stuff');
    expect(parsed.mtime).toBe(1700000000);
    expect(typeof parsed.detectedAt).toBe('number');
  });

  it('ProgrammaticEdit shape round-trips through JSON', () => {
    const edit: ProgrammaticEdit = {
      source: 'repl',
      branchId: null,
      description: 'Replace foo with bar',
      branchId: null,
      edits: [
        { startRow: 0, startCol: 5, endRow: 0, endCol: 8, text: 'bar' },
        { startRow: 3, startCol: 0, endRow: 3, endCol: 3, text: 'bar' },
      ],
    };
    const parsed = JSON.parse(JSON.stringify(edit));
    expect(parsed.source).toBe('repl');
    expect(parsed.branchId).toBeNull();
    expect(parsed.edits.length).toBe(2);
    expect(parsed.edits[0].text).toBe('bar');
    expect(parsed.edits[1].startRow).toBe(3);
  });

  it('ConflictState covers all expected values', () => {
    const states: ConflictState[] = ['clean', 'remote-only', 'local-only', 'conflict'];
    expect(states.length).toBe(4);
    // Type assertion — if this compiles, the type is correct
    const s: ConflictState = 'conflict';
    expect(s).toBe('conflict');
  });
});
```

**Pass criteria:** `npx vitest run test/shared/protocol.test.ts` — all green.

---

## 3. Server Configuration

### Task ID: `CONFIG-01`

**Source file: `server/config.ts`**

```typescript
export interface Config {
  port: number;            // default: 3000, env: LITE_PORT
  host: string;            // default: "0.0.0.0", env: LITE_HOST
  projectRoot: string;     // REQUIRED: first CLI arg or env: LITE_PROJECT
  sessionTimeout: number;  // default: 300000 (5min), env: LITE_SESSION_TIMEOUT
  watchIgnore: string[];   // default: ["node_modules", ".git", "dist"]
  shell: string;           // default: env SHELL or "/bin/bash"
}

export function loadConfig(argv?: string[]): Config {
  // Parse process.argv (or provided argv), merge env vars, validate
  // Throws with helpful message if projectRoot missing or invalid
}
```

### Deliverables

**Test file: `test/server/config.test.ts`**

```typescript
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import { loadConfig } from '@server/config';
import fs from 'fs';
import os from 'os';
import path from 'path';

// TASK: CONFIG-01
describe('CONFIG-01: Server configuration', () => {
  let tmpDir: string;

  beforeEach(() => {
    tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'lite-test-'));
  });

  afterEach(() => {
    fs.rmSync(tmpDir, { recursive: true, force: true });
    vi.unstubAllEnvs();
  });

  it('parses project root from CLI args', () => {
    const config = loadConfig(['node', 'index.js', tmpDir]);
    expect(config.projectRoot).toBe(fs.realpathSync(tmpDir));
  });

  it('uses default port 3000', () => {
    const config = loadConfig(['node', 'index.js', tmpDir]);
    expect(config.port).toBe(3000);
  });

  it('reads port from --port flag', () => {
    const config = loadConfig(['node', 'index.js', tmpDir, '--port', '4000']);
    expect(config.port).toBe(4000);
  });

  it('reads port from LITE_PORT env var', () => {
    vi.stubEnv('LITE_PORT', '5000');
    const config = loadConfig(['node', 'index.js', tmpDir]);
    expect(config.port).toBe(5000);
  });

  it('CLI --port overrides LITE_PORT env', () => {
    vi.stubEnv('LITE_PORT', '5000');
    const config = loadConfig(['node', 'index.js', tmpDir, '--port', '6000']);
    expect(config.port).toBe(6000);
  });

  it('reads project root from LITE_PROJECT env if no CLI arg', () => {
    vi.stubEnv('LITE_PROJECT', tmpDir);
    const config = loadConfig(['node', 'index.js']);
    expect(config.projectRoot).toBe(fs.realpathSync(tmpDir));
  });

  it('throws if no project root provided', () => {
    expect(() => loadConfig(['node', 'index.js'])).toThrow(/project/i);
  });

  it('throws if project root does not exist', () => {
    expect(() => loadConfig(['node', 'index.js', '/nonexistent/path/xyz']))
      .toThrow(/does not exist|not found/i);
  });

  it('throws if project root is a file not a directory', () => {
    const file = path.join(tmpDir, 'file.txt');
    fs.writeFileSync(file, 'hello');
    expect(() => loadConfig(['node', 'index.js', file]))
      .toThrow(/not a directory|directory/i);
  });

  it('defaults session timeout to 300000ms', () => {
    const config = loadConfig(['node', 'index.js', tmpDir]);
    expect(config.sessionTimeout).toBe(300000);
  });

  it('defaults shell to $SHELL or /bin/bash', () => {
    const config = loadConfig(['node', 'index.js', tmpDir]);
    expect(config.shell).toBe(process.env.SHELL || '/bin/bash');
  });

  it('includes default watchIgnore patterns', () => {
    const config = loadConfig(['node', 'index.js', tmpDir]);
    expect(config.watchIgnore).toContain('node_modules');
    expect(config.watchIgnore).toContain('.git');
  });
});
```

**Pass criteria:** `npx vitest run test/server/config.test.ts` — all green.

---

## 4. Transport Layer — Server

### Task ID: `TRANSPORT-SERVER-01`

**Source file: `server/transport.ts`**

Implements `ServerTransport` and `Session` interfaces. WebSocket server on `/ws` path, binary mux/demux using the frame protocol from PROTO-01, session management with timeout and resume.

```typescript
export interface ServerTransport {
  onConnection(handler: (session: Session) => void): void;
  send(sessionId: string, channel: ChannelId, flags: number, payload: Uint8Array): void;
  onMessage(sessionId: string, channel: ChannelId, handler: (data: Uint8Array, flags: number) => void): void;
  close(): Promise<void>;
}

export interface Session {
  readonly id: string;
  readonly isResumed: boolean;
  send(channel: ChannelId, flags: number, payload: Uint8Array): void;
  sendJson(channel: ChannelId, obj: unknown): void;
  onMessage(channel: ChannelId, handler: (data: Uint8Array, flags: number) => void): void;
  onClose(handler: () => void): void;
}
```

**Behaviour:**

1. `ws` server listens for upgrade on `/ws`
2. On new connection with no session ID → generate UUID, create Session, send `session-init` on CH.CONTROL
3. On connection with session ID query param → look up existing Session, reattach WebSocket, send `session-init` with `resumed: true`
4. Incoming binary WebSocket messages → `decodeFrame` → route to channel handlers
5. Outgoing: `encodeFrame` → `ws.send(binary)`
6. Ping/pong keepalive every 30 seconds on CH.CONTROL
7. On WebSocket close → mark Session as disconnected, hold Session in memory for 5 minutes, then discard

**Also update `server/index.ts`** to create HTTP server, attach WebSocket upgrade, serve static files.

### Deliverables

**Test file: `test/server/transport.test.ts`**

```typescript
import { describe, it, expect, beforeAll, afterAll, vi } from 'vitest';
import http from 'http';
import WebSocket from 'ws';
import { ServerTransport } from '@server/transport';
import { CH, FLAGS, encodeFrame, decodeFrame, decodeJsonPayload, encodeJsonFrame } from '@shared/protocol';
import type { SessionInit } from '@shared/protocol';

// TASK: TRANSPORT-SERVER-01
describe('TRANSPORT-SERVER-01: Server transport', () => {
  let server: http.Server;
  let transport: ServerTransport;
  let port: number;

  beforeAll(async () => {
    server = http.createServer();
    transport = new ServerTransport(server);
    await new Promise<void>(resolve => {
      server.listen(0, '127.0.0.1', () => {
        port = (server.address() as any).port;
        resolve();
      });
    });
  });

  afterAll(async () => {
    await transport.close();
    await new Promise<void>(resolve => server.close(() => resolve()));
  });

  function connectWs(sessionId?: string): Promise<WebSocket> {
    const url = `ws://127.0.0.1:${port}/ws${sessionId ? `?sessionId=${sessionId}` : ''}`;
    const ws = new WebSocket(url);
    ws.binaryType = 'arraybuffer';
    return new Promise((resolve, reject) => {
      ws.on('open', () => resolve(ws));
      ws.on('error', reject);
    });
  }

  function readFrame(ws: WebSocket): Promise<ReturnType<typeof decodeFrame>> {
    return new Promise((resolve) => {
      ws.once('message', (data: ArrayBuffer) => {
        resolve(decodeFrame(new Uint8Array(data)));
      });
    });
  }

  it('accepts connection and sends session-init', async () => {
    const ws = await connectWs();
    const frame = await readFrame(ws);
    expect(frame.channel).toBe(CH.CONTROL);
    const init = decodeJsonPayload<SessionInit>(frame.payload);
    expect(init.type).toBe('session-init');
    expect(init.sessionId).toBeTruthy();
    expect(typeof init.sessionId).toBe('string');
    expect(init.resumed).toBe(false);
    ws.close();
  });

  it('generates unique session IDs', async () => {
    const ws1 = await connectWs();
    const frame1 = await readFrame(ws1);
    const init1 = decodeJsonPayload<SessionInit>(frame1.payload);

    const ws2 = await connectWs();
    const frame2 = await readFrame(ws2);
    const init2 = decodeJsonPayload<SessionInit>(frame2.payload);

    expect(init1.sessionId).not.toBe(init2.sessionId);
    ws1.close();
    ws2.close();
  });

  it('resumes existing session by ID', async () => {
    const ws1 = await connectWs();
    const frame1 = await readFrame(ws1);
    const init1 = decodeJsonPayload<SessionInit>(frame1.payload);
    const sessionId = init1.sessionId;
    ws1.close();
    await new Promise(r => setTimeout(r, 100));

    const ws2 = await connectWs(sessionId);
    const frame2 = await readFrame(ws2);
    const init2 = decodeJsonPayload<SessionInit>(frame2.payload);
    expect(init2.sessionId).toBe(sessionId);
    expect(init2.resumed).toBe(true);
    ws2.close();
  });

  it('routes messages to correct channel handlers', async () => {
    const received: { channel: number; data: string }[] = [];

    transport.onConnection((session) => {
      session.onMessage(CH.FS, (data) => {
        received.push({ channel: CH.FS, data: new TextDecoder().decode(data) });
      });
      session.onMessage(CH.SHELL, (data) => {
        received.push({ channel: CH.SHELL, data: new TextDecoder().decode(data) });
      });
    });

    const ws = await connectWs();
    await readFrame(ws); // consume session-init

    ws.send(encodeFrame(CH.FS, FLAGS.JSON, new TextEncoder().encode('fs-msg')));
    await new Promise(r => setTimeout(r, 50));

    ws.send(encodeFrame(CH.SHELL, FLAGS.BINARY, new TextEncoder().encode('shell-msg')));
    await new Promise(r => setTimeout(r, 50));

    expect(received).toContainEqual({ channel: CH.FS, data: 'fs-msg' });
    expect(received).toContainEqual({ channel: CH.SHELL, data: 'shell-msg' });

    const fsMessages = received.filter(r => r.channel === CH.FS);
    expect(fsMessages.every(m => m.data !== 'shell-msg')).toBe(true);

    ws.close();
  });

  it('server sends frames to client', async () => {
    let capturedSession: any;
    transport.onConnection((session) => {
      capturedSession = session;
    });

    const ws = await connectWs();
    await readFrame(ws); // session-init
    await new Promise(r => setTimeout(r, 50));

    capturedSession.sendJson(CH.WATCH, { event: 'change', path: 'test.ts' });
    const frame = await readFrame(ws);
    expect(frame.channel).toBe(CH.WATCH);
    const payload = decodeJsonPayload<any>(frame.payload);
    expect(payload.event).toBe('change');
    expect(payload.path).toBe('test.ts');

    ws.close();
  });

  it('handles client disconnect gracefully', async () => {
    let closeCallCount = 0;
    transport.onConnection((session) => {
      session.onClose(() => { closeCallCount++; });
    });

    const ws = await connectWs();
    await readFrame(ws);
    ws.close();
    await new Promise(r => setTimeout(r, 200));
    expect(closeCallCount).toBeGreaterThanOrEqual(1);
  });
});
```

**Pass criteria:** `npx vitest run test/server/transport.test.ts` — all green.

---

## 5. Transport Layer — Client

### Task ID: `TRANSPORT-CLIENT-01`

**Source files:**
- `client/transport.ts` — WebSocket transport with auto-reconnect
- `client/rpc.ts` — JSON-RPC client over the transport

```typescript
// client/transport.ts
export class ClientTransport {
  // Accept a WebSocket constructor for testability (browser native vs ws package)
  constructor(wsConstructor?: typeof WebSocket);
  connect(url: string, sessionId?: string): void;
  send(channel: ChannelId, flags: number, payload: Uint8Array): void;
  sendJson(channel: ChannelId, obj: unknown): void;
  onMessage(channel: ChannelId, handler: (data: Uint8Array, flags: number) => void): void;
  onStateChange(handler: (state: 'connecting' | 'open' | 'closed') => void): void;
  readonly state: 'connecting' | 'open' | 'closed';
  readonly sessionId: string | null;
  close(): void;
}

// client/rpc.ts
export class RpcClient {
  constructor(transport: ClientTransport, channel: ChannelId);
  call<T>(method: string, params: Record<string, unknown>, opts?: { timeout?: number }): Promise<T>;
  onNotification(handler: (method: string, params: unknown) => void): void;
}
```

### Deliverables

**Test file: `test/client/transport.test.ts`**

```typescript
import { describe, it, expect, beforeAll, afterAll, vi, beforeEach, afterEach } from 'vitest';
import http from 'http';
import { WebSocketServer, WebSocket as WsWebSocket } from 'ws';
import { CH, FLAGS, encodeFrame, decodeFrame, encodeJsonFrame, decodeJsonPayload } from '@shared/protocol';
import { ClientTransport } from '@client/transport';
import { RpcClient } from '@client/rpc';

// TASK: TRANSPORT-CLIENT-01
describe('TRANSPORT-CLIENT-01: Client transport', () => {
  let httpServer: http.Server;
  let wss: WebSocketServer;
  let port: number;
  let lastServerWs: WsWebSocket | null = null;

  beforeAll(async () => {
    httpServer = http.createServer();
    wss = new WebSocketServer({ server: httpServer, path: '/ws' });

    wss.on('connection', (ws, req) => {
      lastServerWs = ws;
      ws.binaryType = 'arraybuffer';
      const sessionId = new URL(req.url!, 'http://localhost').searchParams.get('sessionId') || crypto.randomUUID();
      const resumed = req.url!.includes('sessionId=');
      ws.send(encodeJsonFrame(CH.CONTROL, {
        type: 'session-init',
        sessionId,
        projectRoot: '/test',
        resumed,
      }));
    });

    await new Promise<void>(resolve => {
      httpServer.listen(0, '127.0.0.1', () => {
        port = (httpServer.address() as any).port;
        resolve();
      });
    });
  });

  afterAll(async () => {
    wss.close();
    await new Promise<void>(r => httpServer.close(() => r()));
  });

  it('connects and receives session ID', async () => {
    const transport = new ClientTransport(WsWebSocket as any);
    const states: string[] = [];
    transport.onStateChange(s => states.push(s));

    transport.connect(`ws://127.0.0.1:${port}/ws`);
    await vi.waitUntil(() => transport.state === 'open', { timeout: 3000 });

    expect(transport.sessionId).toBeTruthy();
    expect(states).toContain('open');
    transport.close();
  });

  it('sends and receives frames on correct channels', async () => {
    const transport = new ClientTransport(WsWebSocket as any);
    transport.connect(`ws://127.0.0.1:${port}/ws`);
    await vi.waitUntil(() => transport.state === 'open', { timeout: 3000 });

    const fsMessages: string[] = [];
    const shellMessages: string[] = [];
    transport.onMessage(CH.FS, (data) => fsMessages.push(new TextDecoder().decode(data)));
    transport.onMessage(CH.SHELL, (data) => shellMessages.push(new TextDecoder().decode(data)));

    lastServerWs!.send(encodeFrame(CH.FS, FLAGS.JSON, new TextEncoder().encode('"fs-data"')));
    lastServerWs!.send(encodeFrame(CH.SHELL, FLAGS.BINARY, new TextEncoder().encode('shell-data')));
    await new Promise(r => setTimeout(r, 100));

    expect(fsMessages).toContain('"fs-data"');
    expect(shellMessages).toContain('shell-data');
    expect(fsMessages).not.toContain('shell-data');

    transport.close();
  });

  it('auto-reconnects on disconnect', async () => {
    const transport = new ClientTransport(WsWebSocket as any);
    transport.connect(`ws://127.0.0.1:${port}/ws`);
    await vi.waitUntil(() => transport.state === 'open', { timeout: 3000 });

    const sessionId = transport.sessionId;
    lastServerWs!.close();

    await vi.waitUntil(() => transport.state === 'closed', { timeout: 2000 });
    await vi.waitUntil(() => transport.state === 'open', { timeout: 10000 });

    expect(transport.sessionId).toBe(sessionId);
    transport.close();
  });
});

// TASK: TRANSPORT-CLIENT-01
describe('TRANSPORT-CLIENT-01: RPC client', () => {
  let httpServer: http.Server;
  let wss: WebSocketServer;
  let port: number;

  beforeAll(async () => {
    httpServer = http.createServer();
    wss = new WebSocketServer({ server: httpServer, path: '/ws' });

    wss.on('connection', (ws) => {
      ws.binaryType = 'arraybuffer';
      ws.send(encodeJsonFrame(CH.CONTROL, {
        type: 'session-init', sessionId: 'rpc-test', projectRoot: '/', resumed: false,
      }));

      ws.on('message', (raw: ArrayBuffer) => {
        const frame = decodeFrame(new Uint8Array(raw));
        if (frame.channel === CH.FS && (frame.flags & FLAGS.JSON)) {
          const req = decodeJsonPayload<any>(frame.payload);
          if (req.method === 'echo') {
            ws.send(encodeJsonFrame(CH.FS, { id: req.id, result: req.params }));
          }
          if (req.method === 'error') {
            ws.send(encodeJsonFrame(CH.FS, { id: req.id, error: { code: 500, message: 'test error' } }));
          }
          // 'noop' — deliberately no response (for timeout test)
        }
      });
    });

    await new Promise<void>(resolve => {
      httpServer.listen(0, '127.0.0.1', () => {
        port = (httpServer.address() as any).port;
        resolve();
      });
    });
  });

  afterAll(async () => {
    wss.close();
    await new Promise<void>(r => httpServer.close(() => r()));
  });

  function createClient(): { transport: ClientTransport; rpc: RpcClient } {
    const transport = new ClientTransport(WsWebSocket as any);
    transport.connect(`ws://127.0.0.1:${port}/ws`);
    const rpc = new RpcClient(transport, CH.FS);
    return { transport, rpc };
  }

  it('call resolves with result', async () => {
    const { transport, rpc } = createClient();
    await vi.waitUntil(() => transport.state === 'open', { timeout: 3000 });
    const result = await rpc.call('echo', { msg: 'hello' });
    expect(result).toEqual({ msg: 'hello' });
    transport.close();
  });

  it('call rejects on error response', async () => {
    const { transport, rpc } = createClient();
    await vi.waitUntil(() => transport.state === 'open', { timeout: 3000 });
    await expect(rpc.call('error', {})).rejects.toThrow(/test error/);
    transport.close();
  });

  it('call rejects on timeout', async () => {
    const { transport, rpc } = createClient();
    await vi.waitUntil(() => transport.state === 'open', { timeout: 3000 });
    await expect(rpc.call('noop', {}, { timeout: 500 })).rejects.toThrow(/timeout/i);
    transport.close();
  });

  it('concurrent calls resolve independently', async () => {
    const { transport, rpc } = createClient();
    await vi.waitUntil(() => transport.state === 'open', { timeout: 3000 });
    const results = await Promise.all([
      rpc.call('echo', { n: 1 }),
      rpc.call('echo', { n: 2 }),
      rpc.call('echo', { n: 3 }),
      rpc.call('echo', { n: 4 }),
      rpc.call('echo', { n: 5 }),
    ]);
    expect(results).toEqual([{ n: 1 }, { n: 2 }, { n: 3 }, { n: 4 }, { n: 5 }]);
    transport.close();
  });
});
```

**Pass criteria:** `npx vitest run test/client/transport.test.ts` — all green.

---

## 6. File System API — Server

### Task ID: `FS-SERVER-01`

**Source file: `server/fs-api.ts`**

All paths sandboxed to projectRoot. Binary detection for encoding. Recursive mkdir.

```typescript
export class FsApi {
  constructor(projectRoot: string);
  attach(session: Session): void;
}
```

**RPC Methods:** readdir, readFile, writeFile, mkdir, rename, unlink, stat — all as previously specified.

### Deliverables

**Test file: `test/server/fs-api.test.ts`**

```typescript
import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import fs from 'fs';
import os from 'os';
import path from 'path';
import http from 'http';
import WebSocket from 'ws';
import { ServerTransport } from '@server/transport';
import { FsApi } from '@server/fs-api';
import { ClientTransport } from '@client/transport';
import { RpcClient } from '@client/rpc';
import { CH } from '@shared/protocol';
import type { DirEntry, FileStat } from '@shared/protocol';

// TASK: FS-SERVER-01
describe('FS-SERVER-01: File system API', () => {
  let tmpDir: string;
  let server: http.Server;
  let transport: ServerTransport;
  let fsApi: FsApi;
  let clientTransport: ClientTransport;
  let rpc: RpcClient;
  let port: number;

  beforeAll(async () => {
    tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'lite-fs-test-'));
    fs.mkdirSync(path.join(tmpDir, 'src'));
    fs.writeFileSync(path.join(tmpDir, 'README.md'), '# Hello');
    fs.writeFileSync(path.join(tmpDir, 'src', 'index.ts'), 'console.log("hi");');
    fs.writeFileSync(path.join(tmpDir, 'src', 'app.ts'), 'export default {};');
    const pngHeader = Buffer.from([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00]);
    fs.writeFileSync(path.join(tmpDir, 'icon.png'), pngHeader);

    server = http.createServer();
    transport = new ServerTransport(server);
    fsApi = new FsApi(tmpDir);
    transport.onConnection((session) => { fsApi.attach(session); });

    await new Promise<void>(resolve => {
      server.listen(0, '127.0.0.1', () => {
        port = (server.address() as any).port;
        resolve();
      });
    });

    clientTransport = new ClientTransport(WebSocket as any);
    clientTransport.connect(`ws://127.0.0.1:${port}/ws`);
    rpc = new RpcClient(clientTransport, CH.FS);
    await vi.waitUntil(() => clientTransport.state === 'open', { timeout: 3000 });
  });

  afterAll(async () => {
    clientTransport.close();
    await transport.close();
    await new Promise<void>(r => server.close(() => r()));
    fs.rmSync(tmpDir, { recursive: true, force: true });
  });

  // --- readdir ---
  it('readdir lists root directory', async () => {
    const entries = await rpc.call<DirEntry[]>('readdir', { path: '/' });
    const names = entries.map(e => e.name).sort();
    expect(names).toContain('README.md');
    expect(names).toContain('src');
    expect(names).toContain('icon.png');
  });

  it('readdir lists subdirectory', async () => {
    const entries = await rpc.call<DirEntry[]>('readdir', { path: '/src' });
    expect(entries.map(e => e.name).sort()).toEqual(['app.ts', 'index.ts']);
  });

  it('readdir returns correct types', async () => {
    const entries = await rpc.call<DirEntry[]>('readdir', { path: '/' });
    expect(entries.find(e => e.name === 'src')?.type).toBe('directory');
    expect(entries.find(e => e.name === 'README.md')?.type).toBe('file');
  });

  it('readdir on file errors', async () => {
    await expect(rpc.call('readdir', { path: '/README.md' })).rejects.toThrow();
  });

  it('readdir on non-existent path errors', async () => {
    await expect(rpc.call('readdir', { path: '/nonexistent' })).rejects.toThrow();
  });

  // --- readFile ---
  it('readFile reads text file', async () => {
    const result = await rpc.call<{ content: string; encoding: string }>('readFile', { path: '/README.md' });
    expect(result.content).toBe('# Hello');
    expect(result.encoding).toBe('utf-8');
  });

  it('readFile reads file in subdirectory', async () => {
    const result = await rpc.call<{ content: string; encoding: string }>('readFile', { path: '/src/index.ts' });
    expect(result.content).toBe('console.log("hi");');
  });

  it('readFile reads binary file as base64', async () => {
    const result = await rpc.call<{ content: string; encoding: string }>('readFile', { path: '/icon.png' });
    expect(result.encoding).toBe('base64');
    const decoded = Buffer.from(result.content, 'base64');
    expect(decoded[0]).toBe(0x89);
    expect(decoded[1]).toBe(0x50);
  });

  it('readFile on non-existent file errors', async () => {
    await expect(rpc.call('readFile', { path: '/nope.txt' })).rejects.toThrow();
  });

  // --- writeFile ---
  it('writeFile creates new file', async () => {
    await rpc.call('writeFile', { path: '/new.txt', content: 'new content' });
    expect(fs.readFileSync(path.join(tmpDir, 'new.txt'), 'utf-8')).toBe('new content');
  });

  it('writeFile overwrites existing file', async () => {
    await rpc.call('writeFile', { path: '/new.txt', content: 'updated' });
    expect(fs.readFileSync(path.join(tmpDir, 'new.txt'), 'utf-8')).toBe('updated');
  });

  it('writeFile creates parent directories', async () => {
    await rpc.call('writeFile', { path: '/deep/nested/file.txt', content: 'deep' });
    expect(fs.readFileSync(path.join(tmpDir, 'deep', 'nested', 'file.txt'), 'utf-8')).toBe('deep');
  });

  // --- mkdir ---
  it('mkdir creates directory', async () => {
    await rpc.call('mkdir', { path: '/newdir' });
    expect(fs.statSync(path.join(tmpDir, 'newdir')).isDirectory()).toBe(true);
  });

  it('mkdir creates nested directories', async () => {
    await rpc.call('mkdir', { path: '/a/b/c' });
    expect(fs.statSync(path.join(tmpDir, 'a', 'b', 'c')).isDirectory()).toBe(true);
  });

  // --- rename ---
  it('rename moves file', async () => {
    fs.writeFileSync(path.join(tmpDir, 'old.txt'), 'data');
    await rpc.call('rename', { oldPath: '/old.txt', newPath: '/renamed.txt' });
    expect(fs.existsSync(path.join(tmpDir, 'old.txt'))).toBe(false);
    expect(fs.readFileSync(path.join(tmpDir, 'renamed.txt'), 'utf-8')).toBe('data');
  });

  // --- unlink ---
  it('unlink deletes file', async () => {
    fs.writeFileSync(path.join(tmpDir, 'deleteme.txt'), 'bye');
    await rpc.call('unlink', { path: '/deleteme.txt' });
    expect(fs.existsSync(path.join(tmpDir, 'deleteme.txt'))).toBe(false);
  });

  it('unlink on non-existent file errors', async () => {
    await expect(rpc.call('unlink', { path: '/ghost.txt' })).rejects.toThrow();
  });

  // --- stat ---
  it('stat returns file stats', async () => {
    const stat = await rpc.call<FileStat>('stat', { path: '/README.md' });
    expect(stat.type).toBe('file');
    expect(stat.size).toBeGreaterThan(0);
    expect(stat.mtime).toBeGreaterThan(0);
  });

  it('stat returns directory stats', async () => {
    const stat = await rpc.call<FileStat>('stat', { path: '/src' });
    expect(stat.type).toBe('directory');
  });

  // --- path traversal ---
  it('blocks ../ traversal on readFile', async () => {
    await expect(rpc.call('readFile', { path: '../../etc/passwd' })).rejects.toThrow();
  });

  it('blocks ../ traversal on readdir', async () => {
    await expect(rpc.call('readdir', { path: '../' })).rejects.toThrow();
  });

  it('blocks ../ traversal on writeFile', async () => {
    await expect(rpc.call('writeFile', { path: '../../evil.txt', content: 'bad' })).rejects.toThrow();
  });
});
```

**Pass criteria:** `npx vitest run test/server/fs-api.test.ts` — all green.

---

## 7. File Watcher — Server

### Task ID: `WATCH-01`

**Source file: `server/file-watcher.ts`** — chokidar-based, as specified.

### Deliverables

**Test file: `test/server/file-watcher.test.ts`**

```typescript
import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import fs from 'fs';
import os from 'os';
import path from 'path';
import http from 'http';
import WebSocket from 'ws';
import { ServerTransport } from '@server/transport';
import { FileWatcher } from '@server/file-watcher';
import { ClientTransport } from '@client/transport';
import { CH, decodeJsonPayload } from '@shared/protocol';
import type { WatchEvent } from '@shared/protocol';

// TASK: WATCH-01
describe('WATCH-01: File watcher', () => {
  let tmpDir: string;
  let server: http.Server;
  let transport: ServerTransport;
  let watcher: FileWatcher;
  let clientTransport: ClientTransport;
  let port: number;
  let watchEvents: WatchEvent[];

  beforeAll(async () => {
    tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'lite-watch-test-'));
    fs.mkdirSync(path.join(tmpDir, 'src'));
    fs.writeFileSync(path.join(tmpDir, 'src', 'index.ts'), 'original');
    fs.mkdirSync(path.join(tmpDir, 'node_modules'));
    fs.mkdirSync(path.join(tmpDir, '.git'));

    server = http.createServer();
    transport = new ServerTransport(server);
    watcher = new FileWatcher(tmpDir);
    transport.onConnection((session) => { watcher.attach(session); });

    await new Promise<void>(resolve => {
      server.listen(0, '127.0.0.1', () => {
        port = (server.address() as any).port;
        resolve();
      });
    });

    watchEvents = [];
    clientTransport = new ClientTransport(WebSocket as any);
    clientTransport.connect(`ws://127.0.0.1:${port}/ws`);
    clientTransport.onMessage(CH.WATCH, (data) => {
      watchEvents.push(decodeJsonPayload<WatchEvent>(data));
    });
    await vi.waitUntil(() => clientTransport.state === 'open', { timeout: 3000 });
    await new Promise(r => setTimeout(r, 1000)); // let chokidar stabilize
  });

  afterAll(async () => {
    clientTransport.close();
    await watcher.close();
    await transport.close();
    await new Promise<void>(r => server.close(() => r()));
    fs.rmSync(tmpDir, { recursive: true, force: true });
  });

  it('emits change event on file modification', async () => {
    watchEvents.length = 0;
    fs.writeFileSync(path.join(tmpDir, 'src', 'index.ts'), 'modified');
    await new Promise(r => setTimeout(r, 1500));
    expect(watchEvents.filter(e => e.event === 'change' && e.path === 'src/index.ts').length).toBeGreaterThanOrEqual(1);
  });

  it('emits add event on new file', async () => {
    watchEvents.length = 0;
    fs.writeFileSync(path.join(tmpDir, 'newfile.txt'), 'hello');
    await new Promise(r => setTimeout(r, 1500));
    expect(watchEvents.filter(e => e.event === 'add' && e.path === 'newfile.txt').length).toBeGreaterThanOrEqual(1);
  });

  it('emits unlink event on file deletion', async () => {
    fs.writeFileSync(path.join(tmpDir, 'temp.txt'), 'temp');
    await new Promise(r => setTimeout(r, 500));
    watchEvents.length = 0;
    fs.unlinkSync(path.join(tmpDir, 'temp.txt'));
    await new Promise(r => setTimeout(r, 1500));
    expect(watchEvents.filter(e => e.event === 'unlink' && e.path === 'temp.txt').length).toBeGreaterThanOrEqual(1);
  });

  it('emits addDir event on new directory', async () => {
    watchEvents.length = 0;
    fs.mkdirSync(path.join(tmpDir, 'newdir'));
    await new Promise(r => setTimeout(r, 1500));
    expect(watchEvents.filter(e => e.event === 'addDir' && e.path === 'newdir').length).toBeGreaterThanOrEqual(1);
  });

  it('ignores node_modules changes', async () => {
    watchEvents.length = 0;
    fs.writeFileSync(path.join(tmpDir, 'node_modules', 'ignored.js'), 'changed');
    await new Promise(r => setTimeout(r, 1500));
    expect(watchEvents.filter(e => e.path.includes('node_modules')).length).toBe(0);
  });

  it('ignores .git changes', async () => {
    watchEvents.length = 0;
    fs.writeFileSync(path.join(tmpDir, '.git', 'something'), 'data');
    await new Promise(r => setTimeout(r, 1500));
    expect(watchEvents.filter(e => e.path.includes('.git')).length).toBe(0);
  });

  it('paths are relative to project root', async () => {
    watchEvents.length = 0;
    fs.writeFileSync(path.join(tmpDir, 'src', 'relative.ts'), 'test');
    await new Promise(r => setTimeout(r, 1500));
    expect(watchEvents.filter(e => e.path === 'src/relative.ts').length).toBeGreaterThanOrEqual(1);
    expect(watchEvents.filter(e => e.path.startsWith('/')).length).toBe(0);
  });

  it('debounces rapid changes to same file', async () => {
    watchEvents.length = 0;
    for (let i = 0; i < 5; i++) {
      fs.writeFileSync(path.join(tmpDir, 'src', 'rapid.ts'), `content-${i}`);
    }
    await new Promise(r => setTimeout(r, 1500));
    const rapidEvents = watchEvents.filter(e => e.path === 'src/rapid.ts');
    expect(rapidEvents.length).toBeLessThanOrEqual(2);
    expect(rapidEvents.length).toBeGreaterThanOrEqual(1);
  });
});
```

**Pass criteria:** `npx vitest run test/server/file-watcher.test.ts` — all green.

---

## 8. Shell Manager — Server

### Task ID: `SHELL-01`

**Source file: `server/shell-manager.ts`** — node-pty + tmux, as specified.

### Deliverables

**Test file: `test/server/shell-manager.test.ts`**

```typescript
import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import http from 'http';
import WebSocket from 'ws';
import { ServerTransport } from '@server/transport';
import { ShellManager } from '@server/shell-manager';
import { ClientTransport } from '@client/transport';
import { CH, FLAGS } from '@shared/protocol';

// TASK: SHELL-01
describe('SHELL-01: Shell manager', () => {
  let server: http.Server;
  let transport: ServerTransport;
  let shellManager: ShellManager;
  let port: number;

  beforeAll(async () => {
    server = http.createServer();
    transport = new ServerTransport(server);
    shellManager = new ShellManager();
    transport.onConnection((session) => { shellManager.attach(session); });

    await new Promise<void>(resolve => {
      server.listen(0, '127.0.0.1', () => {
        port = (server.address() as any).port;
        resolve();
      });
    });
  });

  afterAll(async () => {
    shellManager.destroyAll();
    await transport.close();
    await new Promise<void>(r => server.close(() => r()));
  });

  function createClient(): { transport: ClientTransport; shellOutput: Buffer[] } {
    const ct = new ClientTransport(WebSocket as any);
    const shellOutput: Buffer[] = [];
    ct.onMessage(CH.SHELL, (data) => shellOutput.push(Buffer.from(data)));
    ct.connect(`ws://127.0.0.1:${port}/ws`);
    return { transport: ct, shellOutput };
  }

  function getOutput(bufs: Buffer[]): string {
    return Buffer.concat(bufs).toString('utf-8');
  }

  it('produces shell output on connect', async () => {
    const { transport: ct, shellOutput } = createClient();
    await vi.waitUntil(() => ct.state === 'open', { timeout: 3000 });
    await new Promise(r => setTimeout(r, 2000));
    expect(shellOutput.length).toBeGreaterThan(0);
    ct.close();
  });

  it('executes commands and returns output', async () => {
    const { transport: ct, shellOutput } = createClient();
    await vi.waitUntil(() => ct.state === 'open', { timeout: 3000 });
    await new Promise(r => setTimeout(r, 1000));

    shellOutput.length = 0;
    ct.send(CH.SHELL, FLAGS.BINARY, new TextEncoder().encode('echo TESTOUTPUT123\n'));
    await new Promise(r => setTimeout(r, 1000));

    expect(getOutput(shellOutput)).toContain('TESTOUTPUT123');
    ct.close();
  });

  it('handles resize', async () => {
    const { transport: ct, shellOutput } = createClient();
    await vi.waitUntil(() => ct.state === 'open', { timeout: 3000 });
    await new Promise(r => setTimeout(r, 1000));

    ct.sendJson(CH.CONTROL, { type: 'resize', cols: 40, rows: 10 });
    await new Promise(r => setTimeout(r, 500));

    shellOutput.length = 0;
    ct.send(CH.SHELL, FLAGS.BINARY, new TextEncoder().encode('tput cols\n'));
    await new Promise(r => setTimeout(r, 1000));

    expect(getOutput(shellOutput)).toContain('40');
    ct.close();
  });

  it('shell persists across reconnect (tmux)', async () => {
    const { transport: ct1 } = createClient();
    await vi.waitUntil(() => ct1.state === 'open', { timeout: 3000 });
    await new Promise(r => setTimeout(r, 1000));

    ct1.send(CH.SHELL, FLAGS.BINARY, new TextEncoder().encode('export LITE_PERSIST_TEST=hello42\n'));
    await new Promise(r => setTimeout(r, 500));

    const sessionId = ct1.sessionId;
    ct1.close();
    await new Promise(r => setTimeout(r, 500));

    const ct2 = new ClientTransport(WebSocket as any);
    const output2: Buffer[] = [];
    ct2.onMessage(CH.SHELL, (data) => output2.push(Buffer.from(data)));
    ct2.connect(`ws://127.0.0.1:${port}/ws?sessionId=${sessionId}`);
    await vi.waitUntil(() => ct2.state === 'open', { timeout: 3000 });
    await new Promise(r => setTimeout(r, 1000));

    output2.length = 0;
    ct2.send(CH.SHELL, FLAGS.BINARY, new TextEncoder().encode('echo $LITE_PERSIST_TEST\n'));
    await new Promise(r => setTimeout(r, 1000));

    expect(Buffer.concat(output2).toString('utf-8')).toContain('hello42');
    ct2.close();
  });

  it('handles binary data without corruption', async () => {
    const { transport: ct, shellOutput } = createClient();
    await vi.waitUntil(() => ct.state === 'open', { timeout: 3000 });
    await new Promise(r => setTimeout(r, 1000));

    shellOutput.length = 0;
    ct.send(CH.SHELL, FLAGS.BINARY, new TextEncoder().encode('printf "\\x00\\x01\\x02\\xff" | xxd -p\n'));
    await new Promise(r => setTimeout(r, 1000));

    expect(getOutput(shellOutput)).toContain('000102ff');
    ct.close();
  });
});
```

**Pass criteria:** `npx vitest run test/server/shell-manager.test.ts` — all green. tmux required for persistence test.

---

## 9. Client — Layout Shell

### Task ID: `UI-LAYOUT-01`

**Source files:** `client/index.html`, `client/styles/layout.css`, `client/styles/theme.css`, `client/layout.ts`

### Deliverables

**Test file: `test/client/layout.test.ts`**

```typescript
// @vitest-environment jsdom
import { describe, it, expect, beforeEach } from 'vitest';
import fs from 'fs';
import path from 'path';
import { Layout } from '@client/layout';

// TASK: UI-LAYOUT-01
describe('UI-LAYOUT-01: Layout', () => {
  let container: HTMLElement;
  let layout: Layout;

  beforeEach(() => {
    document.body.innerHTML = '';
    container = document.createElement('div');
    document.body.appendChild(container);
    layout = new Layout(container);
  });

  it('creates all required panel elements', () => {
    expect(container.querySelector('.tab-bar')).toBeTruthy();
    expect(container.querySelector('.file-tree')).toBeTruthy();
    expect(container.querySelector('.editor')).toBeTruthy();
    expect(container.querySelector('.terminal-panel')).toBeTruthy();
    expect(container.querySelector('.drag-handle')).toBeTruthy();
  });

  it('exposes panel elements for component mounting', () => {
    expect(layout.treeEl).toBeInstanceOf(HTMLElement);
    expect(layout.editorEl).toBeInstanceOf(HTMLElement);
    expect(layout.tabBarEl).toBeInstanceOf(HTMLElement);
    expect(layout.terminalEl).toBeInstanceOf(HTMLElement);
    expect(layout.terminalTabsEl).toBeInstanceOf(HTMLElement);
  });

  it('updateTreeWidth changes CSS variable', () => {
    layout.updateTreeWidth(300);
    const app = container.querySelector('.app') as HTMLElement;
    expect(app.style.getPropertyValue('--tree-width')).toBe('300px');
  });

  it('updateTerminalHeight changes CSS variable', () => {
    layout.updateTerminalHeight(200);
    const app = container.querySelector('.app') as HTMLElement;
    expect(app.style.getPropertyValue('--terminal-height')).toBe('200px');
  });

  it('enforces minimum tree width', () => {
    layout.updateTreeWidth(50);
    const app = container.querySelector('.app') as HTMLElement;
    const width = parseInt(app.style.getPropertyValue('--tree-width'));
    expect(width).toBeGreaterThanOrEqual(180);
  });

  it('enforces minimum terminal height', () => {
    layout.updateTerminalHeight(20);
    const app = container.querySelector('.app') as HTMLElement;
    const height = parseInt(app.style.getPropertyValue('--terminal-height'));
    expect(height).toBeGreaterThanOrEqual(120);
  });
});

// TASK: UI-LAYOUT-01
describe('UI-LAYOUT-01: Theme CSS', () => {
  it('defines required CSS custom properties', () => {
    const themeCSS = fs.readFileSync(
      path.resolve(__dirname, '../../client/styles/theme.css'), 'utf-8'
    );
    const requiredVars = [
      '--bg-primary', '--bg-secondary', '--bg-tertiary',
      '--fg-primary', '--fg-secondary', '--fg-muted',
      '--accent', '--border',
    ];
    for (const v of requiredVars) {
      expect(themeCSS).toContain(v);
    }
  });
});
```

**Pass criteria:** `npx vitest run test/client/layout.test.ts` — all green.

---

## 10. Client — File Tree

### Task ID: `UI-TREE-01`

**Source files:** `client/tree/tree.ts`, `client/tree/context-menu.ts`

### Deliverables

**Test file: `test/client/tree.test.ts`**

```typescript
// @vitest-environment jsdom
import { describe, it, expect, beforeEach, vi } from 'vitest';
import { FileTree } from '@client/tree/tree';

function createMockRpc() {
  const handlers: Record<string, Function> = {};
  return {
    call: vi.fn(async (method: string, params: any) => {
      if (handlers[method]) return handlers[method](params);
      throw new Error(`No mock for ${method}`);
    }),
    onNotification: vi.fn(),
    _mock(method: string, handler: Function) { handlers[method] = handler; },
  };
}

// TASK: UI-TREE-01
describe('UI-TREE-01: File tree', () => {
  let container: HTMLElement;
  let rpc: ReturnType<typeof createMockRpc>;
  let tree: FileTree;

  const rootEntries = [
    { name: 'src', type: 'directory' as const, size: 0, mtime: Date.now() },
    { name: 'README.md', type: 'file' as const, size: 100, mtime: Date.now() },
    { name: 'package.json', type: 'file' as const, size: 500, mtime: Date.now() },
  ];
  const srcEntries = [
    { name: 'index.ts', type: 'file' as const, size: 200, mtime: Date.now() },
    { name: 'app.ts', type: 'file' as const, size: 300, mtime: Date.now() },
  ];

  beforeEach(async () => {
    document.body.innerHTML = '';
    container = document.createElement('div');
    document.body.appendChild(container);
    rpc = createMockRpc();
    rpc._mock('readdir', (params: any) => {
      if (params.path === '/') return rootEntries;
      if (params.path === '/src') return srcEntries;
      throw new Error('Not found');
    });
    tree = new FileTree(container, rpc as any);
    await tree.init();
  });

  it('renders root directory entries', () => {
    const nodes = container.querySelectorAll('[data-tree-node]');
    expect(nodes.length).toBe(3);
  });

  it('shows directory with chevron', () => {
    const dirNode = container.querySelector('[data-path="/src"]');
    expect(dirNode?.querySelector('.chevron')).toBeTruthy();
  });

  it('shows file without chevron', () => {
    const fileNode = container.querySelector('[data-path="/README.md"]');
    expect(fileNode?.querySelector('.chevron')).toBeFalsy();
  });

  it('expands directory on click', async () => {
    (container.querySelector('[data-path="/src"]') as HTMLElement).click();
    await vi.waitFor(() => {
      expect(container.querySelectorAll('[data-path^="/src/"]').length).toBe(2);
    });
  });

  it('fires onFileSelect on file click', () => {
    const handler = vi.fn();
    tree.onFileSelect(handler);
    (container.querySelector('[data-path="/README.md"]') as HTMLElement).click();
    expect(handler).toHaveBeenCalledWith('/README.md');
  });

  it('lazy-loads only expanded directories', async () => {
    expect(rpc.call).toHaveBeenCalledWith('readdir', { path: '/' });
    expect(rpc.call).not.toHaveBeenCalledWith('readdir', expect.objectContaining({ path: '/src' }));
    (container.querySelector('[data-path="/src"]') as HTMLElement).click();
    await vi.waitFor(() => {
      expect(rpc.call).toHaveBeenCalledWith('readdir', { path: '/src' });
    });
  });

  it('markModified adds visual indicator', () => {
    tree.markModified('/README.md', true);
    const node = container.querySelector('[data-path="/README.md"]');
    expect(node?.classList.contains('modified') || node?.querySelector('.modified-dot')).toBeTruthy();
  });

  it('refresh re-fetches directory', async () => {
    (container.querySelector('[data-path="/src"]') as HTMLElement).click();
    await vi.waitFor(() => {
      expect(container.querySelectorAll('[data-path^="/src/"]').length).toBe(2);
    });

    rpc._mock('readdir', (params: any) => {
      if (params.path === '/src') return [...srcEntries, { name: 'new.ts', type: 'file', size: 0, mtime: Date.now() }];
      return rootEntries;
    });

    await tree.refresh('/src');
    await vi.waitFor(() => {
      expect(container.querySelectorAll('[data-path^="/src/"]').length).toBe(3);
    });
  });

  it('context menu appears on right-click', async () => {
    (container.querySelector('[data-path="/README.md"]') as HTMLElement)
      .dispatchEvent(new MouseEvent('contextmenu', { bubbles: true }));
    await vi.waitFor(() => {
      expect(document.querySelector('.context-menu')).toBeTruthy();
    });
  });

  it('context menu has file actions', async () => {
    (container.querySelector('[data-path="/README.md"]') as HTMLElement)
      .dispatchEvent(new MouseEvent('contextmenu', { bubbles: true }));
    await vi.waitFor(() => {
      const menu = document.querySelector('.context-menu');
      expect(menu?.textContent).toContain('Rename');
      expect(menu?.textContent).toContain('Delete');
    });
  });

  it('context menu has directory actions', async () => {
    (container.querySelector('[data-path="/src"]') as HTMLElement)
      .dispatchEvent(new MouseEvent('contextmenu', { bubbles: true }));
    await vi.waitFor(() => {
      const menu = document.querySelector('.context-menu');
      expect(menu?.textContent).toContain('New File');
      expect(menu?.textContent).toContain('New Folder');
    });
  });

  it('context menu dismisses on outside click', async () => {
    (container.querySelector('[data-path="/README.md"]') as HTMLElement)
      .dispatchEvent(new MouseEvent('contextmenu', { bubbles: true }));
    await vi.waitFor(() => { expect(document.querySelector('.context-menu')).toBeTruthy(); });
    document.body.click();
    await vi.waitFor(() => { expect(document.querySelector('.context-menu')).toBeFalsy(); });
  });
});
```

**Pass criteria:** `npx vitest run test/client/tree.test.ts` — all green.

---

## 11. Client — Ace Editor + Tabs

### Task ID: `UI-EDITOR-01`

**Source files:** `client/editor/editor.ts`, `client/editor/annotations.ts`, `client/editor/themes.ts`

**CRITICAL: This task enforces the Editor Buffer Sovereignty invariant. See Section 0.**

The `EditorManager` MUST NOT expose any public method that directly sets Ace buffer content from external sources. The only write paths into the buffer are:

1. `open(path)` — initial file load (buffer was empty, no prior content to protect)
2. User keystrokes — Ace's native input handling, untouched
3. `applyProgrammaticEdit(path, edit: ProgrammaticEdit)` — for REPL/ELKs, wraps all changes in a single Ace UndoManager group so Ctrl+Z reverses the entire operation atomically

Remote file changes go to `TabState.pendingRemote`, never to the Ace buffer:

```typescript
interface TabState {
  path: string;
  modified: boolean;          // local edits exist
  preview: boolean;
  cursorPos: { row: number; col: number };
  scrollTop: number;
  content: string;            // current buffer content (what Ace has)
  baseContent: string;        // content at last save/open (for dirty detection)
  pendingRemote: PendingRemote | null;  // external change waiting for user action
  conflictState: ConflictState;
  undoManager: any;           // ace.UndoManager, preserved per tab
}
```

**External change flow:**
1. Watch event arrives for an open file → `EditorManager.notifyRemoteChange(path, newContent, mtime)`
2. This sets `tab.pendingRemote = { content, mtime, detectedAt }` — buffer is UNTOUCHED
3. Tab visual indicator updates (icon change, gutter highlights if unmodified locally)
4. User runs `sync` / `accept` in REPL, or clicks the indicator → `EditorManager.acceptRemoteChange(path)`
5. ONLY THEN is the buffer updated (via `applyProgrammaticEdit` so it's undoable)

**Programmatic edit flow (REPL `replace`, etc.):**
1. REPL calls `editor.applyProgrammaticEdit(path, edit)`
2. EditorManager opens an Ace UndoManager group (`session.getUndoManager().startNewGroup()` equivalent)
3. Applies all edits in the `ProgrammaticEdit.edits` array
4. Closes the undo group
5. Returns a summary: `{ applied: number; description: string }`
6. User can Ctrl+Z to undo the entire operation as one step

### Deliverables

**Test file: `test/client/editor.test.ts`**

```typescript
// @vitest-environment jsdom
import { describe, it, expect, beforeEach, vi } from 'vitest';
import { TabManager, detectMode } from '@client/editor/editor';
import type { ProgrammaticEdit } from '@shared/protocol';

// TASK: UI-EDITOR-01
describe('UI-EDITOR-01: Mode detection', () => {
  it('javascript for .js', () => expect(detectMode('app.js')).toBe('ace/mode/javascript'));
  it('typescript for .ts', () => expect(detectMode('index.ts')).toBe('ace/mode/typescript'));
  it('python for .py', () => expect(detectMode('script.py')).toBe('ace/mode/python'));
  it('rust for .rs', () => expect(detectMode('main.rs')).toBe('ace/mode/rust'));
  it('json for .json', () => expect(detectMode('package.json')).toBe('ace/mode/json'));
  it('css for .css', () => expect(detectMode('styles.css')).toBe('ace/mode/css'));
  it('html for .html', () => expect(detectMode('index.html')).toBe('ace/mode/html'));
  it('markdown for .md', () => expect(detectMode('README.md')).toBe('ace/mode/markdown'));
  it('text for unknown', () => expect(detectMode('unknown.xyz')).toBe('ace/mode/text'));
  it('handles no extension', () => expect(detectMode('Makefile')).toBeTruthy());
});

// TASK: UI-EDITOR-01
describe('UI-EDITOR-01: Tab management', () => {
  let tabs: TabManager;
  let mockRpc: any;

  beforeEach(() => {
    mockRpc = {
      call: vi.fn(async (method: string, params: any) => {
        if (method === 'readFile') return { content: `content of ${params.path}`, encoding: 'utf-8' };
        if (method === 'writeFile') return { ok: true };
      }),
    };
    tabs = new TabManager(mockRpc);
  });

  it('open creates a tab', async () => {
    await tabs.open('/test.txt');
    expect(tabs.getTabs().length).toBe(1);
    expect(tabs.getTabs()[0].path).toBe('/test.txt');
  });

  it('open fetches via RPC', async () => {
    await tabs.open('/test.txt');
    expect(mockRpc.call).toHaveBeenCalledWith('readFile', { path: '/test.txt' });
  });

  it('no duplicate tabs for same file', async () => {
    await tabs.open('/test.txt');
    await tabs.open('/test.txt');
    expect(tabs.getTabs().length).toBe(1);
  });

  it('separate tabs for different files', async () => {
    await tabs.open('/a.txt');
    await tabs.open('/b.txt');
    expect(tabs.getTabs().length).toBe(2);
  });

  it('close removes tab', async () => {
    await tabs.open('/a.txt');
    await tabs.open('/b.txt');
    tabs.close('/a.txt');
    expect(tabs.getTabs().length).toBe(1);
    expect(tabs.getTabs()[0].path).toBe('/b.txt');
  });

  it('close sets active to nearest tab', async () => {
    await tabs.open('/a.txt');
    await tabs.open('/b.txt');
    await tabs.open('/c.txt');
    tabs.setActive('/b.txt');
    tabs.close('/b.txt');
    expect(tabs.activePath).toBeTruthy();
    expect(tabs.activePath).not.toBe('/b.txt');
  });

  it('tracks modified state', async () => {
    await tabs.open('/test.txt');
    expect(tabs.isModified('/test.txt')).toBe(false);
    tabs.markModified('/test.txt', true);
    expect(tabs.isModified('/test.txt')).toBe(true);
    tabs.markModified('/test.txt', false);
    expect(tabs.isModified('/test.txt')).toBe(false);
  });

  it('fires onModifiedChange', async () => {
    const handler = vi.fn();
    tabs.onModifiedChange(handler);
    await tabs.open('/test.txt');
    tabs.markModified('/test.txt', true);
    expect(handler).toHaveBeenCalledWith('/test.txt', true);
  });

  it('preview tab replaced by next preview', async () => {
    await tabs.open('/a.txt', { preview: true });
    await tabs.open('/b.txt', { preview: true });
    expect(tabs.getTabs().length).toBe(1);
    expect(tabs.getTabs()[0].path).toBe('/b.txt');
  });

  it('preview promoted on edit', async () => {
    await tabs.open('/a.txt', { preview: true });
    tabs.markModified('/a.txt', true);
    expect(tabs.getTabs()[0].preview).toBe(false);
    await tabs.open('/b.txt', { preview: true });
    expect(tabs.getTabs().length).toBe(2);
  });

  it('save sends writeFile RPC', async () => {
    await tabs.open('/test.txt');
    tabs.setContent('/test.txt', 'new content');
    await tabs.save('/test.txt');
    expect(mockRpc.call).toHaveBeenCalledWith('writeFile', { path: '/test.txt', content: 'new content' });
  });

  it('save clears modified', async () => {
    await tabs.open('/test.txt');
    tabs.markModified('/test.txt', true);
    tabs.setContent('/test.txt', 'new');
    await tabs.save('/test.txt');
    expect(tabs.isModified('/test.txt')).toBe(false);
  });

  it('preserves cursor position per tab', async () => {
    await tabs.open('/a.txt');
    tabs.setCursorPos('/a.txt', { row: 10, col: 5 });
    await tabs.open('/b.txt');
    tabs.setCursorPos('/b.txt', { row: 3, col: 0 });
    expect(tabs.getCursorPos('/a.txt')).toEqual({ row: 10, col: 5 });
    expect(tabs.getCursorPos('/b.txt')).toEqual({ row: 3, col: 0 });
  });
});

// TASK: UI-EDITOR-01
// CRITICAL: Buffer Sovereignty Invariant Tests
describe('UI-EDITOR-01: Buffer sovereignty — remote changes', () => {
  let tabs: TabManager;
  let mockRpc: any;

  beforeEach(async () => {
    mockRpc = {
      call: vi.fn(async (method: string, params: any) => {
        if (method === 'readFile') return { content: 'original content', encoding: 'utf-8' };
        if (method === 'writeFile') return { ok: true };
      }),
    };
    tabs = new TabManager(mockRpc);
    await tabs.open('/test.txt');
  });

  it('notifyRemoteChange does NOT alter buffer content', () => {
    const contentBefore = tabs.getContent('/test.txt');
    tabs.notifyRemoteChange('/test.txt', 'remote new content', Date.now());
    const contentAfter = tabs.getContent('/test.txt');
    expect(contentAfter).toBe(contentBefore);
    expect(contentAfter).toBe('original content');
  });

  it('notifyRemoteChange stores pending remote state', () => {
    tabs.notifyRemoteChange('/test.txt', 'remote new content', Date.now());
    const tab = tabs.getTab('/test.txt');
    expect(tab?.pendingRemote).not.toBeNull();
    expect(tab?.pendingRemote?.content).toBe('remote new content');
  });

  it('conflictState is remote-only when buffer is clean', () => {
    tabs.notifyRemoteChange('/test.txt', 'remote new content', Date.now());
    const tab = tabs.getTab('/test.txt');
    expect(tab?.conflictState).toBe('remote-only');
  });

  it('conflictState is conflict when buffer is dirty', () => {
    tabs.markModified('/test.txt', true);
    tabs.setContent('/test.txt', 'local edits');
    tabs.notifyRemoteChange('/test.txt', 'remote new content', Date.now());
    const tab = tabs.getTab('/test.txt');
    expect(tab?.conflictState).toBe('conflict');
  });

  it('fires onPendingRemote callback', () => {
    const handler = vi.fn();
    tabs.onPendingRemote(handler);
    tabs.notifyRemoteChange('/test.txt', 'remote new content', Date.now());
    expect(handler).toHaveBeenCalledWith('/test.txt', expect.objectContaining({
      content: 'remote new content',
    }));
  });

  it('acceptRemoteChange updates buffer content', () => {
    tabs.notifyRemoteChange('/test.txt', 'remote new content', Date.now());
    tabs.acceptRemoteChange('/test.txt');
    expect(tabs.getContent('/test.txt')).toBe('remote new content');
  });

  it('acceptRemoteChange clears pending state', () => {
    tabs.notifyRemoteChange('/test.txt', 'remote new content', Date.now());
    tabs.acceptRemoteChange('/test.txt');
    const tab = tabs.getTab('/test.txt');
    expect(tab?.pendingRemote).toBeNull();
    expect(tab?.conflictState).toBe('clean');
  });

  it('acceptRemoteChange is undoable', () => {
    tabs.notifyRemoteChange('/test.txt', 'remote new content', Date.now());
    tabs.acceptRemoteChange('/test.txt');
    expect(tabs.getContent('/test.txt')).toBe('remote new content');
    // Undo should restore original
    tabs.undo('/test.txt');
    expect(tabs.getContent('/test.txt')).toBe('original content');
  });

  it('dismissRemoteChange clears pending without changing buffer', () => {
    tabs.notifyRemoteChange('/test.txt', 'remote new content', Date.now());
    tabs.dismissRemoteChange('/test.txt');
    expect(tabs.getContent('/test.txt')).toBe('original content');
    expect(tabs.getTab('/test.txt')?.pendingRemote).toBeNull();
  });

  it('multiple remote changes only keep latest', () => {
    tabs.notifyRemoteChange('/test.txt', 'remote v1', 1000);
    tabs.notifyRemoteChange('/test.txt', 'remote v2', 2000);
    const tab = tabs.getTab('/test.txt');
    expect(tab?.pendingRemote?.content).toBe('remote v2');
    expect(tab?.pendingRemote?.mtime).toBe(2000);
    // Buffer still untouched
    expect(tabs.getContent('/test.txt')).toBe('original content');
  });

  it('notifyRemoteChange on non-open file is a no-op', () => {
    // Should not throw
    tabs.notifyRemoteChange('/not-open.txt', 'content', Date.now());
  });
});

// TASK: UI-EDITOR-01
// CRITICAL: Buffer Sovereignty — Programmatic Edits
describe('UI-EDITOR-01: Buffer sovereignty — programmatic edits', () => {
  let tabs: TabManager;
  let mockRpc: any;

  beforeEach(async () => {
    mockRpc = {
      call: vi.fn(async (method: string, params: any) => {
        if (method === 'readFile') return { content: 'line 1\nline 2\nline 3\nline 4', encoding: 'utf-8' };
        if (method === 'writeFile') return { ok: true };
      }),
    };
    tabs = new TabManager(mockRpc);
    await tabs.open('/test.txt');
  });

  it('applyProgrammaticEdit modifies buffer', () => {
    const edit: ProgrammaticEdit = {
      source: 'repl',
      branchId: null,
      description: 'Replace line 2',
      edits: [{ startRow: 1, startCol: 0, endRow: 1, endCol: 6, text: 'LINE 2' }],
    };
    const result = tabs.applyProgrammaticEdit('/test.txt', edit);
    expect(result.applied).toBe(1);
    expect(tabs.getContent('/test.txt')).toContain('LINE 2');
  });

  it('programmatic edit is undoable as single operation', () => {
    const edit: ProgrammaticEdit = {
      source: 'repl',
      branchId: null,
      description: 'Multi-replace',
      edits: [
        { startRow: 0, startCol: 0, endRow: 0, endCol: 6, text: 'LINE 1' },
        { startRow: 2, startCol: 0, endRow: 2, endCol: 6, text: 'LINE 3' },
      ],
    };
    const contentBefore = tabs.getContent('/test.txt');
    tabs.applyProgrammaticEdit('/test.txt', edit);

    const contentAfter = tabs.getContent('/test.txt');
    expect(contentAfter).not.toBe(contentBefore);

    // Single undo reverses ALL edits in the group
    tabs.undo('/test.txt');
    expect(tabs.getContent('/test.txt')).toBe(contentBefore);
  });

  it('programmatic edit marks file as modified', () => {
    const edit: ProgrammaticEdit = {
      source: 'repl',
      branchId: null,
      description: 'Test edit',
      edits: [{ startRow: 0, startCol: 0, endRow: 0, endCol: 1, text: 'X' }],
    };
    tabs.applyProgrammaticEdit('/test.txt', edit);
    expect(tabs.isModified('/test.txt')).toBe(true);
  });

  it('programmatic edit on non-open file throws', () => {
    const edit: ProgrammaticEdit = {
      source: 'repl',
      branchId: null,
      description: 'Bad edit',
      edits: [{ startRow: 0, startCol: 0, endRow: 0, endCol: 1, text: 'X' }],
    };
    expect(() => tabs.applyProgrammaticEdit('/not-open.txt', edit)).toThrow();
  });

  it('empty edits array is a no-op', () => {
    const contentBefore = tabs.getContent('/test.txt');
    const edit: ProgrammaticEdit = {
      source: 'repl',
      branchId: null,
      description: 'Nothing',
      edits: [],
    };
    const result = tabs.applyProgrammaticEdit('/test.txt', edit);
    expect(result.applied).toBe(0);
    expect(tabs.getContent('/test.txt')).toBe(contentBefore);
  });
});

describe.skip('UI-EDITOR-01: Ace integration (requires browser/Playwright)', () => {
  it('loads content into Ace editor');
  it('Ctrl+S triggers save');
  it('annotations render as Ace markers but do not alter buffer text');
  it('theme switching works');
  it('programmatic edits create a single undo group in Ace UndoManager');
  it('remote change annotation highlights appear without modifying buffer');
});
```

**Pass criteria:** `npx vitest run test/client/editor.test.ts` — all non-skipped green.

---

## 12. Client — Terminal Panel + REPL

### Task ID: `UI-TERMINAL-01`

**Source files:** `client/terminal/terminal.ts`, `client/terminal/repl.ts`, `client/terminal/shell.ts`

### Deliverables

**Test file: `test/client/repl.test.ts`**

```typescript
import { describe, it, expect, beforeEach, vi } from 'vitest';
import { ReplParser, ReplCommandRegistry, ReplHistory } from '@client/terminal/repl';

// TASK: UI-TERMINAL-01
describe('UI-TERMINAL-01: REPL parser', () => {
  let parser: ReplParser;
  beforeEach(() => { parser = new ReplParser(); });

  it('parses simple command', () => {
    const r = parser.parse('help');
    expect(r.command).toBe('help');
    expect(r.args).toEqual([]);
  });

  it('parses command with args', () => {
    const r = parser.parse('open /src/index.ts');
    expect(r.command).toBe('open');
    expect(r.args).toEqual(['/src/index.ts']);
  });

  it('parses multiple args', () => {
    const r = parser.parse('replace foo bar');
    expect(r.command).toBe('replace');
    expect(r.args).toEqual(['foo', 'bar']);
  });

  it('parses quoted args', () => {
    const r = parser.parse('search "hello world"');
    expect(r.command).toBe('search');
    expect(r.args).toEqual(['hello world']);
  });

  it('parses flags', () => {
    const r = parser.parse('replace foo bar --file /src/app.ts');
    expect(r.command).toBe('replace');
    expect(r.args).toEqual(['foo', 'bar']);
    expect(r.flags.file).toBe('/src/app.ts');
  });

  it('handles empty input', () => {
    const r = parser.parse('');
    expect(r.command).toBe('');
  });

  it('trims whitespace', () => {
    const r = parser.parse('  open   /test.txt  ');
    expect(r.command).toBe('open');
    expect(r.args).toEqual(['/test.txt']);
  });
});

// TASK: UI-TERMINAL-01
describe('UI-TERMINAL-01: REPL commands', () => {
  let registry: ReplCommandRegistry;
  let output: string[];
  let mockCtx: any;

  beforeEach(() => {
    output = [];
    mockCtx = {
      editor: {
        open: vi.fn(),
        save: vi.fn(),
        close: vi.fn(),
        currentPath: '/test.txt',
        gotoLine: vi.fn(),
        setTheme: vi.fn(),
        setSetting: vi.fn(),
        // Buffer sovereignty methods
        applyProgrammaticEdit: vi.fn(() => ({ applied: 1, description: 'done' })),
        acceptRemoteChange: vi.fn(),
        dismissRemoteChange: vi.fn(),
        getTab: vi.fn((path: string) => ({
          path,
          pendingRemote: null,
          conflictState: 'clean',
          content: 'file content',
        })),
        getContent: vi.fn(() => 'file content'),
      },
      tree: { init: vi.fn(), refresh: vi.fn() },
      rpc: {
        call: vi.fn(async (method: string) => {
          if (method === 'readdir') return [
            { name: 'index.ts', type: 'file', size: 100, mtime: 0 },
            { name: 'src', type: 'directory', size: 0, mtime: 0 },
          ];
          if (method === 'writeFile') return { ok: true };
        }),
      },
      write: (t: string) => output.push(t),
      writeln: (t: string) => output.push(t + '\n'),
    };
    registry = new ReplCommandRegistry();
    registry.registerDefaults(mockCtx);
    vi.clearAllMocks();
  });

  it('help lists commands', async () => {
    await registry.execute('help', [], {}, mockCtx);
    const combined = output.join('');
    expect(combined).toContain('open');
    expect(combined).toContain('save');
    expect(combined).toContain('help');
    expect(combined).toContain('sync');
    expect(combined).toContain('accept');
    expect(combined).toContain('dismiss');
  });

  it('open calls editor.open', async () => {
    await registry.execute('open', ['/src/index.ts'], {}, mockCtx);
    expect(mockCtx.editor.open).toHaveBeenCalledWith('/src/index.ts');
  });

  it('open with no path shows usage', async () => {
    await registry.execute('open', [], {}, mockCtx);
    expect(output.join('')).toContain('Usage');
  });

  it('new creates and opens file', async () => {
    await registry.execute('new', ['/src/new.ts'], {}, mockCtx);
    expect(mockCtx.rpc.call).toHaveBeenCalledWith('writeFile', { path: '/src/new.ts', content: '' });
    expect(mockCtx.editor.open).toHaveBeenCalledWith('/src/new.ts');
  });

  it('save calls editor.save', async () => {
    await registry.execute('save', [], {}, mockCtx);
    expect(mockCtx.editor.save).toHaveBeenCalled();
  });

  it('close calls editor.close', async () => {
    await registry.execute('close', [], {}, mockCtx);
    expect(mockCtx.editor.close).toHaveBeenCalledWith('/test.txt');
  });

  it('files shows listing', async () => {
    await registry.execute('files', [], {}, mockCtx);
    const combined = output.join('');
    expect(combined).toContain('index.ts');
    expect(combined).toContain('src');
  });

  it('goto calls gotoLine', async () => {
    await registry.execute('goto', ['42'], {}, mockCtx);
    expect(mockCtx.editor.gotoLine).toHaveBeenCalledWith(42);
  });

  it('goto non-numeric shows error', async () => {
    await registry.execute('goto', ['abc'], {}, mockCtx);
    expect(output.join('')).toMatch(/number|invalid/i);
  });

  it('theme calls setTheme', async () => {
    await registry.execute('theme', ['monokai'], {}, mockCtx);
    expect(mockCtx.editor.setTheme).toHaveBeenCalledWith('monokai');
  });

  it('set calls setSetting', async () => {
    await registry.execute('set', ['tabSize', '4'], {}, mockCtx);
    expect(mockCtx.editor.setSetting).toHaveBeenCalledWith('tabSize', '4');
  });

  it('unknown command shows error', async () => {
    await registry.execute('notacommand', [], {}, mockCtx);
    expect(output.join('')).toContain('Unknown command');
  });

  // --- Buffer sovereignty REPL commands ---

  it('replace uses applyProgrammaticEdit (not direct buffer write)', async () => {
    await registry.execute('replace', ['foo', 'bar'], {}, mockCtx);
    expect(mockCtx.editor.applyProgrammaticEdit).toHaveBeenCalled();
    const call = mockCtx.editor.applyProgrammaticEdit.mock.calls[0];
    expect(call[0]).toBe('/test.txt');
    expect(call[1].source).toBe('repl');
  });

  it('accept calls acceptRemoteChange', async () => {
    await registry.execute('accept', [], {}, mockCtx);
    expect(mockCtx.editor.acceptRemoteChange).toHaveBeenCalledWith('/test.txt');
  });

  it('dismiss calls dismissRemoteChange', async () => {
    await registry.execute('dismiss', [], {}, mockCtx);
    expect(mockCtx.editor.dismissRemoteChange).toHaveBeenCalledWith('/test.txt');
  });

  it('sync on clean file with no pending shows message', async () => {
    mockCtx.editor.getTab.mockReturnValue({
      path: '/test.txt',
      pendingRemote: null,
      conflictState: 'clean',
    });
    await registry.execute('sync', [], {}, mockCtx);
    expect(output.join('')).toMatch(/up to date|no pending|clean/i);
    expect(mockCtx.editor.acceptRemoteChange).not.toHaveBeenCalled();
  });

  it('sync on file with pending remote calls accept', async () => {
    mockCtx.editor.getTab.mockReturnValue({
      path: '/test.txt',
      pendingRemote: { content: 'new', mtime: 1000, detectedAt: 1000 },
      conflictState: 'remote-only',
    });
    await registry.execute('sync', [], {}, mockCtx);
    expect(mockCtx.editor.acceptRemoteChange).toHaveBeenCalledWith('/test.txt');
  });

  it('sync on conflict warns user instead of auto-accepting', async () => {
    mockCtx.editor.getTab.mockReturnValue({
      path: '/test.txt',
      pendingRemote: { content: 'new', mtime: 1000, detectedAt: 1000 },
      conflictState: 'conflict',
    });
    await registry.execute('sync', [], {}, mockCtx);
    expect(output.join('')).toMatch(/conflict|modified locally/i);
    // Should NOT auto-accept in conflict state
    expect(mockCtx.editor.acceptRemoteChange).not.toHaveBeenCalled();
  });
});

// TASK: UI-TERMINAL-01
describe('UI-TERMINAL-01: REPL history', () => {
  let history: ReplHistory;

  beforeEach(() => { history = new ReplHistory(100); });

  it('stores entries', () => {
    history.push('help');
    history.push('open /test.txt');
    expect(history.size).toBe(2);
  });

  it('navigates back', () => {
    history.push('first');
    history.push('second');
    history.push('third');
    expect(history.back()).toBe('third');
    expect(history.back()).toBe('second');
    expect(history.back()).toBe('first');
  });

  it('navigates forward after back', () => {
    history.push('first');
    history.push('second');
    history.back();
    history.back();
    expect(history.forward()).toBe('second');
  });

  it('returns empty at forward boundary', () => {
    history.push('only');
    history.back();
    expect(history.forward()).toBe('');
  });

  it('resets position on new push', () => {
    history.push('a');
    history.push('b');
    history.back();
    history.push('c');
    expect(history.back()).toBe('c');
  });

  it('enforces max size', () => {
    const small = new ReplHistory(3);
    small.push('a');
    small.push('b');
    small.push('c');
    small.push('d');
    expect(small.size).toBe(3);
    expect(small.back()).toBe('d');
    expect(small.back()).toBe('c');
    expect(small.back()).toBe('b');
  });
});

// TASK: UI-TERMINAL-01
describe('UI-TERMINAL-01: Shell adapter', () => {
  it('sends data to CH.SHELL on transport', async () => {
    const { ShellAdapter } = await import('@client/terminal/shell');
    const mockTransport = {
      send: vi.fn(),
      onMessage: vi.fn(),
      state: 'open' as const,
    };
    const shell = new ShellAdapter(mockTransport as any);
    shell.write('ls\n');
    expect(mockTransport.send).toHaveBeenCalled();
    expect(mockTransport.send.mock.calls[0][0]).toBe(0x02); // CH.SHELL
  });
});

// TASK: UI-TERMINAL-01
describe('UI-TERMINAL-01: Terminal tab manager', () => {
  it('tracks active tab', async () => {
    const { TerminalTabManager } = await import('@client/terminal/terminal');
    const mgr = new TerminalTabManager();
    mgr.addTab('repl', 'REPL');
    mgr.addTab('shell', 'Shell');
    expect(mgr.activeTab).toBe('repl');
    mgr.switchTab('shell');
    expect(mgr.activeTab).toBe('shell');
  });

  it('fires onSwitch callback', async () => {
    const { TerminalTabManager } = await import('@client/terminal/terminal');
    const mgr = new TerminalTabManager();
    mgr.addTab('repl', 'REPL');
    mgr.addTab('shell', 'Shell');
    const handler = vi.fn();
    mgr.onSwitch(handler);
    mgr.switchTab('shell');
    expect(handler).toHaveBeenCalledWith('shell', 'repl');
  });
});
```

**Pass criteria:** `npx vitest run test/client/repl.test.ts` — all green.

---

## 13. Integration Tests

### Task ID: `INTEGRATION-01`

**Dependencies:** All previous tasks.

**Source files:** `client/app.ts`, updated `server/index.ts`.

### Deliverables

**Test file: `test/helpers.ts`**

```typescript
import http from 'http';
import fs from 'fs';
import os from 'os';
import path from 'path';
import WebSocket from 'ws';
import { ServerTransport } from '@server/transport';
import { FsApi } from '@server/fs-api';
import { FileWatcher } from '@server/file-watcher';
import { ShellManager } from '@server/shell-manager';
import { ClientTransport } from '@client/transport';
import { RpcClient } from '@client/rpc';
import { CH } from '@shared/protocol';

export interface TestContext {
  server: http.Server;
  transport: ClientTransport;
  rpc: RpcClient;
  projectRoot: string;
  teardown(): Promise<void>;
}

export async function startTestServer(): Promise<TestContext> {
  const projectRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'lite-integration-'));
  const server = http.createServer();
  const serverTransport = new ServerTransport(server);
  const fsApi = new FsApi(projectRoot);
  const watcher = new FileWatcher(projectRoot);
  const shellManager = new ShellManager();

  serverTransport.onConnection((session) => {
    fsApi.attach(session);
    watcher.attach(session);
    shellManager.attach(session);
    session.onClose(() => watcher.detach(session.id));
  });

  const port = await new Promise<number>(resolve => {
    server.listen(0, '127.0.0.1', () => resolve((server.address() as any).port));
  });

  const clientTransport = new ClientTransport(WebSocket as any);
  clientTransport.connect(`ws://127.0.0.1:${port}/ws`);
  await new Promise<void>((resolve) => {
    const check = () => clientTransport.state === 'open' ? resolve() : setTimeout(check, 50);
    check();
  });

  const rpc = new RpcClient(clientTransport, CH.FS);

  return {
    server,
    transport: clientTransport,
    rpc,
    projectRoot,
    async teardown() {
      clientTransport.close();
      shellManager.destroyAll?.();
      await watcher.close();
      await serverTransport.close();
      await new Promise<void>(r => server.close(() => r()));
      fs.rmSync(projectRoot, { recursive: true, force: true });
    },
  };
}
```

**Test file: `test/integration/fs-roundtrip.test.ts`**

```typescript
import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { startTestServer, TestContext } from '../helpers';

// TASK: INTEGRATION-01
describe('INTEGRATION: FS round-trip', () => {
  let ctx: TestContext;
  beforeAll(async () => { ctx = await startTestServer(); });
  afterAll(async () => { await ctx.teardown(); });

  it('create → read → modify → read', async () => {
    await ctx.rpc.call('writeFile', { path: '/test.txt', content: 'v1' });
    const v1 = await ctx.rpc.call<{ content: string }>('readFile', { path: '/test.txt' });
    expect(v1.content).toBe('v1');

    await ctx.rpc.call('writeFile', { path: '/test.txt', content: 'v2' });
    const v2 = await ctx.rpc.call<{ content: string }>('readFile', { path: '/test.txt' });
    expect(v2.content).toBe('v2');
  });

  it('mkdir → writeFile → readdir', async () => {
    await ctx.rpc.call('mkdir', { path: '/dir' });
    await ctx.rpc.call('writeFile', { path: '/dir/file.txt', content: 'nested' });
    const entries = await ctx.rpc.call<any[]>('readdir', { path: '/dir' });
    expect(entries.map((e: any) => e.name)).toContain('file.txt');
  });

  it('rename then read at new path', async () => {
    await ctx.rpc.call('writeFile', { path: '/old.txt', content: 'data' });
    await ctx.rpc.call('rename', { oldPath: '/old.txt', newPath: '/new.txt' });
    await expect(ctx.rpc.call('readFile', { path: '/old.txt' })).rejects.toThrow();
    const r = await ctx.rpc.call<{ content: string }>('readFile', { path: '/new.txt' });
    expect(r.content).toBe('data');
  });

  it('unlink then read fails', async () => {
    await ctx.rpc.call('writeFile', { path: '/del.txt', content: 'gone' });
    await ctx.rpc.call('unlink', { path: '/del.txt' });
    await expect(ctx.rpc.call('readFile', { path: '/del.txt' })).rejects.toThrow();
  });
});
```

**Test file: `test/integration/shell-roundtrip.test.ts`**

```typescript
import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { startTestServer, TestContext } from '../helpers';
import { CH, FLAGS } from '@shared/protocol';

// TASK: INTEGRATION-01
describe('INTEGRATION: Shell round-trip', () => {
  let ctx: TestContext;
  let shellOutput: string;

  beforeAll(async () => {
    ctx = await startTestServer();
    shellOutput = '';
    ctx.transport.onMessage(CH.SHELL, (data) => {
      shellOutput += new TextDecoder().decode(data);
    });
    await new Promise(r => setTimeout(r, 2000));
  });

  afterAll(async () => { await ctx.teardown(); });

  function sendInput(text: string) {
    ctx.transport.send(CH.SHELL, FLAGS.BINARY, new TextEncoder().encode(text));
  }

  async function waitFor(contains: string, timeout = 5000) {
    const start = Date.now();
    while (Date.now() - start < timeout) {
      if (shellOutput.includes(contains)) return;
      await new Promise(r => setTimeout(r, 100));
    }
    throw new Error(`Timeout waiting for "${contains}"`);
  }

  it('echo returns output', async () => {
    shellOutput = '';
    sendInput('echo INTEGRATION_TEST\n');
    await waitFor('INTEGRATION_TEST');
  });

  it('shell writes file readable via FS API', async () => {
    sendInput(`echo "from-shell" > ${ctx.projectRoot}/shell.txt\n`);
    await new Promise(r => setTimeout(r, 500));
    const r = await ctx.rpc.call<{ content: string }>('readFile', { path: '/shell.txt' });
    expect(r.content.trim()).toBe('from-shell');
  });

  it('FS API writes file readable from shell', async () => {
    await ctx.rpc.call('writeFile', { path: '/api.txt', content: 'from-api' });
    shellOutput = '';
    sendInput(`cat ${ctx.projectRoot}/api.txt\n`);
    await waitFor('from-api');
  });
});
```

**Test file: `test/integration/watch-roundtrip.test.ts`**

```typescript
import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import fs from 'fs';
import path from 'path';
import { startTestServer, TestContext } from '../helpers';
import { CH } from '@shared/protocol';
import type { WatchEvent } from '@shared/protocol';

// TASK: INTEGRATION-01
// NOTE: These tests verify watch events ARRIVE correctly at the client.
// The Buffer Sovereignty invariant (events do NOT auto-modify the editor) is
// tested in test/client/editor.test.ts under "Buffer sovereignty — remote changes".
// The watch-roundtrip tests confirm the TRANSPORT works; the editor tests confirm
// the INVARIANT holds. Both must pass.
describe('INTEGRATION: Watch events', () => {
  let ctx: TestContext;
  let watchEvents: WatchEvent[];

  beforeAll(async () => {
    ctx = await startTestServer();
    watchEvents = [];
    ctx.transport.onMessage(CH.WATCH, (data) => {
      watchEvents.push(JSON.parse(new TextDecoder().decode(data)));
    });
    await new Promise(r => setTimeout(r, 1500));
  });

  afterAll(async () => { await ctx.teardown(); });

  it('detects direct disk write', async () => {
    watchEvents.length = 0;
    fs.writeFileSync(path.join(ctx.projectRoot, 'disk.txt'), 'hello');
    await new Promise(r => setTimeout(r, 1500));
    expect(watchEvents.filter(e => e.event === 'add' && e.path === 'disk.txt').length).toBeGreaterThanOrEqual(1);
  });

  it('detects RPC-triggered modification', async () => {
    await ctx.rpc.call('writeFile', { path: '/watched.txt', content: 'v1' });
    await new Promise(r => setTimeout(r, 1000));
    watchEvents.length = 0;
    await ctx.rpc.call('writeFile', { path: '/watched.txt', content: 'v2' });
    await new Promise(r => setTimeout(r, 1500));
    expect(watchEvents.filter(e => e.event === 'change' && e.path === 'watched.txt').length).toBeGreaterThanOrEqual(1);
  });

  it('detects RPC-triggered deletion', async () => {
    await ctx.rpc.call('writeFile', { path: '/del.txt', content: 'temp' });
    await new Promise(r => setTimeout(r, 1000));
    watchEvents.length = 0;
    await ctx.rpc.call('unlink', { path: '/del.txt' });
    await new Promise(r => setTimeout(r, 1500));
    expect(watchEvents.filter(e => e.event === 'unlink' && e.path === 'del.txt').length).toBeGreaterThanOrEqual(1);
  });
});
```

**Pass criteria:** `npx vitest run test/integration/` — all green.

---

## 14. Task Execution Order

```
Phase 1 — Foundation
  1. SCAFFOLD-01        → test/scaffold.test.ts
  2. PROTO-01           → test/shared/protocol.test.ts
  3. CONFIG-01          → test/server/config.test.ts

Phase 2 — Transport
  4. TRANSPORT-SERVER-01 → test/server/transport.test.ts
  5. TRANSPORT-CLIENT-01 → test/client/transport.test.ts

Phase 3 — Server APIs
  6. FS-SERVER-01       → test/server/fs-api.test.ts
  7. WATCH-01           → test/server/file-watcher.test.ts
  8. SHELL-01           → test/server/shell-manager.test.ts

Phase 4 — Client UI
  9.  UI-LAYOUT-01      → test/client/layout.test.ts
  10. UI-TREE-01        → test/client/tree.test.ts
  11. UI-EDITOR-01      → test/client/editor.test.ts
  12. UI-TERMINAL-01    → test/client/repl.test.ts

Phase 5 — Integration
  13. INTEGRATION-01    → test/integration/*.test.ts + test/helpers.ts
```

After each task: `npx vitest run` (full suite) to catch regressions.
After each phase: `bash scripts/ci.sh` for full pipeline.

---

## 15. Agent Instructions Template

```
## Task: {TASK-ID}

### Context
You are building a lightweight code editor called "lite-editor".
The full build plan is in ./lite-editor-build-plan.md
You are working on task {TASK-ID}.

### Previous tasks completed
{list of completed task IDs, or "none — this is the first task"}

### Your task
{paste the task section from this document}

### Deliverables
1. All source files described in the task
2. The test file exactly as specified (you may add tests but do not remove any)
3. Run the tests: `npx vitest run {test-file-path}`
4. Report pass/fail for every test case
5. If any test fails, fix the implementation and re-run until green

### Rules
- All tests must pass before marking the task complete
- Do not modify files outside the scope of this task unless strictly necessary
- If a test from a PREVIOUS task breaks, fix it and note what changed
- If you encounter a design issue, document it as a TODO comment and note it in your report
- Commit with message: "{TASK-ID}: {brief description}"
- After committing, run the FULL test suite: `npx vitest run`
- Report any regressions from previous tasks
```

---

## 16. CI Script

**File: `scripts/ci.sh`**

```bash
#!/bin/bash
set -euo pipefail

echo "=== Lite Editor CI ==="

echo "--- Installing dependencies ---"
npm ci

echo "--- Type checking ---"
npx tsc --noEmit

echo "--- Building server ---"
npx tsup

echo "--- Building client ---"
node esbuild.config.ts

echo "--- Running unit tests ---"
npx vitest run --testPathPattern='test/(shared|server|client)/'

echo "--- Running integration tests ---"
npx vitest run --testPathPattern='test/integration/'

echo "=== All checks passed ==="
```
