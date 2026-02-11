# Implement POST /v1/messages/count_tokens Endpoint (501 Not Implemented)

**Document Version:** 1.0  
**Author:** Chason Tang  
**Last Updated:** 2026-02-09  
**Status:** Implemented

---

## 1. Executive Summary

This document describes the implementation of the `POST /v1/messages/count_tokens` endpoint in Odin. The endpoint is part of the Anthropic Messages API surface but is not currently supported by Odin. Rather than letting it fall through to the generic 404 handler, we explicitly register the route and return a `501 Not Implemented` response with a clear error message.

### 1.1 Background

Claude Code (and other Anthropic API clients) may send requests to `POST /v1/messages/count_tokens` to estimate token usage before making a full messages request. Since Odin acts as an Anthropic-compatible proxy that translates requests to Google Cloud Code, and Google Cloud Code does not expose a direct token-counting equivalent, this endpoint cannot be meaningfully implemented at this time.

Currently, hitting this endpoint results in a generic `404 Unknown endpoint` response. A dedicated `501` response is more semantically correct and gives clients a clear signal that the endpoint is recognized but intentionally unimplemented.

### 1.2 Goals

- **Primary**: Return `501 Not Implemented` for `POST /v1/messages/count_tokens` with a well-formed Anthropic error response.
- **Secondary**: Improve client-facing clarity by distinguishing "recognized but unsupported" from "unknown endpoint".
- **Non-Goals**: Actually counting tokens or proxying the request to any upstream service.

### 1.3 Key Features

| Feature | Current | Target |
|---------|---------|--------|
| `POST /v1/messages/count_tokens` | 404 Not Found (generic) | 501 Not Implemented (explicit) |
| Error message clarity | "Unknown endpoint" | "The /v1/messages/count_tokens endpoint is not implemented" |

---

## 2. Technical Design

### 2.1 Architecture Overview

No architectural changes are required. The implementation adds a new route handler in the existing strict routing section of `src/server.js`, following the same pattern as existing endpoints.

```
┌─────────────────────────────────────────────────────────────────┐
│                         Odin HTTP Server                        │
├─────────────────────────────────────────────────────────────────┤
│  GET  /health                          → 200 OK                 │
│  POST /v1/messages                     → Proxy to Cloud Code    │
│  POST /v1/messages/count_tokens        → 501 Not Implemented    │ ← NEW
│  POST /                                → 200 (heartbeat)        │
│  POST /api/event_logging/batch         → 200 (telemetry)        │
│  *                                     → 404 Unknown endpoint   │
└─────────────────────────────────────────────────────────────────┘
```

### 2.2 Core Logic

The handler is minimal — it matches the route and immediately returns a `501` error response using the existing `sendError` helper:

```javascript
if (method === 'POST' && path === '/v1/messages/count_tokens') {
    sendError(res, 501, 'not_implemented_error',
        'The /v1/messages/count_tokens endpoint is not implemented.');
    logResponse(req, 501, startTime);
    logger?.log({ req, body, statusCode: 501, startTime });
    return;
}
```

### 2.3 Route Placement

The new route **must** be placed before the existing `POST /v1/messages` handler in the routing chain. This is because `/v1/messages/count_tokens` is a more specific path — if `POST /v1/messages` were checked first with a simple `path ===` comparison, it would not conflict, but placing the more specific route first follows conventional best practice and guards against any future change to prefix-based matching.

Recommended insertion point in `src/server.js`:

```
GET  /health              (existing)
POST /v1/messages/count_tokens   ← INSERT HERE
POST /v1/messages         (existing)
POST /                    (existing)
...
```

---

## 3. Interface Design

### 3.1 API Interface

```
POST /v1/messages/count_tokens HTTP/1.1
Content-Type: application/json

(any body — body content is ignored)
```

### 3.2 Output Format

**Response (501 Not Implemented):**

```json
{
    "type": "error",
    "error": {
        "type": "not_implemented_error",
        "message": "The /v1/messages/count_tokens endpoint is not implemented."
    }
}
```

**HTTP Headers:**

```
HTTP/1.1 501 Not Implemented
Content-Type: application/json
```

### 3.3 Error Handling

| HTTP Status | Error Type | Condition | Message |
|-------------|------------|-----------|---------|
| 501 | `not_implemented_error` | Any request to this endpoint | "The /v1/messages/count_tokens endpoint is not implemented." |

---

## 4. Implementation Plan

### Phase 1: Add Route Handler (Estimated: 0.5 hours)

**Task 1.1: Add route in `src/server.js`**
- [x] Add the `POST /v1/messages/count_tokens` route block before the `POST /v1/messages` handler
- [x] Use the existing `sendError` helper with status code `501`
- [x] Include `logResponse` and `logger?.log` calls consistent with other routes

**Acceptance Criteria:**
- `POST /v1/messages/count_tokens` returns HTTP 501 with the specified JSON body
- The response is logged via both `logResponse` and the request logger
- Existing endpoints remain unaffected

---

## 5. Testing

### 5.1 Test Cases

| Test Scenario | Input | Expected Output |
|---------------|-------|-----------------|
| Basic request | `POST /v1/messages/count_tokens` with valid JSON body | 501 with `not_implemented_error` |
| Empty body | `POST /v1/messages/count_tokens` with no body | 501 with `not_implemented_error` |
| With query string | `POST /v1/messages/count_tokens?foo=bar` | 501 with `not_implemented_error` |
| GET method | `GET /v1/messages/count_tokens` | 404 (falls through to unknown endpoint handler) |
| Existing messages endpoint | `POST /v1/messages` with valid streaming body | 200 (unchanged behavior) |

### 5.2 Manual Verification

```bash
# Test the count_tokens endpoint
curl -X POST http://localhost:PORT/v1/messages/count_tokens \
  -H "Content-Type: application/json" \
  -d '{"model":"claude-sonnet-4-5-thinking","messages":[{"role":"user","content":"Hello"}]}' \
  -w "\nHTTP Status: %{http_code}\n"

# Expected output:
# {"type":"error","error":{"type":"not_implemented_error","message":"The /v1/messages/count_tokens endpoint is not implemented."}}
# HTTP Status: 501
```

---

## 6. Risk Assessment

### 6.1 Risks and Mitigations

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| Client retries on 501 indefinitely | Low | Low | 501 is a clear "not implemented" signal; well-behaved clients will not retry |
| Route ordering conflict with `/v1/messages` | Low | Medium | Place `/v1/messages/count_tokens` before `/v1/messages` in the routing chain |

---

## 7. Future Considerations

### 7.1 Potential Extensions

| Feature | Status | Notes |
|---------|--------|-------|
| Actual token counting via Google API | 💡 Idea | If Google Cloud Code adds a token counting API, this endpoint could be wired up |
| Local tokenizer-based counting | 💡 Idea | Use a local tokenizer library to estimate token counts without upstream calls |

---

## 8. Appendix

### 8.1 References

1. [Anthropic Messages API - Count Tokens](https://docs.anthropic.com/en/api/counting-tokens) - Official Anthropic API documentation for the count tokens endpoint
2. `odin/src/server.js` - Odin HTTP server with existing route handlers

### 8.2 Related Documents

| Document | Description |
|----------|-------------|
| `odin_basic_design.md` | Odin proxy architecture and core design |
| `odin_request_log_file.md` | Request logging format specification |

---

## Changelog

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.1 | 2026-02-09 | Chason Tang | Phase 1 implemented: route handler added in `src/server.js` before `/v1/messages`; returns 501 with `not_implemented_error`; includes `logResponse` and `logger?.log` calls; mark all tasks complete |
| 1.0 | 2026-02-09 | Chason Tang | Initial version |

---

*End of Technical Design Document*
