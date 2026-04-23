# RFC-008: CLI-Driven Override of metadata.user_id Identity Fields

**Version:** 1.0  
**Author:** Chason Tang  
**Date:** 2026-04-22  
**Status:** Implemented

## 1. Summary

Add two optional CLI flags to frigga — `--device-id` and `--account-uuid` — that override the corresponding fields inside the JSON-encoded `metadata.user_id` of every forwarded `/v1/messages` and `/v1/messages/count_tokens` request body. When neither flag is set, the request body forwards reference-identical to the buffered input (no `JSON.parse`). Malformed bodies pass through unmodified. The implementation is a single helper inserted between `bufferRequestBody` (RFC-007) and `upstreamReq.end()`.

## 2. Motivation

claude-code's `getAPIMetadata` (`claude-code/src/services/api/claude.ts:503`) constructs `metadata.user_id` as a JSON string carrying `device_id` (machine-local), `account_uuid` (OAuth), and `session_id`. Operators running frigga in front of multiple claude-code instances cannot currently pin these identity fields without modifying the upstream client code.

This RFC enables an operator to declare a fixed `device_id` and/or `account_uuid` per frigga process via CLI flags, reusing RFC-007's buffered body so no streaming infrastructure changes are needed. Both flags are independently optional; an absent flag leaves the corresponding field untouched. No quantitative data available at this time.

## 3. Goals and Non-Goals

**Goals:**

- **G1.** When `--device-id=X` and/or `--account-uuid=Y` is set, the corresponding key inside the JSON-parsed `metadata.user_id` of every forwarded `/v1/messages` or `/v1/messages/count_tokens` request body is replaced with the CLI value; all other keys (`session_id`, plus any unknown keys) survive unchanged.
- **G2.** When neither flag is set, the request body Buffer is forwarded reference-identical to the buffered input — no `JSON.parse` or `JSON.stringify` runs.
- **G3.** A malformed top-level body, missing `metadata.user_id`, non-string `user_id`, or failed `JSON.parse(user_id)` does not fail the request — the body forwards unchanged.

**Non-Goals:**

- Overriding any other `metadata` field (e.g., `session_id`) or any non-`metadata` field — this RFC scopes to the two identity keys named by the operator.
- Validating CLI option values (no UUID format check, no length check) — frigga forwards whatever bytes the operator supplies.

## 4. Design

### 4.1 Overview

A new exported helper `overrideMetadataUserId(body, options)` is inserted into the forwarding path between `bufferRequestBody` (RFC-007 §4.2.2) and `upstreamReq.end(requestBody)`:

```
Client ──> bufferRequestBody ──> overrideMetadataUserId ──> upstreamReq.end
                                       │
                                       ├── neither flag set       → return input Buffer
                                       ├── parse fails / bad shape → return input Buffer
                                       └── replace named keys      → return new Buffer
```

`startServer` accepts two new config keys (`deviceId`, `accountUuid`); `cli.js` parses them from `--device-id` and `--account-uuid` flags.

### 4.2 Detailed Design

#### 4.2.1 `overrideMetadataUserId` helper

A new exported function in `frigga/src/server.js`. Returns the input `Buffer` unchanged on every short-circuit path so the forwarding path never branches on a sentinel value:

```javascript
/**
 * Replace metadata.user_id.{device_id, account_uuid} when the corresponding
 * CLI option is set. Returns the input Buffer unchanged on any short-circuit
 * (no override requested, parse failure, or shape mismatch).
 * @param {Buffer} body
 * @param {{ deviceId?: string, accountUuid?: string }} options
 * @returns {Buffer}
 */
export function overrideMetadataUserId(body, { deviceId, accountUuid }) {
  if (deviceId === undefined && accountUuid === undefined) return body;

  let parsed;
  try {
    parsed = JSON.parse(body.toString("utf-8"));
  } catch {
    return body;
  }
  if (parsed === null || typeof parsed !== "object" || Array.isArray(parsed)) {
    return body;
  }

  const userIdStr = parsed.metadata?.user_id;
  if (typeof userIdStr !== "string") return body;

  let userId;
  try {
    userId = JSON.parse(userIdStr);
  } catch {
    return body;
  }
  if (userId === null || typeof userId !== "object" || Array.isArray(userId)) {
    return body;
  }

  if (deviceId !== undefined) userId.device_id = deviceId;
  if (accountUuid !== undefined) userId.account_uuid = accountUuid;

  parsed.metadata.user_id = JSON.stringify(userId);
  return Buffer.from(JSON.stringify(parsed), "utf-8");
}
```

The four guard lines (`null`, `typeof !== "object"`, `Array.isArray`, missing/non-string `user_id`) protect against `JSON.parse` returning `null`, primitives, arrays, or any shape where `parsed.metadata?.user_id` would read garbage. Only `device_id` and `account_uuid` are written; `session_id` and any unknown keys remain untouched because the function does not iterate or filter the parsed object's other properties.

#### 4.2.2 CLI parsing extension

`cli.js` `resolveArgs` gains two passthrough fields. The `typeof === "string"` guard rejects bare flags (e.g., `--device-id` with no `=`), which `parseArgs` records as `true`:

```javascript
const deviceId =
  typeof args["device-id"] === "string" ? args["device-id"] : undefined;
const accountUuid =
  typeof args["account-uuid"] === "string" ? args["account-uuid"] : undefined;
```

`printUsage` gains two lines listing the new flags. `startServer({ port, host, logBody, deviceId, accountUuid })` plumbs both through to the request handler closure.

#### 4.2.3 Forwarding-path integration

A single line is inserted in `server.js` between `bufferRequestBody` and `upstreamReq.end`:

```javascript
requestBody = await bufferRequestBody(req);
// …
requestBody = overrideMetadataUserId(requestBody, { deviceId, accountUuid });
// …
upstreamReq.end(requestBody);
```

Because `requestBody` is reassigned, the existing log path (`createRequestLogEmitter` reads `requestBody.toString("utf-8")`) reflects the post-override bytes that upstream actually receives — the audit log stays consistent with the wire payload.

### 4.3 Design Rationale

- **Chosen:** Silent passthrough on parse failure or shape mismatch.
- **Reason:** Frigga is a transparent proxy (RFC-007 §4.3); rejecting requests whose body shape we don't recognize would couple frigga to a specific client schema and produce a worse failure mode than letting upstream decide.
- **Ruled out:** Returning HTTP 400 on malformed body — diverges from the transparent-forward stance and prevents future endpoint additions that may carry non-JSON or differently shaped bodies.

- **Chosen:** Skip `JSON.parse` entirely when both flags are absent.
- **Reason:** Operators not using this feature pay zero CPU and zero allocation; the same forwarding path covers both override-on and override-off configurations.
- **Ruled out:** Always-parse for code-path uniformity — adds a `JSON.parse` + `JSON.stringify` round trip on every request for no behavioral benefit.

## 5. Interface Changes

**Before:**
```ts
// frigga/src/cli.js — resolveArgs return type
{
  port: number;
  host: string;
  logBody: boolean;
}
```

**After:**
```ts
// frigga/src/cli.js — resolveArgs return type
{
  port: number;
  host: string;
  logBody: boolean;
  deviceId: string | undefined;   // from --device-id, optional
  accountUuid: string | undefined; // from --account-uuid, optional
}
```

`printUsage` text gains two trailing option lines describing the new flags; the format follows the existing two-column layout in `cli.js`.

## 6. Backward Compatibility & Migration

Not applicable — the change is purely additive; `--device-id` and `--account-uuid` default to `undefined`, in which case `overrideMetadataUserId` short-circuits and forwarded bytes are reference-identical to today's behavior.

## 7. Security

Not applicable — override values originate from operator-provided CLI flags (trusted local `process.argv`); body parsing uses standard `JSON.parse` with no `eval`/`Function`, and writes only to two named properties on a function-local object, so no prototype-pollution surface is introduced. The override changes how upstream attributes the request to a user identity, which is the feature itself rather than an unintended exposure.

## 8. Testing Strategy

Unit tests in `frigga/tests/handler.test.js` cover `overrideMetadataUserId` directly. Integration coverage relies on the existing `server.test.js` flows; the override is a pure function call with no I/O, so unit-level verification is sufficient.

| # | Covers | Scenario | Input | Expected Behavior |
|---|--------|----------|-------|-------------------|
| S1 | G1 | `--device-id` only | body `{"metadata":{"user_id":"{\"device_id\":\"old\",\"account_uuid\":\"u\",\"session_id\":\"s\"}"}}`, options `{ deviceId: "new" }` | Output's `metadata.user_id` parses to `{ device_id: "new", account_uuid: "u", session_id: "s" }` |
| S2 | G1 | `--account-uuid` only | Same body as S1, options `{ accountUuid: "u2" }` | Output's `metadata.user_id` parses to `{ device_id: "old", account_uuid: "u2", session_id: "s" }` |
| S3 | G1 | both flags | Same body as S1, options `{ deviceId: "new", accountUuid: "u2" }` | Both fields replaced; `session_id` preserved |
| S4 | G1 | unknown-key preservation | body's inner `user_id` includes `"custom_field":"x"`, options `{ deviceId: "new" }` | `custom_field` and `session_id` both present in output's parsed `user_id` |
| S5 | G2 | no flags set | Any body, options `{}` | Returned Buffer is `===` input Buffer (reference identity) |
| S6 | G3 | top-level body not JSON | `Buffer.from("not-json")`, options `{ deviceId: "x" }` | Returned Buffer is `===` input Buffer |
| S7 | G3 | `metadata.user_id` missing | `{"messages":[]}`, options `{ deviceId: "x" }` | Returned Buffer is `===` input Buffer |
| S8 | G3 | inner `user_id` not JSON | `{"metadata":{"user_id":"not-json"}}`, options `{ deviceId: "x" }` | Returned Buffer is `===` input Buffer |

## 9. Implementation Plan

### Phase 1: Unit Tests

- [ ] Add `overrideMetadataUserId` test group in `tests/handler.test.js` covering S1–S8
- [ ] Import the not-yet-exported `overrideMetadataUserId` from `../src/server.js`

**Done when:** Scenarios S1–S8 written and initially failing (red) — `overrideMetadataUserId` not yet implemented.

### Phase 2: Helper + CLI Extension

- [ ] Implement `overrideMetadataUserId` in `src/server.js` per §4.2.1
- [ ] Extend `resolveArgs` to read `--device-id` and `--account-uuid` per §4.2.2
- [ ] Update `printUsage` text per §5

**Done when:** S1–S8 pass (green); CLI accepts the two new flags (verified via `node src/index.js --help`); G1, G2, G3 satisfied via S1–S8.

### Phase 3: Forwarding-Path Wiring

- [ ] Plumb `deviceId` / `accountUuid` from `startServer` config into the request handler closure
- [ ] Insert `requestBody = overrideMetadataUserId(requestBody, { deviceId, accountUuid })` between `bufferRequestBody` and `upstreamReq.end(requestBody)` in `src/server.js`

**Done when:** `npm test` passes (S1–S8 green; existing integration tests confirm G2, G3 do not interfere with the no-flag forwarding path); `npm run check` passes; manual smoke test with both flags set against a real claude-code request confirms upstream receives the overridden values (G1 end-to-end).

## 10. Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Re-serialized body bytes differ from received bytes (key order, whitespace) when override is active, breaking any byte-keyed cache layer between frigga and upstream | Low | Low | The no-flag path (S5) returns the input Buffer reference-identical, so default deployments are unaffected; operators enabling the flag accept this trade-off as part of the feature |
| Operator typo in CLI value (e.g., `--device-id=` with empty trailing string) silently injects an empty `device_id` | Med | Low | `typeof === "string"` guard in §4.2.2 accepts empty strings as valid intent (consistent with `--host=` behavior); `printUsage` text describes the flag's effect so the operator can self-diagnose |

## 11. Future Work

None at this time.

## 12. References

- `claude-code/src/services/api/claude.ts:503-528` — `getAPIMetadata` source defining the `metadata.user_id` shape this RFC overrides
- `frigga/src/server.js` — `bufferRequestBody` integration point (RFC-007 §4.2.5) and target for `overrideMetadataUserId` insertion
- `frigga/src/cli.js` — `parseArgs` / `resolveArgs` extension target
- [RFC-007: Request Body Full Buffering](rfc_007_request_body_full_buffering.md) — buffering infrastructure this RFC builds on
