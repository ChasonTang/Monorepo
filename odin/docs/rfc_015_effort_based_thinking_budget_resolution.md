# RFC-015: Effort-Based Thinking Budget Resolution

**Version:** 1.2  
**Author:** Chason Tang  
**Date:** 2026-03-06  
**Status:** Implemented

---

## 1. Summary

Claude Code now sends an optional `output_config.effort` field (`low` | `medium` | `high`) alongside adaptive thinking requests. This RFC adds effort-to-thinking-budget resolution to `anthropicToGoogle()`: a lookup table maps effort levels to concrete `thinking_budget` values (low → 2048, medium → 8192, high → 16384). Effort is strictly scoped to adaptive thinking — cross-field validation rejects four conflicting configurations: effort without a thinking block, effort with disabled thinking, effort co-occurring with explicit `budget_tokens` (enabled thinking), and adaptive thinking without effort (no budget guidance). The resolved budget is validated against `maxOutputTokens` using the existing constraint enforcement logic.

## 2. Motivation

The current `anthropicToGoogle()` converter handles thinking configuration in two modes:

1. **`enabled`**: `thinking.budget_tokens` is required and directly assigned to `thinkingConfig.thinking_budget`.
2. **`adaptive`**: no `budget_tokens` is provided; `thinking_budget` is `undefined`, delegating the decision entirely to the upstream API.

```javascript
// converter.js L504–L516 (current)
if (thinking) {
    const thinkingBudget = thinking.budget_tokens;
    googleRequest.generationConfig.thinkingConfig = {
        include_thoughts: thinking.type !== 'disabled',
        thinking_budget: thinkingBudget,
    };

    if (thinkingBudget !== undefined && max_tokens <= thinkingBudget) {
        googleRequest.generationConfig.maxOutputTokens = CLAUDE_THINKING_MAX_OUTPUT_TOKENS;
    }
}
```

Newer versions of Claude Code introduce `output_config.effort` — a coarse-grained hint that controls thinking intensity during adaptive mode. Without explicit handling, Odin silently drops this field, and the upstream API receives no thinking budget guidance for adaptive requests that carry effort hints. This leads to two problems:

- A user setting `effort: 'low'` still gets the API's default thinking budget (potentially high), wasting compute and increasing latency for tasks that do not require deep reasoning.
- A user setting `effort: 'high'` might get a lower budget than intended if the API's default is conservative, resulting in shallower thinking for complex tasks.

By mapping effort levels to concrete thinking budgets, Odin translates the semantic intent ("think less" / "think more") into the numeric `thinking_budget` parameter that the upstream API actually respects.

## 3. Goals and Non-Goals

**Goals:**
- Map `output_config.effort` values to concrete `thinking_budget` values via a lookup table in `constants.js`.
- Strictly scope effort to adaptive thinking: reject requests where effort conflicts with the thinking configuration via cross-field validation (effort without thinking, effort with disabled thinking, effort with explicit `budget_tokens`, and adaptive thinking without effort).
- Ensure the resolved budget satisfies the `maxOutputTokens > thinking_budget` API constraint via the existing enforcement path.
- Validate `output_config` in the request schema to reject malformed effort values early.

**Non-Goals:**
- Dynamically adjusting effort budgets based on model or conversation context — fixed lookup values are sufficient for the current use case and can be tuned via constant changes without code logic modification.
- Applying effort to non-thinking generation parameters (e.g., temperature, top_p) — effort currently only affects thinking budget.

## 4. Design

### 4.1 Overview

The change touches three files: `constants.js` (lookup table), `converter.js` (resolution logic), and `validator.js` (schema + cross-field validation).

```
┌──────────────────────────────────────────────────────────────────────┐
│  Claude Code Request                                                 │
│  ┌─────────────────────────┐  ┌────────────────────────────────┐     │
│  │ thinking                │  │ output_config.effort           │     │
│  │   .type                 │  │ ('low' | 'medium' | 'high')    │     │
│  │   .budget_tokens        │  │ (optional)                     │     │
│  └───────────┬─────────────┘  └──────────────┬─────────────────┘     │
└──────────────┼───────────────────────────────┼───────────────────────┘
               │                               │
               ▼                               ▼
┌──────────────────────────────────────────────────────────────────────┐
│  Cross-Field Validation (validator.js)                               │
│                                                                      │
│  REJECT if effort is present AND:                                    │
│    ✗ no thinking block                                               │
│    ✗ thinking.type === 'disabled'                                    │
│    ✗ thinking.budget_tokens is present (type === 'enabled')          │
│  REJECT if effort is absent AND:                                     │
│    ✗ thinking.type === 'adaptive' (no budget guidance)               │
│                                                                      │
│  PASS only:                                                          │
│    ✓ effort + adaptive thinking                                      │
│    ✓ enabled thinking + budget_tokens (no effort)                    │
│    ✓ disabled thinking (no effort)                                   │
└──────────────────────────┬───────────────────────────────────────────┘
                           │
                           ▼
┌──────────────────────────────────────────────────────────────────────┐
│  Effort Budget Resolver (converter.js)                               │
│                                                                      │
│  1. resolvedBudget = EFFORT_THINKING_BUDGET_MAP[effort]              │
│     (no conflict resolution needed — mutually exclusive by design)   │
│  2. if resolvedBudget ≥ maxOutputTokens → bump to 64000              │
└──────────────────────────┬───────────────────────────────────────────┘
                           │
                           ▼
┌──────────────────────────────────────────────────────────────────────┐
│  Google generationConfig                                             │
│  ┌────────────────────────────┐  ┌───────────────────────────────┐   │
│  │ thinkingConfig:            │  │ maxOutputTokens:              │   │
│  │   include_thoughts: true   │  │   max(max_tokens, 64000)      │   │
│  │   thinking_budget: resolved│  │   (bumped only if needed)     │   │
│  └────────────────────────────┘  └───────────────────────────────┘   │
└──────────────────────────────────────────────────────────────────────┘
```

### 4.2 Detailed Design

#### 4.2.1 Effort-to-Budget Lookup Table (`constants.js`)

Add a constant mapping effort levels to thinking budget values:

```javascript
export const EFFORT_THINKING_BUDGET_MAP = {
    low: 2048,
    medium: 8192,
    high: 16384,
};
```

The values follow a roughly 4× geometric progression:
- **low (2048)**: Minimal thinking — fast responses for straightforward tasks.
- **medium (8192)**: Moderate thinking — balanced quality/latency for typical tasks.
- **high (16384)**: Extensive thinking — thorough reasoning for complex tasks.

All values are well below `CLAUDE_THINKING_MAX_OUTPUT_TOKENS` (64000), leaving ample room for non-thinking output tokens.

#### 4.2.2 Request Destructuring (`converter.js`)

Extract `output_config` alongside existing fields in `anthropicToGoogle()`:

**Before (L452–L462):**
```javascript
const {
    messages, system, max_tokens, temperature,
    top_p, top_k, stop_sequences, tools, thinking,
} = anthropicRequest;
```

**After:**
```javascript
const {
    messages, system, max_tokens, temperature,
    top_p, top_k, stop_sequences, tools, thinking, output_config,
} = anthropicRequest;
```

The import statement also adds the new constant:

**Before (L5):**
```javascript
import { CLAUDE_THINKING_MAX_OUTPUT_TOKENS } from './constants.js';
```

**After:**
```javascript
import { CLAUDE_THINKING_MAX_OUTPUT_TOKENS, EFFORT_THINKING_BUDGET_MAP } from './constants.js';
```

#### 4.2.3 Thinking Budget Resolution (`converter.js`)

Replace the current thinking config block with effort-aware resolution. Cross-field validation (§4.2.4) guarantees that `effort` and `budget_tokens` are mutually exclusive by the time the converter runs, so no `Math.max` conflict resolution is needed.

**Before (L504–L516):**
```javascript
if (thinking) {
    const thinkingBudget = thinking.budget_tokens;
    googleRequest.generationConfig.thinkingConfig = {
        include_thoughts: thinking.type !== 'disabled',
        thinking_budget: thinkingBudget,
    };

    if (thinkingBudget !== undefined && max_tokens <= thinkingBudget) {
        googleRequest.generationConfig.maxOutputTokens = CLAUDE_THINKING_MAX_OUTPUT_TOKENS;
    }
}
```

**After:**
```javascript
if (thinking) {
    let resolvedBudget = thinking.budget_tokens;

    if (output_config?.effort) {
        resolvedBudget = EFFORT_THINKING_BUDGET_MAP[output_config.effort];
    }

    googleRequest.generationConfig.thinkingConfig = {
        include_thoughts: thinking.type !== 'disabled',
        thinking_budget: resolvedBudget,
    };

    if (resolvedBudget !== undefined && max_tokens <= resolvedBudget) {
        googleRequest.generationConfig.maxOutputTokens = CLAUDE_THINKING_MAX_OUTPUT_TOKENS;
    }
}
```

The converter logic is straightforward because validation has already eliminated all ambiguous states. Only three valid combinations reach this code — every path produces a concrete `resolvedBudget` (no `undefined` thinking budgets):

| `thinking.type` | `effort` | `budget_tokens` | `resolvedBudget` source |
|---|---|---|---|
| `adaptive` | present | absent | Lookup table |
| `enabled` | absent | present | `budget_tokens` directly |
| `disabled` | absent | absent | `undefined` (`include_thoughts: false`) |

#### 4.2.4 Schema and Cross-Field Validation (`validator.js`)

**Part A — AJV Schema**

Add `output_config` as an optional property in `messagesRequestSchema.properties`:

```javascript
output_config: {
    type: 'object',
    properties: {
        effort: {
            type: 'string',
            enum: ['low', 'medium', 'high'],
        },
    },
},
```

`output_config` is not added to the top-level `required` array — it is entirely optional. Unknown properties within `output_config` are permitted (no `additionalProperties: false`) for forward compatibility with future fields.

**Part B — Cross-Field Validation**

AJV schema validation handles structural and type checks, but cannot enforce semantic constraints that span multiple top-level fields. Add a `validateCrossFieldConstraints()` function that runs after AJV passes, checking four rules:

```javascript
function validateCrossFieldConstraints(body) {
    const effort = body.output_config?.effort;

    if (effort) {
        if (!body.thinking) {
            return '"output_config.effort" requires a "thinking" block. '
                 + 'Set thinking.type to "adaptive" to use effort-based thinking budget.';
        }

        if (body.thinking.type === 'disabled') {
            return '"output_config.effort" cannot be used with thinking.type "disabled". '
                 + 'Either remove output_config.effort or set thinking.type to "adaptive".';
        }

        if (body.thinking.budget_tokens !== undefined) {
            return '"output_config.effort" and "thinking.budget_tokens" are mutually exclusive. '
                 + 'Use effort for automatic budget selection, '
                 + 'or budget_tokens for explicit control, but not both.';
        }
    }

    if (body.thinking?.type === 'adaptive' && !effort) {
        return 'thinking.type "adaptive" requires "output_config.effort" to specify thinking intensity. '
             + 'Add output_config: { effort: "low" | "medium" | "high" } to your request.';
    }

    return null;
}
```

Update `validateMessagesRequest()` to invoke cross-field validation after AJV:

**Before:**
```javascript
export function validateMessagesRequest(body) {
    if (validate(body)) {
        return { valid: true };
    }

    const message = formatErrors(validate.errors);

    return { valid: false, message };
}
```

**After:**
```javascript
export function validateMessagesRequest(body) {
    if (!validate(body)) {
        const message = formatErrors(validate.errors);

        return { valid: false, message };
    }

    const crossFieldError = validateCrossFieldConstraints(body);
    if (crossFieldError) {
        return { valid: false, message: crossFieldError };
    }

    return { valid: true };
}
```

This two-phase approach keeps the schema declarative (AJV handles structure) and the cross-field rules imperative (plain JS handles semantics). Each error message is specific and actionable — it tells the caller exactly what is wrong and how to fix it.

### 4.3 Design Rationale

**Why a static lookup table instead of a formula?**

A formula (e.g., `effort_index * base_value`) introduces coupling between ordinal position and budget values. A lookup table is explicit, independently tunable per level, and trivially extensible if new effort levels are added. The three values (2048, 8192, 16384) follow a roughly 4× geometric progression that maps well to the semantic distinction between "minimal thinking," "moderate thinking," and "extensive thinking."

**Why strict rejection instead of permissive `Math.max` conflict resolution?**

An earlier design (v1.0) used `Math.max(effortBudget, budget_tokens)` to silently resolve conflicts when both were present. This was replaced with strict validation for three reasons:

1. **Semantic clarity** — `effort` and `budget_tokens` express the same intent (thinking budget) at different abstraction levels. Allowing both creates ambiguity: if a client sends `budget_tokens: 4000` with `effort: 'high'`, did they intend 4000 or 16384? Silent resolution guesses; strict rejection asks.
2. **Simpler converter** — mutual exclusion eliminates the `Math.max` branch and the `|| 0` fallback for `undefined` budget_tokens. The converter becomes a straight lookup or passthrough with no conflict resolution code path.
3. **Fail-fast for client bugs** — a client sending both fields likely has a configuration error (e.g., migrating from `budget_tokens` to `effort` but forgot to remove the old field). Reporting the error immediately is more helpful than silently producing unexpected budget values.

**Why reject effort with disabled thinking or absent thinking block?**

Effort is a thinking intensity hint. Sending it without an active thinking configuration is a semantic contradiction — it signals "think at this intensity" while simultaneously saying "don't think" (disabled) or saying nothing about thinking at all (absent). Rejecting these cases at validation time surfaces client-side misconfiguration immediately, with actionable error messages that explain how to fix the request.

**Why reject adaptive thinking without effort?**

Adaptive thinking without `effort` produces `thinking_budget: undefined`, delegating the budget decision entirely to the upstream API with no guidance from the client. This is an underspecified state: the client has opted into thinking but provided no signal about desired intensity. Rather than silently accepting a request where the thinking behavior is unpredictable, strict rejection forces the client to declare intent. This also eliminates the only remaining `undefined` budget path for active thinking in the converter, ensuring every adaptive request produces a concrete, deterministic `thinking_budget` value.

**Why two-phase validation (AJV + imperative cross-field)?**

AJV excels at structural validation (types, enums, required fields) but expressing cross-field constraints like "if `output_config.effort` is present then `thinking.type` must be `adaptive`" requires `if/then/else` keywords that produce verbose, hard-to-read schemas. A post-AJV imperative function is more readable, produces clear error messages, and is trivially extensible for future cross-field rules.

## 5. Implementation Plan

### Phase 1: Core Implementation — 1 hour

- [x] Add `EFFORT_THINKING_BUDGET_MAP` constant to `constants.js`
- [x] Import `EFFORT_THINKING_BUDGET_MAP` in `converter.js`
- [x] Add `output_config` to destructuring in `anthropicToGoogle()`
- [x] Replace thinking budget assignment with effort-aware resolution logic (straight lookup, no `Math.max`)
- [x] Add `output_config` AJV schema to `validator.js`
- [x] Add `validateCrossFieldConstraints()` function to `validator.js`
- [x] Update `validateMessagesRequest()` to invoke cross-field validation after AJV

**Done when:** `anthropicToGoogle()` correctly resolves thinking budget from effort for adaptive thinking. Validator accepts `adaptive + effort`, rejects `effort + no thinking`, `effort + disabled`, `effort + budget_tokens`, and `adaptive + no effort`.

### Phase 2: Test Coverage — 0.5 hours

- [x] Add converter unit tests for adaptive + effort (all three levels)
- [x] Add converter unit test for enabled + budget_tokens only (unchanged behavior)
- [x] Add converter unit test for `maxOutputTokens` bump when effort budget exceeds `max_tokens`
- [x] Add validator tests for the four cross-field rejection cases
- [x] Add validator tests for valid `adaptive + effort` payloads
- [x] Add validator test for invalid effort enum values

**Done when:** All test scenarios in §6 pass.

## 6. Testing Strategy

Unit tests cover two layers: validator cross-field rejection (§6.1) and converter budget resolution (§6.2).

### 6.1 Validator — Cross-Field Rejection

| # | Scenario | Input | Expected Behavior |
|---|----------|-------|-------------------|
| 1 | Effort + no thinking block | `output_config: { effort: 'high' }`, no `thinking` | **Rejected**: `"output_config.effort" requires a "thinking" block...` |
| 2 | Effort + disabled thinking | `thinking: { type: 'disabled' }`, `effort: 'high'` | **Rejected**: `"output_config.effort" cannot be used with thinking.type "disabled"...` |
| 3 | Effort + enabled (budget_tokens present) | `thinking: { type: 'enabled', budget_tokens: 10000 }`, `effort: 'high'` | **Rejected**: `"output_config.effort" and "thinking.budget_tokens" are mutually exclusive...` |
| 4 | Adaptive without effort | `thinking: { type: 'adaptive' }`, no `output_config` | **Rejected**: `thinking.type "adaptive" requires "output_config.effort"...` |
| 5 | Effort + adaptive (valid) | `thinking: { type: 'adaptive' }`, `effort: 'medium'` | **Accepted** |
| 6 | Enabled + budget_tokens, no effort | `thinking: { type: 'enabled', budget_tokens: 10000 }` | **Accepted** (unchanged behavior) |
| 7 | Disabled, no effort | `thinking: { type: 'disabled' }` | **Accepted** (unchanged behavior) |
| 8 | Invalid effort enum value | `output_config: { effort: 'ultra' }` | **Rejected** by AJV enum validation |

### 6.2 Converter — Budget Resolution (post-validation)

| # | Scenario | Input | Expected Behavior |
|---|----------|-------|-------------------|
| 1 | Adaptive + effort low | `thinking: { type: 'adaptive' }`, `effort: 'low'` | `thinkingConfig: { include_thoughts: true, thinking_budget: 2048 }` |
| 2 | Adaptive + effort medium | `thinking: { type: 'adaptive' }`, `effort: 'medium'` | `thinkingConfig: { include_thoughts: true, thinking_budget: 8192 }` |
| 3 | Adaptive + effort high | `thinking: { type: 'adaptive' }`, `effort: 'high'` | `thinkingConfig: { include_thoughts: true, thinking_budget: 16384 }` |
| 4 | Enabled + budget_tokens only | `thinking: { type: 'enabled', budget_tokens: 10000 }` | `thinking_budget: 10000` (unchanged) |
| 5 | Disabled thinking | `thinking: { type: 'disabled' }` | `include_thoughts: false`, `thinking_budget: undefined` (unchanged) |
| 6 | Effort budget ≥ max_tokens | `max_tokens: 1024`, `effort: 'medium'` (8192) | `maxOutputTokens` bumped to `CLAUDE_THINKING_MAX_OUTPUT_TOKENS` (64000) |

## 7. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Effort budget values (2048/8192/16384) may not align with optimal model behavior | Med | Low | Values are conservative (well below the 64000 cap) and can be tuned via constant changes in `constants.js` without code logic modification. Monitor model output quality per effort level after deployment. |
| Claude Code sends unknown effort values (e.g., `'very_high'`) | Low | Low | Schema validation rejects unknown values at request time with a clear error message. Adding a new value requires a one-line addition to both the lookup table and the validator enum. |
| Claude Code sends both `budget_tokens` and `effort` during migration period | Med | Med | Strict rejection returns a clear, actionable error message explaining mutual exclusivity. Claude Code developers can fix the client-side configuration immediately. If this proves too disruptive during rollout, the cross-field check can be temporarily relaxed to a logged warning (fallback to `budget_tokens`) without changing the converter logic. |
| `output_config` gains additional fields beyond `effort` that affect generation | Low | Low | The converter only reads `effort`; unknown fields within `output_config` are ignored. Future fields can be handled in separate RFCs. |

## 8. Future Work

- **Dynamic effort budget tuning** — instead of fixed lookup values, derive budgets from model-specific limits or empirical quality benchmarks. Deferred because fixed values provide predictable behavior and are easier to debug.
- **Effort → temperature/top_p mapping** — effort could influence other generation parameters beyond thinking budget. Deferred because Claude Code currently only uses effort for thinking intensity.
- **Effort level telemetry** — track the distribution of effort levels in production requests to inform budget value tuning and detect shifts in Claude Code's usage patterns.

## 9. References

- `odin/src/converter.js` — `anthropicToGoogle()` function (L451–L532)
- `odin/src/validator.js` — `messagesRequestSchema` (L14–L185)
- `odin/src/constants.js` — `CLAUDE_THINKING_MAX_OUTPUT_TOKENS` (L14)
- [Anthropic Messages API — thinking parameter](https://docs.anthropic.com/en/api/messages/create)
- [Anthropic Messages API — output configuration](https://docs.anthropic.com/en/api/messages/create)

---

## Changelog

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.2 | 2026-03-06 | Chason Tang | Reject adaptive thinking without effort — the fourth cross-field rule eliminates the `undefined` budget path, ensuring every adaptive request produces a concrete `thinking_budget` |
| 1.1 | 2026-03-06 | Chason Tang | Replace permissive `Math.max` conflict resolution with strict cross-field validation — effort is now exclusive to adaptive thinking; effort + disabled, effort + no thinking, and effort + `budget_tokens` are rejected at validation time |
| 1.0 | 2026-03-06 | Chason Tang | Initial version |
