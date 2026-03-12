# RFC-001: Messages API Proxy Service

**Version:** 1.4  
**Author:** Chason Tang  
**Date:** 2026-03-12  
**Status:** Proposed

---

## 1. Summary

Frigga is a zero-runtime-dependency Node.js CLI proxy service that exposes `/v1/messages` and `/v1/messages/count_tokens` HTTP POST endpoints compatible with the Anthropic Messages API. The service validates incoming requests via `Authorization` header comparison against a startup-configured API key. In the future, validated requests will be forwarded to the Anthropic API with a separate upstream API key injected into the `Authorization` header; the current phase returns `501 Not Implemented` for the forwarding step. In development, the service runs over plain HTTP; in production, Nginx sits in front and provides TLS termination (HTTPS).

## 2. Motivation

In scenarios where an API consumer (e.g., Claude Code) needs to access the Anthropic Messages API but the required authentication credentials must remain within a controlled environment (e.g., a private network, a developer's local machine), a proxy service is needed to decouple the consumer-facing access control from the upstream credential management.

A dedicated proxy service with a focused scope provides a minimal attack surface, zero configuration complexity, and full control over credential injection and streaming behavior.

## 3. Goals and Non-Goals

**Goals:**
- Single CLI entry point that starts an HTTP proxy server
- `/v1/messages` and `/v1/messages/count_tokens` POST endpoints compatible with the Anthropic Messages API path
- Incoming request validation via `--api-key` matched against the `Authorization: Bearer` header
- Upstream forwarding (future) with a separate `--upstream-api-key` injected into the `Authorization` header for Anthropic API requests
- Zero runtime dependencies — all functionality implemented using Node.js built-in modules (`http`, `https`, `crypto`, `url`)
- Environment-agnostic: run over plain HTTP in development; production TLS (HTTPS) is handled entirely by Nginx with no code change
- Transparent SSE passthrough with chunked streaming when upstream forwarding is implemented (no full-response buffering)

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

**Future** (upstream forwarding implemented):

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
| Frigga Server | API consumer authorization, upstream request forwarding with credential injection, SSE response streaming | Both |

### 4.2 Detailed Design

#### 4.2.1 CLI Interface

The CLI entry point (`src/index.js`) starts the proxy server. Arguments use the `--key=value` form exclusively (no space-separated values), parsed by a minimal hand-written parser operating on `process.argv`.

```
node src/index.js --api-key=<key> [--upstream-api-key=<key>] [--port=<port>] [--host=<host>]
```

| Argument | Required | Default | Description |
|----------|----------|---------|-------------|
| `--api-key` | Yes | — | Expected value in the `Authorization: Bearer` header on incoming requests. Requests with a mismatched or missing header receive `401`. |
| `--upstream-api-key` | No | — | API key injected as `Authorization: Bearer <upstream-api-key>` into upstream Anthropic API requests. Required when upstream forwarding is implemented. |
| `--port` | No | `3000` | HTTP listen port |
| `--host` | No | `0.0.0.0` | HTTP listen address (IPv4 only; IPv6 is not supported in the current phase) |

#### 4.2.2 Server Endpoints

The server creates a single `http.createServer()` instance. The server exposes exactly two endpoints:

| Path | Method | Purpose |
|------|--------|---------|
| `/v1/messages` | POST | Messages API proxy |
| `/v1/messages/count_tokens` | POST | Token counting API proxy |

**Authorization validation** (common to both endpoints):

1. Extract the `Authorization` header from the incoming request
2. Expect the format `Bearer <token>` and extract the token portion
3. Hash both the extracted token and the `--api-key` value with SHA-256 (`crypto.createHash('sha256')`), then compare the fixed-length digests via `crypto.timingSafeEqual`. Hashing normalizes both values to 32 bytes, avoiding the `RangeError` that `timingSafeEqual` throws when inputs differ in length, and prevents leaking key length information.
4. If missing, malformed, or mismatched → `401 Unauthorized`

**`POST /v1/messages`**

From the API consumer's perspective, this endpoint will behave identically to the Anthropic Messages API once upstream forwarding is implemented. The consumer sends a standard request and receives an SSE stream in return.

Current behavior: After successful authorization validation, returns `501 Not Implemented` with a JSON body `{ "type": "error", "error": { "type": "not_implemented", "message": "Upstream forwarding is not yet implemented" } }`.

Future behavior:
1. Forward the request body and relevant headers to `https://api.anthropic.com/v1/messages`
2. Inject `Authorization: Bearer <upstream-api-key>` header (replacing any existing value)
3. Stream the upstream SSE response back to the caller transparently (`Content-Type: text/event-stream`)

**`POST /v1/messages/count_tokens`**

Follows the same pattern as `/v1/messages` — identical authorization validation, currently returns `501 Not Implemented`.

Future behavior: Forward to `https://api.anthropic.com/v1/messages/count_tokens` with the upstream API key injected.

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
| `405` | `invalid_request_error` | Non-POST request method |
| `501` | `not_implemented` | Upstream forwarding not yet implemented (current phase) |

#### 4.2.3 Future: SSE Relay Pipeline

When upstream forwarding is implemented, the SSE relay will avoid buffering by operating as a streaming pipeline:

```
Claude Code              Frigga Server              Anthropic API
     |                        |                          |
     |  POST /v1/messages     |                          |
     |----------------------->|                          |
     |                        | POST /v1/messages        |
     |                        |  + upstream auth         |
     |                        |------------------------->|
     |                        |                          |
     |                        |<-- chunk 1 --------------|
     |<-- res.write(chunk 1)  |                          |
     |                        |<-- chunk 2 --------------|
     |<-- res.write(chunk 2)  |                          |
     |                        |<-- END ------------------|
     |<-- res.end()           |                          |
```

Each data chunk from the upstream response is written directly to the client response using `response.write()`. No intermediate buffering occurs — data flows through the proxy as fast as the slowest link allows.

**Timeout strategy** (future, when upstream forwarding is implemented):

| Timeout | Constant | Value | Description |
|---------|----------|-------|-------------|
| Connection timeout | `UPSTREAM_CONNECT_TIMEOUT_MS` | 10 000 | Maximum time to establish a TCP connection with the upstream |
| First-byte timeout | `UPSTREAM_FIRST_BYTE_TIMEOUT_MS` | 120 000 | Maximum time from request sent to first response byte received |

The existing `FORWARD_TIMEOUT_MS` constant is replaced by the two finer-grained timeouts above. No idle timeout is applied to the SSE stream itself — the proxy relies on the upstream to close the connection when the response is complete.

**Client disconnect handling**: The proxy listens for the `close` event on the incoming request (`req.on('close')`). If the client disconnects mid-stream, the proxy immediately aborts the in-flight upstream request (`upstreamReq.destroy()`) to release resources. This prevents orphaned upstream connections from accumulating.

#### 4.2.4 Project Structure

```
frigga/
├── package.json
├── docs/
│   └── rfc_001_api_proxy_service.md
└── src/
    ├── index.js
    ├── cli.js
    ├── constants.js
    └── server.js
```

**`package.json`** — Project manifest with zero runtime `dependencies`. Sets `"type": "module"` for ES module support and `"engines": { "node": ">=24.0.0" }` to match the production environment's standardized Node.js version.

**`src/index.js`** — Shebang entry point (`#!/usr/bin/env node`). Parses CLI arguments via `cli.js`, validates required arguments, and starts the server. Installs `SIGINT`/`SIGTERM` handlers for graceful shutdown.

**`src/cli.js`** — Pure-function module for CLI concerns:
- `parseArgs(argv)` → `Record<string, string | true>` — iterate `process.argv`, extract `--key=value` and `--flag` forms.
- `printUsage()` — write usage text to `stderr`.
- `resolveArgs(args)` → `{ port, host, apiKey, upstreamApiKey }` — validate and extract arguments with defaults.

**`src/constants.js`** — Shared constants exported as named values:
- Network defaults: `DEFAULT_PORT` (3000), `DEFAULT_HOST` ('0.0.0.0')
- Upstream timeouts (future): `UPSTREAM_CONNECT_TIMEOUT_MS` (10 000), `UPSTREAM_FIRST_BYTE_TIMEOUT_MS` (120 000)
- Shutdown: `SHUTDOWN_TIMEOUT_MS` (30 000)

**`src/server.js`** — Server implementation:
- `startServer({ port, host, apiKey, upstreamApiKey })` — create HTTP server, register `/v1/messages` and `/v1/messages/count_tokens` routes with authorization validation, manage graceful shutdown.

#### 4.2.5 Graceful Shutdown

The server implements a two-phase shutdown sequence triggered by `SIGINT` or `SIGTERM`:

1. **Stop accepting** — call `server.close()` to stop accepting new connections immediately.
2. **Drain active connections** — wait for in-flight requests to complete, up to `SHUTDOWN_TIMEOUT_MS` (30 000 ms). For the current phase (no SSE streaming), all requests are short-lived and should complete well within this window.
3. **Force exit** — if active connections remain after the drain timeout, destroy all remaining sockets and call `process.exit(1)`.

Future consideration: when SSE streaming is implemented, in-flight SSE streams will be terminated at the drain timeout boundary. The proxy will not wait indefinitely for long-running streams to complete.

| Constant | Value | Description |
|----------|-------|-------------|
| `SHUTDOWN_TIMEOUT_MS` | 30 000 | Maximum time to wait for active connections to drain before forced exit |

#### 4.2.6 Logging

The server writes structured log lines to `stderr` (keeping `stdout` clean for potential future piping). All log output is plain text, with no runtime logging dependencies.

**Startup log** (once, on successful listen):

```
[INFO] Frigga listening on 0.0.0.0:3000
```

**Request log** (one line per completed request):

```
[INFO] POST /v1/messages 401 2ms
[INFO] POST /v1/messages 501 0ms
```

Format: `[LEVEL] METHOD PATH STATUS DURATION`

**Shutdown log**:

```
[INFO] Shutting down: stop accepting new connections
[INFO] Shutdown complete
```

Sensitive values (`--api-key`, `--upstream-api-key`, `Authorization` header values) are never included in any log output.

### 4.3 Design Rationale

**Endpoint path `/v1/messages` instead of a generic `/proxy`**: By exposing the same path as the Anthropic Messages API, the Frigga server is transparent to the API consumer. Any client SDK or tool configured to call the Anthropic API can be redirected to Frigga simply by changing the base URL — no request-wrapping or format changes required.

**Server-side `Authorization` validation**: Even though Nginx can enforce access control in production, the server must also validate authorization because: (a) development environments have no Nginx, (b) defense-in-depth prevents misconfigured Nginx from exposing the proxy, and (c) it keeps the access control logic explicit and testable in the application layer.

**Constant-time key comparison via SHA-256**: Both the incoming token and the `--api-key` are hashed with SHA-256 before comparison via `crypto.timingSafeEqual`. This serves two purposes: (a) `timingSafeEqual` requires equal-length inputs — hashing normalizes both values to 32 bytes, and (b) it prevents leaking the key length through a direct length check. The implementation cost is minimal and it eliminates an entire class of timing side-channel vulnerabilities.

**Separate `--api-key` and `--upstream-api-key`**: The incoming request validation key and the upstream Anthropic API key are intentionally separate. This allows the proxy operator to issue a local API key for consumers without exposing the upstream credentials. The two keys serve different trust boundaries: `--api-key` controls who can access the proxy, while `--upstream-api-key` controls what the proxy can access on the upstream.

**Environment-agnostic HTTP server**: The Frigga server always binds plain HTTP. This keeps the implementation simple (no certificate management, no TLS configuration) and follows the standard reverse-proxy deployment pattern. In development, the API consumer connects directly over HTTP. In production, Nginx handles TLS and proxies to the same HTTP server.

**501 Not Implemented for current phase**: Returning 501 for the forwarding step makes the authentication layer independently testable. Consumers can verify that their Authorization header is accepted (or rejected) without needing a live Anthropic API connection. The forwarding logic can be added later without changing the external API surface.

## 5. Implementation Plan

### Phase 1: Project Scaffolding & CLI — 0.5 day

- [ ] Create `frigga/package.json` — zero runtime dependencies, `"type": "module"`, `"engines": { "node": ">=24.0.0" }`
- [ ] Implement `src/cli.js` — `parseArgs()`, `printUsage()`, `resolveArgs()`
- [ ] Implement `src/constants.js` — all shared constants
- [ ] Implement `src/index.js` — shebang, parse args, validate required arguments, start server, register signal handlers
- [ ] Create stub `src/server.js` (`startServer()` prints config and listens)

**Done when:**
- `node src/index.js --api-key=test --port=3000` prints server config to stderr and starts listening
- Missing or invalid arguments produce a usage message and exit code 1

### Phase 2: Server Endpoints & Auth — 1 day

- [ ] Implement authorization validation (extract Bearer token, SHA-256 hash + `crypto.timingSafeEqual` comparison with `--api-key`)
- [ ] Implement `POST /v1/messages` — validate auth, return `501 Not Implemented`
- [ ] Implement `POST /v1/messages/count_tokens` — validate auth, return `501 Not Implemented`
- [ ] Implement error responses in Anthropic-compatible JSON format: `401` (`authentication_error`), `404` (`not_found_error`), `405` (`invalid_request_error`)
- [ ] Graceful shutdown on SIGINT/SIGTERM — stop accepting, drain with `SHUTDOWN_TIMEOUT_MS` (30s), force exit
- [ ] Request logging to stderr: method, path, status code, duration per request

**Done when:** Server validates `Authorization: Bearer <api-key>` on both endpoints, returns 401 for incorrect keys, 501 for valid keys, 404 for unknown paths, and 405 for non-POST methods. All error responses follow the Anthropic API error shape. Request logs appear on stderr.

### Phase 3: Upstream Forwarding (Future)

- [ ] Implement request forwarding to Anthropic API with `--upstream-api-key` injection
- [ ] Implement SSE streaming passthrough for `/v1/messages`
- [ ] Implement JSON response passthrough for `/v1/messages/count_tokens`
- [ ] Upstream timeout handling (`UPSTREAM_CONNECT_TIMEOUT_MS` 10s, `UPSTREAM_FIRST_BYTE_TIMEOUT_MS` 120s)
- [ ] Client disconnect detection — abort upstream request on `req.close`

**Done when:** Full end-to-end flow: Claude Code → `POST /v1/messages` on Frigga → Anthropic API → SSE streams back through proxy → Claude Code receives SSE. Tested against a real Anthropic API.

## 6. Testing Strategy

All automated tests use the Node.js built-in test runner (`node --test`), consistent with the zero-runtime-dependency design. Tests are organized into two levels:

**Unit tests** — pure-function tests for the CLI argument parser (`src/cli.js`):
- `parseArgs()` correctly extracts `--key=value` and `--flag` forms
- `resolveArgs()` applies defaults and validates required arguments
- Edge cases: empty argv, duplicate keys, missing `=` separator

**Integration tests** — start a real HTTP server instance per test, send requests via `http.request`, and assert status codes and response bodies:

| # | Scenario | Input | Expected |
|---|----------|-------|----------|
| 1 | Valid auth on `/v1/messages` | `POST /v1/messages` with correct `Authorization: Bearer <key>` | `501`, body `error.type === "not_implemented"` |
| 2 | Valid auth on `/v1/messages/count_tokens` | `POST /v1/messages/count_tokens` with correct `Authorization: Bearer <key>` | `501`, body `error.type === "not_implemented"` |
| 3 | Missing Authorization | `POST /v1/messages` without header | `401`, body `error.type === "authentication_error"` |
| 4 | Wrong Authorization | `POST /v1/messages` with wrong key | `401`, body `error.type === "authentication_error"` |
| 5 | Malformed Authorization | `POST /v1/messages` with `Authorization: <key>` (no Bearer prefix) | `401`, body `error.type === "authentication_error"` |
| 6 | GET instead of POST | `GET /v1/messages` | `405`, body `error.type === "invalid_request_error"` |
| 7 | Unknown path | `POST /v1/unknown` | `404`, body `error.type === "not_found_error"` |

**Manual tests:**

| # | Scenario | Method |
|---|----------|--------|
| 8 | Graceful shutdown | Send SIGINT while server is running; verify clean exit |

## 7. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Memory pressure from large SSE responses (when forwarding is implemented) | Med | Med | Pure streaming pipeline with no full-response buffering; observe `response.write()` return value for backpressure |
| Upstream API key exposure through error messages or logs | Low | High | Never include `--upstream-api-key` in error responses or log output; scrub sensitive headers before logging |
| Timing attack on API key comparison | Low | Low | SHA-256 hash both values before `crypto.timingSafeEqual` comparison; normalizes length and prevents length leakage |

## 8. Future Work

- **Upstream forwarding** — Forward validated requests to the Anthropic API with `--upstream-api-key` injection; stream SSE responses back transparently
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
| 1.4 | 2026-03-12 | Chason Tang | Fix `timingSafeEqual` to use SHA-256 hashing; align error responses with Anthropic API error shape; add graceful shutdown details (§4.2.5); add logging specification (§4.2.6); refine SSE timeout strategy with connect/first-byte split; add client disconnect handling; update Node.js target to >=24.0.0; clarify IPv4-only listening; introduce automated integration tests |
| 1.3 | 2026-03-12 | Chason Tang | Remove WebSocket relay architecture and client mode; simplify to HTTP proxy service; add `/v1/messages/count_tokens` endpoint; separate `--api-key` (incoming auth) and `--upstream-api-key` (upstream auth); current phase returns 501 for upstream forwarding |
| 1.2 | 2026-03-11 | Chason Tang | Merge `/register` + `/ws` into single `/ws` endpoint with shared-secret authentication; add `--client-secret` CLI option to both modes; eliminate token lifecycle; add `crypto.timingSafeEqual` for all secret comparisons; simplify client lifecycle (remove registration step) |
| 1.1 | 2026-03-11 | Chason Tang | Rename `/forward` to `/v1/messages`; add server-side Authorization validation via `--api-key`; remove `clientId` (single-client simplification); add dev/prod deployment modes; add §4.2.7 Project Structure; restructure CLI arguments |
| 1.0 | 2026-03-11 | Chason Tang | Initial version |
