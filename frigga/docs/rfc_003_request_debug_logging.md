# RFC-003: NDJSON Request Debug Logging

**Version:** 1.1
**Author:** Chason Tang
**Date:** 2026-03-16
**Status:** Proposed

---

## 1. Summary

Replace Frigga's plain-text log output with NDJSON (Newline-Delimited JSON) across all log categories (startup, request, shutdown, error). Request logs are extended to include full HTTP request headers, request body, response headers, and response body, enabling effective debugging of the proxy request flow without external packet capture tools.

## 2. Motivation

Current request logs output only method, path, status, and duration:

```
[2026-03-13T10:15:32.100Z] [INFO] POST /v1/messages 401 2ms
```

When debugging authentication failures, malformed requests, or unexpected client behavior, this line provides no visibility into what the client actually sent. The operator must resort to external tools (`tcpdump`, `mitmproxy`, `curl -v`) to inspect headers and body, adding friction to every debugging session.

Switching to NDJSON with full request/response details solves this: headers reveal authentication issues, content-type mismatches, and unexpected client behavior; body reveals malformed JSON, incorrect parameters, or missing fields. NDJSON enables `jq` filtering, `grep` line-based search, and machine parsing — all compatible with `tail -f` workflows.

## 3. Goals and Non-Goals

**Goals:**
- Log full HTTP request headers and request body for every request
- Log response status, headers, and body for every request
- Use NDJSON format for all structured log output (startup, request, shutdown, error)
- Maintain stdout/stderr severity split

**Non-Goals:**
- Log level configuration — all requests are logged at the same detail level; a `--log-level` flag may be added later if verbose output becomes a performance concern
- Security redaction of sensitive headers (e.g., `Authorization`) — deferred until upstream forwarding introduces a second credential boundary
- Request body size limits — the upstream Anthropic API enforces its own limits; the proxy does not add an additional constraint

## 4. Design

### 4.1 Overview

All structured log output becomes NDJSON — one valid JSON object per line, terminated by `\n`. Every log entry shares a common envelope with three fixed fields:

```json
{"timestamp": "<ISO 8601 UTC>", "level": "<INFO|WARN|ERROR>", "event": "<category>", ...}
```

The `event` field identifies the log category and determines which additional fields are present. Operators can filter by category using `jq`:

```bash
# Pretty-print all logs in real time
node src/index.js --api-key=xxx 2>&1 | jq .

# Filter to request logs only
node src/index.js --api-key=xxx | jq 'select(.event == "request")'

# Filter to failed requests
node src/index.js --api-key=xxx | jq 'select(.event == "request" and .status >= 400)'

# Write to file and tail
node src/index.js --api-key=xxx > frigga.log 2>&1 &
tail -f frigga.log | jq .
```

### 4.2 Detailed Design

#### 4.2.1 Log Entry Schemas

All log entries share the common envelope fields:

| Field | Type | Description |
|-------|------|-------------|
| `timestamp` | string | ISO 8601 UTC timestamp (`new Date().toISOString()`) |
| `level` | string | `"INFO"`, `"WARN"`, or `"ERROR"` |
| `event` | string | Log category — determines which additional fields are present |

**Startup** (`event: "startup"`, stdout):

```json
{"timestamp":"2026-03-16T10:15:30.000Z","level":"INFO","event":"startup","host":"127.0.0.1","port":3000}
```

| Field | Type | Description |
|-------|------|-------------|
| `host` | string | Bound address from `server.address()` |
| `port` | number | Bound port from `server.address()` |

**Request** (`event: "request"`, stdout):

```json
{"timestamp":"2026-03-16T10:15:32.100Z","level":"INFO","event":"request","method":"POST","url":"/v1/messages","status":401,"duration_ms":2,"request_headers":{"content-type":"application/json","authorization":"Bearer test-key","user-agent":"claude-code/1.0"},"request_body":"{\"model\":\"claude-sonnet-4-20250514\",\"messages\":[{\"role\":\"user\",\"content\":\"Hello\"}]}","response_headers":{"content-type":"application/json"},"response_body":"{\"type\":\"error\",\"error\":{\"type\":\"authentication_error\",\"message\":\"Invalid API key\"}}"}
```

| Field | Type | Description |
|-------|------|-------------|
| `method` | string | HTTP request method |
| `url` | string | Request URL (includes query string if present) |
| `status` | number | HTTP response status code |
| `duration_ms` | number | Time from `request` event to `res.end()` callback |
| `request_headers` | object | All request headers (Node.js `req.headers` — lowercase keys) |
| `request_body` | string | Full request body decoded as UTF-8; `""` if no body sent |
| `response_headers` | object | All response headers from `res.getHeaders()`, captured in the `res.end()` callback |
| `response_body` | string | Response body string |

**Shutdown** (`event: "shutdown"`, stdout):

```json
{"timestamp":"2026-03-16T10:20:00.000Z","level":"INFO","event":"shutdown","message":"stop accepting new connections"}
{"timestamp":"2026-03-16T10:20:00.050Z","level":"INFO","event":"shutdown","message":"complete"}
```

**Shutdown timeout** (`event: "shutdown"`, stderr):

```json
{"timestamp":"2026-03-16T10:20:30.000Z","level":"WARN","event":"shutdown","message":"timed out, forcing close"}
```

**Server error** (`event: "server_error"`, stderr):

```json
{"timestamp":"2026-03-16T10:20:30.000Z","level":"ERROR","event":"server_error","message":"ECONNRESET"}
```

| Field | Type | Description |
|-------|------|-------------|
| `message` | string | Human-readable description of the event |

#### 4.2.2 Request Body Collection

The HTTP server callback collects the full request body before calling `handleRequest` and emitting the log entry. This changes the callback from synchronous to asynchronous:

**Before** (current — no body reading):

```
request event → handleRequest() → res.end() → log
```

**After** (body collection before handler):

```
request event → collect body chunks via data/end events → handleRequest() → res.end() → log
```

The body is collected via `req.on('data')` / `req.on('end')` and concatenated into a single UTF-8 string. The pure `handleRequest` function is unchanged — it still receives `{ method, url, headers }` and returns `{ statusCode, headers, body }`. Body collection and log emission are the responsibility of the HTTP server callback in `startServer`.

If the request stream emits an `error` event before `end`, the collected partial body is discarded, `handleRequest` is not called, and no request log entry is emitted.

#### 4.2.3 stdout / stderr Split

The severity-based output split is preserved:

| Stream | Levels | Events |
|--------|--------|--------|
| `stdout` | INFO | `startup`, `request`, `shutdown` (normal) |
| `stderr` | WARN, ERROR | `shutdown` (timeout), `server_error` |

`stderr` continues to handle CLI usage errors (`printUsage()` in `cli.js`), which remain plain text since they are user-facing help messages, not structured log entries.

### 4.3 Design Rationale

**NDJSON over plain text**: Plain text cannot represent nested structures (headers, body) in a single line without escaping conventions that would make `grep` unreliable. NDJSON gives one parseable object per line, compatible with both `grep` (line-based) and `jq` (structured). The `tail -f | jq .` workflow provides real-time pretty-printed output.

**Full replacement over `--log-level` toggle**: Adding a log level flag doubles the number of output paths to test and maintain. Since Frigga is a development/debugging proxy, verbose output is always desirable. If performance becomes a concern under high request volume, a log level flag can be added later.

**Body collection before response**: The alternative — logging body asynchronously while streaming — would require teeing the request stream or logging incomplete data. Since the current phase returns immediate error/501 responses (no upstream forwarding), buffering the full body has no latency or memory impact. When upstream forwarding is implemented, this design will be revisited (see §9 Future Work).

**Structured fields over message string**: Using typed fields (`"method": "POST"`, `"status": 401`) instead of embedding values in a message string (`"POST /v1/messages 401"`) enables `jq` filtering without regex: `jq 'select(.status == 401)'`, `jq 'select(.request_headers["content-type"] == "application/json")'`.

**`event` field over `type`**: The Anthropic API error format already uses `type` at the top level (`"type": "error"`). Using `event` for the log category avoids confusion when reading logs alongside API responses.

## 5. Interface Changes

**Before** (plain-text, one line per request):

```
[2026-03-16T10:15:30.000Z] [INFO] Frigga listening on 127.0.0.1:3000
[2026-03-16T10:15:32.100Z] [INFO] POST /v1/messages 401 2ms
[2026-03-16T10:20:00.000Z] [INFO] Shutting down: stop accepting new connections
[2026-03-16T10:20:00.050Z] [INFO] Shutdown complete
```

**After** (NDJSON, one JSON object per line):

```
{"timestamp":"2026-03-16T10:15:30.000Z","level":"INFO","event":"startup","host":"127.0.0.1","port":3000}
{"timestamp":"2026-03-16T10:15:32.100Z","level":"INFO","event":"request","method":"POST","url":"/v1/messages","status":401,"duration_ms":2,"request_headers":{...},"request_body":"...","response_headers":{...},"response_body":"..."}
{"timestamp":"2026-03-16T10:20:00.000Z","level":"INFO","event":"shutdown","message":"stop accepting new connections"}
{"timestamp":"2026-03-16T10:20:00.050Z","level":"INFO","event":"shutdown","message":"complete"}
```

This is a breaking change to log output format. All log consumers (scripts, tests) that parse the previous plain-text format must be updated.

## 6. Testing Strategy

All tests use the Node.js built-in test runner (`node --test`). The pure `handleRequest` function is unchanged, so existing unit tests (`tests/handler.test.js`) require no modifications. Integration tests (`tests/server.test.js`) that assert on log output format must be updated.

**Key Scenarios:**

| # | Scenario | Input | Expected |
|---|----------|-------|----------|
| 1 | Request log is valid NDJSON | `POST /v1/messages` with auth and JSON body | stdout line parses as JSON; contains `event`, `method`, `url`, `status`, `duration_ms`, `request_headers`, `request_body`, `response_headers`, `response_body` |
| 2 | Request headers logged correctly | Request with `Content-Type` and `Authorization` headers | `request_headers` object contains both headers with correct values |
| 3 | Request body logged correctly | `POST` with `{"model": "claude-sonnet-4-20250514"}` body | `request_body` equals the sent JSON string |
| 4 | Empty request body | `POST` with no body | `request_body` is `""` |
| 5 | Response fields match actual response | Any request | `status`, `response_headers`, `response_body` in the log match the HTTP response received by the client |
| 6 | Startup log is valid NDJSON | Server starts | stdout line has `event: "startup"` with correct `host` and `port` |
| 7 | Shutdown log is valid NDJSON | `SIGTERM` sent to server | stdout lines have `event: "shutdown"` with expected messages |

## 7. Implementation Plan

Unit tests for the pure `handleRequest` function are unaffected by this change. This RFC only modifies integration-level behavior (log output format and body collection), so all new and updated tests are integration tests.

### Phase 1: Tests — 0.5 day

- [ ] Update existing integration test log assertions (startup log format, shutdown log format) to expect NDJSON
- [ ] Add integration tests for request log NDJSON fields: `request_headers`, `request_body`, `response_headers`, `response_body`
- [ ] Add integration test for empty request body case

**Done when:** New and updated tests written and failing against the current plain-text implementation (red).

### Phase 2: Implementation — 0.5 day

- [ ] Add request body collection (`data`/`end` events) in the `http.createServer` callback in `startServer`
- [ ] Replace all `process.stdout.write` / `process.stderr.write` log calls in `server.js` with `JSON.stringify()` + `\n` NDJSON output
- [ ] Update RFC-001 §4.2.5 to reference RFC-003 as the current logging specification

**Done when:** All integration tests passing (green). `node src/index.js --api-key=test` emits NDJSON on all log lines; `| jq .` pretty-prints each entry correctly.

## 8. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Large request bodies increase memory usage | Low | Low | Current phase returns immediate responses with no concurrent upstream I/O; practical body sizes are bounded by Anthropic API request limits. Revisit when upstream forwarding requires simultaneous request + response buffering |
| Body collection delays response delivery | Low | Low | Body is fully received before `handleRequest` runs, but current-phase responses are immediate (no upstream latency). No measurable impact on response time |
| `JSON.stringify` on large bodies is slow | Low | Low | Log emission occurs in the `res.end()` callback, after the response is sent to the client; serialization cost does not affect response latency |
| Log format breaking change breaks existing test assertions and external log parsing scripts | High | Low | Implementation Phase 1 updates all test assertions first; no external consumers exist at current scale |

## 9. Future Work

- **Security redaction** — When upstream forwarding introduces `--upstream-api-key`, add a redaction pass for `Authorization` header values and other sensitive fields before log emission
- **Request body size limit** — If body sizes become a concern, add truncation with a `"[truncated at N bytes]"` marker and a `request_body_bytes` field for the original size
- **Log level configuration** — Add `--log-level=info|debug` if operators need to reduce output volume in high-traffic scenarios, where `info` emits only envelope fields (no headers/body) and `debug` emits the full entry
- **Streaming body logging** — When upstream forwarding is implemented, request and response bodies are piped as streams via `stream.pipeline`. Logging full bodies would require teeing the stream, conflicting with the zero-buffering design. Options: log only headers, sample first N bytes, or log body byte count only

## 10. References

- [NDJSON Specification](https://github.com/ndjson/ndjson-spec)
- [jq Manual](https://jqlang.github.io/jq/manual/)
- RFC-001 §4.2.5 — Previous logging specification (superseded by this RFC)

---

## Changelog

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.1 | 2026-03-16 | Chason Tang | Rename `path` field to `url`; change `response_headers` to capture `res.getHeaders()` instead of `handleRequest` headers; specify error handling for request body collection failures; add log format breaking change to risk table |
| 1.0 | 2026-03-16 | Chason Tang | Initial version |
