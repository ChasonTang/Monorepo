# RFC-016: ThinkingConfig CamelCase Alignment and Tool Choice Mode Mapping

**Version:** 1.0  
**Author:** Chason Tang  
**Date:** 2026-03-10  
**Status:** Implemented

---

## 1. Summary

This RFC introduces two improvements to `anthropicToGoogle()` in `converter.js`: (1) migrate `thinkingConfig` inner fields from snake_case (`include_thoughts`, `thinking_budget`) to camelCase (`includeThoughts`, `thinkingBudget`) to align with the Google Generative AI API's canonical naming convention, and (2) map the Anthropic `tool_choice` field (`{ type: "auto" | "any" | "none" }`) to the Google `toolConfig` field (`{ functionCallingConfig: { mode: "AUTO" | "ANY" | "NONE" } }`). Unrecognized `tool_choice.type` values are rejected at the AJV validation layer. Both changes are backward-compatible from the client perspective — the Anthropic-facing API is unchanged.

## 2. Motivation

**ThinkingConfig CamelCase:**

The Google Generative AI API consistently uses camelCase naming: `maxOutputTokens`, `topP`, `topK`, `stopSequences`, `thinkingConfig`. The `thinkingConfig` wrapper itself follows this convention, but its inner fields — `include_thoughts` and `thinking_budget` — use snake_case:

```javascript
// converter.js L513–L516 (current)
googleRequest.generationConfig.thinkingConfig = {
    include_thoughts: thinking.type !== 'disabled',
    thinking_budget: resolvedBudget,
};
```

This inconsistency exists because the initial implementation was based on early API documentation that used snake_case for these fields. The API now supports camelCase for `thinkingConfig` inner fields, and all current documentation uses camelCase exclusively. Aligning with the canonical naming convention eliminates the stylistic inconsistency within the generated request and ensures forward compatibility as the snake_case aliases may eventually be deprecated.

**Tool Choice Mapping:**

Claude Code sends `tool_choice` in its requests to control function calling behavior — `{ type: "auto" }` for model-decided tool use, `{ type: "any" }` to force at least one tool call, and `{ type: "none" }` to disable tool use entirely. Currently, `anthropicToGoogle()` does not destructure or process `tool_choice` — the field is silently dropped. RFC-003 §4.6 explicitly noted this as an unvalidated pass-through field:

> `tool_choice` | Optional, object | Not validated (pass-through) | Odin does not process this field

As a result, every request to the upstream API uses the default tool calling mode regardless of the client's intent. A client sending `tool_choice: { type: "none" }` to suppress tool use still receives tool call responses, while a client sending `{ type: "any" }` to force tool use may receive text-only responses. This silent mismatch between client intent and upstream behavior is the primary motivation for adding explicit mapping.

## 3. Goals and Non-Goals

**Goals:**
- Rename `thinkingConfig.include_thoughts` to `includeThoughts` and `thinkingConfig.thinking_budget` to `thinkingBudget` in all generated Google API requests.
- Add `tool_choice` to the AJV validation schema — only `{ type: "auto" | "any" | "none" }` is accepted; any other value is rejected with a clear error message before reaching the converter.
- Map `tool_choice.type` values `auto`, `any`, and `none` to `toolConfig.functionCallingConfig.mode` values `AUTO`, `ANY`, and `NONE` respectively.
- Only emit `toolConfig` when `tool_choice` is present in the incoming request — absent `tool_choice` preserves the current behavior of omitting `toolConfig` entirely.

**Non-Goals:**
- Supporting Anthropic's `tool_choice: { type: "tool", name: "<specific_tool>" }` with `allowedFunctionNames` — the Google `functionCallingConfig` does support `ALLOWED_FUNCTION_NAMES` mode, but Claude Code's usage of specific tool forcing is rare and can be addressed in a future RFC if demand materializes.
- Mapping `tool_choice.disable_parallel_tool_use` — the Anthropic API supports this boolean field to prevent parallel tool calls, but the Google `functionCallingConfig` has no direct equivalent. This can be addressed in a future RFC if parallel tool call control becomes necessary.

## 4. Design

### 4.1 Overview

The change touches two files: `validator.js` (AJV schema) and `converter.js` (converter logic). In `validator.js`, `tool_choice` is added to the AJV schema to validate that `type` is one of `auto`, `any`, or `none`. In `converter.js`, the `anthropicToGoogle()` function is modified in two places: (1) the `thinkingConfig` object literal is updated to use camelCase field names, and (2) `tool_choice` is added to the destructuring and mapped to a new `toolConfig` top-level field in the Google request.

```
┌─────────────────────────────────────────────────────────────────────────┐
│  Anthropic Request                                                      │
│                                                                         │
│  ┌─────────────────┐  ┌──────────────┐  ┌──────────────┐                │
│  │ thinking        │  │ tool_choice  │  │ tools        │                │
│  │  .type          │  │  .type       │  │ [...]        │                │
│  │  .budget_tokens │  │ auto | any | │  │              │                │
│  │                 │  │ none         │  │              │                │
│  └──────┬──────────┘  └──────┬───────┘  └──────┬───────┘                │
└─────────┼────────────────────┼─────────────────┼────────────────────────┘
          │                    │                 │
          ▼                    ▼                 ▼
┌──────────────────────────────────────────────────────────────────────┐
│  anthropicToGoogle() — converter.js                                  │
│                                                                      │
│  ┌───────────────────────────┐  ┌──────────────────────────────────┐ │
│  │ thinkingConfig (camelCase)│  │ toolConfig (new)                 │ │
│  │  includeThoughts: bool    │  │  functionCallingConfig:          │ │
│  │  thinkingBudget: number   │  │    mode: AUTO | ANY | NONE       │ │
│  └───────────────────────────┘  └──────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────────────┘
          │                    │                 │
          ▼                    ▼                 ▼
┌──────────────────────────────────────────────────────────────────────┐
│  Google Request                                                      │
│                                                                      │
│  generationConfig:                  toolConfig:                      │
│    thinkingConfig:                    functionCallingConfig:         │
│      includeThoughts: true              mode: "AUTO"                 │
│      thinkingBudget: 8192                                            │
│    maxOutputTokens: ...             tools: [...]                     │
└──────────────────────────────────────────────────────────────────────┘
```

### 4.2 Detailed Design

#### 4.2.1 ThinkingConfig CamelCase Migration (`converter.js`)

Rename the two snake_case fields in the `thinkingConfig` object literal to camelCase. No other code changes are needed — these field names only appear in this single object literal.

**Before (L513–L516):**
```javascript
googleRequest.generationConfig.thinkingConfig = {
    include_thoughts: thinking.type !== 'disabled',
    thinking_budget: resolvedBudget,
};
```

**After:**
```javascript
googleRequest.generationConfig.thinkingConfig = {
    includeThoughts: thinking.type !== 'disabled',
    thinkingBudget: resolvedBudget,
};
```

#### 4.2.2 ToolChoice-to-ToolConfig Mapping (`converter.js`)

**Step 1 — Add `tool_choice` to AJV schema (`validator.js`):**

Add `tool_choice` as an optional validated field alongside `tools` in `messagesRequestSchema.properties`:

```javascript
tool_choice: {
    type: 'object',
    required: ['type'],
    properties: {
        type: {
            type: 'string',
            enum: ['auto', 'any', 'none'],
        },
    },
},
```

Requests with `tool_choice.type` values outside `auto`, `any`, `none` (e.g., `{ type: "tool" }`) are rejected with a validation error before reaching the converter.

**Step 2 — Add `tool_choice` to destructuring (L452–L463):**

> Note: Destructuring shown in compressed format for brevity. Source code uses one field per line.

**Before:**
```javascript
const {
    messages, system, max_tokens, temperature,
    top_p, top_k, stop_sequences, tools, thinking, output_config,
} = anthropicRequest;
```

**After:**
```javascript
const {
    messages, system, max_tokens, temperature,
    top_p, top_k, stop_sequences, tools, tool_choice, thinking, output_config,
} = anthropicRequest;
```

**Step 3 — Add `toolConfig` mapping (after the tools block, ~L534):**

Since AJV guarantees `tool_choice.type` is always one of `auto`, `any`, `none`, the converter simply uppercases the value without fallback logic:

```javascript
if (tool_choice) {
    googleRequest.toolConfig = {
        functionCallingConfig: {
            mode: tool_choice.type.toUpperCase(),
        },
    };
}
```

The mapping logic:

| Anthropic `tool_choice.type` | Google `functionCallingConfig.mode` | Notes |
|---|---|---|
| `"auto"` | `"AUTO"` | Model decides whether to call tools |
| `"any"` | `"ANY"` | Model must call at least one tool |
| `"none"` | `"NONE"` | Model must not call any tools |
| (absent) | (no `toolConfig` emitted) | Upstream API uses default behavior |

### 4.3 Design Rationale

**Why camelCase now?**

The Google Generative AI API's canonical field naming is camelCase. Every other field in the generated request — `maxOutputTokens`, `topP`, `topK`, `stopSequences` — already follows this convention. The `thinkingConfig` inner fields were the sole exception, an artifact of early API documentation. The API now accepts both conventions, but camelCase is the documented canonical form. Aligning now is a low-risk change that prevents potential breakage if the snake_case aliases are deprecated.

**Why validate `tool_choice.type` via AJV instead of using a permissive fallback?**

1. **Fail-fast principle** — Invalid `tool_choice.type` values (e.g., `{ type: "tool", name: "..." }` which cannot be accurately mapped) are rejected at the validation layer with a clear error message. This is preferable to silently degrading to `AUTO`, which may produce behavior that diverges from the client's intent without any indication of the mismatch.
2. **Explicit over implicit** — A permissive fallback hides configuration errors. If a client sends an unsupported `tool_choice.type`, it should know immediately rather than observing subtly incorrect tool calling behavior that is difficult to diagnose.
3. **Scope alignment** — Only three `tool_choice.type` values (`auto`, `any`, `none`) have direct semantic equivalents in the Google `functionCallingConfig.mode`. Accepting and silently remapping values that lack a direct equivalent violates the principle of least surprise.

**Why `toolConfig` is only emitted when `tool_choice` is present?**

Omitting `toolConfig` entirely when `tool_choice` is absent preserves the upstream API's default behavior. Explicitly sending `toolConfig: { functionCallingConfig: { mode: "AUTO" } }` would be semantically equivalent for current API versions, but omission is safer against future API changes where the default mode might differ from `AUTO`.

**Why `toolConfig` may be emitted without `tools`?**

When `tool_choice` is present but `tools` is absent or empty, the converter still emits `toolConfig`. This is an intentional simplification — adding an extra existence check for `tools` would increase code complexity without practical benefit, as the upstream API ignores `toolConfig` when no `tools` are declared.

## 5. Implementation Plan

### Phase 1: Core Implementation — 0.5 hours

- [x] Add `tool_choice` to AJV schema in `validator.js` with `enum: ['auto', 'any', 'none']`
- [x] Rename `include_thoughts` → `includeThoughts` in `converter.js` thinkingConfig object literal
- [x] Rename `thinking_budget` → `thinkingBudget` in `converter.js` thinkingConfig object literal
- [x] Add `tool_choice` to destructuring in `anthropicToGoogle()`
- [x] Add `toolConfig` mapping block after the tools block in `anthropicToGoogle()`

**Done when:** AJV rejects invalid `tool_choice.type` values, `anthropicToGoogle()` outputs camelCase `thinkingConfig` fields, and correctly maps `tool_choice` to `toolConfig` for all three recognized values.

### Phase 2: Test Updates — 0.5 hours

- [x] Update all existing thinkingConfig test assertions from `include_thoughts` → `includeThoughts` and `thinking_budget` → `thinkingBudget`
- [x] Add converter test: `tool_choice: { type: "auto" }` → `AUTO`
- [x] Add converter test: `tool_choice: { type: "any" }` → `ANY`
- [x] Add converter test: `tool_choice: { type: "none" }` → `NONE`
- [x] Add converter test: absent `tool_choice` → no `toolConfig`
- [x] Add converter test: `tool_choice` with `tools` coexistence — both `toolConfig` and `tools` independently present
- [x] Add validator test: `tool_choice: { type: "tool" }` → AJV rejection

**Done when:** All test scenarios in §6 pass.

## 6. Testing Strategy

Unit tests cover two areas: thinkingConfig camelCase verification (§6.1) and toolChoice mapping (§6.2).

### 6.1 ThinkingConfig CamelCase

Updates to existing tests in `effort-budget-converter.test.js` — the test logic is unchanged, only the expected field names change.

| # | Scenario | Before | After |
|---|----------|--------|-------|
| 1 | All assertions on `include_thoughts` | `include_thoughts: true/false` | `includeThoughts: true/false` |
| 2 | All assertions on `thinking_budget` | `thinking_budget: <value>` | `thinkingBudget: <value>` |

### 6.2 ToolChoice Mapping

New tests to be added in a dedicated test file or appended to existing converter tests.

| # | Scenario | Input | Expected Behavior |
|---|----------|-------|-------------------|
| 1 | tool_choice auto | `tool_choice: { type: "auto" }` | `toolConfig: { functionCallingConfig: { mode: "AUTO" } }` |
| 2 | tool_choice any | `tool_choice: { type: "any" }` | `toolConfig: { functionCallingConfig: { mode: "ANY" } }` |
| 3 | tool_choice none | `tool_choice: { type: "none" }` | `toolConfig: { functionCallingConfig: { mode: "NONE" } }` |
| 4 | Absent tool_choice | No `tool_choice` in request | `toolConfig` is `undefined` on Google request |
| 5 | tool_choice with tools coexistence | `tool_choice: { type: "any" }` + `tools: [...]` | Both `toolConfig` and `tools` independently present on Google request |
| 6 | Unrecognized type (AJV rejection) | `tool_choice: { type: "tool" }` | AJV validation fails with error message |

## 7. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Google API deprecates snake_case `thinkingConfig` field names before all Odin instances are updated | Low | Med | The API currently accepts both conventions. This RFC's change is the exact fix needed — deploy promptly after approval. |
| `tool_choice: { type: "tool", name: "..." }` is frequently used by Claude Code and AJV rejection disrupts workflow | Low | Med | Monitor AJV rejection logs for `tool_choice` validation errors. If `tool` type is common, implement `allowedFunctionNames` mapping in a follow-up RFC. |
| Upstream Google API changes `functionCallingConfig.mode` enum values (e.g., `REQUIRED` replacing `ANY`) | Very Low | High | The converter uses `toUpperCase()` for a direct case transform — it does not perform value remapping. If the upstream enum changes, the converter will silently produce invalid mode values. Review the mapping table when upgrading the upstream API version. |

## 8. Future Work

- **`tool_choice: { type: "tool", name: "..." }` support** — map to `functionCallingConfig: { mode: "ANY", allowedFunctionNames: ["<name>"] }` to force a specific tool. Deferred because Claude Code rarely uses specific tool forcing. When implemented, the AJV schema should be extended to accept `tool` as a valid `tool_choice.type` value with a required `name` field.
- **`tool_choice.disable_parallel_tool_use` support** — the Anthropic API supports this boolean field to prevent the model from making multiple tool calls in a single response. The Google `functionCallingConfig` has no direct equivalent. If parallel tool call control becomes necessary, a future RFC should investigate whether Antigravity offers an equivalent configuration or whether a workaround (e.g., system instruction hint) is feasible.
- **snake_case deprecation tracking** — once all Odin instances are confirmed to use camelCase thinkingConfig fields, document the convention alignment and monitor for upstream deprecation announcements.

## 9. References

- `odin/src/converter.js` — `anthropicToGoogle()` function (L451–L537)
- `odin/src/validator.js` — `messagesRequestSchema` and `validateMessagesRequest()` (L14–L345)
- `odin/src/constants.js` — `CLAUDE_THINKING_MAX_OUTPUT_TOKENS` (L14)
- `odin/tests/effort-budget-converter.test.js` — existing thinkingConfig tests
- [Anthropic Messages API — tool_choice parameter](https://docs.anthropic.com/en/api/messages/create)
- [Google Generative AI — ToolConfig](https://ai.google.dev/api/generate-content#ToolConfig)
- RFC-003: AJV Request Validation — §4.6 noted `tool_choice` as an unvalidated pass-through field
- RFC-015: Effort-Based Thinking Budget Resolution — introduced current `thinkingConfig` generation logic

---

## Changelog

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2026-03-10 | Chason Tang | Initial version |
