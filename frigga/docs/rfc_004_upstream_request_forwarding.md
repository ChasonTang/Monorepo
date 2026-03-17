# RFC-004: Upstream Request Forwarding

**Version:** 1.0
**Author:** Chason Tang
**Date:** 2026-03-17
**Status:** Proposed

---

## 1. Summary

This proposal replaces the current `501 Not Implemented` stub with a complete upstream forwarding pipeline. Validated client requests are forwarded to the Anthropic API (or a configurable upstream) with credential injection and whitelist-based header filtering. Upstream responses — both streaming (SSE) and non-streaming (JSON) — are piped back to the client transparently. The request body is fully buffered before forwarding to support audit logging (per RFC-003). The design operates identically in both development (direct HTTP) and production (behind Nginx with TLS termination).

## 2. Motivation

Frigga currently validates incoming requests but returns `501 Not Implemented` for all valid requests (RFC-001 §4.2.2). This makes the service unusable as an actual AI gateway. Implementing upstream forwarding is the core remaining functionality that completes frigga's purpose as an enterprise AI review gateway.

Enterprise deployment introduces specific requirements not present in a basic proxy:

- **Header security**: Client requests may carry headers that should not reach the upstream (e.g., `anthropic-dangerous-direct-browser-access`, SDK telemetry headers like `x-stainless-*`). A whitelist approach ensures only explicitly approved headers are forwarded, providing defense-in-depth against header injection and information leakage.
- **Audit logging**: The request body must be available for inspection before forwarding. This requires full buffering of the request body, superseding RFC-001 §8's planned zero-copy `stream.pipeline(req, upstreamReq)` approach.
- **Credential isolation**: The client's proxy-layer Bearer token must be replaced with the upstream Anthropic API key, keeping upstream credentials entirely server-side.

## 3. Goals and Non-Goals

**Goals:**
- Forward validated requests to the upstream Anthropic API with the client credential replaced by the upstream API key
- Whitelist-based header filtering for both request (outbound) and response (inbound) directions
- Full request body buffering to support audit logging via existing RFC-003 NDJSON logs
- Transparent response proxying via stream piping — no content-type branching or response body transformation
- Configurable upstream base URL with secure default (`https://api.anthropic.com`)
- Correct behavior in both direct HTTP (development) and behind Nginx (production) deployments

**Non-Goals:**
- Response body auditing or filtering — only request body auditing is required in the current phase
- Request or response body transformation (e.g., rewriting, redacting) — the proxy is transparent at the body level
- Upstream load balancing or failover across multiple endpoints — a single upstream URL is sufficient
- Response compression handling — delegated to Nginx in production; upstream is requested to send uncompressed responses (see §4.3)

## 4. Design

### 4.1 Overview

```
Development:

  Client                          Frigga                        Anthropic API
    |                               |                                |
    | POST /v1/messages             |                                |
    | Authorization: Bearer <key>   |                                |
    |------------------------------>|                                |
    |                               | 1. Collect full request body   |
    |                               | 2. Validate (route/method/auth)|
    |                               | 3. Build upstream request      |
    |                               |    - Whitelist headers         |
    |                               |    - Inject upstream API key   |
    |                               |                                |
    |                               | POST /v1/messages              |
    |                               | Authorization: Bearer          |
    |                               |   <upstream-api-key>           |
    |                               |------------------------------->|
    |                               |                                |
    |                               |<--- response (JSON or SSE) ----|
    |                               |                                |
    |<--- response (pipe through) --|                                |
    |                               | 4. Log (NDJSON per RFC-003)    |

Production:

  Client          Nginx (TLS)           Frigga                  Anthropic API
    |                 |                    |                          |
    | HTTPS           | proxy_pass HTTP    |                          |
    |---------------->|------------------->| (same flow as above)     |
    |                 |                    |                          |
    |<--- response ---|<--- response ------|                          |
```

The request body is fully buffered (step 1) before validation and forwarding. This is required for audit logging (RFC-003) and supersedes the zero-copy `stream.pipeline(req, upstreamReq)` approach described in RFC-001 §8. The upstream response body is streamed (piped) without full buffering.

### 4.2 Detailed Design

#### 4.2.1 CLI Configuration

Two new CLI arguments are added to `src/cli.js`:

| Argument | Required | Default | Description |
|----------|----------|---------|-------------|
| `--upstream-api-key` | No | — | Anthropic API key injected into `Authorization: Bearer` header on upstream requests. When omitted, the server retains `501 Not Implemented` behavior for valid requests. |
| `--upstream-base-url` | No | `https://api.anthropic.com` | Base URL for the upstream API. Must include the protocol scheme. |

**Validation rules:**
- `--upstream-base-url` without `--upstream-api-key` is an error (exit code 1 with usage message).
- `--upstream-api-key` with an empty value is an error.
- The `--upstream-base-url` value is parsed with `new URL()` to validate its format. An invalid URL produces an error.

When `--upstream-api-key` is provided, `resolveArgs()` returns a new `upstream` field:

```javascript
{
  port: 3000,
  host: "127.0.0.1",
  apiKey: "client-facing-key",
  upstream: {                          // null when --upstream-api-key is omitted
    apiKey: "sk-ant-...",
    baseUrl: "https://api.anthropic.com",
  },
}
```

A new constant is added to `src/constants.js`:

```javascript
export const DEFAULT_UPSTREAM_BASE_URL = "https://api.anthropic.com";
```

#### 4.2.2 Request Header Whitelist

Outbound request headers are constructed using a strict whitelist. Only headers listed below are included in the upstream request; all others are dropped.

| Client Header | Upstream Action | Rationale |
|---------------|-----------------|-----------|
| `content-type` | Forward as-is | Required by the upstream API (`application/json`). |
| `accept` | Forward as-is | Signals expected response format. |
| `user-agent` | Forward as-is | Identifies the client for upstream analytics. |
| `anthropic-beta` | Forward as-is | Required per LLM gateway specification — enables beta features. |
| `anthropic-version` | Forward as-is | Required per LLM gateway specification — specifies API version. |
| `authorization` | **Replace** with `Bearer <upstream-api-key>` | Inject upstream credential; the client's proxy-layer token is never forwarded. |
| `host` | **Replace** with upstream hostname | Automatically set by `node:https` from the request URL (e.g., `api.anthropic.com`). |
| `content-length` | **Set** from buffered body byte length | Computed from `Buffer.byteLength(requestBody)`. Sent regardless of whether the client used chunked or content-length transfer. |

**Headers explicitly NOT forwarded (representative examples, not exhaustive — the whitelist above is the authoritative definition):**

| Header | Reason |
|--------|--------|
| `x-stainless-*` | SDK telemetry — not required by the upstream API. |
| `anthropic-dangerous-direct-browser-access` | SDK internal safety bypass — must not propagate to the upstream. |
| `x-app` | Client telemetry — not required by the upstream API. |
| `connection` | Hop-by-hop header (RFC 9110 §7.6.1) — pertains to the client-frigga connection, not the frigga-upstream connection. |
| `accept-encoding` | See §4.3 Design Rationale for detailed analysis. |
| `content-length` (original) | Replaced with the byte length of the buffered body. |
| `host` (original) | Replaced by `node:https` with the upstream hostname. |
| `x-forwarded-*`, `x-real-ip` | Nginx-injected headers — pertain to the client-nginx-frigga chain, not the frigga-upstream connection. |
| `keep-alive`, `proxy-authorization`, `proxy-connection`, `te`, `trailer`, `transfer-encoding`, `upgrade` | Hop-by-hop headers per RFC 9110. |

Implementation as a pure function in `src/server.js`:

```javascript
const REQUEST_HEADER_FORWARD = new Set([
  "content-type",
  "accept",
  "user-agent",
  "anthropic-beta",
  "anthropic-version",
]);

/**
 * Build upstream request headers from the client request headers.
 * @param {Record<string, string>} incomingHeaders - req.headers (lower-cased by Node.js)
 * @param {string} upstreamApiKey
 * @returns {Record<string, string>}
 */
export function buildUpstreamHeaders(incomingHeaders, upstreamApiKey) {
  const headers = { authorization: `Bearer ${upstreamApiKey}` };
  for (const name of REQUEST_HEADER_FORWARD) {
    if (incomingHeaders[name] !== undefined) {
      headers[name] = incomingHeaders[name];
    }
  }
  return headers;
}
```

`host` and `content-length` are not set in this function — `node:https` sets `host` automatically from the request URL, and `content-length` is set by the caller from the buffered body byte length.

#### 4.2.3 Upstream Request Construction

The upstream request is constructed using `node:https` (or `node:http` when `--upstream-base-url` uses `http://`, which is useful for integration testing against a local mock server).

```javascript
import http from "node:http";
import https from "node:https";

const upstreamUrl = new URL(req.url, upstream.baseUrl);
const transport = upstreamUrl.protocol === "https:" ? https : http;

const upstreamHeaders = buildUpstreamHeaders(req.headers, upstream.apiKey);
upstreamHeaders["content-length"] = Buffer.byteLength(requestBody);

const upstreamReq = transport.request(
  upstreamUrl,
  {
    method: "POST",
    headers: upstreamHeaders,
  },
  (upstreamRes) => {
    // Response handling — see §4.2.4
  },
);

upstreamReq.end(requestBody);
```

The request path from the client (e.g., `/v1/messages`) is appended to the upstream base URL via `new URL(req.url, upstream.baseUrl)`, which handles path resolution correctly.

#### 4.2.4 Response Proxying

When the upstream responds, frigga writes the filtered status code and headers to the client response, then pipes the response body using `stream.pipeline`.

```javascript
import { pipeline } from "node:stream";

// Inside https.request callback:
const isStreaming = upstreamRes.headers["content-type"]?.startsWith(
  "text/event-stream",
);
const responseHeaders = filterResponseHeaders(upstreamRes.headers);
if (isShuttingDown) {
  responseHeaders["connection"] = "close";
}

res.writeHead(upstreamRes.statusCode, responseHeaders);

// Capture non-streaming response body for logging
const responseChunks = isStreaming ? null : [];
if (responseChunks) {
  upstreamRes.on("data", (chunk) => responseChunks.push(chunk));
}

pipeline(upstreamRes, res, () => {
  const responseBody = responseChunks
    ? Buffer.concat(responseChunks).toString("utf-8")
    : null;
  // Emit NDJSON request log (RFC-003 format)
  process.stdout.write(
    `${JSON.stringify({
      timestamp: new Date().toISOString(),
      level: "INFO",
      event: "request",
      method: req.method,
      url: req.url,
      status: upstreamRes.statusCode,
      duration_ms: Date.now() - startTime,
      request_headers: req.headers,
      request_body: requestBody,
      response_headers: res.getHeaders(),
      response_body: responseBody,
    })}\n`,
  );
});
```

**Streaming vs. non-streaming behavior:**

| Response Type | `content-type` | `response_body` in log | Body handling |
|---------------|----------------|------------------------|---------------|
| Non-streaming | `application/json` | Full JSON string | Captured via `data` event listener alongside `pipeline`. |
| Streaming | `text/event-stream` | `null` | Piped directly — not buffered. |

The proxy does not branch on the `stream` parameter in the request body or on `content-type` for piping behavior — it pipes the upstream response through as-is via `stream.pipeline` regardless of content type. The only difference is whether the response body is captured for logging.

#### 4.2.5 Response Header Whitelist

Upstream response headers are filtered before being forwarded to the client. Only whitelisted headers are included.

| Upstream Header | Action | Rationale |
|-----------------|--------|-----------|
| `content-type` | Forward | Client needs to know the response format (`application/json` or `text/event-stream`). |
| `content-length` | Forward if present | Enables the client to allocate buffers. Absent for chunked/streaming responses. |
| `x-request-id` | Forward | Anthropic request tracking — essential for support tickets and debugging. |
| `request-id` | Forward | Alternate request tracking header used by the Anthropic API. |
| `anthropic-ratelimit-*` | Forward (prefix match) | Rate limit visibility — clients need this to implement backoff. |
| `retry-after` | Forward | Rate limit recovery — clients need this to know when to retry. |

All other upstream response headers (e.g., `server`, `date`, `via`, hop-by-hop headers) are dropped.

Implementation as a pure function:

```javascript
const RESPONSE_HEADER_FORWARD = new Set([
  "content-type",
  "content-length",
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

Errors during upstream communication are translated to Anthropic-compatible error responses (same shape as existing error responses in `handleRequest`).

| Condition | Status | `error.type` | Handling |
|-----------|--------|--------------|----------|
| Upstream connection error (DNS, TCP, TLS failure) | `502` | `api_error` | `upstreamReq.on('error')` fires before any upstream response. Send 502 to client. |
| Upstream response stream error (mid-transfer) | — | — | `res.writeHead` already called; cannot change status code. Destroy client connection; log error. |
| Client disconnect during upstream wait | — | — | Abort upstream request via `upstreamReq.destroy()` to stop upstream inference. |
| Client disconnect during response piping | — | — | `pipeline` callback fires with error; `upstreamRes` and the upstream socket are destroyed. |

**502 error response:**

```json
{
  "type": "error",
  "error": {
    "type": "api_error",
    "message": "upstream connection failed"
  }
}
```

The error message is intentionally generic — it does not include upstream host details, error codes, or TLS failure specifics to prevent information leakage. The upstream API key is never included in any error response or log output.

**Error logging:**

Upstream errors are logged to stderr in NDJSON format with a new `upstream_error` event:

```json
{
  "timestamp": "2026-03-17T10:15:32.100Z",
  "level": "ERROR",
  "event": "upstream_error",
  "method": "POST",
  "url": "/v1/messages",
  "message": "ECONNREFUSED"
}
```

#### 4.2.7 Client Disconnect Handling

When the client disconnects mid-request (e.g., Claude Code user pressing ESC), the upstream request must be aborted to stop upstream inference and conserve token usage. Two phases require different handling:

**Phase A — Waiting for upstream response** (`upstreamRes` not yet received):

```javascript
res.on("close", () => {
  if (!res.writableFinished) {
    upstreamReq.destroy();
  }
});
```

**Phase B — Piping upstream response** (`upstreamRes` is being piped to `res`):

`stream.pipeline(upstreamRes, res, callback)` handles this automatically. When `res` is destroyed (client disconnect), pipeline destroys all streams in the chain, including `upstreamRes`. Destroying `upstreamRes` closes the upstream socket, aborting the request.

### 4.3 Design Rationale

**Whitelist over blacklist for request headers** — RFC-001 §8 proposed a blacklist approach ("forward all client request headers by default, removing or rewriting specific categories"). This RFC adopts a whitelist instead. In an enterprise gateway context, the set of headers that *should* reach the upstream is small and well-defined (5 forwarded + 2 replaced + 1 computed = 8 headers total), while the set that *should not* is open-ended and grows with each SDK version, Nginx configuration change, or new client tool. A whitelist provides defense-in-depth: unknown headers are dropped by default, and adding a new forwarded header requires an explicit, auditable change. The blacklist approach has the opposite risk profile — a forgotten header silently leaks to the upstream.

**`accept-encoding` not forwarded to upstream** — Three approaches were evaluated:

| Approach | Description | Pros | Cons |
|----------|-------------|------|------|
| A. **Don't forward** (chosen) | Omit `accept-encoding`; upstream sends uncompressed. | Simple; zero-dependency; response body readable for logging; Nginx handles client-facing compression in production. | Marginally higher bandwidth between frigga and upstream. |
| B. Forward and decompress | Forward `accept-encoding`; decompress upstream response via `node:zlib` before piping to client. | Less bandwidth to upstream. | Complexity; must handle gzip, br, zstd; decompression adds latency; breaks streaming simplicity. |
| C. Forward and pass through | Forward `accept-encoding`; pipe compressed response directly to client. | Minimal proxy overhead. | Cannot log response body; client must support the negotiated encoding; Nginx may double-compress. |

**Approach A** is chosen because:
1. Anthropic API responses are text-based (JSON or SSE) — compression savings are modest relative to the inference latency that dominates overall request time.
2. The frigga-upstream link is a server-to-server HTTPS connection, not a user-facing last-mile connection where compression matters most.
3. In production, Nginx compresses responses for the actual client based on the client's `accept-encoding`. The frigga-Nginx link is local (same host or same network), where compression is unnecessary.
4. In development, uncompressed responses are preferred for debugging with `curl` and `jq`.
5. Not forwarding `accept-encoding` preserves the ability to log non-streaming response bodies in plaintext (RFC-003 compatibility).

**Buffer request body, stream response body** — The request body must be fully buffered before forwarding because: (a) RFC-003 logs the complete request body in the `request` event NDJSON entry, and (b) the enterprise audit requirement mandates that the request body be inspectable for sensitive data before it leaves the gateway. This supersedes RFC-001 §8's zero-copy `stream.pipeline(req, upstreamReq)` design. The response body does not have an audit requirement in the current phase, so it is piped directly to the client via `stream.pipeline(upstreamRes, res)` for low latency and low memory usage.

**`node:https` request over global `fetch`** — Node.js 24 provides a built-in `fetch` (via undici). However, `https.request` is preferred because: (a) it returns Node.js native streams, which integrate directly with `stream.pipeline` and `http.ServerResponse` without web-to-Node stream conversion; (b) it provides event-level control over connection errors and client disconnect propagation (`req.on('error')`, `res.on('close')`); (c) it aligns with the existing event-driven patterns in `startServer`.

**`handleRequest` returns `null` on validation pass** — The pure `handleRequest` function currently returns `{ statusCode: 501, ... }` when validation succeeds. This is changed to return `null`, signaling "validation passed; caller decides next step." The caller (`startServer`) then either forwards to upstream (when `upstream` config is non-null) or returns 501 (when `upstream` is null). This keeps `handleRequest` as a pure validation function with no knowledge of upstream configuration, improving separation of concerns and testability.

**Response body logging for forwarded requests** — For non-streaming responses (`content-type: application/json`), the response body is captured by adding a `data` event listener on `upstreamRes` alongside `stream.pipeline`. Both listeners receive the same chunks because Node.js `EventEmitter` dispatches events to all registered listeners. For streaming responses (`content-type: text/event-stream`), the response body is logged as `null` to avoid unbounded memory growth from long-running SSE streams.

## 5. Interface Changes

**CLI — Before:**
```
node src/index.js --api-key=<key> [--port=<port>] [--host=<host>]
```

**CLI — After:**
```
node src/index.js --api-key=<key> [--port=<port>] [--host=<host>]
                  [--upstream-api-key=<key>] [--upstream-base-url=<url>]
```

**HTTP behavior — Before:** All valid requests return `501 Not Implemented`.

**HTTP behavior — After (with `--upstream-api-key`):** Valid requests are forwarded to the upstream API; the upstream response is returned to the client.

**HTTP behavior — After (without `--upstream-api-key`):** Behavior unchanged — `501 Not Implemented`.

## 6. Backward Compatibility & Migration

- **Breaking changes:** When `--upstream-api-key` is provided, valid requests no longer return `501`. Any test or script relying on the `501` response must be updated. The `handleRequest` function's return type changes from `{ statusCode, headers, body }` to `{ statusCode, headers, body } | null` — callers must handle the `null` case.
- **Migration path:** No action required for existing deployments. The `501` behavior is preserved when `--upstream-api-key` is omitted. To enable forwarding, add `--upstream-api-key=<key>` to the startup command.

## 7. Testing Strategy

Tests are organized into unit tests (pure functions, no I/O) and integration tests (real HTTP servers). All tests use the Node.js built-in test runner (`node --test`), consistent with the zero-runtime-dependency design.

**Unit tests** (`tests/handler.test.js`):
- `buildUpstreamHeaders`: whitelist filtering, credential replacement, handling of missing optional headers
- `filterResponseHeaders`: whitelist filtering, prefix matching, exclusion of non-whitelisted headers
- `handleRequest`: returns `null` for valid requests (replaces existing 501 assertion)

**Integration tests** (`tests/server.test.js`):
- End-to-end forwarding with a local mock upstream HTTP server started per test
- Error handling: mock upstream not listening, mid-stream errors
- Client disconnect: verify upstream request is aborted
- Header whitelisting: verify only whitelisted headers reach mock upstream
- Streaming response: verify SSE events piped through

**Key Scenarios:**

| # | Scenario | Input | Expected Behavior |
|---|----------|-------|-------------------|
| 1 | Non-streaming forward | `POST /v1/messages` with valid auth; mock upstream returns `200` JSON | Client receives `200` with upstream JSON body; request log includes `response_body`. |
| 2 | Streaming forward | `POST /v1/messages` with valid auth; mock upstream returns `200` SSE stream | Client receives `200` with `text/event-stream`; all SSE events piped through; `response_body` logged as `null`. |
| 3 | Request header whitelist | Request includes `anthropic-version`, `x-stainless-arch`, `connection` | Mock upstream receives only `anthropic-version`; `x-stainless-arch` and `connection` are absent. |
| 4 | Authorization replacement | Request with `Authorization: Bearer client-key` | Mock upstream receives `Authorization: Bearer upstream-key`. |
| 5 | Response header whitelist | Mock upstream returns `x-request-id`, `server`, `anthropic-ratelimit-requests-limit` | Client receives `x-request-id` and `anthropic-ratelimit-requests-limit`; `server` is absent. |
| 6 | Upstream connection error | Mock upstream not listening | Client receives `502`; stderr log contains `event: "upstream_error"`. |
| 7 | Client disconnect | Client closes connection after sending request | Mock upstream request is aborted (socket destroyed). |
| 8 | Upstream error response | Mock upstream returns `429` with `retry-after` header | Client receives `429` with `retry-after` header and upstream error body. |
| 9 | No upstream configured | `--upstream-api-key` omitted; valid request | Client receives `501 Not Implemented` (existing behavior). |
| 10 | Shutdown during forwarding | SIGTERM sent while upstream request is in-flight | Response includes `Connection: close`; upstream request completes or is aborted at drain timeout. |
| 11 | Content-Length set correctly | Client sends chunked request body (no `content-length`) | Upstream receives `content-length` header matching the buffered body byte length. |
| 12 | Request body preserved | Client sends JSON body `{"model":"...","messages":[...]}` | Mock upstream receives identical body bytes. |

## 8. Implementation Plan

### Phase 1: Unit Tests — 0.5 day

- [ ] Add tests for `buildUpstreamHeaders` — verify whitelist filtering, `authorization` replacement, handling of missing optional headers (e.g., no `anthropic-beta` in request)
- [ ] Add tests for `filterResponseHeaders` — verify whitelist filtering, `anthropic-ratelimit-*` prefix matching, exclusion of non-whitelisted headers
- [ ] Update existing `handleRequest` tests — change expected result for valid requests from `{ statusCode: 501 }` to `null`

**Done when:** All new unit tests written; updated `handleRequest` tests expect `null` for valid auth (initially failing — red).

### Phase 2: CLI & Constants — 0.5 day

- [ ] Add `DEFAULT_UPSTREAM_BASE_URL` to `src/constants.js`
- [ ] Update `resolveArgs()` in `src/cli.js` to parse and validate `--upstream-api-key` and `--upstream-base-url`
- [ ] Update `printUsage()` to document new arguments
- [ ] Add CLI validation tests — `--upstream-base-url` without `--upstream-api-key` is an error; empty `--upstream-api-key` is an error; invalid `--upstream-base-url` is an error

**Done when:** `resolveArgs` returns `upstream: { apiKey, baseUrl }` or `upstream: null`; invalid argument combinations produce usage error and exit code 1.

### Phase 3: Core Forwarding — 1.5 days

- [ ] Implement `buildUpstreamHeaders()` in `src/server.js`
- [ ] Implement `filterResponseHeaders()` in `src/server.js`
- [ ] Change `handleRequest` to return `null` when validation passes (remove 501 block)
- [ ] Update `startServer` to accept `upstream` in config
- [ ] Implement upstream request construction and response piping in `startServer`'s request callback
- [ ] Implement 502 error handling for upstream connection failures
- [ ] Implement client disconnect detection and upstream request abort
- [ ] Implement `Connection: close` on forwarded responses during shutdown
- [ ] Implement response body capture for non-streaming responses (RFC-003 log compatibility)
- [ ] Implement `upstream_error` NDJSON log entry to stderr
- [ ] Retain 501 response when `upstream` config is null

**Done when:** All Phase 1 unit tests passing (green). Server forwards requests to upstream when `--upstream-api-key` is provided; returns 501 when omitted.

### Phase 4: Integration Tests — 1 day

- [ ] Implement mock upstream HTTP server test fixture (started/stopped per test)
- [ ] Add integration tests for scenarios #1–#12 from §7
- [ ] Verify NDJSON log format for forwarded requests (both streaming and non-streaming)
- [ ] Verify existing integration tests still pass (501 behavior without `--upstream-api-key`)

**Done when:** All integration tests passing. Full scenario coverage from §7.

## 9. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Request body buffering causes memory pressure on large requests | Low | Med | Anthropic API enforces its own request size limits (body is dominated by the prompt, typically < 1 MB). Monitor memory usage; add `--max-request-body-size` guard in a future RFC if needed. |
| Upstream API key exposure in error messages or logs | Low | High | 502 error response uses a generic message with no upstream details. Upstream API key is never included in any log field. `buildUpstreamHeaders` is the only function that handles the key. |
| Whitelist blocks a required header added by a future SDK version | Med | Med | Whitelist is defined as a constant `Set`, making it easy to audit and extend. Add dropped-header logging in a future RFC to detect misses proactively. |
| Non-streaming response body capture causes memory pressure for large responses | Low | Low | Non-streaming API responses are bounded by `max_tokens` (typically < 100 KB JSON). The `data` listener adds proportional memory cost but does not prevent streaming to the client. |
| `stream.pipeline` error handling edge cases (half-open connections, premature close) | Med | Low | Integration tests cover client disconnect and upstream error scenarios. Node.js `stream.pipeline` is battle-tested infrastructure. |

## 10. Future Work

- **Configurable request body audit rules** — Define a pluggable audit interface that can block requests containing sensitive patterns (regex, keyword blocklists) before forwarding. The current design provides the hook point: the request body is fully buffered and available before forwarding.
- **Response body logging for streaming requests** — Capture SSE events in a size-bounded ring buffer for partial response logging without unbounded memory growth.
- **Upstream timeout configuration** — Add `--upstream-connect-timeout` and `--upstream-response-timeout` CLI options for environments where upstream latency bounds are known.
- **Dropped header logging** — Add a DEBUG-level log entry listing headers present in the client request but excluded by the whitelist, to aid in diagnosing SDK compatibility issues.
- **Multiple upstream endpoints** — Support failover or routing across multiple upstream URLs for high availability.

## 11. References

- [RFC-001: Messages API Proxy Service](./rfc_001_api_proxy_service.md) — base service design; §8 Future Work (header strategy, body streaming) superseded by this RFC
- [RFC-003: NDJSON Request Debug Logging](./rfc_003_request_debug_logging.md) — log format and request body collection mechanism
- [LLM Gateway Configuration](https://code.claude.com/docs/en/llm-gateway) — header forwarding requirements (`anthropic-beta`, `anthropic-version`)
- [Anthropic Messages API](https://docs.anthropic.com/en/api/messages)
- [Node.js `stream.pipeline`](https://nodejs.org/api/stream.html#streampipelinesource-transforms-destination-callback)
- [RFC 9110 §7.6.1 — Connection](https://www.rfc-editor.org/rfc/rfc9110.html#name-connection) — hop-by-hop header definition

---

## Changelog

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2026-03-17 | Chason Tang | Initial version |
