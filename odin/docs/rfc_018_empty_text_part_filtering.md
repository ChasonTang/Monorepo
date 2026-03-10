# RFC-018: Filter Empty Text Parts from Antigravity SSE Responses

**Version:** 1.0  
**Author:** Chason Tang  
**Date:** 2026-03-10  
**Status:** Implemented

---

## 1. Summary

When Antigravity streams responses for thinking models, it emits SSE events containing empty text parts (`{ text: "" }`) as structural preamble and postamble markers. The current `classifyPart` function classifies these as `'text'` because `part.text !== undefined` is true for empty strings. This causes the `ContentBlockFSM` to create spurious empty text content blocks — a `content_block_start` + `content_block_delta` with `text: ""` + `content_block_stop` triplet — in the Anthropic SSE output. This RFC proposes a one-condition fix in `classifyPart` to skip non-thought parts with empty text, eliminating the empty text blocks.

## 2. Motivation

An observed Antigravity SSE stream for a thinking model with tool use contains the following event sequence:

```
← data: { parts: [{ text: "" }] }                                        ← preamble
← data: { parts: [{ thought: true, text: "The user is asking..." }] }    ← thinking
← data: { parts: [{ thought: true, text: "" }] }                         ← thinking end
← data: { parts: [{ thought: true, thoughtSignature: "RXFVQ0...", text: "" }] }
← data: { parts: [{ functionCall: { name: "Read", args: {...}, id: "toolu_..." } }] }
← data: { parts: [{ text: "" }], finishReason: "STOP" }                  ← postamble
```

Events 1 and 6 carry `{ text: "" }` — empty text parts that serve as Antigravity-internal structural markers but carry zero semantic content. The current pipeline converts these into Anthropic content blocks:

**Current (incorrect) output:**

```
→ content_block_start   { index: 0, type: 'text' }          ← SPURIOUS (from preamble)
→ content_block_delta   { index: 0, text: '' }               ← SPURIOUS
→ content_block_stop    { index: 0 }                          ← SPURIOUS (thinking transition)
→ content_block_start   { index: 1, type: 'thinking' }
→ content_block_delta   { index: 1, thinking: '...' }
→ content_block_delta   { index: 1, signature: '...' }
→ content_block_stop    { index: 1 }
→ content_block_start   { index: 2, type: 'tool_use' }
→ content_block_delta   { index: 2, partial_json: '...' }
→ content_block_stop    { index: 2 }                          ← SPURIOUS (empty text transition)
→ content_block_start   { index: 3, type: 'text' }           ← SPURIOUS (from postamble)
→ content_block_delta   { index: 3, text: '' }                ← SPURIOUS
→ content_block_stop    { index: 3 }                          ← SPURIOUS (flush)
→ message_delta         { stop_reason: 'tool_use' }
→ message_stop
```

This produces **four** content blocks (indices 0–3) when only **two** are semantically meaningful (thinking at index 0, tool_use at index 1). The spurious empty text blocks cause three issues:

1. **Semantic pollution** — An Anthropic text content block with only `""` content never appears in real Anthropic API responses. Clients that inspect content blocks may encounter unexpected empty text entries.
2. **Index inflation** — Content block indices are inflated by 2, which affects any client logic that relies on block indices for tracking or correlation.
3. **Bandwidth waste** — Each spurious block generates three SSE events (start + delta + stop), adding ~450 bytes of useless payload per empty text part to every streamed response.

**Expected output:**

```
→ content_block_start   { index: 0, type: 'thinking' }
→ content_block_delta   { index: 0, thinking: '...' }
→ content_block_delta   { index: 0, signature: '...' }
→ content_block_stop    { index: 0 }
→ content_block_start   { index: 1, type: 'tool_use' }
→ content_block_delta   { index: 1, partial_json: '...' }
→ content_block_stop    { index: 1 }
→ message_delta         { stop_reason: 'tool_use' }
→ message_stop
```

Two content blocks, correct indices, no empty text artifacts.

## 3. Goals and Non-Goals

**Goals:**
- Eliminate spurious empty text content blocks from Anthropic SSE output by filtering empty text parts (`{ text: "" }`) in `classifyPart()`.
- Preserve correct behavior for thinking parts with empty text (`{ thought: true, text: "" }`), which are already handled by the existing FSM guard.
- Ensure `finishReason` processing and `stopReason` resolution are unaffected — these operate at the event level, independent of part classification.

**Non-Goals:**
- Filtering empty text parts at the `parseGoogleSSEEvents` layer — part classification is `classifyPart`'s responsibility, not the event parser's.
- Adding a general "empty delta suppression" mechanism — the fix is targeted at the classification layer where it naturally belongs. An FSM-level guard would be redundant.
- Modifying the SSE response validation schema (RFC-008) — `{ text: "" }` is a valid part per the Antigravity API contract; the issue is in classification, not validation.

## 4. Design

### 4.1 Overview

The fix is a single-condition change in `classifyPart()` (line 657 of `converter.js`). No new components, no interface changes, no new dependencies.

```
┌─────────────────────────────────────────────────────────┐
│               classifyPart(part)                        │
│                                                         │
│   if part.thought === true    → 'thinking'              │
│                                                         │
│   BEFORE (buggy):                                       │
│   if part.text !== undefined  → 'text'                  │
│   ──── ↑ matches { text: "" } — creates empty block ────│
│                                                         │
│   AFTER (fixed):                                        │
│   if part.text !== undefined                            │
│      && part.text !== ''      → 'text'                  │
│   ──── ↑ skips { text: "" } — returns null below ───────│
│                                                         │
│   if part.functionCall        → 'tool_use'              │
│   return null                                           │
└─────────────────────────────────────────────────────────┘
```

When `classifyPart` returns `null`, the FSM's `process()` method returns `[]` (line 683), producing no SSE events for that part. The `finishReason` and `stopReason` logic is unaffected because it operates at the event level, outside the `for (const part of parts)` loop.

### 4.2 Detailed Design

#### 4.2.1 Root Cause Analysis

The `classifyPart` function uses `part.text !== undefined` to detect text parts:

```javascript
export function classifyPart(part) {
    if (part.thought === true) return 'thinking';
    if (part.text !== undefined) return 'text';       // ← matches { text: "" }
    if (part.functionCall) return 'tool_use';
    return null;
}
```

For the empty string `""`, `part.text !== undefined` evaluates to `true`. The part is classified as `'text'`, and the FSM creates a full content block lifecycle for it. Unlike thinking parts, which have a dedicated guard (`if (type !== 'thinking' || part.text)`) that suppresses empty `thinking_delta` emissions, text parts have no such guard — every classified text part produces a `content_block_start` and a `content_block_delta`.

Antigravity emits `{ text: "" }` in two positions for thinking model responses:

| Position | SSE Event | Purpose |
|----------|-----------|---------|
| Preamble (first event) | `parts: [{ text: "" }]` | Structural marker before thinking content |
| Postamble (final event) | `parts: [{ text: "" }]` with `finishReason: "STOP"` | Carrier for `finishReason`, contains no text content |

Both produce spurious text blocks in the Anthropic output.

**Execution trace (preamble `{ text: "" }`):**

1. `classifyPart({ text: "" })` → `'text'` (because `"" !== undefined` is `true`).
2. FSM: `#state` is `null`, `needsNewBlock` is `true`.
3. FSM emits `content_block_start` (index 0, type text) + `content_block_delta` (text: "").
4. Next part is `{ thought: true, text: "..." }` → type changes to `'thinking'`.
5. FSM emits `content_block_stop` (index 0) + `content_block_start` (index 1, type thinking).

Result: a text block at index 0 with only an empty string delta — semantically meaningless.

**Execution trace (postamble `{ text: "" }` after tool_use):**

1. `classifyPart({ text: "" })` → `'text'`.
2. FSM: `#state` is `'tool_use'`, type is `'text'` → `needsNewBlock` is `true`.
3. FSM emits `content_block_stop` (closes tool_use) + `content_block_start` (new text block) + `content_block_delta` (text: "").
4. Post-loop: `fsm.flush()` emits `content_block_stop` for the spurious text block.

Result: another empty text block at the end of the stream.

#### 4.2.2 The Fix

Change line 657 of `converter.js` from:

```javascript
if (part.text !== undefined) return 'text';
```

to:

```javascript
if (part.text !== undefined && part.text !== '') return 'text';
```

This makes `classifyPart` return `null` for `{ text: "" }`, which the FSM treats as an unrecognized part and skips entirely (line 683: `if (!type) return [];`).

The thinking guard is unaffected: `{ thought: true, text: "" }` is still classified as `'thinking'` by the first condition (`part.thought === true`), and its empty delta is suppressed by the existing FSM guard (`if (type !== 'thinking' || part.text)`).

#### 4.2.3 Event Trace: Before vs After

**Scenario: thinking model with tool use (from observed production traffic)**

Input events:

| # | `parts` | `finishReason` |
|---|---------|----------------|
| 1 | `[{ text: "" }]` | — |
| 2 | `[{ thought: true, text: "The user is asking..." }]` | — |
| 3 | `[{ thought: true, text: "" }]` | — |
| 4 | `[{ thought: true, thoughtSignature: "RXFVQ0...", text: "" }]` | — |
| 5 | `[{ functionCall: { name: "Read", args: {...}, id: "toolu_..." } }]` | — |
| 6 | `[{ text: "" }]` | `STOP` |

**Before (4 content blocks, 2 spurious):**

| Event | Type | Index | Content |
|-------|------|-------|---------|
| 1a | `content_block_start` | 0 | `{ type: 'text', text: '' }` |
| 1b | `content_block_delta` | 0 | `{ type: 'text_delta', text: '' }` |
| 2a | `content_block_stop` | 0 | |
| 2b | `content_block_start` | 1 | `{ type: 'thinking', thinking: '' }` |
| 2c | `content_block_delta` | 1 | `{ type: 'thinking_delta', thinking: '...' }` |
| 3 | *(no delta — existing thinking guard)* | | |
| 4 | `content_block_delta` | 1 | `{ type: 'signature_delta', signature: '...' }` |
| 5a | `content_block_stop` | 1 | |
| 5b | `content_block_start` | 2 | `{ type: 'tool_use', ... }` |
| 5c | `content_block_delta` | 2 | `{ type: 'input_json_delta', ... }` |
| 6a | `content_block_stop` | 2 | |
| 6b | `content_block_start` | 3 | `{ type: 'text', text: '' }` |
| 6c | `content_block_delta` | 3 | `{ type: 'text_delta', text: '' }` |
| flush | `content_block_stop` | 3 | |

**After (2 content blocks, 0 spurious):**

| Event | Type | Index | Content |
|-------|------|-------|---------|
| 2a | `content_block_start` | 0 | `{ type: 'thinking', thinking: '' }` |
| 2b | `content_block_delta` | 0 | `{ type: 'thinking_delta', thinking: '...' }` |
| 3 | *(no delta — existing thinking guard)* | | |
| 4 | `content_block_delta` | 0 | `{ type: 'signature_delta', signature: '...' }` |
| 5a | `content_block_stop` | 0 | |
| 5b | `content_block_start` | 1 | `{ type: 'tool_use', ... }` |
| 5c | `content_block_delta` | 1 | `{ type: 'input_json_delta', ... }` |
| flush | `content_block_stop` | 1 | |

Events 1 and 6 produce zero FSM output. `finishReason: "STOP"` from event 6 is still captured at the event level and correctly resolved to `stop_reason: "tool_use"` via the post-loop precedence logic (RFC-017).

#### 4.2.4 Impact on Adjacent Logic

**`hasEmittedStart` check:**

```javascript
if (!hasEmittedStart && parts.length > 0) {
    hasEmittedStart = true;
    yield formatAndLog('message_start', { ... }, debug);
}
```

This check uses the raw `parts` array length, not classified part count. When event 1 carries `parts: [{ text: "" }]`, `parts.length > 0` is true and `message_start` is emitted — even though the empty text part produces no FSM events. This is correct: `message_start` should be emitted as early as possible so the client knows a response has begun. The content blocks arrive with subsequent events.

**`finishReason` / `stopReason` processing:**

```javascript
if (finishReason && !stopReason) {
    stopReason = finishReason === 'MAX_TOKENS' ? 'max_tokens' : 'end_turn';
}
```

This operates at the event level, outside the `for (const part of parts)` loop. Whether or not the parts in an event produce FSM output has no effect on `finishReason` capture.

**`hasToolUse` post-loop resolution (RFC-017):**

```javascript
if (fsm.hasToolUse && (!stopReason || stopReason === 'end_turn')) {
    stopReason = 'tool_use';
}
```

`hasToolUse` is set by the FSM when it processes a `functionCall` part. Empty text parts never affect `hasToolUse`. The fix does not change `functionCall` classification.

### 4.3 Design Rationale

**Why filter in `classifyPart` instead of adding an FSM guard?**

An alternative approach would mirror the existing thinking guard by adding a text-specific guard in the FSM's `process()` method:

```javascript
// Alternative: FSM-level guard
if (type === 'text' && !part.text) return [];
```

This was rejected for two reasons:

1. **Incomplete solution** — The FSM guard would need to fire before `needsNewBlock` evaluation to prevent `content_block_start` emission. Returning `[]` early works, but it means the FSM has already classified the part and then discards it — classification and filtering are conflated across two functions.

2. **Semantic correctness** — An empty text part is not a text part in any meaningful sense. The `classifyPart` function's purpose is to determine "what type of content block does this part represent?" The answer for `{ text: "" }` is "none" — it represents no content. Returning `null` is the semantically correct classification. The existing thinking guard exists for a different reason: a thinking part with empty text IS a thinking part (it belongs to the thinking block), but its delta is suppressed because there's no text to emit.

**Why is this safe for text-only responses?**

In text-only responses, Antigravity may emit the pattern: `{ text: "" }` → `{ text: "Hello" }` → `{ text: " world" }` → `{ text: "" }` with `finishReason: "STOP"`.

- The preamble `{ text: "" }` is skipped — the text block is created on `{ text: "Hello" }` instead.
- The postamble `{ text: "" }` is skipped — the FSM state remains `'text'`, and `flush()` closes the block.
- The `finishReason` is still captured from the postamble event.

The only behavioral difference: instead of a text block with an initial empty delta followed by `"Hello"`, the text block starts directly with `"Hello"`. This matches real Anthropic API behavior, where `text_delta` events always carry non-empty text.

**Why not filter at the `parseGoogleSSEEvents` layer?**

Part classification is the responsibility of `classifyPart` and the FSM, not the event parser. The event parser extracts structured data from SSE lines — it should not make semantic decisions about which parts are meaningful. Filtering at the parser layer would also prevent future use cases where an empty text part might carry metadata (e.g., a hypothetical `status` field) that the FSM should observe.

**Why not use `part.text` (truthy check) instead of `part.text !== ''`?**

The explicit `!== ''` is preferred over a truthy check for precision. While `""` is the only falsy string, using `part.text` (truthy) would also filter `part.text === 0` or `part.text === null` if they ever appear due to an upstream bug. The SSE response validator (RFC-008) already validates `text` as `type: 'string'`, so non-string values should never reach `classifyPart` — but an explicit check is more readable and self-documenting about the intent.

## 5. Implementation Plan

### Phase 1: Fix and Test — 0.5 day

- [x] Change the text classification condition in `classifyPart()` (line 657 of `converter.js`) from `part.text !== undefined` to `part.text !== undefined && part.text !== ''`
- [x] Update the existing test `'classifies empty text as text'` in `content-block-fsm.test.js` to expect `null` instead of `'text'`
- [x] Add new FSM test: preamble empty text before thinking → no content block created
- [x] Add new FSM test: postamble empty text after tool_use → no content block created
- [x] Add new FSM test: mid-stream empty text between non-empty text parts → skipped, no impact on existing block
- [x] Verify all existing tests pass (stop-reason precedence, content-block-fsm, stream-usage-mapping, effort-budget-converter)

**Done when:** Empty text parts produce no FSM events, no spurious content blocks appear in output, and all existing tests pass.

## 6. Testing Strategy

Tests cover three areas: `classifyPart` classification changes (§6.1), FSM-level empty text filtering (§6.2), and end-to-end stream verification (§6.3).

### 6.1 classifyPart Update

Update the existing test in `content-block-fsm.test.js` and add a new test for the non-thought empty text case:

| # | Scenario | Input | Expected |
|---|----------|-------|----------|
| 1 | Empty text (updated) | `{ text: '' }` | `null` (was `'text'`) |
| 2 | Non-empty text (unchanged) | `{ text: 'hello' }` | `'text'` |
| 3 | Thinking with empty text (unchanged) | `{ thought: true, text: '' }` | `'thinking'` |

### 6.2 FSM Empty Text Filtering

New tests to be added in `content-block-fsm.test.js`:

| # | Scenario | Input Sequence | Expected Behavior |
|---|----------|----------------|-------------------|
| 1 | Preamble empty text before thinking | `{ text: "" }` → `{ thought: true, text: "..." }` | Thinking block at index 0 (no preceding text block) |
| 2 | Postamble empty text after tool_use | `{ functionCall: {...} }` → `{ text: "" }` → `flush()` | Tool_use block at index 0, flush closes it (no trailing text block) |
| 3 | Mid-stream empty text between text parts | `{ text: "a" }` → `{ text: "" }` → `{ text: "b" }` | Single text block at index 0, deltas for "a" and "b" only |
| 4 | Empty text as sole part | `{ text: "" }` → `flush()` | No content blocks, flush returns `[]` |
| 5 | Full thinking + tool_use with preamble/postamble | `{ text: "" }` → `{ thought: true, text: "..." }` → `{ thought: true, thoughtSignature: "...", text: "" }` → `{ functionCall: {...} }` → `{ text: "" }` → `flush()` | 2 blocks: thinking (index 0), tool_use (index 1) |

### 6.3 Existing Test Compatibility

All existing tests in the following files must pass without modification (except the `classifyPart` empty text test update):

| File | Scope | Expected Impact |
|------|-------|-----------------|
| `tests/content-block-fsm.test.js` | FSM transitions, classifyPart | 1 test updated (empty text classification) |
| `tests/stop-reason-precedence.test.js` | stop_reason resolution | No impact — finishReason processing is event-level |
| `tests/stream-usage-mapping.test.js` | Token usage mapping | No impact — usage extraction is event-level |
| `tests/effort-budget-converter.test.js` | Thinking budget resolution | No impact — tests converter logic, not SSE streaming |

The stop-reason-precedence tests use `makeSSELine([{ text: '' }], 'STOP')` for final events. After the fix, the empty text part is skipped by the FSM, but `finishReason` is still captured at the event level. All seven precedence scenarios produce identical `stop_reason` values.

## 7. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Antigravity starts sending meaningful content in `{ text: "" }` preamble/postamble events (e.g., adding metadata fields alongside `text`) | Very Low | Low | `classifyPart` only filters when `text` is empty string — any non-empty text is still classified. New fields alongside empty text would require a `classifyPart` update regardless, since the current FSM only processes `text`, `thought`, `thoughtSignature`, and `functionCall`. |
| A future response format uses `{ text: "" }` as a deliberate "empty text block" signal | Very Low | Med | No current Antigravity documentation or observed traffic assigns semantic meaning to empty text parts. If this changes, the fix is a single-line revert with a corresponding FSM-level guard as the replacement. |
| Existing clients depend on the spurious empty text blocks (e.g., counting content blocks) | Very Low | Low | Empty text blocks with only `""` content never appear in real Anthropic API responses. Any client that handles real Anthropic responses correctly will not depend on these artifacts. |

## 8. Future Work

- **Centralize part filtering policy** — As more Antigravity-specific structural markers are discovered, consider introducing a `filterPart(part)` function that runs before `classifyPart`, separating "what to skip" from "what type is it." This would make filtering rules explicit and independently testable.
- **Empty delta suppression in FSM** — The existing thinking guard (`if (type !== 'thinking' || part.text)`) and the new `classifyPart` filter both address empty content. A unified "skip empty deltas" policy in the FSM would be more systematic, though the current approach is sufficient for the observed Antigravity behavior.

## 9. References

- `odin/src/converter.js` — `classifyPart()` (L655–L661), `ContentBlockFSM.process()` (L681–L739), `streamSSEResponse()` (L797–L883)
- `odin/tests/content-block-fsm.test.js` — existing `classifyPart` and FSM tests
- `odin/tests/stop-reason-precedence.test.js` — stop_reason resolution tests (RFC-017)
- RFC-007: SSE Stream Converter — Finite State Machine Refactor (introduced `classifyPart` and `ContentBlockFSM`)
- RFC-008: Antigravity SSE Response Validation (SSE event schema — `text` validated as `type: 'string'`)
- RFC-017: Fix stop_reason Precedence for Tool Use SSE Responses (post-loop `stopReason` resolution)

---

## Changelog

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2026-03-10 | Chason Tang | Initial version |
