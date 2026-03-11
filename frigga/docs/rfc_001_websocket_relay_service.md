# RFC-001: WebSocket Relay Service

**Version:** 1.2  
**Author:** Chason Tang  
**Date:** 2026-03-11  
**Status:** Proposed

---

## 1. Summary

Frigga is a zero-runtime-dependency Node.js CLI service operating in dual modes — server and client — that establishes a WebSocket-based relay for forwarding API requests and streaming SSE responses. The server exposes a `/v1/messages` HTTP endpoint (compatible with the Anthropic Messages API path) with built-in `Authorization` header validation, and a single `/ws` WebSocket upgrade endpoint where the client authenticates via a shared secret. In development, the service runs over plain HTTP/WS; in production, Nginx sits in front and provides TLS termination (HTTPS/WSS). The client connects directly to the server's WebSocket endpoint, authenticating in one step, then receives forwarded requests, injects its own credentials, and streams SSE responses from an upstream server back through the relay.

## 2. Motivation

In scenarios where an API consumer needs to access an upstream service but the required authentication credentials must remain within a controlled environment (e.g., a private network, a developer's local machine), a relay architecture is needed. The upstream service returns SSE streams, requiring the relay to handle streaming data transparently without buffering the entire response.

Existing tunnel solutions (ngrok, Cloudflare Tunnel) are general-purpose, carry operational overhead, and introduce external trust dependencies. A purpose-built relay service with a focused scope provides a minimal attack surface, zero configuration complexity, and full control over the credential injection and streaming behavior.

## 3. Goals and Non-Goals

**Goals:**
- Single CLI entry point with `--mode=server` and `--mode=client` operation modes
- Server mode: `/v1/messages` endpoint for transparent request forwarding (API-consumer-facing), and a single `/ws` WebSocket upgrade endpoint with shared-secret authentication for the relay client
- Dual-layer access control: `--api-key` for API consumer requests on `/v1/messages`, `--client-secret` for relay client WebSocket connections on `/ws` — both validated in the application layer, independent of Nginx
- Client mode: connect to server WebSocket with shared secret, receive forwarded requests, inject `Authorization` header, forward to upstream, and stream SSE responses back
- Zero runtime dependencies — all functionality implemented using Node.js built-in modules (`http`, `https`, `crypto`, `url`, `events`)
- Environment-agnostic: run over plain HTTP/WS in development; production TLS (HTTPS/WSS) is handled entirely by Nginx with no code change
- Transparent SSE passthrough with chunked streaming (no full-response buffering)

**Non-Goals:**
- Multi-client support — initial version supports exactly one connected client; a single WebSocket reference is sufficient
- TLS termination — delegated entirely to Nginx in production; the server always binds plain HTTP
- Persistent message queuing — if no client is connected, `/v1/messages` returns 503 immediately rather than queuing

## 4. Design

### 4.1 Overview

Frigga operates identically in both environments; the only difference is the network layer in front of it.

**Development** (no Nginx, direct HTTP/WS):

```
Frigga Client                      Frigga Server                                API Consumer
     |                                  |                                            |
     | WS /ws?secret=<client-secret>    |                                            |
     |--------------------------------->| validate secret, 101 Switching Protocols    |
     |<---------------------------------|                                            |
     |                                  |                                            |
     |                                  |    POST /v1/messages                       |
     |                                  |    Authorization: Bearer <api-key>         |
     |                                  |<-------------------------------------------|
     |                                  | validate auth                              |
     |  WS text: {type:"request",...}   |                                            |
     |<---------------------------------|                                            |
     |                                  |                                            |
     | POST /v1/messages + Auth         |                                            |
     |-----------> Upstream Server      |                                            |
     |<--- SSE stream                   |                                            |
     |                                  |                                            |
     |  WS text: {type:"sse-chunk",...} |                                            |
     |--------------------------------->| res.write(data)                            |
     |                                  |------------------------------------------->|
     |  WS text: {type:"sse-end",...}   |                                            |
     |--------------------------------->| res.end()                                  |
     |                                  |------------------------------------------->|
```

**Production** (Nginx TLS termination in front):

```
API Consumer        Nginx (TLS)          Frigga Server          Frigga Client          Upstream Server
     |                   |                     |                      |                      |
     | HTTPS             | proxy_pass HTTP     |                      |                      |
     |------------------>|-------------------->| (same flow as above) |                      |
     |                   |                     |                      |                      |
     | WSS               | proxy_pass WS       |                      |                      |
     |                   |  (Upgrade headers)  |                      |                      |
```

The Frigga server always binds plain HTTP. Nginx, when present, terminates TLS and proxies to the server. The client adapts its transport (`http`/`https`, `ws:`/`wss:`) based on the `--server-url` scheme.

**Component responsibilities:**

| Component | Role | Present in |
|-----------|------|------------|
| Nginx | TLS termination, WebSocket upgrade proxying (`Upgrade`, `Connection` headers) | Production only |
| Frigga Server | API consumer authorization, client secret validation, request-to-WebSocket dispatch, SSE response streaming to HTTP caller | Both |
| Frigga Client | WebSocket connection with secret, credential injection, upstream request execution, SSE-to-WebSocket streaming | Both |

### 4.2 Detailed Design

#### 4.2.1 CLI Interface

The CLI entry point (`src/index.js`) dispatches to server or client mode based on the `--mode` argument. Arguments use the `--key=value` form exclusively (no space-separated values), parsed by a minimal hand-written parser operating on `process.argv`.

**Server mode:**
```
node src/index.js --mode=server --api-key=<key> --client-secret=<secret> [--port=<port>] [--host=<host>]
```

| Argument | Required | Default | Description |
|----------|----------|---------|-------------|
| `--mode` | Yes | — | `server` |
| `--api-key` | Yes | — | Expected value of the `Authorization` header on `/v1/messages` requests. Requests with a mismatched or missing header receive `401`. |
| `--client-secret` | Yes | — | Shared secret that the relay client must present in the WebSocket connection URL to authenticate. |
| `--port` | No | `3000` | HTTP listen port |
| `--host` | No | `0.0.0.0` | HTTP listen address |

**Client mode:**
```
node src/index.js --mode=client --server-url=<url> --upstream-url=<url> --api-key=<key> --client-secret=<secret>
```

| Argument | Required | Default | Description |
|----------|----------|---------|-------------|
| `--mode` | Yes | — | `client` |
| `--server-url` | Yes | — | Frigga server base URL. Use `http://` in development, `https://` in production. The client derives the WebSocket URL automatically (`ws:`/`wss:` based on the scheme). |
| `--upstream-url` | Yes | — | Target upstream server base URL (e.g., `https://api.anthropic.com`) |
| `--api-key` | Yes | — | Credential injected as `Authorization: Bearer <api-key>` into upstream requests |
| `--client-secret` | Yes | — | Shared secret presented to the server during WebSocket connection for authentication |

#### 4.2.2 Server Mode

The server creates a single `http.createServer()` instance that handles both regular HTTP requests and WebSocket upgrades (via the `upgrade` event) on the same port. The server exposes exactly two endpoints:

| Path | Method | Purpose |
|------|--------|---------|
| `/v1/messages` | POST | API consumer request forwarding (SSE relay) |
| `/ws` | GET (Upgrade) | Relay client WebSocket connection |

**`POST /v1/messages` — Request Forwarding**

From the API consumer's perspective, this endpoint behaves identically to the upstream Messages API. The consumer sends a standard request and receives an SSE stream in return. The server acts as a transparent relay.

Authorization validation:
1. Extract the `Authorization` header from the incoming request
2. Compare it against the `--api-key` value provided at startup (constant-time comparison via `crypto.timingSafeEqual`)
3. If missing or mismatched → `401 Unauthorized`

Request processing:
1. Buffer the full request body (the body is always JSON for the Messages API)
2. Generate a request ID (`crypto.randomUUID()`)
3. Wrap as a WebSocket message: `{ type, id, headers, body }` (see §4.2.4)
4. Send to the connected client via WebSocket
5. Hold the HTTP response open; set a timeout (default 120s)
6. As the client sends back `sse-start`, `sse-chunk`, `sse-end` messages, write them to the HTTP response
7. On `sse-end`, call `response.end()`; on timeout or `sse-error`, respond with the appropriate HTTP error

Response: SSE stream (`Content-Type: text/event-stream`) if the client successfully connects to upstream, or a JSON error body for failures.

Error responses:
- `401` — Missing or invalid `Authorization` header
- `503` — No client connected
- `504` — Client did not respond within timeout

**`GET /ws?secret=<secret>` — WebSocket Upgrade**

The relay client connects to this endpoint to establish the persistent WebSocket channel. Authentication and connection are handled in a single step — no separate registration endpoint is needed.

The server intercepts the HTTP `upgrade` event on the `/ws` path. Validation:
1. Extract `secret` from the query parameter
2. Compare against the `--client-secret` value (constant-time comparison via `crypto.timingSafeEqual`)
3. If missing or mismatched → respond `HTTP/1.1 401 Unauthorized\r\n\r\n` and destroy the socket (the WebSocket upgrade is never completed)
4. Check `Upgrade: websocket` and `Connection: upgrade` headers
5. Verify `Sec-WebSocket-Version: 13`
6. Compute `Sec-WebSocket-Accept` from `Sec-WebSocket-Key` per RFC 6455 §4.2.2

On success, respond with `101 Switching Protocols` and establish the WebSocket connection. If a previous client connection exists, it is closed before the new one is accepted (single-client invariant).

On failure:
- Missing or invalid secret → `401 Unauthorized` (before upgrade)
- Malformed WebSocket headers → `400 Bad Request`

#### 4.2.3 Client Mode — Connection Lifecycle

```
┌─────────┐  WS connect /ws?secret=xxx  ┌──────────┐
│  Init   │ ──────────────────────────> │ Connected │ <───┐
└─────────┘                             └────┬──────┘     │
                                             │            │ reconnect
                                        WS close /       │ (exponential backoff)
                                        error            │
                                             │            │
                                             v            │
                                       ┌─────────────┐   │
                                       │ Reconnecting│───┘
                                       └─────────────┘

                                   While connected:

                              receive {type:"request"}
                                        │
                                        v
                              ┌──────────────────┐
                              │ Inject Auth       │
                              │ POST upstream     │
                              │ Stream SSE back   │
                              └──────────────────┘
```

1. **Connect**: Derive WebSocket URL from `--server-url` (`http:` → `ws:`, `https:` → `wss:`), append `/ws?secret=<client-secret>`, and open a WebSocket connection using the Node.js 22+ built-in `WebSocket` API. Authentication occurs during the upgrade handshake — the server validates the secret before completing the 101 response.
2. **Listen**: Wait for messages on the WebSocket.
3. **Process**: For each received `request` message:
   - Set the `Authorization` header to `Bearer <api-key>` (replacing any existing value)
   - Send an HTTP POST to `<upstream-url>/v1/messages` with the forwarded headers and body
   - On receiving the upstream response, send `sse-start` (status + headers) over WebSocket
   - For each data chunk from the upstream response stream, send `sse-chunk` over WebSocket
   - On upstream response end, send `sse-end` over WebSocket
   - On upstream error, send `sse-error` over WebSocket
4. **Reconnect**: On WebSocket close or error, wait with exponential backoff (1s, 2s, 4s, …, capped at 30s), then repeat from step 1.

#### 4.2.4 WebSocket Message Protocol

All messages are JSON-encoded WebSocket text frames. A `type` field discriminates message kinds. An `id` field (UUID v4, generated by the server) correlates a forwarded request with its SSE response stream.

**Server → Client:**

| Type | Fields | Description |
|------|--------|-------------|
| `request` | `id`, `headers`, `body` | Forwarded `/v1/messages` request. `headers` contains forwarded HTTP headers (excluding `Authorization` and `Host`). `body` is the parsed JSON request body. |
| `ping` | — | Application-level keepalive |

**Client → Server:**

| Type | Fields | Description |
|------|--------|-------------|
| `sse-start` | `id`, `status`, `headers` | Upstream response initiated. `status` is the HTTP status code. `headers` contains selected response headers (e.g., `content-type`). |
| `sse-chunk` | `id`, `data` | Raw SSE data chunk from upstream (string, preserving original line breaks and formatting). |
| `sse-end` | `id` | Upstream response stream completed. |
| `sse-error` | `id`, `status`, `message` | Upstream request failed or errored mid-stream. |
| `pong` | — | Application-level keepalive response |

Example exchange:

```
Server → Client:
{
  "type": "request",
  "id": "f47ac10b-58cc-4372-a567-0e02b2c3d479",
  "headers": {
    "content-type": "application/json",
    "anthropic-version": "2023-06-01"
  },
  "body": {
    "model": "claude-sonnet-4-20250514",
    "max_tokens": 1024,
    "stream": true,
    "messages": [{"role": "user", "content": "Hello"}]
  }
}

Client → Server:
{"type": "sse-start", "id": "f47ac10b-...", "status": 200, "headers": {"content-type": "text/event-stream"}}
{"type": "sse-chunk", "id": "f47ac10b-...", "data": "event: message_start\ndata: {\"type\":\"message_start\",...}\n\n"}
{"type": "sse-chunk", "id": "f47ac10b-...", "data": "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",...}\n\n"}
{"type": "sse-end", "id": "f47ac10b-..."}
```

#### 4.2.5 WebSocket Server Implementation

The server-side WebSocket is implemented using Node.js built-in modules, handling the RFC 6455 protocol at the frame level. No third-party WebSocket library is used.

**Handshake** (via `http` server `upgrade` event):

The server computes `Sec-WebSocket-Accept` per RFC 6455 §4.2.2:

```javascript
const WEBSOCKET_GUID = '258EAFA5-E914-47DA-95CA-5AB5DC65C3F5';
const accept = crypto.createHash('sha1')
    .update(secWebSocketKey + WEBSOCKET_GUID)
    .digest('base64');
```

Then responds with `101 Switching Protocols` and the required upgrade headers.

**Frame codec** — supported opcodes:

| Opcode | Name | Usage |
|--------|------|-------|
| `0x1` | Text | Primary data channel (JSON messages) |
| `0x8` | Close | Connection teardown |
| `0x9` | Ping | Transport-level keepalive |
| `0xA` | Pong | Transport-level keepalive response |

Client-to-server frames are always masked (per RFC 6455 §5.3). Server-to-client frames are unmasked. The frame decoder accumulates partial data and yields complete frames, handling the three payload-length encodings (7-bit, 16-bit extended, 64-bit extended).

**Client-side WebSocket**: Uses the Node.js 22+ built-in `WebSocket` global (based on undici). No manual frame handling is needed on the client side.

#### 4.2.6 SSE Relay Pipeline

The SSE relay avoids buffering by operating as a streaming pipeline:

```
Upstream Server              Frigga Client                     Frigga Server              API Consumer
     |                            |                                 |                          |
     | HTTP response (chunked)    |                                 |                          |
     |--- chunk 1 -------------->| WS: {"type":"sse-chunk",...}     |                          |
     |                            |-------------------------------->| res.write(data)          |
     |                            |                                 |------------------------->|
     |--- chunk 2 -------------->| WS: {"type":"sse-chunk",...}     |                          |
     |                            |-------------------------------->| res.write(data)          |
     |                            |                                 |------------------------->|
     |--- END ------------------>| WS: {"type":"sse-end",...}       |                          |
     |                            |-------------------------------->| res.end()                |
     |                            |                                 |------------------------->|
```

Each data chunk from the upstream response stream is wrapped in a JSON envelope (`sse-chunk`) and sent as a WebSocket text frame. The server extracts the `data` field and writes it directly to the pending HTTP response using `response.write()`. When `sse-end` arrives, `response.end()` is called. No intermediate buffering occurs — data flows through the relay as fast as the slowest link allows.

#### 4.2.7 Project Structure

The minimal project layout targets Phase 1 completion (see §5):

```
frigga/
├── package.json
├── docs/
│   └── rfc_001_websocket_relay_service.md
└── src/
    ├── index.js
    ├── cli.js
    ├── constants.js
    ├── ws.js
    ├── server.js
    └── client.js
```

**`package.json`** — Project manifest with zero runtime `dependencies`. Sets `"type": "module"` for ES module support and `"engines": { "node": ">=22.0.0" }` to enforce the minimum Node.js version required for the built-in `WebSocket` API.

**`src/index.js`** — Shebang entry point (`#!/usr/bin/env node`). Parses CLI arguments via `cli.js`, validates `--mode`, and dispatches to `startServer()` or `startClient()`. Installs `SIGINT`/`SIGTERM` handlers for graceful shutdown.

**`src/cli.js`** — Pure-function module for CLI concerns:
- `parseArgs(argv)` → `Record<string, string | true>` — iterate `process.argv`, extract `--key=value` and `--flag` forms.
- `printUsage()` — write usage text to `stderr`.
- `resolveServerArgs(args)` → `{ port, host, apiKey, clientSecret }` — validate and extract server-mode arguments with defaults.
- `resolveClientArgs(args)` → `{ serverUrl, upstreamUrl, apiKey, clientSecret }` — validate required client-mode arguments; exit with usage on missing values.

**`src/constants.js`** — Shared constants exported as named values:
- Network defaults: `DEFAULT_SERVER_PORT` (3000), `DEFAULT_SERVER_HOST` ('0.0.0.0')
- WebSocket: `WEBSOCKET_GUID`, `WS_OPCODE` enum (TEXT, CLOSE, PING, PONG)
- Timing: `HEARTBEAT_INTERVAL_MS` (30 000), `HEARTBEAT_TIMEOUT_MS` (10 000), `FORWARD_TIMEOUT_MS` (120 000), `RECONNECT_BASE_MS` (1 000), `RECONNECT_MAX_MS` (30 000)

**`src/ws.js`** — WebSocket server implementation:
- `computeAcceptKey(secWebSocketKey)` → `string` — SHA-1 hash per RFC 6455.
- `encodeFrame(opcode, payload)` → `Buffer` — encode an unmasked server-to-client frame.
- `class FrameDecoder` — stateful decoder that accumulates incoming `Buffer` data and yields `{ opcode, payload }` frames, handling masking and all three payload-length formats.
- `class WebSocketConnection extends EventEmitter` — wraps a raw `net.Socket` post-handshake. Emits `'message'` (string), `'close'`, `'error'`. Provides `send(string)`, `close(code, reason)`. Manages transport-level ping/pong heartbeat internally.
- `upgradeToWebSocket(req, socket, head)` → `WebSocketConnection | null` — perform the server-side handshake; return `null` on invalid upgrade request.

**`src/server.js`** — Server mode:
- `startServer({ port, host, apiKey, clientSecret })` — create HTTP server, register `/v1/messages` route, attach `upgrade` handler for `/ws` with secret validation, manage single-client state (connected WebSocket reference, pending requests map).

**`src/client.js`** — Client mode:
- `startClient({ serverUrl, upstreamUrl, apiKey, clientSecret })` — connect WebSocket to server with secret, listen for `request` messages, forward to upstream with credential injection, stream SSE back. Implements auto-reconnect with exponential backoff.

### 4.3 Design Rationale

**Merged `/register` + `/ws` into a single `/ws` endpoint**: The original two-step flow (POST `/register` to obtain a token, then connect WebSocket with that token) introduced an extra round trip, token lifecycle management (generation, storage, consumption, expiry), and a separate HTTP endpoint — all to serve a single-client scenario. By merging into a single WebSocket upgrade with a shared secret in the query parameter, the client connects and authenticates in one step. The token-generation code, the pending-token store, and the `/register` route handler are eliminated entirely.

**Shared secret over one-time token**: A one-time token (register → consume) prevents replay but requires the registration round trip. A shared secret (`--client-secret`) is replayable by definition, but in practice this is not a meaningful threat for the single-client model: (a) only one connection is accepted at a time — a replay would replace the legitimate client, which would immediately attempt to reconnect and be detected, (b) in production, TLS encrypts the WebSocket URL including the query parameter, and (c) the secret is a server-startup configuration, not a user-facing credential. If one-time token semantics are needed in the future (e.g., multi-client with untrusted networks), the `/register` endpoint can be reintroduced without protocol changes.

**Secret in query parameter over custom headers**: The Node.js 22+ built-in `WebSocket` API (WHATWG-compliant) does not support custom request headers. The available options for passing the secret are: (a) query parameter — simple, works everywhere, (b) `Sec-WebSocket-Protocol` subprotocol hack — technically works but abuses the semantics of the header, (c) post-connect auth message — adds an unauthenticated connection window and extra protocol state. The query parameter is the most straightforward. The secret is visible in the URL but is protected by TLS in production and is localhost-only in development. Server access logs can be configured to exclude query parameters if needed.

**Constant-time secret comparison**: Both `--api-key` and `--client-secret` comparisons use `crypto.timingSafeEqual` to prevent timing side-channel attacks. While the practical risk is low for a relay service, the implementation cost is one line of code and it eliminates an entire class of vulnerabilities.

**Endpoint path `/v1/messages` instead of a generic `/forward`**: By exposing the same path as the upstream Messages API, the Frigga server is transparent to the API consumer. Any client SDK or tool configured to call the Anthropic API can be redirected to Frigga simply by changing the base URL — no request-wrapping or format changes required.

**Server-side `Authorization` validation on `/v1/messages`**: Even though Nginx can enforce access control in production, the server must also validate authorization because: (a) development environments have no Nginx, (b) defense-in-depth prevents misconfigured Nginx from exposing the relay, and (c) it keeps the access control logic explicit and testable in the application layer.

**Environment-agnostic HTTP server**: The Frigga server always binds plain HTTP. This keeps the implementation simple (no certificate management, no TLS configuration) and follows the standard reverse-proxy deployment pattern. In development, the API consumer connects directly over HTTP/WS. In production, Nginx handles TLS and proxies to the same HTTP server. The client's `--server-url` scheme (`http:`/`https:`) is the only configuration difference between environments.

**WebSocket over HTTP long-polling**: The relay pattern requires server-initiated messages (pushing forwarded requests to the client). WebSocket provides full-duplex communication with lower overhead than polling. The persistent connection also simplifies keepalive and connection state management.

**Built-in WebSocket implementation over `ws` library**: Aligns with the zero-dependency constraint. The WebSocket frame protocol is well-documented (RFC 6455), and a text-frame-only server implementation is approximately 200–300 lines. Node.js 22+ provides a built-in `WebSocket` client (stable since v22), so only the server side requires manual frame handling.

**JSON text frames over binary protocol**: JSON messages are human-readable, easily logged for debugging, and trivially extensible with new fields. Binary framing (e.g., MessagePack) would reduce serialization overhead but adds complexity disproportionate to expected traffic. This can be revisited if throughput benchmarking reveals a bottleneck.

**Application-level ping/pong alongside WebSocket-level ping/pong**: WebSocket protocol ping/pong frames verify transport-level liveness (TCP connection alive). Application-level ping/pong messages verify that the client's event loop is responsive. Both are implemented for defense-in-depth.

## 5. Implementation Plan

### Phase 1: Project Scaffolding & CLI — 0.5 day

- [ ] Create `frigga/package.json` — zero runtime dependencies, `"type": "module"`, `"engines": { "node": ">=22.0.0" }`
- [ ] Implement `src/cli.js` — `parseArgs()`, `printUsage()`, `resolveServerArgs()`, `resolveClientArgs()`
- [ ] Implement `src/constants.js` — all shared constants
- [ ] Implement `src/index.js` — shebang, parse args, validate `--mode`, dispatch to server/client, register signal handlers
- [ ] Create stubs `src/server.js` (`startServer()` prints config and listens) and `src/client.js` (`startClient()` prints config)

**Done when:**
- `node src/index.js --mode=server --api-key=test --client-secret=s3cret --port=3000` prints server config to stderr and starts listening
- `node src/index.js --mode=client --server-url=http://localhost:3000 --upstream-url=https://api.example.com --api-key=sk-xxx --client-secret=s3cret` prints client config to stderr
- Missing or invalid arguments produce a usage message and exit code 1

### Phase 2: WebSocket Server Implementation — 2 days

- [ ] Implement `computeAcceptKey()` in `src/ws.js`
- [ ] Implement `encodeFrame()` — unmasked server-to-client frames (7-bit, 16-bit, 64-bit payload lengths)
- [ ] Implement `FrameDecoder` — accumulate data, decode masked client-to-server frames, yield `{ opcode, payload }` tuples
- [ ] Implement `WebSocketConnection` class — message emit, send, close, ping/pong heartbeat
- [ ] Implement `upgradeToWebSocket()` — validate headers, write 101 response, return `WebSocketConnection`
- [ ] Integration test: connect with browser `WebSocket` or `wscat`, exchange messages, close cleanly

**Done when:** A standard WebSocket client can connect to the Frigga server, send and receive text messages, and disconnect with a clean close handshake. Ping/pong keepalive is observable in debug logs.

### Phase 3: Server Mode Core — 1 day

- [ ] `upgrade` handler on `/ws` — extract and validate `secret` query parameter via `crypto.timingSafeEqual`, call `upgradeToWebSocket()`, store client reference; reject with 401 if secret is invalid
- [ ] `POST /v1/messages` — validate `Authorization` header, buffer body, generate request ID, send over WebSocket, hold response open with timeout
- [ ] WebSocket message handler — dispatch `sse-start`/`sse-chunk`/`sse-end`/`sse-error` to pending HTTP responses

**Done when:** Server validates client secret on WebSocket upgrade, validates API key on `/v1/messages`, and can forward a request body to the client and stream an SSE response back to the HTTP caller (tested with a mock client sending canned SSE data over WebSocket).

### Phase 4: Client Mode Core — 1 day

- [ ] `connectWebSocket()` — derive WS URL from `--server-url` scheme, append `/ws?secret=<client-secret>`, connect using built-in `WebSocket` API, attach message handlers
- [ ] `processRequest()` — extract headers/body from `request` message, inject `Authorization: Bearer <api-key>`, POST to `<upstream-url>/v1/messages`, stream response as `sse-start`/`sse-chunk`/`sse-end`
- [ ] `scheduleReconnect()` — exponential backoff, re-connect

**Done when:** Full end-to-end flow: API consumer → `POST /v1/messages` on server → client receives via WebSocket → client hits upstream → SSE streams back through WebSocket → API consumer receives SSE. Tested against a real upstream or a local mock SSE server.

### Phase 5: Reliability & Hardening — 1 day

- [ ] Application-level ping/pong (30s interval, 10s timeout)
- [ ] Client auto-reconnect with exponential backoff (1s base, 30s cap)
- [ ] Graceful shutdown on SIGINT/SIGTERM — close WebSocket with code 1001, drain active forwarded requests, then close HTTP server
- [ ] Forward request timeout (120s default) — respond 504 if client does not complete SSE stream in time
- [ ] Edge-case error handling: upstream unreachable, malformed WebSocket frames, mid-stream client disconnection, response already started when error occurs

**Done when:** Service survives client disconnection/reconnection, shuts down cleanly under signal, and returns appropriate HTTP errors for all failure modes.

## 6. Testing Strategy

Primary approach: manual integration testing with two terminal sessions (server + client). Automated unit tests for the WebSocket frame codec and CLI argument parser using the Node.js built-in test runner (`node --test`).

**Key Scenarios:**

| # | Scenario | Input | Expected Behavior |
|---|----------|-------|-------------------|
| 1 | Valid `/v1/messages` with connected client | `POST /v1/messages` with correct `Authorization` | SSE stream relayed from upstream |
| 2 | Missing Authorization on `/v1/messages` | `POST /v1/messages` without header | `401 Unauthorized` |
| 3 | Wrong Authorization on `/v1/messages` | `POST /v1/messages` with wrong key | `401 Unauthorized` |
| 4 | No client connected | `POST /v1/messages` | `503 Service Unavailable` |
| 5 | WebSocket with valid secret | `GET /ws?secret=<correct>` | `101 Switching Protocols` |
| 6 | WebSocket with invalid secret | `GET /ws?secret=wrong` | `401 Unauthorized`, upgrade never completes |
| 7 | WebSocket with missing secret | `GET /ws` (no query param) | `401 Unauthorized` |
| 8 | Client disconnects, reconnects | Kill client, wait | Client re-connects via WebSocket with same secret |
| 9 | Second client replaces first | New client connects while one is active | Old connection closed, new one accepted |
| 10 | Large SSE stream | Multi-MB upstream response | All chunks delivered in order, no truncation |
| 11 | Upstream error | Upstream returns 500 | `sse-error` relayed, caller receives error |
| 12 | Request timeout | Client never responds | `504 Gateway Timeout` after 120s |
| 13 | Graceful shutdown | SIGINT during active stream | Active stream completes, then process exits |
| 14 | Dev environment (HTTP/WS) | `--server-url=http://localhost:3000` | Client connects over plain WS, full flow works |

## 7. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| WebSocket frame parser bugs (masking, boundary lengths, fragmentation) | Med | High | Unit tests covering: 0-byte payload, 125-byte boundary, 126-byte (16-bit extended), 65536-byte (64-bit extended); validate against `wscat` and browser WebSocket clients |
| SSE data corruption during relay (encoding issues, truncated chunks) | Low | High | Byte-level comparison of upstream SSE output vs. relayed output in integration tests |
| Client secret exposure in server access logs | Low | Med | Document log configuration guidance; in production, Nginx can be configured to strip query parameters from access logs; secret is always encrypted in transit (TLS) |
| Memory pressure from large SSE responses | Med | Med | Pure streaming pipeline with no full-response buffering; observe `response.write()` return value for backpressure |
| Node.js built-in `WebSocket` API instability | Low | Med | Pin `engines.node >= 22` (LTS); wrap client WebSocket usage behind a thin abstraction for easy replacement |
| Single-client bottleneck under concurrent `/v1/messages` requests | Med | Med | Document as known limitation; multiple concurrent requests are multiplexed over the single WebSocket using distinct `id` values, so throughput is limited by WebSocket bandwidth, not connection count |

## 8. Future Work

- **Multi-client support** with routing strategy (round-robin, affinity) and `clientId`-based dispatch — deferred until concurrent load justifies routing complexity; reintroduce `/register` endpoint for client identity management
- **Token-based WebSocket authentication** replacing shared secret — reintroduce `/register` → one-time-token → `/ws` flow if replay resistance becomes a requirement (e.g., multi-client with untrusted network segments)
- **Binary WebSocket frames** for reduced JSON serialization overhead — requires benchmarking to justify added codec complexity
- **Request queuing** with configurable depth when client is temporarily unavailable — current fail-fast (503) is safer and simpler for the initial version
- **WebSocket compression** (`permessage-deflate`) — trade CPU for bandwidth; defer until network constraints are measured
- **Prometheus-compatible `/metrics` endpoint** — deferred until production deployment patterns are established

## 9. References

- [RFC 6455 — The WebSocket Protocol](https://datatracker.ietf.org/doc/html/rfc6455)
- [Server-Sent Events Specification](https://html.spec.whatwg.org/multipage/server-sent-events.html)
- [Node.js 22 — WebSocket API](https://nodejs.org/docs/latest-v22.x/api/globals.html#websocket)
- [Node.js — HTTP `upgrade` Event](https://nodejs.org/docs/latest-v22.x/api/http.html#event-upgrade)
- [Nginx — WebSocket Proxying](https://nginx.org/en/docs/http/websocket.html)
- [Anthropic Messages API](https://docs.anthropic.com/en/api/messages)

---

## Changelog

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.2 | 2026-03-11 | Chason Tang | Merge `/register` + `/ws` into single `/ws` endpoint with shared-secret authentication; add `--client-secret` CLI option to both modes; eliminate token lifecycle; add `crypto.timingSafeEqual` for all secret comparisons; simplify client lifecycle (remove registration step) |
| 1.1 | 2026-03-11 | Chason Tang | Rename `/forward` to `/v1/messages`; add server-side Authorization validation via `--api-key`; remove `clientId` (single-client simplification); add dev/prod deployment modes; add §4.2.7 Project Structure; restructure CLI arguments |
| 1.0 | 2026-03-11 | Chason Tang | Initial version |
