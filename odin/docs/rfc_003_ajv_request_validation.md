# RFC-003: JSON Schema Request Validation with Ajv

**Version:** 1.3  
**Author:** Chason Tang  
**Date:** 2026-02-13  
**Status:** Proposed

---

## 1. Summary

The `POST /v1/messages` endpoint in Odin currently validates incoming request bodies with three hand-coded `if` checks (`body`, `body.stream`, `body.model`). This is incomplete — malformed fields like `messages: "not-an-array"` or `max_tokens: -5` pass through to the converter or upstream API, producing confusing downstream errors. This RFC proposes introducing [Ajv](https://ajv.js.org/) (Another JSON Validator) to validate requests against a formally defined JSON Schema derived from the Anthropic Messages API specification. The schema is compiled once at startup and reused per-request, replacing the ad-hoc checks with a single `validate(body)` call that returns structured, Anthropic-format error responses.

## 2. Motivation

### 2.1 Current Validation State

The validation logic in `server.js` (lines 117–144) consists of exactly three checks:

```javascript
// Check 1: Body exists
if (!body) {
    sendError(res, 400, 'invalid_request_error', 'Request body is required');
    return;
}
// Check 2: Streaming mode
if (!body.stream) {
    sendError(res, 400, 'invalid_request_error',
        'Only streaming mode is supported. Set "stream": true in your request.');
    return;
}
// Check 3: Model present
if (!body.model) {
    sendError(res, 400, 'invalid_request_error', '"model" is required.');
    return;
}
```

These checks do not validate:

| Missing Validation | What Happens Today |
|---|---|
| `messages` is absent or not an array | `anthropicToGoogle()` throws `TypeError: messages is not iterable` → 500 "Internal proxy error" |
| `max_tokens` is absent | Request reaches Cloud Code API → upstream returns a 400 with a Google-format error, not Anthropic-format |
| `max_tokens` is a string or negative | Silent corruption — `generationConfig.maxOutputTokens` receives invalid value → unpredictable upstream behavior |
| `messages[].role` is missing or invalid | `convertContentToParts()` processes the message but maps it to wrong Google `role` → incorrect conversation structure |
| `tools` is not an array | `tools.map(...)` throws `TypeError` → 500 "Internal proxy error" |
| `temperature` is a string | `generationConfig.temperature` receives a string → upstream rejects with Google-format error |

The pattern is clear: missing validation produces either cryptic 500 errors (when JavaScript throws) or Google-format 400 errors (when the upstream API rejects). Neither provides a clean Anthropic-format `invalid_request_error` response that clients expect from a compliant Anthropic API proxy.

### 2.2 Maintenance Burden

As Odin adds support for more Anthropic API features (e.g., `tool_choice`, `metadata.user_id`, citation support), each new field requires a new hand-coded validation block. This creates:

1. **Scatter** — Validation logic is interleaved with routing logic in `server.js`, making both harder to read.
2. **Inconsistency risk** — Each check uses a slightly different pattern (truthiness check vs. type check vs. value check), and there's no systematic way to ensure completeness.
3. **No single source of truth** — The "shape" of a valid request is implicit in scattered `if` statements rather than declared in a machine-readable schema.

### 2.3 Why JSON Schema + Ajv

JSON Schema is the industry standard for declarative data shape validation. Using it provides:

1. **Declarative completeness** — The schema is the single source of truth for what constitutes a valid request. Every field, type constraint, and enum is visible in one place.
2. **Automatic error messages** — Ajv generates detailed, path-specific error messages (e.g., `"/messages/0/role" must be equal to one of the allowed values`) without any manual message authoring.
3. **Compile-once performance** — Ajv compiles JSON Schema into optimized JavaScript functions at startup. Per-request validation cost is negligible (~0.1ms for a typical request).
4. **Ecosystem alignment** — Anthropic's own API specification is expressed in terms that map directly to JSON Schema. The schema in this RFC is derived from the [official Anthropic API reference](https://docs.anthropic.com/en/api/messages).

## 3. Goals and Non-Goals

**Goals:**

- Define a JSON Schema for the `POST /v1/messages` request body that covers all fields consumed by `anthropicToGoogle()`, derived from the Anthropic API specification.
- Introduce Ajv as a runtime dependency to compile and execute schema validation.
- Replace the three ad-hoc validation checks in `server.js` with a single schema-based validation call.
- Return Anthropic-format `invalid_request_error` responses with detailed, path-specific error messages.
- Compile the schema once at module load time — zero per-request compilation overhead.

**Non-Goals:**

- Validating the full depth of every Anthropic content block type (e.g., `DocumentBlockParam`, `SearchResultBlockParam` discriminated unions) — this is deferred to future work because Odin's converter only processes `text`, `image`, `tool_use`, `tool_result`, and `thinking` blocks. Over-validating would reject valid requests using content types Odin silently passes through.
- Validating request headers (`x-api-key`, `anthropic-version`, etc.) — Odin does not use or enforce these headers; they are not part of the request body schema.
- Adding TypeScript type generation from the schema — the project uses plain JavaScript with JSDoc.
- Validating the response body from the upstream Cloud Code API — this is a separate concern and out of scope.

## 4. Design

### 4.1 Overview

The design introduces one new file (`src/validator.js`) and modifies one existing file (`src/server.js`). The schema is defined in JavaScript, compiled by Ajv at module load, and exported as a reusable validation function.

```
                    src/validator.js (new)
                 ┌────────────────────────────────────────────────┐
                 │                                                │
                 │  ┌──────────────────────────────────┐          │
                 │  │   messagesRequestSchema (const)  │          │
                 │  │                                  │          │
                 │  │  {                               │          │
                 │  │    type: "object",               │          │
                 │  │    required: ["model", ...],     │          │
                 │  │    properties: { ... }           │          │
                 │  │  }                               │          │
                 │  └──────────────┬───────────────────┘          │
                 │                 │ compiled at module load      │
                 │                 ▼                              │
                 │  ┌──────────────────────────────────┐          │
                 │  │   Ajv instance                   │          │
                 │  │   ajv.compile(schema)            │          │
                 │  │        │                         │          │
                 │  │        ▼                         │          │
                 │  │   validate (compiled fn)         │          │
                 │  └──────────────────────────────────┘          │
                 │                                                │
                 │  Exports:                                      │
                 │    validateMessagesRequest(body)               │
                 │      → { valid: true }                         │
                 │      → { valid: false, message: "..." }        │
                 └────────────────────────┬───────────────────────┘
                                          │
                                          │  imported by
                                          ▼
                    src/server.js (modified)
                 ┌────────────────────────────────────────────────┐
                 │                                                │
                 │  POST /v1/messages handler:                    │
                 │                                                │
                 │    1. readBody(req)         → body             │
                 │    2. if (!body) → 400                         │
                 │    3. validateMessagesRequest(body)            │
                 │       └─ invalid? → 400 invalid_request_error  │
                 │    4. anthropicToGoogle(body)                  │
                 │    5. sendRequest(...)                         │
                 │    6. streamSSEResponse(...)                   │
                 │                                                │
                 └────────────────────────────────────────────────┘
```

**Dependency:** `ajv` is added as a **runtime dependency** in `package.json`. This is Odin's first (and only) runtime dependency.

### 4.2 Detailed Design

#### 4.2.1 JSON Schema Definition

The schema validates the `POST /v1/messages` request body according to the [Anthropic Messages API specification](https://docs.anthropic.com/en/api/messages). It covers all fields that `anthropicToGoogle()` in `src/converter.js` destructures and processes (lines 817–828):

```javascript
const { model, messages, system, max_tokens, temperature, 
        top_p, top_k, stop_sequences, tools, thinking } = anthropicRequest;
```

The schema is intentionally **permissive on unknown fields** (`additionalProperties` is not set to `false`) to ensure forward compatibility — clients may send fields like `metadata`, `tool_choice`, or future Anthropic API additions that Odin silently ignores.

**Full schema definition:**

```javascript
const messagesRequestSchema = {
    type: 'object',
    required: ['model', 'messages', 'max_tokens', 'stream'],
    properties: {
        // ── Required Fields ──────────────────────────────────────────

        model: {
            type: 'string',
        },

        messages: {
            type: 'array',
            minItems: 1,
            maxItems: 100000,
            items: {
                type: 'object',
                required: ['role', 'content'],
                properties: {
                    role: {
                        type: 'string',
                        enum: ['user', 'assistant'],
                    },
                    content: {
                        oneOf: [
                            { type: 'string' },
                            {
                                type: 'array',
                                minItems: 1,
                                items: {
                                    type: 'object',
                                    required: ['type'],
                                    properties: {
                                        type: { type: 'string' },
                                    },
                                },
                            },
                        ],
                    },
                },
            },
        },

        max_tokens: {
            type: 'integer',
            minimum: 1,
        },

        stream: {
            const: true,
        },

        // ── Optional Fields ──────────────────────────────────────────

        system: {
            oneOf: [
                { type: 'string' },
                {
                    type: 'array',
                    minItems: 1,
                    items: {
                        type: 'object',
                        required: ['type', 'text'],
                        properties: {
                            type: { const: 'text' },
                            text: { type: 'string' },
                        },
                    },
                },
            ],
        },

        temperature: {
            type: 'number',
            minimum: 0,
            maximum: 1,
        },

        top_p: {
            type: 'number',
            minimum: 0,
            maximum: 1,
        },

        top_k: {
            type: 'integer',
            minimum: 0,
        },

        stop_sequences: {
            type: 'array',
            items: { type: 'string' },
        },

        tools: {
            type: 'array',
            items: {
                type: 'object',
                required: ['name'],
                properties: {
                    name: {
                        type: 'string',
                        minLength: 1,
                    },
                    description: {
                        type: 'string',
                    },
                    input_schema: {
                        type: 'object',
                    },
                },
            },
        },

        thinking: {
            type: 'object',
            required: ['type'],
            properties: {
                type: {
                    type: 'string',
                    enum: ['enabled', 'disabled', 'adaptive'],
                },
                budget_tokens: {
                    type: 'integer',
                    minimum: 1024,
                },
            },
            if: { required: ['type'], properties: { type: { const: 'enabled' } } },
            then: { required: ['type', 'budget_tokens'] },
        },
    },
};
```

**Schema coverage map:**

| Field | Anthropic Spec | Schema Validation | Notes |
|-------|---------------|-------------------|-------|
| `model` | Required, string | `required`, `type: 'string'`, `minLength: 1` | Prevents empty string |
| `messages` | Required, array of MessageParam (max 100,000) | `required`, `type: 'array'`, `minItems: 1`, `maxItems: 100000`, items validated | Role enum, content `oneOf [string, array]`; `maxItems` per Anthropic spec |
| `messages[].content[]` | ContentBlockParam with `type` discriminator | Only `required: ['type']`, `type: 'string'` | Shallow — deep validation deferred (see §3 Non-Goals) |
| `max_tokens` | Required, integer >= 1 | `required`, `type: 'integer'`, `minimum: 1` | Rejects strings, floats, negatives |
| `stream` | Optional boolean (Odin requires `true`) | `required`, `const: true` | Odin-specific constraint |
| `system` | Optional, string or TextBlock array | `oneOf: [string, array]`, items require `type` and `text` | `type` enforced as `const: 'text'`; array `minItems: 1` |
| `temperature` | Optional, number 0–1 | `type: 'number'`, `minimum: 0`, `maximum: 1` | Range-checked |
| `top_p` | Optional, number 0–1 | `type: 'number'`, `minimum: 0`, `maximum: 1` | Range-checked |
| `top_k` | Optional, integer >= 0 | `type: 'integer'`, `minimum: 0` | Rejects negatives |
| `stop_sequences` | Optional, string array | `type: 'array'`, items `type: 'string'` | Rejects non-string items |
| `tools` | Optional, ToolUnion array | Items require `name` only | Server tools (bash, text_editor, web_search) lack `input_schema`; custom tools carry it as optional-validated |
| `thinking` | Optional, ThinkingConfigParam | `required: ['type']`, `type` enum `['enabled', 'disabled', 'adaptive']`; `if/then` with explicit `required: ['type']` guard to require `budget_tokens` when `type` is `"enabled"` | `budget_tokens` minimum 1024 per spec; `if` clause includes `required` to avoid vacuous truth |
| `metadata` | Optional, object | Not validated (pass-through) | Odin does not process this field |
| `tool_choice` | Optional, object | Not validated (pass-through) | Odin does not process this field |

#### 4.2.2 Validator Module

**File:** `src/validator.js` (new)

The module encapsulates all validation concerns:

```javascript
import Ajv from 'ajv';

// Schema compiled once at module load — no per-request overhead
const ajv = new Ajv({ allErrors: true });
const validate = ajv.compile(messagesRequestSchema);

/**
 * Validate an Anthropic Messages API request body.
 *
 * @param {Object} body - Parsed JSON request body
 * @returns {{ valid: true } | { valid: false, message: string }}
 */
export function validateMessagesRequest(body) {
    if (validate(body)) {
        return { valid: true };
    }

    const message = formatErrors(validate.errors);
    return { valid: false, message };
}
```

**Ajv configuration:**

| Option | Value | Rationale |
|--------|-------|-----------|
| `allErrors` | `true` | Report all validation failures, not just the first. Better for developer experience — a single request attempt reveals all issues. |

The `strict` mode is intentionally left at its default (`true` in Ajv v8) to catch schema definition errors during development while allowing `additionalProperties` to remain undefined (defaults to not enforced) for forward compatibility.

#### 4.2.3 Server Integration

**File:** `src/server.js` (modified)

The existing three ad-hoc checks are replaced with a single `validateMessagesRequest()` call. The `!body` null check remains separate because it runs before JSON parsing is known to have succeeded.

**Before (lines 117–144):**

```javascript
if (!body) {
    sendError(res, 400, 'invalid_request_error', 'Request body is required');
    logger.log({ req, body, statusCode: 400, startTime });
    return;
}

if (!body.stream) {
    sendError(res, 400, 'invalid_request_error',
        'Only streaming mode is supported. Set "stream": true in your request.');
    logger.log({ req, body, statusCode: 400, startTime });
    return;
}

if (!body.model) {
    sendError(res, 400, 'invalid_request_error', '"model" is required.');
    logger.log({ req, body, statusCode: 400, startTime });
    return;
}
```

**After:**

```javascript
if (!body) {
    sendError(res, 400, 'invalid_request_error', 'Request body is required');
    logger.log({ req, body, statusCode: 400, startTime });
    return;
}

const validation = validateMessagesRequest(body);
if (!validation.valid) {
    sendError(res, 400, 'invalid_request_error', validation.message);
    logger.log({ req, body, statusCode: 400, startTime });
    return;
}
```

The `!body` check stays because `body` is `null` when the request has no body or unparseable JSON — this is a transport-level check, not a schema-level check. The Ajv validator expects a non-null object input.

#### 4.2.4 Error Message Formatting

Ajv error objects contain structured information (`instancePath`, `keyword`, `message`, `params`). The error formatter converts these into a single human-readable message string that fits the Anthropic error response format.

**Formatting strategy:**

```javascript
/**
 * Format Ajv validation errors into a human-readable message.
 *
 * @param {import('ajv').ErrorObject[]} errors
 * @returns {string}
 */
function formatErrors(errors) {
    // Special-case: stream must be true (Odin-specific)
    const streamError = errors.find(
        (e) => e.instancePath === '/stream' || 
               (e.keyword === 'required' && e.params.missingProperty === 'stream')
    );
    if (streamError) {
        return 'Only streaming mode is supported. Set "stream": true in your request.';
    }

    // General case: format all errors
    const messages = errors.map((err) => {
        const path = err.instancePath
            ? `"${err.instancePath.slice(1).replace(/\//g, '.')}"`
            : 'request body';
        return `${path} ${err.message}`;
    });

    // Deduplicate and join
    const unique = [...new Set(messages)];
    return unique.length === 1 ? unique[0] : unique.join('; ');
}
```

**Example error messages produced:**

| Input Problem | Ajv Error | Formatted Message |
|--------------|-----------|-------------------|
| `stream` missing | `required`, `missingProperty: 'stream'` | `Only streaming mode is supported. Set "stream": true in your request.` |
| `stream: false` | `const`, `allowedValue: true` | `Only streaming mode is supported. Set "stream": true in your request.` |
| `model` missing | `required`, `missingProperty: 'model'` | `request body must have required property 'model'` |
| `model: 123` | `type`, `type: 'string'` | `"model" must be string` |
| `model: ""` | `minLength`, `limit: 1` | `"model" must NOT have fewer than 1 characters` |
| `max_tokens: -1` | `minimum`, `limit: 1` | `"max_tokens" must be >= 1` |
| `max_tokens: "ten"` | `type`, `type: 'integer'` | `"max_tokens" must be integer` |
| `messages: "hello"` | `type`, `type: 'array'` | `"messages" must be array` |
| `messages: []` | `minItems`, `limit: 1` | `"messages" must NOT have fewer than 1 items` |
| `messages[0].role: "system"` | `enum`, `allowedValues: [...]` | `"messages.0.role" must be equal to one of the allowed values` |
| `temperature: 2.0` | `maximum`, `limit: 1` | `"temperature" must be <= 1` |

The `stream` error is special-cased because it is an Odin-specific constraint that benefits from a prescriptive hint ("Set `"stream": true`"). All other errors use Ajv's standard messages, which are already clear and path-specific.

### 4.3 Design Rationale

**Why Ajv over alternatives?**

| Criterion | Ajv | Zod | Hand-coded `if` chains |
|-----------|-----|-----|----------------------|
| **Standard** | JSON Schema (IETF standard) | Proprietary DSL | None |
| **Performance** | Compiles to optimized JS; ~0.1ms/validate | Runtime interpretation; ~1–5ms/validate | Fastest (native `if`), but incomplete |
| **Error quality** | Path-specific, structured errors | Path-specific, structured errors | Manual messages (inconsistent) |
| **Schema reuse** | Schema usable in OpenAPI docs, tests, other tools | TypeScript-only | Not reusable |
| **Runtime size** | ~135KB (minified) | ~55KB (minified) | 0KB |
| **Type generation** | Via `json-schema-to-ts` if needed | Built-in TypeScript inference | N/A |

**Decision:** Ajv. The JSON Schema standard aligns with the Anthropic API specification (which is defined in JSON Schema terms), the schema is reusable for documentation and testing, and Ajv's compile-once architecture means the runtime cost is negligible. Zod was considered but rejected because its proprietary DSL doesn't align with the Anthropic spec format, and it requires a separate translation step if we ever want to export the schema for external use.

**Why a first runtime dependency is acceptable:**

Odin's zero-dependency philosophy has served the project well for simplicity, but it has a cost: validation logic must be reimplemented by hand, which is error-prone and incomplete (as demonstrated in §2.1). Ajv is a mature, well-maintained library (13M+ weekly npm downloads, used by ESLint, Webpack, and numerous production systems). The trade-off — one 135KB dependency vs. hundreds of lines of hand-coded validation that will grow with every new field — favors Ajv.

To minimize the dependency footprint:
- Only `ajv` itself is added (no plugins like `ajv-formats` or `ajv-errors`).
- The schema uses only JSON Schema draft-07 features that Ajv supports natively.

**Why `additionalProperties` is omitted (not set to `false`):**

JSON Schema allows additional properties by default when `additionalProperties` is not specified. The schema deliberately omits this keyword at every level — no `additionalProperties: true` (redundant noise) and no `additionalProperties: false` (would break forward compatibility). Odin is a proxy: clients (e.g., Claude Code) may send fields that Odin does not process (`metadata`, `tool_choice`, `cache_control` on content blocks, future Anthropic API additions). The schema validates only the fields Odin cares about and silently ignores the rest — consistent with the existing behavior in `anthropicToGoogle()`, which destructures only known fields.

This principle applies uniformly: `MessageParam` objects allow fields beyond `role` and `content`; content block objects allow fields beyond `type`; tool objects allow fields beyond `name` (the Anthropic `ToolUnion` is a discriminated union — server tools like `ToolBash20250124` and `ToolTextEditor20250429` have `type` but no `input_schema`, while custom `Tool` objects carry both). No level of the schema restricts unknown properties.

**Why `stream` uses `const: true` instead of a separate check:**

Including the `stream` constraint in the schema keeps all validation in one place. The error formatter special-cases the `stream` error to provide the same prescriptive message as the current hand-coded check. This preserves the user experience while centralizing validation logic.

**Why `allErrors: true`:**

In interactive development (e.g., a developer testing a new integration), a single request attempt should reveal all validation problems — not force the developer into a fix-one-retry-fix-another loop. The marginal cost of collecting all errors vs. short-circuiting on the first is negligible for schemas of this size.

## 5. Implementation Plan

### Phase 1: Add Ajv Dependency — 5 minutes

- [ ] Run `npm install ajv` in the `odin/` directory.
- [ ] Verify `package.json` lists `ajv` under `dependencies` (not `devDependencies`).
- [ ] Verify `npm ls ajv` shows the installed version.

**Done when:** `node -e "import('ajv')"` (run from `odin/`) succeeds without errors.

### Phase 2: Create Validator Module — 30 minutes

- [ ] Create `src/validator.js` with the JSON Schema definition from §4.2.1.
- [ ] Implement the Ajv compilation and `validateMessagesRequest()` function from §4.2.2.
- [ ] Implement the `formatErrors()` function from §4.2.4.
- [ ] Export `validateMessagesRequest` as the public API.
- [ ] Verify the module loads without errors: `node -e "import('./src/validator.js')"`.

**Done when:** The module exports a working `validateMessagesRequest` function that accepts an object and returns `{ valid: true }` or `{ valid: false, message: string }`.

### Phase 3: Integrate into Server — 15 minutes

- [ ] Import `validateMessagesRequest` in `src/server.js`.
- [ ] Replace the three ad-hoc checks (lines 117–144) with the single validation call from §4.2.3.
- [ ] Preserve the `!body` null check as the first validation gate.
- [ ] Run `npm run check` to verify ESLint and Prettier compliance.

**Done when:** `server.js` uses `validateMessagesRequest()` for all body validation, and `npm run check` passes.

### Phase 4: End-to-End Verification — 30 minutes

- [ ] Start Odin and send a valid streaming request through the proxy. Verify it succeeds.
- [ ] Send a request with `stream: false` — verify the error message matches the current behavior.
- [ ] Send a request with missing `model` — verify a clear `invalid_request_error` is returned.
- [ ] Send a request with `max_tokens: "ten"` — verify type validation catches it.
- [ ] Send a request with extra unknown fields (e.g., `metadata: {}`) — verify it is accepted (not rejected).

**Done when:** All five scenarios produce expected behavior, and existing Claude Code workflows are unaffected.

## 6. Testing Strategy

The testing approach combines static validation (schema correctness) with behavioral testing (error message quality and pass-through compatibility).

- **Module-level testing:** Import `validateMessagesRequest` directly and assert results for a matrix of valid and invalid inputs. This can be done with a simple test script or a future test framework.
- **Integration testing:** Start Odin and send HTTP requests with `curl` or a test client, verifying both successful pass-through and error responses.
- **Regression testing:** Ensure that requests currently accepted by Odin (from Claude Code, etc.) continue to be accepted after the change.

**Key Scenarios:**

| # | Scenario | Input | Expected Behavior |
|---|----------|-------|-------------------|
| 1 | Valid minimal request | `{ model: "claude-3-5-sonnet", messages: [{ role: "user", content: "hi" }], max_tokens: 1024, stream: true }` | `{ valid: true }` |
| 2 | Valid request with tools | Above + `tools: [{ name: "get_weather", description: "...", input_schema: { type: "object" } }]` | `{ valid: true }` |
| 3 | Valid request with thinking | Above + `thinking: { type: "enabled", budget_tokens: 10000 }` | `{ valid: true }` |
| 4 | Valid request with unknown fields | Above + `metadata: { user_id: "u123" }, tool_choice: { type: "auto" }` | `{ valid: true }` (unknown fields ignored) |
| 5 | Missing body | `null` | Rejected before schema validation: "Request body is required" |
| 6 | Empty object | `{}` | `{ valid: false }` — missing `model`, `messages`, `max_tokens`, `stream` |
| 7 | Missing `stream` | Valid except `stream` absent | Error: "Only streaming mode is supported. Set \"stream\": true in your request." |
| 8 | `stream: false` | Valid except `stream: false` | Same error as #7 |
| 9 | `model: ""` | Empty string model | Error mentioning `model` minLength |
| 10 | `model: 123` | Numeric model | Error: `"model" must be string` |
| 11 | `max_tokens: -1` | Negative max_tokens | Error: `"max_tokens" must be >= 1` |
| 12 | `max_tokens: 3.5` | Float max_tokens | Error: `"max_tokens" must be integer` |
| 13 | `messages: "hello"` | String instead of array | Error: `"messages" must be array` |
| 14 | `messages: []` | Empty array | Error: `"messages" must NOT have fewer than 1 items` |
| 15 | `messages[0].role: "system"` | Invalid role | Error mentioning allowed values |
| 16 | `temperature: 2.0` | Out-of-range temperature | Error: `"temperature" must be <= 1` |
| 17 | `tools: "not-array"` | Wrong type for tools | Error: `"tools" must be array` |
| 18 | String content in messages | `messages: [{ role: "user", content: "hello" }]` | `{ valid: true }` (string content is valid) |
| 19 | Array content in messages | `messages: [{ role: "user", content: [{ type: "text", text: "hello" }] }]` | `{ valid: true }` (array content is valid) |
| 20 | Valid request with server tools | Above + `tools: [{ name: "bash", type: "bash_20250124" }]` | `{ valid: true }` (server tools have no `input_schema`) |
| 21 | Valid request with adaptive thinking | Above + `thinking: { type: "adaptive" }` | `{ valid: true }` |
| 22 | `thinking.budget_tokens` below minimum | `thinking: { type: "enabled", budget_tokens: 512 }` | Error: `"thinking.budget_tokens" must be >= 1024` |
| 23 | Enabled thinking without `budget_tokens` | `thinking: { type: "enabled" }` | Error: `"thinking" must have required property 'budget_tokens'` |
| 24 | `content` is a number (non-string, non-array) | `messages: [{ role: "user", content: 123 }]` | `{ valid: false }` — `oneOf` error indicating content must be a string or array |
| 25 | `system` is a number (non-string, non-array) | `system: 42` | `{ valid: false }` — `oneOf` error indicating system must be a string or array |
| 26 | Empty object (multiple missing required fields) | `{}` | `{ valid: false }` — errors for all four missing required properties (`model`, `messages`, `max_tokens`, `stream`) |

## 7. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Schema rejects valid requests from Claude Code or other existing clients | Low | High | Phase 4 tests against real Claude Code traffic. The schema is permissive (`additionalProperties` allowed). Rollback is a one-line import removal in `server.js`. |
| Ajv adds runtime startup latency | Low | Low | Schema compilation runs once at module load (~5ms for a schema this size). This is amortized over the server's lifetime and is not per-request. |
| Ajv dependency introduces supply chain risk | Low | Medium | Ajv has 13M+ weekly npm downloads, is maintained by a dedicated team, and is a transitive dependency of ESLint (already in devDependencies). Pin the version in `package-lock.json`. |
| Error messages differ from current behavior, breaking client-side error handling | Medium | Low | The `stream` error message is preserved verbatim via special-casing. Other errors are new (previously they were 500s or upstream errors), so no existing client parses them. The `error.type` remains `invalid_request_error`. |
| Schema becomes stale as Anthropic API evolves | Medium | Low | The schema is permissive — new fields are accepted without validation. Staleness only means we miss validation for new fields (which is the current state for all fields). Update the schema as needed when new features are implemented in the converter. |

## 8. Future Work

- **Deep content block validation** — Add discriminated union (`oneOf` + `const` discriminator) schemas for content block types (`text`, `image`, `tool_use`, `tool_result`, `thinking`, `redacted_thinking`). Deferred because the converter already handles unknown block types gracefully (they fall through the `switch` statement), and the deep schema would add significant complexity.
- **Test framework introduction** — Add Vitest or Node.js built-in test runner to automate the validation test matrix from §6. Currently deferred because the project has no test infrastructure.
- **Schema-driven documentation** — Generate API documentation from the JSON Schema definition, ensuring docs and validation are always in sync.
- **`ajv-formats` integration** — Add the `ajv-formats` plugin to validate string formats (e.g., `uri` for URL image sources) if deep content block validation is implemented.
- **`ajv-errors` integration** — Add the `ajv-errors` plugin for custom error messages defined inline in the schema, replacing the `formatErrors()` special-casing logic.

## 9. References

- [Anthropic Messages API — Create a Message](https://docs.anthropic.com/en/api/messages/create) — Official API reference defining the request body schema.
- [Ajv JSON Schema Validator](https://ajv.js.org/) — Library documentation, configuration options, and API reference.
- [JSON Schema Specification (draft-07)](https://json-schema.org/specification-links#draft-7) — The JSON Schema draft used by the schema in this RFC.
- `odin/src/server.js:114–144` — Current ad-hoc validation logic (modification target).
- `odin/src/converter.js:816–828` — `anthropicToGoogle()` field destructuring (defines which fields the schema must cover).

---

## Changelog

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.3 | 2026-02-13 | Chason Tang | Architectural review fixes: `thinking` `if` clause add explicit `required: ['type']` to prevent vacuous truth; `messages` add `maxItems: 100000` per Anthropic spec limit; add test scenarios #24–#26 (`oneOf` edge cases, empty object multi-error); update coverage map |
| 1.2 | 2026-02-13 | Chason Tang | Schema review against Anthropic API spec: `tools` items require `name` only (server tools lack `input_schema`); remove `tools` `minItems: 1`; `thinking.type` add `'adaptive'` enum value; `thinking.budget_tokens` minimum 1 → 1024; add `if/then` to require `budget_tokens` when `thinking.type` is `"enabled"`; update coverage map and test scenarios |
| 1.1 | 2026-02-13 | Chason Tang | Schema review: remove redundant `additionalProperties: true` (4 instances); system block enforce `const: 'text'` and require `text` field with `minItems: 1`; `thinking` require `type`; `tools` require `input_schema` with `minItems: 1`; add `messages[].content[]` row to coverage map; rewrite `additionalProperties` rationale |
| 1.0 | 2026-02-13 | Chason Tang | Initial version |
