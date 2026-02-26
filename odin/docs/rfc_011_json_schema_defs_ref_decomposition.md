# RFC-011: JSON Schema `$defs`/`$ref` Decomposition for Request Validator

**Version:** 1.0  
**Author:** Chason Tang  
**Date:** 2026-02-26  
**Status:** Proposed

---

## 1. Summary

The JSON Schema in `src/validator.js` (request validation, RFC-003) contains duplicated sub-schema fragments that violate DRY. The `TextContentBlock` schema (`{ type: "text", text: string }`) is defined identically three times; the `"string or array of text blocks"` union pattern appears twice. This RFC proposes using JSON Schema `$defs` / `$ref` to extract these two repeated fragments into named, reusable sub-schemas — eliminating duplication and ensuring future changes to the `TextBlockParam` shape require a single edit instead of three.

## 2. Motivation

The request validator defines the Anthropic Messages API request body schema (303 lines). Two structural patterns are duplicated:

**Pattern 1: `TextContentBlock` — 3 identical occurrences**

The schema `{ type: 'text', text: string }` with `required: ['type', 'text']` appears in:

1. **Message content block** (line 48–54) — as a `oneOf` branch in message content items.
2. **Tool result sub-content** (line 82–89) — as `items` in the `tool_result.content` array alternative.
3. **System prompt array items** (line 122–129) — as `items` in the system prompt array alternative.

These three schemas are semantically identical — they all validate an Anthropic `TextBlockParam`. Any future change (e.g., adding an optional `citations` field per the [Anthropic API spec](https://platform.claude.com/docs/en/api/messages/create)) must be replicated across all three sites.

Current code — occurrence 1 (message content block, line 48–54):

```javascript
{
    properties: {
        type: { const: 'text' },
        text: { type: 'string' },
    },
    required: ['type', 'text'],
},
```

Current code — occurrence 2 (tool_result sub-content, line 82–89):

```javascript
{
    type: 'object',
    required: ['type', 'text'],
    properties: {
        type: { const: 'text' },
        text: { type: 'string' },
    },
},
```

Current code — occurrence 3 (system prompt items, line 122–129):

```javascript
{
    type: 'object',
    required: ['type', 'text'],
    properties: {
        type: { const: 'text' },
        text: { type: 'string' },
    },
},
```

Occurrences 2 and 3 are byte-identical. Occurrence 1 differs only in the absence of `type: 'object'` (inherited from the parent `items` level).

**Pattern 2: `StringOrTextBlockArray` — 2 identical occurrences**

The pattern `oneOf: [{ type: 'string' }, { type: 'array', minItems: 1, items: TextContentBlock }]` appears in:

1. **`system` field** (line 117–131) — system prompt accepts a string or an array of text blocks.
2. **`tool_result.content` field** (line 76–91) — tool result content accepts the same union.

These are structurally identical schemas. When both embed `TextContentBlock` inline, the total duplicated surface is 2 × 12 lines = 24 lines of identical schema definition.

## 3. Goals and Non-Goals

**Goals:**

- Extract `TextContentBlock` into a single `$defs` entry, referenced 3 times via `$ref`.
- Extract `StringOrTextBlockArray` into a single `$defs` entry, referenced 2 times via `$ref`.
- Maintain identical validation semantics — the refactored schema must accept and reject exactly the same inputs as the current schema.

**Non-Goals:**

- **Extracting single-use sub-schemas for readability** (e.g., `ThinkingContentBlock`, `ToolUseContentBlock`, `ThinkingConfig`, `ToolDefinition`) — these appear only once. Readability-driven extraction is deferred to a future RFC.
- **Refactoring `response-validator.js`** — the response schema's `NonNegativeInteger` repetition (`{ type: 'integer', minimum: 0 }` × 3 in `usageMetadata`) is minor enough that inline definitions are sufficient.
- **Adding new content block types** (e.g., `redacted_thinking`, `image`) — this is a feature change, not a refactoring.
- **Modifying error formatting** — `formatErrors()` is unchanged. Ajv's `instancePath` (data path) is independent of `$ref` resolution.

## 4. Design

### 4.1 Overview

The request schema gains a top-level `$defs` object containing two named sub-schemas. Inline duplicates are replaced with `{ $ref: '#/$defs/Name' }` references. No new files are created; no exports change; no Ajv configuration changes.

**Decomposition map:**

```
validator.js (request schema)
┌──────────────────────────────────────────────────────────┐
│  $defs:                                                  │
│    TextContentBlock ──────────┬── message content oneOf  │
│                               ├── tool_result.content    │
│                               └── system array items     │
│    StringOrTextBlockArray ────┬── system                 │
│         └─ refs TextContentBlock                         │
│                               └── tool_result.content    │
└──────────────────────────────────────────────────────────┘
```

**Reference graph:**

```
StringOrTextBlockArray
  └─ items ──▶ $ref TextContentBlock

system ──────▶ $ref StringOrTextBlockArray
tool_result.content ──▶ $ref StringOrTextBlockArray
message content oneOf[0] ──▶ $ref TextContentBlock
```

### 4.2 Detailed Design

#### 4.2.1 `$defs` Definitions

**2 named definitions** are extracted:

| `$def` Name | Usage Count | Duplication Eliminated |
|-------------|-------------|----------------------|
| `TextContentBlock` | 3 | 3 identical inline schemas → 1 `$def` + 3 `$ref` |
| `StringOrTextBlockArray` | 2 | 2 identical inline `oneOf` unions → 1 `$def` + 2 `$ref` |

**`TextContentBlock` definition:**

```javascript
TextContentBlock: {
    type: 'object',
    required: ['type', 'text'],
    properties: {
        type: { const: 'text' },
        text: { type: 'string' },
    },
},
```

The `$def` includes `type: 'object'` to be self-contained — it must work correctly in both contexts: (a) as a `oneOf` branch inside message content items (where the parent `items` already specifies `type: 'object'`), and (b) as `items` in a text block array (where no parent provides `type: 'object'`). The redundant `type: 'object'` in context (a) is harmless.

**`StringOrTextBlockArray` definition:**

```javascript
StringOrTextBlockArray: {
    oneOf: [
        { type: 'string' },
        {
            type: 'array',
            minItems: 1,
            items: { $ref: '#/$defs/TextContentBlock' },
        },
    ],
},
```

This `$def` references `TextContentBlock`, creating a `StringOrTextBlockArray` → `TextContentBlock` reference chain. Ajv resolves `$ref` pointers recursively against the root schema.

#### 4.2.2 Full Refactored Schema

```javascript
const messagesRequestSchema = {
    type: 'object',
    required: ['model', 'messages', 'max_tokens', 'stream'],
    $defs: {
        TextContentBlock: {
            type: 'object',
            required: ['type', 'text'],
            properties: {
                type: { const: 'text' },
                text: { type: 'string' },
            },
        },
        StringOrTextBlockArray: {
            oneOf: [
                { type: 'string' },
                {
                    type: 'array',
                    minItems: 1,
                    items: { $ref: '#/$defs/TextContentBlock' },
                },
            ],
        },
    },
    properties: {
        model: {
            type: 'string',
            minLength: 1,
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
                                    discriminator: { propertyName: 'type' },
                                    oneOf: [
                                        { $ref: '#/$defs/TextContentBlock' },
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
                                                content: { $ref: '#/$defs/StringOrTextBlockArray' },
                                            },
                                            required: ['type', 'tool_use_id'],
                                        },
                                    ],
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

        system: { $ref: '#/$defs/StringOrTextBlockArray' },

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
                required: ['name', 'input_schema'],
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
                        required: ['type'],
                        properties: {
                            type: { const: 'object' },
                        },
                    },
                },
            },
        },

        thinking: {
            type: 'object',
            required: ['type'],
            discriminator: { propertyName: 'type' },
            oneOf: [
                {
                    properties: {
                        type: { const: 'enabled' },
                        budget_tokens: { type: 'integer', minimum: 1024 },
                    },
                    required: ['type', 'budget_tokens'],
                },
                {
                    properties: { type: { const: 'disabled' } },
                    required: ['type'],
                },
                {
                    properties: { type: { const: 'adaptive' } },
                    required: ['type'],
                },
            ],
        },
    },
};
```

**Sites changed vs. current schema:**

| Location | Before | After |
|----------|--------|-------|
| `messages[].content[].oneOf[0]` (text block) | 6-line inline object | `{ $ref: '#/$defs/TextContentBlock' }` |
| `tool_result.content` | 15-line inline `oneOf` with nested text block | `{ $ref: '#/$defs/StringOrTextBlockArray' }` |
| `system` | 15-line inline `oneOf` with nested text block | `{ $ref: '#/$defs/StringOrTextBlockArray' }` |

All other schema content (`thinking`, `tool_use`, `tool_result` structure, `ThinkingConfig`, `ToolDefinition`, `tools`) remains inline and unchanged.

#### 4.2.3 Ajv Compatibility

**1. `$defs` / `$ref` resolution**

Ajv v8 fully supports `$defs` (JSON Schema draft 2019-09+) and `$ref` with `#/$defs/Name` pointers. All references are local (within the same schema), avoiding any cross-schema resolution complexity.

**2. `$ref` within `discriminator` + `oneOf`**

The message content items use `discriminator: { propertyName: 'type' }` with `oneOf` branches. After refactoring, the first branch becomes a `$ref`:

```javascript
discriminator: { propertyName: 'type' },
oneOf: [
    { $ref: '#/$defs/TextContentBlock' },  // $ref branch
    { ... thinking (inline) ... },
    { ... tool_use (inline) ... },
    { ... tool_result (inline, but content uses $ref) ... },
],
```

Ajv's discriminator implementation explicitly supports `$ref` in `oneOf` subschemas — it resolves the reference to find the discriminator property's `const` value. The `TextContentBlock` `$def` includes `properties.type.const: 'text'`, which the discriminator requires. The remaining 3 branches stay inline and are unaffected.

**3. Chained `$ref` resolution**

The `tool_result` branch's `content` field references `StringOrTextBlockArray`, which in turn references `TextContentBlock`:

```
tool_result.content → $ref StringOrTextBlockArray → items.$ref TextContentBlock
```

Ajv resolves `$ref` pointers recursively against the root schema, handling this two-level chain correctly.

**Ajv instance configuration — no changes required:**

| Current Config | After Refactoring |
|---------------|-------------------|
| `new Ajv({ allErrors: true, discriminator: true })` | Unchanged |

### 4.3 Design Rationale

**Why `$defs` / `$ref` instead of JavaScript variables?**

An alternative is to extract sub-schemas as JavaScript constants and embed them by reference:

```javascript
const TEXT_CONTENT_BLOCK = { type: 'object', required: ['type', 'text'], ... };
const schema = {
    // ...
    oneOf: [TEXT_CONTENT_BLOCK, ...],
};
```

This eliminates source-level duplication but has a critical drawback: the schema passed to `ajv.compile()` contains the actual expanded objects (since JavaScript resolves variable references at object-creation time). The compiled schema has no knowledge of the logical groupings. With `$defs` / `$ref`, the schema itself carries the decomposition metadata — Ajv's compiled validator understands the reference structure, tools like schema visualizers can render it, and the `$defs` block serves as a self-documenting registry of sub-schemas.

Additionally, if a future RFC introduces external schema files (e.g., shared between Odin and a test harness), `$defs`/`$ref` is the standard JSON Schema mechanism and requires no adaptation.

**Why include `type: 'object'` in `TextContentBlock`?**

In the current schema, the `type: 'object'` constraint for message content items is specified at the `items` level, not inside each `oneOf` branch. The `$def` includes `type: 'object'` to be self-contained — without it, `TextContentBlock` would not validate correctly when used as `items` in the `StringOrTextBlockArray` array alternative (where no parent provides `type: 'object'`). The redundant `type: 'object'` in the message content `oneOf` context is harmless — JSON Schema allows redundant constraints, and Ajv's `discriminator` is unaffected.

**Why a mixed `$ref` + inline approach in the `oneOf` branches?**

Only `TextContentBlock` is extracted as a `$ref` branch; `thinking`, `tool_use`, and `tool_result` remain inline. This is intentional — the goal is strictly DRY, and these three block types appear exactly once. Extracting single-use schemas adds indirection without eliminating duplication. A future RFC may extract them for readability if the schema grows further.

**Why not inline `TextContentBlock` inside `StringOrTextBlockArray` and only extract `StringOrTextBlockArray`?**

`TextContentBlock` has 3 usage sites — 2 via `StringOrTextBlockArray` and 1 directly in the message content `oneOf`. If only `StringOrTextBlockArray` were extracted (with `TextContentBlock` inlined inside it), the message content `oneOf` usage would still need a separate copy. Two `$defs` are needed to fully deduplicate.

## 5. Implementation Plan

### Phase 1: Add `$defs` and Replace Inline Duplicates — 20 minutes

- [ ] Add `$defs` block to `messagesRequestSchema` in `src/validator.js` with `TextContentBlock` and `StringOrTextBlockArray` per §4.2.1.
- [ ] Replace the text block `oneOf` branch in message content items (line 48–54) with `{ $ref: '#/$defs/TextContentBlock' }`.
- [ ] Replace the `tool_result.content` inline `oneOf` (line 76–91) with `{ $ref: '#/$defs/StringOrTextBlockArray' }`.
- [ ] Replace the `system` inline `oneOf` (line 117–131) with `{ $ref: '#/$defs/StringOrTextBlockArray' }`.
- [ ] Verify Ajv compilation succeeds: `node -e "import('./src/validator.js')"`.
- [ ] Run `npm run check` to verify ESLint and Prettier compliance.

**Done when:** `validator.js` compiles without errors, `npm run check` passes, and all 3 inline duplicates are replaced with `$ref` pointers.

### Phase 2: Equivalence Verification — 20 minutes

- [ ] Construct a test input matrix covering all content block types, `system` variants, `tool_result.content` variants, and error cases (§6 scenarios).
- [ ] Run every test input through both the old and new schemas — assert identical `valid` / error results.
- [ ] Run an end-to-end request through Odin to verify no behavioral regression.

**Done when:** Every test input produces identical validation results before and after refactoring. End-to-end request succeeds.

## 6. Testing Strategy

The testing approach is centered on **equivalence verification**: the refactored schema must accept and reject exactly the same inputs as the original. No new validation behavior is introduced.

- **Schema equivalence testing:** Construct a matrix of valid and invalid inputs. Run each input through both the pre-refactoring and post-refactoring compiled validators. Assert identical `valid` boolean and equivalent error paths.
- **`$ref` resolution testing:** Verify that the chained `$ref` path (`tool_result.content` → `StringOrTextBlockArray` → `TextContentBlock`) resolves correctly by testing `tool_result` with array content.
- **Discriminator + `$ref` testing:** Verify that the `discriminator` keyword correctly selects the `TextContentBlock` branch via `$ref` (e.g., input with `type: "text"` matches `TextContentBlock`, input with `type: "thinking"` matches the inline thinking branch).

**Key Scenarios:**

| # | Scenario | Input | Expected |
|---|----------|-------|----------|
| 1 | Valid text block | `{ type: "text", text: "hello" }` | `valid: true` |
| 2 | Valid thinking block | `{ type: "thinking", thinking: "...", signature: "..." }` | `valid: true` |
| 3 | Valid tool_use block | `{ type: "tool_use", id: "t1", name: "fn", input: {} }` | `valid: true` |
| 4 | Valid tool_result with string content | `{ type: "tool_result", tool_use_id: "t1", content: "ok" }` | `valid: true` |
| 5 | Valid tool_result with array content ($ref chain) | `{ type: "tool_result", tool_use_id: "t1", content: [{ type: "text", text: "ok" }] }` | `valid: true` |
| 6 | Valid system as string ($ref) | `system: "You are helpful"` | `valid: true` |
| 7 | Valid system as text block array ($ref chain) | `system: [{ type: "text", text: "You are helpful" }]` | `valid: true` |
| 8 | Unknown content block type (discriminator + $ref) | `{ type: "image", source: {...} }` | `valid: false` |
| 9 | Missing text in text block ($ref) | `{ type: "text" }` | `valid: false` |
| 10 | Text block with wrong text type ($ref) | `{ type: "text", text: 42 }` | `valid: false` |
| 11 | Tool result with non-text array content ($ref chain) | `{ type: "tool_result", tool_use_id: "t1", content: [{ type: "image" }] }` | `valid: false` |
| 12 | System with non-text array content ($ref chain) | `system: [{ type: "image" }]` | `valid: false` |

## 7. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Ajv `discriminator` does not resolve `$ref` correctly for the `TextContentBlock` branch | Very Low | High (text block validation breaks) | Ajv documentation explicitly states `$ref` is supported with discriminator. Scenario #1 and #8 verify correct branch selection. |
| Chained `$ref` resolution fails (`tool_result.content` → `StringOrTextBlockArray` → `TextContentBlock`) | Very Low | Medium (tool_result array content validation breaks) | Scenario #5 exercises the full chain. Fallback: flatten chain by inlining `TextContentBlock` into `StringOrTextBlockArray`. |
| Error message paths change due to `$ref` indirection, breaking `formatErrors()` pattern matching | Low | Medium (error messages degrade) | `formatErrors()` matches on `instancePath` (data path, e.g., `/messages/0/content/0`), not `schemaPath`. Ajv's `instancePath` is independent of `$ref`. Verify by comparing error-case outputs before and after. |

## 8. Future Work

- **Readability-driven `$defs` extraction** — extract single-use content block types (`ThinkingContentBlock`, `ToolUseContentBlock`, `ToolResultContentBlock`), `ThinkingConfig`, and `ToolDefinition` as named `$defs` for improved readability and extensibility (e.g., adding `redacted_thinking` becomes a one-`$def` + one-`$ref` change).
- **Response schema `$defs` extraction** — extract `TextPart`, `FunctionCallPart`, and `UsageMetadata` in `response-validator.js` for named part types and readability.
- **Add `RedactedThinkingBlockParam` support** — the [Anthropic API spec](https://platform.claude.com/docs/en/api/messages/create) defines a `redacted_thinking` content block type (`{ data: string, type: "redacted_thinking" }`).
- **Schema visualization** — with named `$defs`, schemas can be rendered as dependency graphs or documentation tables.

## 9. References

- [RFC-003: JSON Schema Request Validation with Ajv](rfc_003_ajv_request_validation.md) — established the Ajv validation pattern in `src/validator.js`.
- [Anthropic Messages API — Create](https://platform.claude.com/docs/en/api/messages/create) — canonical API spec for the request body schema; defines `TextBlockParam`, `ThinkingBlockParam`, `ToolUseBlockParam`, and `ToolResultBlockParam`.
- [Ajv JSON Schema Validator — `$ref`](https://ajv.js.org/guide/combining-schemas.html) — Ajv documentation on `$ref` resolution and `$defs` support.
- [Ajv JSON Schema Validator — `discriminator`](https://ajv.js.org/json-schema.html#discriminator) — confirms `$ref` support within `discriminator` + `oneOf`.
- `odin/src/validator.js` — current request schema (modification target).

---

## Changelog

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2026-02-26 | Chason Tang | Initial version |
