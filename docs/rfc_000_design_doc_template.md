# RFC-{NNN}: {Feature Title}

**Version:** 1.0  
**Author(s):** {Name, or comma-separated list for multiple}  
**Date:** {YYYY-MM-DD}  
**Status:** Proposed | Implemented  
*(First draft is always `Proposed`. The author flips to `Implemented` only after the work is merged and has passed review — never on first draft.)*

---

## Project Context (read first — these constraints override generic RFC conventions)

> Meta-instruction for the RFC author/LLM. Do NOT include this section in the final RFC output.

This template is for a single-maintainer open-source project. Two facts constrain every RFC:

- **Limited operational surface.** There is no canary, A/B, percentage rollout, feature-flag, or shadow-traffic infrastructure. Telemetry is limited to Nginx access/error logs and the web service's own business logs. Do not propose SLOs, dashboards, alerts, staged rollouts, or any mechanism that depends on infrastructure not listed here. Observability changes, when needed, are limited to "what to log, at what level, in which existing log stream."
- **All RFCs describe code changes.** Process-only, org-structure, or documentation-only proposals are out of scope and should not be written against this template. §5 Interface Changes always describes real code/config/protocol — never substitute pseudo-code or prose flowcharts for a "process change."

---

## Writing Instructions (read before filling this template)

> Meta-instruction for the RFC author/LLM. Do NOT include this section in the final RFC output.

These rules override the instinct to "fill every section." Ignoring them produces a long RFC that looks complete but is fabricated or padded.

**Match scope to the change.** For simple proposals (single-file change, pure config/dependency update, etc.), prefer the minimum counts allowed (e.g., 1 Goal, 1 §4.2 subsection, 3 §8 scenarios, 2 §9 phases, 0 §10 risks) and use `Not applicable — {one-sentence reason}` for any Optional section that does not apply. Do not invent subsections, scenarios, or risks to fill the template's shape. When in doubt about whether a section applies, err toward including it with real content rather than padding.

- **No fabrication.** Do not invent metrics, user quotes, bug IDs, incident dates, benchmark numbers, error messages, or historical events. If you lack real data, write `No quantitative data available at this time` and move on.
- **No placeholder residue.** Every `{...}` token in this template is a hint to the author, not content. Replace each one with real text or delete the surrounding line. The final document must contain zero `{YYYY-MM-DD}`, `{Name}`, `{Input}`, or similar tokens. Replace the date placeholder with an absolute date (e.g. `2026-04-20`), never a relative one ("today", "this week").
- **No example-row residue.** Tables in §8 (Testing), §10 (Risks), and the Changelog each contain a sample row marked `(sample — delete this row)`. Delete every sample row before submitting; do not submit `{...}` cells as content.
- **No Example block residue.** Throughout the template, blocks labeled `**Example`...`**` (including every `**Example (bad vs. good)**` block in §1, §2, §3, §4, §5, §6, §7, §9, §10) are author guidance only. Delete every Example block before submitting — they are not part of the final document's structure.
- **Optional sections stay numbered.** When a section marked *(Optional)* does not apply, keep the heading and write one line in the body: `Not applicable — {one-sentence reason}`. Never renumber subsequent sections.
- **`None` is a valid answer.** For Non-Goals, Risks, Future Work, and References, writing `None` (or `No significant risks identified.`) is preferred over inventing filler.
- **Respect length caps.** Word counts and entry counts stated in each section are hard ceilings.
- **Total length.** Typical RFC runs 1,000–2,500 words. >4,000 words usually signals padding or over-scope — trim, or split into multiple RFCs.
- **Ban empty phrases.** Do not write "comprehensively improve", "significantly enhance", "robust and scalable", "we will monitor", "we will document", "works correctly", "behaves as expected", "best practices", "industry standard", "leverage", "seamlessly", "ensure that", "in order to", "going forward". Replace with the specific behavior, metric, or action.
- **Cross-section consistency.** Every `G#` in §3 must appear in at least one §8 row's `Covers` cell *and* at least one §9 phase's "Done when" clause. If a Goal is genuinely untestable, mark it `(non-testable: {reason})` in §3 itself; §8 may then omit it (see §3's Non-testable Goals fallback).

---

## 1. Summary

{One paragraph, max 150 words. What is being proposed, why, and the core idea. A reader should understand the full picture in 30 seconds. Do not duplicate §2 Motivation — state the proposal, not the backstory.}

**Example (bad vs. good) — scenario: add ESLint + Prettier to frigga:**
- *Bad:* "Introduce ESLint and Prettier to frigga to comprehensively improve code quality, unify code style, and lay the foundation for future engineering work."
- *Good:* "Add ESLint (typescript-eslint recommended config) and Prettier to frigga, applied to `frigga/src/**/*.ts`. Enforced via a `lint-staged` pre-commit hook and a GitHub Actions CI step that fails the build on violations."

## 2. Motivation

*Hard cap: 200 words total for this section (problem + value combined).*

{What problem exists today? Be specific — reference real behavior, error messages, user complaints, or performance data. If you do not have such data, write `No quantitative data available at this time` and stop — do not pad with speculation, generic claims, or fabricated numbers, quotes, or bug IDs. Avoid vague statements like "the system is not good enough."

What value does solving this bring? Describe the expected benefits — e.g., improved developer experience, better performance, reduced complexity, or new capabilities unlocked.}

**Example — problem statement (bad vs. good):**
- *Bad:* "The proxy is too slow and users are unhappy."
- *Good:* "When forwarding request bodies, the proxy streams chunk-by-chunk; the upstream Anthropic API rejects chunked transfer on this endpoint with a 400, so every request currently fails after the first chunk (see `frigga/src/proxy.ts`)."

**Example — value statement (bad vs. good) — scenario: add ESLint + Prettier to frigga:**
- *Bad:* "After adding ESLint and Prettier, code quality will improve significantly, bug count is expected to drop ~40%, and PR review will be 30% faster."
- *Good:* "PR review discussion about indentation, quote style, and unused variables shifts from human reviewers to local/CI checks. Magnitude of speedup is not quantified."

## 3. Goals and Non-Goals

**Goals:** 1–5 items, numbered `G1`, `G2`, ... so §8 and §9 can cite them by ID. Each must be concrete and verifiable — a reader should be able to tell, after the work is done, whether the goal was met.

- **G1.** {Concrete, measurable outcome 1}
- **G2.** {Concrete, measurable outcome 2}

**Example (bad vs. good):**
- *Bad:* "**G1.** Improve error handling."
- *Good:* "**G1.** Every upstream 5xx response is logged with `{status, url, request-id}` and returned to the client unchanged."

**Means vs. Goals:** Means (e.g., "use Redis for caching") describe HOW; Goals (e.g., "P95 read latency < 50 ms") describe WHAT outcome is achieved. Goals must be testable independently of the implementation choice — if swapping the implementation invalidates the goal, you wrote a means.

**Example (bad vs. good) — scenario: add ESLint + Prettier to frigga:**
- *Bad (means dressed as goal):* "**G1.** Use ESLint with the `typescript-eslint` recommended config and Prettier with default settings." — this describes HOW; if the team later swaps ESLint for Biome, the "goal" is invalidated even though the intended outcome (consistent, lint-clean code) is unchanged.
- *Good (true goal):* "**G1.** All files under `frigga/src/**/*.ts` pass the configured static analyzer with zero violations on `main`, enforced by a CI check that fails the build on violation." — survives swapping ESLint → Biome; the outcome is what's asserted, not the tool.

**Non-testable Goals (fallback):** if a Goal is genuinely not expressible as a §8 testable scenario (e.g., a developer-experience improvement that cannot be asserted with code), append `(non-testable: {one-sentence reason})` after the goal text — e.g., `**G3.** Improve onboarding (non-testable: subjective DX gain)`. §8's per-Goal coverage requirement and the Pre-submit Checklist accept this mark in lieu of inventing a scenario to satisfy traceability. **At most 1 Goal per RFC may use this fallback** — if you reach 2, the Goals are likely means in disguise (or vague aspirations); rewrite them as outcomes that a test or assertion could verify.

**Non-Goals:** 0–5 items. Only list things a reader could plausibly mistake for a goal of this proposal. If nothing qualifies, write `None`. Do not invent non-goals to pad the list.

**Do NOT mirror Goals.** A Non-Goal is something a reader might genuinely expect this RFC to address but it does not — not the negation of a Goal. Forbidden mirror pattern: G1 = "add ESLint to enforce style" → NG = "do not add Prettier to enforce style" (this is just the inverse phrasing of an adjacent unaddressed concern, not a real misconception to head off). Acceptable Non-Goal: "Migrating the existing 200+ pre-existing lint violations is out of scope; this RFC only enforces the rule on new/changed lines."

- {What this proposal explicitly does NOT address, and a one-sentence reason why}

## 4. Design

### 4.1 Overview

{High-level description of the approach. Include an architecture diagram inline as ASCII or a Mermaid code block — do not reference external image files, since they cannot be inspected during review.

*Diagram fallback:* write `N/A — textual description above is sufficient` and skip the diagram if **either**: (a) the proposal is a pure algorithm, data-structure, or single-function change where a diagram adds no information beyond the prose; **or** (b) the structure cannot be expressed cleanly in ASCII or Mermaid (e.g., dense graph topologies, freeform layouts, or anything that would need a real bitmap to read). Do not produce a trivial, distorted, or tautological diagram just to fill the slot, and do not invent a hierarchy that exists only to make the diagram non-empty.}

**Example (bad vs. good) — scenario: §4.2 describes a single-function change in `frigga/src/proxy.ts` to buffer the request body before forwarding:**
- *Bad — invented hierarchy to make the diagram non-empty:* a three-box flow `Client → Frigga → Anthropic API` with arrows labelled "request" / "response". Adds nothing the prose does not already convey.
- *Good:* `N/A — textual description above is sufficient (single-function change; no new component or boundary introduced).`

### 4.2 Detailed Design

{The core of the document. **Default to a single paragraph (no `#### 4.2.x` subsection headings)** — most changes have one coherent aspect and need no subdivision. Add `#### 4.2.x` subsections only when the design genuinely splits into multiple aspects that each need their own discussion (e.g., new data model AND new API surface AND new state machine). **Hard cap: 5 subsections** — if you need more, the proposal is too large; split into multiple RFCs. Each subsection (when used) should cover one coherent aspect of the design.

**Soft cap per 4.2.x subsection: ≤300 words.** Exceeding this means the subsection should be split, or this RFC is over-scoped and should be split into multiple RFCs.

*Code snippets:* show interface, signature, or key algorithmic skeleton — not full implementations. Snippets longer than ~30 lines should be replaced with a reference to the specific file path (e.g., `frigga/src/buffer.ts:22`).}

#### 4.2.1 {Aspect 1: e.g., Data Model / API / Algorithm / State Machine}

{Describe the design with enough detail that another engineer could implement it. Include code snippets, schemas, or protocol definitions where they add clarity.}

*(Add §4.2.2 through §4.2.5 ONLY when the design genuinely splits into multiple aspects that each need their own discussion. Most RFCs do not.)*

### 4.3 Design Rationale

{**Hard cap: 1–3 decisions.** Document only the decisions that a future reader (or you in 6 months) would actually question — not every micro-choice. For each, use a 3-line structure: **Chosen:** ... / **Reason:** ... / **Ruled out:** ... . Focus on key trade-offs and constraints. Exhaustive alternative comparison is not required.

**If no real alternative was considered**, write `Ruled out: N/A — no viable alternative considered` rather than inventing a strawman option just to fill the line. Fabricated alternatives mislead future readers about what was actually weighed.}

**Example:**
- **Chosen:** Buffer the full request body in memory before forwarding.
- **Reason:** The upstream endpoint requires a known `content-length` and rejects chunked transfer; buffering is the simplest way to satisfy this.
- **Ruled out:** Pass-through streaming — would require upstream to accept chunked transfer, which it does not on this endpoint.

## 5. Interface Changes *(Optional)*

**Skip if** no public API, CLI, configuration field, file format, or wire-protocol surface is added, changed, or removed → write `Not applicable — {one-sentence reason}` in the body, keep the heading, and stop. Otherwise, fill in the Before/After blocks below.

**Format requirement:** Both Before and After blocks must be syntactically valid in their language (a patch tool could apply the diff between them). No pseudo-code, no partial signatures, no narrative descriptions in place of code.

**Example (bad vs. good) — scenario: add `maxBodyBytes` config option to frigga:**
- *Bad:* "We add a new config option `maxBodyBytes` (a number) to limit request body size; if exceeded, the proxy returns 413."
- *Good:*
  ```ts
  // Before (frigga/src/config.ts)
  export interface Config {
    upstreamUrl: string;
    timeoutMs: number;
  }

  // After (frigga/src/config.ts)
  export interface Config {
    upstreamUrl: string;
    timeoutMs: number;
    maxBodyBytes: number; // requests with content-length > this → HTTP 413
  }
  ```

**Before:**
```
{existing interface — or "N/A — new interface"}
```

**After:**
```
{proposed interface — or "(removed)" for pure deletions}
```

## 6. Backward Compatibility & Migration *(Optional)*

**Skip if** the change is purely additive, or this is a brand-new feature with no prior behavior to preserve → write `Not applicable — {one-sentence reason}` in the body, keep the heading, and stop. Otherwise, list breaking changes and migration path below.

- **Breaking changes:** {List any behaviors, APIs, or configurations that will stop working or change semantics.} If 3 or more breaking changes are listed, §10 Risks must contain at least one corresponding row.
- **Migration path:** {How should existing users adapt? Provide concrete steps or code examples.}

**Example (bad vs. good) — scenario: frigga renames config field `timeoutMs` to `requestTimeoutMs`:**
- *Bad — Breaking changes:* "Existing config files may stop working." *Bad — Migration:* "Users should update their config to use the new field name."
- *Good — Breaking changes:* "Config field `timeoutMs` is removed; configs that still set it will fail at startup with `ConfigError: unknown field 'timeoutMs'` (raised by `frigga/src/config.ts:loadConfig`). No silent fallback or alias is provided."
- *Good — Migration:* "Run `sed -i '' 's/^timeoutMs:/requestTimeoutMs:/' config.yml` against any existing config. Bump the package's major version so consumers pinning to a major see the break."

## 7. Security *(Optional)*

**Skip if** the code path does not touch external input, credentials/keys, headers/URLs derived from input, upstream responses, authentication, or persisted data → write `Not applicable — {one-sentence reason}` in the body, keep the heading, and stop. Otherwise, list 1–4 concrete concerns below.

**Hard cap: 0–4 concerns.** Do not list generic threats (XSS, SQLi, CSRF, etc.) unless the code path actually exposes that surface — padding with non-applicable threats dilutes the real ones.

{For each concern, state three things: the specific attack or failure mode, which component is responsible for mitigation, and how that mitigation is enforced in the code.}

**Example (bad vs. good) — scenario: add ESLint + Prettier to frigga:**
- *Bad:* "ESLint plugins may introduce malicious code; Prettier formatting could break string literals and create XSS opportunities."
- *Good:* "Not applicable — ESLint and Prettier run only at development time and in CI; no runtime code path is altered, and no external input is processed."

## 8. Testing Strategy

{Describe the overall testing approach: what types of testing are planned (unit, integration, end-to-end, manual), what areas require the most coverage, and any testing infrastructure or tooling needed. For simple changes, one or two sentences may suffice.}

**Key Scenarios:** 3–8 rows. Delete the sample row below before submitting. Each `Input` must be concrete (specific values, paths, payloads). Each `Expected Behavior` must be assertable — a test could pass or fail against it. Do not write "works correctly" or "behaves as expected." The sample row's content is for format reference only; never copy its scenario or expected behavior verbatim into a real RFC.

**Coverage requirement:** every `G#` in §3 must appear in at least one row's `Covers` cell — UNLESS that Goal is marked `(non-testable: {reason})` in §3, in which case §8 may omit it. Number scenarios `S1`, `S2`, ... so §9 phases can cite them by ID. One scenario may cover multiple Goals (e.g., `Covers: G1, G3`).

**`Covers` cell format (strict):** comma-separated `G#` IDs only — e.g., `G1` or `G1, G3`. Do not use slashes (`G1/G3`), the word `and` (`G1 and G3`), ampersands (`G1 & G3`), or whitespace-only separators (`G1 G3`). Downstream tooling parses this cell by splitting on `,`.

| # | Covers | Scenario | Input | Expected Behavior |
|---|--------|----------|-------|-------------------|
| S1 | G1 | *(sample — delete this row)* Happy path | `{concrete input}` | `{assertable expectation}` |

## 9. Implementation Plan

{Break the work into 2–5 phases. Each phase should be independently shippable or at least independently verifiable. Unit tests should be written as early as possible to define expected behavior and serve as the specification for subsequent implementation work.

Unit test phase placement: By default, unit tests are Phase 1 — this applies when building new features on an existing codebase. If prerequisite work (e.g., project scaffolding, infrastructure setup) must be completed before tests can be written, defer the unit test phase to the earliest point where it becomes feasible. If the proposal involves no unit-testable logic (e.g., pure configuration changes, documentation), this phase may be omitted.

**Example (Phase 1 placement, bad vs. good):**
- *Bad (scaffolding RFC, no test runner or source files exist yet):* "Phase 1: Unit Tests — write Vitest tests for the parser." Vitest is not installed and there are no source files; tests cannot be authored, let alone fail.
- *Good (same scenario):* "Phase 1: Project scaffolding (`package.json`, `tsconfig.json`, install Vitest). Phase 2: Unit Tests (red). Phase 3: Implementation (green)."
- *Bad (pure config RFC — rename a CI env var):* "Phase 1: Unit Tests — assert the new env var is read." There is no unit-testable application logic; the change lives in CI YAML.
- *Good (same scenario):* Omit a unit-test phase entirely; Phase 1 is "update workflow YAML", verified by the CI run on the PR.

**Done-when discipline:** each phase's "Done when" clause must cite specific `G#` (from §3) or `S#` (from §8) IDs — not just "tests pass" or "code merged." Collectively, the phases must cite every `G#` in §3.

**Example (Done-when, bad vs. good) — scenario: add ESLint + Prettier to frigga:**
- *Bad:* "All tests pass and lint runs successfully."
- *Good:* "G2 verified by S3: running `pnpm lint` on the sample file with 4 known formatting violations reports all 4 errors and exits non-zero."}

### Phase 1: Unit Tests {by default — if scaffolding-first applies, RENAME this heading (e.g., "Project Scaffolding") and move Unit Tests to Phase 2; if no unit-testable logic exists, OMIT this phase entirely}

- [ ] {Define test cases based on key scenarios from §8}
- [ ] {Implement unit tests for core logic}

**Done when:** {Scenarios S1–S# written and initially failing (red).}

### Phase 2: {Core Implementation}

- [ ] {Task with enough detail to be a work item}
- [ ] {Task}

**Done when:** {Phase 1 scenarios (S1–S#) passing (green); satisfies G#.}

### Phase N: {Name}

- [ ] {Task}

**Done when:** {Specific acceptance criteria citing G# or S#.}

## 10. Risks

> Do not propose monitoring, alerting, dashboard, or staged-rollout mitigations — see Project Context (no such infrastructure exists). Mitigations must be concrete code/config changes; the mechanism should already be specified in §4 or covered by a §8 scenario.

0–6 rows. If no significant risks apply, write `No significant risks identified.` and omit the table. Each risk must point to a specific mechanism or dependency introduced in §4; do not list speculative or generic risks. Mitigation must be a concrete action — not "we will monitor" or "we will document."

Delete the sample row below before submitting.

**Likelihood/Impact values:** Only `High`, `Med`, or `Low` are accepted. Do not invent additional levels (e.g., `Critical`, `Catastrophic`, `Trivial`, `N/A`).

**Example — Mitigation (bad vs. good):**
- *Bad:* "We will monitor memory usage in production."
- *Good:* "`buffer.ts:22` rejects requests with `content-length > 10 MB` by returning HTTP 413 before any allocation; the limit is covered by a unit test in §8."

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| *(sample — delete this row)* {specific failure mode tied to §4} | High / Med / Low | High / Med / Low | {concrete action} |

## 11. Future Work

0–5 items. Ideas that are related but explicitly out of scope for this proposal. Recording them here prevents scope creep while preserving institutional memory. If none, write `None at this time.`

**Only list items that have been actually discussed during this RFC's design, or that this RFC's scope decisions explicitly deferred.** Do not invent aspirational ideas, generic "nice-to-haves", or speculative roadmap items to fill the slot — `None at this time.` is the correct answer when no real deferred work exists. Each item must trace back to a concrete trade-off made in §3 (Non-Goals), §4 (Design), or §6 (Migration).

- {Idea 1 — brief rationale for deferral}
- {Idea 2}

## 12. References

List only real, verifiable items: file paths in this repo, URLs you have checked, prior RFCs. If none apply, write `None`. Do not fabricate citations. Any commit SHA cited must exist in the repository's history — verify with `git cat-file -e <sha>` if uncertain; do not invent SHAs to fill the slot.

- {Related documents, code paths, external links, prior art}

---

## Pre-submit Checklist

> Meta-instruction for the RFC author/LLM. Do NOT include this section in the final RFC output. Use it as a self-check during generation; the published RFC ends after the Changelog.

Verify each item before submitting. Any unchecked item is a blocker.

- [ ] No `{...}` placeholder tokens remain anywhere in the document (run this check AFTER deleting sample rows in §8/§10/Changelog, which intentionally contain `{...}` cells).
- [ ] The header date is an absolute date (not `{YYYY-MM-DD}` or a relative word like "today").
- [ ] Every Optional section (§5, §6, §7) is either filled or contains a `Not applicable — {reason}` line. No section has been renumbered.
- [ ] Sample rows in §8 and §10 have been deleted or replaced. Changelog contains exactly one row (v1.0) with both `Date` and `Author` filled in — no placeholders — unless this is a post-submission revision.
- [ ] All `**Example`...`**` guidance blocks have been deleted from the body.
- [ ] No fabricated metrics, user quotes, bug IDs, error messages, or commit SHAs.
- [ ] Every `G#` in §3 appears in at least one §8 row's `Covers` cell (or is marked `(non-testable: {reason})` in §3) AND in at least one §9 phase's "Done when" clause.
- [ ] At most 1 Goal in §3 carries the `(non-testable: ...)` mark. If 2 or more, rewrite them as outcomes a test or assertion could verify.
- [ ] All hard caps respected: §1 ≤150 words, §2 ≤200 words, §3 ≤5 Goals, §4.2 ≤5 subsections (≤300 words each), §4.3 1–3 decisions, §7 ≤4 concerns, §8 3–8 scenarios, §9 2–5 phases, §10 ≤6 risks, §11 ≤5 items, total ≤4,000 words.
- [ ] All banned phrases absent: "we will monitor", "we will document", "works correctly", "behaves as expected", "comprehensively improve", "significantly enhance", "robust and scalable", "best practices", "industry standard", "leverage", "seamlessly", "ensure that", "in order to", "going forward".
- [ ] References in §12 are all real paths or URLs that were actually checked.

---

## Changelog

{Entries are listed in reverse chronological order (newest first).  
Only substantive design changes (scope, approach, interface, etc.) require a new entry and version bump. Implementation progress updates (e.g., checking off tasks) do not.}

**Initial draft:** the table must contain exactly one row — v1.0 — with both `Date` and `Author` cells filled in (real absolute date, real author name). Add v1.1 (or higher) rows only when substantive changes are made after first submission.

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | {YYYY-MM-DD — replace with absolute date} | {Name — replace with real author} | Initial version |

*Example of a row added after first submission (illustrative — do NOT include in the initial draft; do NOT copy the placeholder values verbatim — `{absolute-date}` and `{author-name}` are format hints, not real data):*

```
| 1.1 | {absolute-date} | {author-name} | {change-summary} |
```
