# RFC-008: Antigravity SSE Response Validation with Ajv

**Version:** 1.1  
**Author:** Chason Tang  
**Date:** 2026-02-22  
**Status:** Proposed

---

## 1. Summary

Odin's SSE event parser (`parseGoogleSSEEvents`) processes Antigravity streaming responses with defensive optional chaining (`?.`) but no structural validation. If the upstream API sends a malformed response — wrong types, missing fields, or unexpected structure — the converter silently produces incorrect Anthropic-format output or throws cryptic errors deep in the Content Block FSM. This RFC proposes adding Ajv-based JSON Schema validation to each Antigravity SSE event payload within the `parseGoogleSSEEvents` pipeline. The schema is compiled once at module load (reusing the existing Ajv dependency from RFC-003) and validates each event inline, logging structured warnings on failure without interrupting the stream. This provides an early-warning system for upstream API changes at negligible performance cost.

## 2. Motivation

### 2.1 Current Response Processing State

The SSE event parser in `converter.js` (lines 520–547) handles Antigravity responses with a pattern of optional chaining and fallback defaults:

```javascript
const data = JSON.parse(jsonText);
const inner = data.response || data;

yield {
    parts: inner.candidates?.[0]?.content?.parts || [],
    usage: inner.usageMetadata || null,
    finishReason: inner.candidates?.[0]?.finishReason || null,
    responseId: inner.responseId || null,
};
```

This "best-effort extraction" approach has several blind spots:

| Missing Validation | What Happens Today |
|---|---|
| `response` envelope missing | `data.response` is `undefined`, fallback to `data` — parser silently treats the raw envelope as the response body, extracting wrong fields |
| `candidates` is a string or number | `candidates?.[0]` returns `undefined` → parts = `[]` → event silently dropped, no visibility |
| `parts[].text` is a number instead of string | `part.text` is truthy → FSM emits `text_delta` with a numeric value → downstream client receives type-invalid data |
| `usageMetadata.promptTokenCount` is a string | Token count becomes `"123"` instead of `123` → Anthropic response `usage` field has wrong type |
| `finishReason` is `"DONE"` (not `STOP`/`MAX_TOKENS`/`OTHER`) | `streamSSEResponse` maps unknown values to `"end_turn"` — silently masks a potential upstream contract change |
| New part type (e.g., `codeExecution`) appears | `classifyPart()` returns `null` → part silently skipped with zero visibility |
| `responseId` missing from all events | `streamSSEResponse` throws `Error('Antigravity response missing required responseId field')` only after the first content-bearing event, producing a partial stream followed by an unrecoverable error |

The pattern is clear: the parser has no early-warning system for upstream API changes. Failures are either silent (wrong types propagate to the client) or catastrophic (late throws after partial streaming has begun).

### 2.2 Why Response Validation Matters

Antigravity is an external API managed by a separate team within Google. Its response format has already evolved — the `thoughtSignature` field for thinking models and `cachedContentTokenCount` for prompt caching are additions that arrived without advance notice to downstream consumers.

RFC-003 introduced Ajv-based validation for the **request** path (Anthropic → Odin). This RFC completes the "validate at boundaries" strategy by adding validation on the **response** path (Antigravity → Odin):

```
Client (Claude Code)
        │
        │  Anthropic format request
        ▼
┌─────────────────────────┐
│   RFC-003: Ajv request  │ ← Validate inbound
│   validation (existing) │
└────────────┬────────────┘
             │
             ▼
       anthropicToGoogle()
             │
             ▼
    Antigravity Cloud Code
             │
             │  SSE response stream
             ▼
┌─────────────────────────┐
│  RFC-008: Ajv response  │ ← Validate outbound (this RFC)
│  validation (proposed)  │
└────────────┬────────────┘
             │
             ▼
       ContentBlockFSM
             │
             ▼
  Anthropic SSE → Client
```

Together they provide:

1. **Early detection** — Schema violations surface immediately as structured warnings, not as cryptic downstream bugs in the FSM or client.
2. **Change monitoring** — Validation failures on new fields, renamed values, or structural changes signal Antigravity API evolution before it causes production incidents.
3. **Debugging aid** — Validation errors include the event data and error paths, making it easy to reproduce and diagnose upstream issues.

### 2.3 Why Ajv (Again)

Ajv is already a runtime dependency (added in RFC-003). Reusing it for response validation:

- Adds zero new dependencies.
- Follows the established pattern in `src/validator.js`.
- Provides the same compile-once performance characteristics (~0.01ms per validation for the event payload size).
- Keeps all schema validation in the Ajv ecosystem, enabling potential future schema consolidation.

## 3. Goals and Non-Goals

**Goals:**

- Define a JSON Schema for Antigravity SSE event payloads that validates the structure consumed by `parseGoogleSSEEvents()` and the downstream `ContentBlockFSM`.
- Compile the schema once at module load using the existing Ajv dependency — zero per-request compilation overhead.
- Validate each SSE event inline within `parseGoogleSSEEvents()`, logging structured warnings on validation failure without interrupting the stream.
- Detect upstream API changes (new part types, renamed fields, structural changes) through validation failure logs before they cause downstream bugs.

**Non-Goals:**

- **Stream-level validation** (e.g., "responseId must appear in the first event", "finishReason must appear exactly once in the final event") — these are temporal/stateful invariants that JSON Schema cannot express. Deferred to future work as a potential FSM-level assertion layer.
- **Blocking invalid events** — Odin must remain resilient to minor upstream deviations. Validation operates in warn mode only; events are always processed best-effort regardless of validation outcome.
- **Validating the SSE transport layer** (line framing, `data:` prefix) — this is handled by Layer 1 (`readline.createInterface()`, see RFC-009) and is out of scope.
- **Validating Antigravity error responses** (HTTP 4xx/5xx bodies) — error handling in `server.js` lines 158–209 already processes these. This RFC covers only successful streaming SSE event payloads.
- **Enforcing `additionalProperties: false`** — Antigravity may add new fields at any time. The schema validates known structure without rejecting unknown additions.

## 4. Design

### 4.1 Overview

The design introduces one new file (`src/response-validator.js`) and modifies one existing file (`src/converter.js`). The validation module follows the same architecture as `src/validator.js` (RFC-003): schema defined in JavaScript, compiled once by Ajv at module load, exported as a reusable validation function.

**Module structure:**

```
src/response-validator.js (new)
┌──────────────────────────────────────────────────┐
│                                                  │
│  ┌────────────────────────────────────────┐      │
│  │  antigravitySSEEventSchema (const)     │      │
│  │                                        │      │
│  │  {                                     │      │
│  │    type: "object",                     │      │
│  │    required: ["response"],             │      │
│  │    properties: {                       │      │
│  │      response: { ... },                │      │
│  │      traceId: ...,                     │      │
│  │      metadata: ...                     │      │
│  │    }                                   │      │
│  │  }                                     │      │
│  └───────────────┬────────────────────────┘      │
│                  │ compiled at module load       │
│                  ▼                               │
│  ┌────────────────────────────────────────┐      │
│  │  Ajv instance                          │      │
│  │  ajv.compile(schema)                   │      │
│  │       │                                │      │
│  │       ▼                                │      │
│  │  validate (compiled fn)                │      │
│  └────────────────────────────────────────┘      │
│                                                  │
│  Exports:                                        │
│    validateSSEEvent(data)                        │
│      → { valid: true }                           │
│      → { valid: false, errors: [...] }           │
└───────────────────────┬──────────────────────────┘
                        │
                        │  imported by
                        ▼
src/converter.js (modified)
┌──────────────────────────────────────────────────┐
│                                                  │
│  parseGoogleSSEEvents(lines, debug):             │
│                                                  │
│    for await (const line of lines) {             │
│      const data = JSON.parse(jsonText);          │
│                                                  │
│      // ── NEW: validate SSE event ──            │
│      const v = validateSSEEvent(data);           │
│      if (!v.valid) {                             │
│        console.error("[Odin] SSE response        │
│          validation warning:", v.errors);        │
│      }                                           │
│                                                  │
│      // ── existing extraction (unchanged) ──    │
│      const inner = data.response || data;        │
│      yield { parts, usage, ... };                │
│    }                                             │
│                                                  │
└──────────────────────────────────────────────────┘
```

**Integration point within the existing 3-layer SSE pipeline:**

```
Antigravity SSE Stream
        │
        ▼
Layer 1: readline.createInterface()  (byte stream → lines)
        │
        ▼
Layer 2: parseGoogleSSEEvents()  (lines → structured events)
        │
        ├─ JSON.parse()
        ├─ validateSSEEvent()    ← NEW (this RFC)
        ├─ extract inner fields
        └─ yield event
        │
        ▼
Layer 3: ContentBlockFSM         (events → Anthropic SSE)
        │
        ▼
Anthropic SSE Output
```

Validation sits in Layer 2 immediately after `JSON.parse()` and before field extraction. This is the natural insertion point because: (a) the JSON is already parsed — schema validation requires a parsed object, (b) it precedes all downstream consumers — invalid data is flagged before the FSM processes it, and (c) it is the single centralized point where all event data flows through.

### 4.2 Detailed Design

#### 4.2.1 Antigravity SSE Event Schema

The schema is derived from observed Antigravity SSE response payloads (see `response.txt` and the [Antigravity API Spec](../opencode-antigravity-auth/docs/ANTIGRAVITY_API_SPEC.md) §Response Format). Every observed event follows this structure:

```
data: {
  "response": {                                    ← response envelope
    "candidates": [{                               ← always single-element array
      "content": {
        "role": "model",                           ← always "model"
        "parts": [                                 ← array of content parts
          { "text": "..." }                        ← text part
          { "thought": true, "text": "..." }       ← thinking part (Claude)
          { "thought": true,
            "thoughtSignature": "...",
            "text": "" }                           ← thinking end (Claude)
          { "functionCall":
              { "name": "...", "args": {}, "id": "..." }
          }                                        ← tool call part
        ]
      },
      "finishReason": "STOP"                       ← only in final event
    }],
    "usageMetadata": {
      "promptTokenCount": 304,
      "candidatesTokenCount": 4,
      "totalTokenCount": 308,
      "cachedContentTokenCount": 456               ← optional (prompt caching)
    },
    "modelVersion": "claude-opus-4-6-thinking",
    "responseId": "req_vrtx_011CYHicRkRQpGDXoLvsU921"
  },
  "traceId": "69ceaff3e15e0e1e",
  "metadata": {}
}
```

**Full schema definition:**

```javascript
const antigravitySSEEventSchema = {
    type: 'object',
    required: ['response'],
    properties: {
        response: {
            type: 'object',
            required: ['candidates', 'responseId'],
            properties: {
                candidates: {
                    type: 'array',
                    maxItems: 1,
                    items: {
                        type: 'object',
                        properties: {
                            content: {
                                type: 'object',
                                required: ['role', 'parts'],
                                properties: {
                                    role: {
                                        type: 'string',
                                    },
                                    parts: {
                                        type: 'array',
                                        items: {
                                            type: 'object',
                                            anyOf: [
                                                {
                                                    required: ['text'],
                                                    properties: {
                                                        text: { type: 'string' },
                                                        thought: { type: 'boolean' },
                                                        thoughtSignature: { type: 'string' },
                                                    },
                                                },
                                                {
                                                    required: ['functionCall'],
                                                    properties: {
                                                        functionCall: {
                                                            type: 'object',
                                                            required: ['name'],
                                                            properties: {
                                                                name: { type: 'string', minLength: 1 },
                                                                args: { type: 'object' },
                                                                id: { type: 'string' },
                                                            },
                                                        },
                                                    },
                                                },
                                            ],
                                        },
                                    },
                                },
                            },
                            finishReason: {
                                type: 'string',
                                enum: ['STOP', 'MAX_TOKENS', 'OTHER'],
                            },
                        },
                    },
                },
                usageMetadata: {
                    type: 'object',
                    required: ['promptTokenCount', 'candidatesTokenCount', 'totalTokenCount'],
                    properties: {
                        promptTokenCount: { type: 'integer', minimum: 0 },
                        candidatesTokenCount: { type: 'integer', minimum: 0 },
                        totalTokenCount: { type: 'integer', minimum: 0 },
                        cachedContentTokenCount: { type: 'integer', minimum: 0 },
                    },
                },
                modelVersion: { type: 'string' },
                responseId: { type: 'string' },
            },
        },
        traceId: { type: 'string' },
        metadata: { type: 'object' },
    },
};
```

**Schema coverage map:**

| Field | Observed in Production | Schema Validation | Downstream Consumer |
|-------|----------------------|-------------------|-------------------|
| `response` | Every event | `required`, `type: 'object'` | `data.response \|\| data` in `parseGoogleSSEEvents` |
| `response.candidates` | Every event | `required`, `type: 'array'`, `maxItems: 1` | `inner.candidates?.[0]` |
| `candidates[].content` | Every event | `type: 'object'` | `?.content?.parts` |
| `candidates[].content.role` | Always `"model"` | `required`, `type: 'string'` | Not consumed directly (assumed) |
| `candidates[].content.parts` | Every event | `required`, `type: 'array'`, items validated via `anyOf` | FSM processes each part via `classifyPart()` |
| `parts[]: text/thinking` | Text and thinking events | `anyOf` branch 1: `required: ['text']`, `text: string`, `thought: boolean`, `thoughtSignature: string` | `part.thought`, `part.text`, `part.thoughtSignature` in `classifyPart()` and `ContentBlockFSM` |
| `parts[]: functionCall` | Tool call events | `anyOf` branch 2: `required: ['functionCall']`, `name: string`, `args: object`, `id: string` | `part.functionCall` in `classifyPart()` and `BLOCK_TYPES.tool_use` |
| `candidates[].finishReason` | Final event only | `type: 'string'`, `enum: ['STOP', 'MAX_TOKENS', 'OTHER']` | `finishReason` in `streamSSEResponse` → stop_reason mapping |
| `response.usageMetadata` | Every event | `type: 'object'`, `required: [promptTokenCount, candidatesTokenCount, totalTokenCount]`, properties `integer >= 0` | Token counts in `streamSSEResponse` |
| `response.usageMetadata.cachedContentTokenCount` | Prompt caching | `type: 'integer'`, `minimum: 0` | `usage.cachedContentTokenCount` in `streamSSEResponse` |
| `response.modelVersion` | Every event | `type: 'string'` | Not consumed directly (informational) |
| `response.responseId` | Every event | `required`, `type: 'string'` | `messageId` in `streamSSEResponse` — **required for message_start** |
| `traceId` | Every event | `type: 'string'` | Not consumed (debugging) |
| `metadata` | Every event (empty `{}`) | `type: 'object'` | Not consumed (future use) |

**Schema design principles:**

1. **Strict on types** — Every field that Odin consumes has its type validated (string, integer, boolean, array, object). This catches the most impactful category of errors: type mismatches that propagate silently.
2. **Strict on enums** — `finishReason` is constrained to `['STOP', 'MAX_TOKENS', 'OTHER']`. New values will trigger a validation warning, giving visibility into upstream changes.
3. **Strict on critical fields, permissive on the rest** — Fields that the downstream code critically depends on (`candidates`, `responseId` in `response`; `promptTokenCount`, `candidatesTokenCount`, `totalTokenCount` in `usageMetadata`) are `required`. Without `candidates`, the event has no content; without `responseId`, `streamSSEResponse` throws a runtime error after partial streaming. Other fields remain optional — SSE events may legitimately omit them (e.g., `finishReason` in non-final events). This balances early detection of critical failures against false-positive avoidance.
4. **Permissive on unknown fields** — `additionalProperties` is not set to `false` at any level. Antigravity may add new fields (as it has historically) without breaking validation.
5. **`anyOf` for part types** — Parts are validated with `anyOf` matching either text/thinking (requires `text`) or function call (requires `functionCall`). A new part type that matches neither branch will trigger a validation warning — this is the **desired behavior** for detecting new upstream part types.

#### 4.2.2 Response Validator Module

**File:** `src/response-validator.js` (new)

The module mirrors the structure of `src/validator.js` (RFC-003): schema definition, Ajv compilation, error formatting, and a single exported validation function.

```javascript
import Ajv from 'ajv';

// Schema compiled once at module load — no per-request overhead
const ajv = new Ajv({ allErrors: true });
const validate = ajv.compile(antigravitySSEEventSchema);

/**
 * Format Ajv validation errors into a concise diagnostic string.
 *
 * @param {import('ajv').ErrorObject[]} errors
 * @returns {string}
 */
function formatValidationErrors(errors) {
    const messages = errors.map((err) => {
        const path = err.instancePath
            ? err.instancePath.slice(1).replace(/\//g, '.')
            : '(root)';
        return `${path} ${err.message}`;
    });
    const unique = [...new Set(messages)];
    return unique.join('; ');
}

/**
 * Validate an Antigravity SSE event payload.
 *
 * @param {Object} data - Parsed JSON from an SSE data line
 * @returns {{ valid: true } | { valid: false, errors: string }}
 */
export function validateSSEEvent(data) {
    if (validate(data)) {
        return { valid: true };
    }
    return { valid: false, errors: formatValidationErrors(validate.errors) };
}
```

**Ajv configuration:**

| Option | Value | Rationale |
|--------|-------|-----------|
| `allErrors` | `true` | Report all validation failures per event. A single log line captures every issue, enabling complete diagnosis without replaying the stream. Matches RFC-003 configuration. |

The `strict` mode is left at default (`true` in Ajv v8). The `discriminator` option (used by the request validator in `validator.js`) is not needed here — the response schema does not use discriminated unions. `additionalProperties` is not specified at any schema level, defaulting to "allow all" for forward compatibility.

#### 4.2.3 Integration into SSE Event Parser

**File:** `src/converter.js` (modified)

Validation is inserted into `parseGoogleSSEEvents()` immediately after `JSON.parse()`, before the existing field extraction logic. The existing extraction is unchanged — validation is purely additive.

**Before (lines 520–547):**

```javascript
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
                responseId: inner.responseId || null,
            };
        } catch (e) {
            if (debug) {
                console.error(`[Odin:debug] ← SSE parse error:`, e.message);
            }
        }
    }
}
```

**After:**

```javascript
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

            try {
                const validation = validateSSEEvent(data);
                if (!validation.valid) {
                    console.error(
                        `[Odin] SSE response validation warning: ${validation.errors}` +
                            (debug ? ` | raw: ${jsonText.slice(0, 500)}` : ''),
                    );
                }
            } catch (validationError) {
                console.error(
                    `[Odin] SSE validator internal error:`,
                    validationError.message,
                );
            }

            const inner = data.response || data;

            yield {
                parts: inner.candidates?.[0]?.content?.parts || [],
                usage: inner.usageMetadata || null,
                finishReason: inner.candidates?.[0]?.finishReason || null,
                responseId: inner.responseId || null,
            };
        } catch (e) {
            if (debug) {
                console.error(`[Odin:debug] ← SSE parse error:`, e.message);
            }
        }
    }
}
```

**Key design decisions in the integration:**

1. **Always validate, always yield.** Validation failure does not prevent the event from being processed. The `yield` statement executes regardless of validation outcome. This ensures Odin remains resilient to minor upstream deviations.

2. **Always log validation failures.** Unlike debug-only logging (`if (debug)`), validation warnings are always emitted to stderr. Rationale: validation failures indicate potential upstream contract changes that warrant attention regardless of debug mode. The false-positive rate is expected to be zero under normal operation (the schema was derived from observed production traffic).

3. **Debug mode controls raw data inclusion.** When `debug` is true, the warning includes the first 500 characters of the raw JSON payload. When false, only the validation error paths and messages are logged. This balances diagnostic value against log volume.

4. **Validation has its own error boundary.** `validateSSEEvent` is wrapped in a dedicated `try/catch` separate from the JSON parse handler. If the validator itself throws (e.g., due to an Ajv bug), the error is logged with a distinct `[Odin] SSE validator internal error:` prefix, preventing confusion with JSON parse failures. Stream processing continues regardless.

#### 4.2.4 Validation Behavior: Warn Mode

The validation operates exclusively in **warn mode**: validate every event, log failures, never block. This is the only mode.

**Rationale for warn-only (no strict/blocking mode):**

| Concern | Why warn-only is correct |
|---------|------------------------|
| Antigravity is an external API | Odin cannot control upstream response format. Blocking would turn a minor format deviation into a complete request failure. |
| Partial streams are worse than degraded data | If Odin has already emitted `message_start` and streaming `content_block_delta` events, blocking a mid-stream event would leave the client in an unrecoverable state. |
| The current code already handles missing fields | Optional chaining and defaults (`\|\| []`, `\|\| null`) provide graceful degradation. Validation adds visibility, not new failure modes. |
| New fields should not break Odin | When Antigravity adds a field, Odin should continue working and log that something new appeared — not refuse to process the response. |

A future RFC may introduce a configurable strict mode for testing environments (e.g., CI pipelines that replay captured SSE traffic), but this is explicitly out of scope.

### 4.3 Design Rationale

**Why a separate `response-validator.js` instead of extending `validator.js`?**

`validator.js` is focused on inbound request validation (Anthropic → Odin). Response validation (Antigravity → Odin) is a distinct concern with a different schema, different error handling strategy (warn vs reject), and different consumer (`converter.js` vs `server.js`). Separating them follows the single-responsibility principle and avoids coupling inbound and outbound validation concerns. Each validator module encapsulates its own Ajv instance, schema, and error formatting — no shared state.

**Why validate the full SSE event envelope (including `response` wrapper) rather than the inner response object?**

The code extracts the inner response via `data.response || data`. If the `response` key is absent or has the wrong type, this fallback silently treats the entire payload as the response body — potentially extracting `traceId` or `metadata` as if they were response fields. Validating the full envelope catches this case. The schema's `required: ['response']` constraint means that an unwrapped response (which would lack the `response` key) triggers a validation warning, giving visibility into this exact scenario.

**Why `anyOf` (not `oneOf`) for part items?**

`oneOf` requires exactly one branch to match. If a part has both `text` and `functionCall` (unlikely but theoretically possible), `oneOf` would fail because both branches match. `anyOf` succeeds as long as at least one branch matches, which is the correct semantic — we want to validate that the part has a recognizable structure, not enforce mutual exclusivity. In practice, `classifyPart()` has its own priority order (`thought` → `text` → `functionCall`) that handles ambiguous parts correctly.

**Why `maxItems: 1` on `candidates`?**

The Antigravity API consistently returns a single candidate per SSE event. Odin reads only `candidates[0]` — additional candidates would be silently ignored. A `maxItems: 1` constraint surfaces this case as a validation warning rather than silent data loss. This is particularly valuable if Antigravity ever introduces multi-candidate streaming (e.g., for sampling/best-of-N), which would require explicit Odin support.

**Why not validate `content.role` as `enum: ['model']`?**

While every observed SSE event has `role: "model"`, the schema validates `role` only as `type: 'string'` without an enum constraint. Rationale: if Antigravity introduces a new role value (e.g., `"tool"` for server-side tool execution results), an enum-based schema would flag every such event as invalid. Since Odin does not consume the `role` field from SSE responses (it is implied by the streaming context), a type check provides sufficient structural validation without risking false positives on a field Odin ignores.

**Performance impact:**

| Metric | Value | Method |
|--------|-------|--------|
| Schema compilation (once at startup) | ~2ms | Ajv compiles to optimized JS function |
| Per-event validation | ~0.01–0.02ms | Compiled validator on small JSON payload (~200 bytes typical) |
| Typical stream (200 events) | ~2–4ms total | Negligible vs. model inference (seconds) and network RTT |
| Large stream (10K events) | ~100–200ms total | Still negligible — <0.5% of total response time |

## 5. Implementation Plan

### Phase 1: Create Response Validator Module — 20 minutes

- [ ] Create `src/response-validator.js` with the JSON Schema from §4.2.1.
- [ ] Implement Ajv compilation and `validateSSEEvent()` function from §4.2.2.
- [ ] Implement `formatValidationErrors()` for structured error output.
- [ ] Export `validateSSEEvent` as the public API.
- [ ] Verify the module loads without errors: `node -e "import('./src/response-validator.js')"`.

**Done when:** The module exports a working `validateSSEEvent` function that accepts a parsed SSE event object and returns `{ valid: true }` or `{ valid: false, errors: string }`.

### Phase 2: Integrate into SSE Event Parser — 15 minutes

- [ ] Import `validateSSEEvent` in `src/converter.js`.
- [ ] Add validation call inside `parseGoogleSSEEvents()` per §4.2.3.
- [ ] Ensure the existing `yield` is unchanged — validation is additive only.
- [ ] Run `npm run check` to verify ESLint and Prettier compliance.

**Done when:** `parseGoogleSSEEvents()` validates every parsed SSE event and logs warnings on failure, without altering the yielded event data. `npm run check` passes.

### Phase 3: Verification against Production Traffic — 30 minutes

- [ ] Start Odin and send a request through the proxy to a thinking model (e.g., `claude-opus-4-6-thinking`). Verify the stream completes successfully with zero validation warnings.
- [ ] Send a request to a non-thinking model (e.g., `claude-sonnet-4-6`). Verify zero warnings.
- [ ] Replay the captured `response.txt` traffic through the validator (standalone test script). Verify all 354 events pass validation.
- [ ] Manually inject a malformed event (e.g., `parts[0].text` as a number) and verify the validation warning is logged with the correct error path.

**Done when:** Real Antigravity traffic produces zero validation warnings, and synthetic malformed events produce accurate, path-specific warnings.

## 6. Testing Strategy

The testing approach validates the schema against real production traffic and synthetic edge cases, ensuring both correctness and forward compatibility.

- **Schema unit testing:** Import `validateSSEEvent` directly and assert results for a matrix of valid and invalid payloads. Test against events from `response.txt` (known-good production traffic) and synthetic malformed events.
- **Integration testing:** Start Odin, proxy a real request, verify the stream completes with zero validation warnings in stderr.
- **Regression testing:** Verify that adding validation does not change the data yielded by `parseGoogleSSEEvents()` — the function's output must be byte-identical before and after this change.

**Key Scenarios:**

| # | Scenario | Input | Expected Behavior |
|---|----------|-------|-------------------|
| 1 | Valid text event | `{ response: { candidates: [{ content: { role: "model", parts: [{ text: "hello" }] } }], usageMetadata: { promptTokenCount: 10, candidatesTokenCount: 1, totalTokenCount: 11 }, modelVersion: "claude-sonnet-4-6", responseId: "msg_vrtx_123" }, traceId: "abc", metadata: {} }` | `{ valid: true }` |
| 2 | Valid thinking event (Claude) | Parts: `[{ thought: true, text: "Let me think..." }]` | `{ valid: true }` |
| 3 | Valid thinking end with signature | Parts: `[{ thought: true, thoughtSignature: "RXBz...==", text: "" }]` | `{ valid: true }` |
| 4 | Valid function call event | Parts: `[{ functionCall: { name: "get_weather", args: { location: "Paris" }, id: "toolu_vrtx_123" } }]` | `{ valid: true }` |
| 5 | Valid final event with finishReason | Candidate includes `finishReason: "STOP"` | `{ valid: true }` |
| 6 | Valid event with empty parts | Parts: `[]` | `{ valid: true }` |
| 7 | Valid event with unknown usageMetadata field | `usageMetadata: { promptTokenCount: 10, candidatesTokenCount: 1, totalTokenCount: 11, newField: 42 }` | `{ valid: true }` (unknown fields allowed) |
| 8 | Missing `response` wrapper | `{ candidates: [...], traceId: "..." }` | `{ valid: false }` — missing required `response` |
| 9 | `candidates` is a string | `response.candidates: "invalid"` | `{ valid: false }` — `candidates` must be array |
| 10 | `candidates` has 2 elements | `response.candidates: [{...}, {...}]` | `{ valid: false }` — `maxItems: 1` |
| 11 | `parts[].text` is a number | Parts: `[{ text: 42 }]` | `{ valid: false }` — `text` must be string |
| 12 | `parts[].thought` is a string | Parts: `[{ thought: "yes", text: "..." }]` | `{ valid: false }` — `thought` must be boolean |
| 13 | `usageMetadata.promptTokenCount` is a string | `usageMetadata: { promptTokenCount: "304", candidatesTokenCount: 4, totalTokenCount: 308 }` | `{ valid: false }` — must be integer |
| 14 | `finishReason` is `"DONE"` | Candidate: `{ finishReason: "DONE" }` | `{ valid: false }` — not in enum `[STOP, MAX_TOKENS, OTHER]` |
| 15 | `functionCall.name` missing | Parts: `[{ functionCall: { args: {} } }]` | `{ valid: false }` — missing required `name` |
| 16 | `functionCall.name` is empty string | Parts: `[{ functionCall: { name: "", args: {} } }]` | `{ valid: false }` — `minLength: 1` |
| 17 | Unknown part type | Parts: `[{ codeExecution: { code: "print(1)" } }]` | `{ valid: false }` — matches no `anyOf` branch (desired: detects new part type) |
| 18 | `content` missing `parts` | `content: { role: "model" }` | `{ valid: false }` — missing required `parts` |
| 19 | `metadata` is a string | `metadata: "unexpected"` | `{ valid: false }` — must be object |
| 20 | Full production event from response.txt | Actual event line from captured traffic | `{ valid: true }` |
| 21 | All production events from response.txt | All 354 event lines | Every event: `{ valid: true }` |

## 7. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Schema generates false-positive warnings on valid Antigravity responses | Low | Medium | Schema derived from real production traffic (response.txt, 354 events). Verification phase (§5 Phase 3) replays actual traffic. Unknown fields are allowed at all levels. |
| Validation overhead degrades streaming latency | Very Low | Low | Per-event validation is ~0.01ms. Even 10K-event streams add <200ms — negligible vs. model inference time. Benchmark in Phase 3. |
| `console.error` logging on every validation failure floods stderr in a degraded upstream scenario | Low | Medium | Validation failures are expected to be extremely rare (zero in normal operation). If Antigravity makes a breaking structural change, the flood of warnings is actually desirable — it's a high-signal alarm. A future RFC can add rate-limiting if needed. |
| Schema becomes stale as Antigravity evolves | Medium | Low | The schema is permissive — new fields are accepted without warning. Staleness means we miss validation for new fields (same as today's status quo). New enum values or structural changes produce warnings, which is the desired detection mechanism. |
| Ajv compiled function throws on unexpected input types | Very Low | Low | `validateSSEEvent` is wrapped in its own `try/catch` with a distinct `[Odin] SSE validator internal error:` log prefix. Any Ajv exception is caught and clearly distinguished from JSON parse errors. Stream processing continues. |

## 8. Future Work

- **Stream-level invariant assertions** — Validate temporal properties that JSON Schema cannot express: "responseId must be present in the first content-bearing event", "finishReason must appear exactly once and only in the final event", "parts must not be empty in non-final events". These could be implemented as assertions within `streamSSEResponse()` or as a dedicated stream-level validator.
- **Validation metrics** — Expose validation pass/fail counts as structured metrics (e.g., total events validated, total warnings, breakdown by error path). This enables monitoring dashboards and alerting on upstream format changes.
- **Rate-limited logging** — If validation failures become frequent (e.g., due to a new Antigravity field that triggers `anyOf` failures on every event), add a rate limiter to collapse repeated warnings into periodic summaries.
- **Configurable strict mode for CI** — Allow replaying captured SSE traffic through the validator in strict mode (fail-fast) for regression testing in CI pipelines.
- **Request validation for the Google-format request** — Validate the `googleRequest` object produced by `anthropicToGoogle()` before sending it to Antigravity, completing the bidirectional validation boundary (inbound Anthropic → outbound Google → inbound Antigravity response).

## 9. References

- [RFC-003: JSON Schema Request Validation with Ajv](rfc_003_ajv_request_validation.md) — Established the Ajv validation pattern, schema design principles, and `additionalProperties` philosophy used in this RFC.
- [RFC-007: SSE Stream Converter — Finite State Machine Refactor](rfc_007_sse_stream_finite_state_machine.md) — Defines the 3-layer SSE pipeline architecture (stream framer → event parser → content block FSM) that this RFC integrates into.
- [Antigravity Unified Gateway API Specification](../../opencode-antigravity-auth/docs/ANTIGRAVITY_API_SPEC.md) — Response format documentation (§Response Format, §Streaming Response, §Thinking Response).
- [Ajv JSON Schema Validator](https://ajv.js.org/) — Library documentation.
- `odin/src/converter.js:520–547` — `parseGoogleSSEEvents()` function (modification target).
- `odin/src/converter.js:590–596` — `classifyPart()` function (defines which part fields are consumed).
- `odin/src/converter.js:732–818` — `streamSSEResponse()` function (consumes parsed events).
- `odin/src/validator.js` — Existing request validator (architectural precedent).
- `response.txt` — Captured Antigravity SSE traffic from `claude-opus-4-6-thinking` (354 events, used for schema derivation and validation testing).

---

## Changelog

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.1 | 2026-02-22 | Chason Tang | Review fixes: added `required` constraints for critical fields in `response` and `usageMetadata`; removed Gemini thinking part references (Odin only supports Claude models); isolated validation `try/catch` from JSON parse error handler |
| 1.0 | 2026-02-22 | Chason Tang | Initial version |
