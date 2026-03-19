# RFC-006: Optional Request Body Logging

**Version:** 1.0
**Author:** Chason Tang
**Date:** 2026-03-19
**Status:** Implemented

---

## 1. Summary

This proposal adds a `--log-body` CLI flag to frigga that controls whether the request body is included in NDJSON request logs. The flag defaults to off (body omitted). When enabled, the existing sidecar body-capture behavior is preserved. Since request bodies for the Messages API frequently contain large prompts (tens to hundreds of KB), disabling body logging by default substantially reduces log volume while retaining all other diagnostic fields.

## 2. Motivation

Today, frigga unconditionally captures and logs the full `request_body` for every forwarded request (RFC-003). The Messages API request body typically contains the full conversation history — often 10–200 KB of JSON per request. In continuous-use scenarios this produces gigabytes of log data per day, most of which is never inspected.

This creates three concrete problems:

1. **Disk pressure** — log files grow rapidly, requiring frequent rotation or manual cleanup.
2. **Noise** — when piping logs through `jq` for debugging, the `request_body` field dominates output and obscures the fields operators actually care about (status, duration, headers).
3. **Sensitivity** — request bodies may contain user prompts or API keys embedded in messages, making unrestricted logging a data-handling concern.

Making body logging opt-in eliminates these problems for the default case while preserving full-fidelity logging when explicitly requested for debugging.

## 3. Goals and Non-Goals

**Goals:**
- Add a `--log-body` boolean CLI flag that enables request body capture in NDJSON logs
- Default to body-omitted logging, reducing per-request log size by 90%+ for typical Messages API calls
- Zero behavioral change to proxy forwarding, header handling, or error paths

**Non-Goals:**
- Response body logging — response streams are not buffered by design (RFC-004) and adding buffering would change memory and latency characteristics. This is a separate concern.
- Log-level filtering (e.g., suppressing INFO logs entirely) — orthogonal to field-level control and better addressed by external tooling (`jq`, `grep`).
- Partial body logging (e.g., first N bytes) — adds complexity for marginal benefit; operators who need the body typically need the full body.

## 4. Design

### 4.1 Overview

The change threads a single boolean (`logBody`) from CLI parsing through to the request handler. When `logBody` is `false` (the default), the forwarding path skips sidecar body capture entirely — no `data` listener is registered, no `requestChunks` array is allocated, and `createRequestLogEmitter` receives no `requestChunks` parameter, which already causes it to omit the `request_body` field (RFC-005 §4.2).

```
CLI (--log-body)
      │
      ▼
  parseArgs()  ──►  resolveArgs()  ──►  startServer({ port, host, logBody })
                                              │
                                              ▼
                                    request handler
                                              │
                              ┌───────────────┴───────────────┐
                              │                               │
                        logBody=true                     logBody=false
                              │                               │
                   register data listener            skip data listener
                   pass requestChunks                omit requestChunks
                              │                               │
                              ▼                               ▼
                   createRequestLogEmitter        createRequestLogEmitter
                   (with requestChunks)           (without requestChunks)
                              │                               │
                              ▼                               ▼
                   log includes request_body      log omits request_body
```

### 4.2 Detailed Design

#### 4.2.1 CLI Argument Parsing (`cli.js`)

The `--log-body` flag uses the existing boolean-flag parsing path in `parseArgs()` (line 16: `args[arg.slice(2)] = true` when no `=` is present). No changes to `parseArgs()` are required.

`resolveArgs()` extracts the flag and defaults to `false`:

```javascript
const logBody = args["log-body"] === true;

return { port, host, logBody };
```

The `printUsage()` function adds one line to the usage output:

```
  --log-body  Include request body in NDJSON logs (default: off)
```

#### 4.2.2 Config Propagation (`index.js` → `server.js`)

The config object shape changes from `{ port, host }` to `{ port, host, logBody }`. `startServer()` destructures the new field:

```javascript
export function startServer({ port, host, logBody }) {
```

No new constants are needed.

#### 4.2.3 Conditional Sidecar Capture (`server.js` — forwarding path)

The forwarding path (line 206+) currently unconditionally sets up the sidecar:

```javascript
// Current — always captures body
const requestChunks = [];
req.on("data", (chunk) => requestChunks.push(chunk));
const emitRequestLog = createRequestLogEmitter({ req, requestChunks, startTime });
```

With the change:

```javascript
// Proposed — conditionally captures body
const requestChunks = logBody ? [] : undefined;
if (requestChunks) {
  req.on("data", (chunk) => requestChunks.push(chunk));
}
const emitRequestLog = createRequestLogEmitter({ req, requestChunks, startTime });
```

When `requestChunks` is `undefined`, `createRequestLogEmitter` already handles this correctly:
- Line 108: `if (requestChunks)` guard skips body serialization
- Line 116: `if (requestChunks && ...)` guard skips deferred finalization
- The `request_body` field is simply absent from the log entry

No changes to `createRequestLogEmitter` itself are required.

#### 4.2.4 Local Error Path — No Change

The local error path (404/405, line 196–203) already omits `requestChunks` per RFC-005. The `logBody` flag has no effect on this path, which is correct — local errors never reach upstream, so body capture serves no purpose regardless of the flag.

### 4.3 Design Rationale

**Why a boolean flag instead of a log-level system?** A boolean flag is the minimal viable control. Log-level systems (debug/info/warn) conflate verbosity with field selection and require defining what "debug" means for every log field. A single flag maps directly to the user's intent: "I want body data" or "I don't." If more granular control is needed in the future, this flag remains a sensible default.

**Why default to off?** The primary use case for frigga is as a transparent proxy. Operators need to know that requests are flowing and what status codes are returned. The body is only needed when actively debugging a specific request — a minority scenario. Defaulting to off follows the principle of least surprise for log volume.

**Why skip the `data` listener entirely instead of capturing and discarding?** Registering a `data` listener and pushing chunks into an array that is never read wastes memory and GC cycles. By not registering the listener at all, the request stream flows directly through `req.pipe(upstreamReq)` with no sidecar overhead.

**Why reuse the existing `requestChunks` undefined-path?** RFC-005 already established the convention that omitting `requestChunks` from `createRequestLogEmitter` results in no `request_body` field. Reusing this path avoids any changes to the logging core and keeps the delta minimal.

## 5. Testing Strategy

All tests use Node.js built-in `node:test` runner, consistent with the existing test suite. Unit tests validate CLI parsing and conditional sidecar behavior; integration tests validate end-to-end log output.

**Key Scenarios:**

| # | Scenario | Input | Expected Behavior |
|---|----------|-------|-------------------|
| 1 | Default (no flag) | `node src/index.js` | Request logs omit `request_body` field |
| 2 | Flag enabled | `node src/index.js --log-body` | Request logs include `request_body` field |
| 3 | Flag does not affect local errors | `--log-body` + `GET /v1/unknown` | 404 log still omits `request_body` (local error path) |
| 4 | `resolveArgs` defaults `logBody` to `false` | `resolveArgs({})` | `{ ..., logBody: false }` |
| 5 | `resolveArgs` sets `logBody` to `true` | `resolveArgs({ "log-body": true })` | `{ ..., logBody: true }` |
| 6 | No `data` listener when flag is off | Internal verification | `requestChunks` is `undefined`, no listener registered |
| 7 | Usage text includes `--log-body` | `printUsage()` output | Contains `--log-body` description |

## 6. Implementation Plan

### Phase 1: Unit Tests — 15 min

- [x] Add `resolveArgs` tests: default `logBody` is `false`; `--log-body` flag sets it to `true`
- [x] Add `printUsage` test: output contains `--log-body`
- [x] Add `createRequestLogEmitter` test confirming existing behavior: `requestChunks` undefined → no `request_body` (already covered, verify)

**Done when:** New unit tests written and initially failing (red) for the `resolveArgs` changes.

### Phase 2: CLI and Config Changes — 15 min

- [x] Update `resolveArgs()` in `cli.js` to extract `logBody` from args
- [x] Update `printUsage()` in `cli.js` to document `--log-body`
- [x] Update `startServer()` signature in `server.js` to destructure `logBody`

**Done when:** Unit tests passing (green); `node src/index.js --help` shows the new flag.

### Phase 3: Conditional Sidecar Capture — 15 min

- [x] Modify forwarding path in `server.js` to conditionally create `requestChunks` and register `data` listener based on `logBody`
- [x] Verify no changes needed to `createRequestLogEmitter`

**Done when:** All unit tests passing; manual smoke test confirms body present/absent based on flag.

### Phase 4: Integration Tests — 15 min

- [x] Add integration test: default startup → forwarding-path log omits `request_body`
- [x] Add integration test: `--log-body` startup → forwarding-path log includes `request_body`
- [x] Verify existing integration tests still pass (local error tests should be unaffected)

**Done when:** Full test suite green (`node --test`).

## 7. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Existing log-parsing scripts break because `request_body` field disappears by default | Med | Low | Field was always optional (RFC-005 local errors already omit it); any robust consumer should handle missing fields. Document the default change in release notes. |
| Operators forget to enable `--log-body` when debugging | Low | Low | Error/warning logs on stderr are unaffected; body is only needed for deep debugging. Usage text makes the flag discoverable. |

## 8. Future Work

- **Per-request body logging via header** — allow clients to request body capture for specific requests (e.g., `X-Frigga-Log-Body: true`), useful for selective debugging without restarting the proxy.
- **Response body logging** — would require buffering the upstream response stream, with significant memory and latency implications. Deferred until a concrete use case emerges.
- **Body truncation** — log only the first N bytes of the body to balance debuggability with log size. Deferred as the binary on/off flag covers the primary use case.

## 9. References

- RFC-003: NDJSON Request Debug Logging — established the `request_body` field
- RFC-004: Upstream Request Forwarding — established the sidecar capture pattern
- RFC-005: Unify Local Error Path Logging — established the `requestChunks`-omission convention

---

## Changelog

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2026-03-19 | Chason Tang | Initial version |
