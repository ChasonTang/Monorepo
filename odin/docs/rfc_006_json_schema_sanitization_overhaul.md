# RFC-006: JSON Schema Sanitization Overhaul for Antigravity Compatibility

**Version:** 3.2  
**Author:** Chason Tang  
**Date:** 2026-02-20  
**Status:** Implemented

---

## 1. Summary

The current Odin proxy uses a legacy denylist-based JSON Schema sanitization pipeline that has drifted out of sync with the JSON Schema 2020-12 specification. Of the 57 standard keywords defined in `draft/2020-12/schema`, **34 keywords are unsupported** by Google Antigravity; additionally, 4 deprecated keywords from previous drafts total **38 non-whitelisted keywords**. This RFC proposes replacing the existing two-stage approach (`cleanSchemaForAntigravity` → `toGeminiSchema`) with a **single-stage whitelist-based deep sanitizer** that systematically strips or transforms all unsupported keywords, ensuring 100% coverage of the JSON Schema 2020-12 vocabulary.

**Key design decisions:**
- **Whitelist architecture** — only 23 known-safe keywords pass through; everything else is stripped.
- **Minimal semantic transforms** — only 2 transforms produce output: `const→enum`, `exclusiveBounds→min/max+hint`. All other unsupported keywords (including `propertyNames`) are stripped by the whitelist filter with monitoring.
- **Three-tier logging** — unknown keywords (not in any registry), monitorable keywords (could have semantic transforms in the future), and routine known-unsupported keywords each have distinct logging behavior for proactive monitoring.

## 2. Motivation

**Problem:** Anthropic tool definitions use `input_schema` conforming to JSON Schema 2020-12. When the Odin proxy forwards these to Google Antigravity, schemas containing unsupported keywords are either:

1. **Silently passed through** — causing Antigravity to reject the request with opaque 400 errors, or
2. **Stripped without semantic preservation** — losing constraint information that the model could otherwise use.

**Coverage gaps in the current sanitizer:**

| Vocabulary | Total | Unsupported | Currently Handled |
|---|---|---|---|
| Core | 9 | 9 | 5 of 9 |
| Applicator | 15 | 7 | 0 of 7 |
| Validation | 20 | 8 | 2 of 8 |
| Unevaluated | 2 | 2 | 1 of 2 |
| Meta-Data | 7 | 5 | 1 of 5 |
| Format | 1 | 0 | N/A |
| Content | 3 | 3 | 1 of 3 |
| **Deprecated** | **4** | **4** | **1 of 4** |

The existing two-stage pipeline (`cleanSchemaForAntigravity` → `toGeminiSchema`) contains stale denylist constants (`UNSUPPORTED_CONSTRAINTS`, `UNSUPPORTED_KEYWORDS`, `UNSUPPORTED_GEMINI_FIELDS`) that overlap inconsistently and miss numerous keywords. A single-stage whitelist approach consolidates all sanitization logic, eliminating redundancy and inter-stage drift.

## 3. Goals and Non-Goals

**Goals:**
- 100% coverage of JSON Schema 2020-12 keywords (57 standard + 4 deprecated).
- Whitelist-based architecture that is safe-by-default against unknown keywords.
- Single-stage sanitizer replacing the two-stage pipeline.
- **Monitoring logging** for keywords that could have semantic-preserving transforms in the future, enabling data-driven prioritization.
- Log unknown (unrecognized) keywords to detect upstream schema changes.

**Non-Goals:**
- Full JSON Schema 2020-12 evaluation or `$ref`/`$defs` inline resolution.
- `$vocabulary` negotiation, non-standard extension handling (`x-*`).
- Description field length truncation (deferred to production validation).

## 4. Design

### 4.1 Overview

The sanitizer is a single-stage pure function `sanitizeSchemaForAntigravity(schema, logger?) → schema` that replaces the previous two-stage design. It operates in two phases:

```
┌─────────────────────────────────────────────────────────────┐
│                   Anthropic input_schema                    │
│                (JSON Schema 2020-12 full)                   │
└─────────────────┬───────────────────────────────────────────┘
                  │
    ┌─────────────▼──────────────────┐
    │  Phase 1: Semantic Transforms  │   Rewrite unsupported → supported equivalents
    │  1a. Convert const → enum      │   { const: "x" } → { enum: ["x"] }
    │  1b. Exclusive bounds          │   exclusiveMin/Max → min/max + hint
    └─────────────┬──────────────────┘
                  │
    ┌─────────────▼──────────────────┐
    │  Phase 2: Whitelist Filter     │   Remove everything not in ALLOWED set
    │  2a. Recursive allowlist strip │   Only permit known-safe keywords
    │  2b. Log monitorable keywords  │   Log keywords with future transform potential
    │  2c. Log unknown keywords      │   Log unrecognized keywords for monitoring
    └─────────────┬──────────────────┘
                  │
    ┌─────────────▼──────────────────┐
    │    Antigravity-safe schema     │
    │   (supported subset only)      │
    └────────────────────────────────┘
```

> **Key simplification:** Previous versions had individual Phase 1 transforms for stripping deprecated keywords, Core keywords, `if`/`then`/`else`, `dependentSchemas`, and other constraint keywords. Since all of these are simple deletions with no output production, they are now handled uniformly by the Phase 2 whitelist filter — no dedicated functions needed.

### 4.2 Detailed Design

#### 4.2.1 Keyword Registries

Three sets govern the sanitizer's behavior:

**① Allowed Keywords (23 total)** — pass through unchanged:

```javascript
const ALLOWED_KEYWORDS = new Set([
    // Applicator (8 of 15)
    'allOf', 'anyOf', 'oneOf', 'not', 'prefixItems', 'items', 'properties', 'additionalProperties',
    // Validation (12 of 20)
    'type', 'enum', 'maximum', 'minimum', 'maxLength', 'minLength', 'pattern',
    'maxItems', 'minItems', 'maxProperties', 'minProperties', 'required',
    // Meta-Data (2 of 7)
    'description', 'default',
    // Format (1 of 1)
    'format',
]);
```

**② Monitorable Keywords** — currently deleted but could have semantic-preserving transforms. Logged at `warn` level when encountered, to track Claude Code usage and prioritize future implementation:

```javascript
/**
 * Keywords that are currently stripped but COULD be converted to description
 * hints or other semantic-preserving transforms in the future.
 *
 * When any of these keywords is encountered and stripped, a warn-level log is
 * emitted with the keyword name and value. This enables us to:
 * 1. Detect when Claude Code starts sending these keywords
 * 2. Measure frequency to prioritize which transforms to implement next
 * 3. Proactively improve model accuracy before users report quality issues
 *
 * Grouped by potential future transform complexity:
 */
const MONITORABLE_KEYWORDS = new Set([
    // ── Core vocabulary — future: $ref/$defs inline resolution ──
    '$ref',
    '$defs',

    // ── Applicator — future: conditional/dependency/constraint description hints ──
    'if', 'then', 'else',
    'dependentSchemas',
    'contains',
    'propertyNames',
    'patternProperties',

    // ── Validation — future: constraint description hints ──
    'multipleOf',
    'uniqueItems',
    'maxContains',
    'minContains',
    'dependentRequired',

    // ── Unevaluated — future: structural constraint hints ──
    'unevaluatedItems',
    'unevaluatedProperties',

    // ── Meta-Data — future: preserve as description hints ──
    'title',
    'deprecated',
    'readOnly',
    'writeOnly',
    'examples',

    // ── Content — future: encoding/media type hints ──
    'contentEncoding',
    'contentMediaType',
    'contentSchema',

    // ── Deprecated — future: merge/rewrite if encountered ──
    'definitions',
    'dependencies',
]);
```

**③ Known Unsupported Keywords (38 total)** — all non-allowed keywords from JSON Schema 2020-12 + deprecated. Used to distinguish expected vs. unexpected removals:

```javascript
/**
 * All JSON Schema 2020-12 keywords NOT in the whitelist (34 standard + 4 deprecated = 38).
 * Keywords in this set but NOT in MONITORABLE_KEYWORDS are stripped silently
 * (they have no meaningful semantic-preserving transform potential).
 */
const KNOWN_UNSUPPORTED_KEYWORDS = new Set([
    // ── Core vocabulary (9, all unsupported) ──
    '$schema', '$id', '$ref', '$anchor', '$dynamicRef', '$dynamicAnchor',
    '$vocabulary', '$comment', '$defs',
    // ── Applicator (7 of 15 unsupported) ──
    'if', 'then', 'else', 'dependentSchemas', 'contains', 'patternProperties', 'propertyNames',
    // ── Validation (8 of 20 unsupported) ──
    'const',               // Converted to enum in Phase 1a
    'multipleOf', 'exclusiveMaximum', 'exclusiveMinimum',
    'uniqueItems', 'maxContains', 'minContains', 'dependentRequired',
    // ── Unevaluated (2, all unsupported) ──
    'unevaluatedItems', 'unevaluatedProperties',
    // ── Meta-Data (5 of 7 unsupported) ──
    'title', 'deprecated', 'readOnly', 'writeOnly', 'examples',
    // ── Content (3, all unsupported) ──
    'contentEncoding', 'contentMediaType', 'contentSchema',
    // ── Deprecated (4, from 2020-12 root meta-schema) ──
    'definitions', 'dependencies', '$recursiveAnchor', '$recursiveRef',
]);
```

> **Three-tier logging behavior** in Phase 2:
> - Keyword in `ALLOWED_KEYWORDS` → **keep** (pass through)
> - Keyword in `MONITORABLE_KEYWORDS` → **strip + warn log** (e.g., `[schema-sanitizer] Monitorable keyword stripped: "$ref"`)
> - Keyword in `KNOWN_UNSUPPORTED_KEYWORDS` only → **strip silently** (routine, no transform potential)
> - Keyword in **none of the above** → **strip + info log** (unknown keyword, may indicate upstream changes)

#### 4.2.2 Phase 1: Semantic Transforms

Phase 1 contains only transforms that **produce output** (convert an unsupported keyword into a supported representation). Phase 1 transforms extract values from unsupported keywords and create supported equivalents, but **do not delete the original keywords** — all keyword deletion is handled uniformly by Phase 2's whitelist filter.

##### Phase 1a: Convert const → enum

`const` has an exact semantic equivalent: `{ const: "x" }` → `{ enum: ["x"] }`.

##### Phase 1b: Exclusive Bounds

`exclusiveMinimum` / `exclusiveMaximum` have close supported equivalents (`minimum` / `maximum`):

| Keyword | Transform |
|---|---|
| `exclusiveMinimum` | → description hint `"(exclusiveMinimum: N)"`; for `type: "integer"`: `minimum = exclusiveMinimum + 1` |
| `exclusiveMaximum` | → description hint `"(exclusiveMaximum: N)"`; for `type: "integer"`: `maximum = exclusiveMaximum - 1` |

For `type: "number"`, only the description hint is added (no `minimum`/`maximum` conversion since the conversion is not exact for continuous values).

```javascript
function convertExclusiveBounds(schema) {
    if (!schema || typeof schema !== 'object') return schema;
    if (Array.isArray(schema)) return schema.map(convertExclusiveBounds);

    let result = { ...schema };

    if (result.exclusiveMinimum !== undefined) {
        const excMin = result.exclusiveMinimum;
        result = appendDescriptionHint(result, `exclusiveMinimum: ${excMin}`);
        if (result.type === 'integer' && result.minimum === undefined) {
            result.minimum = excMin + 1;
        }
    }

    if (result.exclusiveMaximum !== undefined) {
        const excMax = result.exclusiveMaximum;
        result = appendDescriptionHint(result, `exclusiveMaximum: ${excMax}`);
        if (result.type === 'integer' && result.maximum === undefined) {
            result.maximum = excMax - 1;
        }
    }

    for (const [key, value] of Object.entries(result)) {
        if (typeof value === 'object' && value !== null) {
            result[key] = convertExclusiveBounds(value);
        }
    }
    return result;
}
```

#### 4.2.3 Phase 2: Whitelist Filter with Monitoring

The whitelist filter is the **single safety net** that strips all unsupported keywords — including deprecated keywords, Core vocabulary, `if`/`then`/`else`, `dependentSchemas`, and all remaining constraint keywords. Previous versions had dedicated Phase 1 functions for each category of deletion; these are now unified here.

```javascript
/**
 * Phase 2: Whitelist filter — only permit known-safe JSON Schema keywords.
 * All keywords not in ALLOWED_KEYWORDS are stripped. Logging tiers:
 *   - MONITORABLE_KEYWORDS:      warn-level  (future transform potential)
 *   - unknown (not in any set):  info-level  (upstream schema changes)
 *   - other KNOWN_UNSUPPORTED:   silent      (routine, no action needed)
 */
function filterToAllowedKeywords(schema, logger) {
    if (!schema || typeof schema !== 'object') return schema;
    if (Array.isArray(schema)) return schema.map(s => filterToAllowedKeywords(s, logger));

    const result = {};

    for (const [key, value] of Object.entries(schema)) {
        if (ALLOWED_KEYWORDS.has(key)) {
            // ── Allowed: recurse into sub-schemas ──
            if (key === 'properties' && typeof value === 'object' && value !== null) {
                const props = {};
                for (const [propName, propSchema] of Object.entries(value)) {
                    props[propName] = filterToAllowedKeywords(propSchema, logger);
                }
                result[key] = props;
            } else if (
                (key === 'items' || key === 'not' || key === 'additionalProperties') &&
                typeof value === 'object' && value !== null
            ) {
                result[key] = filterToAllowedKeywords(value, logger);
            } else if (
                (key === 'allOf' || key === 'anyOf' || key === 'oneOf' || key === 'prefixItems') &&
                Array.isArray(value)
            ) {
                result[key] = value.map(item => filterToAllowedKeywords(item, logger));
            } else {
                result[key] = value;
            }
            continue;
        }

        // ── Not allowed: three-tier logging ──
        if (MONITORABLE_KEYWORDS.has(key)) {
            // Keyword with future transform potential — log for prioritization
            if (logger) {
                logger.warn(
                    `[schema-sanitizer] Monitorable keyword stripped: "${key}" ` +
                    `(value: ${JSON.stringify(value)?.slice(0, 200)})`
                );
            }
        } else if (!KNOWN_UNSUPPORTED_KEYWORDS.has(key)) {
            // Unknown keyword — may indicate new upstream pattern or Antigravity support
            if (logger) {
                logger.info(
                    `[schema-sanitizer] Unknown keyword stripped: "${key}" ` +
                    `(value: ${JSON.stringify(value)?.slice(0, 100)})`
                );
            }
        }
        // Known unsupported (not monitorable): strip silently
    }

    return result;
}
```

#### 4.2.4 Orchestration

```javascript
function sanitizeSchemaForAntigravity(schema, logger) {
    if (!schema || typeof schema !== 'object') return schema;

    let result = schema;

    // ── Phase 1: Semantic Transforms (only output-producing transforms) ──
    result = convertConstToEnum(result);              // 1a: const → enum
    result = convertExclusiveBounds(result);          // 1b: exclusiveMin/Max → min/max + hint

    // ── Phase 2: Whitelist Filter ──
    result = filterToAllowedKeywords(result, logger); // 2a-c: strip + log (monitorable/unknown)

    return result;
}
```

### 4.3 Design Rationale

**1. Whitelist vs. Denylist** — The denylist approach requires exhaustively enumerating every unsupported keyword across multiple filter stages. The whitelist is safe-by-default: unknown keywords are automatically stripped. The trade-off (must explicitly add newly supported keywords) is the safer failure mode.

**2. Single-Stage Pipeline** — The previous two-stage design suffered from denylist drift between `UNSUPPORTED_KEYWORDS` and `UNSUPPORTED_GEMINI_FIELDS`. A single whitelist eliminates this class of bugs.

**3. Phase Ordering** — Phase 1 runs before Phase 2 because semantic transforms need access to unsupported keywords to extract their values. The whitelist would destroy this information.

**4. Direct Stripping of Core Vocabulary (`$ref`/`$defs`)** — Implementing `$ref` inline resolution adds significant complexity (demand-driven resolution, cross-reference handling, circular reference detection, 2020-12 sibling keyword semantics). Direct stripping keeps the initial implementation simple. `$ref`/`$defs` are in `MONITORABLE_KEYWORDS` so their occurrence will be logged for future prioritization.

**5. Integer-Only Exclusive Bounds Conversion** — For `type: "integer"`, `exclusiveMinimum: 5` ≡ `minimum: 6` (mathematically exact). For `type: "number"`, no conversion is attempted — only a description hint is added.

**6. Direct Deletion with Monitoring** — Keywords like `if`/`then`/`else`, `dependentSchemas`, Meta-Data, and Content keywords are directly deleted with `warn`-level logging (via `MONITORABLE_KEYWORDS`). This avoids implementation complexity while providing visibility into real-world usage. If monitoring shows significant Claude Code usage of any keyword, we can prioritize implementing its semantic-preserving transform.

**7. Unified Deletion in Whitelist** — Previous RFC versions had individual Phase 1 functions for stripping deprecated keywords, Core keywords, conditionals, etc. Since these are all simple deletions with no output production, they are now handled uniformly by the Phase 2 whitelist filter — reducing code, eliminating redundant recursive traversals, and simplifying the architecture.

**8. Native `allOf`/`anyOf`/`oneOf` Support** — These are natively supported by Antigravity. Sub-schemas within them are recursively sanitized by the whitelist filter.

## 5. Implementation Plan

### Step 1: Keyword Registries + Whitelist Filter — 1 day

- [x] Define `ALLOWED_KEYWORDS`, `MONITORABLE_KEYWORDS`, `KNOWN_UNSUPPORTED_KEYWORDS` sets
- [x] Implement `filterToAllowedKeywords()` with three-tier logging
- [x] Remove old `removeUnsupportedKeywords()`, `UNSUPPORTED_KEYWORDS`, `UNSUPPORTED_GEMINI_FIELDS` constants
- [x] Unit tests: all 23 allowed keywords pass, all 38 non-whitelisted keywords stripped
- [x] Unit tests: monitorable keywords emit warn log, unknown keywords emit info log

**Done when:** whitelist correctly filters all keywords with appropriate logging.

### Step 2: Semantic Transforms — 1 day

- [x] Implement `convertConstToEnum()` — `const → enum`
- [x] Implement `convertExclusiveBounds()` — with integer optimization and description hint
- [x] Implement `appendDescriptionHint()` utility
- [x] Unit tests for each transform

**Done when:** all semantic transforms produce correct output with test coverage.

### Step 3: Integration & End-to-End Testing — 1 day

- [x] Create `sanitizeSchemaForAntigravity()` orchestration function
- [x] Remove old `cleanSchemaForAntigravity()` and `toGeminiSchema()` two-stage pipeline
- [x] End-to-end tests with real-world Anthropic tool schemas (MCP, Cursor, etc.)
- [x] Verify all transformed schemas accepted by Antigravity API
- [x] Update `antigravity-claude-proxy/src/format/schema-sanitizer.js` to align

**Done when:** E2E tests pass, Antigravity API accepts all transformed schemas.

## 6. Testing Strategy

| # | Scenario | Input | Expected |
|---|----------|-------|----------|
| 1 | Supported schema passthrough | `{ type: "object", properties: { name: { type: "string" } }, required: ["name"] }` | Unchanged |
| 2 | `$ref`/`$defs` stripping + monitoring log | `{ $defs: { Addr: {…} }, properties: { home: { $ref: "#/$defs/Addr" } } }` | `$defs`/`$ref` stripped; `home` → `{}`; **warn log** for `$ref` and `$defs` |
| 3 | `const` → `enum` | `{ properties: { status: { const: "active" } } }` | `{ properties: { status: { enum: ["active"] } } }` |
| 4 | `exclusiveMinimum` on integer | `{ type: "integer", exclusiveMinimum: 0 }` | `{ type: "integer", minimum: 1, description: "(exclusiveMinimum: 0)" }` |
| 5 | `exclusiveMaximum` on number | `{ type: "number", exclusiveMaximum: 100 }` | `{ type: "number", description: "(exclusiveMaximum: 100)" }` — no `maximum` |
| 6 | `if`/`then`/`else` stripping + monitoring | `{ if: {…}, then: {…}, else: {…} }` | All stripped; **warn log** for each keyword |
| 7 | `dependentRequired` stripping + monitoring | `{ dependentRequired: { cc: ["billing"] } }` | Stripped; **warn log** |
| 8 | Meta-data stripping + monitoring | `{ title: "User", deprecated: true, description: "The user" }` | `title`/`deprecated` stripped; `description` preserved; **warn log** for `title`, `deprecated` |
| 9 | `examples` stripping + monitoring | `{ type: "string", examples: ["a","b"] }` | `examples` stripped; **warn log** |
| 10 | Unknown keyword logging | `{ type: "string", $futureKeyword: true, x-custom: "foo" }` | `{ type: "string" }`; **info log** for unknown keywords |
| 11 | Known unsupported (silent) | `{ type: "string", $schema: "…", $id: "…" }` | `{ type: "string" }` — stripped silently (not monitorable) |
| 12 | Deeply nested unsupported | `{ properties: { tags: { type: "array", items: { contentEncoding: "base64" }, uniqueItems: true } } }` | Both stripped; **warn log** for `contentEncoding`, `uniqueItems` |
| 13 | Deprecated `definitions` | `{ definitions: { Foo: { type: "string" } } }` | Stripped; **warn log** |
| 14 | `allOf`/`anyOf`/`oneOf` passthrough | `{ anyOf: [{ type: "string" }, { type: "integer" }] }` | Unchanged; sub-schemas recursively sanitized |
| 15 | `propertyNames` stripping + monitoring | `{ propertyNames: { pattern: "^[a-z]+$" } }` | `propertyNames` stripped; **warn log** |

## 7. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Description field bloat from accumulated hints | Med | Low | Monitor in production; add truncation if needed |
| `$ref` stripping loses referenced type structure | Med | Med | Monitorable logging tracks frequency; add inline resolution if needed |
| Removing a keyword that Antigravity actually supports | Low | Low | One-line change to `ALLOWED_KEYWORDS` to restore |
| Non-standard extensions (`x-*`) | Med | Low | Whitelist naturally strips; logged as unknown |
| Performance regression from transform phases | Low | Low | O(n) tree walks; schemas typically <100 nodes |

## 8. Future Work

- **`$ref`/`$defs` inline resolution** — Demand-driven resolution with circular reference detection. Currently in `MONITORABLE_KEYWORDS`; implement when monitoring shows significant usage.
- **Description hints for monitorable keywords** — Add semantic-preserving transforms for: `propertyNames`, `if`/`then`/`else`, `dependentSchemas`, `dependentRequired`, `multipleOf`, `uniqueItems`, `contains`, `patternProperties`, `unevaluatedItems`/`unevaluatedProperties`, Meta-Data (`title`, `deprecated`, `readOnly`, `writeOnly`, `examples`), Content (`contentEncoding`, `contentMediaType`, `contentSchema`). Prioritize based on monitoring log frequency.
- **Type array normalization** — Convert `type: ["string", "null"]` → `type: "string"` with `(nullable)` hint, if encountered.
- **Final fixup transforms** — `addEmptySchemaPlaceholder()` (ensure object schemas have ≥1 property for VALIDATED mode), `ensureArrayItems()` (ensure `type: "array"` has `items`).
- **Schema size optimization** — Truncate description hints when total schema exceeds token threshold.
- **Shared sanitizer library** — Extract into a shared package for `odin` and `antigravity-claude-proxy`.
- **Active keyword probing** — Test suite that probes Antigravity API for newly supported keywords.

## 9. References

- [JSON Schema 2020-12 Meta-Schema](https://json-schema.org/draft/2020-12/schema)
- [JSON Schema 2020-12 Validation Vocabulary](https://json-schema.org/draft/2020-12/json-schema-validation)
- [JSON Schema 2020-12 Core Vocabulary](https://json-schema.org/draft/2020-12/json-schema-core)
- `odin/src/converter.js` — Current implementation (to be replaced)
- `antigravity-claude-proxy/src/format/schema-sanitizer.js` — Alternative implementation (to be aligned)
- [Anthropic Tool Use API](https://docs.anthropic.com/en/docs/build-with-claude/tool-use)
- [Google Generative AI API — Function Declarations](https://ai.google.dev/gemini-api/docs/function-calling)

---

## Changelog

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 3.2 | 2026-02-20 | Chason Tang | (1) Move `propertyNames` from Phase 1c description-hint transform into `MONITORABLE_KEYWORDS` — monitor-first approach consistent with §4.3 rationale #6; (2) Remove Phase 1c, `convertPropertyNamesToHint()`, and `summarizeSchema()` — Phase 1 now only has 2 output-producing transforms; (3) Update test case #15 to expect warn-log stripping instead of description hint |
| 3.1 | 2026-02-20 | Chason Tang | (1) Phase 1 transforms no longer delete original keywords — all deletion unified in Phase 2 whitelist; (2) Remove `cleanupRequiredFields()` (Phase 2d) — deferred to future iteration; (3) Merge `summarizeSchema()` utility into Phase 1c section; (4) Remove changelog template placeholder |
| 3.0 | 2026-02-20 | Chason Tang | Major restructure: (1) Add `MONITORABLE_KEYWORDS` set with warn-level logging for all directly-deleted keywords that could have semantic-preserving transforms (`$ref`/`$defs`, `if`/`then`/`else`, `dependentSchemas`, Meta-Data, Content, constraints, deprecated) — enables data-driven prioritization of future implementations; (2) Remove empty Phase 2 (type normalization) and Phase 4 (final fixup) — moved to Future Work; (3) Merge all simple-deletion Phase 1 transforms (deprecated, Core, conditionals, dependentSchemas, remaining constraints) into Phase 2 whitelist filter — Phase 1 now only contains output-producing transforms (const→enum, exclusiveBounds, propertyNames); (4) Compact document: consolidate redundant sections, remove individual code examples for simple deletions, tighten rationale |
| 2.5 | 2026-02-20 | Chason Tang | Simplify Phase 1g: only `propertyNames` retains description hint conversion, all other constraint keywords directly deleted; remove Phase 2a `flattenTypeArrays`; defer Phase 4 entirely |
| 2.4 | 2026-02-20 | Chason Tang | Simplify Phase 1: directly delete `if`/`then`/`else`, `dependentSchemas`, Meta-Data, Content keywords without description hints; remove Enum Hints and additionalProperties Hint (natively supported) |
| 2.3 | 2026-02-20 | Chason Tang | Replace `$ref`/`$defs` inline resolution with direct stripping of all 9 Core vocabulary keywords |
| 2.2 | 2026-02-20 | Chason Tang | Move deprecated keywords to Phase 1a (first step); simplify to direct deletion |
| 2.1 | 2026-02-20 | Chason Tang | Remove `allOf`/`anyOf`/`oneOf` flattening (natively supported); enhance `$ref` resolver |
| 2.0 | 2026-02-20 | Chason Tang | Single-stage pipeline; fix unsupported count; add deprecated keyword handling; add `KNOWN_UNSUPPORTED_KEYWORDS` with logging |
| 1.0 | 2026-02-20 | Chason Tang | Initial version |
