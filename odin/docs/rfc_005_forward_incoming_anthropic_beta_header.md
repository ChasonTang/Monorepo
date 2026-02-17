# RFC-005: Forward Incoming `anthropic-beta` Header in buildHeaders

**Version:** 2.0  
**Author:** Chason Tang  
**Date:** 2026-02-17  
**Status:** Implemented

---

## 1. Summary

Odin's `buildHeaders()` function constructs all outgoing Cloud Code API headers from scratch, completely ignoring the HTTP headers sent by Claude Code. For the `anthropic-beta` header, the function redundantly re-derives the value using `isThinkingModel()`, rather than forwarding what Claude Code already provides. This RFC proposes replacing the manual `anthropic-beta` injection logic with a simple passthrough of the incoming request's `anthropic-beta` header. Claude Code — not Odin — is the authority on which beta features to request; Odin should act as a transparent conduit, not re-derive what the client has already determined.

## 2. Motivation

### 2.1 Silent Beta Flag Stripping

The `buildHeaders()` function in `constants.js` (lines 37–56) builds the outgoing Cloud Code API headers entirely from hardcoded values:

```javascript
export function buildHeaders(apiKey, model) {
    const headers = {
        Authorization: `Bearer ${apiKey}`,
        'Content-Type': 'application/json',
        Accept: 'text/event-stream',
        'User-Agent': `antigravity/1.15.8 ${platform()}/${arch()}`,
        'X-Goog-Api-Client': 'google-cloud-sdk vscode_cloudshelleditor/0.1',
        'Client-Metadata': JSON.stringify({
            ideType: 'ANTIGRAVITY',
            platform: 'MACOS',
            pluginType: 'GEMINI',
        }),
    };

    if (isThinkingModel(model)) {
        headers['anthropic-beta'] = 'interleaved-thinking-2025-05-14';
    }

    return headers;
}
```

The function is called from `sendRequest()` in `cloudcode.js` (line 39):

```javascript
export async function sendRequest(googleRequest, model, apiKey, debug) {
    const headers = buildHeaders(apiKey, model);
    // ...
}
```

Neither function receives any reference to the incoming HTTP request. When Claude Code sends a request like:

```
POST /v1/messages HTTP/1.1
anthropic-beta: output-128k-2025-02-19,interleaved-thinking-2025-05-14
```

The `output-128k-2025-02-19` flag is silently discarded. The outgoing request to Cloud Code only contains `interleaved-thinking-2025-05-14` (if the model is a thinking model) or no `anthropic-beta` header at all.

### 2.2 Odin Should Not Re-derive What Claude Code Already Provides

The current `buildHeaders()` implementation contains a model-conditional check that attempts to re-derive the `anthropic-beta` value:

```javascript
if (isThinkingModel(model)) {
    headers['anthropic-beta'] = 'interleaved-thinking-2025-05-14';
}
```

This logic is **misplaced**. To understand why, consider the two proxy implementations and their respective upstream clients:

| Proxy | Upstream Client | Client adds `anthropic-beta`? | Proxy's responsibility |
|-------|----------------|-------------------------------|----------------------|
| `opencode-antigravity-auth` | OpenCode | **No** — OpenCode targets the Google Generative Language API and does not set Anthropic-specific headers | Must inject `interleaved-thinking-2025-05-14` for thinking models, since the client does not |
| **Odin** | **Claude Code** | **Yes** — Claude Code natively manages `anthropic-beta` as part of its Anthropic API integration | Should forward the client's header as-is, not re-derive it |

The `opencode-antigravity-auth` proxy legitimately adds the `interleaved-thinking` header because its upstream client (OpenCode) is unaware of this Anthropic-specific mechanism. The merge logic in `prepareAntigravityRequest()` (request.ts lines 1380–1393) exists to fill a gap that the client cannot fill.

Odin's upstream client is fundamentally different. Claude Code is purpose-built for the Anthropic API and manages its own `anthropic-beta` header with full awareness of which beta features to enable. When Odin re-derives this header:

1. **It overrides the client's intent** — Claude Code may have specific reasons for including or excluding certain beta flags. Odin's hardcoded `isThinkingModel()` check replaces the client's nuanced decision with a crude heuristic.
2. **It drops flags it doesn't know about** — Claude Code may send beta flags that Odin has never heard of (e.g., `output-128k-2025-02-19`). Since `buildHeaders()` starts from scratch, all unknown flags are silently discarded.
3. **It duplicates logic that belongs to the client** — The `isThinkingModel()` check in Odin is a redundant mirror of logic that Claude Code already implements. Maintaining this mirror creates an ongoing synchronization burden with no benefit.

The correct responsibility boundary: Claude Code decides which beta features to request; Odin forwards the decision transparently.

### 2.3 Behavioral Divergence Risk

The `anthropic-beta` header is a comma-separated list of beta feature identifiers. Claude Code uses it to opt in to features such as:

- `interleaved-thinking-2025-05-14` — streaming thinking tokens
- `output-128k-2025-02-19` — extended output token limit
- Other current and future beta features

When Odin strips these flags, the Cloud Code API may not enable the corresponding features, causing behavioral divergence between direct API usage and Odin-proxied usage. This is particularly insidious because:

1. The request succeeds — no error is returned.
2. The feature is silently downgraded — e.g., output is truncated to the default limit instead of 128k tokens.
3. The root cause is non-obvious — the developer does not see that a header was stripped.

## 3. Goals and Non-Goals

**Goals:**

- Forward the incoming request's `anthropic-beta` header value to the outgoing Cloud Code API request as-is, preserving all client-specified beta feature flags.
- Remove the `isThinkingModel()` check and hardcoded `interleaved-thinking-2025-05-14` injection from `buildHeaders()`, eliminating Odin's redundant re-derivation of the client's beta flags.
- Remove the now-unused `model` parameter from `buildHeaders()`, since it was only used for the `isThinkingModel()` check.
- Maintain backward compatibility: if no incoming `anthropic-beta` header is present, no `anthropic-beta` header is set (the client did not request any beta features, and Odin should not assume otherwise).

**Non-Goals:**

- Validating or filtering individual `anthropic-beta` feature flags — Cloud Code API is the authority on which flags it supports, and Odin should act as a transparent pass-through for this header.
- Forwarding all incoming request headers — only `anthropic-beta` is relevant to Cloud Code API behavior. Other headers (e.g., `x-api-key`, `anthropic-version`) are Anthropic-specific and should not be forwarded.
- Adding a fallback `interleaved-thinking` injection for cases where Claude Code omits the header — if Claude Code does not send `anthropic-beta`, that is the client's decision and Odin should respect it.

## 4. Design

### 4.1 Overview

The change threads the incoming HTTP request's headers from `server.js` through `sendRequest()` to `buildHeaders()`, where the `anthropic-beta` value is extracted and forwarded directly — no merge, no model check, no re-derivation.

```
Claude Code                    Odin server.js              Odin cloudcode.js         Cloud Code API
    │                              │                             │                        │
    │── POST /v1/messages ────────▶│                             │                        │
    │   anthropic-beta:            │                             │                        │
    │   output-128k-2025-02-19,    │                             │                        │
    │   interleaved-thinking-...   │                             │                        │
    │                              │── sendRequest(              │                        │
    │                              │     ...,                    │                        │
    │                              │     req.headers) ──────────▶│                        │
    │                              │                             │                        │
    │                              │                   buildHeaders(apiKey,               │
    │                              │                     incomingHeaders)                 │
    │                              │                             │                        │
    │                              │                   Forward anthropic-beta as-is       │
    │                              │                   (no merge, no model check)         │
    │                              │                             │                        │
    │                              │                             │── POST (Cloud Code) ──▶│
    │                              │                             │   anthropic-beta:      │
    │                              │                             │   output-128k-...,     │
    │                              │                             │   interleaved-thinking │
```

The change touches three files:

| File | Change |
|------|--------|
| `src/constants.js` | `buildHeaders()` removes `model` parameter, removes `isThinkingModel()` check, adds `incomingHeaders` parameter, forwards `anthropic-beta` |
| `src/cloudcode.js` | `sendRequest()` accepts optional `incomingHeaders` parameter, passes to `buildHeaders()` without `model` |
| `src/server.js` | Passes `req.headers` to `sendRequest()` |

### 4.2 Detailed Design

#### 4.2.1 `buildHeaders()` — Replace Injection with Passthrough

**File:** `src/constants.js`

The function signature changes: the `model` parameter is removed (it was only used for the `isThinkingModel()` check), and an `incomingHeaders` parameter is added. The `anthropic-beta` logic is replaced with a direct forward.

**Before:**

```javascript
/**
 * Build required headers for Cloud Code API requests.
 *
 * @param {string} apiKey - Bearer token for authentication
 * @param {string} model - Model name (used to determine thinking model headers)
 * @returns {Object} Headers object
 */
export function buildHeaders(apiKey, model) {
    const headers = {
        Authorization: `Bearer ${apiKey}`,
        'Content-Type': 'application/json',
        Accept: 'text/event-stream',
        'User-Agent': `antigravity/1.15.8 ${platform()}/${arch()}`,
        'X-Goog-Api-Client': 'google-cloud-sdk vscode_cloudshelleditor/0.1',
        'Client-Metadata': JSON.stringify({
            ideType: 'ANTIGRAVITY',
            platform: 'MACOS',
            pluginType: 'GEMINI',
        }),
    };

    if (isThinkingModel(model)) {
        headers['anthropic-beta'] = 'interleaved-thinking-2025-05-14';
    }

    return headers;
}
```

**After:**

```javascript
/**
 * Build required headers for Cloud Code API requests.
 *
 * @param {string} apiKey - Bearer token for authentication
 * @param {Object} [incomingHeaders={}] - Headers from the incoming client request
 * @returns {Object} Headers object
 */
export function buildHeaders(apiKey, incomingHeaders = {}) {
    const headers = {
        Authorization: `Bearer ${apiKey}`,
        'Content-Type': 'application/json',
        Accept: 'text/event-stream',
        'User-Agent': `antigravity/1.15.8 ${platform()}/${arch()}`,
        'X-Goog-Api-Client': 'google-cloud-sdk vscode_cloudshelleditor/0.1',
        'Client-Metadata': JSON.stringify({
            ideType: 'ANTIGRAVITY',
            platform: 'MACOS',
            pluginType: 'GEMINI',
        }),
    };

    if (incomingHeaders['anthropic-beta']) {
        headers['anthropic-beta'] = incomingHeaders['anthropic-beta'];
    }

    return headers;
}
```

The logic is now a single conditional: if the incoming request has an `anthropic-beta` header, forward it; otherwise, do not set it.

| Incoming `anthropic-beta` | Outgoing `anthropic-beta` |
|---------------------------|---------------------------|
| `'output-128k-2025-02-19,interleaved-thinking-2025-05-14'` | `'output-128k-2025-02-19,interleaved-thinking-2025-05-14'` (forwarded as-is) |
| `'interleaved-thinking-2025-05-14'` | `'interleaved-thinking-2025-05-14'` (forwarded as-is) |
| `'output-128k-2025-02-19'` | `'output-128k-2025-02-19'` (forwarded as-is) |
| (not present) | (not set) |

No merge logic. No model check. No `isThinkingModel()` call. The function no longer makes any decisions about which beta features should be enabled — it delegates that entirely to the upstream client.

**Note on `isThinkingModel()` in `constants.js`:** The function is not removed from the module because it is still imported and used by `converter.js` (line 2: `import { ANTIGRAVITY_SYSTEM_INSTRUCTION, isThinkingModel } from './constants.js'`). The function remains exported; only its usage within `buildHeaders()` is removed.

#### 4.2.2 `sendRequest()` — Thread Incoming Headers, Remove Model from buildHeaders Call

**File:** `src/cloudcode.js`

An optional `incomingHeaders` parameter is added. The `buildHeaders()` call is updated to pass `incomingHeaders` instead of `model`.

**Before:**

```javascript
export async function sendRequest(googleRequest, model, apiKey, debug) {
    const headers = buildHeaders(apiKey, model);
    // ...
}
```

**After:**

```javascript
export async function sendRequest(googleRequest, model, apiKey, debug, incomingHeaders = {}) {
    const headers = buildHeaders(apiKey, incomingHeaders);
    // ...
}
```

Note: `model` is still a parameter of `sendRequest()` because it is used in the Cloud Code request payload (line 44: `model,`). It is only removed from the `buildHeaders()` call.

#### 4.2.3 `server.js` — Pass Incoming Request Headers

**File:** `src/server.js`

The `sendRequest()` call is updated to pass the incoming request's headers object.

**Before:**

```javascript
const cloudResponse = await sendRequest(googleRequest, model, apiKey, debug);
```

**After:**

```javascript
const cloudResponse = await sendRequest(googleRequest, model, apiKey, debug, req.headers);
```

Node.js `http.IncomingMessage.headers` is a plain object with lowercased header names as keys and string values — this matches the `incomingHeaders['anthropic-beta']` access pattern in `buildHeaders()`.

### 4.3 Design Rationale

**Why pure passthrough instead of merge (the `opencode-antigravity-auth` approach):**

The `opencode-antigravity-auth` proxy uses a merge strategy: check if the incoming header already contains `interleaved-thinking-2025-05-14`, and if not, append it. This is correct for `opencode-antigravity-auth` because its upstream client (OpenCode) targets the Google Generative Language API and does not set Anthropic-specific headers — the proxy must fill this gap.

Odin's upstream client is Claude Code, which natively manages `anthropic-beta` with full awareness of which beta features to enable for each request. If Claude Code sends `anthropic-beta: output-128k-2025-02-19,interleaved-thinking-2025-05-14`, it has already determined the correct set of flags. If Claude Code sends a request without `interleaved-thinking`, it has a reason for that omission. Odin adding it back would override the client's decision.

The pure passthrough approach respects the separation of concerns: Claude Code owns the "what beta features to use" decision; Odin owns the "how to proxy to Cloud Code" plumbing.

**Why remove the `model` parameter from `buildHeaders()` instead of keeping it for future use:**

The `model` parameter was introduced solely for the `isThinkingModel()` check. With that check removed, the parameter has no purpose. Keeping an unused parameter:

1. Confuses future readers — "Why does this function take `model` if it doesn't use it?"
2. Creates a false dependency — callers must provide a model name even though it has no effect.
3. Violates YAGNI — if a future feature needs the model name, the parameter can be re-added at that point.

Removing it produces a cleaner, more honest API: `buildHeaders(apiKey, incomingHeaders)` — "I need credentials and the incoming request context."

**Why pass the full `incomingHeaders` object instead of just the `anthropic-beta` string:**

Passing the full headers object (rather than `req.headers['anthropic-beta']`) provides two advantages:

1. **Extensibility** — If future RFCs need to forward additional headers, the plumbing is already in place. Only `buildHeaders()` needs modification to extract and forward the additional header.
2. **Self-documenting API** — The parameter name `incomingHeaders` clearly communicates that this is the incoming request's context. A bare string parameter like `anthropicBeta` would lose this semantic information and require a JSDoc comment to explain its origin.

The trade-off is that `buildHeaders()` now has access to all incoming headers (including sensitive ones like `Authorization`). However, the function only reads `incomingHeaders['anthropic-beta']` — it does not iterate over or forward unknown headers. This is a safe, bounded access pattern.

**Why an optional parameter with `= {}` default instead of requiring callers to always pass headers:**

The `= {}` default makes the change backward-compatible at the function call level. If `buildHeaders()` or `sendRequest()` is called without the new parameter (e.g., in tests or future code paths), it produces the same output as before the change minus the `anthropic-beta` header. This is the correct behavior: without incoming headers, there is no beta flag to forward.

**Why forward `anthropic-beta` but not other Anthropic headers (e.g., `anthropic-version`, `x-api-key`):**

The Cloud Code API is Google's API endpoint, not Anthropic's. Most Anthropic-specific headers (authentication, versioning) are meaningless to the Cloud Code API. The `anthropic-beta` header is special — it controls beta feature behavior that the Cloud Code API explicitly supports (evidenced by the existing `interleaved-thinking-2025-05-14` usage in both `opencode-antigravity-auth` and the current Odin implementation). Forwarding only `anthropic-beta` follows the principle of least privilege: pass only what is needed, leave everything else out.

## 5. Implementation Plan

### Phase 1: Modify buildHeaders — 5 minutes

- [x] Remove the `model` parameter from `buildHeaders()`.
- [x] Remove the `isThinkingModel(model)` conditional and the hardcoded `'interleaved-thinking-2025-05-14'` value.
- [x] Add the `incomingHeaders = {}` parameter.
- [x] Add the `if (incomingHeaders['anthropic-beta'])` conditional to forward the header.
- [x] Update the JSDoc comment to reflect the new parameter and removed parameter.
- [x] Run `npm run check` to verify ESLint and Prettier compliance.

**Done when:** `buildHeaders(apiKey, { 'anthropic-beta': 'output-128k-2025-02-19' })` returns headers with `anthropic-beta: 'output-128k-2025-02-19'`; `buildHeaders(apiKey)` returns headers without `anthropic-beta`.

### Phase 2: Thread Headers Through sendRequest — 5 minutes

- [x] Add the `incomingHeaders = {}` parameter to `sendRequest()` in `cloudcode.js`.
- [x] Update the `buildHeaders()` call from `buildHeaders(apiKey, model)` to `buildHeaders(apiKey, incomingHeaders)`.
- [x] Update the JSDoc comment to document the new parameter.
- [x] Run `npm run check`.

**Done when:** `sendRequest()` signature includes the new parameter and passes it to `buildHeaders()`.

### Phase 3: Connect Server to sendRequest — 5 minutes

- [x] Update the `sendRequest()` call in `server.js` to pass `req.headers` as the fifth argument.
- [x] Run `npm run check`.

**Done when:** `server.js` passes incoming headers to `sendRequest()`, completing the data flow from incoming request to outgoing Cloud Code request.

### Phase 4: End-to-End Verification — 15 minutes

- [x] Start Odin with `--debug` and send a request with `anthropic-beta: output-128k-2025-02-19,interleaved-thinking-2025-05-14`. Verify the outgoing Cloud Code request includes the identical `anthropic-beta` value.
- [x] Send a request with `anthropic-beta: output-128k-2025-02-19` only (no interleaved-thinking). Verify the outgoing request includes exactly `anthropic-beta: output-128k-2025-02-19` — Odin should not inject `interleaved-thinking`.
- [x] Send a request without any `anthropic-beta` header. Verify the outgoing request does not include `anthropic-beta` at all — Odin should not add it.
- [x] Run a normal Claude Code session through the proxy to verify no regression on existing workflows. Claude Code will natively send `interleaved-thinking-2025-05-14` for thinking models.

**Done when:** All three passthrough scenarios produce correct outgoing headers, and existing workflows are unaffected.

## 6. Testing Strategy

The testing approach focuses on the `buildHeaders()` function since it contains the changed logic. The other changes (`sendRequest`, `server.js`) are pure plumbing with no conditional logic.

**Key Scenarios:**

| # | Scenario | Input | Expected Behavior |
|---|----------|-------|-------------------|
| 1 | No incoming headers (default) | `buildHeaders(key)` | No `anthropic-beta` header in result |
| 2 | Empty incoming headers | `buildHeaders(key, {})` | No `anthropic-beta` header in result |
| 3 | Incoming beta with single flag | `buildHeaders(key, { 'anthropic-beta': 'output-128k-2025-02-19' })` | `anthropic-beta: 'output-128k-2025-02-19'` |
| 4 | Incoming beta with multiple flags | `buildHeaders(key, { 'anthropic-beta': 'output-128k-2025-02-19,interleaved-thinking-2025-05-14' })` | `anthropic-beta: 'output-128k-2025-02-19,interleaved-thinking-2025-05-14'` (forwarded as-is) |
| 5 | Incoming beta with only interleaved | `buildHeaders(key, { 'anthropic-beta': 'interleaved-thinking-2025-05-14' })` | `anthropic-beta: 'interleaved-thinking-2025-05-14'` (forwarded as-is) |
| 6 | Non-anthropic-beta headers are not forwarded | `buildHeaders(key, { 'x-api-key': 'sk-...', 'anthropic-version': '2023-06-01' })` | No `x-api-key`, `anthropic-version`, or `anthropic-beta` in result |
| 7 | Incoming beta with unrelated headers | `buildHeaders(key, { 'anthropic-beta': 'flag-a', 'x-api-key': 'sk-...' })` | Only `anthropic-beta: 'flag-a'` forwarded; `x-api-key` not in result |
| 8 | Incoming empty anthropic-beta string | `buildHeaders(key, { 'anthropic-beta': '' })` | No `anthropic-beta` header (empty string is falsy) |
| 9 | Full data flow: server → sendRequest → buildHeaders | POST `/v1/messages` with `anthropic-beta` header via curl | Debug log shows outgoing headers include the exact forwarded `anthropic-beta` value |
| 10 | Backward compat: model parameter no longer affects headers | Compare old `buildHeaders(key, 'claude-sonnet-4-5-thinking')` vs new `buildHeaders(key)` | Old call would set `anthropic-beta`; new call correctly does not (no incoming header = no beta flag) |

## 7. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Claude Code sends a request without `anthropic-beta` for a thinking model | Very Low | Medium | Claude Code is purpose-built for the Anthropic API and consistently sets `anthropic-beta` for thinking models. If a future Claude Code version changes this behavior, it is a deliberate client-side decision that Odin should respect. If this causes issues, the `isThinkingModel()` fallback can be re-added as an opt-in `--ensure-thinking-header` CLI flag. |
| Forwarded beta flag is not supported by Cloud Code API | Low | Low | Cloud Code API ignores unrecognized `anthropic-beta` values (standard behavior for beta feature headers). The flag is passed through transparently; if Cloud Code does not support it, it is silently ignored on the API side — no error, no behavioral change. |
| Claude Code sends malformed `anthropic-beta` value | Low | Low | The value is forwarded as-is without parsing or normalization. If the value is malformed, the same error would occur with direct API usage. Odin adds no new failure modes. |
| `req.headers` object contains unexpected properties | Low | Low | `buildHeaders()` only reads `incomingHeaders['anthropic-beta']` via direct property access. It does not iterate, spread, or merge the object. Unexpected properties are ignored. |
| Breaking existing callers of `buildHeaders()` | Low | Low | The old signature `buildHeaders(apiKey, model)` would now interpret `model` as `incomingHeaders`. Since `model` is a string, `incomingHeaders['anthropic-beta']` would be `undefined`, and no `anthropic-beta` header would be set. This is functionally equivalent to the old non-thinking-model behavior. Any test code using the old signature must be updated. |

## 8. Future Work

- **Forward additional headers** — If future Claude Code features require other headers to be forwarded (e.g., `anthropic-version` for API version negotiation), the `incomingHeaders` plumbing is already in place. Only `buildHeaders()` needs modification to extract and forward the additional header.
- **Header allowlist configuration** — Add a CLI flag or config option to specify which incoming headers should be forwarded to Cloud Code API, for operators who need to pass custom headers through the proxy.
- **`--ensure-thinking-header` fallback flag** — If a non-Claude Code client is ever used with Odin and does not set `anthropic-beta`, add an opt-in flag that re-enables the `isThinkingModel()` injection as a safety net. Deferred because Odin's sole upstream client is Claude Code.

## 9. References

- `odin/src/constants.js:37–56` — Current `buildHeaders()` implementation (modification target).
- `odin/src/constants.js:66–70` — `isThinkingModel()` function (usage removed from `buildHeaders()`, still used by `converter.js`).
- `odin/src/cloudcode.js:38–59` — `sendRequest()` function (plumbing target).
- `odin/src/server.js:160` — `sendRequest()` call site (integration target).
- `odin/src/converter.js:2` — `isThinkingModel` import (confirms the function is still needed in the module).
- `opencode-antigravity-auth/src/plugin/request.ts:638` — `new Headers(init?.headers ?? {})` — the reference implementation starts from incoming headers (contrast with Odin's from-scratch approach).
- `opencode-antigravity-auth/src/plugin/request.ts:1380–1393` — `anthropic-beta` merge logic for OpenCode (justified because OpenCode does not set the header; inapplicable to Odin where Claude Code does).
- [Anthropic API — Beta Features](https://docs.anthropic.com/en/api/versioning#beta-features) — Documents the `anthropic-beta` header format (comma-separated feature identifiers).
- RFC-004: Defensive Content Block Validation in Converter — Prior RFC in the Odin project, referenced for style and conventions.

---

## Changelog

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 2.0 | 2026-02-17 | Chason Tang | Design pivot: replaced merge strategy with pure passthrough. Claude Code — not Odin — is the authority on `anthropic-beta` values. Removed `isThinkingModel()` usage and `model` parameter from `buildHeaders()`. Simplified design, testing, and implementation plan accordingly. |
| 1.0 | 2026-02-17 | Chason Tang | Initial version (merge strategy, mirroring `opencode-antigravity-auth` approach) |
