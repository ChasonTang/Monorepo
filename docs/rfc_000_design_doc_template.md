# RFC-{NNN}: {Feature Title}

**Version:** 1.0<br>
**Author(s):** {Name, or comma-separated list}<br>
**Date:** {YYYY-MM-DD}<br>
**Status:** Proposed | Implemented
*(First draft is `Proposed`. Flip to `Implemented` only after the work is merged and reviewed.)*

---

## Writing Instructions (delete before submitting)

These rules override the instinct to "fill every section." Treat them as hard constraints.

**Anti-fabrication:**
- **No invented data.** No fake metrics, user quotes, bug IDs, incident dates, benchmark numbers, error messages, or commit SHAs. If data is missing, write `No quantitative data available at this time` and move on.
- **References in §12 must be real** — paths in this repo, URLs you actually checked, prior RFCs. Verify any commit SHA with `git cat-file -e <sha>` before citing.

**Strip template artifacts before submitting:**
- Every `{...}` token is a hint, not content — replace or delete the surrounding line. Zero `{YYYY-MM-DD}`, `{Name}`, `{Input}`, etc. should remain. Date is absolute (e.g., `2026-04-23`), never relative ("today").
- Delete every `**Example...**` block — they are author guidance, not document content.
- Delete sample table rows in §8 and §10 (marked `(sample — delete this row)`).
- Delete every `---` horizontal rule from the final output.
- **Delete this Writing Instructions section and the Pre-submit Checklist** — the final RFC ends after §12 References. Do not append Changelog / History / Revision / Version History — anything after §12 is forbidden.

**Preserve metadata `<br>` tags.** The `<br>` after `**Version:**`, `**Author(s):**`, and `**Date:**` is an intentional Markdown line break — without it those fields collapse into a single rendered line. `**Status:**` does not need one (the blank line before `## 1. Summary` ends the paragraph).

**Scope discipline:**
- **Match scope to change.** Simple proposals use minimum counts (1 Goal, 1 §4.2 subsection, 3 §8 scenarios, 2 §9 phases, 0 §10 risks). Don't invent subsections, scenarios, or risks to fill the shape.
- **Length:** typical RFC 1,000–2,500 words; >4,000 signals padding or over-scope (split into multiple RFCs).
- **Optional sections (§5, §6, §7) stay numbered.** When they don't apply, write `Not applicable — {one-sentence reason}` in the body. Never delete or renumber.
- **`None` is a valid answer** for Non-Goals, Risks, Future Work, References.

**Cross-section consistency:**
- Every `G#` in §3 must appear in at least one §8 row's `Covers` cell **and** at least one §9 phase's "Done when" clause.
- Untestable Goals: append `(non-testable: {one-sentence reason})` in §3 — at most 1 per RFC. Such goals may be omitted from §8.

**Banned phrases.** Replace each with the specific behavior, metric, or action: "comprehensively improve", "significantly enhance", "robust and scalable", "we will monitor", "we will document", "works correctly", "behaves as expected", "best practices", "industry standard", "leverage", "seamlessly", "ensure that", "in order to", "going forward".

---

## 1. Summary

{One paragraph, max 150 words. State the proposal and core idea — not the backstory (that's §2). A reader should understand the full picture in 30 seconds.}

**Example (bad → good) — adding ESLint + Prettier to frigga:**
- *Bad:* "Introduce ESLint and Prettier to comprehensively improve code quality."
- *Good:* "Add ESLint (typescript-eslint recommended) and Prettier to `frigga/src/**/*.ts`. Enforced via a `lint-staged` pre-commit hook and a GitHub Actions step that fails on violations."

## 2. Motivation

*Hard cap: 200 words (problem + value combined).*

{What problem exists today? Reference real behavior, error messages, or data. Without data, write `No quantitative data available at this time` and stop — do not pad with speculation, generic claims, or fabricated numbers.

What value does solving this bring? Concrete benefits — improved DX, performance, reduced complexity, new capabilities unlocked.}

## 3. Goals and Non-Goals

**Goals:** 1–5 items, numbered `G1`, `G2`, ... so §8 and §9 can cite them by ID. Each must be concrete and verifiable — a reader can tell after the work is done whether the goal was met.

- **G1.** {Concrete, measurable outcome}

**Means vs Goals.** Means describe HOW (e.g., "use Redis for caching"); Goals describe WHAT outcome (e.g., "P95 read latency < 50 ms"). If swapping the implementation invalidates the goal, you wrote a means.

**Example (bad → good) — adding ESLint + Prettier to frigga:**
- *Bad (means):* "**G1.** Use ESLint with `typescript-eslint` recommended config and Prettier defaults." If the team later swaps ESLint → Biome, the goal is invalidated even though the intended outcome (consistent, lint-clean code) is unchanged.
- *Good (outcome):* "**G1.** All files under `frigga/src/**/*.ts` pass the configured static analyzer with zero violations on `main`, enforced by a CI check that fails the build on violation."

**Non-testable Goals (fallback):** if a goal is genuinely not expressible as a §8 scenario (e.g., subjective DX gain), append `(non-testable: {reason})` in §3. **Max 1 per RFC** — if you need 2+, the goals are likely means or vague aspirations; rewrite as outcomes a test can verify.

**Non-Goals:** 0–5 items. Only list things a reader could plausibly mistake for a goal. **Don't mirror Goals.** A Non-Goal is something a reader might genuinely expect this RFC to address but it does not — not the negation of a Goal. Forbidden mirror: G1 = "add ESLint to enforce style" → NG = "do not add Prettier". Acceptable: "Migrating the existing 200+ pre-existing lint violations is out of scope; this RFC enforces the rule on new/changed lines only." If nothing qualifies, write `None`.

- {Non-Goal with one-sentence reason}

## 4. Design

### 4.1 Overview

{High-level approach. Include an inline ASCII or Mermaid diagram — no external image refs (cannot be inspected during review).

*Skip the diagram* (write `N/A — textual description above is sufficient`) when (a) the change is a single-function, algorithm, or data-structure change where a diagram adds no information beyond the prose; or (b) the structure cannot be expressed cleanly in ASCII/Mermaid (e.g., dense graphs). Don't invent hierarchy to fill the slot.}

### 4.2 Detailed Design

{**Default to a single paragraph** (no `#### 4.2.x` subsection headings). Add subsections only when the design genuinely splits into multiple aspects (e.g., new data model AND new API AND new state machine). **Hard cap: 5 subsections, ≤300 words each.** If you exceed this, the proposal is too large — split into multiple RFCs.

Show interface, signature, or key algorithmic skeleton — not full implementations. Snippets >30 lines should be replaced with a file:line reference (e.g., `frigga/src/buffer.ts:22`).}

#### 4.2.1 {Aspect: e.g., Data Model / API / Algorithm / State Machine}

{Implementation-level detail with code snippets, schemas, or protocol definitions where they add clarity.}

*(Add §4.2.2 through §4.2.5 ONLY when the design genuinely splits into multiple aspects. Most RFCs do not.)*

### 4.3 Design Rationale

{**Hard cap: 1–3 decisions.** Document only decisions a future reader would actually question — not every micro-choice. For each, use a 3-line structure:
- **Chosen:** ...
- **Reason:** ...
- **Ruled out:** ...

If no real alternative was considered, write `Ruled out: N/A — no viable alternative considered` rather than inventing a strawman.}

## 5. Interface Changes *(Optional)*

**Skip if** no public API, CLI, configuration field, file format, or wire-protocol surface is added, changed, or removed → write `Not applicable — {one-sentence reason}` in the body, keep the heading, and stop.

**Format requirement:** Both Before and After blocks must be syntactically valid in their language (a patch tool could apply the diff between them). No pseudo-code, no partial signatures, no narrative descriptions in place of code.

**Before:**
```
{existing interface — or "N/A — new interface"}
```

**After:**
```
{proposed interface — or "(removed)" for pure deletions}
```

## 6. Backward Compatibility & Migration *(Optional)*

**Skip if** the change is purely additive, or this is a brand-new feature with no prior behavior to preserve → write `Not applicable — {one-sentence reason}` and stop.

- **Breaking changes:** {What stops working or changes semantics. If 3+ are listed, §10 must contain at least one corresponding row.}
- **Migration path:** {Concrete steps or commands. Include the exact error message or symptom users will see if they don't migrate.}

## 7. Security *(Optional)*

**Skip if** the code path does not touch external input, credentials/keys, headers/URLs derived from input, upstream responses, authentication, or persisted data → write `Not applicable — {one-sentence reason}` and stop.

**Hard cap: 0–4 concerns.** Don't list generic threats (XSS, SQLi, CSRF, etc.) unless the code path actually exposes that surface — padding with non-applicable threats dilutes the real ones.

{For each concern: the specific attack/failure mode, the component responsible for mitigation, and how that mitigation is enforced in the code.}

## 8. Testing Strategy

{Overall approach: test types planned (unit / integration / e2e / manual), areas needing the most coverage, infrastructure required. One or two sentences may suffice for simple changes.}

**Key Scenarios:** 3–8 rows. Delete the sample row before submitting. Each `Input` is concrete (specific values, paths, payloads). Each `Expected Behavior` is assertable — a test passes or fails on it. Never write "works correctly" or "behaves as expected".

**Coverage requirement:** every `G#` in §3 appears in at least one row's `Covers` cell — unless that goal is marked `(non-testable: {reason})` in §3, in which case §8 may omit it. Number scenarios `S1`, `S2`, ... so §9 phases can cite them by ID. One scenario may cover multiple goals (e.g., `Covers: G1, G3`).

**`Covers` format (strict):** comma-separated `G#` IDs only — `G1` or `G1, G3`. Not `G1/G3`, `G1 and G3`, `G1 & G3`, or `G1 G3`. Tooling parses this cell by splitting on `,`.

| # | Covers | Scenario | Input | Expected Behavior |
|---|--------|----------|-------|-------------------|
| S1 | G1 | *(sample — delete this row)* Happy path | `{concrete input}` | `{assertable expectation}` |

## 9. Implementation Plan

{2–5 phases. Each independently shippable or independently verifiable. Unit tests written early to define expected behavior and act as the spec for subsequent work.

**Unit-test phase placement:** by default Phase 1 — applies when building features on an existing codebase. If prerequisite work (scaffolding, infrastructure setup) must come first, defer the unit-test phase to the earliest point where tests can actually be written. If the proposal has no unit-testable logic (pure config, docs), omit this phase.

**Done-when discipline:** each phase's "Done when" cites specific `G#` (from §3) or `S#` (from §8) — not just "tests pass" or "code merged". Collectively, every `G#` in §3 must be cited.}

### Phase 1: Unit Tests {RENAME if scaffolding-first applies; OMIT if no unit-testable logic}

- [ ] {Test cases derived from §8 scenarios}

**Done when:** Scenarios S1–S# written and initially failing (red).

### Phase 2: {Core Implementation}

- [ ] {Task with enough detail to be a work item}

**Done when:** {Phase 1 scenarios passing (green); satisfies G#.}

### Phase N: {Name}

- [ ] {Task}

**Done when:** {Specific acceptance criteria citing G# or S#.}

## 10. Risks

{0–6 rows. If none apply, write `No significant risks identified.` and omit the table.}

**Mitigations must be concrete code/config changes** — the mechanism should already be specified in §4 or covered by a §8 scenario. Mitigations may not depend on infrastructure not available in this project (consult the invoking prompt for operational constraints). Don't write "we will monitor" or "we will document".

**Likelihood/Impact values:** only `High`, `Med`, or `Low`. No invented levels (`Critical`, `Catastrophic`, `Trivial`, `N/A`).

Delete the sample row below before submitting.

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| *(sample — delete this row)* {specific failure mode tied to §4} | High / Med / Low | High / Med / Low | {concrete action} |

## 11. Future Work

{0–5 items. Ideas explicitly out of scope for this proposal. Each must trace back to a concrete trade-off in §3 (Non-Goals), §4 (Design), or §6 (Migration). Don't invent aspirational ideas, generic "nice-to-haves", or speculative roadmap items — `None at this time.` is the right answer when no real deferred work exists.}

- {Idea — brief rationale for deferral}

## 12. References

{Real, verifiable items only: file paths in this repo, URLs you've checked, prior RFCs. If none apply, write `None`. Don't fabricate citations.}

- {Related document, code path, external link, or prior RFC}

---

## Pre-submit Checklist (delete before submitting)

Verify each item before submitting. Any unchecked item is a blocker.

- [ ] No `{...}` placeholder tokens remain (check AFTER deleting §8/§10 sample rows, which intentionally contain `{...}` cells).
- [ ] Header date is absolute (not `{YYYY-MM-DD}` or "today").
- [ ] §5/§6/§7 are filled or contain `Not applicable — {reason}`. No section renumbered.
- [ ] §8 and §10 sample rows deleted.
- [ ] All `**Example...**` guidance blocks removed from the body.
- [ ] No fabricated metrics, user quotes, bug IDs, error messages, or commit SHAs.
- [ ] Every §3 `G#` appears in §8's `Covers` (or carries `(non-testable: {reason})`) AND in §9's "Done when".
- [ ] At most 1 Goal in §3 carries the `(non-testable: ...)` mark.
- [ ] Hard caps respected: §1 ≤150 words, §2 ≤200 words, §3 ≤5 Goals, §4.2 ≤5 subsections (≤300 words each), §4.3 1–3 decisions, §7 ≤4 concerns, §8 3–8 scenarios, §9 2–5 phases, §10 ≤6 risks, §11 ≤5 items, total ≤4,000 words.
- [ ] No banned phrases ("we will monitor", "we will document", "comprehensively improve", "leverage", "ensure that", etc. — full list in Writing Instructions).
- [ ] §12 references are all real paths or URLs that were actually checked.
- [ ] `<br>` tags after `**Version:**`, `**Author(s):**`, and `**Date:**` are intact.
- [ ] No `---` horizontal rules remain.
- [ ] Document ends after §12 — no Changelog / History / Revision section appended.
