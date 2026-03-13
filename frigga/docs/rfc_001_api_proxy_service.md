# RFC-001: Messages API Proxy Service

**Version:** 2.1  
**Author:** Chason Tang  
**Date:** 2026-03-13  
**Status:** Proposed

---

## 1. Summary

Frigga is a zero-runtime-dependency Node.js CLI proxy service that exposes `/v1/messages` and `/v1/messages/count_tokens` HTTP POST endpoints compatible with the Anthropic Messages API. The service validates incoming requests via `Authorization` header comparison against a startup-configured API key. In the future, validated requests will be forwarded to the Anthropic API with an upstream API key injected into the `Authorization` header; the upstream response is relayed back to the caller transparently. The primary consumer (Claude Code) uses `stream: true`, so responses are predominantly SSE streams; non-streaming JSON responses are also supported for API compatibility. The current phase returns `501 Not Implemented` for the forwarding step. In development, the service runs over plain HTTP; in production, Nginx sits in front and provides TLS termination (HTTPS).

## 2. Motivation

In scenarios where an API consumer (e.g., Claude Code) needs to access the Anthropic Messages API but the required authentication credentials must remain within a controlled environment (e.g., a private network, a developer's local machine), a proxy service is needed to decouple the consumer-facing access control from the upstream credential management.

A dedicated proxy service with a focused scope provides a minimal attack surface, zero configuration complexity, and full control over credential injection and streaming behavior.

## 3. Goals and Non-Goals

**Goals:**
- Single CLI entry point that starts an HTTP proxy server
- `/v1/messages` and `/v1/messages/count_tokens` POST endpoints compatible with the Anthropic Messages API path
- Incoming request validation via `--api-key` matched against the `Authorization: Bearer` header
- Upstream forwarding (future) with an upstream API key injected into the `Authorization` header for Anthropic API requests
- Zero runtime dependencies — all functionality implemented using Node.js built-in modules (`http`, `https`, `crypto`, `url`)
- Environment-agnostic: run over plain HTTP in development; production TLS (HTTPS) is handled entirely by Nginx with no code change
- Transparent response passthrough when upstream forwarding is implemented: the proxy pipes upstream responses without content-type branching or full-response buffering. The primary consumer (Claude Code) uses `stream: true` (SSE); non-streaming JSON responses are also supported for API compatibility

**Non-Goals:**
- TLS termination — delegated entirely to Nginx in production; the server always binds plain HTTP
- Request queuing or retry logic — if the upstream is unavailable, the error propagates immediately
- Rate limiting — delegated to Nginx or upstream infrastructure

## 4. Design

### 4.1 Overview

Frigga operates as a transparent HTTP proxy between the API consumer and the Anthropic API. The only difference between environments is the network layer in front of it.

**Current phase** (upstream forwarding not yet implemented):

```
Claude Code                      Frigga Server
     |                                |
     | POST /v1/messages              |
     | Authorization: Bearer <key>    |
     |------------------------------->|
     |                                | validate auth
     |                                | (compare with --api-key)
     |                                |
     |    501 Not Implemented         |
     |<-------------------------------|
```

**Future** (upstream forwarding implemented — shown with `stream: true`, the primary path for Claude Code):

```
Claude Code                      Frigga Server                      Anthropic API
     |                                |                                  |
     | POST /v1/messages              |                                  |
     | Authorization: Bearer <key>    |                                  |
     |------------------------------->|                                  |
     |                                | validate auth                    |
     |                                |                                  |
     |                                | POST /v1/messages                |
     |                                | Authorization: Bearer            |
     |                                |   <upstream-api-key>             |
     |                                |--------------------------------->|
     |                                |<--- SSE stream                   |
     |                                |                                  |
     |<--- SSE stream (passthrough)   |                                  |
```

**Production** (Nginx TLS termination in front):

```
Claude Code         Nginx (TLS)          Frigga Server          Anthropic API
     |                   |                     |                      |
     | HTTPS             | proxy_pass HTTP     |                      |
     |------------------>|-------------------->| (same flow as above) |
```

The Frigga server always binds plain HTTP. Nginx, when present, terminates TLS and proxies to the server.

**Component responsibilities:**

| Component | Role | Present in |
|-----------|------|------------|
| Nginx | TLS termination | Production only |
| Frigga Server | API consumer authorization, upstream request forwarding with credential injection (future), response relay | Both |

### 4.2 Detailed Design

#### 4.2.1 CLI Interface

The CLI entry point (`src/index.js`) starts the proxy server. Arguments use the `--key=value` form exclusively (no space-separated values), parsed by a minimal hand-written parser operating on `process.argv`.

```
node src/index.js --api-key=<key> [--port=<port>] [--host=<host>]
```

| Argument | Required | Default | Description |
|----------|----------|---------|-------------|
| `--api-key` | Yes | — | Expected value in the `Authorization: Bearer` header on incoming requests. Requests with a mismatched or missing header receive `401`. |
| `--port` | No | `3000` | HTTP listen port |
| `--host` | No | `127.0.0.1` | HTTP listen address (IPv4 only; IPv6 is not supported in the current phase) |

**Startup error handling**: If the server fails to start (e.g., `EADDRINUSE` from port conflict, `EADDRNOTAVAIL` from invalid host address), the error message is written to `stderr` and the process exits with code `1`. No retry is attempted.

#### 4.2.2 Server Endpoints

The server creates a single `http.createServer()` instance. The server registers a `checkContinue` handler to support clients that send `Expect: 100-continue`. Node.js does not emit the `request` event for such requests — it emits `checkContinue` instead. The handler calls `res.writeContinue()` to send a `100 Continue` interim response, then delegates to the same request pipeline as the `request` event. Without this handler, requests with `Expect: 100-continue` would hang indefinitely.

The server exposes exactly two endpoints, both handled by a shared request pipeline (authorization → forwarding → response relay) parameterized only by the upstream URL path:

| Path | Method | Purpose |
|------|--------|---------|
| `/v1/messages` | POST | Messages API proxy |
| `/v1/messages/count_tokens` | POST | Token counting API proxy |

**Request body handling**: The proxy treats the request body as an opaque byte stream. No JSON parsing, schema validation, or size enforcement is performed at the proxy layer — the body is forwarded to the upstream as-is via `stream.pipeline(req, upstreamReq)` when upstream forwarding is implemented (see §8 Future Work). The upstream Anthropic API enforces its own request size limits. In the current phase (501), the server responds immediately after authorization validation without consuming the request body.

**Authorization validation** (common to both endpoints):

1. Extract the `Authorization` header from the incoming request
2. Expect the format `Bearer <token>` and extract the token portion
3. Hash both the extracted token and the `--api-key` value with SHA-256 (`crypto.createHash('sha256')`), then compare the fixed-length digests via `crypto.timingSafeEqual`. Hashing normalizes both values to 32 bytes, avoiding the `RangeError` that `timingSafeEqual` throws when inputs differ in length, and prevents leaking key length information.
4. If missing, malformed, or mismatched → `401 Unauthorized`

**Current behavior** (both endpoints): After successful authorization validation, returns `501 Not Implemented` with a JSON body `{ "type": "error", "error": { "type": "not_implemented", "message": "Upstream forwarding is not yet implemented" } }`.

When upstream forwarding is implemented (see §8 Future Work), the proxy will forward requests to the corresponding Anthropic API endpoint and relay responses transparently. The primary consumer (Claude Code) uses `stream: true`, so responses are predominantly SSE streams (`text/event-stream`); non-streaming JSON responses are also supported for API compatibility. The proxy does not branch on the `stream` parameter — it pipes upstream responses through as-is.

**Error responses** (common to both endpoints):

All error responses use `Content-Type: application/json` and follow the [Anthropic API error shape](https://docs.anthropic.com/en/api/errors):

```json
{
  "type": "error",
  "error": {
    "type": "<error_type>",
    "message": "<human_readable_message>"
  }
}
```

| Status | `error.type` | Condition |
|--------|-------------|-----------|
| `401` | `authentication_error` | Missing, malformed, or invalid `Authorization` header |
| `404` | `not_found_error` | Unrecognized path |
| `405` | `invalid_request_error` | Non-POST request method. Response includes `Allow: POST` header per RFC 9110 §15.5.6 |
| `501` | `not_implemented` | Upstream forwarding not yet implemented (current phase) |

**Request body drain on error responses**: When the server sends an error response (`401`, `404`, `405`, `501`) before the client has finished transmitting the request body, the unconsumed body data could cause the client to receive a TCP RST before reading the response. To avoid this, the server calls `req.resume()` before sending any error response. This causes Node.js to read and discard the remaining request body data in the background, allowing the TCP connection to close cleanly after the response is fully sent. `req.resume()` is safe to call unconditionally — it is a no-op on an already-ended stream.

#### 4.2.3 Project Structure

```
frigga/
├── package.json
├── docs/
│   └── rfc_001_api_proxy_service.md
├── src/
│   ├── index.js
│   ├── cli.js
│   ├── constants.js
│   └── server.js
└── tests/
    ├── handler.test.js
    └── server.test.js
```

**`package.json`** — Project manifest with zero runtime `dependencies`. Sets `"type": "module"` for ES module support to match the production environment's standardized Node.js version.

**`src/index.js`** — Shebang entry point (`#!/usr/bin/env node`). Parses CLI arguments via `cli.js`, validates required arguments, and starts the server. Installs `SIGINT`/`SIGTERM` handlers for graceful shutdown.

**`src/cli.js`** — Pure-function module for CLI concerns:
- `parseArgs(argv)` → `Record<string, string | true>` — iterate `process.argv`, extract `--key=value` and `--flag` forms.
- `printUsage()` — write usage text to `stderr`.
- `resolveArgs(args)` → `{ port, host, apiKey }` — validate and extract arguments with defaults.

**`src/constants.js`** — Shared constants exported as named values:
- Network defaults: `DEFAULT_PORT` (3000), `DEFAULT_HOST` ('127.0.0.1')
- Shutdown: `SHUTDOWN_TIMEOUT_MS` (30 000)

**`src/server.js`** — Server implementation:
- `startServer({ port, host, apiKey })` — create HTTP server, register `/v1/messages` and `/v1/messages/count_tokens` routes with shared authorization validation pipeline, manage graceful shutdown. The request handler core logic is implemented as a pure function that takes request metadata (method, URL, headers) and returns response data (status code, headers, body), enabling direct unit testing without HTTP overhead.

**`tests/handler.test.js`** — Unit tests for the request handler pure function: route matching, authorization validation, method validation, error response formatting. No HTTP server started.

**`tests/server.test.js`** — Integration tests: start a real HTTP server instance per test, send requests via `http.request`, verify end-to-end HTTP behavior (status codes, headers, body content, concurrent requests, startup failures).

#### 4.2.4 Graceful Shutdown

The server implements a shutdown sequence triggered by `SIGINT` or `SIGTERM`:

1. **Stop accepting** — call `server.close()` to stop accepting new connections immediately.
2. **Close idle connections** — immediately destroy all keep-alive connections that have no in-flight request. HTTP/1.1 defaults to `Connection: keep-alive`, so idle connections will accumulate during normal operation; these must be closed proactively to avoid blocking the shutdown sequence until the drain timeout.
3. **Drain active connections** — wait for in-flight requests to complete, up to `SHUTDOWN_TIMEOUT_MS` (30 000 ms). During the drain phase, all responses include the `Connection: close` header to signal that the server will not accept further requests on the same connection. This prevents clients from sending new requests on an existing keep-alive connection after their in-flight response completes. For the current phase (no SSE streaming), all requests are short-lived and should complete well within this window.
4. **Exit** — if all connections drain within the timeout, exit with code `0`. If active connections remain after the drain timeout, destroy all remaining sockets and call `process.exit(1)`.

| Constant | Value | Description |
|----------|-------|-------------|
| `SHUTDOWN_TIMEOUT_MS` | 30 000 | Maximum time to wait for active connections to drain before forced exit |

**`Connection: close` and HTTP version compatibility**: The `Connection` header is an HTTP/1.1 hop-by-hop mechanism (RFC 9110 §7.6.1). Its behavior differs across protocol versions:

- **HTTP/1.1** — The only protocol spoken by the Frigga server (`http.createServer()`). `Connection: close` is the standard way to signal the server will close the connection after the current response. Clients that receive this header will not attempt to pipeline or reuse the connection. Safe and correct.
- **HTTP/2** — The `Connection` header is explicitly prohibited (RFC 9113 §8.2.2); compliant implementations must treat it as malformed. However, this does not affect Frigga: Node.js `http.createServer()` speaks HTTP/1.1 only. If Nginx terminates HTTP/2 from clients, it proxies to Frigga over HTTP/1.1 and handles HTTP/2 GOAWAY frames on the client-facing side independently.
- **HTTP/3** — HTTP/3 (RFC 9114) similarly prohibits the `Connection` header and uses its own GOAWAY mechanism over QUIC. Node.js has no built-in HTTP/3 support. As with HTTP/2, Nginx handles the protocol translation; Frigga always speaks HTTP/1.1 to Nginx.

Since Frigga exclusively uses `http.createServer()` (HTTP/1.1), attaching `Connection: close` during shutdown is both safe and correct. Protocol-level shutdown signaling (GOAWAY) for HTTP/2 and HTTP/3 clients is the responsibility of the reverse proxy (Nginx) when present.

#### 4.2.5 Logging

The server writes plain-text structured log lines with no runtime logging dependencies. Log output is split by severity following cloud-native conventions (12-Factor App, Kubernetes):

- **`stdout`** — INFO-level operational logs: startup, request, shutdown
- **`stderr`** — error-level output: `printUsage()` (triggered by invalid CLI arguments)

All log lines are prefixed with an ISO 8601 UTC timestamp (`new Date().toISOString()`), enabling event ordering without reliance on external timestamp injection.

**Startup log** (once, on successful listen — `stdout`):

```
[2026-03-13T10:15:30.000Z] [INFO] Frigga listening on 127.0.0.1:3000
```

**Request log** (one line per completed request — `stdout`):

```
[2026-03-13T10:15:32.100Z] [INFO] POST /v1/messages 401 2ms
[2026-03-13T10:15:33.042Z] [INFO] POST /v1/messages 501 0ms
```

Format: `[TIMESTAMP] [LEVEL] METHOD PATH STATUS DURATION`

**Shutdown log** (`stdout`):

```
[2026-03-13T10:20:00.000Z] [INFO] Shutting down: stop accepting new connections
[2026-03-13T10:20:00.050Z] [INFO] Shutdown complete
```

Sensitive values (`--api-key`, `Authorization` header values) are never included in any log output.

### 4.3 Design Rationale

**Endpoint path `/v1/messages` instead of a generic `/proxy`**: By exposing the same path as the Anthropic Messages API, the Frigga server is transparent to the API consumer. Any client SDK or tool configured to call the Anthropic API can be redirected to Frigga simply by changing the base URL — no request-wrapping or format changes required.

**Endpoint `/v1/messages/count_tokens`**: Included for complete Anthropic Messages API compatibility. Although the primary use case is message streaming, Claude Code and other API consumers may use token counting for prompt size estimation. Exposing the same path ensures the proxy is a transparent drop-in replacement for the Anthropic API base URL.

**Server-side `Authorization` validation**: Even though Nginx can enforce access control in production, the server must also validate authorization because: (a) development environments have no Nginx, (b) defense-in-depth prevents misconfigured Nginx from exposing the proxy, and (c) it keeps the access control logic explicit and testable in the application layer.

**Constant-time key comparison via SHA-256**: Both the incoming token and the `--api-key` are hashed with SHA-256 before comparison via `crypto.timingSafeEqual`. This serves two purposes: (a) `timingSafeEqual` requires equal-length inputs — hashing normalizes both values to 32 bytes, and (b) it prevents leaking the key length through a direct length check. The implementation cost is minimal and it eliminates an entire class of timing side-channel vulnerabilities.

**Separate incoming and upstream API keys**: The incoming request validation key (`--api-key`) and the upstream Anthropic API key (to be added as `--upstream-api-key` when upstream forwarding is implemented) are intentionally separate. This allows the proxy operator to issue a local API key for consumers without exposing the upstream credentials. The two keys serve different trust boundaries: the incoming key controls who can access the proxy, while the upstream key controls what the proxy can access on the upstream.

**Environment-agnostic HTTP server**: The Frigga server always binds plain HTTP. This keeps the implementation simple (no certificate management, no TLS configuration) and follows the standard reverse-proxy deployment pattern. In development, the API consumer connects directly over HTTP. In production, Nginx handles TLS and proxies to the same HTTP server.

**501 Not Implemented for current phase**: Returning 501 for the forwarding step makes the authentication layer independently testable. Consumers can verify that their Authorization header is accepted (or rejected) without needing a live Anthropic API connection. The forwarding logic can be added later without changing the external API surface.

## 5. Testing Strategy

All automated tests use the Node.js built-in test runner (`node --test`), consistent with the zero-runtime-dependency design. Tests are organized into two levels:

**Unit tests** (`tests/handler.test.js`) — pure-function tests with no HTTP overhead:

*Request handler* (`src/server.js`): The core request handling logic (routing, authorization validation, error response formatting) is extracted into a pure function that accepts request metadata `{ method, url, headers }` and returns response data `{ statusCode, headers, body }`. This enables unit testing all endpoint behavior without starting an HTTP server:
- Route matching: correct paths return expected responses, unknown paths return 404
- Authorization validation: missing, malformed, wrong key → 401; correct key → 501
- Method validation: non-POST methods → 405 with `Allow: POST` header

**Integration tests** (`tests/server.test.js`) — start a real HTTP server instance per test, send requests via `http.request`, and verify end-to-end HTTP behavior (TCP connection, chunked transfer, header serialization, concurrent request handling):

**Key Scenarios:**

| # | Scenario | Input | Expected |
|---|----------|-------|----------|
| 1 | Valid auth on `/v1/messages` | `POST /v1/messages` with correct `Authorization: Bearer <key>` | `501`, body `error.type === "not_implemented"` |
| 2 | Valid auth on `/v1/messages/count_tokens` | `POST /v1/messages/count_tokens` with correct `Authorization: Bearer <key>` | `501`, body `error.type === "not_implemented"` |
| 3 | Missing Authorization | `POST /v1/messages` without header | `401`, body `error.type === "authentication_error"` |
| 4 | Wrong Authorization | `POST /v1/messages` with wrong key | `401`, body `error.type === "authentication_error"` |
| 5 | Malformed Authorization | `POST /v1/messages` with `Authorization: <key>` (no Bearer prefix) | `401`, body `error.type === "authentication_error"` |
| 6 | GET instead of POST | `GET /v1/messages` | `405`, `Allow: POST` header present, body `error.type === "invalid_request_error"` |
| 7 | Unknown path | `POST /v1/unknown` | `404`, body `error.type === "not_found_error"` |
| 8 | Concurrent requests | 10 simultaneous `POST /v1/messages` with correct auth | All 10 receive `501`; no request blocked or dropped |
| 9 | Startup with port conflict | Start server on an already-occupied port | Process exits with code `1`, error message on stderr |
| 10 | `Expect: 100-continue` | `POST /v1/messages` with correct auth and `Expect: 100-continue` header | Server sends `100 Continue`, then `501` response |

**Manual tests:**

| # | Scenario | Method |
|---|----------|--------|
| 11 | Graceful shutdown | Send SIGINT while server is running; verify clean exit |

## 6. Implementation Plan

Unit tests are placed in Phase 2 (after project scaffolding) because Phase 1 establishes the prerequisite project structure and module stubs that tests depend on.

### Phase 1: Project Scaffolding & CLI — 0.5 day

- [ ] Create `frigga/package.json` — zero runtime dependencies, `"type": "module"`
- [ ] Implement `src/cli.js` — `parseArgs()`, `printUsage()`, `resolveArgs()`
- [ ] Implement `src/constants.js` — all shared constants (`DEFAULT_PORT`, `DEFAULT_HOST`, `SHUTDOWN_TIMEOUT_MS`)
- [ ] Implement `src/index.js` — shebang, parse args, validate required arguments, start server, register signal handlers
- [ ] Create stub `src/server.js` (`startServer()` prints config and listens)

**Done when:**
- `node src/index.js --api-key=test --port=3000` prints startup log to stdout and starts listening
- Missing or invalid arguments produce a usage message on stderr and exit code 1
- Startup failures (e.g., port already in use) produce an error message on stderr and exit code 1

### Phase 2: Tests — 0.5 day

- [ ] Define test cases based on key scenarios from Section 5
- [ ] Implement unit tests for request handler pure function (routing, authorization, error formatting) in `tests/handler.test.js`
- [ ] Implement integration tests for end-to-end HTTP behavior in `tests/server.test.js`

**Done when:** All key scenario tests written and executable

### Phase 3: Server Endpoints & Auth — 1 day

- [ ] Extract request handler as pure function: accept `{ method, url, headers }`, return `{ statusCode, headers, body }`
- [ ] Implement shared authorization validation (extract Bearer token, SHA-256 hash + `crypto.timingSafeEqual` comparison with `--api-key`)
- [ ] Implement shared request pipeline for both endpoints — validate auth, return `501 Not Implemented`
- [ ] Implement error responses in Anthropic-compatible JSON format: `401` (`authentication_error`), `404` (`not_found_error`), `405` (`invalid_request_error` with `Allow: POST` header)
- [ ] Register `checkContinue` handler: send `100 Continue` then delegate to the shared request pipeline
- [ ] Graceful shutdown on SIGINT/SIGTERM — stop accepting, drain with `SHUTDOWN_TIMEOUT_MS` (30s), exit 0 on success or exit 1 on timeout
- [ ] Request logging: INFO-level logs to stdout (startup, request, shutdown); error output to stderr (`printUsage`)

**Done when:** All unit and integration tests passing (green). Server validates `Authorization: Bearer <api-key>` on both endpoints via shared handler, returns 401 for incorrect keys, 501 for valid keys, 404 for unknown paths, and 405 (with `Allow: POST` header) for non-POST methods. Requests with `Expect: 100-continue` receive `100 Continue` before the normal response. All error responses follow the Anthropic API error shape. INFO logs appear on stdout.

## 7. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Memory pressure from large responses (when forwarding is implemented) | Med | Med | Pure streaming pipeline via `stream.pipeline` with no full-response buffering; observe `response.write()` return value for backpressure |
| Upstream API key exposure through error messages or logs (when forwarding is implemented) | Low | High | Never include upstream API keys in error responses or log output; scrub sensitive headers before logging |
| Timing attack on API key comparison | Low | Low | SHA-256 hash both values before `crypto.timingSafeEqual` comparison; normalizes length and prevents length leakage |

## 8. Future Work

- **Upstream forwarding** — Add `--upstream-api-key` CLI option; forward validated requests to the Anthropic API (`https://api.anthropic.com`) with credential injection. The upstream base URL is hardcoded — no `--upstream-base-url` option is provided. Request bodies are forwarded via `stream.pipeline(req, upstreamReq)` without buffering or size enforcement — the upstream Anthropic API enforces its own limits. The proxy relays the upstream response back to the client via `stream.pipeline(upstreamRes, res)`, regardless of response type (SSE or JSON). The proxy does not inspect or branch on the `stream` parameter or `Content-Type` — it copies the upstream response through as-is.
- **Header forwarding strategy** — Use a blacklist approach: forward all client request headers to the upstream by default, excluding a known set of proxy-layer headers (e.g., Nginx-injected headers such as `X-Forwarded-For`, `X-Real-IP`). A blacklist is preferred over a whitelist because Claude Code may add new headers at any time, and the set of headers to exclude is small and empirically determinable.
- **Client disconnect handling** — When the client disconnects mid-request (e.g., Claude Code user pressing ESC — a frequent and expected operation), the `stream.pipeline` callback fires with an error, and all streams in the pipeline are automatically destroyed, aborting the in-flight upstream request. The primary motivation is cost savings: Anthropic API charges per output token, and continuing to generate tokens for a cancelled request wastes inference compute resources. `stream.pipeline` is preferred over `.pipe()` because `.pipe()` does **not** automatically destroy the writable end (`upstreamReq`) when the readable end (`req`) is destroyed — the upstream request would continue indefinitely. `stream.pipeline` propagates errors and destruction across all streams in the chain.
- **Upstream timeout strategy** — Split into two finer-grained timeouts: connection timeout (`UPSTREAM_CONNECT_TIMEOUT_MS`, 10 000 ms) for TCP establishment, and first-byte timeout (`UPSTREAM_FIRST_BYTE_TIMEOUT_MS`, 120 000 ms) for time from request sent to first response byte. No idle timeout on the response stream — the proxy relies on the upstream to close the connection when the response is complete.
- **Double SIGINT/SIGTERM handling** — Second signal triggers immediate `process.exit(1)` instead of waiting for the drain timeout. Low priority for the current scale (< 5 concurrent connections).
- **Health check endpoint** — `GET /health` returning `200 OK` for Nginx / load balancer probes
- **Structured JSON logging** — Upgrade plain-text request logs to structured JSON for log aggregation systems
- **Rate limiting** — Application-level rate limiting to protect the upstream API key from abuse
- **Prometheus-compatible `/metrics` endpoint** — deferred until production deployment patterns are established
- **IPv6 support** — Dual-stack listening via `::` host binding

## 9. References

- [Server-Sent Events Specification](https://html.spec.whatwg.org/multipage/server-sent-events.html)
- [Anthropic Messages API](https://docs.anthropic.com/en/api/messages)
- [Anthropic Count Tokens API](https://docs.anthropic.com/en/api/counting-tokens)
- [Nginx — Reverse Proxy](https://nginx.org/en/docs/http/ngx_http_proxy_module.html)

---

## Changelog

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 2.1 | 2026-03-13 | Chason Tang | Add `Allow: POST` header to `405` responses per RFC 9110 §15.5.6; add `checkContinue` handler for `Expect: 100-continue` requests to prevent request hang; add integration test scenario #10 for `Expect: 100-continue` |
| 2.0 | 2026-03-13 | Chason Tang | Add `tests/` directory to project structure (`handler.test.js`, `server.test.js`); remove CLI unit tests from testing scope; add `Connection: close` header during graceful shutdown drain phase with HTTP/1.1, HTTP/2, HTTP/3 compatibility analysis; add ISO 8601 UTC timestamps to all log formats; add request body drain (`req.resume()`) behavior for error responses |
| 1.9 | 2026-03-13 | Chason Tang | Remove request body size limit (`MAX_REQUEST_BODY_BYTES`, `413`) — the proxy forwards request bodies via `stream.pipeline` without buffering; upstream Anthropic API enforces its own limits. Replace `.pipe()` with `stream.pipeline` in Future Work for automatic stream cleanup on client disconnect. Remove `bodyLength` from pure function signature. Update client disconnect handling to explain `stream.pipeline` vs `.pipe()` behavior |
| 1.8 | 2026-03-13 | Chason Tang | Clarify request body size enforcement mechanism (two-layer: `Content-Length` fast-reject + streaming byte counter); fix unit inconsistency (32 MB → 32 MiB); add startup error handling (`EADDRINUSE`, `EADDRNOTAVAIL`); add keep-alive idle connection cleanup to graceful shutdown; clarify request body is treated as opaque byte stream (no JSON parsing); add concurrent request and startup failure test scenarios; hardcode upstream base URL to `https://api.anthropic.com` in Future Work |
| 1.7 | 2026-03-13 | Chason Tang | Scope reduction: defer `--upstream-api-key` CLI option and Response Relay Pipeline to §8 Future Work; add shared request pipeline for both endpoints; add 32 MB request body size limit (`413`); split log output by severity (INFO→stdout, errors→stderr); extract handler as pure function for unit testability; add `/v1/messages/count_tokens` design rationale; clarify `stream: true` as primary consumer path; add exit code 0 for clean shutdown; add client disconnect business motivation, header blacklist strategy, upstream timeout strategy, and double SIGINT to Future Work |
| 1.6 | 2026-03-13 | Chason Tang | Align with updated design doc template: reorder Testing Strategy (§5) before Implementation Plan (§6); add Unit Tests phase (Phase 2) to Implementation Plan with test-first guidance; renumber subsequent phases |
| 1.5 | 2026-03-12 | Chason Tang | Support both SSE and JSON response relay — the proxy transparently pipes upstream responses regardless of content type; update §4.1 diagrams, §4.2.2 endpoint descriptions, rename §4.2.3 to "Response Relay Pipeline" with dual-mode coverage; update implementation plan, risks, and future work accordingly |
| 1.4 | 2026-03-12 | Chason Tang | Fix `timingSafeEqual` to use SHA-256 hashing; align error responses with Anthropic API error shape; add graceful shutdown details (§4.2.5); add logging specification (§4.2.6); refine SSE timeout strategy with connect/first-byte split; add client disconnect handling; clarify IPv4-only listening; introduce automated integration tests |
| 1.3 | 2026-03-12 | Chason Tang | Remove WebSocket relay architecture and client mode; simplify to HTTP proxy service; add `/v1/messages/count_tokens` endpoint; separate `--api-key` (incoming auth) and `--upstream-api-key` (upstream auth); current phase returns 501 for upstream forwarding |
| 1.2 | 2026-03-11 | Chason Tang | Merge `/register` + `/ws` into single `/ws` endpoint with shared-secret authentication; add `--client-secret` CLI option to both modes; eliminate token lifecycle; add `crypto.timingSafeEqual` for all secret comparisons; simplify client lifecycle (remove registration step) |
| 1.1 | 2026-03-11 | Chason Tang | Rename `/forward` to `/v1/messages`; add server-side Authorization validation via `--api-key`; remove `clientId` (single-client simplification); add dev/prod deployment modes; add §4.2.7 Project Structure; restructure CLI arguments |
| 1.0 | 2026-03-11 | Chason Tang | Initial version |
