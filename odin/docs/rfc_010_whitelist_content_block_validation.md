# RFC-010: Whitelist-Based Content Block Validation

**Version:** 2.0  
**Author:** Chason Tang  
**Date:** 2026-02-26  
**Status:** Implemented

---

## 1. Summary

This RFC replaces the blacklist-based content block validation (`UNSUPPORTED_CONTENT_TYPES` + imperative `validateContentBlocks()`) with a whitelist enforced directly in the Ajv JSON Schema. The four supported content block types — `text`, `thinking`, `tool_use`, `tool_result` — are expressed as a discriminated `oneOf` on the `type` property within the existing `messagesRequestSchema` in `validator.js`. This consolidates all request validation into a single Ajv pass, removes the dedicated `validateContentBlocks()` function and its server-side call, and closes the `image` validate-then-lose gap by making `image` (and any future unknown type) fail at schema validation.

## 2. Motivation

### 2.1 Blacklist Requires Maintenance to Stay Secure

The current `validateContentBlocks()` (introduced in RFC-004) uses a blacklist — the `UNSUPPORTED_CONTENT_TYPES` Set containing five explicitly rejected types:

```javascript
const UNSUPPORTED_CONTENT_TYPES = new Set([
    'document',
    'search_result',
    'redacted_thinking',
    'server_tool_use',
    'web_search_tool_result',
]);
```

Any content block type **not** in this set passes validation. This creates a security-by-enumeration problem: when Anthropic adds a new content block type (e.g., `audio`, `video`, `file`, `citation`), Odin silently accepts it, `convertContentToParts()` hits the `default` case, and the content is dropped with only a stderr warning. The exact silent-data-loss scenario that RFC-004 was designed to prevent resurfaces every time Anthropic extends its API.

The blacklist approach requires Odin maintainers to **proactively track** every new Anthropic content block type and add it to the rejection list. Missing one — which is likely given that Anthropic does not notify downstream proxies of API changes — causes silent data loss until someone notices degraded model responses and traces the issue back to the proxy.

A whitelist inverts the maintenance burden: new types are rejected by default. Maintenance is only needed when Odin **adds** converter support for a new type — a conscious decision accompanied by a new `case` in `convertContentToParts()` and a corresponding schema branch in `validator.js`.

### 2.2 The `image` Type Validate-Then-Lose Gap

The current validation treats `image` as a supported type. The Category 2 check in `validateContentBlocks()` only rejects image blocks with non-base64 sources, implying that base64 images are valid:

```javascript
// Category 2: Image blocks with non-base64 source (whitelist approach)
if (block.type === 'image' && block.source?.type !== 'base64') {
    return {
        valid: false,
        message: `Unsupported image source type ...`,
    };
}
```

However, `convertContentToParts()` has **no `case 'image'` handler**:

```javascript
switch (block.type) {
    case 'text':      // ✓ handled
    case 'tool_use':  // ✓ handled
    case 'tool_result': // ✓ handled
    case 'thinking':  // ✓ handled
    default:          // image lands here → logged + skipped
        console.error(`[Odin] Warning: unhandled content block type "${block.type}" — skipped`);
        break;
}
```

A base64 image block passes validation, enters the converter's switch, falls through to `default`, and is silently dropped. This is the exact "validate-then-lose" gap that RFC-004 identified as the worst failure mode for a proxy — the request appears to succeed (HTTP 200), but the model never sees the image, leading to confused or hallucinated responses about visual content.

The error message compounds the confusion by listing `image (base64)` as a supported type:

```
Supported types: text, image (base64), tool_use, tool_result, thinking.
```

This tells the developer that base64 images are supported, when in fact they are silently dropped.

### 2.3 Two-Layer Validation Creates Maintenance Surface

Today, content block validation lives in two places:

1. **Structural shape** — Ajv schema in `validator.js` (validates that `content` is `string | array-of-objects-with-type`)
2. **Semantic types** — imperative `validateContentBlocks()` in `converter.js` (validates block type values and nested content)

This split means changes to supported types require edits in both files. The schema doesn't know what types the converter supports, and the converter's validation duplicates structural checks already done by Ajv. Consolidating both layers into the schema eliminates this duplication and makes the JSON Schema the single source of truth for what constitutes a valid request.

## 3. Goals and Non-Goals

**Goals:**

- Enforce a content block type whitelist (`text`, `thinking`, `tool_use`, `tool_result`) via the existing Ajv JSON Schema using discriminated `oneOf`, making the schema the single source of truth for request validity.
- Validate `tool_result` inner content (only `text` blocks allowed) at the schema level, replacing the imperative Category 3 check.
- Remove the `validateContentBlocks()` function from `converter.js` and its call site in `server.js`, reducing the validation pipeline from three steps to two.
- Remove `image` from accepted types, aligning validation with the actual capabilities of `convertContentToParts()`.

**Non-Goals:**

- Adding `image` conversion support to `convertContentToParts()` — this is a separate feature requiring Google `inlineData`/`fileData` format integration, MIME type handling, and size limit considerations. Deferred to a dedicated RFC.
- Modifying `convertContentToParts()` itself — the converter's `default` case warning remains as defense-in-depth; the schema prevents unsupported types from reaching it in normal operation.
- Achieving identical error message text — the schema-based approach produces structurally equivalent but textually different error messages. The error response envelope (`type: "error"`, `error.type: "invalid_request_error"`) is preserved.

## 4. Design

### 4.1 Overview

The design consolidates all request validation into the Ajv schema pass, eliminating the separate content block validation step.

```
                    BEFORE (3-step pipeline)

            ┌──────────────────┐
            │  1. Ajv schema   │───── structural validation (validator.js)
            │     validation   │      content type: any string
            └────────┬─────────┘
                     │
                     ▼
            ┌──────────────────┐
            │  2. Content block│───── semantic validation (converter.js)
            │     validation   │      blacklist + image check + tool_result inner
            └────────┬─────────┘
                     │
                     ▼
            ┌──────────────────┐
            │  3. anthropicTo  │───── conversion
            │     Google()     │
            └──────────────────┘

                    AFTER (2-step pipeline)

            ┌──────────────────┐
            │  1. Ajv schema   │───── structural + semantic validation (validator.js)
            │     validation   │      content type: discriminated oneOf whitelist
            └────────┬─────────┘      tool_result inner: text-only enforced
                     │
                     ▼
            ┌──────────────────┐
            │  2. anthropicTo  │───── conversion
            │     Google()     │
            └──────────────────┘
```

### 4.2 Detailed Design

#### 4.2.1 Ajv Configuration Change

**File:** `src/validator.js`

Enable the `discriminator` keyword (supported in Ajv ≥ 8.12.0; project uses `^8.18.0`):

```javascript
const ajv = new Ajv({ allErrors: true, discriminator: true });
```

The `discriminator` keyword (from OpenAPI 3.1) tells Ajv to use the `type` property to select which `oneOf` branch to validate against, rather than trying all branches. This has two critical benefits:

1. **Clean errors** — When `type` is unknown (e.g., `"image"`), Ajv reports a single discriminator error instead of N branch failures. When `type` is known but fields are missing, Ajv reports only the matched branch's errors.
2. **Performance** — Ajv validates against one branch instead of all four, reducing per-item validation cost.

#### 4.2.2 Schema Changes

**File:** `src/validator.js` — replaces the `content` array `items` definition

The current loose schema:

```javascript
items: {
    type: 'object',
    required: ['type'],
    properties: {
        type: { type: 'string' },
    },
},
```

Is replaced with a discriminated `oneOf` that encodes the four supported types and their required fields:

```javascript
items: {
    type: 'object',
    required: ['type'],
    discriminator: { propertyName: 'type' },
    oneOf: [
        {
            properties: {
                type: { const: 'text' },
                text: { type: 'string' },
            },
            required: ['type', 'text'],
        },
        {
            properties: {
                type: { const: 'thinking' },
                thinking: { type: 'string' },
                signature: { type: 'string' },
            },
            required: ['type', 'thinking', 'signature'],
        },
        {
            properties: {
                type: { const: 'tool_use' },
                id: { type: 'string' },
                name: { type: 'string' },
                input: { type: 'object' },
            },
            required: ['type', 'id', 'name', 'input'],
        },
        {
            properties: {
                type: { const: 'tool_result' },
                tool_use_id: { type: 'string' },
                content: {
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
            },
            required: ['type', 'tool_use_id'],
        },
    ],
},
```

**Schema–converter correspondence:**

| Schema Branch | `const` value | `convertContentToParts()` case | Required fields validated |
|---|---|---|---|
| text | `"text"` | `case 'text':` → `{ text }` | `text` |
| thinking | `"thinking"` | `case 'thinking':` → `{ text, thought, thoughtSignature }` | `thinking`, `signature` |
| tool_use | `"tool_use"` | `case 'tool_use':` → `{ functionCall }` | `id`, `name`, `input` |
| tool_result | `"tool_result"` | `case 'tool_result':` → `{ functionResponse }` | `tool_use_id` |

**`tool_result.content` sub-schema:** The `content` field is optional (not in `required`) but when present must be either a string or an array of `{type: "text", text: string}` objects. This replaces the imperative Category 3 check — `tool_result` blocks with non-text inner content (e.g., `[{type: "image"}]`) are now rejected at the schema level via the `const: 'text'` constraint.

#### 4.2.3 Error Formatting

**File:** `src/validator.js` — new special case in `formatErrors()`

The `discriminator` keyword produces a specific error when the `type` value doesn't match any branch:

```javascript
{
    keyword: 'discriminator',
    instancePath: '/messages/0/content/1',
    message: 'value of tag "type" must be in oneOf',
    params: { tag: 'type', tagValue: 'image' }
}
```

A new special case in `formatErrors()` detects this and produces a clear error:

```javascript
const contentBlockError = errors.find(
    (e) => e.keyword === 'discriminator' && e.params?.tag === 'type'
        && /^\/messages\/\d+\/content\/\d+$/.test(e.instancePath),
);
if (contentBlockError) {
    const path = contentBlockError.instancePath.slice(1).replace(/\//g, '.');
    return (
        `Unsupported content block type "${contentBlockError.params.tagValue}" at ${path}. ` +
        `Supported types: text, thinking, tool_use, tool_result.`
    );
}
```

This produces error messages equivalent to the old imperative approach:

```
Unsupported content block type "image" at messages.0.content.1. Supported types: text, thinking, tool_use, tool_result.
```

For known-type errors (e.g., `{type: "text"}` missing the `text` field), Ajv validates only the matched branch and reports a standard `required` error, which the existing general-case formatting handles correctly:

```
"messages.0.content.0" must have required property 'text'
```

For `tool_result` inner content errors (e.g., non-text items in the `content` array), the nested `const: 'text'` constraint produces a `const` error. A second special case detects this:

```javascript
const toolResultContentError = errors.find(
    (e) => e.keyword === 'const'
        && /\/content\/\d+\/content\/\d+\/type$/.test(e.instancePath),
);
if (toolResultContentError) {
    const path = toolResultContentError.instancePath.slice(1).replace(/\//g, '.');
    return `Only "text" content is supported inside tool_result blocks. Error at ${path}.`;
}
```

#### 4.2.4 Code Removal

**File:** `src/converter.js`

Remove:
- `UNSUPPORTED_CONTENT_TYPES` constant (lines 9–22)
- `validateContentBlocks()` function (lines 352–423)
- `validateContentBlocks` from the `export` list

**File:** `src/server.js`

Remove:
- `validateContentBlocks` from the import statement (line 3)
- Content block validation call and its early-return block (lines 134–142)

### 4.3 Design Rationale

**Why consolidate into JSON Schema instead of a separate imperative whitelist:**

The v1.0 design (imperative `SUPPORTED_CONTENT_TYPES` Set in `converter.js`) was chosen primarily for co-location with `convertContentToParts()` and control over error message formatting. However, moving the whitelist into the schema provides stronger guarantees:

1. **Single source of truth** — The schema IS the validation. There is no possibility of the schema and the imperative check diverging (e.g., schema allows a type that the imperative check rejects, or vice versa).
2. **Deeper validation for free** — The discriminated `oneOf` validates not just the `type` value but also the required fields for each type (`text.text`, `thinking.thinking`, `thinking.signature`, `tool_use.id`, `tool_use.name`, `tool_use.input`, `tool_result.tool_use_id`). The imperative approach only checked the `type` value. This catches malformed content blocks earlier — before they reach the converter.
3. **`tool_result` inner content enforcement** — The nested `tool_result.content` schema (string or text-only array) replaces the imperative Category 3 check, consolidating all content-related validation in one place.
4. **Elimination of a pipeline step** — The three-step pipeline (Ajv → `validateContentBlocks()` → conversion) becomes two steps (Ajv → conversion). Fewer steps means fewer opportunities for checks to fall out of sync.

**Why `discriminator` instead of plain `oneOf`:**

Without `discriminator`, Ajv's `allErrors: true` mode evaluates all four `oneOf` branches for every content block, producing up to 4× the error objects. When the `type` doesn't match any branch, all four branches fail, and the errors include `const` mismatches plus `required` failures from every branch. The `formatErrors()` function would need complex heuristics to determine whether the failure is "unknown type" or "known type with missing fields."

With `discriminator`, Ajv reads the `type` value first and validates only the matching branch. Unknown types produce a single `discriminator` error. Known types with missing fields produce only the matched branch's `required` errors. This makes `formatErrors()` special-case logic straightforward and error messages clean.

**Why the co-location concern is mitigated:**

The v1.0 design valued co-location of the whitelist with `convertContentToParts()` in `converter.js`. In the schema approach, this co-location is replaced by a stronger contract: the schema branches correspond 1:1 to the converter's `case` statements, documented in the correspondence table (§4.2.2). When a developer adds a new `case` to the converter, they must add a corresponding `oneOf` branch to the schema — and the request will fail at validation until they do, making the omission immediately obvious during development.

**Why `allErrors: true` is acceptable:**

A concern in the v1.0 analysis was that `allErrors: true` conflicts with first-error semantics for content blocks. With `discriminator`, this concern is eliminated: unknown types produce exactly one error per block (the discriminator error), and `formatErrors()` returns at the first content block discriminator error found. The behavior is effectively first-error for the caller.

## 5. Interface Changes

**Removed export:** `validateContentBlocks` is removed from `converter.js` exports. Any external code importing this function will break at import time.

**Error message changes:**

Unsupported content block type:

**Before:**
```
Unsupported content block type "image" at messages[0].content[1]. Odin does not support "image" blocks. Supported types: text, image (base64), tool_use, tool_result, thinking.
```

**After:**
```
Unsupported content block type "image" at messages.0.content.1. Supported types: text, thinking, tool_use, tool_result.
```

Path format changes from bracket notation (`messages[0].content[1]`) to dot notation (`messages.0.content.1`), consistent with Ajv's `instancePath` formatting used by all other validation errors.

Tool result inner content:

**Before:**
```
Unsupported content type "image" in tool_result at messages[0].content[0].content[1]. Only "text" content is supported inside tool_result blocks.
```

**After:**
```
Only "text" content is supported inside tool_result blocks. Error at messages.0.content.0.content.1.type.
```

**New validation errors (not previously caught):**

Content blocks with a supported `type` but missing required fields now produce errors:

```
"messages.0.content.0" must have required property 'text'
```

Previously, a `{type: "text"}` block (missing `text` field) passed both Ajv and `validateContentBlocks()`, then produced `undefined` text in the converter. The schema now catches this at validation time.

## 6. Backward Compatibility & Migration

- **Breaking changes:**
  - Requests containing `image` content blocks that previously returned HTTP 200 (with the image silently dropped by the converter) will now return HTTP 400.
  - Requests containing content blocks with supported types but missing required fields (e.g., `{type: "text"}` without `text`, or `{type: "tool_use"}` without `id`/`name`/`input`) that previously passed validation will now return HTTP 400. This is a correctness improvement — such blocks would have produced malformed converter output.
  - `validateContentBlocks` is no longer exported from `converter.js`.
- **Migration path:** Claude Code does not send `image` blocks or malformed content blocks through Odin in normal workflows. No user action is required. The stricter validation catches bugs earlier; any newly rejected request was already producing incorrect behavior.

## 7. Implementation Plan

### Phase 1: Schema Changes — 15 minutes

- [x] Enable `discriminator: true` in the Ajv constructor options in `validator.js`.
- [x] Replace the content block `items` schema with the discriminated `oneOf` (four branches: `text`, `thinking`, `tool_use`, `tool_result`).
- [x] Add `formatErrors()` special cases for discriminator errors (unsupported content type) and `tool_result` inner content errors.
- [x] Run `npm run check` to verify ESLint and Prettier compliance.

**Done when:** Schema rejects unsupported content types and malformed blocks; `formatErrors()` produces clear error messages for both cases.

### Phase 2: Code Removal — 5 minutes

- [x] Remove `UNSUPPORTED_CONTENT_TYPES` constant from `converter.js`.
- [x] Remove `validateContentBlocks()` function from `converter.js`.
- [x] Remove `validateContentBlocks` from the export statement in `converter.js`.
- [x] Remove `validateContentBlocks` from the import statement in `server.js`.
- [x] Remove the content block validation call and its early-return block in `server.js`.
- [x] Run `npm run check` to verify ESLint and Prettier compliance.

**Done when:** No references to `validateContentBlocks` remain in the codebase; `npm run check` passes.

### Phase 3: End-to-End Verification — 15 minutes

- [x] Start Odin and send a valid request with only supported content types (`text`, `tool_use`, `tool_result`, `thinking`). Verify it succeeds.
- [x] Send a request with an `image` content block. Verify HTTP 400 with error message listing supported types.
- [x] Send a request with a `document` content block. Verify HTTP 400.
- [x] Send a request with a hypothetical future type (e.g., `audio`). Verify HTTP 400.
- [x] Send a request with `tool_result` containing non-text inner content. Verify HTTP 400.
- [x] Send a request with a known type but missing required fields (e.g., `{type: "text"}`). Verify HTTP 400 with clear field-level error.
- [x] Run a normal Claude Code session through the proxy to verify no regression.

**Done when:** All rejection scenarios produce correct error responses, and normal Claude Code workflows are unaffected.

## 8. Testing Strategy

All validation is now in `validateMessagesRequest()`, which is testable with constructed request objects. The `discriminator` keyword behavior can be verified through Ajv's error output.

**Key Scenarios:**

| # | Scenario | Input | Expected Behavior |
|---|----------|-------|-------------------|
| 1 | Valid text block | `content: [{ type: "text", text: "hello" }]` | `{ valid: true }` |
| 2 | Valid string content | `content: "hello"` | `{ valid: true }` |
| 3 | Valid thinking block | `content: [{ type: "thinking", thinking: "...", signature: "..." }]` | `{ valid: true }` |
| 4 | Valid tool_use block | `content: [{ type: "tool_use", id: "t1", name: "fn", input: {} }]` | `{ valid: true }` |
| 5 | Valid tool_result (string content) | `content: [{ type: "tool_result", tool_use_id: "t1", content: "ok" }]` | `{ valid: true }` |
| 6 | Valid tool_result (text array) | `content: [{ type: "tool_result", tool_use_id: "t1", content: [{ type: "text", text: "ok" }] }]` | `{ valid: true }` |
| 7 | Valid tool_result (no content) | `content: [{ type: "tool_result", tool_use_id: "t1" }]` | `{ valid: true }` |
| 8 | **Image block** | `content: [{ type: "image", source: { type: "base64", ... } }]` | **`{ valid: false }`** — discriminator error, lists supported types |
| 9 | **Document block** | `content: [{ type: "document" }]` | `{ valid: false }` — discriminator error |
| 10 | **Unknown future type** | `content: [{ type: "audio" }]` | **`{ valid: false }`** — discriminator error |
| 11 | **Text missing `text`** | `content: [{ type: "text" }]` | **`{ valid: false }`** — `required` error for `text` |
| 12 | **Thinking missing `signature`** | `content: [{ type: "thinking", thinking: "..." }]` | **`{ valid: false }`** — `required` error for `signature` |
| 13 | **Tool_use missing `input`** | `content: [{ type: "tool_use", id: "t1", name: "fn" }]` | **`{ valid: false }`** — `required` error for `input` |
| 14 | **Tool_result missing `tool_use_id`** | `content: [{ type: "tool_result" }]` | **`{ valid: false }`** — `required` error for `tool_use_id` |
| 15 | Tool_result non-text inner | `content: [{ type: "tool_result", tool_use_id: "t1", content: [{ type: "image" }] }]` | `{ valid: false }` — inner content error |
| 16 | Tool_result mixed inner | `..., content: [{ type: "text", text: "ok" }, { type: "image" }]` | `{ valid: false }` — error at inner `content[1]` |
| 17 | Multiple unsupported blocks | `content: [{ type: "document" }, { type: "image" }]` | `{ valid: false }` — discriminator errors |
| 18 | Mixed valid and unsupported | `content: [{ type: "text", text: "hi" }, { type: "image" }]` | `{ valid: false }` — discriminator error for second block |
| 19 | Block missing `type` | `content: [{ text: "hello" }]` | `{ valid: false }` — `required` error for `type` |
| 20 | Empty content array | `content: []` | `{ valid: false }` — `minItems` error (already enforced) |

Scenarios **8–14** (bolded) represent new or strengthened validation compared to the current codebase. Scenarios 1–7 and 15–20 preserve existing behavior.

## 9. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Requests with `image` blocks that previously "worked" (silently degraded) now fail with 400 | Low | Low | Claude Code does not send `image` blocks through Odin in normal workflows. The previous "success" was illusory — images were silently dropped. |
| Content blocks with supported type but missing fields (e.g., `{type: "text"}`) that previously "worked" now fail with 400 | Low | Low | Such blocks produced `undefined` or empty values in the converter output. The earlier rejection is strictly better — it surfaces malformed requests before they waste an API call. |
| `discriminator` keyword produces unexpected error objects in edge cases | Low | Medium | The `discriminator` keyword is stable in Ajv ≥ 8.12.0 (project uses `^8.18.0`). The `formatErrors()` special cases fall back to the general-case formatting if the discriminator error is not detected, so unexpected error shapes degrade gracefully. |
| Schema complexity increases maintainability burden | Medium | Low | The discriminated `oneOf` is a well-understood JSON Schema pattern. The correspondence table (§4.2.2) documents the schema-to-converter mapping. Adding a new content type requires one new `oneOf` branch — more verbose than a Set entry, but also more thorough (validates fields, not just type). |
| `formatErrors()` special cases accumulate over time | Medium | Low | The function now has four special cases (stream, thinking, content type, tool_result inner). Each handles a specific Ajv error pattern for a specific schema construct. This is inherent to using `allErrors: true` with human-readable error formatting. The alternative (custom error messages via `ajv-errors`) would add a dependency. |

## 10. Future Work

- **Image conversion support** — Add a `case 'image'` handler to `convertContentToParts()` using Google's `inlineData` format, and add a corresponding `image` branch to the schema `oneOf`. Deferred because it requires MIME type mapping, size limit handling, and Google API `inlineData` format investigation.
- **`ajv-errors` for custom messages** — The `ajv-errors` package allows attaching custom error messages directly to schema nodes, potentially eliminating the need for `formatErrors()` special cases. Evaluate if the special-case count continues to grow.
- **Monitoring for rejected types** — Add structured logging that tracks which content block types are rejected by the discriminator, and how frequently. This data can prioritize which converter support to add next.

## 11. References

- RFC-003: JSON Schema Request Validation with Ajv — Established the Ajv-based validation pipeline and `formatErrors()` function being extended here.
- RFC-004: Defensive Content Block Validation in Converter — Introduced `validateContentBlocks()` and the blacklist mechanism being replaced. The motivation analysis (§2: silent data loss, §4.2: HTTP 400 vs log-only) remains valid and is inherited by this RFC.
- [Ajv `discriminator` keyword documentation](https://ajv.js.org/json-schema.html#discriminator) — Reference for the discriminator keyword behavior and Ajv version requirements.
- [Anthropic Messages API — Content Types](https://docs.anthropic.com/en/api/messages) — Defines all content block types including `ImageBlockParam`, `DocumentBlockParam`, etc.
- `odin/src/validator.js:14–151` — `messagesRequestSchema` definition (modification target).
- `odin/src/validator.js:167–206` — `formatErrors()` function (modification target).
- `odin/src/converter.js:434–493` — `convertContentToParts()` switch statement (the four `case` handlers that define what the schema whitelist must match).
- `odin/src/converter.js:16–22` — `UNSUPPORTED_CONTENT_TYPES` constant (to be removed).
- `odin/src/converter.js:368–423` — `validateContentBlocks()` function (to be removed).
- `odin/src/server.js:3` — import statement (to be modified).
- `odin/src/server.js:134–142` — validation call site (to be removed).

---

## Changelog

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 2.0 | 2026-02-26 | Chason Tang | Major design revision: moved content block whitelist from imperative code (`validateContentBlocks()` in converter.js) into Ajv JSON Schema (discriminated `oneOf` in validator.js). Added field-level validation for each content block type. Removed `validateContentBlocks()` and server-side call. Added `formatErrors()` special cases for discriminator and tool_result inner content errors. |
| 1.0 | 2026-02-26 | Chason Tang | Initial version (imperative whitelist approach) |
