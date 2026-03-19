# RFC-005: Unify Local Error Path Logging

**Version:** 1.0
**Author:** Chason Tang
**Date:** 2026-03-19
**Status:** Implemented

---

## 1. Summary

This proposal eliminates the inline request logging in the local 404/405 error path by reusing the existing `createRequestLogEmitter` factory function. The local error path will no longer log `request_body` — only `request_headers` — because the body is frequently incomplete when clients disconnect after receiving the error response, and the body content is meaningless for routing and method validation errors.

## 2. Motivation

The local 404/405 error path in `server.js` (lines 184–217) sends the HTTP response before fully consuming the request body. This is correct for latency, but creates a logging defect: if the client disconnects after receiving the error (common during development with direct HTTP connections), the `req` stream emits `error` instead of `end`. At that point `requestChunks` contains only a partial payload, and the `request_body` field in the NDJSON log is silently truncated.

Beyond the correctness issue, the local error path contains a 27-line inline logging implementation that duplicates the logic already encapsulated in `createRequestLogEmitter`. The two implementations share the same log schema, exactly-once guard pattern, and stream-draining behavior, but diverge in subtle ways (the inline version lacks the dual-guard pattern and does not check `readableEnded`/`destroyed`). Consolidating to a single implementation reduces maintenance surface and prevents future drift.

## 3. Goals and Non-Goals

**Goals:**
- Eliminate incomplete `request_body` in local error logs by omitting the field entirely for 404/405 responses
- Replace the inline logging implementation with `createRequestLogEmitter`, reducing code duplication
- Ensure the request stream is properly drained on the local error path to avoid socket resource leaks

**Non-Goals:**
- Changing the forwarding path logging behavior — `request_body` remains captured for forwarded requests where it provides diagnostic value
- Adding new log fields or events — the schema change is limited to omitting `request_body` on local errors

## 4. Design

### 4.1 Overview

The change touches two components: the `createRequestLogEmitter` factory function and the local error path in the `startServer` request handler.

```
Before:
┌─────────────────┐       ┌────────────────────────────────┐
│ Local error path │──────→│ Inline logging (27 lines)      │
│ (404 / 405)     │       │ • collects body → often partial │
└─────────────────┘       └────────────────────────────────┘
┌─────────────────┐       ┌────────────────────────────────┐
│ Forwarding path  │──────→│ createRequestLogEmitter()      │
│ (upstream)       │       │ • collects body → complete      │
└─────────────────┘       └────────────────────────────────┘

After:
┌─────────────────┐       ┌────────────────────────────────┐
│ Local error path │──┐   │ createRequestLogEmitter()      │
│ (404 / 405)     │  ├──→│ • requestChunks omitted → no   │
└─────────────────┘  │   │   body, immediate finalization  │
┌─────────────────┐  │   │ • requestChunks provided →     │
│ Forwarding path  │──┘   │   body collected, deferred     │
│ (upstream)       │       └────────────────────────────────┘
```

### 4.2 Detailed Design

#### 4.2.1 Make `requestChunks` Optional in `createRequestLogEmitter`

The `requestChunks` parameter becomes optional. Its presence controls two behaviors:

| `requestChunks` | Body in log | Finalization timing |
|-----------------|-------------|---------------------|
| Provided (array) | `request_body` included | Deferred until stream ends (current behavior) |
| Omitted (`undefined`) | `request_body` omitted | Immediate |

When `requestChunks` is omitted:
- The `finalize()` function builds the log entry without `request_body`.
- The emitter finalizes immediately — no `end`/`error` listeners are registered for deferred finalization.
- If the request stream is still active (`!req.readableEnded && !req.destroyed`), a no-op `error` listener is registered to suppress unhandled `error` events (which would otherwise crash the process if the client disconnects during draining), and `req.resume()` is called to drain the stream and free the socket for HTTP keep-alive reuse.

When `requestChunks` is provided:
- Behavior is identical to the current implementation: the dual-guard pattern, deferred finalization via `end`/`error` listeners, and `req.resume()` to trigger stream completion.

Updated implementation:

```javascript
export function createRequestLogEmitter({ req, requestChunks, startTime }) {
  let emitted = false;

  return function emitRequestLog(status, responseHeaders) {
    if (emitted) return;
    emitted = true;

    let finalized = false;
    const finalize = () => {
      if (finalized) return;
      finalized = true;
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
      if (requestChunks) {
        entry.request_body = Buffer.concat(requestChunks).toString("utf-8");
        requestChunks.length = 0;
      }
      if (responseHeaders) entry.response_headers = responseHeaders;
      process.stdout.write(`${JSON.stringify(entry)}\n`);
    };

    if (requestChunks && !req.readableEnded && !req.destroyed) {
      req.on("end", finalize);
      req.on("error", () => finalize());
    } else {
      finalize();
    }

    if (!req.readableEnded && !req.destroyed) {
      if (!requestChunks) {
        req.on("error", () => {});
      }
      req.resume();
    }
  };
}
```

Key design decisions:

- **`req.resume()` is unconditional** (when stream is active): In the body-collection path, it triggers `end`/`error` events for deferred finalization. In the no-body path, it drains the stream to free the socket. In the forwarding path, `req.pipe()` has already activated flowing mode, making `resume()` a no-op.
- **`finalized` guard is retained** even in the immediate path: While the `finalized` guard is strictly necessary only for the deferred path (where both `end` and `error` may fire), keeping it in all paths costs nothing and protects against future refactoring errors.
- **No-op `error` listener in the no-body path**: When `requestChunks` is omitted and the stream is still active, `req.resume()` triggers flowing mode, which may cause the stream to emit `error` if the client disconnects during draining. Without an `error` listener, Node.js treats this as an uncaught exception and crashes the process. The no-op listener suppresses this. In the body-collection path, the existing `req.on("error", () => finalize())` already serves this role.

#### 4.2.2 Replace Inline Local Error Path

The local error path in `startServer` is simplified from 27 lines to 5:

```javascript
// ── Local error path ──────────────────────────────────
if (result !== null) {
  const { statusCode, headers, body } = result;
  res.writeHead(statusCode, headers);
  res.end(body);

  const emitRequestLog = createRequestLogEmitter({ req, startTime });
  emitRequestLog(statusCode);
  return;
}
```

Removed:
- `requestChunks` array and `data` listener — no body collection
- `localLogEmitted` boolean guard — replaced by the emitter's built-in `emitted` guard
- `emitLocalLog` closure — replaced by `emitRequestLog`
- `error`/`end` listeners for deferred logging — finalization is immediate

### 4.3 Design Rationale

**Why omit `request_body` instead of fixing the truncation?**

For 404 (wrong route) and 405 (wrong method), the request body carries no diagnostic value — the error is fully determined by the URL and method, both of which are already logged. Attempting to reliably capture the body would require buffering it before sending the response, adding latency to the error path for data that nobody uses.

**Why make `requestChunks` optional rather than creating a separate function?**

A separate function (e.g., `createHeaderOnlyLogEmitter`) would duplicate the log schema construction, the `emitted` guard, and the `process.stdout.write` call. Making `requestChunks` optional is a one-line conditional (`if (requestChunks)`) that naturally extends the existing function. The forwarding path call sites pass `requestChunks` unchanged — zero migration required.

**Why call `req.resume()` inside the emitter?**

Stream draining is essential on the local error path to prevent socket resource leaks on HTTP keep-alive connections. Placing it inside the emitter (rather than requiring callers to remember it) makes the function self-contained and eliminates a class of bugs where a new caller forgets to drain.

## 5. Testing Strategy

Unit tests cover the new `requestChunks`-omitted behavior of `createRequestLogEmitter`. Integration tests verify the end-to-end local error path produces correct log entries. Existing forwarding-path tests serve as regression coverage for the unchanged behavior.

**Key Scenarios:**

| # | Scenario | Input | Expected Behavior |
|---|----------|-------|-------------------|
| 1 | `createRequestLogEmitter` without `requestChunks`, stream ended | `{ req: { readableEnded: true }, startTime }` | Immediate finalization, log entry has no `request_body` field |
| 2 | `createRequestLogEmitter` without `requestChunks`, stream active | `{ req: { readableEnded: false }, startTime }` | Immediate finalization, `req.resume()` called, log entry has no `request_body` field |
| 3 | `createRequestLogEmitter` with `requestChunks` (regression) | `{ req, requestChunks: [Buffer], startTime }` | Current behavior: deferred finalization, `request_body` in log |
| 4 | 404 integration log | `POST /v1/unknown` with body | Log entry has `request_headers`, no `request_body` |
| 5 | 405 integration log | `GET /v1/messages` | Log entry has `request_headers`, no `request_body` |
| 6 | Forwarding path log (regression) | `POST /v1/messages` | `request_body` still present in log |

## 6. Implementation Plan

### Phase 1: Unit Tests — 10 min

- [x] Add test: `createRequestLogEmitter` without `requestChunks` emits log with no `request_body`, stream already ended
- [x] Add test: `createRequestLogEmitter` without `requestChunks` calls `req.resume()` when stream active
- [x] Add test: `createRequestLogEmitter` without `requestChunks`, exactly-once guard still works

**Done when:** New unit tests written and initially failing (red)

### Phase 2: Core Implementation — 15 min

- [x] Modify `createRequestLogEmitter` to make `requestChunks` optional
- [x] Replace inline local error path with `createRequestLogEmitter` call
- [x] Update integration tests: 404/405 logs assert `request_body` is absent

**Done when:** All unit tests and integration tests passing (green)

## 7. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Log consumers that parse `request_body` on 404/405 entries break | Low | Low | The field was already unreliable (often truncated). Consumers should handle missing fields. |
| `req.resume()` in emitter conflicts with `req.pipe()` in forwarding path | Low | Low | `resume()` on an already-flowing stream is a documented no-op in Node.js. Existing forwarding-path tests provide regression coverage. |

## 8. Future Work

- Structured error codes in local error logs (e.g., `error_type: "not_found_error"`) to enable filtering without parsing the response body

## 9. References

- [RFC-003: NDJSON Request Debug Logging](./rfc_003_request_debug_logging.md) — original NDJSON log schema definition
- [RFC-004: Upstream Request Forwarding](./rfc_004_upstream_request_forwarding.md) — `createRequestLogEmitter` design and dual-guard pattern
- [Node.js Readable Stream documentation](https://nodejs.org/api/stream.html#readable-streams) — `resume()`, `readableEnded`, flowing mode semantics
- `frigga/src/server.js:83–119` — current `createRequestLogEmitter` implementation
- `frigga/src/server.js:184–217` — current inline local error path

---

## Changelog

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2026-03-19 | Chason Tang | Initial version |
