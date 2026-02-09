# Odin Request Log File

**Document Version:** 1.1  
**Author:** Chason Tang  
**Last Updated:** 2026-02-09  
**Status:** Implemented

---

## 1. Executive Summary

Add a structured request log file output to Odin so that developers can inspect every HTTP request (including full headers and body) without enabling the noisy `--debug` mode. The log file uses NDJSON format (one JSON object per line) and is activated via a new `--log-file=<path>` CLI flag.

### 1.1 Background

Odin currently has two logging levels, neither of which is well-suited for production request analysis:

1. **Default mode** — prints a single summary line per request to stderr (`[Odin] <timestamp> <method> <path> <status> <duration>`). No headers or body are included, making it impossible to inspect what Claude Code actually sends.
2. **Debug mode** (`--debug`) — logs full headers, body, converted Google payloads, raw SSE chunks, and converted Anthropic events. The output volume is too high for production use and mixes protocol-conversion internals with the request data developers actually care about.

There is no middle ground: developers cannot get request headers and body without also drowning in conversion noise, and no output is structured for machine consumption.

Questions developers need to answer in production:

- Are `/health` and `/` (heartbeat) actually being called by Claude Code? What are the headers and body? This determines whether these endpoints should be kept or deprecated.
- What exactly does Claude Code send in `/v1/messages` requests? Inspecting headers and body across a full session reveals how multi-turn conversations, tool use, and thinking are structured.
- Does Claude Code ever hit unimplemented endpoints (those returning 404)? This surfaces new API requirements that Odin needs to support.

### 1.2 Goals

- **Primary**: Write every HTTP request's full details (method, path, status, duration, headers, body) to a dedicated log file in structured NDJSON format
- **Secondary**: Maintain the zero-runtime-dependency principle (use only Node.js built-in modules)
- **Non-Goals**:
  - Log rotation — delegate to external tools (e.g., `logrotate`) or manual management
  - Response body recording — response bodies are SSE streams; capturing them is expensive and out of scope
  - Replacing existing stderr logging — the file log is an additional output channel; stderr behavior remains unchanged

### 1.3 Key Features

| Feature | Current | Target |
|---------|---------|--------|
| Request log destination | stderr only | stderr + dedicated log file |
| Headers/body recording | `--debug` mode only | Always recorded in log file |
| Log format | Plain text | NDJSON (one JSON object per line) |
| Runtime dependencies | 0 | 0 (uses `node:fs`) |

---

## 2. Technical Design

### 2.1 Architecture Overview

```
┌──────────────────────────────────────────────────────────────────────┐
│                              Odin                                    │
├──────────────────────────────────────────────────────────────────────┤
│                                                                      │
│  ┌───────────────┐    ┌──────────────────┐    ┌──────────────────┐   │
│  │ Claude Code   │───▶│  Native HTTP     │───▶│ Antigravity      │   │
│  │ CLI           │◀───│  Server          │◀───│ Cloud Code API   │   │
│  └───────────────┘    └──────────────────┘    └──────────────────┘   │
│                               │                                      │
│                               │ every request                        │
│                               ▼                                      │
│                  ┌────────────────────────┐                          │
│                  │     logger.js          │                          │
│                  │  ┌──────────────────┐  │                          │
│                  │  │ RequestLogger    │  │                          │
│                  │  │  - writeStream   │──│──▶ logs/requests.ndjson  │
│                  │  │  - log()         │  │                          │
│                  │  │  - close()       │  │                          │
│                  │  └──────────────────┘  │                          │
│                  └────────────────────────┘                          │
│                                                                      │
└──────────────────────────────────────────────────────────────────────┘
```

### 2.2 File Changes

```
odin/
├── src/
│   ├── logger.js              # NEW: request log file writer
│   ├── index.js               # MODIFIED: add --log-file CLI parameter
│   └── server.js              # MODIFIED: integrate logger into request handlers
└── logs/                      # Created at runtime (.gitignore)
    └── requests.ndjson        # Default log file path
```

### 2.3 Data Structures

#### 2.3.1 Log Entry Format (NDJSON)

Each request produces a single JSON line with the following fields:

```javascript
/**
 * Request log entry structure.
 *
 * @typedef {Object} RequestLogEntry
 * @property {string} timestamp    - ISO 8601 timestamp
 * @property {string} method       - HTTP method (GET, POST, etc.)
 * @property {string} path         - Request path (query string stripped)
 * @property {string} url          - Original URL (including query string)
 * @property {number} statusCode   - Response status code
 * @property {number} durationMs   - Request processing time in milliseconds
 * @property {Object} headers      - Full request headers
 * @property {Object|null} body    - Parsed request body (JSON), null if empty/unparseable
 */
```

Example log lines:

```json
{"timestamp":"2026-02-09T10:30:00.123Z","method":"GET","path":"/health","url":"/health","statusCode":200,"durationMs":1,"headers":{"host":"localhost:8080","user-agent":"claude-code/1.0","accept":"*/*"},"body":null}
```

```json
{"timestamp":"2026-02-09T10:30:01.456Z","method":"POST","path":"/v1/messages","url":"/v1/messages","statusCode":200,"durationMs":1523,"headers":{"host":"localhost:8080","content-type":"application/json","x-api-key":"sk-ant-...","anthropic-version":"2023-06-01"},"body":{"model":"claude-sonnet-4-5-thinking","messages":[{"role":"user","content":"Hello!"}],"max_tokens":4096,"stream":true}}
```

```json
{"timestamp":"2026-02-09T10:30:05.789Z","method":"GET","path":"/v1/models","url":"/v1/models","statusCode":404,"durationMs":0,"headers":{"host":"localhost:8080","accept":"application/json"},"body":null}
```

#### 2.3.2 Why NDJSON

| Dimension | NDJSON | Plain Text | CSV |
|-----------|--------|------------|-----|
| Structured queries | `jq 'select(.path=="/health")'` | Requires complex regex | Limited |
| Nested data (headers/body) | Native JSON nesting | Custom delimiters needed | Cannot store nested data |
| Append writes | Each line is independent | Same | Same |
| Readability | One JSON per line; format with `jq .` | Good | Fair |
| Fault tolerance | Corrupted line does not affect others | Same | Same |

### 2.4 Core Logic

#### 2.4.1 RequestLogger Class

```javascript
// src/logger.js

import fs from 'node:fs';
import path from 'node:path';

/**
 * Writes structured request logs in NDJSON format to a file.
 *
 * Uses fs.createWriteStream in append mode:
 * - High performance: leverages Node.js internal buffering to minimize syscalls
 * - Safety: call close() on graceful shutdown to flush the buffer
 */
export class RequestLogger {
    /**
     * @param {string} filePath - Path to the log file
     */
    constructor(filePath) {
        // Ensure the log directory exists
        const dir = path.dirname(filePath);
        fs.mkdirSync(dir, { recursive: true });

        this._stream = fs.createWriteStream(filePath, {
            flags: 'a',          // Append mode
            encoding: 'utf8'
        });

        this._stream.on('error', (err) => {
            console.error(`[Odin] Log file write error: ${err.message}`);
        });
    }

    /**
     * Write a single request log entry.
     *
     * @param {Object} params
     * @param {http.IncomingMessage} params.req - Request object
     * @param {Object|null} params.body - Parsed request body
     * @param {number} params.statusCode - Response status code
     * @param {number} params.startTime - Request start timestamp (Date.now())
     */
    log({ req, body, statusCode, startTime }) {
        const entry = {
            timestamp: new Date().toISOString(),
            method: req.method,
            path: req.url.split('?')[0],
            url: req.url,
            statusCode,
            durationMs: Date.now() - startTime,
            headers: req.headers,
            body: body ?? null
        };

        this._stream.write(JSON.stringify(entry) + '\n');
    }

    /**
     * Close the write stream and flush the buffer.
     * Call this during graceful shutdown.
     *
     * @returns {Promise<void>}
     */
    close() {
        return new Promise((resolve) => {
            this._stream.end(resolve);
        });
    }
}
```

#### 2.4.2 server.js Integration

Call `logger.log()` after sending the response in every route handler:

```javascript
// server.js changes (abridged)

/**
 * Create and return the Odin HTTP server.
 *
 * @param {Object} options
 * @param {string} options.apiKey - Cloud Code API key
 * @param {boolean} options.debug - Enable debug logging
 * @param {RequestLogger|null} options.logger - Request log writer (optional)
 * @returns {http.Server}
 */
export function createServer({ apiKey, debug, logger }) {
    const server = http.createServer(async (req, res) => {
        const body = await readBody(req);
        const startTime = logRequest(req, body, debug);

        const { method, url } = req;
        const path = url.split('?')[0];

        // ── /health ──
        if (method === 'GET' && path === '/health') {
            sendJSON(res, 200, { status: 'ok' });
            logResponse(req, 200, startTime);
            logger?.log({ req, body, statusCode: 200, startTime });  // ← NEW
            return;
        }

        // ── /v1/messages ──
        if (method === 'POST' && path === '/v1/messages') {
            // ... existing logic ...
            // Add before every return:
            // logger?.log({ req, body, statusCode: xxx, startTime });
        }

        // ── / (heartbeat) ──
        if (method === 'POST' && path === '/') {
            sendJSON(res, 200, {});
            logResponse(req, 200, startTime);
            logger?.log({ req, body, statusCode: 200, startTime });  // ← NEW
            return;
        }

        // ── /api/event_logging/batch ──
        if (method === 'POST' && path === '/api/event_logging/batch') {
            sendJSON(res, 200, {});
            logResponse(req, 200, startTime);
            logger?.log({ req, body, statusCode: 200, startTime });  // ← NEW
            return;
        }

        // ── 404 Unknown Endpoint ──
        sendError(res, 404, 'not_found_error', `Unknown endpoint: ${method} ${path}`);
        logResponse(req, 404, startTime, '← UNKNOWN ENDPOINT');
        logger?.log({ req, body, statusCode: 404, startTime });  // ← NEW
    });

    return server;
}
```

**Design note**: The optional chaining `logger?.log()` ensures zero side effects when no logger is provided (i.e., `--log-file` is not specified), maintaining full backward compatibility.

#### 2.4.3 index.js Integration

```javascript
// index.js changes (abridged)

import { RequestLogger } from './logger.js';

// Parse the new CLI parameter
const logFile = args['log-file'] || null;

// Create logger only when --log-file is specified
const logger = logFile ? new RequestLogger(logFile) : null;

// Pass to createServer
const server = createServer({ apiKey, debug, logger });

server.listen(port, () => {
    console.error(`[Odin] Server listening on http://localhost:${port}`);
    console.error(`[Odin] Debug mode: ${debug ? 'ON' : 'OFF'}`);
    if (logger) {
        console.error(`[Odin] Request log file: ${logFile}`);
    }
    console.error(`[Odin] Press Ctrl+C to stop`);
});

// Flush log buffer on graceful shutdown
process.on('SIGINT', async () => {
    console.error('\n[Odin] Shutting down...');
    await logger?.close();
    server.close(() => process.exit(0));
});

process.on('SIGTERM', async () => {
    console.error('[Odin] Received SIGTERM, shutting down...');
    await logger?.close();
    server.close(() => process.exit(0));
});
```

### 2.5 Flow Diagram

```
Claude Code CLI                 Odin server.js                     logger.js
      │                              │                                 │
      │  HTTP Request                │                                 │
      │─────────────────────────────▶│                                 │
      │                              │                                 │
      │                              │  readBody(req)                  │
      │                              │  logRequest(req, body, debug)   │
      │                              │  ──────────────▶ stderr         │
      │                              │                                 │
      │                              │  ... route handling ...         │
      │                              │  sendJSON / sendError / SSE     │
      │◀─────────────────────────────│                                 │
      │  HTTP Response               │                                 │
      │                              │  logResponse(...)               │
      │                              │  ──────────────▶ stderr         │
      │                              │                                 │
      │                              │  logger.log({req, body,         │
      │                              │    statusCode, startTime})      │
      │                              │────────────────────────────────▶│
      │                              │                                 │  JSON.stringify
      │                              │                                 │  + '\n'
      │                              │                                 │  ──▶ write stream
      │                              │                                 │       ──▶ file
```

---

## 3. Interface Design

### 3.1 CLI Interface

```
Usage: node src/index.js --api-key=<key> [--port=<port>] [--debug] [--log-file=<path>]

Options:
  --api-key=<key>       API key for Antigravity Cloud Code (required)
  --port=<port>         Port to listen on (default: 8080)
  --debug               Enable debug logging
  --log-file=<path>     Write request logs to file in NDJSON format (optional)

Examples:
  # Basic usage (no log file)
  node src/index.js --api-key="ya29.a0AeO..."

  # Enable request log file
  node src/index.js --api-key="ya29..." --log-file=logs/requests.ndjson

  # Enable both debug mode and log file
  node src/index.js --api-key="ya29..." --debug --log-file=logs/requests.ndjson
```

### 3.2 Querying the Log File

NDJSON pairs naturally with `jq`. The most useful pattern is `tail -f` for real-time monitoring while Odin is running — pipe new log lines into `jq` as they are written.

#### Real-time monitoring with `tail -f`

```bash
# Watch all requests in real time (pretty-printed)
tail -f logs/requests.ndjson | jq .

# Live-monitor only /health and / (heartbeat) requests to determine if
# these endpoints are actually used in production
tail -f logs/requests.ndjson | jq 'select(.path == "/health" or .path == "/")'

# Live-monitor /health and / with full headers for deprecation analysis
tail -f logs/requests.ndjson | jq 'select(.path == "/health" or .path == "/") | {timestamp, path, headers, body}'

# Live-monitor all /v1/messages requests — inspect headers and body to
# understand how Claude Code implements multi-turn conversations
tail -f logs/requests.ndjson | jq 'select(.path == "/v1/messages") | {timestamp, headers, body}'

# Live-monitor /v1/messages and show conversation depth (number of turns)
tail -f logs/requests.ndjson | jq 'select(.path == "/v1/messages") | {timestamp, turns: (.body.messages | length), model: .body.model}'

# Live-monitor 404 responses — discover unimplemented endpoints that
# Claude Code is trying to reach
tail -f logs/requests.ndjson | jq 'select(.statusCode == 404)'

# Live-monitor all non-200 responses
tail -f logs/requests.ndjson | jq 'select(.statusCode != 200)'
```

#### Post-hoc analysis

```bash
# Show the last 20 requests
tail -20 logs/requests.ndjson | jq .

# Count requests per path
jq -r '.path' logs/requests.ndjson | sort | uniq -c | sort -rn

# Filter by time range
jq 'select(.timestamp >= "2026-02-09T10:00:00" and .timestamp <= "2026-02-09T11:00:00")' logs/requests.ndjson

# Show all unique 404 paths (unimplemented endpoints)
jq -r 'select(.statusCode == 404) | "\(.method) \(.path)"' logs/requests.ndjson | sort -u

# Average response time for /v1/messages
jq '[select(.path == "/v1/messages") | .durationMs] | add / length' logs/requests.ndjson
```

### 3.3 Error Handling

| Condition | Behavior |
|-----------|----------|
| `--log-file` points to a non-existent directory | `RequestLogger` constructor creates the directory tree via `fs.mkdirSync(dir, { recursive: true })` |
| Log file write failure (e.g., disk full) | Error printed to stderr (`[Odin] Log file write error: ...`); request handling is not affected |
| `--log-file` not specified | No `RequestLogger` is created; zero file I/O overhead; behavior identical to current version |
| Ungraceful termination (SIGKILL) | Last few log entries in the write stream buffer may be lost (acceptable) |

---

## 4. Implementation Plan

### Phase 1: RequestLogger Module (Estimated: 0.5 hours)

**Task 1.1: Create `src/logger.js`**
- [x] Implement `RequestLogger` class
- [x] Constructor: accept file path, create directory, open write stream in append mode
- [x] `log()` method: serialize request info to JSON and append to file
- [x] `close()` method: return a Promise that resolves when the stream is flushed and closed

**Acceptance Criteria:**
- `RequestLogger` can be independently instantiated and writes valid NDJSON
- Write errors are caught and printed to stderr without throwing or interrupting the caller

### Phase 2: CLI and Server Integration (Estimated: 1 hour)

**Task 2.1: Modify `src/index.js`**
- [x] Parse new `--log-file=<path>` argument
- [x] Conditionally create `RequestLogger` instance
- [x] Pass logger to `createServer()` options
- [x] Print log file path on startup
- [x] Update `printUsage()` help text
- [x] Call `logger.close()` on graceful shutdown (SIGINT, SIGTERM)

**Task 2.2: Modify `src/server.js`**
- [x] Accept `logger` parameter in `createServer()`
- [x] Add `logger?.log()` call after response is sent in every route (5 locations: `/health`, `/v1/messages` multiple exits, `/`, `/api/event_logging/batch`, 404 fallback)

**Acceptance Criteria:**
- Without `--log-file`: behavior is identical to the current version
- With `--log-file`: every request produces exactly one log line
- Log file is properly closed on process exit

### Phase 3: Validation (Estimated: 0.5 hours)

**Task 3.1: Manual testing**
- [ ] Start Odin with `--log-file`
- [ ] Run a full Claude Code conversation
- [ ] Verify log file contains entries for all request types (`/health`, `/v1/messages`, `/`, potential 404s)
- [ ] Verify `tail -f | jq` queries work in real time

---

## 5. Testing

### 5.1 Test Cases

| Test Scenario | Input | Expected Output |
|---------------|-------|-----------------|
| No `--log-file` specified | Normal requests | No log file created; stderr logs unchanged |
| `--log-file=logs/test.ndjson` | GET /health | File contains one JSON line with `path: "/health"`, `statusCode: 200` |
| POST /v1/messages | Normal streaming request | File contains one JSON line with full request body |
| POST / (heartbeat) | Empty or `{}` body | File contains one JSON line with `body: null` or `body: {}` |
| Unknown endpoint | GET /v1/models | File contains one JSON line with `statusCode: 404` |
| Non-existent log directory | `--log-file=new_dir/test.ndjson` | Directory auto-created; writes succeed |
| SIGINT shutdown | Ctrl+C | Log file is complete; buffer is flushed |
| Real-time monitoring | `tail -f \| jq .` | New entries appear as requests arrive |

### 5.2 Manual Testing Steps

```bash
# 1. Start Odin with log file
node src/index.js --api-key="$(cat ~/.odin-key)" --port=8080 --log-file=logs/requests.ndjson

# 2. In a second terminal, start real-time monitoring
tail -f logs/requests.ndjson | jq .

# 3. In a third terminal, configure and launch Claude Code
export ANTHROPIC_BASE_URL=http://localhost:8080
export ANTHROPIC_API_KEY=dummy
export ANTHROPIC_MODEL=claude-sonnet-4-5-thinking
claude

# 4. Interact with Claude Code and observe live log output in the monitoring terminal

# 5. After exiting Claude Code, run post-hoc queries
jq -r '.path' logs/requests.ndjson | sort | uniq -c | sort -rn
jq 'select(.statusCode == 404)' logs/requests.ndjson
```

---

## 6. Risk Assessment

### 6.1 Risks and Mitigations

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| Log file grows unbounded | Medium | Low | Document use of external rotation tools (e.g., `logrotate`); consider built-in rotation in a future iteration |
| Large request bodies (e.g., base64 images) produce oversized log lines | Low | Low | No truncation for now to preserve completeness; add `--log-max-body-size` if needed later |
| Write I/O affects request latency | Low | Low | `fs.WriteStream` buffers internally; `write()` is asynchronous and does not block the event loop |
| Sensitive data leakage (API keys in headers) | Medium | Medium | Log file is opt-in (requires explicit `--log-file`); document that operators should manage file permissions appropriately |

---

## 7. Future Considerations

### 7.1 Potential Extensions

| Feature | Status | Notes |
|---------|--------|-------|
| Body size truncation | 💡 Idea | Add `--log-max-body-size` flag; bodies exceeding the threshold are truncated and marked `"truncated": true` |
| Built-in log rotation | 💡 Idea | Rotate by file size or date (e.g., `requests-2026-02-09.ndjson`) to prevent unbounded growth |
| Response body recording | 💡 Idea | Record response bodies for non-streaming endpoints (`/health`, 404, etc.); for streaming endpoints, record a token usage summary instead |
| Header redaction | 💡 Idea | Automatically mask sensitive header values (`Authorization`, `x-api-key`, etc.) |

---

## 8. Appendix

### 8.1 References

1. `odin/src/server.js` — Current request handling and logging logic
2. `odin/src/index.js` — Current CLI argument parsing and server startup
3. [NDJSON Specification](http://ndjson.org/) — Newline Delimited JSON format
4. [Node.js fs.createWriteStream](https://nodejs.org/api/fs.html#fscreatewritestreampath-options) — Write stream API documentation

### 8.2 Related Documents

| Document | Description |
|----------|-------------|
| `odin/docs/odin_basic_design.md` | Odin base architecture design; §2.7 describes the existing logging mechanism |

---

## Changelog

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.1 | 2026-02-09 | Chason Tang | Rewrite in English; add `tail -f \| jq` real-time monitoring examples |
| 1.0 | 2026-02-09 | Chason Tang | Initial version |

---

*End of Technical Design Document*
