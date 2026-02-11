# Odin Unified File Logging

**Document Version:** 1.3  
**Author:** Chason Tang  
**Last Updated:** 2026-02-10  
**Status:** Implemented

---

## 1. Executive Summary

Overhaul Odin's request logging system to make NDJSON file logging the default and primary destination for all request and error data, including Google Antigravity Cloud Code upstream errors that are currently only printed to stderr. Remove the per-request stderr summary lines (which are easily lost to terminal clears) while preserving stderr for standard diagnostic output (startup messages, debug traces, operational errors).

Extend `--debug` mode to also record structured response data for the `/v1/messages` endpoint (SSE events) in the log file, providing a complete request/response pair for conversion debugging without introducing a separate flag.

### 1.1 Background

Odin's logging is currently split across two channels with significant gaps:

1. **stderr request summary lines (always on)** — The `logRequest()` and `logResponse()` functions print a one-line summary per request (`[Odin] <timestamp> <method> <path> <status> <duration>`). These are **request logs**, not diagnostic messages, yet they live on stderr where they are easily lost. A single terminal clear (`Cmd+K`, `clear`, or IDE terminal reset) permanently destroys the request history. When investigating an issue that happened minutes or hours ago, the data is gone.

2. **NDJSON file (opt-in via `--log-file`)** — Writes structured request entries (method, path, status, duration, headers, body) to a file. This was added in the Request Log File feature but has two limitations:
   - It is **opt-in** — developers must remember to pass `--log-file=<path>` every time they start Odin. If they forget, no file log is produced.
   - It does **not record Cloud Code errors** — when the upstream API returns an error (e.g., 429 rate limit, 401 auth failure, 500 server error), the error status code, message, and raw body are only printed to stderr via `console.error`. The NDJSON entry records the final HTTP status Odin returns to Claude Code, but not the upstream error details that explain *why* the request failed.

3. **stderr diagnostic logging (unchanged)** — Startup/shutdown messages, `--debug` traces (converted Google requests, upstream request details, SSE chunks), and operational error messages. These are standard diagnostic outputs that follow Unix conventions (diagnostic → stderr) and are **not** being deprecated.

The net result: the most critical diagnostic information — upstream Cloud Code API failures — is recorded only in the request log channel (stderr summary lines), which is the least reliable. Meanwhile, the structured file log that would be the ideal destination does not capture error details.

### 1.2 Goals

- **Primary**: Make file logging always-on by default (output to `logs/requests.ndjson`), including Cloud Code upstream error details in every log entry
- **Secondary**: Remove per-request stderr summary lines (`logRequest()` / `logResponse()`), whose role is fully superseded by the always-on file logger
- **Tertiary**: Extend `--debug` to record `/v1/messages` response SSE events in the log file, enabling full request/response inspection for conversion debugging
- **Non-Goals**:
  - Deprecating stderr for diagnostic output — startup messages, debug traces, and operational errors remain on stderr (standard Unix practice)
  - Log rotation — delegate to external tools or manual management
  - Response recording for non-`/v1/messages` endpoints — their responses are trivial (`{"status":"ok"}`, `{}`, static error objects) and do not warrant recording

### 1.3 Key Features

| Feature | Current | Target |
|---------|---------|--------|
| Request log destination | stderr summary + opt-in file | File only (always on) |
| Cloud Code error recording | stderr only | NDJSON file with structured `error` field |
| Default log file | None (requires `--log-file`) | `logs/requests.ndjson` |
| `--log-file` behavior | Enables file logging | Overrides default file path |
| Log file on startup | Appended | Cleared (truncated) |
| Per-request stderr lines | `logRequest()` + `logResponse()` | Removed (superseded by file logger) |
| Diagnostic stderr | Startup, debug, errors | Unchanged — remains on stderr |
| `--debug` scope | stderr traces only | stderr traces + `debug` and `response` fields in log file |
| `/v1/messages` response recording | Not available | Opt-in via `--debug` (SSE events as structured array) |

---

## 2. Technical Design

### 2.1 Architecture Overview

```
┌───────────────────────────────────────────────────────────────────────────────┐
│                                  Odin                                         │
├───────────────────────────────────────────────────────────────────────────────┤
│                                                                               │
│  ┌───────────────┐    ┌──────────────────┐    ┌──────────────────┐            │
│  │ Claude Code   │───▶│  Native HTTP     │───▶│ Antigravity      │            │
│  │ CLI           │◀───│  Server          │◀───│ Cloud Code API   │            │
│  └───────────────┘    └──────────────────┘    └──────────────────┘            │
│                               │                       │                       │
│                               │ every request         │ errors                │
│                               ▼                       ▼                       │
│                  ┌──────────────────────────────────────────┐                 │
│                  │              logger.js                   │                 │
│                  │  ┌──────────────────────────────────┐    │                 │
│                  │  │ RequestLogger                    │    │                 │
│                  │  │  - writeStream (mode: 'w')       │────│──▶ logs/requests.ndjson
│                  │  │  - log()  ← always called        │    │   (cleared on startup)
│                  │  │  - close()                       │    │                 │
│                  │  └──────────────────────────────────┘    │                 │
│                  └──────────────────────────────────────────┘                 │
│                                                                               │
│  stderr: diagnostic output (startup, debug, operational errors) — UNCHANGED   │
│                                                                               │
└───────────────────────────────────────────────────────────────────────────────┘
```

### 2.2 File Changes

```
odin/
├── src/
│   ├── logger.js              # MODIFIED: write mode, error/response/debug field support
│   ├── index.js               # MODIFIED: default log file, always create logger
│   ├── server.js              # MODIFIED: remove request summary lines, add error/response/debug to logger
│   └── cloudcode.js           # UNCHANGED
└── logs/
    └── requests.ndjson        # Cleared on every startup
```

### 2.3 Data Structures

#### 2.3.1 Enhanced Log Entry Format (NDJSON)

Each request produces a single JSON line. The `error` field is conditionally present on any endpoint. The `response` and `debug` fields are only present for `/v1/messages` when `--debug` is active:

```javascript
/**
 * Enhanced request log entry structure.
 *
 * @typedef {Object} RequestLogEntry
 * @property {string} timestamp    - ISO 8601 timestamp
 * @property {string} method       - HTTP method (GET, POST, etc.)
 * @property {string} path         - Request path (query string stripped)
 * @property {string} url          - Original URL (including query string)
 * @property {number} statusCode   - Response status code returned to client
 * @property {number} durationMs   - Request processing time in milliseconds
 * @property {Object} headers      - Full request headers
 * @property {Object|null} body    - Parsed request body (JSON), null if empty/unparseable
 * @property {ErrorInfo} [error]   - Error details (omitted for successful requests)
 * @property {ResponseInfo} [response] - /v1/messages response SSE events (only when --debug is on)
 * @property {DebugInfo} [debug]   - Debug details (only when --debug is on)
 */

/**
 * Error information from Cloud Code or internal failures.
 *
 * @typedef {Object} ErrorInfo
 * @property {'cloud_code'|'internal'} source  - Error origin
 * @property {number} [upstreamStatus]         - HTTP status from Cloud Code (cloud_code only)
 * @property {string} message                  - Error message
 * @property {string} [rawBody]                - Raw error response body (cloud_code only)
 * @property {string} [stack]                  - Error stack trace (internal only)
 */

/**
 * Response information for /v1/messages endpoint (only when --debug flag is active).
 *
 * Only recorded for /v1/messages — the sole endpoint with non-trivial responses.
 * Other endpoints return static JSON bodies ({"status":"ok"}, {}, error objects)
 * that do not warrant recording.
 *
 * @typedef {Object} ResponseInfo
 * @property {SSEEvent[]} events    - Parsed SSE events sent to Claude Code
 */

/**
 * A single parsed SSE event.
 *
 * @typedef {Object} SSEEvent
 * @property {string} event  - SSE event type (e.g., "message_start", "content_block_delta")
 * @property {Object} data   - Parsed JSON data payload
 */
```

#### 2.3.2 Example Log Entries

**Successful request** (no optional fields):

```json
{"timestamp":"2026-02-10T10:30:00.123Z","method":"GET","path":"/health","url":"/health","statusCode":200,"durationMs":1,"headers":{"host":"localhost:8080","user-agent":"claude-code/1.0"},"body":null}
```

**Cloud Code upstream error** (`error.source: "cloud_code"`):

```json
{"timestamp":"2026-02-10T10:30:01.456Z","method":"POST","path":"/v1/messages","url":"/v1/messages","statusCode":429,"durationMs":523,"headers":{"host":"localhost:8080","content-type":"application/json"},"body":{"model":"claude-sonnet-4-5-thinking","messages":[{"role":"user","content":"Hello"}],"stream":true},"error":{"source":"cloud_code","upstreamStatus":429,"message":"Rate limit exceeded. Please retry after 60 seconds.","rawBody":"{\"error\":{\"code\":429,\"message\":\"Rate limit exceeded. Please retry after 60 seconds.\",\"status\":\"RESOURCE_EXHAUSTED\"}}"}}
```

**Internal error** (`error.source: "internal"`):

```json
{"timestamp":"2026-02-10T10:30:05.789Z","method":"POST","path":"/v1/messages","url":"/v1/messages","statusCode":500,"durationMs":3012,"headers":{"host":"localhost:8080","content-type":"application/json"},"body":{"model":"claude-sonnet-4-5-thinking","messages":[{"role":"user","content":"Hello"}],"stream":true},"error":{"source":"internal","message":"fetch failed","stack":"TypeError: fetch failed\n    at node:internal/deps/undici..."}}
```

**With `--debug` — /v1/messages with response SSE events (abridged):**

```json
{"timestamp":"2026-02-10T10:30:01.456Z","method":"POST","path":"/v1/messages","url":"/v1/messages","statusCode":200,"durationMs":1523,"headers":{"host":"localhost:8080","content-type":"application/json"},"body":{"model":"claude-sonnet-4-5-thinking","messages":[{"role":"user","content":"Hello"}],"stream":true},"response":{"events":[{"event":"message_start","data":{"type":"message_start","message":{"id":"msg_abc123","role":"assistant","content":[],"model":"claude-sonnet-4-5-thinking"}}},{"event":"content_block_start","data":{"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}}},{"event":"content_block_delta","data":{"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Hello!"}}},{"event":"content_block_stop","data":{"type":"content_block_stop","index":0}},{"event":"message_delta","data":{"type":"message_delta","delta":{"stop_reason":"end_turn"}}},{"event":"message_stop","data":{"type":"message_stop"}}]},"debug":{"googleRequest":{"contents":[{"role":"user","parts":[{"text":"Hello"}]}],"generationConfig":{"maxOutputTokens":4096}}}}
```

#### 2.3.3 Why `response` is Controlled by `--debug`

| Concern | Analysis |
|---------|----------|
| Log file size | SSE events for a single `/v1/messages` response can be hundreds of KB (thinking blocks, tool calls, long text). With response logging always on, a single Claude Code session could produce a multi-MB log file. |
| Default use case | Most debugging needs are served by request-side data (what did Claude Code send?) plus the status code and error details. Response data is only needed for conversion debugging — the same audience that uses `--debug`. |
| No separate flag needed | `--debug` already means "give me full diagnostic data". Adding response recording to it is a natural extension rather than introducing a new `--log-response` flag that would almost always be used together with `--debug` anyway. |
| Only `/v1/messages` matters | Other endpoints return trivial static bodies (`{"status":"ok"}`, `{}`, Anthropic error objects). The `/v1/messages` SSE stream is the only response worth recording — it reveals how Google-format responses are converted to Anthropic SSE events. |
| Performance | Buffering SSE events in memory during streaming adds overhead. For the common case (no `--debug`), there is zero allocation cost. |

#### 2.3.4 Debug Field

The `debug` field is populated when `--debug` is active, providing conversion-level details that are too verbose for normal operation:

```javascript
/**
 * Debug information (only when --debug flag is active).
 * This field captures data that was previously logged to stderr via
 * console.error in debug blocks. The stderr debug output remains
 * unchanged — this field is an additional structured copy in the log file.
 *
 * @typedef {Object} DebugInfo
 * @property {Object} [googleRequest]     - Converted Google-format request
 */
```

### 2.4 Core Logic

#### 2.4.1 logger.js Changes

Three changes: (1) switch from append mode (`'a'`) to write mode (`'w'`) to clear the file on startup; (2) accept optional `error`, `response`, and `debug` fields in `log()` for enhanced entries; (3) keep `console.error` for write errors (it is a diagnostic message).

```javascript
// src/logger.js

import fs from 'node:fs';
import path from 'node:path';

/**
 * Writes structured request logs in NDJSON format to a file.
 *
 * Uses fs.createWriteStream in write mode:
 * - File is truncated (cleared) when the stream is opened, ensuring each
 *   Odin session starts with a clean log
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
            flags: 'w',          // Write mode — clear file on startup
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
     * @param {Object} [params.error] - Error details (Cloud Code or internal)
     * @param {Object} [params.response] - Response SSE events for /v1/messages (when --debug is active)
     * @param {Object} [params.debug] - Debug information (when --debug is active)
     */
    log({ req, body, statusCode, startTime, error, response, debug }) {
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

        if (error) {
            entry.error = error;
        }

        if (response) {
            entry.response = response;
        }

        if (debug) {
            entry.debug = debug;
        }

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

**Key change: `flags: 'w'` vs. `flags: 'a'`**

The previous implementation used append mode (`'a'`), which preserved logs across restarts. This change uses write mode (`'w'`), which truncates the file when opened. Rationale:

- Odin is a development proxy, not a production service. Logs from a previous session are rarely useful because the API key may have changed (tokens expire), the conversation context is lost, and stale log entries create confusion when debugging the current session.
- Developers who need to preserve logs across sessions can copy the file before restarting, or use an external tool to tail and archive the stream.

#### 2.4.2 server.js Changes

Remove the `logRequest()` and `logResponse()` functions — their purpose (per-request stderr summary lines) is fully superseded by the always-on file logger. All other `console.error` calls (debug traces, error diagnostics) remain unchanged. Add `error` field for Cloud Code/internal failures, and `response`/`debug` fields for `/v1/messages` when `--debug` is active.

```javascript
// server.js changes (abridged — showing only modified sections)

import http from 'node:http';

import { anthropicToGoogle, streamSSEResponse } from './converter.js';
import { sendRequest } from './cloudcode.js';

// ─── REMOVED ────────────────────────────────────────────────────────────────
// logRequest()  — removed (per-request stderr summary superseded by file logger)
// logResponse() — removed (per-request stderr summary superseded by file logger)

// ─── Body Parsing ───────────────────────────────────────────────────────────

// readBody() — unchanged

// ─── Response Helpers ───────────────────────────────────────────────────────

// sendJSON() — unchanged
// sendError() — unchanged

// ─── Server Factory ─────────────────────────────────────────────────────────

/**
 * Create and return the Odin HTTP server.
 *
 * @param {Object} options
 * @param {string} options.apiKey - Cloud Code API key
 * @param {boolean} options.debug - Enable debug logging
 * @param {import('./logger.js').RequestLogger} options.logger - Request log writer (required)
 * @returns {http.Server}
 */
export function createServer({ apiKey, debug, logger }) {
    const server = http.createServer(async (req, res) => {
        const body = await readBody(req);
        const startTime = Date.now();

        const { method, url } = req;
        const path = url.split('?')[0];

        // ── /health ──────────────────────────────────────────────────
        if (method === 'GET' && path === '/health') {
            sendJSON(res, 200, { status: 'ok' });
            logger.log({ req, body, statusCode: 200, startTime });
            return;
        }

        // ── /v1/messages/count_tokens ────────────────────────────────
        if (method === 'POST' && path === '/v1/messages/count_tokens') {
            sendError(res, 501, 'not_implemented_error',
                'The /v1/messages/count_tokens endpoint is not implemented.');
            logger.log({ req, body, statusCode: 501, startTime });
            return;
        }

        // ── /v1/messages (main endpoint) ─────────────────────────────
        if (method === 'POST' && path === '/v1/messages') {
            if (!body) {
                sendError(res, 400, 'invalid_request_error', 'Request body is required');
                logger.log({ req, body, statusCode: 400, startTime });
                return;
            }

            if (!body.stream) {
                sendError(res, 400, 'invalid_request_error',
                    'Only streaming mode is supported. Set "stream": true in your request.');
                logger.log({ req, body, statusCode: 400, startTime });
                return;
            }

            try {
                const model = body.model || 'claude-sonnet-4-5-thinking';
                const googleRequest = anthropicToGoogle(body);

                // Capture debug info for log file (stderr debug output is unchanged)
                const debugInfo = debug ? { googleRequest } : undefined;

                if (debug) {
                    console.error(`[Odin:debug] Converted Google request:`,
                        JSON.stringify(googleRequest, null, 2));
                }

                const cloudResponse = await sendRequest(googleRequest, model, apiKey, debug);

                // ── Handle upstream error responses ──────────────────
                if (!cloudResponse.ok) {
                    const errorBody = await cloudResponse.text();
                    let errorMessage = `Cloud Code API error: ${cloudResponse.status}`;
                    let errorType = 'api_error';
                    let statusCode = 500;

                    try {
                        const errorJson = JSON.parse(errorBody);
                        errorMessage = errorJson.error?.message || errorJson.message || errorBody;
                    } catch {
                        errorMessage = errorBody || errorMessage;
                    }

                    if (cloudResponse.status === 401 || cloudResponse.status === 403) {
                        errorType = 'authentication_error';
                        statusCode = 401;
                    } else if (cloudResponse.status === 429) {
                        errorType = 'rate_limit_error';
                        statusCode = 429;
                    } else if (cloudResponse.status === 400) {
                        errorType = 'invalid_request_error';
                        statusCode = 400;
                    }

                    if (debug) {
                        console.error(`[Odin:debug] Cloud Code error (${cloudResponse.status}):`,
                            errorBody);
                    }

                    sendError(res, statusCode, errorType, errorMessage);

                    // Log with Cloud Code error details ← NEW
                    logger.log({
                        req, body, statusCode, startTime,
                        error: {
                            source: 'cloud_code',
                            upstreamStatus: cloudResponse.status,
                            message: errorMessage,
                            rawBody: errorBody
                        },
                        debug: debugInfo
                    });
                    return;
                }

                // ── Stream SSE response ─────────────────────────────
                res.writeHead(200, {
                    'Content-Type': 'text/event-stream',
                    'Cache-Control': 'no-cache',
                    'Connection': 'keep-alive'
                });

                const events = debug ? [] : undefined;
                for await (const sseEvent of streamSSEResponse(cloudResponse.body, model, debug)) {
                    res.write(sseEvent);
                    if (events) {
                        // Parse "event: xxx\ndata: {...}\n\n" into structured form
                        const lines = sseEvent.trim().split('\n');
                        const eventName = lines[0]?.slice(7);  // strip "event: "
                        const eventData = lines[1]
                            ? JSON.parse(lines[1].slice(6))    // strip "data: "
                            : null;
                        events.push({ event: eventName, data: eventData });
                    }
                }

                res.end();
                logger.log({
                    req, body, statusCode: 200, startTime,
                    response: events ? { events } : undefined,
                    debug: debugInfo
                });

            } catch (err) {
                console.error(`[Odin] Error processing /v1/messages:`, err.message);
                if (debug) {
                    console.error(`[Odin:debug] Full error:`, err);
                }

                if (!res.headersSent) {
                    sendError(res, 500, 'api_error', `Internal proxy error: ${err.message}`);
                    logger.log({
                        req, body, statusCode: 500, startTime,
                        error: {
                            source: 'internal',
                            message: err.message,
                            stack: err.stack
                        }
                    });
                } else {
                    res.end();
                    logger.log({
                        req, body, statusCode: 200, startTime,
                        error: {
                            source: 'internal',
                            message: err.message,
                            stack: err.stack
                        }
                    });
                }
            }
            return;
        }

        // ── / (heartbeat) ───────────────────────────────────────────
        if (method === 'POST' && path === '/') {
            sendJSON(res, 200, {});
            logger.log({ req, body, statusCode: 200, startTime });
            return;
        }

        // ── /api/event_logging/batch ────────────────────────────────
        if (method === 'POST' && path === '/api/event_logging/batch') {
            sendJSON(res, 200, {});
            logger.log({ req, body, statusCode: 200, startTime });
            return;
        }

        // ── Unknown Endpoint (404) ──────────────────────────────────
        if (debug) {
            console.error(`[Odin:debug] Unknown endpoint hit: ${method} ${path}`);
            console.error(`[Odin:debug] Headers:`, JSON.stringify(req.headers, null, 2));
            if (body) {
                console.error(`[Odin:debug] Body:`, JSON.stringify(body, null, 2));
            }
        }

        sendError(res, 404, 'not_found_error', `Unknown endpoint: ${method} ${path}`);
        logger.log({ req, body, statusCode: 404, startTime });
    });

    return server;
}
```

**Key changes**:

| Before | After |
|--------|-------|
| `logRequest()` called on every request → stderr | Removed (superseded by file logger) |
| `logResponse()` called on every response → stderr | Removed (superseded by file logger) |
| `console.error` for debug blocks | **Unchanged** — diagnostic output stays on stderr |
| `console.error` for error messages | **Unchanged** — diagnostic output stays on stderr |
| Cloud Code error details | **Added** to `logger.log()` via `error` field |
| `/v1/messages` response SSE events | **Added** to `logger.log()` via `response` field (when `debug` is true) |
| `logger?.log()` (optional chaining) | `logger.log()` (logger is always present) |

#### 2.4.3 index.js Changes

Make file logging always-on with a sensible default path. Keep all `console.error` calls (they are diagnostic messages).

```javascript
// src/index.js

#!/usr/bin/env node

import { createServer } from './server.js';
import { RequestLogger } from './logger.js';

// ─── CLI Argument Parsing ───────────────────────────────────────────────────

function parseArgs(argv) {
    // ... unchanged ...
}

function printUsage() {
    console.error(`Usage: node src/index.js --api-key=<key> [--port=<port>] [--debug] [--log-file=<path>]

Options:
  --api-key=<key>       API key for Antigravity Cloud Code (required)
  --port=<port>         Port to listen on (default: 8080)
  --debug               Enable debug logging (also records response SSE events in log file)
  --log-file=<path>     Custom log file path (default: logs/requests.ndjson)

Examples:
  node src/index.js --api-key="ya29.a0AeO..."
  node src/index.js --api-key="ya29..." --port=3000 --debug
  node src/index.js --api-key="ya29..." --log-file=/tmp/odin.ndjson`);
}

// ─── Main ───────────────────────────────────────────────────────────────────

const args = parseArgs(process.argv);

if (!args['api-key']) {
    console.error('[Odin] Error: --api-key is required\n');
    printUsage();
    process.exit(1);
}

const apiKey = args['api-key'];
const port = parseInt(args['port'], 10) || 8080;
const debug = args['debug'] === true;
const logFile = args['log-file'] || 'logs/requests.ndjson';  // ← Always has a value

// Logger is always created (no longer opt-in)
const logger = new RequestLogger(logFile);

const server = createServer({ apiKey, debug, logger });

server.listen(port, () => {
    console.error(`[Odin] Server listening on http://localhost:${port}`);
    console.error(`[Odin] Debug mode: ${debug ? 'ON' : 'OFF'}`);
    console.error(`[Odin] Log file: ${logFile}`);
    console.error(`[Odin] Press Ctrl+C to stop`);
});

process.on('SIGINT', async () => {
    console.error('\n[Odin] Shutting down...');
    await logger.close();
    server.close(() => {
        process.exit(0);
    });
});

process.on('SIGTERM', async () => {
    console.error('[Odin] Received SIGTERM, shutting down...');
    await logger.close();
    server.close(() => {
        process.exit(0);
    });
});
```

**Key changes**:

| Before | After |
|--------|-------|
| `const logFile = args['log-file'] \|\| null;` | `const logFile = args['log-file'] \|\| 'logs/requests.ndjson';` |
| `const logger = logFile ? new RequestLogger(logFile) : null;` | `const logger = new RequestLogger(logFile);` |
| `await logger?.close()` (optional chaining) | `await logger.close()` |
| All `console.error(...)` | **Unchanged** (diagnostic output) |

#### 2.4.4 cloudcode.js Changes

**No changes.** The `logUpstream()` function and `debug` parameter remain as-is — they produce diagnostic stderr output which is standard practice and not being deprecated.

### 2.5 Flow Diagram

#### 2.5.1 Successful Request Flow

```
Claude Code CLI                 Odin server.js                     logger.js
      │                              │                                 │
      │  HTTP Request                │                                 │
      │─────────────────────────────▶│                                 │
      │                              │                                 │
      │                              │  readBody(req)                  │
      │                              │  startTime = Date.now()         │
      │                              │                                 │
      │                              │  ... route handling ...         │
      │                              │  sendJSON / SSE                 │
      │◀─────────────────────────────│                                 │
      │  HTTP Response               │                                 │
      │                              │                                 │
      │                              │  logger.log({req, body,         │
      │                              │    statusCode, startTime})      │
      │                              │────────────────────────────────▶│
      │                              │                                 │  → file
      │                              │                                 │
      │                              │  (stderr: debug traces          │
      │                              │   if --debug is on)             │
```

#### 2.5.2 Cloud Code Error Flow

```
Claude Code CLI         Odin server.js          Cloud Code API      logger.js
      │                      │                        │                 │
      │  POST /v1/messages   │                        │                 │
      │─────────────────────▶│                        │                 │
      │                      │  sendRequest()         │                 │
      │                      │───────────────────────▶│                 │
      │                      │                        │                 │
      │                      │◀───────────────────────│                 │
      │                      │  HTTP 429 + error body │                 │
      │                      │                        │                 │
      │                      │  console.error(...)    │                 │
      │                      │  ──────────▶ stderr    │                 │
      │                      │  (diagnostic, if debug)│                 │
      │                      │                        │                 │
      │◀─────────────────────│                        │                 │
      │  429 rate_limit_error│                        │                 │
      │                      │                        │                 │
      │                      │  logger.log({          │                 │
      │                      │    ...,                │                 │
      │                      │    error: {            │                 │
      │                      │      source: 'cloud_code',               │
      │                      │      upstreamStatus: 429,                │
      │                      │      message: '...',   │                 │
      │                      │      rawBody: '...'    │                 │
      │                      │    },                  │                 │
      │                      │    debug?              │                 │
      │                      │  })                    │                 │
      │                      │─────────────────────────────────────────▶│
      │                      │                        │                 │  → file
```

---

## 3. Interface Design

### 3.1 CLI Interface

```
Usage: node src/index.js --api-key=<key> [--port=<port>] [--debug] [--log-file=<path>]

Options:
  --api-key=<key>       API key for Antigravity Cloud Code (required)
  --port=<port>         Port to listen on (default: 8080)
  --debug               Enable debug logging (also records response SSE events in log file)
  --log-file=<path>     Custom log file path (default: logs/requests.ndjson)

Examples:
  # Standard usage — logs to logs/requests.ndjson automatically
  node src/index.js --api-key="ya29.a0AeO..."

  # Custom log file location
  node src/index.js --api-key="ya29..." --log-file=/tmp/odin-debug.ndjson

  # Full diagnostic mode — stderr traces + debug/response fields in log file
  node src/index.js --api-key="ya29..." --debug
```

**Changes from previous version**:

| Flag | Before | After |
|------|--------|-------|
| `--log-file` | Enables file logging (opt-in) | Overrides default file path (file logging is always on) |
| `--debug` | stderr traces only | stderr traces + `debug` field (googleRequest) and `response` field (SSE events) in log file for `/v1/messages` |

### 3.2 Querying the Log File

#### Monitoring Cloud Code errors (new capability)

```bash
# Live-monitor all Cloud Code upstream errors
tail -f logs/requests.ndjson | jq 'select(.error.source == "cloud_code")'

# Live-monitor rate limiting specifically
tail -f logs/requests.ndjson | jq 'select(.error.upstreamStatus == 429)'

# Live-monitor all errors (both Cloud Code and internal)
tail -f logs/requests.ndjson | jq 'select(.error != null)'

# Show Cloud Code error messages with timestamps
tail -f logs/requests.ndjson | jq 'select(.error != null) | {timestamp, statusCode, error}'

# Show upstream status distribution for errors
jq 'select(.error.source == "cloud_code") | .error.upstreamStatus' logs/requests.ndjson | sort | uniq -c | sort -rn
```

#### Inspecting response data (requires `--debug`)

```bash
# Show SSE event count per /v1/messages request
tail -f logs/requests.ndjson | jq 'select(.response.events != null) | {timestamp, eventCount: (.response.events | length)}'

# Show the final message_delta event (contains stop_reason and usage)
tail -f logs/requests.ndjson | jq 'select(.response.events != null) | .response.events[] | select(.event == "message_delta")'

# Compare request and response for /v1/messages
tail -f logs/requests.ndjson | jq 'select(.response != null) | {model: .body.model, turns: (.body.messages | length), stopReason: (.response.events[] | select(.event == "message_delta") | .data.delta.stop_reason)}'

# Show converted Google request alongside response (full round-trip)
tail -f logs/requests.ndjson | jq 'select(.debug != null) | {timestamp, googleRequest: .debug.googleRequest, eventCount: (.response.events | length)}'
```

#### General request monitoring (unchanged)

```bash
# Watch all requests in real time
tail -f logs/requests.ndjson | jq .

# Live-monitor /v1/messages requests
tail -f logs/requests.ndjson | jq 'select(.path == "/v1/messages") | {timestamp, statusCode, durationMs}'

# Count requests per path
jq -r '.path' logs/requests.ndjson | sort | uniq -c | sort -rn

# Average response time for /v1/messages
jq '[select(.path == "/v1/messages") | .durationMs] | add / length' logs/requests.ndjson
```

### 3.3 Error Handling

| Condition | Behavior |
|-----------|----------|
| Default startup (no `--log-file`) | Logger writes to `logs/requests.ndjson`; directory created if missing |
| `--log-file` specified | Logger writes to the custom path; directory created if missing |
| Log file already exists on startup | File is truncated (cleared) when the write stream opens |
| Log file write failure (e.g., disk full) | Error printed to stderr (diagnostic); request handling is not affected |
| Cloud Code returns non-2xx | Error details (status, message, raw body) recorded in `error` field |
| Network failure during Cloud Code request | Error details (message, stack) recorded in `error` field |
| `--debug` with large SSE stream | All SSE events for `/v1/messages` buffered in memory then written; acceptable for dev proxy |
| Ungraceful termination (SIGKILL) | Last few log entries in the write stream buffer may be lost (acceptable) |

---

## 4. Implementation Plan

### Phase 1: logger.js Enhancements (Estimated: 0.5 hours)

**Task 1.1: Switch to write mode**
- [x] Change `flags: 'a'` to `flags: 'w'` in `fs.createWriteStream` options

**Task 1.2: Extend `log()` method signature**
- [x] Add optional `error` parameter to `log()` method
- [x] Add optional `response` parameter to `log()` method
- [x] Add optional `debug` parameter to `log()` method
- [x] Include `error`, `response`, and `debug` fields in JSON output when present

**Acceptance Criteria:**
- Log file is cleared on each `RequestLogger` instantiation
- `log()` correctly serializes entries with and without optional fields
- Optional fields are omitted (not `null`) when not provided

### Phase 2: server.js Overhaul (Estimated: 2 hours)

**Task 2.1: Remove per-request stderr summary functions**
- [x] Remove `logRequest()` function entirely
- [x] Remove `logResponse()` function entirely
- [x] Replace `const startTime = logRequest(...)` with `const startTime = Date.now()`

**Task 2.2: Add error information to logger calls**
- [x] Cloud Code upstream error (around line 209): pass `error: { source: 'cloud_code', upstreamStatus, message, rawBody }`
- [x] Catch-block error, headers not sent (around line 239): pass `error: { source: 'internal', message, stack }`
- [x] Catch-block error, headers sent (around line 244): pass `error: { source: 'internal', message, stack }`

**Task 2.3: Add response capture for `/v1/messages`**
- [x] In the `/v1/messages` streaming handler: when `debug` is true, buffer SSE events by parsing `"event: ...\ndata: ...\n\n"` strings into `{ event, data }` objects
- [x] Pass buffered events as `response: { events }` to `logger.log()` when `debug` is true
- [x] Non-`/v1/messages` endpoints: no response capture (trivial responses)

**Task 2.4: Add debug information to logger calls**
- [x] Capture `googleRequest` before `sendRequest()` call when `debug` is true
- [x] Pass `debug: { googleRequest }` to `logger.log()` calls within the `/v1/messages` handler

**Task 2.5: Make logger required**
- [x] Change `logger?.log()` to `logger.log()` at all call sites (10 locations)
- [x] Update `createServer()` JSDoc: `logger` parameter is no longer optional

**Acceptance Criteria:**
- `logRequest()` and `logResponse()` are removed; no per-request stderr output
- All diagnostic `console.error` calls remain unchanged
- Cloud Code errors produce log entries with `error.source === 'cloud_code'`
- Internal errors produce log entries with `error.source === 'internal'`
- With `--debug`: `/v1/messages` log entries have `response.events` and `debug.googleRequest`
- Without `--debug`: no `response` or `debug` fields in log entries

### Phase 3: index.js Changes (Estimated: 0.5 hours)

**Task 3.1: Default log file and always-on logger**
- [x] Change `const logFile = args['log-file'] || null` to `args['log-file'] || 'logs/requests.ndjson'`
- [x] Change `const logger = logFile ? new RequestLogger(logFile) : null` to `new RequestLogger(logFile)`
- [x] Remove the conditional `if (logger)` around log file path display (always print)
- [x] Change `await logger?.close()` to `await logger.close()` in both signal handlers

**Task 3.2: Update `printUsage()` help text**
- [x] Update `--debug` description to mention response SSE event recording
- [x] Update `--log-file` description to reflect default path

**Acceptance Criteria:**
- Odin starts with file logging active even without `--log-file` flag
- All `console.error` calls remain (diagnostic messages stay on stderr)

### Phase 4: Validation (Estimated: 0.5 hours)

**Task 4.1: End-to-end testing**
- [x] Start Odin without `--log-file` → verify `logs/requests.ndjson` is created and cleared
- [x] Start Odin with `--log-file=/tmp/test.ndjson` → verify custom path is used
- [x] Restart Odin → verify log file is cleared (previous entries gone)
- [x] Trigger a Cloud Code error (e.g., expired API key) → verify `error` field in log entry
- [x] Run a full Claude Code conversation → verify all request types are logged
- [x] Start with `--debug` → verify `response` and `debug` fields present in `/v1/messages` log entries
- [x] Start without `--debug` → verify no `response` or `debug` fields in log entries
- [x] Verify diagnostic stderr output still works (startup messages, debug traces)
- [x] Run `tail -f logs/requests.ndjson | jq 'select(.error != null)'` to verify error filtering

---

## 5. Testing

### 5.1 Test Cases

| Test Scenario | Input | Expected Output |
|---------------|-------|-----------------|
| Default log file | Start without `--log-file` | `logs/requests.ndjson` created, entries written |
| Custom log file | `--log-file=/tmp/test.ndjson` | File at custom path, entries written |
| Clear on startup | Restart Odin | Previous log entries are gone; file starts empty |
| Cloud Code 429 error | Expired/rate-limited API key | Log entry with `error.source: "cloud_code"`, `error.upstreamStatus: 429` |
| Cloud Code 401 error | Invalid API key | Log entry with `error.source: "cloud_code"`, `error.upstreamStatus: 401` |
| Network failure | Unreachable Cloud Code endpoint | Log entry with `error.source: "internal"`, `error.message` containing network error |
| Successful request | Normal `/v1/messages` | Log entry without `error` field |
| Debug off | No `--debug` | No `response` or `debug` fields in any log entry |
| Debug on: /v1/messages | `--debug`, POST `/v1/messages` | `response.events` array + `debug.googleRequest` in log entry |
| Debug on: other endpoints | `--debug`, GET `/health` | No `response` or `debug` fields (only /v1/messages gets them) |
| Diagnostic stderr | Start with `--debug`, send request | Debug traces appear on stderr as before |
| No request summary stderr | Any request | No `[Odin] <timestamp> <method>...` lines on stderr |
| SIGINT shutdown | Ctrl+C | Log file is complete; buffer is flushed |

### 5.2 Manual Testing Steps

```bash
# 1. Start Odin (note: no --log-file needed)
node src/index.js --api-key="$(cat ~/.odin-key)" --port=8080

# 2. Verify startup messages still appear on stderr
#    Expected output (stderr):
#    [Odin] Server listening on http://localhost:8080
#    [Odin] Debug mode: OFF
#    [Odin] Log file: logs/requests.ndjson
#    [Odin] Press Ctrl+C to stop

# 3. In a second terminal, monitor the log file
tail -f logs/requests.ndjson | jq .

# 4. In a third terminal, start Claude Code
export ANTHROPIC_BASE_URL=http://localhost:8080
export ANTHROPIC_API_KEY=dummy
export ANTHROPIC_MODEL=claude-sonnet-4-5-thinking
claude

# 5. Interact with Claude Code and observe log output
# 6. Verify error entries (if any) contain the error field:
jq 'select(.error != null)' logs/requests.ndjson

# 7. Verify per-request summary lines are GONE from stderr
#    (No more "[Odin] 2026-... POST /v1/messages 200 1523ms" lines)
#    But startup/debug messages still appear

# 8. Test debug mode response recording
node src/index.js --api-key="$(cat ~/.odin-key)" --port=8080 --debug
# Interact with Claude Code, then:
jq 'select(.response != null)' logs/requests.ndjson
# Verify /v1/messages entries have response.events array and debug.googleRequest
# Verify other endpoints (health, heartbeat) do NOT have response or debug fields

# 9. Verify log file is cleared on restart
wc -l logs/requests.ndjson  # Note the count
# Restart Odin
node src/index.js --api-key="$(cat ~/.odin-key)" --port=8080
wc -l logs/requests.ndjson  # Should be 0
```

---

## 6. Risk Assessment

### 6.1 Risks and Mitigations

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| Log file cleared accidentally on restart | Medium | Medium | By design — Odin is a dev proxy, not a production service. Document the behavior clearly. Developers who need persistence can archive before restart. |
| No per-request stderr feedback | Low | Low | The file logger is always active. Developers use `tail -f \| jq` for real-time visibility. Diagnostic messages (errors, debug) still go to stderr. |
| Large `rawBody` in error entries | Low | Low | Cloud Code error responses are typically small JSON payloads. No truncation needed for now. |
| Large SSE event arrays with `--debug` | Medium | Low | Only active when `--debug` is on; developers explicitly choose full diagnostic mode. A single long conversation may produce MB-scale entries. Consider adding a truncation limit in the future if needed. |
| Memory pressure during streaming with `--debug` | Low | Low | SSE events are buffered as parsed objects. For typical conversations (~100 events), memory impact is negligible. |

---

## 7. Future Considerations

### 7.1 Potential Extensions

| Feature | Status | Notes |
|---------|--------|-------|
| Opt-in append mode | 💡 Idea | Add `--log-append` flag for users who want to preserve logs across restarts |
| Response size limit | 💡 Idea | Truncate `response.events` beyond a configurable threshold to cap log file size in debug mode |
| Structured startup/shutdown log entries | 💡 Idea | Write `{"type":"startup","timestamp":"...","port":8080}` as the first log entry, enabling fully machine-parseable operational logs |
| Header redaction | 💡 Idea | Automatically mask sensitive header values (`Authorization`, `x-api-key`) in log entries |

---

## 8. Appendix

### 8.1 References

1. `odin/src/server.js` — Current request handling and logging logic
2. `odin/src/index.js` — Current CLI argument parsing and server startup
3. `odin/src/logger.js` — Current RequestLogger implementation (append mode)
4. `odin/src/cloudcode.js` — Cloud Code client with diagnostic stderr logging

### 8.2 Related Documents

| Document | Description |
|----------|-------------|
| `odin/docs/odin_basic_design.md` | Odin base architecture; §2.7 describes the existing per-request stderr logging being removed |
| `odin/docs/odin_request_log_file.md` | Original request log file design; this document supersedes its logging architecture while preserving the NDJSON format and `jq` query patterns |

### 8.3 Migration Summary

For existing Odin users, the behavioral changes are:

| What you did before | What you do now |
|---------------------|-----------------|
| `node src/index.js --api-key=... --log-file=logs/requests.ndjson` | `node src/index.js --api-key=...` (log file is automatic) |
| Watch `[Odin] ... POST /v1/messages 200 1523ms` on stderr | `tail -f logs/requests.ndjson \| jq .` |
| Rely on stderr for Cloud Code errors | `jq 'select(.error != null)' logs/requests.ndjson` |
| No response inspection capability | `--debug` + `jq 'select(.response != null)' logs/requests.ndjson` |

**What does NOT change:**
- Startup/shutdown messages remain on stderr
- `--debug` traces remain on stderr
- Operational error messages remain on stderr

---

## Changelog

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.3 | 2026-02-10 | Chason Tang | All phases implemented and validated. Status updated to Implemented. |
| 1.2 | 2026-02-10 | Chason Tang | Replace `--log-response` with `--debug`: response SSE events for `/v1/messages` are now recorded under the existing `--debug` flag instead of a separate flag. Remove response capture from non-`/v1/messages` endpoints (trivial responses). |
| 1.1 | 2026-02-10 | Chason Tang | Scope stderr deprecation to request summary lines only; keep diagnostic stderr unchanged. Add `--log-response` opt-in flag for response body recording. Keep `cloudcode.js` unchanged. |
| 1.0 | 2026-02-10 | Chason Tang | Initial version |

---

*End of Technical Design Document*
