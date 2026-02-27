# RFC-013: Align requestId Generation with pi-mono Convention

**Version:** 1.0  
**Author:** Chason Tang  
**Date:** 2026-02-28  
**Status:** Implemented

---

## 1. Summary

Replace the `crypto.randomUUID()`-based `requestId` in `odin/src/cloudcode.js` with a `Date.now()` timestamp plus a base-36 random suffix, matching the format used by pi-mono's Google Gemini CLI provider. This makes request IDs temporally sortable and debuggable while maintaining uniqueness, and eliminates the `node:crypto` import that exists solely for this purpose.

## 2. Motivation

The current `requestId` format is:

```
agent-550e8400-e29b-41d4-a716-446655440000
```

A UUID v4 is opaque — it provides strong uniqueness but carries zero temporal information. When triaging issues in Cloud Code server logs, engineers must cross-reference separate timestamp fields to determine when a request was issued. This adds friction during incident response and ad-hoc debugging.

pi-mono's upstream implementation (`packages/ai/src/providers/google-gemini-cli.ts:913`) already uses a timestamp-embedded format:

```
agent-1740700800000-k8f3m2x9a
```

By adopting the same convention, Odin gains:

- **Instant temporal context** — the millisecond-precision timestamp is human-readable (convertible via `new Date(1740700800000)`), enabling at-a-glance request ordering without log joins.
- **Cross-system consistency** — identical `requestId` structure across Odin and pi-mono simplifies grep-based log correlation when both systems interact with Cloud Code.
- **Reduced dependency surface** — the `node:crypto` import exists solely for `randomUUID()`; removing it shrinks the module's dependency footprint.

## 3. Goals and Non-Goals

**Goals:**
- Adopt the `{prefix}-{timestamp}-{random}` request ID format to match pi-mono's convention.
- Remove the `node:crypto` import from `cloudcode.js`.
- Maintain request ID uniqueness guarantees sufficient for Cloud Code's requirements.

**Non-Goals:**
- Guaranteeing cryptographic-strength randomness in the request ID — Cloud Code uses `requestId` for log correlation, not security. `Math.random()` is adequate.
- Changing the `requestId` format in pi-mono — this RFC only covers Odin.

## 4. Design

### 4.1 Overview

The change is localized to a single file (`odin/src/cloudcode.js`). The `requestId` field in the Cloud Code request payload switches from UUID v4 to a three-part composite string: a static prefix, a millisecond timestamp, and a base-36 random suffix.

```
Before:  agent-550e8400-e29b-41d4-a716-446655440000
After:   agent-1740700800000-k8f3m2x9a
         ─────┬───── ──────┬────── ────┬────
           prefix    Date.now()    random(9)
```

### 4.2 Detailed Design

#### 4.2.1 requestId Format

The new `requestId` is constructed as:

```js
requestId: `agent-${Date.now()}-${Math.random().toString(36).slice(2, 11)}`,
```

**Components:**

| Segment | Source | Example | Purpose |
|---------|--------|---------|---------|
| Prefix | Hardcoded `"agent"` | `agent` | Identifies the request origin as an antigravity agent (consistent with pi-mono's `isAntigravity` branch) |
| Timestamp | `Date.now()` | `1740700800000` | Millisecond-precision epoch; enables temporal sorting and human-readable conversion |
| Random suffix | `Math.random().toString(36).slice(2, 11)` | `k8f3m2x9a` | 9-character base-36 string (~46.4 bits of entropy); prevents collisions for concurrent requests |

#### 4.2.2 Import Cleanup

The `node:crypto` module import on line 1 exists exclusively for `crypto.randomUUID()`. With the new approach relying on `Date.now()` and `Math.random()` (both global built-ins), the import is removed entirely.

**Before:**
```js
import crypto from 'node:crypto';

import { STREAMING_URL, PROJECT_ID, buildHeaders } from './constants.js';
```

**After:**
```js
import { STREAMING_URL, PROJECT_ID, buildHeaders } from './constants.js';
```

### 4.3 Design Rationale

**Why timestamp + random instead of UUID v4?**

| Criterion | UUID v4 (`crypto.randomUUID()`) | Timestamp + Random (`Date.now()` + `Math.random()`) |
|-----------|-------------------------------|-----------------------------------------------------|
| Temporal information | None | Millisecond precision embedded |
| Sortability | Not sortable | Lexicographically sortable by time |
| Uniqueness | 122 bits of randomness | ~46.4 bits random + time partitioning |
| Debuggability | Requires log cross-reference | Request time visible in ID |
| External dependency | Requires `node:crypto` | Uses global built-ins only |
| pi-mono alignment | Divergent | Identical format |

The uniqueness reduction is acceptable: Cloud Code uses `requestId` for log correlation within a single project scope, not as a globally unique primary key. The combination of millisecond timestamp and ~46.4 bits of randomness makes collisions effectively impossible for Odin's request volume (a collision requires two requests in the same millisecond with the same 9-char random suffix — probability ≈ 2.6 × 10⁻¹⁴ per millisecond pair).

**Why not `crypto.randomBytes()` for the suffix?**

`Math.random()` is sufficient for non-security-critical identifiers and avoids retaining the `node:crypto` dependency. The pi-mono reference implementation uses the same approach.

## 5. Implementation Plan

### Phase 1: Code Change — 0.5 hours

- [x] Remove `import crypto from 'node:crypto';` from `odin/src/cloudcode.js`
- [x] Replace `requestId: \`agent-${crypto.randomUUID()}\`` with `requestId: \`agent-${Date.now()}-${Math.random().toString(36).slice(2, 11)}\``

**Done when:** `odin/src/cloudcode.js` produces request IDs in the `agent-{timestamp}-{random9}` format and no longer imports `node:crypto`.

### Phase 2: Validation — 0.5 hours

- [x] Run existing test suite to confirm no regressions
- [x] Manual smoke test: send a request through Odin, verify the `requestId` in debug log output matches the new format

**Done when:** All existing tests pass; debug log shows a `requestId` like `agent-1740700800000-k8f3m2x9a`.

## 6. Testing Strategy

The change is narrow (single line of ID generation + import removal), so the testing approach is correspondingly focused.

**Key Scenarios:**

| # | Scenario | Input | Expected Behavior |
|---|----------|-------|-------------------|
| 1 | Normal request | Any valid `sendRequest()` call | `requestId` matches `/^agent-\d{13}-[a-z0-9]{9}$/` |
| 2 | Rapid consecutive requests | Two `sendRequest()` calls in tight loop | Distinct `requestId` values (different timestamp or different suffix) |
| 3 | Debug log output | `debug=true` | Logged payload contains `requestId` in the new format |
| 4 | No crypto import | Module load | `cloudcode.js` loads without `node:crypto`; no runtime errors |

## 7. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Cloud Code server rejects new format | Low | High | Cloud Code accepts arbitrary strings for `requestId`; pi-mono already uses this exact format in production |
| `Math.random()` collision under extreme concurrency | Very Low | Low | Timestamp partitioning reduces collision space to same-millisecond requests; 46.4 bits of randomness within that window is more than sufficient |

## 8. Future Work

- Consolidate request ID generation into a shared utility if more Odin modules need the same format, avoiding inline duplication.
- Consider extracting the `"agent"` prefix into a named constant alongside `PROJECT_ID` and `STREAMING_URL` for consistency with the constants module pattern.

## 9. References

- pi-mono reference implementation: `packages/ai/src/providers/google-gemini-cli.ts:913`
- Current Odin implementation: `odin/src/cloudcode.js:49`
- Cloud Code API internal spec: §2.5.4 (request wrapper format)

---

## Changelog

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2026-02-28 | Chason Tang | Initial version |
