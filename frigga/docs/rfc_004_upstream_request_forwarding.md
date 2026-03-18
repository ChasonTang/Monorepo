# RFC-004: Upstream Request Forwarding

**Version:** 2.6
**Author:** Chason Tang
**Date:** 2026-03-18
**Status:** Implemented

---

## 1. Summary

This proposal replaces the current `501 Not Implemented` stub with a transparent upstream forwarding pipeline. Client requests that pass route and method validation are forwarded to the Anthropic API (`https://api.anthropic.com`) with whitelist-based header filtering. The request body is streamed to the upstream in real-time via `req.pipe(upstreamReq)`, with a sidecar `data` event listener that captures the body for audit logging without adding latency to the forwarding path. Upstream responses are piped back to the client transparently. Client disconnects (e.g., Claude Code user pressing ESC) abort the upstream request immediately via `req.unpipe()` + `upstreamReq.destroy()` to stop upstream inference and conserve token usage. A `createRequestLogEmitter` factory with `emitted` and `finalized` dual guards ensures exactly-once NDJSON audit logging across all termination paths — normal completion, upstream connection error, client request stream error, and client disconnect. Authorization validation and credential injection are deferred to a future RFC — the client's `Authorization` header is forwarded as-is. The design operates identically in both development (direct HTTP) and production (behind Nginx with TLS termination).

## 2. Motivation

Frigga currently validates incoming requests but returns `501 Not Implemented` for all valid requests (RFC-001 §4.2.2). This makes the service unusable as an actual AI gateway. Implementing upstream forwarding is the core remaining functionality that completes frigga's purpose as an enterprise AI review gateway.

Enterprise deployment introduces specific requirements not present in a basic proxy:

- **Header security**: Client requests may carry headers that should not reach the upstream (e.g., `anthropic-dangerous-direct-browser-access`, SDK telemetry headers like `x-stainless-*`). A whitelist approach ensures only explicitly approved headers are forwarded, providing defense-in-depth against header injection and information leakage.
- **Audit logging**: The request body must be captured for audit review. A sidecar `data` event listener captures the body alongside the streaming pipeline, ensuring audit capability without adding latency to the forwarding path — employees experience the same responsiveness as a direct API connection.

## 3. Goals and Non-Goals

**Goals:**

- Forward validated requests to `https://api.anthropic.com` with whitelist-based header filtering
- Stream request body to upstream in real-time via `req.pipe(upstreamReq)` — no full buffering before forwarding
- Capture request body via sidecar `data` event listener for audit logging (RFC-003 format)
- Forward the client's `Authorization` header as-is to the upstream
- Transparent response proxying via stream piping — no content-type branching or response body transformation
- Correct behavior in both direct HTTP (development) and behind Nginx (production) deployments
- Abort upstream requests on client disconnect (e.g., ESC in Claude Code) via `req.unpipe()` + `upstreamReq.destroy()` — stop upstream inference and conserve token usage
- Exactly-once NDJSON audit logging across all termination paths (normal completion, upstream error, client error, client disconnect) via a centralized `emitRequestLog` function with a `requestLogEmitted` guard

**Non-Goals:**

- Authorization validation or credential injection — deferred to a future RFC; the client's `Authorization` header is forwarded as-is
- Configurable upstream URL — the upstream is fixed to `https://api.anthropic.com`
- Response body auditing or logging — only request body auditing is required in the current phase
- Request or response body transformation (e.g., rewriting, redacting) — the proxy is transparent at the body level
- Upstream load balancing or failover across multiple endpoints — a single upstream URL is sufficient
- Response compression or decompression — `accept-encoding` is forwarded and compressed responses are piped through as-is; Nginx does not re-compress (see §4.3)

## 4. Design

### 4.1 Overview

```
Development:

  Client                          Frigga                        Anthropic API
    |                               |                                |
    | POST /v1/messages             |                                |
    | Authorization: Bearer <key>   |                                |
    |------------------------------>|                                |
    |                               | 1. Validate (route/method)     |
    |                               | 2. Build upstream request      |
    |                               |    - Whitelist headers         |
    |                               |    - Forward Authorization     |
    |                               |                                |
    |                               | POST /v1/messages              |
    |                               | Authorization: Bearer <key>    |
    |                               |   (forwarded from client)      |
    |                               |------------------------------->|
    |                               |   req.pipe(upstreamReq)        |
    |                               |   + sidecar data capture       |
    |                               |                                |
    |                               |<--- response (JSON or SSE) ----|
    |                               |                                |
    |<--- response (pipe through) --|                                |
    |                               | 3. Log (NDJSON per RFC-003)    |

Production:

  Client          Nginx (TLS)           Frigga                  Anthropic API
    |                 |                    |                          |
    | HTTPS           | proxy_pass HTTP    |                          |
    |---------------->|------------------->| (same flow as above)     |
    |                 |                    |                          |
    |<--- response ---|<--- response ------|                          |
```

The request body is streamed through in real-time via `req.pipe(upstreamReq)`. A sidecar `data` event listener on `req` captures the body for audit logging without blocking the pipeline. This design prioritizes employee responsiveness — the upstream request starts receiving data as soon as the client sends it, with no buffering delay. The upstream response body is also streamed (piped) without buffering.

### 4.2 Detailed Design

#### 4.2.1 CLI Configuration Changes

The existing `--api-key` argument is removed. Authorization validation and credential injection are deferred to a future RFC.

**CLI — Before (RFC-001):**

```
node src/index.js --api-key=<key> [--port=<port>] [--host=<host>]
```

**CLI — After:**

```
node src/index.js [--port=<port>] [--host=<host>]
```

No new CLI arguments are introduced. The upstream URL is hardcoded to `https://api.anthropic.com`. Both `--port` and `--host` retain their existing defaults (`3000` and `127.0.0.1` respectively).

`resolveArgs()` in `src/cli.js` is simplified to remove `--api-key` parsing and validation. The function no longer requires any arguments — both remaining arguments have defaults.

A new constant is added to `src/constants.js`:

```javascript
export const UPSTREAM_BASE_URL = "https://api.anthropic.com";
```

#### 4.2.2 Request Header Whitelist

Outbound request headers are constructed using a strict whitelist. Only headers listed below are included in the upstream request; all others are dropped.

| Client Header       | Upstream Action         | Rationale                                                                                                                                                                                                                                                                                            |
| ------------------- | ----------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `content-type`      | Forward as-is           | Required by the upstream API (`application/json`).                                                                                                                                                                                                                                                   |
| `accept`            | Forward as-is           | Signals expected response format.                                                                                                                                                                                                                                                                    |
| `accept-encoding`   | Forward as-is           | Forwarded to upstream to enable response compression. Nginx does not re-compress responses that already carry `Content-Encoding`. See §4.3 for rationale.                                                                                                                                            |
| `user-agent`        | Forward as-is           | Identifies the client for upstream analytics.                                                                                                                                                                                                                                                        |
| `anthropic-beta`    | Forward as-is           | Required per LLM gateway specification — enables beta features.                                                                                                                                                                                                                                      |
| `anthropic-version` | Forward as-is           | Required per LLM gateway specification — specifies API version.                                                                                                                                                                                                                                      |
| `authorization`     | Forward as-is           | Client's API key forwarded directly to the upstream — no credential injection in this phase.                                                                                                                                                                                                         |
| `content-length`    | Forward as-is           | Forwarded from the client when present. When set, `node:https` uses fixed-length transfer (not chunked) — since `req.pipe()` forwards the identical body bytes, the value remains accurate. Absent for chunked-encoded requests, in which case Node.js uses chunked transfer encoding automatically. |
| `host`              | **Set** by `node:https` | Automatically set to upstream hostname (`api.anthropic.com`) from the request URL. Not included in `buildUpstreamHeaders`.                                                                                                                                                                           |

**Headers explicitly NOT forwarded (representative examples, not exhaustive — the whitelist above is the authoritative definition):**

| Header                                                                              | Reason                                                                                                                     |
| ----------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------- |
| `x-stainless-*`                                                                     | SDK telemetry — not required by the upstream API.                                                                          |
| `anthropic-dangerous-direct-browser-access`                                         | SDK internal safety bypass — must not propagate to the upstream.                                                           |
| `x-app`                                                                             | Client telemetry — not required by the upstream API.                                                                       |
| `connection`                                                                        | Hop-by-hop header (RFC 9110 §7.6.1) — pertains to the client-frigga connection, not the frigga-upstream connection.        |
| `transfer-encoding`                                                                 | Hop-by-hop header — Node.js HTTP client handles transfer encoding automatically based on the presence of `content-length`. |
| `host` (original)                                                                   | Replaced by `node:https` with the upstream hostname.                                                                       |
| `x-forwarded-*`, `x-real-ip`                                                        | Nginx-injected headers — pertain to the client-nginx-frigga chain, not the frigga-upstream connection.                     |
| `keep-alive`, `proxy-authorization`, `proxy-connection`, `te`, `trailer`, `upgrade` | Hop-by-hop headers per RFC 9110.                                                                                           |

Implementation as a pure function in `src/server.js`:

```javascript
const REQUEST_HEADER_FORWARD = new Set([
  "content-type",
  "accept",
  "accept-encoding",
  "user-agent",
  "anthropic-beta",
  "anthropic-version",
  "authorization",
  "content-length",
]);

/**
 * Build upstream request headers from the client request headers.
 * @param {Record<string, string>} incomingHeaders - req.headers (lower-cased by Node.js)
 * @returns {Record<string, string>}
 */
export function buildUpstreamHeaders(incomingHeaders) {
  const headers = {};
  for (const name of REQUEST_HEADER_FORWARD) {
    if (incomingHeaders[name] !== undefined) {
      headers[name] = incomingHeaders[name];
    }
  }
  return headers;
}
```

`host` is not set in this function — `node:https` sets it automatically from the request URL.

#### 4.2.3 Request Callback Control Flow and Body Streaming

The `startServer` request callback is restructured into two code paths, replacing the current RFC-003 pattern where `req.on("end")` collects the full body before calling `handleRequest`:

1. **Route/method validation**: Call `handleRequest(req, isShuttingDown)` immediately — before registering any `data` event listeners. This function inspects only `req.method` and `req.url`; it does not need the request body.
2. **Local error response** (`handleRequest` returns non-`null`): Write the error response (404, 405, etc.), then register a `data` event listener to capture the request body for audit logging and an `emitLocalLog` helper that both `error` and `end` handlers delegate to. A `localLogEmitted` boolean guard ensures exactly-once log emission — if `error` fires (preventing `end`), the log is still emitted with the partially captured body; if `end` fires normally, the `error` handler is a no-op. The `data` listener puts the stream into flowing mode, replacing the need for an explicit `req.resume()`. Draining is essential on keep-alive connections — unconsumed body data blocks subsequent requests on the same socket. Body capture on local error paths ensures audit completeness — every request that reaches the request callback produces a request log entry with `request_body`, regardless of validation outcome.
3. **Upstream forwarding** (`handleRequest` returns `null`): Initialize forwarding state (`upstreamResponseReceived`, `requestChunks`), register the sidecar `data` event listener for audit capture, create the `emitRequestLog` function via `createRequestLogEmitter()`, register `req.on("error")` for client stream error handling (§4.2.7), register `res.on("close")` for client disconnect detection (§4.2.7), then construct the upstream request and pipe the body via `req.pipe(upstreamReq)`.

**Local error path:**

```javascript
const { statusCode, headers, body } = result;
res.writeHead(statusCode, headers);
res.end(body);

const requestChunks = [];
let localLogEmitted = false;

const emitLocalLog = () => {
  if (localLogEmitted) return;
  localLogEmitted = true;
  const requestBody = Buffer.concat(requestChunks).toString("utf-8");
  requestChunks.length = 0;
  process.stdout.write(
    `${JSON.stringify({
      timestamp: new Date().toISOString(),
      level: "INFO",
      event: "request",
      method: req.method,
      url: req.url,
      status: statusCode,
      duration_ms: Date.now() - startTime,
      request_headers: req.headers,
      request_body: requestBody,
    })}\n`,
  );
};

req.on("data", (chunk) => requestChunks.push(chunk));
req.on("error", emitLocalLog);
req.on("end", emitLocalLog);
```

The forwarding path uses two exported helper functions in `src/server.js`, shared across all termination handlers. Extracting these as standalone functions (rather than closures) enables direct unit testing with mock objects — see §7 scenarios #10–#14.

- `abortUpstream(req, upstreamReq)` — disconnects the pipe via `req.unpipe(upstreamReq)` before calling `upstreamReq.destroy()`, preventing write-after-destroy race conditions (see §4.3 for rationale). No-op when `upstreamReq` is `undefined` or already destroyed.
- `createRequestLogEmitter({ req, requestChunks, startTime })` — factory that returns an `emitRequestLog(status, responseHeaders?)` function with an exactly-once guard (`emitted`). The first call produces the NDJSON audit log; subsequent calls are no-ops. If `req` has not finished streaming (`!req.readableEnded && !req.destroyed`), finalization is deferred to the `req` `end` or `error` event to capture the maximum request body. A nested `finalized` guard inside the deferred path prevents double execution if both `end` and `error` fire for the same `req` (see §4.3 for rationale).

**Exported helper functions** (`src/server.js`):

```javascript
/**
 * Abort the upstream request by disconnecting the pipe first.
 * @param {import("node:http").IncomingMessage} req
 * @param {import("node:http").ClientRequest} upstreamReq
 */
export function abortUpstream(req, upstreamReq) {
  if (upstreamReq && !upstreamReq.destroyed) {
    req.unpipe(upstreamReq);
    upstreamReq.destroy();
  }
}

/**
 * Create a request log emitter with exactly-once guard.
 * @param {object} ctx
 * @param {import("node:http").IncomingMessage} ctx.req
 * @param {Buffer[]} ctx.requestChunks - sidecar-captured body chunks
 * @param {number} ctx.startTime - Date.now() at request start
 * @returns {(status: number, responseHeaders?: Record<string, string>) => void}
 */
export function createRequestLogEmitter({ req, requestChunks, startTime }) {
  let emitted = false;

  return function emitRequestLog(status, responseHeaders) {
    if (emitted) return;
    emitted = true;

    let finalized = false;
    const finalize = () => {
      if (finalized) return;
      finalized = true;
      const requestBody = Buffer.concat(requestChunks).toString("utf-8");
      requestChunks.length = 0;
      const entry = {
        timestamp: new Date().toISOString(),
        level: "INFO",
        event: "request",
        method: req.method,
        url: req.url,
        status,
        duration_ms: Date.now() - startTime,
        request_headers: req.headers,
        request_body: requestBody,
      };
      if (responseHeaders) entry.response_headers = responseHeaders;
      process.stdout.write(JSON.stringify(entry) + "\n");
    };

    if (req.readableEnded || req.destroyed) {
      finalize();
    } else {
      req.on("end", finalize);
      req.on("error", () => finalize());
      req.resume();
    }
  };
}
```

**Forwarding path — state and upstream request construction:**

```javascript
import https from "node:https";

// ── Forwarding path state ────────────────────────────
let upstreamReq;
let upstreamResponseReceived = false;
const requestChunks = [];

// Sidecar: capture request body for audit logging
req.on("data", (chunk) => requestChunks.push(chunk));

// ── Create request log emitter ───────────────────────
const emitRequestLog = createRequestLogEmitter({
  req,
  requestChunks,
  startTime,
});

// ── Event handlers (§4.2.7) ──────────────────────────
req.on("error", (err) => {
  /* abortUpstream(req, upstreamReq), stderr client_error, emitRequestLog(499) */
});
res.on("close", () => {
  /* abortUpstream(req, upstreamReq) if !writableFinished, emitRequestLog(499) if Phase A */
});

// ── Upstream request ─────────────────────────────────
const upstreamUrl = new URL(req.url, UPSTREAM_BASE_URL);
const upstreamHeaders = buildUpstreamHeaders(req.headers);

upstreamReq = https.request(
  upstreamUrl,
  { method: "POST", headers: upstreamHeaders },
  (upstreamRes) => {
    upstreamResponseReceived = true;
    // Response proxying — see §4.2.4
  },
);

// Upstream connection error — see §4.2.6
upstreamReq.on("error", (err) => {
  /* stderr upstream_error, 502, emitRequestLog(502) */
});

req.pipe(upstreamReq);
```

**URL construction safety**: `new URL(req.url, UPSTREAM_BASE_URL)` constructs the upstream URL from the client's request path. This is safe because: (a) `handleRequest` validates the route _before_ this code executes — only `/v1/messages` passes through; (b) `new URL` normalizes path traversal sequences (e.g., `/../` is resolved); (c) query strings are preserved intentionally to support API parameters. The route validation provides defense-in-depth against URL manipulation.

The `data` event listener and `req.pipe()` coexist because Node.js `EventEmitter` dispatches `data` events to all registered listeners. `pipe()` internally adds its own `data` listener that writes each chunk to `upstreamReq`; the sidecar listener receives the same chunks independently for audit capture. Neither blocks the other.

`req.pipe(upstreamReq)` is preferred over `pipeline(req, upstreamReq, ...)` for the request direction because `pipeline` would destroy `req` on `upstreamReq` errors. Destroying `req` destroys the shared TCP socket between `req` and `res`, preventing error response delivery to the client. Upstream connection errors are handled separately via `upstreamReq.on('error')` (see §4.2.6).

`upstreamReq` is declared with `let` (not `const`) because event handlers (`req.on("error")`, `res.on("close")`) are registered _before_ `https.request()` to eliminate race conditions — see §4.2.7 for details.

#### 4.2.4 Response Proxying

When the upstream responds, frigga writes the filtered status code and headers to the client response, then pipes the response body using `stream.pipeline`.

```javascript
import { pipeline } from "node:stream";

// Inside https.request callback:
const responseHeaders = filterResponseHeaders(upstreamRes.headers);
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
```

The `pipeline` callback fires on both success and error. On error (e.g., mid-transfer upstream failure or client disconnect), the error is logged to stderr as an `upstream_error` event. The callback delegates to `emitRequestLog` (§4.2.3) for NDJSON audit logging with the upstream status code and raw response headers. If `res.on("close")` (§4.2.7) fired first for the same client disconnect, `emitRequestLog`'s `requestLogEmitted` guard ensures no duplicate log — the `pipeline` callback's call is a no-op. Conversely, if `pipeline` fires first, `res.on("close")` skips logging because `upstreamResponseReceived` is `true` (Phase B).

The log includes request metadata, the sidecar-captured request body, and the raw upstream response headers (`upstreamRes.headers`, before whitelist filtering). `request_headers` likewise records the raw client headers (`req.headers`, before whitelist filtering). Logging unfiltered headers provides full audit visibility — the whitelist is a known, deterministic transformation. Response body is not logged — response body logging is deferred to a future RFC.

The proxy does not branch on the `stream` parameter in the request body or on `content-type` for piping behavior — it pipes the upstream response through as-is via `stream.pipeline` regardless of content type.

#### 4.2.5 Response Header Whitelist

Upstream response headers are filtered before being forwarded to the client. Only whitelisted headers are included.

| Upstream Header         | Action                 | Rationale                                                                                                                                                        |
| ----------------------- | ---------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `content-type`          | Forward                | Client needs to know the response format (`application/json` or `text/event-stream`).                                                                            |
| `content-length`        | Forward if present     | Enables the client to allocate buffers. Absent for chunked/streaming responses.                                                                                  |
| `content-encoding`      | Forward if present     | Required when upstream compresses the response — client needs this to decompress. Present when `accept-encoding` was forwarded and upstream applied compression. |
| `x-request-id`          | Forward                | Anthropic request tracking — essential for support tickets and debugging.                                                                                        |
| `request-id`            | Forward                | Alternate request tracking header used by the Anthropic API.                                                                                                     |
| `anthropic-ratelimit-*` | Forward (prefix match) | Rate limit visibility — clients need this to implement backoff.                                                                                                  |
| `retry-after`           | Forward                | Rate limit recovery — clients need this to know when to retry.                                                                                                   |

All other upstream response headers (e.g., `server`, `date`, `via`, hop-by-hop headers) are dropped.

Implementation as a pure function:

```javascript
const RESPONSE_HEADER_FORWARD = new Set([
  "content-type",
  "content-length",
  "content-encoding",
  "x-request-id",
  "request-id",
  "retry-after",
]);

const RESPONSE_HEADER_PREFIXES = ["anthropic-ratelimit-"];

/**
 * Filter upstream response headers for the client.
 * @param {Record<string, string>} headers - upstreamRes.headers (lower-cased by Node.js)
 * @returns {Record<string, string>}
 */
export function filterResponseHeaders(headers) {
  const filtered = {};
  for (const [name, value] of Object.entries(headers)) {
    if (
      RESPONSE_HEADER_FORWARD.has(name) ||
      RESPONSE_HEADER_PREFIXES.some((prefix) => name.startsWith(prefix))
    ) {
      filtered[name] = value;
    }
  }
  return filtered;
}
```

#### 4.2.6 Error Handling

Errors during the forwarding pipeline are handled by the termination path that detects them first. Each path logs to stderr for diagnostics and delegates to `emitRequestLog` (§4.2.3) for exactly-once NDJSON audit logging.

| Condition                                          | Status | `error.type` | Handling                                                                                                                                                                                               |
| -------------------------------------------------- | ------ | ------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| Upstream connection error (DNS, TCP, TLS failure)  | `502`  | `api_error`  | `upstreamReq.on('error')` fires before any upstream response. Send 502 to client (if `!res.headersSent && !res.destroyed`). Log `upstream_error` to stderr. Call `emitRequestLog(502)`.                |
| Upstream response stream error (mid-transfer)      | —      | —            | `res.writeHead` already called; cannot change status code. `pipeline` callback fires with error, logs `upstream_error` to stderr, calls `emitRequestLog(upstreamRes.statusCode, upstreamRes.headers)`. |
| Client request stream error                        | `499`  | —            | `req.on("error")` fires. Call `abortUpstream()` (unpipe + destroy). Log `client_error` to stderr. Call `emitRequestLog(499)`. See §4.2.7.                                                              |
| Client disconnect during upstream wait (Phase A)   | `499`  | —            | `res.on("close")` fires with `!res.writableFinished`. Call `abortUpstream()`. Log `client_disconnect` to stderr. Call `emitRequestLog(499)`. See §4.2.7.                                               |
| Client disconnect during response piping (Phase B) | —      | —            | `pipeline` callback fires with error; `upstreamRes` and the upstream socket are destroyed. `res.on("close")` skips logging (`upstreamResponseReceived` is `true`). See §4.2.7.                         |

**502 error response:**

```json
{
  "type": "error",
  "error": {
    "type": "api_error",
    "message": "upstream connection failed: ECONNREFUSED"
  }
}
```

The error message includes the system error code (e.g., `ECONNREFUSED`, `ENOTFOUND`, `CERT_HAS_EXPIRED`) to aid development-phase debugging. The upstream host is not included — it is hardcoded and known.

**Stderr events:**

Upstream connection errors and client errors are logged to stderr in NDJSON format:

```json
{
  "timestamp": "2026-03-17T10:15:32.100Z",
  "level": "ERROR",
  "event": "upstream_error",
  "method": "POST",
  "url": "/v1/messages",
  "message": "ECONNREFUSED",
  "duration_ms": 45
}
```

```json
{
  "timestamp": "2026-03-17T10:15:32.100Z",
  "level": "ERROR",
  "event": "client_error",
  "method": "POST",
  "url": "/v1/messages",
  "message": "ECONNRESET",
  "duration_ms": 120
}
```

```json
{
  "timestamp": "2026-03-17T10:15:32.100Z",
  "level": "WARN",
  "event": "client_disconnect",
  "method": "POST",
  "url": "/v1/messages",
  "duration_ms": 120
}
```

The `client_error` event is `ERROR` level (unexpected stream failure); `client_disconnect` is `WARN` level (normal operation — Claude Code ESC is a routine user action). Both are distinct from `upstream_error`.

After sending the 502 response and logging the error to stderr, the handler delegates to `emitRequestLog(502)`. The centralized `emitRequestLog` function (§4.2.3) handles body completion — if `req` has not finished streaming, finalization is deferred to the `req` `end` or `error` event to capture the maximum request body. `response_headers` is absent from the NDJSON log because no upstream response was received. `requestChunks` is cleared after logging to release memory.

**Implementation:**

```javascript
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
```

The `!res.destroyed` guard (in addition to `!res.headersSent`) prevents writing to a client response whose underlying socket has already been destroyed — this occurs when the client disconnects and `res.on("close")` fires before the upstream error. Writing to a destroyed `ServerResponse` is a no-op in Node.js but the guard makes the intent explicit.

#### 4.2.7 Client Disconnect and Request Error Handling

When the client disconnects mid-request (e.g., Claude Code user pressing ESC) or the client request stream emits an error, the upstream request must be aborted to stop upstream inference and conserve token usage. Client disconnects are a **normal, high-frequency operation** — Claude Code users routinely press ESC to cancel in-flight requests. Both handlers use `abortUpstream(req, upstreamReq)` (§4.2.3) to safely disconnect the pipe before destroying the upstream request, log to stderr for diagnostics, and delegate to `emitRequestLog(499)` for audit logging.

**`req.on("error")` — Client request stream error:**

The `req.on("error")` handler fires when the client's TCP connection emits an error (e.g., `ECONNRESET` from ESC). Without this handler, `req.pipe()` would not propagate the error to `upstreamReq`, leaving the upstream request hanging. The handler calls `abortUpstream(req, upstreamReq)` to safely disconnect the pipe (see §4.3 for the `req.unpipe()` rationale), logs `client_error` to stderr, and calls `emitRequestLog(499)`. At the point this handler fires, `req.destroyed` is already `true` (Node.js sets `destroyed` before emitting `error`), so `emitRequestLog` takes the immediate finalization path — the sidecar-captured body is as complete as it can be.

**`res.on("close")` — Client disconnect detection:**

The `res.on("close")` handler fires when the client's response socket closes. The `!res.writableFinished` guard distinguishes premature close (client disconnect) from normal completion. The `upstreamResponseReceived` flag distinguishes Phase A (waiting for upstream response — log here) from Phase B (upstream response being piped — `pipeline` callback handles logging, §4.2.4).

Both handlers are registered _before_ `https.request()` to eliminate race conditions. `upstreamReq` is declared with `let` (not `const`) because it is assigned after event handlers are registered; the `abortUpstream(req, upstreamReq)` guard check (`if (upstreamReq && !upstreamReq.destroyed)`) ensures safety if a handler fires before assignment completes.

```javascript
let upstreamReq;
let upstreamResponseReceived = false;

req.on("error", (err) => {
  abortUpstream(req, upstreamReq);
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
  emitRequestLog(499);
});

res.on("close", () => {
  if (!res.writableFinished) {
    abortUpstream(req, upstreamReq);
    if (!upstreamResponseReceived) {
      // Phase A: no upstream response received yet
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
    // Phase B: pipeline callback handles logging — see §4.2.4
  }
});

upstreamReq = https.request(
  upstreamUrl,
  { method: "POST", headers: upstreamHeaders },
  (upstreamRes) => {
    upstreamResponseReceived = true;
    /* ... */
  },
);
req.pipe(upstreamReq);
```

**Phase B — Piping upstream response** (`upstreamRes` is being piped to `res`):

`stream.pipeline(upstreamRes, res, callback)` handles this automatically. When `res` is destroyed (client disconnect), pipeline destroys all streams in the chain, including `upstreamRes`. Destroying `upstreamRes` closes the upstream socket, aborting the request. The `res.on("close")` handler's `abortUpstream()` call is redundant in Phase B (the upstream socket is already destroyed via pipeline) but harmless — `!upstreamReq.destroyed` guard prevents double-destroy.

**Concurrent event ordering:**

When the client disconnects, both `req.on("error")` and `res.on("close")` may fire for the same TCP connection close (they share the underlying socket). The order is non-deterministic. Both paths are safe regardless of execution order:

| First handler     | Second handler    | Result                                                                                                                                                                                            |
| ----------------- | ----------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `req.on("error")` | `res.on("close")` | error: `abortUpstream()` ✓, stderr ✓, `emitRequestLog(499)` ✓ → `requestLogEmitted = true`. close: `abortUpstream()` → no-op (`upstreamReq.destroyed`); `emitRequestLog(499)` → guard hit, no-op. |
| `res.on("close")` | `req.on("error")` | close: `abortUpstream()` ✓, stderr ✓, `emitRequestLog(499)` ✓. error: `abortUpstream()` → no-op; `emitRequestLog(499)` → guard hit, no-op.                                                        |

#### 4.2.8 NDJSON Request Log Schema

The NDJSON request log (`event: "request"`, emitted to stdout) uses a unified schema across all response paths. The centralized `emitRequestLog` function (§4.2.3) ensures exactly one log entry per request via the `requestLogEmitted` guard. The following matrix defines field presence for each path:

| Field              | Forwarded (upstream responded) | Local Error (404/405)  | 502 (upstream connection error) |   499 (client error / disconnect)   |
| ------------------ | :----------------------------: | :--------------------: | :-----------------------------: | :---------------------------------: |
| `timestamp`        |               ✓                |           ✓            |                ✓                |                  ✓                  |
| `level`            |            `"INFO"`            |        `"INFO"`        |            `"INFO"`             |              `"INFO"`               |
| `event`            |          `"request"`           |      `"request"`       |           `"request"`           |             `"request"`             |
| `method`           |               ✓                |           ✓            |                ✓                |                  ✓                  |
| `url`              |               ✓                |           ✓            |                ✓                |                  ✓                  |
| `status`           |        upstream status         |       404 / 405        |               502               |                 499                 |
| `duration_ms`      |               ✓                |           ✓            |                ✓                |                  ✓                  |
| `request_headers`  |     ✓ (raw client headers)     | ✓ (raw client headers) |     ✓ (raw client headers)      |       ✓ (raw client headers)        |
| `request_body`     |      ✓ (sidecar capture)       |   ✓ (drain capture)    |       ✓ (sidecar capture)       | ✓ (sidecar capture, may be partial) |
| `response_headers` |    ✓ (raw upstream headers)    |           —            |                —                |                  —                  |

`response_headers` is present only when an upstream response was received. All other fields are present in every request log entry. `status: 499` follows the Nginx convention for "client closed request" — this is not sent to the client (who has already disconnected) but recorded in the audit log.

For the 502 and 499 paths, `emitRequestLog` (§4.2.3) handles body completion: if `req.readableEnded || req.destroyed`, finalization is immediate; otherwise it defers to `req` `end` or `error` event to capture the maximum request body. For the 499 path, `request_body` may be partial if the client disconnected mid-send — the sidecar captures whatever was received before the disconnect.

**Stderr event matrix:**

Diagnostic events are emitted to stderr as separate NDJSON log entries, independent of the stdout request log:

| Event               | Level   | Emitted by                                                       | When                                              |
| ------------------- | ------- | ---------------------------------------------------------------- | ------------------------------------------------- |
| `upstream_error`    | `ERROR` | `upstreamReq.on("error")` (§4.2.6), `pipeline` callback (§4.2.4) | Upstream connection failure or mid-transfer error |
| `client_error`      | `ERROR` | `req.on("error")` (§4.2.7)                                       | Client request stream error (e.g., `ECONNRESET`)  |
| `client_disconnect` | `WARN`  | `res.on("close")` Phase A (§4.2.7)                               | Client disconnects before upstream responds       |

`client_disconnect` uses `WARN` level (not `ERROR`) because it is a normal, high-frequency operation — Claude Code users routinely press ESC to cancel requests. `client_error` uses `ERROR` level because it indicates an unexpected stream failure. When both `req.on("error")` and `res.on("close")` fire for the same client disconnect, both stderr entries are emitted (they are independent diagnostic events), but only one NDJSON request log is produced (via the `requestLogEmitted` guard).

### 4.3 Design Rationale

**Whitelist over blacklist for request headers** — RFC-001 §8 proposed a blacklist approach ("forward all client request headers by default, removing or rewriting specific categories"). This RFC adopts a whitelist instead. In an enterprise gateway context, the set of headers that _should_ reach the upstream is small and well-defined (8 forwarded + 1 auto-set = 9 headers total), while the set that _should not_ is open-ended and grows with each SDK version, Nginx configuration change, or new client tool. A whitelist provides defense-in-depth: unknown headers are dropped by default, and adding a new forwarded header requires an explicit, auditable change. The blacklist approach has the opposite risk profile — a forgotten header silently leaks to the upstream.

**`accept-encoding` forwarded to upstream (pass-through)** — Three approaches were evaluated:

| Approach                                 | Description                                                                                      | Pros                                                                               | Cons                                                                                                                             |
| ---------------------------------------- | ------------------------------------------------------------------------------------------------ | ---------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------- |
| A. Don't forward                         | Omit `accept-encoding`; upstream sends uncompressed.                                             | Simple; zero-dependency.                                                           | Higher bandwidth between frigga and upstream; loses upstream compression benefit.                                                |
| B. Forward and decompress                | Forward `accept-encoding`; decompress upstream response via `node:zlib` before piping to client. | Less bandwidth to upstream; body available in plaintext.                           | Complexity; must handle gzip, br, zstd; decompression adds latency; breaks streaming simplicity.                                 |
| C. **Forward and pass through** (chosen) | Forward `accept-encoding`; pipe compressed response directly to client.                          | Minimal proxy overhead; bandwidth savings; future-proof for response body logging. | Client must support the negotiated encoding (always true — the encoding was negotiated from the client's own `accept-encoding`). |

**Approach C** is chosen because:

1. The client's `accept-encoding` is forwarded as-is, so the upstream negotiates an encoding the client already supports — no compatibility risk.
2. Nginx's `gzip` module does not re-compress responses that already carry a `Content-Encoding` header. No Nginx configuration changes are required.
3. This approach is future-proof for response body logging: when a future RFC adds response body capture, Node.js can inspect the `content-encoding` header and decompress via a sidecar `data` listener on `upstreamRes` using `node:zlib` (e.g., `zlib.createGunzip()`, `zlib.createBrotliDecompress()`), without changing the pass-through pipeline.
4. Bandwidth savings on the frigga-upstream link are a free benefit with no added complexity.

**Sidecar request body capture via `data` event listener** — Two approaches were evaluated for audit logging:

| Approach                        | Description                                                                               | Pros                                                                                                              | Cons                                                                                                        |
| ------------------------------- | ----------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------- |
| A. **Full buffering**           | Collect entire request body before forwarding (`upstreamReq.end(requestBody)`).           | Simple; body available before forwarding, enabling pre-forward blocking rules.                                    | Adds latency equal to full body transfer time; employee perceives slower time-to-first-token.               |
| B. **Sidecar capture** (chosen) | Stream body via `req.pipe(upstreamReq)` while a parallel `data` listener captures chunks. | Zero forwarding latency — body arrives at upstream as fast as the client sends it; audit capture is non-blocking. | Body only fully available after forwarding completes; cannot block based on body content before forwarding. |

**Approach B** is chosen because the current phase does not require pre-forward content inspection — audit logging is retrospective. Employee responsiveness is the primary concern: adding a full buffering step before forwarding would increase time-to-first-token by the request transfer time, which is perceptible for large prompts. The sidecar approach delivers the same audit data with zero impact on the forwarding path. Pre-forward content inspection (e.g., blocking requests containing sensitive patterns) can be added in a future RFC by switching to Approach A for requests that match a configurable audit rule.

**`req.pipe()` over `pipeline()` for request body** — `req.pipe(upstreamReq)` is preferred over `pipeline(req, upstreamReq, callback)` for the request direction because `pipeline` destroys all streams in the chain on error. If the upstream connection fails, `pipeline` would destroy `req` (the client's incoming request). Since `req` and `res` share the same underlying TCP socket, destroying `req` destroys the socket, preventing the server from sending an error response (e.g., 502) to the client. Upstream connection errors are handled separately via `upstreamReq.on('error')`.

**`node:https` request over global `fetch`** — Node.js 24 provides a built-in `fetch` (via undici). However, `https.request` is preferred because: (a) it returns Node.js native streams, which integrate directly with `stream.pipeline`, `req.pipe()`, and `http.ServerResponse` without web-to-Node stream conversion; (b) it provides event-level control over connection errors and client disconnect propagation (`req.on('error')`, `res.on('close')`); (c) it aligns with the existing event-driven patterns in `startServer`.

**`handleRequest` returns `null` on validation pass** — The pure `handleRequest` function currently returns `{ statusCode: 501, ... }` when validation succeeds. This is changed to return `null`, signaling "validation passed; forward to upstream." The caller (`startServer`) handles the upstream forwarding. This keeps `handleRequest` as a pure validation function with no knowledge of upstream configuration, improving separation of concerns and testability. The function's signature is also simplified — the `apiKeyHash` parameter is removed since authorization validation is deferred.

**Deferring authorization** — The current phase prioritizes getting the forwarding pipeline operational. Authorization validation (`--api-key` for client auth) and credential injection (`--upstream-api-key` for upstream auth) add complexity that is orthogonal to the core forwarding mechanics. Deferring them allows the forwarding pipeline, header filtering, sidecar logging, error handling, and client disconnect handling to be implemented and tested in isolation. Authorization will be reintroduced in a future RFC.

**`req.unpipe()` before `upstreamReq.destroy()` in abort handlers** — When `req.on("error")` or `res.on("close")` fires and calls `upstreamReq.destroy()`, a race condition exists: `destroy()` sets `upstreamReq.destroyed = true` synchronously, but `pipe()`'s cleanup (unpipe via `close` event) is asynchronous. In the window between these two operations, `pipe()`'s internal `data` listener can still fire and call `upstreamReq.write(chunk)` — writing to a destroyed writable emits `ERR_STREAM_DESTROYED` via `nextTick`, which triggers `upstreamReq.on("error")` with a misleading error (not a genuine upstream failure). Calling `req.unpipe(upstreamReq)` first synchronously removes `pipe()`'s `data` listener, closing the race window entirely. After `unpipe()`, `req` remains in flowing mode because the sidecar `data` listener is still registered — remaining buffered data continues to be captured for audit logging, which is the desired behavior.

**Centralized `emitRequestLog` with `requestLogEmitted` guard** — The forwarding path has four termination handlers that all need to emit a NDJSON audit log: `pipeline` callback (§4.2.4), `upstreamReq.on("error")` (§4.2.6), `req.on("error")` (§4.2.7), and `res.on("close")` (§4.2.7). When the client disconnects, multiple handlers may fire for the same event (e.g., both `req.on("error")` and `res.on("close")` fire because they share the same TCP socket). A centralized `emitRequestLog` function with a boolean guard ensures exactly-once log emission regardless of handler execution order. The function also centralizes `requestChunks` consumption and cleanup — only the first handler to call `emitRequestLog` reads the captured body; subsequent calls are no-ops, eliminating the risk of a second handler reading an empty array. The three-branch finalization logic (`req.readableEnded` → immediate; `req.destroyed` → immediate with partial body; else → defer to `end`/`error`) is shared across all termination paths rather than duplicated in each handler.

**Status 499 for client disconnect** — HTTP does not define a status code for "client closed the connection before receiving a response." Nginx uses the non-standard `499` for this case. Since the NDJSON audit log is internal (not sent to clients), using `499` provides a clear, recognizable convention. The alternative — omitting `status` or using `0` — would complicate log parsing and require special-case handling in log analysis tools.

**`finalized` guard in deferred log finalization** — In `createRequestLogEmitter`'s deferred path, both `end` and `error` listeners are registered on `req`. In edge cases, Node.js may emit both events for the same stream (e.g., `end` event already queued in the microtask queue when a socket error triggers `error`). Without a guard, `finalize()` would execute twice — the second call reads an emptied `requestChunks` (after `requestChunks.length = 0`) and emits a duplicate log entry with empty `request_body`. The nested `finalized` boolean guard ensures exactly-once finalization at zero cost.

**`emitLocalLog` with `localLogEmitted` guard on local error path** — The local error path registers both `error` and `end` listeners on `req` for body drain. If `error` fires (e.g., client TCP reset), `end` may not follow — without logging in the `error` handler, the NDJSON audit log would never be emitted, violating the audit completeness guarantee ("every request that reaches the request callback produces a request log entry"). The `localLogEmitted` guard ensures the first event to fire produces the log; the second is a no-op.

**Extracting `abortUpstream` and `createRequestLogEmitter` as exported functions** — The original design defined these as closures inside the request callback, making them untestable in isolation despite being listed as unit test targets (§7 scenarios #10–#14). Extracting them as standalone exported functions in `src/server.js` enables direct unit testing with mock objects while maintaining the same runtime behavior. `abortUpstream(req, upstreamReq)` takes both stream objects as parameters. `createRequestLogEmitter` uses a factory pattern — the returned `emitRequestLog` closure captures the context via the factory's parameters, providing the same ergonomic call-site API (`emitRequestLog(status, responseHeaders?)`) while making the factory itself importable and testable.

## 5. Interface Changes

**CLI — Before (RFC-001):**

```
node src/index.js --api-key=<key> [--port=<port>] [--host=<host>]
```

**CLI — After:**

```
node src/index.js [--port=<port>] [--host=<host>]
```

The `--api-key` argument is removed. No new arguments are added.

**HTTP behavior — Before:** All valid requests return `501 Not Implemented` (after authorization check).

**HTTP behavior — After:** Valid requests (correct route and method) are forwarded to `https://api.anthropic.com`; the upstream response is returned to the client. Invalid routes return `404`; invalid methods return `405`.

**NDJSON request log — Before (RFC-003):**

```json
{
  "timestamp": "...",
  "level": "INFO",
  "event": "request",
  "method": "POST",
  "url": "/v1/messages",
  "status": 501,
  "duration_ms": 1,
  "request_headers": {},
  "request_body": "...",
  "response_headers": {},
  "response_body": "..."
}
```

**NDJSON request log — After:**

```json
{
  "timestamp": "...",
  "level": "INFO",
  "event": "request",
  "method": "POST",
  "url": "/v1/messages",
  "status": 200,
  "duration_ms": 1500,
  "request_headers": {},
  "request_body": "...",
  "response_headers": {}
}
```

The `response_body` field is removed from all request logs — both forwarded requests and local error responses (404, 405). `request_body` is included in all request logs, including local error responses (404, 405) — the request body is captured during drain for audit completeness. `response_headers` is retained for forwarded requests — it contains the raw upstream response headers (before whitelist filtering); absent for local error responses, 502 errors, and 499 (client disconnect — no upstream response received). See §4.2.8 for the complete field-presence matrix across all response paths. Response body logging is deferred to a future RFC.

**Stderr diagnostic events — New:**

Three NDJSON events are emitted to stderr for diagnostics (see §4.2.8 for the complete stderr event matrix):

- `upstream_error` (`ERROR`): Upstream connection failure or mid-transfer error — emitted by `upstreamReq.on("error")` (§4.2.6) and `pipeline` callback (§4.2.4).
- `client_error` (`ERROR`): Client request stream error (e.g., `ECONNRESET`) — emitted by `req.on("error")` (§4.2.7).
- `client_disconnect` (`WARN`): Client disconnects before upstream responds — emitted by `res.on("close")` Phase A (§4.2.7). Uses `WARN` level because this is a normal, high-frequency operation (Claude Code ESC).

These stderr events are independent of the stdout NDJSON request log. When both `req.on("error")` and `res.on("close")` fire for the same client disconnect, both stderr entries are emitted, but only one stdout request log is produced (via the `requestLogEmitted` guard).

## 6. Backward Compatibility & Migration

- **Breaking changes:**
  - The `--api-key` CLI argument is removed. Existing startup commands that include `--api-key` will fail with a usage error. Remove `--api-key` from all startup scripts and configurations.
  - Valid requests are now forwarded to the upstream instead of returning `501`. Any test or script relying on the `501` response for valid requests must be updated.
  - The `handleRequest` function's signature changes from `handleRequest(req, apiKeyHash, isShuttingDown)` to `handleRequest(req, isShuttingDown)`. The return type changes from `{ statusCode, headers, body }` to `{ statusCode, headers, body } | null` — callers must handle the `null` case.
  - The NDJSON request log no longer includes the `response_body` field for any request type (forwarded, local error, or 502). The `response_headers` field is retained for forwarded requests. Log consumers that depend on `response_body` must be updated.
- **Migration path:** Remove `--api-key` from startup commands. No other configuration changes are required.

## 7. Testing Strategy

Tests are organized into unit tests (pure functions, no I/O) and integration tests (real HTTP servers, local-only — no upstream mock). All tests use the Node.js built-in test runner (`node --test`), consistent with the zero-runtime-dependency design. The upstream URL is hardcoded to `https://api.anthropic.com`, so upstream forwarding behavior is not mock-tested — unit tests cover the pure functions in the forwarding pipeline, and integration tests cover only the local error paths that do not require an upstream connection.

**Unit tests** (`tests/handler.test.js`):

- `buildUpstreamHeaders`: whitelist filtering, `authorization` forwarded as-is, `content-length` forwarded when present, non-whitelisted headers excluded (e.g., `x-stainless-arch`, `connection`), handling of missing optional headers (e.g., no `anthropic-beta` in request)
- `filterResponseHeaders`: whitelist filtering, `anthropic-ratelimit-*` prefix matching, exclusion of non-whitelisted headers (e.g., `server`, `date`)
- `handleRequest`: returns `null` for valid route and method; returns 404/405 for invalid route/method (replaces existing 501 and auth assertions)
- `abortUpstream(req, upstreamReq)`: `req.unpipe()` called before `upstreamReq.destroy()`, no-op when `upstreamReq.destroyed` is `true`, no-op when `upstreamReq` is `undefined`
- `createRequestLogEmitter`: exactly-once `emitted` guard (second call of returned `emitRequestLog` is no-op), immediate finalization when `req.readableEnded` or `req.destroyed`, deferred finalization to `end`/`error` event when `req` is still streaming, `finalized` guard prevents double execution when both `end` and `error` fire

**Integration tests** (`tests/server.test.js`):

- Route validation: verify 404 for invalid routes, with `request_body` captured in NDJSON log
- Method validation: verify 405 for invalid methods, with `request_body` captured in NDJSON log
- NDJSON log schema for local errors: `response_headers` absent, `request_body` present, `response_body` absent
- Existing integration tests updated: remove `--api-key` from server startup, remove authorization header assertions

**Key Scenarios:**

| #   | Scenario                                    | Type        | Input                                                                                 | Expected Behavior                                                                                                               |
| --- | ------------------------------------------- | ----------- | ------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------- |
| 1   | Request header whitelist                    | Unit        | Headers with `anthropic-version`, `accept-encoding`, `x-stainless-arch`, `connection` | `buildUpstreamHeaders` returns `anthropic-version` and `accept-encoding`; `x-stainless-arch` and `connection` absent.           |
| 2   | Authorization forwarded                     | Unit        | `authorization: Bearer sk-ant-xxx`                                                    | `buildUpstreamHeaders` includes identical `authorization: Bearer sk-ant-xxx`.                                                   |
| 3   | Content-Length forwarded                    | Unit        | `content-length: 42`                                                                  | `buildUpstreamHeaders` includes identical `content-length: 42`.                                                                 |
| 4   | Response header whitelist                   | Unit        | `x-request-id`, `content-encoding`, `server`, `anthropic-ratelimit-requests-limit`    | `filterResponseHeaders` returns `x-request-id`, `content-encoding`, `anthropic-ratelimit-requests-limit`; `server` absent.      |
| 5   | Rate limit prefix match                     | Unit        | `anthropic-ratelimit-requests-limit`, `anthropic-ratelimit-tokens-remaining`          | `filterResponseHeaders` returns both headers.                                                                                   |
| 6   | Valid route returns null                    | Unit        | `POST /v1/messages`                                                                   | `handleRequest` returns `null`.                                                                                                 |
| 7   | Route validation                            | Integration | `POST /v1/completions` with JSON body                                                 | Client receives `404`; NDJSON log contains `request_body` matching sent body — request is not forwarded.                        |
| 8   | Method validation                           | Integration | `GET /v1/messages`                                                                    | Client receives `405`; NDJSON log contains `request_body` (empty for bodyless requests) — request is not forwarded.             |
| 9   | Log schema (local error)                    | Integration | 404/405 response paths                                                                | NDJSON log matches §4.2.8 local error column — `response_headers` absent, `request_body` present, no `response_body` field.     |
| 10  | `abortUpstream` unpipe safety                        | Unit        | Mock `req` with `unpipe` and `upstreamReq` with `destroyed`/`destroy`                 | `abortUpstream(req, upstreamReq)` calls `req.unpipe(upstreamReq)` before `upstreamReq.destroy()`; no-op when `upstreamReq.destroyed` is `true`. |
| 11  | `createRequestLogEmitter` exactly-once guard         | Unit        | Call returned `emitRequestLog` twice with different arguments                         | Second call is a no-op; stdout contains exactly one NDJSON entry with the first call's `status`.                                               |
| 12  | `createRequestLogEmitter` deferred finalization      | Unit        | Call `emitRequestLog` with `req.readableEnded = false`, then emit `end`               | NDJSON entry is emitted after `end` event with complete `request_body`.                                                                        |
| 13  | `createRequestLogEmitter` immediate on destroyed req | Unit        | Call `emitRequestLog` with `req.destroyed = true`                                     | NDJSON entry is emitted immediately with sidecar-captured `request_body`.                                                                      |
| 14  | `createRequestLogEmitter` finalized guard            | Unit        | Call `emitRequestLog` with `req.readableEnded = false`, emit both `end` and `error`   | Exactly one NDJSON entry emitted; `finalize()` executes only once despite both events firing.                                                  |

## 8. Implementation Plan

### Phase 1: Unit Tests — 1 day

- [x] Add tests for `buildUpstreamHeaders` — verify whitelist filtering, `authorization` forwarded as-is, `content-length` forwarded when present, non-whitelisted headers excluded (e.g., `x-stainless-arch`, `connection`), handling of missing optional headers (e.g., no `anthropic-beta` in request)
- [x] Add tests for `filterResponseHeaders` — verify whitelist filtering, `anthropic-ratelimit-*` prefix matching, exclusion of non-whitelisted headers (e.g., `server`, `date`)
- [x] Update existing `handleRequest` tests — remove all authorization-related tests; change expected result for valid route+method from `{ statusCode: 501 }` to `null`; update function signature to `handleRequest(req, isShuttingDown)`
- [x] Add tests for `abortUpstream(req, upstreamReq)` — verify `req.unpipe()` called before `upstreamReq.destroy()`, no-op when `upstreamReq.destroyed` is `true`, no-op when `upstreamReq` is `undefined`
- [x] Add tests for `createRequestLogEmitter` — verify exactly-once `emitted` guard (second call of returned `emitRequestLog` is no-op), immediate finalization when `req.readableEnded` or `req.destroyed`, deferred finalization to `end`/`error` event, `finalized` guard prevents double execution, `requestChunks` cleared after consumption

**Done when:** All new unit tests written; updated `handleRequest` tests expect `null` for valid route+method (initially failing — red). Scenarios #1–#6, #10–#14 from §7 covered.

### Phase 2: CLI & Constants — 0.5 day

- [x] Add `UPSTREAM_BASE_URL` to `src/constants.js`
- [x] Remove `--api-key` from `resolveArgs()` in `src/cli.js` — the function no longer requires any arguments
- [x] Update `printUsage()` to remove `--api-key` documentation
- [x] Update `startServer` signature to remove `apiKey` from the config object
- [x] Update `src/index.js` to remove `apiKey` from the `startServer` call

**Done when:** Server starts without `--api-key`. `resolveArgs` returns `{ port, host }` only.

### Phase 3: Core Forwarding — 2 days

- [x] Restructure `startServer` request callback: call `handleRequest` before body buffering; bifurcate into local error path (with body drain capture and `req.on("error")`) and forwarding path
- [x] Implement forwarding path state variables (`upstreamReq`, `upstreamResponseReceived`, `requestChunks`)
- [x] Implement `abortUpstream(req, upstreamReq)` as exported function in `src/server.js` — `req.unpipe(upstreamReq)` before `upstreamReq.destroy()`
- [x] Implement `createRequestLogEmitter({ req, requestChunks, startTime })` as exported factory function in `src/server.js` — `emitted` exactly-once guard, `finalized` guard in deferred path, three-branch finalization (`readableEnded` / `destroyed` / defer to `end`+`error`)
- [x] Implement `buildUpstreamHeaders()` in `src/server.js`
- [x] Implement `filterResponseHeaders()` in `src/server.js`
- [x] Change `handleRequest` to return `null` when route and method are valid (remove auth validation and 501 block); remove `apiKeyHash` parameter
- [x] Implement upstream request construction with `https.request` in `startServer`'s request callback
- [x] Implement request body streaming via `req.pipe(upstreamReq)` with sidecar `data` capture
- [x] Implement response piping via `pipeline(upstreamRes, res, callback)` with error logging and `emitRequestLog` delegation
- [x] Implement 502 error handling for upstream connection failures with `!res.destroyed` guard and `emitRequestLog(502)` delegation
- [x] Implement `req.on("error")` handler — `abortUpstream()`, stderr `client_error`, `emitRequestLog(499)`
- [x] Register `res.on("close")` before `https.request()` — Phase A/B distinction via `upstreamResponseReceived`, `abortUpstream()`, stderr `client_disconnect`, `emitRequestLog(499)`
- [x] Implement `Connection: close` on forwarded responses during shutdown
- [x] Remove `response_body` from local error response logs

**Done when:** All Phase 1 unit tests passing (green). Server forwards requests to `https://api.anthropic.com` and pipes responses back.

### Phase 4: Integration Tests & Test Cleanup — 0.5 day

- [x] Verify `request_body` is captured in local error response logs (404, 405) — scenarios #7–#8 from §7
- [x] Verify NDJSON log schema for local error paths — `response_headers` absent, `request_body` present, no `response_body` field (scenario #9 from §7)
- [x] **Delete** authorization-related tests: API key validation, `401 Unauthorized` for missing/invalid keys, `apiKeyHash` parameter tests
- [x] **Modify** existing integration tests: remove `--api-key` from server startup; remove authorization header assertions

**Done when:** All tests passing. Full scenario coverage from §7.

## 9. Risks

| Risk                                                                                     | Likelihood | Impact | Mitigation                                                                                                                                                                                                                                                                                                                                                               |
| ---------------------------------------------------------------------------------------- | ---------- | ------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| No authorization — anyone with network access can use the proxy                          | High       | Med    | The proxy is deployed on a private network (enterprise LAN or VPN). Nginx can enforce client certificate authentication or IP allowlisting at the TLS layer. Authorization will be reintroduced in a future RFC.                                                                                                                                                         |
| Sidecar `data` listener captures body after forwarding — cannot block malicious requests | Med        | Med    | Current phase is audit-only (retrospective review). Pre-forward content inspection can be added in a future RFC by switching to full buffering for requests matching configurable audit rules.                                                                                                                                                                           |
| Whitelist blocks a required header added by a future SDK version                         | Med        | Med    | Whitelist is defined as a constant `Set`, making it easy to audit and extend. Add dropped-header logging in a future RFC to detect misses proactively.                                                                                                                                                                                                                   |
| `stream.pipeline` error handling edge cases (half-open connections, premature close)     | Med        | Low    | Integration tests cover client disconnect and upstream error scenarios. Node.js `stream.pipeline` is battle-tested infrastructure.                                                                                                                                                                                                                                       |
| Sidecar body capture grows memory for large requests                                     | Low        | Low    | Anthropic API enforces its own request size limits (body is dominated by the prompt, typically < 1 MB). The sidecar listener adds proportional memory cost but does not block forwarding.                                                                                                                                                                                |
| `emitRequestLog` deferred finalization — `req` never emits `end` or `error`              | Low        | Low    | If `req.readableEnded` and `req.destroyed` are both `false` when `emitRequestLog` is called, finalization defers to `end`/`error`. If neither fires (client keeps connection open but sends no data), `requestChunks` remains in memory. Bounded by client behavior — after receiving 502, the client closes the connection, which eventually triggers `end` or `error`. |

## 10. Future Work

- **Authorization and credential injection** — Reintroduce `--api-key` for client-facing Bearer token validation and `--upstream-api-key` for upstream credential injection. This restores credential isolation where the client never sees the upstream API key.
- **Response body logging** — Capture and decompress response body based on `content-encoding` header using `node:zlib`, then log as `response_body`.
- **Dropped header logging** — Add a DEBUG-level log entry listing headers present in the client request but excluded by the whitelist, to aid in diagnosing SDK compatibility issues.

## 11. References

- [RFC-001: Messages API Proxy Service](./rfc_001_api_proxy_service.md) — base service design; `--api-key` argument removed by this RFC; §8 Future Work (header strategy, body streaming) superseded by this RFC
- [RFC-003: NDJSON Request Debug Logging](./rfc_003_request_debug_logging.md) — log format and request body collection mechanism; `response_headers` and `response_body` fields removed from forwarded request logs by this RFC
- [LLM Gateway Configuration](https://code.claude.com/docs/en/llm-gateway) — header forwarding requirements (`anthropic-beta`, `anthropic-version`)
- [Anthropic Messages API](https://docs.anthropic.com/en/api/messages)
- [Node.js `stream.pipeline`](https://nodejs.org/api/stream.html#streampipelinesource-transforms-destination-callback)
- [RFC 9110 §7.6.1 — Connection](https://www.rfc-editor.org/rfc/rfc9110.html#name-connection) — hop-by-hop header definition

---

## Changelog

| Version | Date       | Author      | Changes                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               |
| ------- | ---------- | ----------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 2.6     | 2026-03-18 | Chason Tang | Add `finalized` guard in `createRequestLogEmitter` deferred path to prevent double execution when both `end` and `error` fire; replace no-op `req.on("error", () => {})` on local error path with `emitLocalLog` + `localLogEmitted` guard for audit completeness; extract `abortUpstream(req, upstreamReq)` and `createRequestLogEmitter({ req, requestChunks, startTime })` as exported functions in `src/server.js` for unit testability (§7 scenarios #10–#14) |
| 2.5     | 2026-03-18 | Chason Tang | Add centralized `emitRequestLog` function with `requestLogEmitted` exactly-once guard — replaces inline NDJSON logging in `pipeline` callback (§4.2.4) and inline `readableEnded` defer logic in `upstreamReq.on("error")` (§4.2.6); add `abortUpstream()` helper — `req.unpipe(upstreamReq)` before `upstreamReq.destroy()` to prevent write-after-destroy race condition; add `client_error` (ERROR) and `client_disconnect` (WARN) stderr events for `req.on("error")` and `res.on("close")` Phase A; add NDJSON audit logging to client error/disconnect paths via `emitRequestLog(499)` (Nginx convention); add `upstreamResponseReceived` flag for Phase A/B distinction in `res.on("close")`; add `!res.destroyed` guard to 502 response in `upstreamReq.on("error")`; add concurrent event ordering analysis to §4.2.7; add §4.2.8 stderr event matrix; add §4.3 rationale entries for `req.unpipe()`, centralized `emitRequestLog`, and status 499; add unit test scenarios #10–#13 for `abortUpstream` and `emitRequestLog` |
| 2.4     | 2026-03-18 | Chason Tang | Defer NDJSON request log in `upstreamReq.on("error")` to `req` `end` event via `req.readableEnded` guard — fixes incomplete `request_body` when upstream error fires mid-stream; remove all mock-upstream integration tests; restructure §7 to maximize unit test coverage of pure functions; integration tests retained only for local error paths (404, 405)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        |
| 2.3     | 2026-03-18 | Chason Tang | Add `req.on("error")` handling to both local error and forwarding paths; capture `request_body` on local error paths for audit completeness (replacing bare `req.resume()`); add complete `upstreamReq.on("error")` implementation to §4.2.6; add §4.2.8 NDJSON request log schema matrix for field-presence across all response paths                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                |
| 2.2     | 2026-03-18 | Chason Tang | Restructure §4.2.3 with explicit request callback control flow and `req.resume()` for local errors; fix client disconnect race — register `res.on("close")` before `https.request()`; add `pipeline` error logging and `requestChunks` cleanup; add NDJSON request log to 502 error path; add `duration_ms` to `upstream_error` log; remove `response_body` from all request logs; add URL construction safety note; expand §8 with explicit test list; fix Phase 4 `response_headers` verification                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| 2.1     | 2026-03-17 | Chason Tang | Add `accept-encoding` to request header whitelist (Approach A → C pass-through); add `content-encoding` to response header whitelist; retain `response_headers` in NDJSON logs (only `response_body` removed); include system error code in 502 error response; clarify `content-length` / chunked mutual exclusivity                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 |
| 2.0     | 2026-03-17 | Chason Tang | Remove authorization validation and credential injection (defer to future RFC); remove `--api-key`, `--upstream-api-key`, `--upstream-base-url` CLI options; hardcode upstream to `https://api.anthropic.com`; replace full request body buffering with streaming via `req.pipe()` + sidecar `data` capture; remove response logging (`response_headers`, `response_body`) from NDJSON logs                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           |
| 1.0     | 2026-03-17 | Chason Tang | Initial version                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
