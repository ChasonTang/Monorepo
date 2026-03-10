# RFC-019: Function Response Struct Compliance

**Version:** 1.0  
**Author:** Chason Tang  
**Date:** 2026-03-11  
**Status:** Implemented

---

## 1. Summary

The `tool_result` → `functionResponse` conversion in `converter.js` passes `block.content` (a raw string) as `functionResponse.response`, but Antigravity expects a `google.protobuf.Struct` (a JSON object). This causes `INVALID_ARGUMENT` validation failures in production. The fix wraps the content string in a keyed object — `{ error: content }` or `{ output: content }` — based on the previously-ignored `block.is_error` field from the Anthropic API.

## 2. Motivation

In production, every multi-turn conversation that includes a tool result triggers the following Antigravity error:

```json
{
  "status": "INVALID_ARGUMENT",
  "details": [{
    "@type": "type.googleapis.com/google.rpc.BadRequest",
    "fieldViolations": [{
      "field": "request.contents[2].parts[0].function_response.response",
      "description": "Invalid value at 'request.contents[2].parts[0].function_response.response' (type.googleapis.com/google.protobuf.Struct)"
    }]
  }]
}
```

The offending payload contains a raw string where a Struct is required:

```json
{
  "functionResponse": {
    "id": "toolu_vrtx_01AodpazcJE5XhgGKv7ScdY9",
    "name": "toolu_vrtx_01AodpazcJE5XhgGKv7ScdY9",
    "response": "     1→/**\n     2→ * Shared utilities...\n"
  }
}
```

`google.protobuf.Struct` is a well-known protobuf message type that maps to a JSON object. A JSON string is not a valid Struct, hence the validation failure.

Additionally, Anthropic's `tool_result` block includes an optional `is_error` boolean field that indicates whether the tool execution failed. Odin currently ignores this field, losing error semantics during conversion.

Fixing this resolves the production breakage and aligns Odin's converter with the existing reference implementation in `pi-mono/packages/ai/src/providers/google-shared.ts`.

## 3. Goals and Non-Goals

**Goals:**
- Wrap `functionResponse.response` in a Struct-compatible JSON object to eliminate the `INVALID_ARGUMENT` error.
- Respect `block.is_error` by using `{ error: content }` for error results and `{ output: content }` for success results.
- Pass `block.content` through as-is, deferring content normalization until production data justifies it.

**Non-Goals:**
- Multimodal function responses (e.g., images in tool results) — Odin's converter has no image handling infrastructure today; this belongs in a separate RFC.
- Resolving the `name: block.tool_use_id` simplification (using `tool_use_id` as the function name) — this is a pre-existing concern already documented in the codebase.

## 4. Design

### 4.1 Overview

The change is localized to the `tool_result` branch of `convertContentToParts()` in `converter.js`. `block.content` is passed through as-is and wrapped in `{ error: content }` or `{ output: content }` based on `block.is_error`.

```
┌──────────────────────────────────────────────────────────┐
│               Anthropic tool_result block                │
│  { type: "tool_result",                                  │
│    tool_use_id: "toolu_...",                             │
│    content: <string | ContentBlock[] | null>,            │
│    is_error: <boolean | undefined> }                     │
└─────────────────┬────────────────────────────────────────┘
                  │
                  ▼
┌──────────────────────────────────────────────────────────┐
│           Wrap into Struct-compatible object             │
│  is_error === true  → { error: block.content }           │
│  otherwise          → { output: block.content }          │
└─────────────────┬────────────────────────────────────────┘
                  │
                  ▼
┌──────────────────────────────────────────────────────────┐
│             Google functionResponse part                 │
│  { functionResponse: {                                   │
│      id: block.tool_use_id,                              │
│      name: block.tool_use_id,                            │
│      response: { output: "..." }   ← Struct ✓            │
│  }}                                                      │
└──────────────────────────────────────────────────────────┘
```

### 4.2 Detailed Design

#### 4.2.1 Struct Wrapping with Error Semantics

`block.content` is passed through directly and wrapped based on `block.is_error`:

```javascript
case 'tool_result':
    parts.push({
        functionResponse: {
            id: block.tool_use_id,
            name: block.tool_use_id,
            response: block.is_error
                ? { error: block.content }
                : { output: block.content },
        },
    });
    break;
```

The key names `error` and `output` are chosen to match the Google GenAI SDK convention, consistent with the reference implementation in `google-shared.ts:200`.

No content normalization is applied to `block.content`. In current production traffic, the upstream always sends string content for tool results. If non-string shapes (array, null) cause Antigravity errors in the future, normalization will be added at that point.

### 4.3 Design Rationale

**Why `{ output: content }` / `{ error: content }` instead of `{ result: content }`?**

The Google Gemini SDK documentation and the reference implementation in `pi-mono` both use `output` for success and `error` for error results. Aligning with this convention avoids divergence between the two codebases and follows the API provider's established pattern.

**Why not normalize `block.content` upfront?**

In current production traffic, the upstream (Claude Code) always sends string content for tool results. Adding normalization logic for array/null shapes would be speculative code with no production validation. Following YAGNI, we defer this until actual errors surface.

## 5. Implementation Plan

### Phase 1: Bug Fix — 0.5h

- [x] Update the `tool_result` case in `convertContentToParts()` to wrap `block.content` in `{ error: ... }` or `{ output: ... }` based on `block.is_error`

**Done when:** Antigravity no longer returns `INVALID_ARGUMENT` for `functionResponse.response`, and tool error results are distinguished from success results.

## 6. Testing Strategy

Unit tests should cover the `convertContentToParts` function with `tool_result` blocks across the full input matrix.

**Key Scenarios:**

| # | Scenario | Input | Expected Behavior |
|---|----------|-------|-------------------|
| 1 | String content, success | `{ type: "tool_result", tool_use_id: "t1", content: "65 degrees" }` | `response: { output: "65 degrees" }` |
| 2 | String content, error | `{ type: "tool_result", tool_use_id: "t1", content: "ENOENT: not found", is_error: true }` | `response: { error: "ENOENT: not found" }` |
| 3 | `is_error` explicitly false | `{ type: "tool_result", tool_use_id: "t1", content: "ok", is_error: false }` | `response: { output: "ok" }` |
| 4 | `is_error` absent | `{ type: "tool_result", tool_use_id: "t1", content: "ok" }` | `response: { output: "ok" }` |

## 7. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Antigravity silently ignores `error` key and treats errored tool results as successful | Low | Low | The `error` / `output` key convention is documented in the Google GenAI SDK and validated by the `google-shared.ts` reference implementation |
| `block.content` is non-string (array or null) causing Struct field type error | Low | Med | Current upstream always sends string content; if this changes, the error will surface immediately in production logs and can be addressed reactively |

## 8. Future Work

- **Content normalization** — handle `block.content` shapes beyond plain strings (e.g., `ContentBlock[]`, `null`) if production traffic evolves to include them.
- **Multimodal tool results** — support image content blocks in `tool_result` → `functionResponse` conversion, potentially using `functionResponse.parts` for Gemini 3+ models (as implemented in `google-shared.ts:203`).
- **Function name resolution** — resolve the `name: block.tool_use_id` simplification by maintaining a `tool_use_id → function_name` mapping, as noted in the existing code comment.

## 9. References

- `odin/src/converter.js:409-419` — current `tool_result` conversion code
- `pi-mono/packages/ai/src/providers/google-shared.ts:198-206` — reference implementation with correct Struct wrapping
- [Google Protocol Buffers — Struct](https://protobuf.dev/reference/protobuf/google.protobuf/#struct) — `google.protobuf.Struct` type definition
- [Anthropic Messages API — Tool Use](https://docs.anthropic.com/en/docs/build-with-claude/tool-use#tool-result) — `tool_result` content block specification

---

## Changelog

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2026-03-11 | Chason Tang | Initial version |
