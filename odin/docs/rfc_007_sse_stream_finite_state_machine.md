# RFC-007: SSE Stream Converter — Finite State Machine Refactor

**Version:** 1.0  
**Author:** Chason Tang  
**Date:** 2026-02-21  
**Status:** Implemented

---

## 1. Summary

The current `streamSSEResponse` function (240 lines) converts Google Antigravity SSE events into Anthropic-format SSE events inside a single monolithic async generator. The function contains an implicit finite state machine (FSM) — the `currentBlockType` variable tracks transitions between `thinking`, `text`, and `tool_use` content blocks — but the FSM logic is entangled with four other concerns: stream framing, protocol envelope, usage accumulation, and debug logging. This RFC proposes decomposing `streamSSEResponse` into a layered architecture with an **explicit FSM** at its core, eliminating the triplicated block-transition boilerplate and making the state diagram self-documenting.

## 2. Motivation

**Problem:** The current `streamSSEResponse` implementation suffers from three concrete issues:

1. **Triplicated transition logic.** The "close previous block → open new block" pattern is copy-pasted across three branches (`thinking`, `text`, `tool_use`), each with identical structure but different type strings. This violates DRY and makes every block-type addition require edits to three locations.

2. **Entangled concerns.** A single function handles five distinct responsibilities:
   - Byte-stream → line parsing (stream framing, lines 623–633)
   - JSON parsing + `data:` line extraction (event parsing, lines 635–648)
   - `message_start` / `message_delta` / `message_stop` envelope (protocol lifecycle, lines 660–684, 815–840)
   - `content_block_start` / `content_block_delta` / `content_block_stop` state transitions (FSM core, lines 687–798)
   - `if (debug)` logging scattered across every branch (cross-cutting concern, 14 occurrences)

3. **Implicit state machine.** The `currentBlockType` variable is a de facto state, but the state diagram — which transitions are valid, what events each transition emits — is only discoverable by reading 150+ lines of nested conditionals. There is no single place to see the full state transition table.

**Value of solving:** Extracting an explicit FSM will reduce the content-block conversion logic from ~110 lines to ~60 lines, eliminate the triplicated transition pattern, and make adding new block types (e.g., `redacted_thinking`, `server_tool_use`) a one-row table addition instead of a three-location code change.

## 3. Goals and Non-Goals

**Goals:**
- Extract an explicit, table-driven content-block FSM that is self-documenting.
- Eliminate the triplicated "close previous block → open new block" pattern.
- Separate stream framing, protocol envelope, FSM core, usage accumulation, and debug logging into distinct, composable layers.
- Maintain byte-for-byte output compatibility with the current implementation.
- Keep the public API (`streamSSEResponse(stream, model, debug)`) unchanged.

**Non-Goals:**
- Changing the Anthropic SSE event format or adding new event types — this is a pure internal refactor.
- Adding error recovery or retry logic to the stream reader — out of scope for this structural refactor.
- Optimizing stream parsing performance — the current `TextDecoder` + line-split approach is adequate.

## 4. Design

### 4.1 Overview

The refactored architecture decomposes `streamSSEResponse` into three extracted layers plus an orchestrator. Layers 1–3 are standalone async generators / classes; the protocol envelope (message lifecycle) is handled inline within the orchestrator rather than as a separated layer, because it depends on both usage accumulation state and FSM state simultaneously.

```
┌───────────────────────────────────────────────────────────────────────┐
│                      streamSSEResponse()                              │
│         Orchestrator: composes layers 1–3, handles protocol           │
│         envelope (message_start/delta/stop) + usage accumulation      │
│         inline. Yields final Anthropic SSE strings.                   │
│                                                                       │
│  ┌─────────────────────────────────────────────────────────────────┐  │
│  │  Layer 3: Content Block FSM  (ContentBlockFSM class)            │  │
│  │  States: IDLE → THINKING | TEXT | TOOL_USE → (transitions)      │  │
│  │  Emits content_block_start, content_block_delta,                │  │
│  │        content_block_stop                                       │  │
│  │  Table-driven transitions eliminate triplicated boilerplate     │  │
│  ├─────────────────────────────────────────────────────────────────┤  │
│  │  Layer 2: Event Parser  (parseGoogleSSEEvents generator)        │  │
│  │  Parses "data:" SSE lines → structured Google events            │  │
│  │  Extracts parts[], usageMetadata, finishReason                  │  │
│  ├─────────────────────────────────────────────────────────────────┤  │
│  │  Layer 1: Stream Framer  (readSSELines generator)               │  │
│  │  ReadableStream → line-buffered text lines                      │  │
│  └─────────────────────────────────────────────────────────────────┘  │
└───────────────────────────────────────────────────────────────────────┘
```

> **Note on protocol envelope:** `message_start`, `message_delta`, and `message_stop` are emitted directly by the orchestrator (`streamSSEResponse`) rather than via a separate layer. This is intentional — the envelope depends on cross-cutting state (usage tokens, `hasToolUse` flag, `finishReason`) that spans multiple layers, so extracting it would require threading multiple out-of-band values through the interface. Keeping it inline in the orchestrator avoids artificial coupling.

### 4.2 Detailed Design

#### 4.2.1 Layer 1: Stream Framer — `readSSELines(stream)`

A pure async generator that converts a `ReadableStream` into individual text lines. This isolates the `TextDecoder` + line-buffering logic.

```javascript
/**
 * Read a ReadableStream and yield individual text lines.
 *
 * Handles incremental decoding and line buffering. The final
 * partial line (if any) is yielded when the stream closes.
 *
 * @param {ReadableStream} stream
 * @yields {string} Individual text lines (without trailing newline)
 */
async function* readSSELines(stream) {
    const reader = stream.getReader();
    const decoder = new TextDecoder();
    let buffer = '';

    while (true) {
        const { done, value } = await reader.read();
        if (done) break;

        buffer += decoder.decode(value, { stream: true });
        const lines = buffer.split('\n');
        buffer = lines.pop() || '';

        for (const line of lines) {
            yield line;
        }
    }

    if (buffer) {
        yield buffer;
    }
}
```

#### 4.2.2 Layer 2: Event Parser — `parseGoogleSSEEvents(lines, debug)`

Consumes lines from Layer 1, filters for `data:` lines, parses JSON, and yields structured event objects. Each yielded object contains the extracted `parts`, `usage`, and `finishReason` — the three pieces of information the upper layers need.

```javascript
/**
 * @typedef {Object} ParsedSSEEvent
 * @property {Array} parts - Content parts from candidates[0].content.parts
 * @property {Object|null} usage - usageMetadata if present
 * @property {string|null} finishReason - candidates[0].finishReason if present
 */

/**
 * Parse SSE data lines into structured Google events.
 *
 * @param {AsyncIterable<string>} lines - Line stream from readSSELines
 * @param {boolean} debug - Enable debug logging
 * @yields {ParsedSSEEvent}
 */
async function* parseGoogleSSEEvents(lines, debug) {
    for await (const line of lines) {
        if (!line.startsWith('data:')) continue;

        const jsonText = line.slice(5).trim();
        if (!jsonText) continue;

        if (debug) {
            console.error(`[Odin:debug] ← SSE:`, line.trimEnd());
        }

        try {
            const data = JSON.parse(jsonText);
            const inner = data.response || data;

            yield {
                parts: inner.candidates?.[0]?.content?.parts || [],
                usage: inner.usageMetadata || null,
                finishReason: inner.candidates?.[0]?.finishReason || null,
            };
        } catch (e) {
            if (debug) {
                console.error(`[Odin:debug] ← SSE parse error:`, e.message);
            }
        }
    }
}
```

#### 4.2.3 Layer 3: Content Block FSM — `ContentBlockFSM`

This is the core of the refactor. The FSM replaces the triplicated if/else branches with a **transition table** and a single `processInput()` method.

**State diagram:**

```
                  ┌────────────────────────────────────────┐
                  │                                        │
                  ▼                                        │
            ┌──────────┐                                   │
            │   IDLE   │──── thought part ────►┌──────────────────┐
            │(initial) │                       │    THINKING      │◄─── thought part (same state)
            │          │──── text part ───────►┌──────────────────┐
            │          │                       │      TEXT        │◄─── text part (same state)
            │          │──── functionCall ────►┌──────────────────┐
            └──────────┘                       │    TOOL_USE      │
                ▲                              └──────────────────┘
                │                                  │         ▲
                │  flush()                         │         │
                │  (close final block,             │         │
                │   reset to IDLE)                 │         │
                │                                  │         │
      ┌──────── │ ─────────────────────────────────┘         │
      │         │                                            │
      │  THINKING ─── flush() ───►  IDLE                     │
      │  TEXT ─────── flush() ───►  IDLE                     │
      │  TOOL_USE ─── flush() ───►  IDLE                     │
      │                                                      │
      │  Any state can transition to any other state:        │
      │  THINKING → TEXT:       close thinking, open text    │
      │  THINKING → TOOL_USE:  close thinking, open tool_use │
      │  TEXT → THINKING:       close text, open thinking    │
      │  TEXT → TOOL_USE:       close text, open tool_use    │
      │  TOOL_USE → TOOL_USE:  close tool_use, open tool_use │
      │  TOOL_USE → TEXT:       close tool_use, open text    │
      │  TOOL_USE → THINKING: close tool_use, open thinking  │
      └──────────────────────────────────────────────────────┘
```

> **`flush()` transition:** When the stream ends, the orchestrator calls `flush()` on the FSM to close the last open block and reset to `IDLE`. This is a terminal operation — no further `process()` calls should follow a `flush()`.

**Input classification table:**

| Google Part Shape | Classified As | Anthropic Block Type |
|---|---|---|
| `{ thought: true, text }` | `thinking` | `thinking` |
| `{ text }` (no `thought`) | `text` | `text` |
| `{ functionCall }` | `tool_use` | `tool_use` |

**Transition rule:** When the input type differs from `currentState` (or `currentState` is `IDLE`), close the current block (if any) and open a new one. When the input type matches `currentState`, emit only a delta. **Exception:** `tool_use` always opens a new block (each tool call is a separate block per Anthropic protocol).

```javascript
/**
 * Block-type definitions: maps input type to block start/delta constructors.
 *
 * Each entry defines how to create the content_block for block_start,
 * and how to create the delta for block_delta. This table replaces
 * the triplicated if/else branches.
 */
const BLOCK_TYPES = {
    thinking: {
        makeStartBlock: () => ({ type: 'thinking', thinking: '' }),
        makeDelta: (part) => ({ type: 'thinking_delta', thinking: part.text || '' }),
        alwaysNewBlock: false,
    },
    text: {
        makeStartBlock: () => ({ type: 'text', text: '' }),
        makeDelta: (part) => ({ type: 'text_delta', text: part.text }),
        alwaysNewBlock: false,
    },
    tool_use: {
        makeStartBlock: (part) => ({
            type: 'tool_use',
            id: part.functionCall.id || `toolu_${randomHex(12)}`,
            name: part.functionCall.name,
            input: {},
        }),
        makeDelta: (part) => ({
            type: 'input_json_delta',
            partial_json: JSON.stringify(part.functionCall.args || {}),
        }),
        alwaysNewBlock: true,
    },
};

/**
 * Classify a Google part into a block type key.
 *
 * @param {Object} part - A Google content part
 * @returns {string|null} One of 'thinking', 'text', 'tool_use', or null
 */
function classifyPart(part) {
    if (part.thought === true) return 'thinking';
    if (part.text !== undefined) return 'text';
    if (part.functionCall) return 'tool_use';
    return null;
}

/**
 * Content Block FSM.
 *
 * Manages content block lifecycle (start/delta/stop) with a
 * table-driven transition model. Replaces the triplicated
 * if/else branches in the original streamSSEResponse.
 */
class ContentBlockFSM {
    #state = null;       // Current block type: null | 'thinking' | 'text' | 'tool_use'
    #blockIndex = 0;     // Monotonically increasing block index
    #hasOpened = false;  // Whether any block has been opened
    #hasToolUse = false; // Whether any tool_use block has been emitted

    /**
     * Process a single Google part and return the Anthropic SSE events to emit.
     *
     * @param {Object} part - Google content part
     * @returns {Array<{event: string, data: Object}>} Events to emit
     */
    process(part) {
        const type = classifyPart(part);
        if (!type) return [];

        const def = BLOCK_TYPES[type];
        const events = [];

        // Determine if a new block is needed
        const needsNewBlock = this.#state !== type || def.alwaysNewBlock;

        if (needsNewBlock) {
            // Close current block (if any)
            if (this.#state !== null) {
                events.push({
                    event: 'content_block_stop',
                    data: { type: 'content_block_stop', index: this.#blockIndex },
                });
                this.#blockIndex++;
            }

            // Open new block
            this.#state = type;
            this.#hasOpened = true;
            events.push({
                event: 'content_block_start',
                data: {
                    type: 'content_block_start',
                    index: this.#blockIndex,
                    content_block: def.makeStartBlock(part),
                },
            });
        }

        // Emit delta
        events.push({
            event: 'content_block_delta',
            data: {
                type: 'content_block_delta',
                index: this.#blockIndex,
                delta: def.makeDelta(part),
            },
        });

        // Thinking signature (sub-event within thinking block)
        if (type === 'thinking' && part.thoughtSignature?.length >= 50) {
            events.push({
                event: 'content_block_delta',
                data: {
                    type: 'content_block_delta',
                    index: this.#blockIndex,
                    delta: {
                        type: 'signature_delta',
                        signature: part.thoughtSignature,
                    },
                },
            });
        }

        // Track tool_use for stop_reason
        if (type === 'tool_use') {
            this.#hasToolUse = true;
        }

        return events;
    }

    /**
     * Flush: close the final open block (if any).
     *
     * @returns {Array<{event: string, data: Object}>} Final close event(s)
     */
    flush() {
        if (this.#state === null) return [];
        const events = [{
            event: 'content_block_stop',
            data: { type: 'content_block_stop', index: this.#blockIndex },
        }];
        this.#state = null;
        return events;
    }

    get hasToolUse() {
        return this.#hasToolUse;
    }
}
```

**Key properties of this design:**

- **Adding a new block type** (e.g., `server_tool_use`) requires adding one entry to `BLOCK_TYPES`, one case to `classifyPart`, and zero changes to FSM logic.
- The `alwaysNewBlock` flag captures the Anthropic semantic that each `tool_use` call occupies its own block, distinguishing it from `thinking`/`text` which coalesce consecutive parts.
- The `process()` method is **deterministic and synchronous** — given the same state and input sequence, it produces the same output events and state transitions — making it independently unit-testable without async machinery. (Note: `process()` is not a pure function in the strict sense, as it mutates internal state via `#state`, `#blockIndex`, and `#hasToolUse`. However, the mutation is fully encapsulated within the class boundary.)

#### 4.2.4 Orchestrator + Protocol Envelope — `streamSSEResponse()`

The top-level `streamSSEResponse` composes all layers and handles the protocol envelope inline (see §4.1 note). It is responsible for:
1. Emitting `message_start` (once, on first content).
2. Feeding Google parts through the FSM.
3. Accumulating usage metadata.
4. Emitting `message_delta` + `message_stop` at the end.
5. Forwarding debug logging to `formatAndLog()`.

```javascript
/**
 * Format and optionally log an SSE event.
 *
 * @param {string} event - Event type
 * @param {Object} data - Event data
 * @param {boolean} debug - Whether to log
 * @returns {string} Formatted SSE string
 */
function formatAndLog(event, data, debug) {
    const sse = formatSSE(event, data);
    if (debug) {
        console.error(`[Odin:debug] → SSE:`, sse.trimEnd());
    }
    return sse;
}

export async function* streamSSEResponse(stream, model, debug = false) {
    const messageId = `msg_${randomHex(16)}`;
    let inputTokens = 0;
    let outputTokens = 0;
    let cacheReadTokens = 0;
    let stopReason = null;
    let hasEmittedStart = false;

    const fsm = new ContentBlockFSM();

    // Layer 1 → Layer 2 pipeline
    const events = parseGoogleSSEEvents(readSSELines(stream), debug);

    for await (const { parts, usage, finishReason } of events) {
        // Accumulate usage
        if (usage) {
            inputTokens = usage.promptTokenCount || inputTokens;
            outputTokens = usage.candidatesTokenCount || outputTokens;
            cacheReadTokens = usage.cachedContentTokenCount || cacheReadTokens;
        }

        // Emit message_start on first content
        if (!hasEmittedStart && parts.length > 0) {
            hasEmittedStart = true;
            yield formatAndLog('message_start', {
                type: 'message_start',
                message: {
                    id: messageId,
                    type: 'message',
                    role: 'assistant',
                    content: [],
                    model,
                    stop_reason: null,
                    stop_sequence: null,
                    usage: {
                        input_tokens: inputTokens - cacheReadTokens,
                        output_tokens: 0,
                        cache_read_input_tokens: cacheReadTokens,
                        cache_creation_input_tokens: 0,
                    },
                },
            }, debug);
        }

        // Feed parts through FSM
        for (const part of parts) {
            for (const fsmEvent of fsm.process(part)) {
                yield formatAndLog(fsmEvent.event, fsmEvent.data, debug);
            }
        }

        // Track finish reason
        if (finishReason && !stopReason) {
            stopReason = finishReason === 'MAX_TOKENS' ? 'max_tokens' : 'end_turn';
        }
    }

    // Flush FSM (close final block)
    for (const fsmEvent of fsm.flush()) {
        yield formatAndLog(fsmEvent.event, fsmEvent.data, debug);
    }

    // Override stop reason if FSM saw tool_use
    if (fsm.hasToolUse && !stopReason) {
        stopReason = 'tool_use';
    }

    // Protocol envelope: message_delta + message_stop
    yield formatAndLog('message_delta', {
        type: 'message_delta',
        delta: { stop_reason: stopReason || 'end_turn', stop_sequence: null },
        usage: {
            output_tokens: outputTokens,
            cache_read_input_tokens: cacheReadTokens,
            cache_creation_input_tokens: 0,
        },
    }, debug);

    yield formatAndLog('message_stop', { type: 'message_stop' }, debug);
}
```

**Line count comparison:**

| Component | Before | After |
|---|---|---|
| Stream framing | inline (11 lines) | `readSSELines` (17 lines) |
| Event parsing | inline (15 lines) | `parseGoogleSSEEvents` (25 lines) |
| Content block logic | inline (~110 lines) | `ContentBlockFSM` (~80 lines) + `BLOCK_TYPES` (~25 lines) |
| Protocol envelope | inline (~35 lines) | inline in `streamSSEResponse` (~40 lines) |
| Debug logging | 14 inline `if (debug)` blocks | `formatAndLog` (6 lines) + call sites |
| **Total** | **~240 lines (1 function)** | **~195 lines (4 functions + 1 class)** |

The raw line count reduction is modest (~19%), but the **cognitive complexity** drops significantly: each unit has a single responsibility, and the FSM transition table is inspectable at a glance.

### 4.3 Design Rationale

**1. Class-based FSM vs. plain function.** The FSM needs mutable state (`#state`, `#blockIndex`) that persists across calls. A class with private fields makes this explicit and prevents external mutation. An alternative — a closure returning a `process` function — would work equally well but loses the self-documenting property of the class name and method signatures. The class also enables future extension points (e.g., `onStateChange` hooks for metrics).

**2. `BLOCK_TYPES` table vs. strategy pattern.** A lookup table is simpler than a full strategy/polymorphism hierarchy for three block types. If block types proliferate beyond ~6, refactoring to a strategy pattern would be warranted.

**3. `alwaysNewBlock` flag for tool_use.** In the Anthropic protocol, each `tool_use` is a separate content block (unlike `thinking` and `text`, which coalesce consecutive parts). Rather than special-casing `tool_use` in the FSM logic, the flag makes this semantic difference data-driven and explicit.

**4. `tool_use` emits complete `partial_json` in a single delta.** In the native Anthropic SSE stream, `tool_use` blocks typically receive multiple incremental `input_json_delta` events that together form the complete JSON input. However, since Google Antigravity SSE delivers each `functionCall` as a complete object (with `name` and `args` fully populated in a single SSE event), the FSM emits the entire `args` as one `input_json_delta` immediately after `content_block_start`. This is protocol-compliant — the Anthropic spec does not require `partial_json` to be split across multiple deltas — but consumers should be aware that each `tool_use` block will contain exactly one `input_json_delta` followed by `content_block_stop`, rather than the multi-delta pattern seen in native Anthropic streams.

**5. Async generator pipeline vs. transform streams.** WHATWG `TransformStream` is a viable alternative, but async generators compose more naturally in Node.js and match the existing `async function*` signature. Transform streams would add API surface without clarity benefit.

**6. `process()` returns events array (not yields).** Returning an array makes `ContentBlockFSM` a synchronous, deterministic object whose internal mutation is fully encapsulated. This is trivially unit-testable without async machinery — call `process()`, assert on the returned array, inspect state via `hasToolUse`. The calling async generator handles yielding.

**7. `formatAndLog` extraction.** The 14 identical `if (debug) console.error(...)` blocks are replaced by a single helper. This is a cross-cutting concern that doesn't belong in either the FSM or the protocol envelope.

**8. Signature handling inside FSM.** The `thoughtSignature` sub-event remains inside the FSM's `process()` method (rather than being a separate post-processing step) because it is semantically part of the thinking block's delta sequence and must be emitted at the correct `blockIndex`. Extracting it would require the caller to know about thinking-specific semantics, defeating the FSM's encapsulation.

**9. `stopReason` priority — intentional behavioral change.** The current implementation assigns `stopReason = 'tool_use'` unconditionally inside the `functionCall` branch, meaning `tool_use` always wins regardless of any subsequent `finishReason`. The refactored version changes the priority order: `finishReason` (from Google's upstream signal) takes precedence, and `tool_use` is only applied as a fallback when no `finishReason` was received (`if (fsm.hasToolUse && !stopReason)`). This is an **intentional semantic improvement**: if Google reports `MAX_TOKENS` as the finish reason, it should propagate as `max_tokens` to the Anthropic consumer rather than being silently overwritten by `tool_use`. In practice, this edge case (receiving both `functionCall` parts and `finishReason: 'MAX_TOKENS'` in the same stream) is unlikely in normal operation, but the refactored priority is more correct. The FSM itself does not track `stopReason` — it only exposes the `hasToolUse` flag, keeping protocol-level semantics in the orchestrator where they belong.

## 5. Implementation Plan

### Phase 1: Extract Layers 1 & 2 — 0.5 days

- [x] Extract `readSSELines(stream)` async generator
- [x] Extract `parseGoogleSSEEvents(lines, debug)` async generator
- [x] Verify `streamSSEResponse` produces identical output by composing the new layers inline

**Done when:** All existing integration tests pass with the extracted layers; byte-for-byte output verified with debug logging.

### Phase 2: Implement Content Block FSM — 1 day

- [x] Define `BLOCK_TYPES` lookup table
- [x] Implement `classifyPart(part)` classifier
- [x] Implement `ContentBlockFSM` class with `process()` and `flush()`
- [x] Unit tests for FSM state transitions (see §6)
- [x] Replace inline block logic in `streamSSEResponse` with FSM calls

**Done when:** FSM unit tests pass; `streamSSEResponse` integration tests produce identical output.

### Phase 3: Extract Debug Logging + Final Cleanup — 0.5 days

- [x] Extract `formatAndLog()` helper
- [x] Remove all inline `if (debug)` blocks from `streamSSEResponse`
- [x] Final integration test pass
- [x] Code review

**Done when:** No triplicated patterns remain; all tests green; debug output identical.

## 6. Testing Strategy

The FSM is independently unit-testable (synchronous, no I/O), which is a significant improvement over the current implementation where testing requires mocking a `ReadableStream`.

**Key Scenarios:**

| # | Scenario | Input | Expected Behavior |
|---|----------|-------|-------------------|
| 1 | Single thinking block | `[{ thought: true, text: "hmm" }]` | `block_start(thinking, 0)` → `block_delta(thinking_delta, 0)` |
| 2 | Single text block | `[{ text: "hello" }]` | `block_start(text, 0)` → `block_delta(text_delta, 0)` |
| 3 | Thinking → text transition | `[{ thought: true, text: "..." }, { text: "hi" }]` | `start(thinking,0)` → `delta(0)` → `stop(0)` → `start(text,1)` → `delta(1)` |
| 4 | Consecutive thinking (coalesce) | `[{ thought: true, text: "a" }, { thought: true, text: "b" }]` | `start(thinking,0)` → `delta(0,"a")` → `delta(0,"b")` — no stop/start between |
| 5 | Consecutive text (coalesce) | `[{ text: "a" }, { text: "b" }]` | `start(text,0)` → `delta(0,"a")` → `delta(0,"b")` — no stop/start between |
| 6 | Tool use (always new block) | `[{ functionCall: { name: "f1", args: {} } }, { functionCall: { name: "f2", args: {} } }]` | `start(tool_use,0)` → `delta(0)` → `stop(0)` → `start(tool_use,1)` → `delta(1)` |
| 7 | Text → tool_use transition | `[{ text: "I'll call" }, { functionCall: { name: "f", args: {} } }]` | `start(text,0)` → `delta(0)` → `stop(0)` → `start(tool_use,1)` → `delta(1)` |
| 8 | Thinking with signature | `[{ thought: true, text: "x", thoughtSignature: "sig...50chars" }]` | `start(thinking,0)` → `delta(thinking_delta,0)` → `delta(signature_delta,0)` |
| 9 | Thinking with short signature (ignored) | `[{ thought: true, text: "x", thoughtSignature: "short" }]` | `start(thinking,0)` → `delta(thinking_delta,0)` — no signature_delta |
| 10 | `flush()` closes final block | After processing `[{ text: "hi" }]`, call `flush()` | `stop(0)` |
| 11 | `flush()` on idle (no-op) | Call `flush()` with no prior input | Empty array |
| 12 | Unknown part type (ignored) | `[{ unknownField: 123 }]` | Empty array — `classifyPart` returns null |
| 13 | End-to-end: full stream | Complete Google SSE stream with thinking + text + tool_use | Byte-for-byte identical to current `streamSSEResponse` output |
| 14 | `hasToolUse` flag | Process parts including `functionCall` | `fsm.hasToolUse === true` |
| 15 | Empty parts array | SSE event with `parts: []` | No FSM events emitted |

## 7. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Subtle behavioral difference in edge cases (e.g., empty text parts, missing fields) | Med | High | Byte-for-byte comparison test against current implementation using captured real SSE streams |
| `BLOCK_TYPES` table becomes stale if Anthropic adds new block types | Low | Low | Same risk as current code; table is easier to update than scattered if/else |
| Performance regression from async generator chaining overhead | Low | Low | Micro-benchmark; V8 optimizes async generator chains well; stream I/O dominates latency |
| FSM `process()` returning arrays creates GC pressure from short-lived arrays | Low | Low | Typical array is 2-3 elements; negligible vs. JSON.parse/stringify per SSE event |

## 8. Future Work

- **`redacted_thinking` block type** — When Anthropic adds redacted thinking support, add one entry to `BLOCK_TYPES` and one case to `classifyPart`.
- **`server_tool_use` block type** — Same pattern for server-side tool execution blocks.
- **Metrics hooks** — Add an optional `onTransition(from, to)` callback to `ContentBlockFSM` for observability.
- **Error recovery** — Add retry/reconnect logic in Layer 1 (`readSSELines`) for transient network failures; orthogonal to the FSM design.
- **Transform stream variant** — If the codebase migrates to WHATWG Streams API, refactor Layer 1/2 into `TransformStream` instances while keeping the FSM unchanged.

## 9. References

- `odin/src/converter.js` — Current `streamSSEResponse` implementation (lines 601–841)
- `odin/src/server.js` — Consumer of `streamSSEResponse` (lines 228–240)
- [Anthropic Streaming Messages API](https://docs.anthropic.com/en/api/messages-streaming)
- [Google Generative AI — Streaming](https://ai.google.dev/gemini-api/docs/text-generation#generate-a-text-stream)
- [MDN — Async Generators](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Statements/async_function*)

---

## Changelog

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2026-02-21 | Chason Tang | Initial version |
