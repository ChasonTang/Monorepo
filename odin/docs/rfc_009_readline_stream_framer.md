# RFC-009: Replace Manual Stream Framer with Node.js `readline`

**Version:** 1.1  
**Author:** Chason Tang  
**Date:** 2026-02-26  
**Status:** Implemented

---

## 1. Summary

The SSE stream framer (`readSSELines`) currently performs manual byte decoding, string concatenation, and line splitting in JavaScript. This RFC proposes deleting the function entirely and inlining Node.js's native `readline` module backed by `Readable.fromWeb()` at the call site, delegating all buffering and line-terminator handling to Node.js's C++ internals. The change eliminates 22 lines of manual state management in favor of a single `createInterface()` expression, improves correctness by handling all three SSE-legal line terminators (`\n`, `\r\n`, `\r`), and preserves the existing `AsyncIterable<string>` contract consumed by Layer 2 (`parseGoogleSSEEvents`).

## 2. Motivation

**Problem:** The current `readSSELines` implementation in `converter.js` (lines 610–631) manually reimplements functionality that Node.js provides natively:

```javascript
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

This approach has three specific drawbacks:

1. **Incomplete line-terminator handling.** The SSE specification (WHATWG HTML Living Standard, §9.2.6) defines three legal line terminators: `\n` (LF), `\r\n` (CRLF), and `\r` (CR). The current implementation only splits on `\n`, meaning a bare `\r` terminator would be silently concatenated into the adjacent line. While Google Antigravity's SSE responses currently use `\n`, this is an upstream implementation detail — not a contractual guarantee.

2. **Redundant JavaScript-layer work.** `TextDecoder` + `buffer.split('\n')` performs string concatenation and split operations in JavaScript for every chunk received. Node.js's `readline` module performs the equivalent work in its C++ binding (`string_decoder` + internal line parser), avoiding intermediate JS string allocations.

3. **Surface area for bugs.** The manual buffer pattern (accumulate → split → keep remainder) is a well-known source of off-by-one and encoding-boundary bugs. The current implementation is correct, but it requires careful reading to verify — the `{ stream: true }` flag on `TextDecoder.decode()`, the `lines.pop()` remainder trick, and the final `if (buffer)` flush all need to be understood as a unit.

**Value of solving:** The `readline` module has been in Node.js since v0.1.98, is used by millions of production applications, handles all line terminators correctly, and delegates performance-critical work to native code. Replacing `readSSELines` with `readline` eliminates a class of potential bugs while reducing code that must be reviewed, tested, and maintained.

## 3. Goals and Non-Goals

**Goals:**
- Delete the `readSSELines` function and inline `readline.createInterface()` + `Readable.fromWeb()` at the call site, eliminating the wrapper entirely.
- Correctly handle all three SSE-legal line terminators (`\n`, `\r\n`, `\r`).
- Preserve the `AsyncIterable<string>` contract consumed by `parseGoogleSSEEvents`.
- Maintain byte-for-byte output compatibility for all inputs using `\n` as line terminator (the current production behavior).

**Non-Goals:**
- Refactoring Layers 2 or 3 of the SSE pipeline — this change is scoped exclusively to Layer 1.
- Adding error recovery or retry logic to the stream framer — orthogonal concern, deferred per RFC-007 §8.
- Performance benchmarking — the bottleneck is network I/O, not line splitting; the native-code delegation is a correctness and maintainability improvement, not a performance optimization.

## 4. Design

### 4.1 Overview

The change deletes the `readSSELines` async generator and inlines a `readline.createInterface()` call directly at the consumption site in `streamSSEResponse`. The `readline` interface implements `Symbol.asyncIterator`, making it a drop-in replacement for the async generator.

```
                        BEFORE                              AFTER
┌──────────────────────────────────┐   ┌──────────────────────────────────────────┐
│  ReadableStream (Web API)        │   │  ReadableStream (Web API)                │
│         │                        │   │         │                                │
│         ▼                        │   │         ▼                                │
│  stream.getReader()              │   │  Readable.fromWeb(stream)                │
│         │                        │   │         │                                │
│         ▼                        │   │         ▼                                │
│  TextDecoder + manual buffer     │   │  readline.createInterface()              │
│  + split('\n') + remainder       │   │  (C++ line parser, handles \n \r\n \r)   │
│         │                        │   │         │                                │
│         ▼                        │   │         ▼                                │
│  AsyncGenerator<string>          │   │  AsyncIterable<string>                   │
│         │                        │   │         │                                │
│         ▼                        │   │         ▼                                │
│  parseGoogleSSEEvents()          │   │  parseGoogleSSEEvents()                  │
└──────────────────────────────────┘   └──────────────────────────────────────────┘
```

The downstream consumer (`parseGoogleSSEEvents`) accepts `AsyncIterable<string>` and is unaffected by the change.

### 4.2 Detailed Design

#### 4.2.1 New Imports

Two new `node:` imports are added to `converter.js`:

```javascript
import { createInterface } from 'node:readline';
import { Readable } from 'node:stream';
```

Both are Node.js built-in modules with zero external dependencies.

#### 4.2.2 Delete `readSSELines` and Inline at Call Site

The entire `readSSELines` function (lines 599–631, including its section comment and JSDoc) is deleted. A single-expression wrapper function that merely forwards one argument to a standard library constructor carries no semantic weight — it exists only as indirection.

**Deleted (22 lines + 10 lines comment/JSDoc):**

```javascript
// ─── SSE Stream Layer 1: Stream Framer ───────────────────────────────────────

/**
 * Read a ReadableStream and yield individual text lines.
 * ...
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

#### 4.2.3 Call Site Update

The call site in `streamSSEResponse` is updated to inline the `readline` construction:

**Before:**

```javascript
const events = parseGoogleSSEEvents(readSSELines(stream), debug);
```

**After:**

```javascript
const lines = createInterface({ input: Readable.fromWeb(stream), crlfDelay: Infinity });
const events = parseGoogleSSEEvents(lines, debug);
```

The two-line form is chosen over a single nested expression for readability — it names the intermediate `lines` iterable, matching the parameter name in `parseGoogleSSEEvents(lines, debug)`.

Key details on the inlined expression:

- **`Readable.fromWeb(stream)`**: Converts the WHATWG `ReadableStream` (from `fetch().body`) into a Node.js `Readable` stream. This is the official Node.js bridge between Web Streams and Node Streams. Available since Node.js 17.0.0; stable since Node.js 20.0.0.

- **`createInterface({ input, crlfDelay: Infinity })`**: Creates a `readline.Interface` that reads from the Node.js stream and yields lines. The `crlfDelay: Infinity` option ensures that `\r\n` is always treated as a single line break regardless of timing — the `\r` and `\n` may arrive in separate chunks, and without this option, the `\r` could be treated as a standalone line terminator with a 100ms heuristic delay.

- **`readline.Interface` as `AsyncIterable<string>`**: The returned interface implements `Symbol.asyncIterator`, conforming to the same contract that the deleted async generator provided. `parseGoogleSSEEvents` accepts any `AsyncIterable<string>` and is unaffected.

### 4.3 Design Rationale

**1. `readline` over manual buffer.** The `readline` module exists precisely to solve the "read lines from a stream" problem. It handles encoding, buffering, line-terminator detection, and backpressure in native code. Re-implementing this in JavaScript provides no benefit and creates maintenance burden.

**2. `crlfDelay: Infinity`.** Without this option, `readline` uses a 100ms heuristic to distinguish between `\r` (standalone CR) and `\r\n` (CRLF). In a streaming context where `\r` and `\n` may arrive in separate TCP segments, this heuristic can misfire. Setting `Infinity` disables the heuristic and always treats `\r` followed by `\n` as a single delimiter, which matches the SSE specification's intent. This is also the [recommended setting](https://nodejs.org/api/readline.html#rlsymbolasynciterator) in Node.js documentation for `for await...of` usage.

**3. `Readable.fromWeb()` over `stream.Readable.from()`.** `Readable.fromWeb()` is the official Node.js API for converting WHATWG ReadableStreams. An alternative — `Readable.from()` which accepts async iterables — would require first getting the stream's reader and wrapping it, adding unnecessary indirection. `fromWeb()` is the direct, semantic-correct bridge.

**4. Inline over wrapper function.** A wrapper function whose body is a single expression (`return createInterface(...)`) adds a layer of indirection without carrying any semantic weight — it does not compose multiple operations, enforce preconditions, or provide a domain-specific name that improves comprehension beyond what the `createInterface` call already communicates. Inlining at the call site with a descriptive local variable (`const lines = ...`) achieves the same readability without the function-call hop. If future requirements (e.g., error recovery, metrics) demand a richer abstraction, a function or class can be introduced at that time — but premature abstraction over a single standard-library call is unjustified.

## 5. Implementation Plan

### Phase 1: Delete `readSSELines` and Inline `readline` — 0.5 days

- [x] Add `import { createInterface } from 'node:readline'` and `import { Readable } from 'node:stream'` to `converter.js`
- [x] Delete the `readSSELines` function and its section comment/JSDoc block
- [x] Inline `createInterface({ input: Readable.fromWeb(stream), crlfDelay: Infinity })` at the call site in `streamSSEResponse`
- [x] Run existing test suite (`npm test`) to verify no regressions
- [x] Manual end-to-end test: send a real request through Odin and verify SSE output is identical

**Done when:** All existing tests pass; manual end-to-end stream output is byte-for-byte identical for `\n`-terminated SSE responses.

### Phase 2: Add Unit Test for Line Terminator Coverage — 0.5 days

- [x] Add test cases verifying correct line splitting for `\n`, `\r\n`, and `\r` terminators
- [x] Add test case for multi-byte UTF-8 characters split across chunk boundaries
- [x] Add test case for empty lines (SSE event separator)

**Done when:** New test cases pass; `npm test` green.

## 6. Testing Strategy

The primary risk is behavioral divergence between the manual buffer and `readline`. Testing focuses on verifying that all existing behavior is preserved and that the new line-terminator handling is correct.

**Key Scenarios:**

| # | Scenario | Input | Expected Behavior |
|---|----------|-------|-------------------|
| 1 | Standard `\n` termination | `"data: {}\ndata: {}\n"` | Two lines: `"data: {}"`, `"data: {}"` |
| 2 | `\r\n` termination | `"data: {}\r\ndata: {}\r\n"` | Two lines: `"data: {}"`, `"data: {}"` |
| 3 | Bare `\r` termination | `"data: {}\rdata: {}\r"` | Two lines: `"data: {}"`, `"data: {}"` |
| 4 | Mixed terminators | `"data: a\ndata: b\r\ndata: c\r"` | Three lines: `"data: a"`, `"data: b"`, `"data: c"` |
| 5 | Empty lines (SSE separator) | `"data: {}\n\ndata: {}\n\n"` | Four lines: `"data: {}"`, `""`, `"data: {}"`, `""` |
| 6 | Multi-byte UTF-8 across chunks | `"data: 你"` + `"好\n"` (split across two chunks) | One line: `"data: 你好"` |
| 7 | Trailing data without terminator | `"data: {}\npartial"` | Two lines: `"data: {}"`, `"partial"` |
| 8 | Empty stream | (no data) | No lines yielded |
| 9 | End-to-end SSE pipeline | Full Google SSE stream through `streamSSEResponse` | Identical Anthropic SSE output to current implementation |

## 7. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| `Readable.fromWeb()` emits experimental warning on Node.js 18–19 | Low (project likely targets 20+) | Low (cosmetic stderr warning) | Document minimum Node.js 20 in README; the project already uses `fetch()` which has the same stability timeline |
| `readline` yields empty string for empty lines differently than manual split | Low | Med | Explicit test case (#5 above); `parseGoogleSSEEvents` already skips non-`data:` lines, so empty lines are harmless |
| `readline` `close` event timing differs from manual `if (buffer)` flush | Low | Med | Test case #7 verifies trailing data; `readline` documents that it emits the final line on stream `end` |
| `readline` internal buffering changes backpressure characteristics | Low | Low | SSE events are small (< 10 KB per line typically); backpressure differences are irrelevant at this scale |

## 8. Future Work

- **Error recovery in Layer 1** — With the stream now flowing through Node.js's stream infrastructure, adding `error` event handlers and retry logic becomes straightforward. Deferred per RFC-007 §8.
- **`stream.Writable` for SSE output** — The response writing in `server.js` (lines 229–239) could similarly benefit from using Node.js stream primitives instead of manual `res.write()` calls. Out of scope for this RFC.
- **Remove `TextDecoder` import** — After this change, `converter.js` no longer uses `TextDecoder`. No action needed since it is a global (not imported), but this eliminates one cognitive dependency.

## 9. References

- `odin/src/converter.js` — Current `readSSELines` implementation (lines 610–631)
- `odin/docs/rfc_007_sse_stream_finite_state_machine.md` — SSE pipeline architecture (Layer 1 is the target of this RFC)
- [Node.js `readline` documentation](https://nodejs.org/api/readline.html)
- [Node.js `stream.Readable.fromWeb()` documentation](https://nodejs.org/api/stream.html#streamreadablefromwebreadablestream-options)
- [WHATWG HTML Living Standard — Server-Sent Events §9.2.6](https://html.spec.whatwg.org/multipage/server-sent-events.html#event-stream-interpretation)
- [Node.js `readline` async iterator recommendation](https://nodejs.org/api/readline.html#rlsymbolasynciterator)

---

## Changelog

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.1 | 2026-02-26 | Chason Tang | Delete `readSSELines` entirely; inline `createInterface` at call site instead of wrapping in a function |
| 1.0 | 2026-02-26 | Chason Tang | Initial version |
