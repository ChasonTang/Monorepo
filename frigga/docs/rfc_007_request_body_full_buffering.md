# RFC-007: Request Body Full Buffering

**Version:** 1.0
**Author:** Chason Tang
**Date:** 2026-04-13
**Status:** Proposed

---

## 1. Summary

Replace frigga's request body forwarding strategy from streaming (`req.pipe(upstreamReq)`) to full buffering: read the entire client request body into an in-memory `Buffer`, then write it to the upstream request atomically via `upstreamReq.end(requestBody)`. This change lays the groundwork for future request body modifications (JSON field injection/removal, credential embedding, etc.) while simplifying audit log capture and error handling.

## 2. Motivation

Frigga currently pipes request bodies directly to the upstream via `req.pipe(upstreamReq)`. Data is forwarded the instant it arrives — there is no opportunity to inspect or modify the body before it reaches the upstream server.

Upcoming work requires the ability to modify request body content (e.g., injecting or replacing JSON fields, embedding credentials). The streaming model fundamentally cannot support this — data is written to the upstream as it is read from the client, leaving no complete body available for manipulation.

Switching to full buffering provides:

- **Request body modification capability** — with the complete `Buffer` in hand, any JSON parsing, field injection/removal, or content replacement becomes straightforward
- **Simpler audit log capture** — the current design uses a separate sidecar `data` listener to capture the request body for logging; with full buffering the body is naturally available without an extra listener
- **Simpler error handling** — eliminates the write-after-destroy race condition inherent to `pipe`, along with the `unpipe` cleanup logic

## 3. Goals and Non-Goals

**Goals:**
- Buffer the complete request body into an in-memory `Buffer` before forwarding to upstream
- Replace `req.pipe(upstreamReq)` with `upstreamReq.end(requestBody)` for atomic body writing
- Unify body capture — use the buffered result directly for logging instead of a separate sidecar listener
- Simplify helper functions (`abortUpstream`, `createRequestLogEmitter`)
- All existing tests pass with no functional regression

**Non-Goals:**
- This RFC does not implement any concrete request body modification logic — it only establishes the buffering infrastructure; specific modifications are deferred to future RFCs
- No request body size limits — the upstream API enforces its own limits, and frigga remains transparent
- No changes to response body handling — responses continue to stream via `stream.pipeline()`

## 4. Design

### 4.1 Overview

The change converts the request forwarding flow from a single-stage streaming pipe to a two-phase model:

```
Before:
  Client ──pipe──> Upstream    (data flows through immediately, cannot be modified)

After:
  Client ──buffer──> [Buffer]  (Phase 1: read entire body into memory)
                       |
                       v
                   Upstream    (Phase 2: write body atomically)
```

Components in `server.js` affected by this change:

| Component | Change Type |
|-----------|-------------|
| `REQUEST_HEADER_FORWARD` | **Updated** — remove `content-length` from whitelist |
| `bufferRequestBody()` | **New** — request body buffering helper |
| `abortUpstream()` | **Simplified** — remove `req` parameter and `unpipe` call |
| `createRequestLogEmitter()` | **Simplified** — `requestChunks` -> `requestBody`, eliminate deferred finalization |
| `startServer()` forwarding path | **Restructured** — async callback + two-phase model |

### 4.2 Detailed Design

#### 4.2.1 `REQUEST_HEADER_FORWARD` — Remove `content-length`

Under the streaming model, frigga was a transparent pipe — the client's `content-length` accurately described the body flowing through to upstream, so forwarding it was correct. With full buffering, the client-to-frigga transfer and the frigga-to-upstream transfer are independent hops. Future body modifications will change the body size, making the client's `content-length` incorrect for the upstream request. Node.js automatically sets `Content-Length` (or uses chunked transfer encoding) based on the data passed to `end()`, so explicit forwarding is unnecessary.

```javascript
// After — content-length removed
const REQUEST_HEADER_FORWARD = new Set([
  "content-type",
  "accept",
  "accept-encoding",
  "user-agent",
  "anthropic-beta",
  "anthropic-version",
  "authorization",
]);
```

The remaining 7 headers were reviewed and are all end-to-end in nature — they describe the client's identity, desired API behavior, or response format negotiation, none of which are affected by request body buffering:

| Header | Semantics | Affected by buffering? |
|--------|-----------|----------------------|
| `content-type` | Body media type (`application/json`). Body modifications do not change the media type. | No |
| `accept` | Desired response format. Unrelated to request body. | No |
| `accept-encoding` | Supported response compression. frigga streams responses via `pipeline` without decompressing. | No |
| `user-agent` | Client identification. | No |
| `anthropic-beta` | API feature flags. | No |
| `anthropic-version` | API version negotiation. | No |
| `authorization` | Client's API key. | No |

#### 4.2.2 `bufferRequestBody` — Request Body Buffering Helper

A new exported function added to `server.js`, placed after `abortUpstream`:

```javascript
/**
 * Collect the entire request body into a single Buffer.
 * Resolves with the concatenated body; rejects on stream error.
 * @param {import("node:http").IncomingMessage} req
 * @returns {Promise<Buffer>}
 */
export function bufferRequestBody(req) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    req.on("data", (chunk) => chunks.push(chunk));
    req.on("end", () => resolve(Buffer.concat(chunks)));
    req.on("error", (err) => {
      chunks.length = 0;
      reject(err);
    });
  });
}
```

Design notes:

- Returns a `Buffer` (not a `string`) to preserve binary fidelity and enable downstream byte-level operations
- Registering the `data` listener switches `req` into flowing mode and begins buffering
- Stream errors (e.g., `ECONNRESET` from client disconnect) propagate through Promise rejection
- A request with no body data (immediate `end`) resolves to `Buffer.alloc(0)`

#### 4.2.3 `abortUpstream` — Simplified

Current implementation:

```javascript
// Current (server.js:70-75)
export function abortUpstream(req, upstreamReq) {
  if (upstreamReq && !upstreamReq.destroyed) {
    req.unpipe(upstreamReq);    // disconnect pipe first to prevent write-after-destroy
    upstreamReq.destroy();
  }
}
```

The `unpipe` call exists solely to prevent the write-after-destroy race condition that arises when `pipe` continues writing after `destroy` is called (RFC-004 section 4.3). With full buffering there is no pipe, so `unpipe` is unnecessary and the `req` parameter can be removed:

```javascript
// After
/**
 * Abort the upstream request.
 * @param {import("node:http").ClientRequest} upstreamReq
 */
export function abortUpstream(upstreamReq) {
  if (upstreamReq && !upstreamReq.destroyed) {
    upstreamReq.destroy();
  }
}
```

#### 4.2.4 `createRequestLogEmitter` — Parameter and Logic Simplification

**Parameter change**: `requestChunks: Buffer[]` -> `requestBody: Buffer | undefined`

The current implementation contains deferred finalization logic: when `requestChunks` is present but the request stream has not yet ended, it registers `end`/`error` listeners to wait for stream completion before emitting the log entry, using a `finalized` guard to prevent duplicate output when both `end` and `error` fire. With full buffering, the body is fully available when `createRequestLogEmitter` is called, so deferred finalization can be eliminated entirely.

```javascript
// After
/**
 * Create a request log emitter with exactly-once guard.
 * @param {object} ctx
 * @param {import("node:http").IncomingMessage} ctx.req
 * @param {Buffer} [ctx.requestBody] - buffered request body (omit to skip request_body)
 * @param {number} ctx.startTime - Date.now() at request start
 * @returns {(status: number, responseHeaders?: Record<string, string>) => void}
 */
export function createRequestLogEmitter({ req, requestBody, startTime }) {
  let emitted = false;

  return function emitRequestLog(status, responseHeaders) {
    if (emitted) return;
    emitted = true;

    const entry = {
      timestamp: new Date().toISOString(),
      level: "INFO",
      event: "request",
      method: req.method,
      url: req.url,
      status,
      duration_ms: Date.now() - startTime,
      request_headers: req.headers,
    };
    if (requestBody !== undefined) {
      entry.request_body = requestBody.toString("utf-8");
    }
    if (responseHeaders) entry.response_headers = responseHeaders;
    process.stdout.write(`${JSON.stringify(entry)}\n`);

    // Drain the request stream if still active (local error path only;
    // on the forwarding path the stream is already consumed by bufferRequestBody).
    if (!req.readableEnded && !req.destroyed) {
      req.on("error", () => {});
      req.resume();
    }
  };
}
```

Complexity removed:
- `finalized` inner guard and `finalize` closure
- `req.on("end", finalize)` and `req.on("error", () => finalize())` deferred finalization registration
- `requestChunks.length = 0` array cleanup
- `Buffer.concat(requestChunks)` dynamic concatenation — `requestBody` is already a complete `Buffer`

Logic retained:
- `emitted` outer guard — prevents duplicate log entries (exactly-once semantics unchanged)
- `req.resume()` + no-op error listener — the local error path still needs to drain the request stream to maintain HTTP keep-alive (per RFC-005 section 4.3)

#### 4.2.5 Forwarding Path Restructuring

The `http.createServer` callback becomes `async` to support the two-phase model:

```javascript
const server = http.createServer(async (req, res) => {
  const startTime = Date.now();

  const result = handleRequest(
    { method: req.method, url: req.url },
    isShuttingDown,
  );

  // -- Local error path (unchanged) -------------------------
  if (result !== null) {
    const { statusCode, headers, body } = result;
    res.writeHead(statusCode, headers);
    res.end(body);
    const emitRequestLog = createRequestLogEmitter({ req, startTime });
    emitRequestLog(statusCode);
    return;
  }

  // -- Forwarding path --------------------------------------

  // Phase 1: Buffer the request body
  let requestBody;
  try {
    requestBody = await bufferRequestBody(req);
  } catch (err) {
    // Client disconnected or stream error during buffering.
    // req and res share the same TCP socket (HTTP/1.1), so the socket
    // is almost certainly dead. Destroy res explicitly for cleanup.
    if (!res.destroyed) {
      res.destroy();
    }
    process.stderr.write(
      `${JSON.stringify({
        timestamp: new Date().toISOString(),
        level: "ERROR",
        event: "client_error",
        method: req.method,
        url: req.url,
        message: err.code || err.message,
        duration_ms: Date.now() - startTime,
      })}\n`,
    );
    const emitRequestLog = createRequestLogEmitter({
      req,
      requestBody: logBody ? Buffer.alloc(0) : undefined,
      startTime,
    });
    emitRequestLog(499);
    return;
  }

  // Phase 2: Forward to upstream
  let upstreamReq;
  let upstreamResponseReceived = false;

  const emitRequestLog = createRequestLogEmitter({
    req,
    requestBody: logBody ? requestBody : undefined,
    startTime,
  });

  // Client disconnect detection (post-buffering, pre/during upstream response)
  res.on("close", () => {
    if (!res.writableFinished) {
      abortUpstream(upstreamReq);
      if (!upstreamResponseReceived) {
        process.stderr.write(
          `${JSON.stringify({
            timestamp: new Date().toISOString(),
            level: "WARN",
            event: "client_disconnect",
            method: req.method,
            url: req.url,
            duration_ms: Date.now() - startTime,
          })}\n`,
        );
        emitRequestLog(499);
      }
    }
  });

  // Build upstream request
  const upstreamUrl = new URL(req.url, UPSTREAM_BASE_URL);
  const upstreamHeaders = buildUpstreamHeaders(req.headers);

  upstreamReq = https.request(
    upstreamUrl,
    { method: "POST", headers: upstreamHeaders },
    (upstreamRes) => {
      upstreamResponseReceived = true;

      const responseHeaders = filterResponseHeaders(upstreamRes.headers);
      responseHeaders["x-accel-buffering"] = "no";
      if (isShuttingDown) {
        responseHeaders["connection"] = "close";
      }

      res.writeHead(upstreamRes.statusCode, responseHeaders);

      pipeline(upstreamRes, res, (err) => {
        if (err) {
          process.stderr.write(
            `${JSON.stringify({
              timestamp: new Date().toISOString(),
              level: "ERROR",
              event: "upstream_error",
              method: req.method,
              url: req.url,
              message: err.code || err.message,
              duration_ms: Date.now() - startTime,
            })}\n`,
          );
        }
        emitRequestLog(upstreamRes.statusCode, upstreamRes.headers);
      });
    },
  );

  // Upstream connection error
  upstreamReq.on("error", (err) => {
    process.stderr.write(
      `${JSON.stringify({
        timestamp: new Date().toISOString(),
        level: "ERROR",
        event: "upstream_error",
        method: req.method,
        url: req.url,
        message: err.code || err.message,
        duration_ms: Date.now() - startTime,
      })}\n`,
    );

    if (!res.headersSent && !res.destroyed) {
      const errorHeaders = { "content-type": "application/json" };
      if (isShuttingDown) {
        errorHeaders["connection"] = "close";
      }
      res.writeHead(502, errorHeaders);
      res.end(
        JSON.stringify({
          type: "error",
          error: {
            type: "api_error",
            message: `upstream connection failed: ${err.code || err.message}`,
          },
        }),
      );
    }

    emitRequestLog(502);
  });

  upstreamReq.end(requestBody);
});
```

**Key changes explained:**

| Change | Reason |
|--------|--------|
| `async` callback | Required for `await bufferRequestBody(req)` |
| Remove `requestChunks` and sidecar `data` listener | Buffered result is reused directly for logging |
| Remove `req.on("error")` handler | Stream errors are caught by `try/catch`; after `bufferRequestBody` resolves, `req.readableEnded === true` and no further `error` events will fire |
| `abortUpstream(req, upstreamReq)` -> `abortUpstream(upstreamReq)` | No pipe to disconnect |
| `req.pipe(upstreamReq)` -> `upstreamReq.end(requestBody)` | Core change — atomic write replaces streaming pipe |

**`async` callback safety analysis:**

`http.createServer` ignores the return value of its callback, so returning a `Promise` does not affect HTTP behavior. The rejection from `await bufferRequestBody(req)` is caught by `try/catch`, preventing unhandled Promise rejections. All code after the `try/catch` is synchronous (registering event handlers, creating the upstream request) and produces no additional Promises.

**`res.on("close")` registration timing analysis:**

`res.on("close")` is registered after `await bufferRequestBody(req)` resolves. Could a `close` event be missed if the client disconnects during buffering? No — in Node.js, Promise continuations execute as microtasks, which drain completely before the event loop advances to the I/O poll phase. When `bufferRequestBody` resolves, the subsequent code (including `res.on("close")` registration) runs within the same microtask, before any pending I/O events are dispatched.

### 4.3 Design Rationale

**Why full buffering instead of partial buffering or stream transforms?**

- **Partial buffering** (e.g., buffering only the JSON header) cannot determine the buffer boundary without knowing the specific modification requirements, and adds implementation complexity
- **Stream transforms** (`Transform` stream) are more memory-efficient but cannot support structured JSON modifications (field injection/removal) that require complete parsing
- **Full buffering** is the simplest and most general approach, and Anthropic Messages API request bodies are typically in the KB to low-MB range, making memory overhead negligible

**Why remove `unpipe` instead of keeping it as a harmless call?**

The presence of `unpipe` implies that callers need to understand that a pipe exists. Removing it makes the semantics of `abortUpstream` clearer — it does exactly one thing: destroy the upstream request. Keeping a harmless but unnecessary call would mislead maintainers into thinking a pipe is still in use.

**Why not introduce a request body size limit?**

Frigga is designed as a transparent proxy. The upstream Anthropic API enforces its own request body size limits and returns 413 errors. Adding an additional limit at the frigga layer would increase configuration complexity and risk falling out of sync with upstream limits.

## 5. Testing Strategy

Testing covers both unit and integration levels. Unit tests verify behavioral changes in each helper function, and integration tests verify no end-to-end regression.

**Key Scenarios:**

| # | Scenario | Input | Expected Behavior |
|---|----------|-------|-------------------|
| 1 | Normal buffering | Multiple data chunks followed by `end` | `bufferRequestBody` resolves with concatenated Buffer |
| 2 | Empty body | Immediate `end` with no data | `bufferRequestBody` resolves with `Buffer.alloc(0)` |
| 3 | Stream error | `destroy(new Error("ECONNRESET"))` | `bufferRequestBody` rejects |
| 4 | `abortUpstream` normal | `upstreamReq.destroyed === false` | Calls `upstreamReq.destroy()` |
| 5 | `abortUpstream` already destroyed | `upstreamReq.destroyed === true` | No-op |
| 6 | `abortUpstream` undefined | `upstreamReq === undefined` | No-op |
| 7 | Log includes `request_body` | `requestBody: Buffer.from("...")` | Log JSON contains `request_body` field |
| 8 | Log omits `request_body` | `requestBody` omitted | Log JSON has no `request_body` field |
| 9 | Exactly-once guard | Call `emitRequestLog` twice | Only one log entry emitted |
| 10 | Local error path drain | Stream still active, `requestBody` omitted | `req.resume()` is called |
| 11 | `content-length` not forwarded | Client sends `content-length: 42` | `buildUpstreamHeaders` output does not contain `content-length` |
| 12 | Client disconnect during buffering | `req.destroy(new Error("ECONNRESET"))` before `end` | `bufferRequestBody` rejects; `res.destroy()` called; stderr `client_error` logged; request log emitted with status 499 |

**Test change checklist:**

`handler.test.js`:
- **Add** `bufferRequestBody` test group (scenarios 1-3); import `PassThrough` from `node:stream`
- **Update** `buildUpstreamHeaders` tests — `content-length` should no longer appear in forwarded headers
- **Update** `abortUpstream` tests — remove `req.unpipe` related tests, update to single-parameter signature
- **Update** `createRequestLogEmitter` tests — `requestChunks` -> `requestBody`
- **Delete** deferred finalization tests ("defers finalization to end event", "finalized guard prevents double execution")
- **Delete** "clears requestChunks after consumption" test

`server.test.js`:
- Existing integration tests unchanged — they do not test upstream forwarding (upstream URL is hardcoded to `api.anthropic.com`), and local error path logic is unmodified

**Scenario 12 coverage note:**

Scenario 12 (client disconnect during buffering) exercises the `catch` path inside the `startServer` forwarding callback. This path is not directly unit-testable because it lives inside the `http.createServer` closure. Coverage is achieved by composition of individually unit-tested components:

- `bufferRequestBody` rejection — scenario 3 verifies that a stream error produces a Promise rejection
- `createRequestLogEmitter` with `requestBody: Buffer.alloc(0)` — scenario 7/8 verifies `request_body` inclusion/omission based on `requestBody` presence
- Exactly-once guard — scenario 9 verifies duplicate calls are suppressed

The catch-path wiring itself (`res.destroy()` call, stderr `client_error` log emission, `emitRequestLog(499)` invocation) is verified by code review during Phase 3.

## 6. Implementation Plan

### Phase 1: Unit Tests — 30 min

- [ ] Add `bufferRequestBody` test group: normal buffering, empty body, stream error
- [ ] Update `buildUpstreamHeaders` tests: verify `content-length` is no longer forwarded
- [ ] Update `abortUpstream` tests: single-parameter signature, remove `unpipe`-related assertions
- [ ] Update `createRequestLogEmitter` tests: `requestChunks` -> `requestBody`, delete deferred finalization and array cleanup tests

**Done when:** New tests written and failing (red) — `bufferRequestBody` not yet implemented, `abortUpstream` and `createRequestLogEmitter` signatures not yet changed

### Phase 2: Helper Functions — 20 min

- [ ] Remove `content-length` from `REQUEST_HEADER_FORWARD`
- [ ] Add `bufferRequestBody` exported function
- [ ] Simplify `abortUpstream` — remove `req` parameter and `unpipe` call
- [ ] Refactor `createRequestLogEmitter` — `requestChunks` -> `requestBody`, eliminate deferred finalization logic

**Done when:** All Phase 1 tests pass (green)

### Phase 3: Forwarding Path — 30 min

- [ ] Add `async` to `http.createServer` callback
- [ ] Replace forwarding path: `bufferRequestBody` + `try/catch` + `upstreamReq.end(requestBody)`
- [ ] Remove `req.pipe(upstreamReq)`
- [ ] Remove sidecar `req.on("data")` listener and `requestChunks` variable
- [ ] Remove `req.on("error")` handler from forwarding path
- [ ] Update `abortUpstream` calls in `res.on("close")` to single-parameter

**Done when:** All tests pass (`npm test`), lint passes (`npm run check`)

## 7. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| `async` callback produces unhandled Promise rejection | Low | High | `try/catch` wraps `await bufferRequestBody(req)`; subsequent code is synchronous with no additional Promises |
| Large request bodies cause memory pressure | Low | Low | Anthropic API bodies are typically KB to low-MB; comparable to current sidecar capture memory usage when `--log-body` is enabled |
| Latency introduced by buffering | Low | Low | Request bodies traverse localhost; sub-millisecond even for MB-sized payloads |
| `abortUpstream` exported signature change | Low | Low | Internal function with no external consumers; all call sites and tests updated together |
| Partial body data lost in `--log-body` audit logs on client disconnect | High | Low | In the current streaming design, the sidecar `data` listener captures chunks in parallel, so `requestChunks` retains partial data when the client disconnects mid-transfer, and `createRequestLogEmitter` logs it. In the new design, `bufferRequestBody` rejection discards the internal `chunks` array with the closure, and the catch path substitutes `Buffer.alloc(0)`. This is an intentional simplification: partial bodies have limited audit value (incomplete JSON cannot be parsed), and recovering them would require exposing `bufferRequestBody` internals, adding complexity that conflicts with this RFC's goal of simplification. |

## 8. Future Work

- **Request body modification framework** — insert a pluggable transform layer between `bufferRequestBody` and `upstreamReq.end()` to support JSON field injection/removal, credential embedding, etc.
- **Response body audit logging** — decompress based on `content-encoding` and log response bodies (planned in RFC-004 section 10, RFC-006 section 8)
- **Request body size monitoring** — add a `request_body_size` field to log entries for monitoring body size distribution (non-blocking to forwarding)

## 9. References

- [RFC-004: Upstream Request Forwarding](rfc_004_upstream_request_forwarding.md) — current streaming forwarding design and sidecar capture strategy
- [RFC-006: Optional Request Body Logging](rfc_006_optional_request_body_logging.md) — `--log-body` flag and request body logging design
- [RFC-005: Unify Local Error Path Logging](rfc_005_unify_local_error_logging.md) — `createRequestLogEmitter` design decision (stream draining inside the emitter)
- [Node.js `http.createServer` documentation](https://nodejs.org/api/http.html#httpcreateserveroptions-requestlistener) — callback return value is ignored
- [Node.js Event Loop documentation](https://nodejs.org/en/learn/asynchronous-work/event-loop-timers-and-nexttick) — microtask vs. I/O poll execution order

---

## Changelog

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2026-04-13 | Chason Tang | Initial version |
