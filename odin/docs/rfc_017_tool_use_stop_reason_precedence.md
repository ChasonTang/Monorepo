# RFC-017: Fix stop_reason Precedence for Tool Use SSE Responses

**Version:** 1.0  
**Author:** Chason Tang  
**Date:** 2026-03-10  
**Status:** Implemented

---

## 1. Summary

When Antigravity returns a tool-calling response, it sends two SSE events: the first carries `functionCall` parts (no `finishReason`), and the second carries an empty text part with `finishReason: "STOP"`. The current `streamSSEResponse` logic sets `stopReason = 'end_turn'` upon seeing `finishReason: "STOP"` inside the event loop, which prevents the post-loop `hasToolUse` guard from overriding it to `'tool_use'`. This RFC proposes a one-line fix that gives `tool_use` precedence over `end_turn` in the post-loop resolution.

## 2. Motivation

Anthropic-compatible clients rely on `stop_reason` to decide whether to execute tool calls and continue the agentic loop. When `stop_reason` is `end_turn`, clients treat the turn as terminal and discard any `tool_use` content blocks — the agent stalls and tools are never invoked.

The bug is reproducible with every tool-calling response from Antigravity. An observed SSE payload:

```
data: {"response":{"candidates":[{"content":{"role":"model","parts":[{"functionCall":{"name":"Read","args":{"file_path":"src/index.ts"},"id":"toolu_vrtx_01AkLz..."}}]}}],"usageMetadata":{...},"modelVersion":"claude-opus-4-6-thinking","responseId":"req_vrtx_011C..."}}
data: {"response":{"candidates":[{"content":{"role":"model","parts":[{"text":""}]},"finishReason":"STOP"}],"usageMetadata":{...},"modelVersion":"claude-opus-4-6-thinking","responseId":"req_vrtx_011C..."}}
```

**Current (incorrect) output:**

```json
{"type":"message_delta","delta":{"stop_reason":"end_turn","stop_sequence":null},...}
```

**Expected output:**

```json
{"type":"message_delta","delta":{"stop_reason":"tool_use","stop_sequence":null},...}
```

## 3. Goals and Non-Goals

**Goals:**
- Emit `stop_reason: "tool_use"` whenever the FSM has processed at least one `functionCall` part, unless the response was truncated by token limits.
- Preserve the existing `max_tokens` precedence so that truncated responses are never misreported as `tool_use`.

**Non-Goals:**
- Refactoring the three-layer SSE pipeline or the ContentBlockFSM — the existing architecture is sound and the bug is isolated to one conditional.
- Handling hypothetical `finishReason` values beyond the validated set (`STOP`, `MAX_TOKENS`, `OTHER`) — that concern belongs to the response validator (RFC-008).

## 4. Design

### 4.1 Overview

The fix is a single-line change to the post-loop `stopReason` resolution in `streamSSEResponse`. No new components, no interface changes, no new dependencies.

```
┌─────────────────────────────────────────────────┐
│            streamSSEResponse loop               │
│                                                 │
│  for each SSE event:                            │
│    ┌─────────────┐                              │
│    │ FSM.process │ ← sets hasToolUse=true       │
│    │  (parts)    │   when functionCall seen     │
│    └─────────────┘                              │
│    if finishReason && !stopReason:              │
│      stopReason = MAX_TOKENS→max_tokens         │
│                    else→end_turn                │
│                                                 │
├─────────────────────────────────────────────────┤
│            Post-loop resolution                 │
│                                                 │
│  FSM.flush()                                    │
│                                                 │
│  BEFORE (buggy):                                │
│  if hasToolUse && !stopReason → tool_use        │
│  ──── ↑ never fires: stopReason is 'end_turn' ──│
│                                                 │
│  AFTER (fixed):                                 │
│  if hasToolUse &&                               │
│    (!stopReason || stopReason === 'end_turn')   │
│    → tool_use                                   │
│  ──── ↑ allowlist: only overrides null/end_turn │
│                                                 │
│  Output: stopReason || 'end_turn'               │
│  ──── ↑ fallback when stopReason is still null ─│
└─────────────────────────────────────────────────┘
```

### 4.2 Detailed Design

#### 4.2.1 Root Cause Analysis

The `streamSSEResponse` generator contains two `stopReason` decision points and one output default:

**Decision Point 1 — inside the event loop (line 853–855):**

```javascript
if (finishReason && !stopReason) {
    stopReason = finishReason === 'MAX_TOKENS' ? 'max_tokens' : 'end_turn';
}
```

**Decision Point 2 — after the loop, post `fsm.flush()` (line 862–864):**

```javascript
if (fsm.hasToolUse && !stopReason) {
    stopReason = 'tool_use';
}
```

**Output Default — final emission (line 870):**

```javascript
delta: { stop_reason: stopReason || 'end_turn', stop_sequence: null }
```

If `stopReason` remains `null` after Decision Points 1 and 2 (e.g., no `finishReason` received and no tool calls), the output falls back to `'end_turn'`. This fallback is the mechanism behind Scenario 6 in the precedence table (§4.2.2).

For a tool-calling response, Antigravity emits two events:

| SSE event | `parts`                  | `finishReason` |
|-----------|--------------------------|----------------|
| 1         | `[{functionCall: {...}}]`| `null`         |
| 2         | `[{text: ""}]`           | `"STOP"`       |

Execution trace:

1. **Event 1:** FSM processes `functionCall` → `hasToolUse = true`. `finishReason` is `null` → Decision Point 1 is skipped. `stopReason` remains `null`.
2. **Event 2:** FSM processes empty text (no-op semantically but state transitions). `finishReason` is `"STOP"` → Decision Point 1 fires: `stopReason = 'end_turn'`.
3. **Post-loop:** Decision Point 2 checks `fsm.hasToolUse && !stopReason`. `!stopReason` evaluates to `false` (it is `'end_turn'`). The override to `'tool_use'` **never fires**.

#### 4.2.2 The Fix

Change Decision Point 2 from:

```javascript
if (fsm.hasToolUse && !stopReason) {
    stopReason = 'tool_use';
}
```

to:

```javascript
if (fsm.hasToolUse && (!stopReason || stopReason === 'end_turn')) {
    stopReason = 'tool_use';
}
```

This produces the correct precedence:

| Scenario                           | `finishReason` | `hasToolUse`| Result        |
|------------------------------------|----------------|-------------|---------------|
| Tool call, normal finish           | `STOP`         | `true`      | `tool_use`    |
| Tool call, finish reason OTHER     | `OTHER`        | `true`      | `tool_use`    |
| Tool call, truncated               | `MAX_TOKENS`   | `true`      | `max_tokens`  |
| No tool call, normal finish        | `STOP`         | `false`     | `end_turn`    |
| No tool call, truncated            | `MAX_TOKENS`   | `false`     | `max_tokens`  |
| No tool call, no finishReason      | `null`         | `false`     | `end_turn` \* |
| Tool call, no finishReason (guard) | `null`         | `true`      | `tool_use`    |

\* `stopReason` remains `null` through Decision Points 1 and 2; `end_turn` comes from the output default (`stopReason \|\| 'end_turn'` on line 870).

### 4.3 Design Rationale

**Why override `end_turn` with `tool_use`?**

In the Anthropic Messages API, `stop_reason: "tool_use"` is the **only** signal for clients to execute tools and continue the conversation. If a response contains `tool_use` content blocks but the `stop_reason` is `end_turn`, compliant clients will discard the tool calls. The presence of `functionCall` parts is the authoritative signal — it must take precedence over the upstream `finishReason` field, which uses a different semantic vocabulary.

**Why preserve `max_tokens`?**

`max_tokens` indicates the response was truncated. Even if a `functionCall` part was emitted before truncation, the overall response may be incomplete (e.g., a text block following the tool call was cut off). Reporting `max_tokens` is the conservative-correct choice — it lets the client decide whether to retry or proceed.

**Why not move Decision Point 1 out of the loop?**

An alternative design would accumulate `finishReason` across events and resolve `stopReason` entirely after the loop. While cleaner in theory, it would be a larger diff that touches the loop body, increasing review and regression risk for a straightforward bug. The current in-loop logic is correct for its purpose (capturing `finishReason`); only the post-loop precedence is wrong.

**Why not check the `finishReason` value inside the loop against `hasToolUse`?**

At the time Decision Point 1 executes, `hasToolUse` may not yet be true — Antigravity can emit the `finishReason` in a separate event **after** the tool call event. The only safe place to consult `hasToolUse` is after all events have been processed and `fsm.flush()` has been called.

## 5. Implementation Plan

### Phase 1: Fix and Test — 0.5 day

- [x] Change the post-loop condition in `streamSSEResponse` (line 862 of `converter.js`) from `!stopReason` to `(!stopReason || stopReason === 'end_turn')`
- [x] Add unit tests covering the seven scenarios from the precedence table in §4.2.2
- [x] Run the full existing test suite to verify no regressions

**Done when:** All seven stop_reason scenarios produce the correct output and existing tests pass.

## 6. Testing Strategy

Unit tests will be added to `tests/content-block-fsm.test.js` (or a new `tests/stop-reason-precedence.test.js` if scope warrants separation). Since `streamSSEResponse` is an async generator that depends on a `ReadableStream`, the most practical approach is to test the stop_reason resolution logic by simulating the same conditions the generator encounters.

**Key Scenarios:**

| # | Scenario | finishReason | hasToolUse | Expected stop_reason |
|---|----------|-------------|------------|---------------------|
| 1 | Tool call with STOP finish | `STOP` | `true` | `tool_use` |
| 2 | Tool call with OTHER finish | `OTHER` | `true` | `tool_use` |
| 3 | Tool call truncated by max tokens | `MAX_TOKENS` | `true` | `max_tokens` |
| 4 | Text-only with STOP finish | `STOP` | `false` | `end_turn` |
| 5 | Text-only truncated | `MAX_TOKENS` | `false` | `max_tokens` |
| 6 | Text-only with no finishReason | `null` | `false` | `end_turn` |
| 7 | Tool call with no finishReason | `null` | `true` | `tool_use` |

## 7. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Antigravity changes the SSE event structure for tool calls (e.g., merging into a single event) | Low | Low | The fix is based on `hasToolUse` from FSM state, not on event count or ordering; it remains correct regardless of how events are batched. |
| A future `finishReason` value should take precedence over `tool_use` (similar to `MAX_TOKENS`) | Low | Med | The condition uses a positive allowlist (`!stopReason \|\| stopReason === 'end_turn'`): `tool_use` only overrides `null` or `end_turn`. Any new `stopReason` value mapped from a future `finishReason` is preserved by default — no code change needed unless the new value should also be overridable. |

## 8. Future Work

- Centralize `stopReason` resolution into a dedicated function with explicit priority tiers, making the precedence rules self-documenting and testable in isolation.
- Consider emitting a warning log when `finishReason` and `hasToolUse` disagree, to aid debugging of upstream behavior changes.

## 9. References

- `odin/src/converter.js` — `streamSSEResponse`, `ContentBlockFSM`, `parseGoogleSSEEvents`
- `odin/src/response-validator.js` — SSE event validation schema (`finishReason` enum)
- `odin/tests/content-block-fsm.test.js` — existing FSM test suite
- RFC-007: SSE Stream Converter — Finite State Machine Refactor
- RFC-008: Antigravity SSE Response Validation

---

## Changelog

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2026-03-10 | Chason Tang | Initial version |
