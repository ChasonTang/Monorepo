# RFC-004: Defensive Content Block Validation in Converter

**Version:** 1.1  
**Author:** Chason Tang  
**Date:** 2026-02-17  
**Status:** Implemented

---

## 1. Summary

Odin's `convertContentToParts()` function silently drops content blocks it cannot convert — non-base64 images (including URL-based), `document`, `search_result`, `redacted_thinking`, `server_tool_use`, `web_search_tool_result`, and non-text content inside `tool_result` blocks all fall through the `switch` statement or are filtered out without any indication to the caller. This RFC proposes adding a dedicated content block validation pass (`validateContentBlocks()`) that runs before conversion, rejecting requests containing unsupported content types with clear HTTP 400 errors in Anthropic format. The key design decision — HTTP 400 error vs. log-only warning — is analyzed in §4.2, with the recommendation to fail fast via HTTP 400 to prevent silent data loss.

## 2. Motivation

### 2.1 Silent Data Loss in the Current Converter

The `convertContentToParts()` function in `converter.js` (lines 737–804) processes Anthropic content blocks via a `switch` statement that handles five types: `text`, `image` (base64 only), `tool_use`, `tool_result`, and `thinking`. Any content block type not covered by these cases is silently dropped — no error, no warning, no log entry. The content simply vanishes.

This creates three distinct categories of silent data loss:

**Category 1: Image blocks with URL sources**

The `image` case (lines 757–765) only processes `base64`-encoded images:

```javascript
case 'image':
    if (block.source?.type === 'base64') {
        parts.push({
            inlineData: {
                mimeType: block.source.media_type,
                data: block.source.data,
            },
        });
    }
    break;
```

When Claude Code sends an image with `source.type: 'url'` (e.g., a web image reference), the entire `if` body is skipped. The image is silently dropped. The model receives a conversation that references an image it never sees — leading to confused or hallucinated responses about visual content.

**Category 2: Unsupported content block types**

The Anthropic Messages API defines several content block types that Odin's converter does not handle:

| Type | Anthropic Purpose | What Happens in Odin |
|------|-------------------|---------------------|
| `document` | PDF document content (DocumentBlockParam) | Silently dropped (no `case` in switch) |
| `search_result` | Web search results (SearchResultBlockParam) | Silently dropped |
| `redacted_thinking` | Redacted thinking from previous turns (RedactedThinkingBlockParam) | Silently dropped |
| `server_tool_use` | Server-side tool invocations — bash, text_editor (ServerToolUseBlockParam) | Silently dropped |
| `web_search_tool_result` | Web search tool results (WebSearchToolResultBlockParam) | Silently dropped |

These types fall through the `switch` statement silently. As Claude Code evolves and adopts new Anthropic API features, the probability of encountering these block types increases. Without explicit detection, each new unsupported type will produce the same silent-drop behavior.

**Category 3: Non-text content inside tool_result blocks**

The `tool_result` case (lines 777–788) delegates to `extractTextContent()` (lines 718–728):

```javascript
function extractTextContent(content) {
    if (typeof content === 'string') return content;
    if (Array.isArray(content)) {
        return content
            .filter((c) => c.type === 'text')
            .map((c) => c.text)
            .join('\n');
    }
    return '';
}
```

When a tool result contains an `image` block (e.g., a screenshot captured by a tool), the `.filter((c) => c.type === 'text')` silently excludes it. The tool's visual output is lost, and the model receives only the textual portion of the result.

### 2.2 Why Silent Data Loss Is Worse Than an Error

Silent data loss is the worst failure mode for a proxy because it violates the principle of least surprise:

1. **The request appears to succeed** — HTTP 200 is returned, the SSE stream is delivered, the model responds. Nothing indicates that content was lost.
2. **The model's response is degraded but plausible** — Missing an image, the model might say "I can't see any image" or hallucinate content. Missing a tool result's screenshot, the model might guess at the visual state. These responses look wrong but are difficult to attribute to a proxy-side conversion issue.
3. **Debugging requires deep knowledge** — To diagnose the issue, the developer must: (a) suspect the proxy is dropping content, (b) enable `--debug` mode and inspect the converted Google request, (c) compare it against the original Anthropic request to identify missing blocks. This investigative chain is non-obvious and time-consuming.

An explicit error, by contrast, is immediately visible, self-documenting, and actionable.

### 2.3 Increasing Attack Surface from Claude Code Evolution

Claude Code is actively evolving. Recent versions have introduced:

- PDF document support (`type: 'document'`)
- Web search capabilities (`type: 'web_search_tool_result'`)
- Server-side tools (`type: 'server_tool_use'`)

As Claude Code adopts new Anthropic API features, the likelihood of Odin receiving unsupported content block types increases. Without defensive validation, each new feature will produce the same silent-failure pattern — working correctly on direct Anthropic API but silently degrading through Odin.

## 3. Goals and Non-Goals

**Goals:**

- Detect and reject requests containing content block types that Odin's converter cannot process, before attempting conversion.
- Detect and reject image blocks with any `source.type` other than `'base64'` (only `base64` is supported by the Google API conversion path). This uses a whitelist approach: `'url'` sources, missing `source` fields, and any future unknown source types are all rejected.
- Detect and reject `tool_result` blocks whose content array contains non-text items (only `text` is extractable).
- Return clear, path-specific HTTP 400 errors in Anthropic format (`invalid_request_error`) that tell the developer exactly which content block is unsupported and why.
- Integrate with the existing validation pipeline (after Ajv schema validation, before conversion) without modifying the `anthropicToGoogle()` function signature.

**Non-Goals:**

- Adding conversion support for `document`, `search_result`, `redacted_thinking`, `server_tool_use`, `web_search_tool_result`, or URL-based images — this is a separate, larger effort that requires understanding Google's equivalent capabilities and API constraints.
- Validating the deep structure of supported content blocks (e.g., verifying `source.media_type` is a valid MIME type for base64 images) — the current converter handles these without issues, and over-validating risks rejecting valid requests.
- Adding MCP-related content block validation (`mcp_tool_use`, `mcp_tool_result`) — Claude Code does not currently send these through the proxy endpoint. Defer until there is evidence of real traffic.

## 4. Design

### 4.1 Overview

The design introduces one new exported function (`validateContentBlocks()`) in `converter.js` and adds a single validation call in `server.js`. The function scans all content blocks in the request's `messages` array and rejects the request at the first unsupported block encountered.

```
                    POST /v1/messages
                          │
                          ▼
                 ┌──────────────────┐
                 │  1. !body check  │───── null → 400 "Request body is required"
                 └────────┬─────────┘
                          │
                          ▼
                 ┌──────────────────┐
                 │  2. Ajv schema   │───── invalid → 400 (field-level errors)
                 │     validation   │      [RFC-003]
                 └────────┬─────────┘
                          │
                          ▼
                 ┌──────────────────┐
                 │  3. Content block│───── unsupported → 400 (block-level errors)
                 │     validation   │      [THIS RFC]               ◄── NEW
                 └────────┬─────────┘
                          │
                          ▼
                 ┌──────────────────┐
                 │  4. anthropicTo  │
                 │     Google()     │───── conversion
                 └────────┬─────────┘
                          │
                          ▼
                 ┌──────────────────┐
                 │  5. sendRequest  │───── Cloud Code API
                 └──────────────────┘
```

The new validation step is positioned between Ajv schema validation (step 2, which validates structural shape) and conversion (step 4, which transforms content). This creates a layered validation pipeline:

| Layer | Validates | Catches |
|-------|-----------|---------|
| Transport | Body exists and is JSON | Missing/malformed request body |
| Schema (RFC-003) | Field types, required fields, value ranges | Wrong types, missing fields, out-of-range values |
| **Content blocks (this RFC)** | **Content block types and configurations** | **Unsupported block types, non-base64 images, non-text tool results** |
| Conversion | (implicit) Known block types converted correctly | N/A — all unsupported cases already caught |

### 4.2 Key Decision: HTTP 400 Error vs. Log-Only Warning

This RFC's central design question is how to surface unsupported content blocks to the operator and caller. Two approaches were analyzed:

#### Option A: HTTP 400 Error (Recommended)

Reject the request with a 400 `invalid_request_error` before conversion. The full request body is captured in the NDJSON log file (via the existing `logger.log()` call), and a `console.error` warning is emitted for immediate stderr visibility.

**Behavior:** Client receives a clear error response. No request is sent to Cloud Code API. The original request body is logged for forensic analysis.

```
Client                    Odin                        Cloud Code API
  │                        │                                │
  │── POST /v1/messages ──▶│                                │
  │   (with document block)│                                │
  │                        │── validate content blocks      │
  │                        │   ✗ unsupported: "document"    │
  │                        │                                │
  │◀── 400 ────────────────│   (request NOT forwarded)      │
  │   invalid_request_error│                                │
  │   "Unsupported content │                                │
  │    block type..."      │                                │
```

#### Option B: Log-Only Warning (Rejected)

Accept the request, log a warning about the unsupported content block, proceed with conversion (silently dropping the unsupported block), and forward the incomplete request to Cloud Code API.

**Behavior:** Client receives a 200 response with a model response based on incomplete context. The warning is recorded in the log file but not visible to the client.

```
Client                    Odin                        Cloud Code API
  │                        │                                │
  │── POST /v1/messages ──▶│                                │
  │   (with document block)│                                │
  │                        │── validate content blocks      │
  │                        │   ⚠ unsupported: "document"    │
  │                        │   (log warning, continue)      │
  │                        │                                │
  │                        │── anthropicToGoogle()          │
  │                        │   (document block dropped)     │
  │                        │                                │
  │                        │── sendRequest() ──────────────▶│
  │                        │   (incomplete request)         │
  │                        │                                │
  │                        │◀── 200 SSE stream ─────────────│
  │◀── 200 SSE stream ─────│   (response based on           │
  │                        │    incomplete context)         │
```

#### Decision Analysis

| Criterion | Option A: HTTP 400 | Option B: Log-Only |
|-----------|--------------------|--------------------|
| **Data integrity** | No data loss — request rejected before conversion | Silent data loss — unsupported blocks dropped |
| **Debuggability** | Error is immediately visible to the client | Must check log files after observing degraded model behavior |
| **API quota** | Zero Cloud Code API cost for unsupported requests | Full API cost for a request that produces degraded results |
| **User experience** | Clear, actionable error: "This content type is not supported" | Confusing model response: "I don't see any image" / hallucination |
| **Fail-fast principle** | ✓ Problems caught at the earliest possible point | ✗ Problems deferred to model response interpretation |
| **Consistency** | Consistent with Ajv validation (RFC-003): validate early, fail clearly | Inconsistent — Ajv rejects malformed structure, but content-level issues silently pass |
| **Disruption risk** | Higher — requests that previously "worked" (with silent degradation) will now fail with 400 | Lower — existing behavior unchanged |
| **Recovery path** | Client can inspect error, remove unsupported content, retry | Client must: notice degradation → check logs → identify dropped content → retry |

**Decision: Option A (HTTP 400 Error).**

The decisive factor is that **all three categories of unsupported content represent irreversible data loss**. When a content block is silently dropped:

1. The model receives an incomplete context window.
2. The model's response is based on this incomplete context.
3. The model's degraded response may trigger additional API calls (retries, follow-up questions), compounding the cost.
4. The root cause (a dropped content block in the proxy) is non-obvious and requires deep investigation.

An HTTP 400 error breaks this chain at step 1. The cost is that requests which previously "succeeded" (with silent degradation) will now fail explicitly. This is a **feature, not a bug** — the previous "success" was illusory, and making the failure visible is strictly better for the developer experience.

Additionally, HTTP 400 errors are **already logged** by the existing server infrastructure:

```javascript
sendError(res, statusCode, errorType, message);
logger.log({ req, body, statusCode: 400, startTime });
```

The log file captures the full request body (including the unsupported content blocks) and the error status code. There is no loss of diagnostic information compared to the log-only approach — the HTTP 400 approach provides **strictly more** information (the client sees the error AND the log captures the details).

### 4.3 Detailed Design

#### 4.3.1 Unsupported Content Types Constant

**File:** `src/converter.js`

A new `Set` constant enumerates content block types that the converter cannot process. This is placed alongside the existing constants at the top of the file.

```javascript
/**
 * Content block types that convertContentToParts() cannot convert.
 * Requests containing these types should be rejected before conversion
 * to prevent silent data loss.
 *
 * @see https://docs.anthropic.com/en/api/messages
 */
const UNSUPPORTED_CONTENT_TYPES = new Set([
    'document',              // PDF content — no Google equivalent in current pipeline
    'search_result',         // Web search results — no conversion path
    'redacted_thinking',     // Redacted thinking — only produced by Anthropic native API, not in Odin proxy traffic
    'server_tool_use',       // Server-side tool invocations — not proxied
    'web_search_tool_result', // Web search tool results — no conversion path
]);
```

Using a `Set` (rather than a hardcoded list in an `if` chain) makes it trivial to add new unsupported types as Claude Code evolves. The inline comments document why each type is unsupported — this aids future maintainers deciding whether to add converter support or extend the rejection list.

#### 4.3.2 Validation Function

**File:** `src/converter.js`

The new `validateContentBlocks()` function scans all messages' content blocks and returns at the first unsupported content encountered. It follows the same `{ valid: true } | { valid: false, message: string }` return pattern established by `validateMessagesRequest()` in RFC-003.

```javascript
/**
 * Validate that all content blocks in the request are supported by the converter.
 *
 * Checks three categories:
 * 1. Unsupported content block types (document, search_result, etc.)
 * 2. Image blocks with non-base64 sources (only base64 is supported)
 * 3. Non-text content inside tool_result blocks (only text is extractable)
 *
 * This function is designed to run AFTER Ajv schema validation (which ensures
 * structural correctness) and BEFORE anthropicToGoogle() (which performs conversion).
 *
 * @param {Object} anthropicRequest - Anthropic format request body
 * @returns {{ valid: true } | { valid: false, message: string }}
 */
export function validateContentBlocks(anthropicRequest) {
    const { messages } = anthropicRequest;
    if (!Array.isArray(messages)) return { valid: true };

    for (let i = 0; i < messages.length; i++) {
        const msg = messages[i];
        const content = msg.content;
        if (!Array.isArray(content)) continue;

        for (let j = 0; j < content.length; j++) {
            const block = content[j];
            if (!block || typeof block !== 'object') continue;

            const path = `messages[${i}].content[${j}]`;

            // Category 1: Unsupported content block types
            if (UNSUPPORTED_CONTENT_TYPES.has(block.type)) {
                return {
                    valid: false,
                    message:
                        `Unsupported content block type "${block.type}" at ${path}. ` +
                        `Odin does not support "${block.type}" blocks. ` +
                        `Supported types: text, image (base64), tool_use, tool_result, thinking.`,
                };
            }

            // Category 2: Image blocks with non-base64 source (whitelist approach)
            if (block.type === 'image' && block.source?.type !== 'base64') {
                return {
                    valid: false,
                    message:
                        `Unsupported image source type "${block.source?.type ?? 'undefined'}" at ${path}.source. ` +
                        `Only base64-encoded images (source.type: "base64") are supported.`,
                };
            }

            // Category 3: Non-text content inside tool_result blocks
            if (block.type === 'tool_result' && Array.isArray(block.content)) {
                for (let k = 0; k < block.content.length; k++) {
                    const inner = block.content[k];
                    if (inner && typeof inner === 'object' && inner.type !== 'text') {
                        return {
                            valid: false,
                            message:
                                `Unsupported content type "${inner.type}" in tool_result ` +
                                `at ${path}.content[${k}]. ` +
                                `Only "text" content is supported inside tool_result blocks.`,
                        };
                    }
                }
            }
        }
    }

    return { valid: true };
}
```

**Design notes:**

- **First-error semantics:** The function returns at the first unsupported block. This is deliberate — unlike Ajv's `allErrors` mode (RFC-003), content block issues typically indicate a fundamental capability mismatch (e.g., Claude Code is using a feature Odin doesn't support). Reporting the first unsupported block is sufficient and avoids noise.
- **Array-only scanning:** String content (`typeof content === 'string'`) is skipped via the `if (!Array.isArray(content)) continue` guard. String content is always valid text and needs no block-level validation.
- **Null-safe access:** The `block.source?.type` optional chaining and `if (!block || typeof block !== 'object')` guard prevent `TypeError` on malformed blocks that passed Ajv's shallow validation.
- **Path-specific error messages:** Each error includes the exact JSON path to the problematic block (e.g., `messages[2].content[0]`), consistent with Ajv error path formatting from RFC-003. This allows the developer to locate the issue in their request payload without guessing.

#### 4.3.3 Server Integration

**File:** `src/server.js`

A new import and validation call are added after the existing Ajv validation and before the conversion call.

**New import (line 3):**

```javascript
import { anthropicToGoogle, streamSSEResponse, validateContentBlocks } from './converter.js';
```

**New validation block (after Ajv validation, before `try`):**

```javascript
// Existing: Ajv schema validation
const validation = validateMessagesRequest(body);
if (!validation.valid) {
    sendError(res, 400, 'invalid_request_error', validation.message);
    logger.log({ req, body, statusCode: 400, startTime });
    return;
}

// NEW: Content block validation
const contentValidation = validateContentBlocks(body);
if (!contentValidation.valid) {
    console.error(`[Odin] Unsupported content: ${contentValidation.message}`);
    sendError(res, 400, 'invalid_request_error', contentValidation.message);
    logger.log({ req, body, statusCode: 400, startTime });
    return;
}

try {
    // 1. Convert Anthropic request to Google format
    const model = body.model;
    const googleRequest = anthropicToGoogle(body);
    // ... existing code continues
```

The `console.error` line emits a warning to stderr regardless of `--debug` mode. This ensures that unsupported content blocks are visible in the terminal even without debug logging enabled. The `[Odin]` prefix (without `:debug`) follows the convention established for non-debug operational warnings (e.g., line 237 of `server.js`: `console.error('[Odin] Error processing /v1/messages:', err.message)`).

#### 4.3.4 Defense-in-Depth: Default Case in Switch

**File:** `src/converter.js`

As a secondary defense layer, a `default` case is added to the `switch` statement in `convertContentToParts()`. This guards against content block types that bypass the validation function (e.g., if the validation constant set falls out of sync with the converter's supported types).

```javascript
// In convertContentToParts(), after the 'thinking' case:
default:
    console.error(
        `[Odin] Warning: unhandled content block type "${block.type}" — skipped`,
    );
    break;
```

This `default` case serves as a safety net, not the primary defense. In normal operation, unsupported blocks are caught by `validateContentBlocks()` before reaching `convertContentToParts()`. The `default` case only fires if:

1. `validateContentBlocks()` has a bug (fails to catch a new unsupported type).
2. `convertContentToParts()` is called directly without validation (e.g., from a future code path).
3. A content block type is unsupported but not listed in the `UNSUPPORTED_CONTENT_TYPES` set (indicates the constant is stale).

In all three cases, the stderr warning provides immediate visibility for the operator.

### 4.4 Design Rationale

**Why validation in `converter.js` instead of `validator.js`:**

The `validator.js` module (RFC-003) handles structural schema validation using Ajv — it validates that fields have correct types, required fields are present, and values are within range. It uses a declarative JSON Schema approach.

Content block validation is fundamentally different: it validates **converter capabilities**, not schema structure. The question "can this block be converted?" depends on what `convertContentToParts()` supports, which is defined in `converter.js`. Placing the validation in the same file ensures co-location — when a developer adds support for a new content block type (e.g., adding a `case 'document':` handler), the `UNSUPPORTED_CONTENT_TYPES` set is in the same file, making it obvious that it should be updated.

Ajv JSON Schema also cannot naturally express the required constraints: "if `type` is `'image'` AND `source.type` is NOT `'base64'`, reject" requires conditional schema logic (`if/then/else` with deeply nested paths) that would be complex and fragile. Imperative JavaScript code is clearer and more maintainable for these cross-field validation rules.

**Why first-error semantics instead of `allErrors`:**

RFC-003 chose `allErrors: true` for Ajv validation because schema errors are often independent fixable issues (e.g., wrong type for `temperature` AND missing `model` — fixing one does not fix the other). Content block errors are different — they typically indicate a fundamental feature mismatch between the client and Odin. Reporting the first unsupported block is sufficient:

1. The developer fixes the one reported issue (or learns that Odin doesn't support the feature they need).
2. If there are more unsupported blocks, they'll be reported on the next request attempt.
3. In practice, unsupported blocks come in clusters (e.g., all from a single Claude Code feature adoption), so fixing the root cause often addresses all of them.

First-error semantics also produces cleaner error messages. A single-issue message like `Unsupported content block type "document" at messages[2].content[0]` is immediately actionable. A multi-issue message concatenating several unsupported types adds noise without additional diagnostic value.

**Why `console.error` in addition to `logger.log()`:**

The NDJSON log file captures complete request details including the full request body with unsupported content blocks. However, the log file is primarily for forensic analysis — it requires navigating to the file and parsing NDJSON entries. The `console.error` warning provides immediate visibility in the terminal, which is critical for:

1. **Real-time monitoring** — Operators watching the terminal see unsupported content warnings immediately.
2. **Frequency awareness** — Multiple warnings in quick succession indicate a Claude Code feature adoption that needs Odin converter support.
3. **Low-cost triage** — A glance at stderr distinguishes "Odin is rejecting requests due to unsupported content" from "Odin has an internal error" without opening log files.

**Why a separate validation function instead of inline checks in `anthropicToGoogle()`:**

An alternative approach would be to add validation logic directly inside `anthropicToGoogle()` and change its return type to include an error path. This was rejected because:

1. **Single Responsibility** — `anthropicToGoogle()` converts formats; validation is a separate concern. Mixing them makes the function harder to reason about and test.
2. **Consistent patterns** — The codebase already established the `validateX() → { valid, message }` pattern in RFC-003. Reusing this pattern reduces cognitive load for maintainers.
3. **API stability** — `anthropicToGoogle()` returns a Google request object. Changing its return type to a union (`GoogleRequest | ValidationError`) would require modifying all call sites and break the clean pipeline.

## 5. Implementation Plan

### Phase 1: Add Validation Function — 15 minutes

- [x] Add the `UNSUPPORTED_CONTENT_TYPES` constant to `src/converter.js` (after the existing constants block, before the tool schema sanitization functions).
- [x] Implement the `validateContentBlocks()` function in `src/converter.js`.
- [x] Add the `default` case to the `switch` statement in `convertContentToParts()`.
- [x] Export `validateContentBlocks` from `src/converter.js`.
- [x] Run `npm run check` to verify ESLint and Prettier compliance.

**Done when:** The function is implemented, exported, and passes lint/format checks. `node -e "import('./src/converter.js')"` succeeds without errors.

### Phase 2: Server Integration — 10 minutes

- [x] Import `validateContentBlocks` in `src/server.js`.
- [x] Add the content block validation call after Ajv validation, before the `try` block.
- [x] Run `npm run check` to verify ESLint and Prettier compliance.

**Done when:** `server.js` calls `validateContentBlocks()` and returns 400 on validation failure. `npm run check` passes.

### Phase 3: End-to-End Verification — 30 minutes

- [x] Start Odin and send a valid request with only supported content types (text, base64 image, tool_use, tool_result with text content, thinking). Verify it succeeds.
- [x] Send a request with a `document` content block. Verify HTTP 400 is returned with a clear error message mentioning "document".
- [x] Send a request with an image block using `source.type: 'url'`. Verify HTTP 400 is returned with a clear error message mentioning "url".
- [x] Send a request with a `tool_result` block whose content array contains an `image` block. Verify HTTP 400 is returned with a clear error message.
- [x] Verify the NDJSON log file captures the full request body for all rejected requests.
- [x] Run a normal Claude Code session through the proxy to verify no regression on existing workflows.

**Done when:** All rejection scenarios produce correct error responses, and normal Claude Code workflows are unaffected.

## 6. Testing Strategy

The testing approach combines unit-level function testing with integration-level HTTP testing. Since `validateContentBlocks()` is a pure function (no I/O, no side effects beyond the return value), it is straightforward to test with constructed inputs.

**Key Scenarios:**

| # | Scenario | Input | Expected Behavior |
|---|----------|-------|-------------------|
| 1 | Valid text-only message | `messages: [{ role: "user", content: [{ type: "text", text: "hello" }] }]` | `{ valid: true }` |
| 2 | Valid string content | `messages: [{ role: "user", content: "hello" }]` | `{ valid: true }` (string content skipped) |
| 3 | Valid base64 image | `messages: [{ role: "user", content: [{ type: "image", source: { type: "base64", media_type: "image/png", data: "..." } }] }]` | `{ valid: true }` |
| 4 | Valid tool_use block | `messages: [{ role: "assistant", content: [{ type: "tool_use", id: "t1", name: "fn", input: {} }] }]` | `{ valid: true }` |
| 5 | Valid tool_result with text array | `messages: [{ role: "user", content: [{ type: "tool_result", tool_use_id: "t1", content: [{ type: "text", text: "ok" }] }] }]` | `{ valid: true }` |
| 6 | Valid tool_result with string content | `messages: [{ role: "user", content: [{ type: "tool_result", tool_use_id: "t1", content: "result text" }] }]` | `{ valid: true }` (string content not array-scanned) |
| 7 | Valid thinking block | `messages: [{ role: "assistant", content: [{ type: "thinking", thinking: "...", signature: "..." }] }]` | `{ valid: true }` |
| 8 | URL image source | `messages: [{ role: "user", content: [{ type: "image", source: { type: "url", url: "https://example.com/img.png" } }] }]` | `{ valid: false }` — message mentions "url" and "base64" |
| 9 | Document block | `messages: [{ role: "user", content: [{ type: "document" }] }]` | `{ valid: false }` — message mentions "document" and lists supported types |
| 10 | Search result block | `content: [{ type: "search_result" }]` | `{ valid: false }` — message mentions "search_result" |
| 11 | Redacted thinking block | `content: [{ type: "redacted_thinking", data: "..." }]` | `{ valid: false }` — message mentions "redacted_thinking" |
| 12 | Server tool use block | `content: [{ type: "server_tool_use" }]` | `{ valid: false }` — message mentions "server_tool_use" |
| 13 | Web search tool result | `content: [{ type: "web_search_tool_result" }]` | `{ valid: false }` — message mentions "web_search_tool_result" |
| 14 | Tool result with image content | `content: [{ type: "tool_result", tool_use_id: "t1", content: [{ type: "image", source: { type: "base64" } }] }]` | `{ valid: false }` — message mentions "image" in tool_result context |
| 15 | Tool result with mixed content | `content: [{ type: "tool_result", tool_use_id: "t1", content: [{ type: "text", text: "ok" }, { type: "image" }] }]` | `{ valid: false }` — first non-text item triggers error at `content[1]` |
| 16 | Multiple unsupported blocks | `content: [{ type: "document" }, { type: "search_result" }]` | `{ valid: false }` — first unsupported block reported ("document") |
| 17 | Unsupported block in later message | `messages: [valid_msg, { role: "user", content: [{ type: "document" }] }]` | `{ valid: false }` — error path shows `messages[1].content[0]` |
| 18 | Empty messages array | `messages: []` | `{ valid: true }` (no content to validate; Ajv catches the structural issue) |
| 19 | No messages field | `{}` | `{ valid: true }` (returns early; Ajv catches the structural issue) |
| 20 | Mixed valid and invalid in same message | `content: [{ type: "text", text: "hi" }, { type: "document" }]` | `{ valid: false }` — error at `content[1]` |
| 21 | Image block without source field | `content: [{ type: "image" }]` | `{ valid: false }` — message mentions "undefined" source type and "base64" requirement |
| 22 | HTTP integration: unsupported block | POST `/v1/messages` with document block | HTTP 400, `type: "error"`, `error.type: "invalid_request_error"` |
| 23 | HTTP integration: log capture | Same as #22, inspect NDJSON log | Log entry contains full request body with document block and `statusCode: 400` |
| 24 | HTTP integration: stderr output | Same as #22, inspect stderr | `[Odin] Unsupported content: ...` line present in terminal output |

## 7. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Validation rejects requests that currently "work" (with silent degradation) | Medium | Medium | This is intentional — the previous behavior was silently lossy. The error message clearly explains what is unsupported and what is supported. If a specific rejection is too aggressive, the `UNSUPPORTED_CONTENT_TYPES` set can be narrowed in minutes. |
| Claude Code sends `redacted_thinking` blocks in multi-turn conversations | Low | Medium | `redacted_thinking` blocks only appear when the Anthropic native API redacts thinking output. Odin exclusively proxies to Google Cloud Code API, and there is no mixed-provider usage scenario — users do not simultaneously use both Odin and the Anthropic native API within the same conversation. Google's API returns standard thinking parts with `thoughtSignature`, which Odin converts to `type: 'thinking'` blocks (not `redacted_thinking`). Therefore `redacted_thinking` will not appear in normal Odin traffic. |
| `UNSUPPORTED_CONTENT_TYPES` set becomes stale as Anthropic API adds new types | Medium | Low | The `default` case in the `switch` statement provides defense-in-depth: any new type that bypasses the constant set will emit a stderr warning. Operators can then update the set. Additionally, new Anthropic content types are typically announced in changelogs, providing advance notice. |
| Validation adds per-request latency | Low | Low | The function iterates over content blocks with simple type checks (`Set.has()`, string comparisons). For a typical request (1–50 content blocks), this is < 0.01ms — negligible compared to the ~200ms+ network round-trip to Cloud Code API. |
| Error messages expose internal proxy details to clients | Low | Low | Error messages reference content block types from the public Anthropic API specification (e.g., "document", "search_result"), not Odin internals. The path format (`messages[i].content[j]`) mirrors Ajv's error paths already returned to clients in RFC-003. No sensitive information is disclosed. |

## 8. Future Work

- **Converter support for URL images** — The Google Generative AI API supports `fileData` with GCS URIs. Adding a `source.type: 'url'` conversion path (download → base64, or upload to GCS → `fileData`) would remove the need for the URL image rejection. Deferred because it requires understanding Cloud Code API's specific `fileData` requirements and adds significant complexity (download, size limits, timeout handling).
- **Converter support for `document` blocks** — PDF content could potentially be forwarded via text extraction or the Google API's document processing capabilities. Deferred because the Google API equivalent is not straightforward and may require server-side PDF processing.
- **MCP content block validation** — Add `mcp_tool_use` and `mcp_tool_result` to the unsupported types set when there is evidence of Claude Code sending these through the proxy.
- **Configurable validation strictness** — Add an `--allow-lossy` CLI flag that downgrades content block validation from HTTP 400 to log-only warning, for users who prefer best-effort conversion over strict rejection. Deferred because the strict default is the safer choice and no user has requested lossy mode.
- **Validation for system block types** — The `system` field can also contain content blocks. Currently the converter only processes `type: 'text'` blocks in `system`. Adding validation for unsupported system block types (e.g., `type: 'cache_control'` wrappers) is a natural extension.

## 9. References

- [Anthropic Messages API — Create a Message](https://docs.anthropic.com/en/api/messages/create) — Defines all content block types including `DocumentBlockParam`, `ImageBlockParam`, `ToolResultBlockParam`, etc.
- [Anthropic Messages API — Content Types](https://docs.anthropic.com/en/api/messages#content-types) — Enumerates `source.type` options for `ImageBlockParam` (`"base64"` and `"url"`).
- `odin/src/converter.js:737–804` — `convertContentToParts()` function (primary defense target).
- `odin/src/converter.js:718–728` — `extractTextContent()` function (tool_result content extraction, Category 3 data loss source).
- `odin/src/converter.js:816–906` — `anthropicToGoogle()` function (call site for content conversion).
- `odin/src/server.js:115–273` — `POST /v1/messages` handler (server integration target).
- `odin/src/validator.js` — Ajv-based request validation (RFC-003) — establishes the `{ valid: true } | { valid: false, message: string }` return pattern reused here.
- RFC-003: JSON Schema Request Validation with Ajv — Prior art for the validation pipeline architecture and Anthropic-format error response convention.

---

## Changelog

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.1 | 2026-02-17 | Chason Tang | Review fixes: image validation changed from blacklist (`source.type === 'url'`) to whitelist (`source.type !== 'base64'`) to catch all non-base64 sources including missing/unknown types; `redacted_thinking` risk likelihood downgraded Low — Odin exclusively proxies Google Cloud Code API with no mixed-provider usage; removed `redacted_thinking` passthrough from Future Work; test scenario #21 updated to `{ valid: false }` |
| 1.0 | 2026-02-17 | Chason Tang | Initial version |
