# RFC-013: Usage Token Mapping Correction

**Version:** 1.2  
**Author:** Chason Tang  
**Date:** 2026-03-05  
**Status:** Implemented

---

## 1. Summary

The Google → Anthropic usage token mapping in `streamSSEResponse` references a phantom field (`cachedContentTokenCount`) that does not exist in Antigravity's `usageMetadata`. Antigravity only provides three fields: `promptTokenCount`, `candidatesTokenCount`, and `totalTokenCount` (the arithmetic sum of the first two). This RFC corrects the mapping to eliminate dead code paths, directly map `promptTokenCount` → `input_tokens` and `candidatesTokenCount` → `output_tokens`, and hardcode both cache-related Anthropic fields to zero.

## 2. Motivation

The current `streamSSEResponse` in `converter.js` (lines 782–868) tracks a `cacheReadTokens` accumulator sourced from `usage.cachedContentTokenCount`:

```javascript
// converter.js L784–L802 (current)
let cacheReadTokens = 0;
// ...
cacheReadTokens = usage.cachedContentTokenCount ?? cacheReadTokens;
```

This field **does not exist** in Antigravity's SSE responses. The SSE response validation schema in `response-validator.js` already confirms the ground truth — `usageMetadata` requires exactly three properties:

```javascript
usageMetadata: {
    type: 'object',
    required: ['promptTokenCount', 'candidatesTokenCount', 'totalTokenCount'],
    properties: {
        promptTokenCount:    { type: 'integer', minimum: 0 },
        candidatesTokenCount: { type: 'integer', minimum: 0 },
        totalTokenCount:     { type: 'integer', minimum: 0 },
    },
},
```

Because `cachedContentTokenCount` is always `undefined`, the nullish coalescing (`??`) keeps `cacheReadTokens` at its initial value of `0`. The downstream subtraction `inputTokens - cacheReadTokens` therefore happens to produce the correct result by accident. However, the code is misleading in three ways:

1. **Phantom dependency** — it references a field that the upstream never provides, creating the false impression that cache-granular token tracking is functional.
2. **Fragile invariant** — if Antigravity ever adds a field with a similar name but different semantics, the code would silently produce wrong token counts.
3. **Documentation drift** — the legacy design document (`odin_basic_design.md`, §2.4.3) documents a mapping for `cachedContentTokenCount → cache_read_input_tokens` that has never worked in practice.

Meanwhile, Anthropic's Messages API specifies four usage fields:

| Anthropic Field | Semantics |
|---|---|
| `input_tokens` | Non-cached input tokens consumed |
| `cache_creation_input_tokens` | Tokens used to create a cache entry |
| `cache_read_input_tokens` | Tokens read from an existing cache entry |
| `output_tokens` | Output tokens generated |

The documentation further clarifies: *"Total input tokens in a request is the summation of `input_tokens`, `cache_creation_input_tokens`, and `cache_read_input_tokens`."* This "total" is a derived concept, **not** a field in the `usage` object.

Since Antigravity provides only the aggregate prompt token count with no cache breakdown, the only correct mapping is to place the entire `promptTokenCount` into `input_tokens` and zero out both cache fields.

## 3. Goals and Non-Goals

**Goals:**
- Remove the phantom `cacheReadTokens` accumulator and all references to `cachedContentTokenCount` from `streamSSEResponse`.
- Produce a clean, direct mapping: `promptTokenCount` → `input_tokens`, `candidatesTokenCount` → `output_tokens`, with `cache_creation_input_tokens` and `cache_read_input_tokens` hardcoded to `0`.
- Unify the usage object shape across `message_start` and `message_delta` events — both emit the same four Anthropic fields (`input_tokens`, `output_tokens`, `cache_creation_input_tokens`, `cache_read_input_tokens`), consistent with the Anthropic `MessageDeltaUsage` specification.

**Non-Goals:**
- Implementing real cache token tracking — this requires Antigravity to expose cache-granular token counts, which is outside our control.

## 4. Design

### 4.1 Overview

The change refactors `streamSSEResponse` in `converter.js`. It removes the phantom `cachedContentTokenCount` dependency and unifies both SSE usage objects (`message_start` and `message_delta`) to emit all four Anthropic token fields.

```
┌────────────────────────────────────────────────────────────┐
│  Antigravity SSE usageMetadata                             │
│  ┌──────────────────┐  ┌──────────────────────┐            │
│  │ promptTokenCount │  │ candidatesTokenCount │            │
│  └────────┬─────────┘  └─────────┬────────────┘            │
│           │                      │         totalTokenCount │
│           │                      │         (= sum, unused) │
└───────────┼──────────────────────┼─────────────────────────┘
            │                      │
            ▼                      ▼
┌───────────────────────────────────────────────────────────┐
│  Anthropic usage (emitted in SSE)                         │
│                                                           │
│  input_tokens ◄──── promptTokenCount                      │
│  output_tokens ◄─── candidatesTokenCount                  │
│  cache_creation_input_tokens ◄─── 0 (no upstream data)    │
│  cache_read_input_tokens ◄─────── 0 (no upstream data)    │
└───────────────────────────────────────────────────────────┘
```

### 4.2 Detailed Design

#### 4.2.1 Variable Declarations

**Before (L783–L786):**
```javascript
let messageId = null;
let inputTokens = 0;
let outputTokens = 0;
let cacheReadTokens = 0;
let stopReason = null;
```

**After:**
```javascript
let messageId = null;
let inputTokens = 0;
let outputTokens = 0;
let stopReason = null;
```

Remove `cacheReadTokens` — no upstream source provides cache-granular data.

#### 4.2.2 Token Accumulation

**Before (L799–L802):**
```javascript
if (usage) {
    inputTokens = usage.promptTokenCount || inputTokens;
    outputTokens = usage.candidatesTokenCount || outputTokens;
    cacheReadTokens = usage.cachedContentTokenCount ?? cacheReadTokens;
}
```

**After:**
```javascript
if (usage) {
    inputTokens = usage.promptTokenCount || inputTokens;
    outputTokens = usage.candidatesTokenCount || outputTokens;
}
```

Remove the `cachedContentTokenCount` line entirely.

#### 4.2.3 `message_start` Usage Object

**Before (L822–L827):**
```javascript
usage: {
    input_tokens: inputTokens - cacheReadTokens,
    output_tokens: 0,
    cache_read_input_tokens: cacheReadTokens,
    cache_creation_input_tokens: 0,
},
```

**After:**
```javascript
usage: {
    input_tokens: inputTokens,
    output_tokens: outputTokens,
    cache_read_input_tokens: 0,
    cache_creation_input_tokens: 0,
},
```

Two changes: (1) `input_tokens` receives `promptTokenCount` directly — no subtraction needed since there is no cache breakdown; (2) `output_tokens` uses the live accumulator instead of hardcoded `0`. The usage accumulation block (L799–L802) executes before the `message_start` guard (L805), so `outputTokens` already reflects the current SSE event's `candidatesTokenCount` by the time `message_start` is emitted.

#### 4.2.4 `message_delta` Usage Object

**Before (L858–L861):**
```javascript
usage: {
    output_tokens: outputTokens,
    cache_read_input_tokens: cacheReadTokens,
    cache_creation_input_tokens: 0,
},
```

**After:**
```javascript
usage: {
    input_tokens: inputTokens,
    output_tokens: outputTokens,
    cache_read_input_tokens: 0,
    cache_creation_input_tokens: 0,
},
```

Both events now emit an identical four-field usage object. Anthropic's `MessageDeltaUsage` defines `output_tokens` as required and `input_tokens`, `cache_creation_input_tokens`, `cache_read_input_tokens` as optional. Emitting the full set in both events gives downstream consumers a single, consistent schema to parse — they can read any usage field from either event without knowing which one they're looking at.

### 4.3 Design Rationale

**Why map `promptTokenCount` directly to `input_tokens` instead of inventing a split?**

The Anthropic token model decomposes total input into three buckets: `input_tokens`, `cache_creation_input_tokens`, and `cache_read_input_tokens`. Since Antigravity provides only the aggregate total (`promptTokenCount`) with no cache breakdown, fabricating non-zero values for the cache fields would be dishonest. Placing the entire count into `input_tokens` and zeroing the cache fields is the only semantically correct mapping — it truthfully reports "all input tokens were consumed as non-cached input."

**Why use the live `outputTokens` accumulator in `message_start` instead of hardcoding `0`?**

Every Antigravity SSE event carries all three token fields (`promptTokenCount`, `candidatesTokenCount`, `totalTokenCount`). The usage accumulation block runs before the `message_start` guard in the event loop, so by the time `message_start` is emitted, `outputTokens` already holds the value from the first SSE event. Hardcoding `0` discards available data for no reason. Using the accumulator is both more accurate and more consistent — both events now source all four fields from the same accumulators.

**Why unify the usage shape across `message_start` and `message_delta`?**

Anthropic's `MessageDeltaUsage` specification defines four fields: `output_tokens` (required), `input_tokens`, `cache_creation_input_tokens`, and `cache_read_input_tokens` (all optional). Emitting the same four-field object from both events provides two benefits: (1) downstream consumers can use a single type/parser for all usage objects — no conditional logic based on event type; (2) each event is self-contained — a consumer that only reads one of the two events still gets complete token accounting.

## 5. Implementation Plan

### Phase 1: Converter Fix — 0.5 hours

- [x] Remove `cacheReadTokens` variable declaration (L786)
- [x] Remove `cachedContentTokenCount` accumulation line (L802)
- [x] Simplify `message_start` usage: `input_tokens: inputTokens`, `output_tokens: outputTokens`, `cache_read_input_tokens: 0` (L822–L827)
- [x] Unify `message_delta` usage: add `input_tokens: inputTokens`, replace `cacheReadTokens` with literal `0` for `cache_read_input_tokens` (L858–L861)
- [x] Run existing test suite to verify no regressions

**Done when:** All existing tests pass; `streamSSEResponse` no longer references `cachedContentTokenCount` or `cacheReadTokens`.

> **Note on legacy documentation:** `odin_basic_design.md` (§2.4.3) documents the original `cachedContentTokenCount` mapping. That document is in Implemented status and is intentionally left unchanged — it serves as a historical record of the design at the time it was written. This RFC supersedes its token mapping description.

## 6. Testing Strategy

Existing tests should pass without modification. Additional focused tests should verify the token mapping and the unified usage shape across both events.

**Key Scenarios:**

| # | Scenario | Input (`usageMetadata`) | Expected `message_start` `usage` | Expected `message_delta` `usage` |
|---|----------|------------------------|----------------------------------|----------------------------------|
| 1 | Standard response (single event) | `{ promptTokenCount: 100, candidatesTokenCount: 50, totalTokenCount: 150 }` | `{ input_tokens: 100, output_tokens: 50, cache_read_input_tokens: 0, cache_creation_input_tokens: 0 }` | `{ input_tokens: 100, output_tokens: 50, cache_read_input_tokens: 0, cache_creation_input_tokens: 0 }` |
| 2 | Multi-event stream | First: `{ promptTokenCount: 100, candidatesTokenCount: 5, totalTokenCount: 105 }`; Last: `{ ..., candidatesTokenCount: 80, ... }` | `{ input_tokens: 100, output_tokens: 5, ... }` (from first event) | `{ input_tokens: 100, output_tokens: 80, ... }` (from last event) |
| 3 | Zero output tokens | `{ promptTokenCount: 200, candidatesTokenCount: 0, totalTokenCount: 200 }` | `{ input_tokens: 200, output_tokens: 0, cache_read_input_tokens: 0, cache_creation_input_tokens: 0 }` | `{ input_tokens: 200, output_tokens: 0, cache_read_input_tokens: 0, cache_creation_input_tokens: 0 }` |
| 4 | Unknown extra field | `{ promptTokenCount: 100, candidatesTokenCount: 50, totalTokenCount: 150, cachedContentTokenCount: 30 }` | `{ input_tokens: 100, output_tokens: 50, ... }` — extra field ignored | `{ input_tokens: 100, output_tokens: 50, ... }` — same; extra field ignored |
| 5 | Usage missing from event | `usage: null` | Token accumulators retain previous values (fallback via `\|\|` operator) | Same — accumulators unchanged |

## 7. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Downstream client does not expect `input_tokens` in `message_delta` | Low | Low | The field is optional per Anthropic spec; well-behaved clients ignore unknown fields. Adding a previously-absent optional field is a backward-compatible change. |
| Antigravity adds `cachedContentTokenCount` in the future | Low | Med | The validation schema (`response-validator.js`) uses `additionalProperties: true` so it won't reject the field. A follow-up RFC would add proper cache token mapping when the upstream data becomes available. |

## 8. Future Work

- **Cache-granular token tracking** — if Antigravity adds `cachedContentTokenCount` or equivalent fields, implement a proper decomposition: `input_tokens = promptTokenCount - cachedContentTokenCount`, `cache_read_input_tokens = cachedContentTokenCount`. This would require a new RFC with updated validation schema and test coverage.
- **`totalTokenCount` validation** — consider adding a debug-mode assertion that `totalTokenCount == promptTokenCount + candidatesTokenCount` to detect upstream accounting anomalies.

## 9. References

- [Anthropic Messages API — `usage` field specification](https://platform.claude.com/docs/en/api/messages/create)
- `odin/src/converter.js` — `streamSSEResponse` function (L782–L868)
- `odin/src/response-validator.js` — `usageMetadata` schema (L84–L92)
- `odin/docs/legacy_format_documentation/odin_basic_design.md` — §2.4.3 Response Format Mapping
- RFC-007: SSE Stream Finite State Machine
- RFC-008: Antigravity SSE Response Validation

---

## Changelog

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.2 | 2026-03-05 | Chason Tang | Use live `outputTokens` accumulator in `message_start` instead of hardcoded `0` — both events now source all four fields identically from accumulators |
| 1.1 | 2026-03-05 | Chason Tang | Unify `message_delta` usage shape with `message_start` — emit all four Anthropic token fields in both events per `MessageDeltaUsage` spec |
| 1.0 | 2026-03-05 | Chason Tang | Initial version |
