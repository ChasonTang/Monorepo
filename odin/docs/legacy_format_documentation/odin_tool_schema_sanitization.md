# Odin Tool Schema Sanitization

**Document Version:** 1.0  
**Author:** Chason Tang  
**Last Updated:** 2026-02-11  
**Status:** Implemented

---

## 1. Executive Summary

Odin currently passes Anthropic `tools[].input_schema` directly to Google Antigravity Cloud Code without any transformation. The Antigravity gateway uses strict protobuf-backed JSON validation that rejects many standard JSON Schema features (e.g., `$ref`, `additionalProperties`, `const`, `allOf`), causing tool-bearing requests to fail with schema validation errors.

This document proposes a two-stage schema sanitization pipeline ported from the `opencode-antigravity-auth` reference implementation, ensuring all tool schemas pass Antigravity's validation while preserving semantic information through description hints.

### 1.1 Background

The existing `odin_basic_design.md` (§6) already identified this as an open item:

> Tool schema sanitization | ⏳ To verify | Google API may have stricter schema validation.

And in §6.1 Future Improvements:

> Schema sanitization pipeline | Low | Add multi-phase schema cleaning if tool schema errors are encountered.

In practice, Claude Code's built-in tools use relatively simple schemas. However, when MCP servers are connected, their tool schemas frequently include complex JSON Schema features (`$ref`, `$defs`, `allOf`, `anyOf`, `const`, type arrays, etc.) that Antigravity's protobuf validation rejects outright. This causes the entire request to fail, not just the offending tool.

### 1.2 Goals

- **Primary**: All tool schemas from Claude Code (including MCP tools) pass Antigravity's protobuf validation
- **Secondary**: Preserve semantic information (constraints, type unions) as description hints rather than silently dropping them
- **Non-Goals**: Tool name sanitization (not implemented in reference code), parameter signature injection (anti-hallucination measure, not a schema compliance concern)

### 1.3 Key Metrics / Features

| Feature | Current | Target |
|---------|---------|--------|
| Simple tool schemas (Claude Code built-in) | Pass (by luck) | Pass (guaranteed) |
| MCP tool schemas with `$ref` / `$defs` | Rejected | Pass (converted to hints) |
| MCP tool schemas with `allOf` / `anyOf` / `oneOf` | Rejected | Pass (flattened) |
| Schemas with `additionalProperties` | Rejected | Pass (removed with hint) |
| Schemas with `const` | Rejected | Pass (converted to `enum`) |
| Empty object schemas (no properties) | Rejected in VALIDATED mode | Pass (placeholder added) |

---

## 2. Technical Design

### 2.1 Architecture Overview

The sanitization pipeline sits inside `converter.js`, between receiving `input_schema` from Anthropic tools and emitting `parameters` in Google `functionDeclarations`. No new files are introduced.

```
Claude Code                    converter.js                           Antigravity
    │                              │                                       │
    │  tools[].input_schema        │                                       │
    │  (arbitrary JSON Schema)     │                                       │
    │─────────────────────────────▶│                                       │
    │                              │  ┌─────────────────────────────┐      │
    │                              │  │ cleanSchemaForAntigravity() │      │
    │                              │  │  Phase 1: Semantic hints    │      │
    │                              │  │  Phase 2: Flatten complex   │      │
    │                              │  │  Phase 3: Remove keywords   │      │
    │                              │  │  Phase 4: Empty placeholder │      │
    │                              │  └─────────────┬───────────────┘      │
    │                              │                │                      │
    │                              │  ┌─────────────▼───────────────┐      │
    │                              │  │     toGeminiSchema()        │      │
    │                              │  │  Uppercase types            │      │
    │                              │  │  Strip Gemini-unsupported   │      │
    │                              │  │  Fix required & items       │      │
    │                              │  └─────────────┬───────────────┘      │
    │                              │                │                      │
    │                              │  functionDeclarations[].parameters    │
    │                              │  (Gemini-compatible schema)           │
    │                              │─────────────────────────────────────▶│
```

### 2.2 Data Structures

#### 2.2.1 Unsupported Constraint Keywords

Constraints that cannot be represented in Gemini schema but carry useful semantic information. These are moved into the `description` field as hints before removal.

```javascript
const UNSUPPORTED_CONSTRAINTS = [
    'minLength', 'maxLength', 'exclusiveMinimum', 'exclusiveMaximum',
    'pattern', 'minItems', 'maxItems', 'format',
    'default', 'examples',
];
```

#### 2.2.2 Unsupported Keywords (Full Removal List)

All keywords removed after hint extraction. Superset of `UNSUPPORTED_CONSTRAINTS`.

```javascript
const UNSUPPORTED_KEYWORDS = new Set([
    ...UNSUPPORTED_CONSTRAINTS,
    '$schema', '$defs', 'definitions', 'const', '$ref', 'additionalProperties',
    'propertyNames', 'title', '$id', '$comment',
]);
```

#### 2.2.3 Gemini-Specific Unsupported Fields

Additional fields that `toGeminiSchema()` strips. These are JSON Schema features that `cleanSchemaForAntigravity()` does not explicitly handle but Gemini's protobuf rejects.

```javascript
const UNSUPPORTED_GEMINI_FIELDS = new Set([
    'additionalProperties', '$schema', '$id', '$comment', '$ref', '$defs',
    'definitions', 'const', 'contentMediaType', 'contentEncoding',
    'if', 'then', 'else', 'not', 'patternProperties',
    'unevaluatedProperties', 'unevaluatedItems', 'dependentRequired',
    'dependentSchemas', 'propertyNames', 'minContains', 'maxContains',
]);
```

#### 2.2.4 Empty Schema Placeholder

VALIDATED mode requires at least one property in object schemas.

```javascript
const EMPTY_SCHEMA_PLACEHOLDER = {
    _placeholder: {
        type: 'BOOLEAN',
        description: 'Placeholder. Always pass true.',
    },
};
```

### 2.3 Algorithms / Core Logic

The pipeline consists of two stages executed in sequence. Both are ported from `opencode-antigravity-auth` — Stage 1 from `request-helpers.ts` (`cleanJSONSchemaForAntigravity`) and Stage 2 from `gemini.ts` (`toGeminiSchema`).

#### 2.3.1 Stage 1: `cleanSchemaForAntigravity(schema)` — Semantic-Preserving Cleanup

Transforms complex JSON Schema features into simpler equivalents, preserving semantic information as description hints.

```javascript
/**
 * Clean a JSON Schema for Antigravity API compatibility.
 * Transforms unsupported features into description hints while preserving
 * semantic information.
 *
 * Ported from opencode-antigravity-auth request-helpers.ts
 * (cleanJSONSchemaForAntigravity)
 *
 * @param {Object} schema - JSON Schema object
 * @returns {Object} Cleaned schema
 */
function cleanSchemaForAntigravity(schema) {
    if (!schema || typeof schema !== 'object') return schema;

    let result = schema;

    // Phase 1: Convert and add hints (information-preserving transforms)
    result = convertRefsToHints(result);           // $ref → description hint
    result = convertConstToEnum(result);           // const → enum: [value]
    result = addEnumHints(result);                 // enum → description hint
    result = addAdditionalPropertiesHints(result); // additionalProperties → hint
    result = moveConstraintsToDescription(result); // minLength etc. → hint

    // Phase 2: Flatten complex structures
    result = mergeAllOf(result);                   // allOf → merged object
    result = flattenAnyOfOneOf(result);            // anyOf/oneOf → best option
    result = flattenTypeArrays(result);            // ["string","null"] → string

    // Phase 3: Remove unsupported keywords
    result = removeUnsupportedKeywords(result);
    result = cleanupRequiredFields(result);

    // Phase 4: Add placeholder for empty object schemas
    result = addEmptySchemaPlaceholder(result);

    return result;
}
```

**Phase 1 Sub-functions:**

| Function | Input | Output | Example |
|----------|-------|--------|---------|
| `convertRefsToHints` | `{ $ref: "#/$defs/Foo" }` | `{ type: "object", description: "See: Foo" }` | Inline reference resolution |
| `convertConstToEnum` | `{ const: "bar" }` | `{ enum: ["bar"] }` | Gemini supports enum but not const |
| `addEnumHints` | `{ enum: ["a","b","c"] }` | `{ enum: [...], description: "(Allowed: a, b, c)" }` | 2-10 items only |
| `addAdditionalPropertiesHints` | `{ additionalProperties: false }` | `{ description: "(No extra properties allowed)" }` | Preserve intent as hint |
| `moveConstraintsToDescription` | `{ minLength: 1, maxLength: 100 }` | `{ description: "(minLength: 1) (maxLength: 100)" }` | All `UNSUPPORTED_CONSTRAINTS` |

**Phase 2 Sub-functions:**

| Function | Input | Output | Notes |
|----------|-------|--------|-------|
| `mergeAllOf` | `{ allOf: [{props: {a}}, {props: {b}}] }` | `{ properties: {a, b} }` | Merges properties and required |
| `flattenAnyOfOneOf` | `{ anyOf: [{const:"a"},{const:"b"}] }` | `{ type: "string", enum: ["a","b"] }` | Enum pattern detection; otherwise picks best option by score |
| `flattenTypeArrays` | `{ type: ["string","null"] }` | `{ type: "string", description: "(nullable)" }` | Removes nullable from required |

**Phase 3 & 4:** Remove all `UNSUPPORTED_KEYWORDS`, filter `required` to only existing properties, add `_placeholder` property to empty object schemas.

#### 2.3.2 Stage 2: `toGeminiSchema(schema)` — Gemini Format Normalization

Converts the cleaned schema to Gemini's required format.

```javascript
/**
 * Transform a JSON Schema to Gemini-compatible format.
 *
 * Ported from opencode-antigravity-auth gemini.ts (toGeminiSchema)
 *
 * @param {*} schema - A JSON Schema object or primitive
 * @returns {*} Gemini-compatible schema
 */
function toGeminiSchema(schema) {
    if (!schema || typeof schema !== 'object' || Array.isArray(schema)) {
        return schema;
    }

    const result = {};
    const propertyNames = new Set();

    // Collect property names for required validation
    if (schema.properties && typeof schema.properties === 'object') {
        for (const name of Object.keys(schema.properties)) {
            propertyNames.add(name);
        }
    }

    for (const [key, value] of Object.entries(schema)) {
        if (UNSUPPORTED_GEMINI_FIELDS.has(key)) continue;

        if (key === 'type' && typeof value === 'string') {
            result[key] = value.toUpperCase();
        } else if (key === 'properties' && typeof value === 'object' && value !== null) {
            const props = {};
            for (const [propName, propSchema] of Object.entries(value)) {
                props[propName] = toGeminiSchema(propSchema);
            }
            result[key] = props;
        } else if (key === 'items' && typeof value === 'object') {
            result[key] = toGeminiSchema(value);
        } else if ((key === 'anyOf' || key === 'oneOf' || key === 'allOf') && Array.isArray(value)) {
            result[key] = value.map(item => toGeminiSchema(item));
        } else if (key === 'required' && Array.isArray(value)) {
            if (propertyNames.size > 0) {
                const valid = value.filter(p => typeof p === 'string' && propertyNames.has(p));
                if (valid.length > 0) result[key] = valid;
            } else {
                result[key] = value;
            }
        } else {
            result[key] = value;
        }
    }

    // Arrays must have items (Gemini API requirement)
    if (result.type === 'ARRAY' && !result.items) {
        result.items = { type: 'STRING' };
    }

    return result;
}
```

#### 2.3.3 Integration Point

The only change to the existing `anthropicToGoogle()` function:

```javascript
// Before (current code, line 183-194)
if (tools?.length) {
    googleRequest.tools = [{
        functionDeclarations: tools.map(tool => ({
            name: tool.name,
            description: tool.description || '',
            parameters: tool.input_schema || { type: 'object' }
        }))
    }];
    // ...
}

// After
if (tools?.length) {
    googleRequest.tools = [{
        functionDeclarations: tools.map(tool => ({
            name: tool.name,
            description: tool.description || '',
            parameters: toGeminiSchema(
                cleanSchemaForAntigravity(tool.input_schema || { type: 'object' })
            )
        }))
    }];
    // ...
}
```

### 2.4 Flow Diagram

```
    input_schema (from Claude Code / MCP)
         │
         ▼
    ┌─────────────────────────────────────┐
    │    cleanSchemaForAntigravity()      │
    │                                     │
    │  Phase 1: Semantic hints            │
    │    $ref → hint ──────────────────┐  │
    │    const → enum                  │  │
    │    enum → hint                   ├──┤ information preserved
    │    additionalProperties → hint   │  │ in description field
    │    constraints → hint ───────────┘  │
    │                                     │
    │  Phase 2: Flatten                   │
    │    allOf → merge properties         │
    │    anyOf/oneOf → best option        │
    │    type arrays → single type        │
    │                                     │
    │  Phase 3: Remove keywords           │
    │    strip $schema, $defs, title...   │
    │    filter required                  │
    │                                     │
    │  Phase 4: Empty schema placeholder  │
    │    {} → { _placeholder: BOOLEAN }   │
    └─────────────┬───────────────────────┘
                  │ clean JSON Schema (lowercase types)
                  ▼
    ┌─────────────────────────────────────┐
    │         toGeminiSchema()            │
    │                                     │
    │    type → TYPE (uppercase)          │
    │    strip if/then/else/not/...       │
    │    required: filter to existing     │
    │    array: add items if missing      │
    │    recursive on properties/items    │
    └─────────────┬───────────────────────┘
                  │ Gemini-compatible schema
                  ▼
    functionDeclarations[].parameters
```

---

## 3. Interface Design

### 3.1 API / CLI Interface

No changes to Odin's external interface. The sanitization is entirely internal to the `anthropicToGoogle()` conversion.

### 3.2 Input / Output Format

**Input (Anthropic tool schema — worst-case MCP example):**

```json
{
    "type": "object",
    "properties": {
        "format": {
            "anyOf": [
                { "const": "text" },
                { "const": "markdown" },
                { "const": "html" }
            ],
            "description": "Output format"
        },
        "options": {
            "$ref": "#/$defs/OutputOptions"
        },
        "count": {
            "type": ["integer", "null"],
            "minimum": 1,
            "maximum": 100
        }
    },
    "$defs": {
        "OutputOptions": {
            "type": "object",
            "properties": {
                "verbose": { "type": "boolean" }
            }
        }
    },
    "required": ["format"],
    "additionalProperties": false
}
```

**Output (after both stages):**

```json
{
    "type": "OBJECT",
    "properties": {
        "format": {
            "type": "STRING",
            "enum": ["text", "markdown", "html"],
            "description": "Output format"
        },
        "options": {
            "type": "OBJECT",
            "description": "See: OutputOptions",
            "properties": {
                "_placeholder": {
                    "type": "BOOLEAN",
                    "description": "Placeholder. Always pass true."
                }
            },
            "required": ["_placeholder"]
        },
        "count": {
            "type": "INTEGER",
            "description": "(nullable)"
        }
    },
    "required": ["format"]
}
```

### 3.3 Error Handling

The sanitization pipeline does not throw errors. Invalid or unexpected input is handled defensively:

| Condition | Behavior |
|-----------|----------|
| `input_schema` is `null` / `undefined` | Falls back to `{ type: 'object' }` (existing behavior) |
| `input_schema` is a non-object primitive | Returned as-is (no-op) |
| Malformed `$ref` value | Converted to generic `{ type: "object", description: "See: <name>" }` |
| Empty `properties` object | Placeholder property added |
| `required` references non-existent property | Entry silently filtered out |
| Unknown schema keys not in removal lists | Passed through unchanged |

---

## 4. Implementation Plan

### Phase 1: Stage 1 — `cleanSchemaForAntigravity` ✅

**Task 1.1: Phase 1 sub-functions**
- [x] `convertRefsToHints(schema)` — recursive `$ref` → description hint
- [x] `convertConstToEnum(schema)` — recursive `const` → `enum: [value]`
- [x] `addEnumHints(schema)` — recursive enum → description hint
- [x] `addAdditionalPropertiesHints(schema)` — recursive hint injection
- [x] `moveConstraintsToDescription(schema)` — all `UNSUPPORTED_CONSTRAINTS`

**Task 1.2: Phase 2 sub-functions**
- [x] `mergeAllOf(schema)` — merge properties/required from allOf items
- [x] `flattenAnyOfOneOf(schema)` — enum pattern detection + best-option scoring
- [x] `flattenTypeArrays(schema)` — type array → single type + nullable hint

**Task 1.3: Phase 3 & 4 sub-functions**
- [x] `removeUnsupportedKeywords(schema)` — strip all `UNSUPPORTED_KEYWORDS`
- [x] `cleanupRequiredFields(schema)` — filter required to existing properties
- [x] `addEmptySchemaPlaceholder(schema)` — placeholder for empty objects

**Acceptance Criteria:** ✅ All met
- `cleanSchemaForAntigravity()` produces a schema with no `$ref`, `const`, `allOf`, `anyOf`, `additionalProperties`, or type arrays
- Semantic information is preserved in `description` field hints
- Empty object schemas have a `_placeholder` property

### Phase 2: Stage 2 — `toGeminiSchema` ✅

**Task 2.1: Core function**
- [x] `toGeminiSchema(schema)` — recursive type uppercasing, field stripping, required filtering, array items fix

**Acceptance Criteria:** ✅ All met
- All `type` values are uppercase
- No fields from `UNSUPPORTED_GEMINI_FIELDS` remain
- Arrays without `items` get default `{ type: "STRING" }`
- `required` only references existing properties

### Phase 3: Integration ✅

**Task 3.1: Wire into `anthropicToGoogle()`**
- [x] Replace `tool.input_schema || { type: 'object' }` with `toGeminiSchema(cleanSchemaForAntigravity(tool.input_schema || { type: 'object' }))`
- [x] Update `odin_basic_design.md` §6 to mark tool schema sanitization as ✅ Done
- [x] Update `odin_basic_design.md` §6.1 to mark schema sanitization pipeline as implemented

---

## 5. Testing

### 5.1 Test Cases

| Test Scenario | Input Schema | Expected Output | Validates |
|---------------|-------------|-----------------|-----------|
| Simple object (passthrough) | `{ type: "object", properties: { path: { type: "string" } }, required: ["path"] }` | `{ type: "OBJECT", properties: { path: { type: "STRING" } }, required: ["path"] }` | Basic type uppercasing |
| `additionalProperties: false` | `{ type: "object", additionalProperties: false, properties: { a: { type: "string" } } }` | `{ type: "OBJECT", properties: { a: { type: "STRING", description: "..." } } }` (no `additionalProperties`) | Hint injection + removal |
| `$ref` field | `{ $ref: "#/$defs/Foo" }` | `{ type: "OBJECT", description: "See: Foo" }` | $ref conversion |
| `const` value | `{ const: "bar" }` | `{ enum: ["bar"] }` (after Stage 1), then uppercased | const → enum |
| `anyOf` with consts (enum pattern) | `{ anyOf: [{ const: "a" }, { const: "b" }] }` | `{ type: "STRING", enum: ["a", "b"] }` | Enum pattern detection |
| `anyOf` with mixed types | `{ anyOf: [{ type: "string" }, { type: "number" }] }` | `{ type: "STRING", description: "(Accepts: string \| number)" }` | Best-option selection |
| `allOf` merge | `{ allOf: [{ properties: { a: { type: "string" } } }, { properties: { b: { type: "number" } } }] }` | `{ type: "OBJECT", properties: { a: { type: "STRING" }, b: { type: "NUMBER" } } }` | Property merging |
| Type array with null | `{ type: ["string", "null"] }` | `{ type: "STRING", description: "(nullable)" }` | Type array flattening |
| Array without items | `{ type: "array" }` | `{ type: "ARRAY", items: { type: "STRING" } }` | Default items injection |
| Empty object schema | `{ type: "object" }` | `{ type: "OBJECT", properties: { _placeholder: { type: "BOOLEAN", ... } }, required: ["_placeholder"] }` | Placeholder injection |
| Invalid required | `{ type: "object", properties: { a: {} }, required: ["a", "nonexistent"] }` | `required: ["a"]` | Required filtering |
| Constraint hints | `{ type: "string", minLength: 1, maxLength: 100, pattern: "^[a-z]+$" }` | `{ type: "STRING", description: "(minLength: 1) (maxLength: 100) (pattern: ^[a-z]+$)" }` | Constraint → description |

### 5.2 Integration Testing

```bash
# 1. Start Odin
node src/index.js --api-key="$(cat ~/.odin-key)" --port=8080 --debug

# 2. Configure Claude Code with MCP server that has complex schemas
export ANTHROPIC_BASE_URL=http://localhost:8080
export ANTHROPIC_API_KEY=dummy

# 3. Start Claude Code and trigger MCP tool usage
claude

# 4. Verify in debug output:
#    - [Odin:debug] → Payload shows sanitized schemas in functionDeclarations
#    - No schema validation errors from Antigravity
#    - Tool calls execute successfully
```

---

## 6. Risk Assessment

### 6.1 Risks and Mitigations

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| `$ref` targets are lost (not inlined, only hinted) | Medium | Low | Odin does not have access to `$defs` definitions from the calling context. The reference implementation (`opencode-antigravity-auth`) also only converts `$ref` to description hints — not full inlining. Claude can infer from context. |
| `anyOf`/`oneOf` flattening picks wrong option | Low | Low | Scoring heuristic (object > array > other > null) matches the reference implementation. Description hints preserve all type options. |
| Future Antigravity gateway changes reject new fields | Low | Medium | Defense-in-depth: two-stage pipeline strips fields aggressively. Unknown fields that survive both stages are unlikely to cause issues since protobuf ignores unknown fields in some modes. |
| Performance overhead from recursive schema traversal | Low | Low | Tool schemas are small (typically <50 properties). Two recursive traversals add negligible latency (<1ms) compared to network round-trip. |

---

## 7. Future Considerations

### 7.1 Potential Extensions

| Feature | Status | Notes |
|---------|--------|-------|
| Full `$ref` / `$defs` inlining | 💡 Idea | If `$defs` is present in the top-level schema, inline referenced definitions before hint conversion. Would produce more accurate schemas but the reference implementation does not do this. |
| Tool name sanitization | 💡 Idea | Enforce `[a-zA-Z_][a-zA-Z0-9_\-.:]{0,63}` pattern. Not implemented in reference code but documented in Antigravity API spec. Implement only if rejection errors are observed. |
| Parameter signature injection | 💡 Idea | Port `injectParameterSignatures()` from reference implementation to add `STRICT PARAMETERS` hints to tool descriptions. Anti-hallucination measure, not a schema compliance concern. |

---

## 8. Appendix

### 8.1 References

1. `opencode-antigravity-auth/src/plugin/transform/gemini.ts` — `toGeminiSchema()`, `UNSUPPORTED_SCHEMA_FIELDS`
2. `opencode-antigravity-auth/src/plugin/request-helpers.ts` — `cleanJSONSchemaForAntigravity()`, all Phase 1-4 sub-functions
3. `opencode-antigravity-auth/docs/ANTIGRAVITY_API_SPEC.md` — Antigravity gateway JSON Schema support matrix
4. `opencode-antigravity-auth/docs/ARCHITECTURE.md` — Schema cleaning allowlist documentation

### 8.2 Related Documents

| Document | Description |
|----------|-------------|
| `odin_basic_design.md` | Main Odin design document (§6, §6.1 updated to ✅ Done) |

### 8.3 Glossary

| Term | Definition |
|------|------------|
| Antigravity | Google's AI coding assistant platform, accessed via Cloud Code API |
| VALIDATED mode | `toolConfig.functionCallingConfig.mode` that enforces strict schema validation on tool parameters |
| MCP | Model Context Protocol — allows Claude Code to connect external tool servers |
| protobuf-backed validation | Antigravity gateway validates JSON against protobuf message definitions, rejecting unknown fields |

---

## Changelog

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2026-02-10 | Chason Tang | Initial version |
| 1.1 | 2026-02-11 | Chason Tang | All phases implemented. Status → Implemented. Updated implementation plan checkboxes, related documents, and changelog. |

---

*End of Technical Design Document*
