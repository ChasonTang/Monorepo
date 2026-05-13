# RFC-{NNN}: {Feature Title}

**Version:** 1.0  
**Author:** Chason Tang  
**Date:** {YYYY-MM-DD}  
**Status:** Proposed

## Writing Instructions (delete before submitting)

These rules override the instinct to "fill every section." Treat them as hard constraints.

**Anti-fabrication:**
- **No invented data.** No fake metrics, user quotes, bug IDs, incident dates, benchmark numbers, error messages, or commit SHAs. If quantitative or supporting data is unavailable, include `No data available at this time`; still describe observable behavior and concrete value without inventing evidence.
- **References must be real** — when citing paths in this repo, URLs, or prior RFCs anywhere in the document, verify they exist. Verify any commit SHA with `git cat-file -e <sha>^{commit}` before citing.
- **Code is authoritative; prior RFCs are not.** RFCs capture the design at write-time, but later changes routinely skip RFC review, so the docs drift from the implementation. Before citing a prior RFC's signature, path, schema, flag, or behavior, verify against the current code (read the file, grep the symbol, run the test). When the code disagrees with the RFC, trust the code — either skip the citation or note the divergence in this RFC. Never copy a contract surface from a prior RFC without re-reading the file it documents.

**Strip template artifacts before submitting:**
- Every `{...}` token is a hint, not content — replace or delete the surrounding line. Zero `{YYYY-MM-DD}`, `{Name}`, `{Input}`, etc. should remain. Date is absolute (e.g., `2026-04-23`), never relative ("today").
- Delete every block from `**TEMPLATE EXAMPLE BEGIN**` through `**TEMPLATE EXAMPLE END**` — they are author guidance, not document content.
- Delete sample table rows in §7 (marked `TEMPLATE SAMPLE ROW - delete before submitting`).
- **Delete this Writing Instructions section and the Pre-submit Checklist** — the final RFC ends after §8 Implementation Plan.

**Preserve metadata trailing spaces.** The two trailing spaces (`  `) after `**Version:**`, `**Author:**`, and `**Date:**` are an intentional Markdown line break — without them those fields collapse into a single rendered line. `**Status:**` does not need them (the blank line before `## 1. Summary` ends the paragraph).

**Scope discipline:**
- **Match scope to change.** Simple proposals use minimum counts (1 Goal, 1 §4.2.1 subsection, 3 §7 scenarios, 2 §8 phases).
- **Length:** most RFCs fit in 800–2,500 words; shorter is fine for simple changes. Total length must stay ≤4,000 words; exceeding that usually means padding or over-scope, so split the proposal.
- **Sections §5 and §6 stay numbered.** Never delete or renumber them; when content does not apply or evidence is unavailable, use the template's exact fallback text (`Not applicable — {one-sentence reason}`) instead of filler or speculation.

**Cross-section consistency:**
- Every `G#` in §3 must appear in at least one §4.2 subsection's `Satisfies:` line, at least one §7 row's `Covers` cell, **and** at least one §8 phase's "Done when" clause.
- Every `T#` in §7 must transition red→green across §8 phases — at least one earlier phase's "Done when" lists the row as red (committed but skipped/`xfail`/flag-gated so the project's local test suite stays green), a later phase's "Done when" lists it as green (un-marked, asserting for real).
- Untestable Goals: append `non-testable: {one-sentence reason}` in §3 — at most 1 per RFC. Such goals may be omitted from §7 but must still appear in §4.2 `Satisfies:` and §8 "Done when".

**Avoid vague phrases.** Do not use broad claims as a substitute for specific behavior, metrics, or actions. Rewrite phrases such as "comprehensively improve", "significantly enhance", "robust and scalable", "works correctly", "behaves as expected", "best practices", and "industry standard" into concrete acceptance criteria. "We will monitor" and "we will document" are not valid mitigations unless they name the exact signal, threshold, owner, artifact, or enforcement point.

## 1. Summary

{**Hard cap: 150 words.** One paragraph. State the proposal and core idea — not the backstory (that's §2). A reader should understand the full picture in 30 seconds.}

**TEMPLATE EXAMPLE BEGIN**

**Good:**

> Add a new empty project `numkit` to the Monorepo, exposing `uint32_t gcd(uint32_t a, uint32_t b)` that computes the greatest common divisor using the iterative Euclidean algorithm. Defines `gcd(0, 0) = 0` and `gcd(0, n) = n` so all callers share one zero-input contract. Pure function, no I/O, no external dependencies.

Why it works: one paragraph; names the proposal concretely (new project + signature + algorithm + zero-input contract) without backstory; a reader understands scope in 30 seconds; ~50 words, well under the 150-word cap.

**Bad:**

> We have long lacked a comprehensive math library, so this RFC proposes building a robust, scalable, industry-standard numerical computing platform under the Monorepo to significantly enhance our algorithmic infrastructure. The first milestone is GCD; later milestones will cover primality testing, matrix operations, FFT, and ML-ready primitives. Centralizing GCD will modernize our codebase.

Why it fails: opens with backstory that belongs in §2; vague claims (`comprehensive`, `robust`, `scalable`, `industry-standard`, `significantly enhance`, `modernize`); pads with out-of-scope future work (FFT, ML); never states the actual signature, algorithm, or zero-input contract — a reader cannot tell what ships.

**TEMPLATE EXAMPLE END**

## 2. Motivation

{**Hard cap: 200 words (problem + value combined).**

What problem exists today? Reference real behavior, error messages, or available data. If quantitative or supporting data is unavailable, include `No data available at this time`; still describe the observable problem and concrete value without speculation, generic claims, or fabricated numbers.

What value does solving this bring? Concrete benefits — improved DX, performance, reduced complexity, new capabilities unlocked.}

**TEMPLATE EXAMPLE BEGIN**

**Good:**

> No shared numeric utility exists in the Monorepo today, so each module that needs GCD writes its own. Two concrete pains follow: (1) zero-input semantics drift between copies — some return `0` for `gcd(0, 0)`, others abnormal termination, and there is no canonical reference to point reviewers at; (2) follow-on numeric helpers (`lcm`, fraction reduction) cannot land until GCD has a home. Standing up a new empty `numkit` project with `gcd` as its first export resolves both at minimal cost: pure function, zero dependencies, no caller migration required because the project starts empty. No data available at this time.

Why it works: names specific, observable problems instead of generic complaints; uses the prescribed fallback wording for missing quantitative/supporting data rather than fabricating numbers; ties each value claim (`removes divergence`, `unblocks follow-ups`) back to a listed problem; ~110 words, under the 200-word cap.

**Bad:**

> Engineers waste roughly 200 hours per year reimplementing GCD across our services (see incident #4421), causing significant performance bottlenecks and major maintainability issues. Without this shared utility, our codebase cannot scale to enterprise requirements, and we risk falling behind competitors who already provide industry-standard numeric libraries. Centralizing GCD will robustly transform our math story and unlock the next generation of algorithmic capabilities.

Why it fails: fabricates a metric (`200 hours per year`) and an incident ID (`#4421`) — both forbidden by the anti-fabrication rules; vague phrases (`significant`, `major`, `robustly transform`, `next generation`, `industry-standard`); implausible framing (GCD is not a performance bottleneck); speculation (`cannot scale`, `falling behind competitors`) with no supporting evidence.

**TEMPLATE EXAMPLE END**

## 3. Goals and Non-Goals

{**Hard cap: 1–5 Goals, 0–5 Non-Goals, max 1 non-testable Goal.**

**Goals:** numbered `G1`, `G2`, ... so §7 and §8 can cite them by ID. Each must be concrete and verifiable.

**Means vs Goals.** Goals describe outcomes (e.g., "P95 < 50 ms"), not implementations (e.g., "use Redis"). If swapping implementations invalidates the goal, rewrite it.

**Non-testable Goals (fallback):** if a goal isn't expressible as a §7 scenario (e.g., subjective DX gain), append `non-testable: {one-sentence reason}`. Needing 2+ usually means goals are vague — rewrite as testable outcomes.}

- **G1.** {Concrete, measurable outcome}

{Add G2 through G5 only when each names a genuinely distinct outcome.}

{**Non-Goals:** list only items a reader might mistake for a goal — not the negation of a Goal. Empty → `None`.}

- {Non-Goal with one-sentence reason}

{List up to 5 in total only when each names a separate confusion worth ruling out.}

**TEMPLATE EXAMPLE BEGIN**

**Good:**

> - **G1.** Expose `uint32_t gcd(uint32_t a, uint32_t b)` from a new empty `numkit` project, returning the greatest common divisor for every input pair.
> - **G2.** The public header documents the zero-input contract `gcd(0, 0) = 0` and `gcd(0, n) = gcd(n, 0) = n`; the implementation honors it.
> - **G3.** Unit tests cover the zero-input contract, equal inputs, coprime pairs, one-side-multiple-of-the-other, and the largest `uint32_t` Fibonacci pair `(F47, F46) = (2971215073, 1836311903)`; all pass when the project's local test suite is run.
>
> **Non-Goals:**
>
> - Signed-integer (`int32_t`/`int64_t`) overloads — defer until a caller needs them; locking in overflow semantics now would commit to a contract we cannot test against demand.
> - Other numeric helpers (`lcm`, modular inverse, prime sieve) — they ship as separate exports once `gcd` lands.

Why it works: each `G#` names a verifiable outcome that a §7 scenario can assert (signature exists, header documents the contract, named edge cases pass); G2 pins behavior, not algorithm — swapping iterative Euclidean for binary GCD does not invalidate it; Non-Goals call out the two confusions a reader is most likely to bring (signed inputs, broader math library) with a one-sentence reason each.

**Bad:**

> - **G1.** Build a comprehensive, scalable numeric utility library that significantly improves the Monorepo's math story.
> - **G2.** Implement `gcd` using the iterative Euclidean algorithm in C for optimal performance.
> - **G3.** Add tests.
> - **G4.** Lay the groundwork for future `lcm`, fraction reduction, FFT, and ML primitives.
>
> **Non-Goals:** None.

Why it fails: G1 is vague (`comprehensive`, `scalable`, `significantly improves`) with nothing to verify; G2 prescribes an implementation, not an outcome — swapping to binary GCD should not invalidate the goal, so the algorithm choice belongs in the design section, not §3; G3 is unmeasurable (which tests? what coverage?); G4 lists out-of-scope future work that belongs in Non-Goals, not as a goal of this RFC; declaring `Non-Goals: None` here skips the two confusions (signed inputs, broader math library) a reader actually needs ruled out.

**TEMPLATE EXAMPLE END**

## 4. Design

### 4.1 Overview

{**Describe the components touched and how data/control flows between them.** Contract surfaces (signatures, schemas, transition tables, wire formats) and mechanism sketches belong in §4.2 — keep this section above that level. Include an inline ASCII or Mermaid diagram when the component/flow structure is easier to read as a picture than as prose.

*Skip the diagram* (write `N/A — textual description above is sufficient`) when (a) the change is a single-function, algorithm, or data-structure change whose structure is already conveyed by §1 Summary and the §4.2 description; or (b) the structure cannot be expressed cleanly in ASCII/Mermaid (e.g., dense graphs). Don't invent hierarchy to fill the slot.}

**TEMPLATE EXAMPLE BEGIN**

**Good:**

> A new project `numkit/` is added at the Monorepo root with one public header (`numkit/include/numkit/gcd.h`) and one translation unit (`numkit/src/gcd.c`). The header is the only public surface and brings in `<stdint.h>` and nothing else; the implementation has no other dependencies. No existing module is modified — `numkit` is registered as a new leaf in the top-level project list, and callers opt in by including the header. Control flow is a single synchronous call with no I/O, allocations, or global state.
>
> N/A — textual description above is sufficient.

Why it works: names the concrete files added and the single build-graph change a reviewer needs to inspect; pins the public dependency surface (`<stdint.h>` only) so `numkit` stays a leaf; defers the signature and mechanism to §4.2 as the template directs; invokes the prescribed `N/A` fallback because a single pure function has no flow worth diagramming.

**Bad:**

> `numkit` sits at the foundation of a new layered architecture, providing comprehensive numeric primitives upward through a robust, scalable interface. The new function `uint32_t gcd(uint32_t a, uint32_t b)` runs the iterative Euclidean loop `while (b) { uint32_t t = b; b = a % b; a = t; } return a;` to produce the result. The end-to-end data flow:
>
> ```
> +--------+      +-----+      +--------+
> | Caller | ---> | gcd | ---> | Result |
> +--------+      +-----+      +--------+
> ```

Why it fails: leaks §4.2 content into §4.1 — both the signature and the mechanism belong one section down; substitutes vague filler (`layered architecture`, `foundation`, `comprehensive`, `robust, scalable`) for the actual files added and the build-graph change a reviewer needs to see; invents a three-box "Caller → gcd → Result" diagram for a single pure function, the exact case the template tells you to skip with the `N/A` fallback.

**TEMPLATE EXAMPLE END**

### 4.2 Detailed Design

{**Use 1–5 subsections, one aspect per subsection (≤300 words each).** Most §4.2 sections have only `#### 4.2.1` — add 4.2.2 and beyond only when the change genuinely spans multiple distinct aspects, each with its own contract worth documenting separately. Distinct aspects include: data model, public API, internal algorithm, state machine, wire/serialization format, concurrency/locking model, storage layout, failure/retry policy. Two aspects that share one contract (e.g., a new endpoint and its trivial DTO) stay in one subsection, not two.

**Each subsection pins, in this order:**

1. **The contract surface** — the concrete artifact being defined (signature / schema fragment / state-transition table / wire format / CLI flag), in a code block using the notation the codebase already uses.
2. **The unstated contract** — what a reader could get wrong from the artifact alone: zero/empty-input semantics, ordering, idempotency, error and failure modes, threading, versioning.
3. **The mechanism** — pseudocode, flow sketch, or transition rules.

Each subsection ends with `Satisfies: G# via {one phrase}` so every Goal in §3 has a traceable design hook in at least one subsection. Use `;` to separate when one subsection covers multiple goals (e.g., `Satisfies: G1 via …; G3 via …`).

**Boundary rules:**

- **One aspect per subsection.** Do not split one aspect across `4.2.1` and `4.2.2`.
- **If 300 words is not enough,** the aspect is not actually one aspect — split it into finer sub-aspects (contract / algorithm / state / wire format), or scope the RFC down.
- **>5 subsections → the RFC is too large; split it.**
- **Use descriptive aspect names** — `#### 4.2.1 Wire Format`, not `#### 4.2.1 Details`.

**Content rules:**

- *Show the contract, not the code.* Exact signatures, schemas, message formats, or transition-table rows go in code blocks. Skip glue, error plumbing, and obvious branches.
- *Snippet length and source.* ≤30 lines per block. For code that already exists in the repo, replace the snippet with a `path/to/file.ext:line` reference. For Proposed code that does not yet exist, write pseudocode in the language of the affected codebase (or near-pseudocode) one level above the implementation — no error-handling boilerplate, no language-specific sugar.
- *Pseudocode discipline.* Name every input, every output, and every observable side effect. `// handle the error` and `// process input` are placeholders, not pseudocode.
- *Notation matches the affected codebase.* SQL DDL for relational schema changes; JSON Schema / Protobuf / OpenAPI for wire formats; BNF for grammars; transition tables when the machine has >2 states or >3 transitions. Pick what the rest of the project uses; do not introduce a new notation for one RFC.
- *Stay inside the component.* §4.1 says which boxes change; §4.2 says what happens inside one box. Do not redraw component diagrams or list which files are added — that belongs to §4.1.
- *Cite, don't paste, large artifacts.* Full schema files, full state diagrams, and 100-line algorithms belong in the diff or a linked file — quote only the smallest fragment that pins the contract.}

#### 4.2.1 {Aspect: see distinct-aspects list above}

{One aspect of the design at implementation level. End with `Satisfies: G# via {one phrase}` (use `;` to separate when one subsection covers multiple goals).}

{Add §4.2.2 through §4.2.5 only when each covers a genuinely distinct aspect.}

**TEMPLATE EXAMPLE BEGIN**

**Good:**

> #### 4.2.1 Public API and Algorithm
>
> ```c
> /* numkit/include/numkit/gcd.h */
> #include <stdint.h>
>
> uint32_t gcd(uint32_t a, uint32_t b);
> ```
>
> **Unstated contract.** `gcd(0, 0)` returns `0`; `gcd(0, n)` and `gcd(n, 0)` return `n`. These are pinned in the header doc-comment — they cannot be inferred from the signature and are the canonical reference §2 names as missing today. Pure function: no I/O, no allocation, no global state, and no failure path (so no return-code overload, no `errno` write). Thread-safe because both arguments pass by value and the body touches no shared state.
>
> **Mechanism (iterative Euclidean).**
>
> ```
> while b != 0:
>     t = b
>     b = a mod b
>     a = t
> return a
> ```
>
> Termination: `a mod b < b`, so the second argument strictly decreases until it reaches 0. The `(0, 0)` and `(n, 0)` cases skip the loop entirely and return `a` directly, which gives the contract above with no special-case branch. Worst case is `O(log(min(a, b)))` iterations on Fibonacci-pair input.
>
> Satisfies: G1 via the exported signature; G2 via the zero-input contract pinned in the header doc-comment; G3 via the pure, allocation-free design that lets each §7 edge-case row assert on a single return value.

Why it works: one subsection because signature, unstated contract, and mechanism share one aspect (the public function), as the rules direct; the contract surface uses C — the codebase's notation — and shows only the signature, deferring the body to pseudocode; the **Unstated contract** paragraph names the four things a reader could miss from the signature alone (zero-input semantics, purity, no error path, thread-safety); the pseudocode is one level above the implementation with every variable named; the termination paragraph confirms the zero-input contract falls out of the same loop, so no special-case branch is needed (a subtlety the Bad example below gets wrong); leaves algorithm-choice rationale (e.g., why not binary GCD?) for §4.3 rather than muddling it into the Mechanism paragraph; closes with a `Satisfies:` line that traces back to all three goals from §3; well under the 300-word cap.

**Bad:**

> #### 4.2.1 Function
>
> The new project `numkit/` lives at the Monorepo root, with header `numkit/include/numkit/gcd.h` and implementation `numkit/src/gcd.c`. We use the iterative Euclidean algorithm for optimal performance on modern CPUs.
>
> ```c
> #include "numkit/gcd.h"
> #include <stdint.h>
>
> uint32_t gcd(uint32_t a, uint32_t b) {
>     if (a == 0 && b == 0) {
>         return 0;
>     }
>     while (b != 0) {
>         uint32_t t = b;
>         b = a % b;
>         a = t;
>     }
>     return a;
> }
> ```
>
> #### 4.2.2 Edge Cases
>
> Zero inputs are handled by the early return above. The function works correctly for all valid `uint32_t` pairs.
>
> #### 4.2.3 Performance
>
> Runs in O(log n) time, which is fast enough.

Why it fails: aspect name `Function` is generic — the rule requires descriptive names like `Public API and Algorithm`; the opening paragraph lists which files are added and where the project sits, but that is §4.1's job — §4.2 stays inside the box; pastes the full `.c` instead of pseudocode, so the snippet ships with an `if (a == 0 && b == 0) return 0;` early return that is dead code (when `b == 0` the `while` loop is skipped and `return a` already yields `0` for the `(0, 0)` input) — pseudocode would have surfaced that the loop alone covers the zero-input contract; splits one aspect across `4.2.1`/`4.2.2`/`4.2.3` for no reason — `Edge Cases` and `Performance` are notes about the same function, not distinct aspects with their own contracts; "works correctly", "fast enough", and "optimal performance" are exactly the vague phrases the writing instructions ban; no `Unstated contract` paragraph, so the zero-input semantics live only in the (redundant) code; missing the closing `Satisfies: G# via …` line on every subsection, so §3 goals have no traceable design hook.

**TEMPLATE EXAMPLE END**

### 4.3 Design Rationale

{**Hard cap: 0–3 decisions.** Document only decisions a future reader would actually question — concretely, an entry is worth recording only if (a) swapping the choice would invalidate at least one `G#`, or (b) a reviewer would predictably ask "why not X". Neither passes → micro-choice, do not record.

Distinct from §3 Non-Goals: Non-Goals exclude scope ("we won't build X"); §4.3 explains within-scope choices ("we chose A over B"). When §4.2's Mechanism, contract surface, or notation silently carries a "we picked X over Y", lift the comparison to §4.3 and leave only the chosen artifact in §4.2.

For each, use a 3-line structure:
- **Chosen:** ...
- **Reason:** ...
- **Ruled out:** ...

`None` is a common legitimate state, not a failure signal — when §3 Goals and §4.2 contract surface have already locked every within-scope choice, write `None` and stop. Do not invent decisions to fill the slot. If an entry passes the worth-recording test above but no real alternative was considered, write `Ruled out: N/A — no viable alternative considered` rather than inventing a strawman.

§4.3 entries do not need to trace to §3 Goals, §7 rows, or §8 phases — decisions stand independently.}

**TEMPLATE EXAMPLE BEGIN**

**Good:**

> - **Chosen:** Iterative Euclidean (the loop pinned in §4.2.1).
> - **Reason:** Worst case is `O(log(min(a, b)))` — under 50 iterations for any `uint32_t` input pair (Fibonacci-pair worst case bounded by `~1.44 · log₂(n)`), well under any caller's budget. The `%` operator compiles to a single hardware-divide instruction on every platform this Monorepo targets, so the loop body is already at the noise floor; trading it for shifts and subtractions would not measurably help.
> - **Ruled out:** Binary GCD (Stein's algorithm) replaces `%` with shifts and subtractions; the speedup matters only on targets without a hardware divider (older microcontrollers). Revisit when such a target lands in the Monorepo — until then the simpler algorithm with the easier termination proof is the safer default.

Why it works: one decision is enough — signature, zero-input contract, and file layout are pinned by §3 and §4.2 with no real alternative, so they do not belong here; **Reason** quantifies the budget (under 50 iterations on `uint32_t`, single hardware-divide instruction) instead of hand-waving about "good performance"; **Ruled out** names the real alternative *and* the specific condition (no hardware divide) that would flip the decision, so the next author has a checklist to revisit against — not a strawman; lifts the binary-GCD verdict out of §4.2.1's Mechanism paragraph (where it conflated "how the loop works" with "why we picked this loop") and lands it where within-scope alternatives belong.

**Bad:**

> - **Chosen:** Iterative Euclidean — the classic, well-known, battle-tested GCD algorithm.
> - **Reason:** Clean, elegant, robust algorithm with optimal performance that works correctly for every input.
> - **Ruled out:** Recursive Euclidean (stack overflow risk), trial division (way too slow), and other less suitable approaches.
>
> - **Chosen:** Return type `uint32_t`.
> - **Reason:** Matches the input type.
> - **Ruled out:** Out-parameter, struct return.
>
> - **Chosen:** Function name `gcd`.
> - **Reason:** Short and conventional.
> - **Ruled out:** `greatest_common_divisor`, `compute_gcd`.

Why it fails: `Reason` uses every vague phrase the writing instructions ban ("clean, elegant, robust", "optimal performance", "works correctly") with no numbers or named platform — a future reader cannot tell *why* this algorithm beats the alternatives; `Ruled out` invents strawmen (recursive Euclidean is not a stack-overflow risk at the few-dozen-frame depth `uint32_t` inputs allow, trial division is so absurd no reviewer would ask about it) and skips the one alternative a real reviewer would actually raise — binary GCD; pads with two micro-choices (return type matching the input, function naming) that no future reader would re-litigate — they belong in §4.2's contract surface, not §4.3, and exist here only to fill the slot; the 0–3 cap exists to prevent exactly this kind of noise — burying the one real decision (binary GCD) inside a list of cosmetic ones.

**TEMPLATE EXAMPLE END**

## 5. Backward Compatibility & Migration

{**Skip when** previously-working callers see no new failure (additive APIs, brand-new exports, or refactors invisible at the public boundary) → write `Not applicable — {one-sentence reason}` and stop.

**Hard cap: 1–5 entries.** Each entry pins one observable way a previously-working caller stops working; fan-out from one root change collapses into one entry, independent breaks split. >5 means the RFC bundles too much — split it.

**Per-break structure (3 lines):**

- **Breaks:** the exact signature, schema, wire format, or semantic that changes — cite the §4.2 subsection that pins the new contract.
- **Symptom on un-migrated caller:** the exact compile error, runtime exception, log line, wire mismatch, or wrong-but-silent output the caller observes. "It will fail" / "callers must update" / "behavior changes" are placeholders, not symptoms.
- **Migration:** the concrete command, codemod, version bump, or config flip that resolves the symptom. If non-mechanical, say so and cite the existing runbook — never "we will document".

**Don't:**

- Invent a break to fill the slot — if the RFC is additive, Skip.
- List breaks still hidden behind a deprecation alias — record them the cycle the alias is removed.
- List internal refactors or cosmetic renames invisible at the public boundary.

**Cross-section consistency:** every entry implies at least one §7 row exercising the new behavior on previously-succeeding input.}

**TEMPLATE EXAMPLE BEGIN**

**Good:**

> Not applicable — `numkit/gcd` is a brand-new export with no prior callers; nothing that compiled or ran before this RFC changes behavior.

Why it works: invokes the Skip clause for the genuine reason — a newly added export breaks nothing that previously existed — instead of manufacturing a migration story to fill the slot; uses the prescribed `Not applicable — {one-sentence reason}` wording verbatim so the checklist parser accepts it; one short sentence and stops, exactly as the rule directs.

**Bad:**

> - **Breaks:** Existing math code may need updates.
> - **Symptom on un-migrated caller:** Build errors or unexpected results.
> - **Migration:** We will document the upgrade path before release.

Why it fails: the GCD RFC adds a new file to a project that previously had no GCD function — there is *nothing* to break, so the right answer is the Skip clause, not invented rows; "Existing math code may need updates" names no signature, no §4.2 hook, and no observable change — the vague placeholder the rule forbids; "Build errors or unexpected results" wraps a vague second clause around an over-broad first ("build errors") and joins them by `or`, so a caller cannot tell which to grep for — a real Symptom names one observable per entry, e.g., `error: too few arguments to function 'gcd'`; "We will document the upgrade path before release" is the banned `we will document` pattern, with the future-tense framing making the §5 entry depend on an artifact that does not yet exist when the RFC is reviewed.

**TEMPLATE EXAMPLE END**

## 6. Security

{**Skip if** the code path does not cross a trust boundary — no external or attacker-controlled input, no credentials/keys, no headers/URLs derived from input, no upstream responses, no authentication or authorization, no persisted data → write `Not applicable — {one-sentence reason}` and stop.

**Hard cap: 1–4 concerns.** Each entry pins one specific attack or failure mode plus its mitigation; variants of the same attack (e.g., several malformed-input shapes the same parser rejects) collapse into one entry, distinct attacks split. >4 means the RFC bundles a security-sensitive surface large enough to split.

**Per-concern structure (3 lines):**

- **Threat:** the specific attack/failure mode plus the trigger (input shape, operation, or caller) that exposes it. "Possible vulnerability" / "may be unsafe" / "input must be validated" are placeholders, not threats.
- **Mitigation:** the code-level check, library call, schema constraint, or config flag that prevents it — cite the §4.2 subsection (or §4.1 component) that pins the enforcement point. "Validate input" / "sanitize before use" without naming the validator are placeholders.
- **Enforcement:** the concrete mechanism that proves the mitigation is in place — the §7 row that fires the trigger input, the static-analysis lint that fails the build, the config value committed to the repo. "We will harden" / "code review will catch it" / "monitoring will alert" are placeholders, not enforcement.

**Don't:**

- Pad with generic threats (XSS, SQLi, CSRF, buffer overflow, path traversal, ...) the code path does not actually expose — every "N/A" row a reviewer reads to dismiss steals attention from the real concern. If every entry would be non-applicable, invoke the Skip clause instead.
- List threats already neutralized upstream (framework escaping, platform sandbox, transport TLS) unless this RFC introduces a new path that bypasses them — record the bypass, not the upstream defense.
- Cite a Mitigation that does not yet exist in code — "we will add a sanitizer", "we plan to enforce ..." — the enforcement point must be in this RFC's diff or already in the repo before §6 cites it.

**Cross-section consistency:** every entry implies at least one §7 row that exercises the trigger input and asserts the rejection, sanitization, or safe outcome.}

**TEMPLATE EXAMPLE BEGIN**

**Good:**

> Not applicable — `numkit/gcd` takes two `uint32_t` arguments by value, performs no I/O or allocation, reads no external state, and exposes no credentials, so no input crosses a trust boundary that did not exist before this RFC.

Why it works: invokes the Skip clause for the genuine reason — every category named in the Skip rule (external input, credentials, headers, upstream responses, authentication, persisted data) is absent because two pass-by-value integers carry no attacker-controlled bytes and the function touches no external state; uses the prescribed `Not applicable — {one-sentence reason}` wording verbatim so the checklist parser accepts it; one short sentence and stops, exactly as the rule directs.

**Bad:**

> - **Threat:** Buffer overflow.
> - **Mitigation:** Use safe C coding practices.
> - **Enforcement:** Code review and static analysis will catch issues before merge.
>
> - **Threat:** SQL injection.
> - **Mitigation:** N/A — gcd does not interact with databases.
> - **Enforcement:** N/A.
>
> - **Threat:** Denial of service from very large inputs.
> - **Mitigation:** Iterative Euclidean is O(log n).
> - **Enforcement:** A performance test will be added.

Why it fails: `gcd` takes two `uint32_t` arguments by value with no array access, pointer arithmetic, or `memcpy`, so "Buffer overflow" is impossible — the "code path actually exposes that surface" trigger from the Don't rule is not met, and "safe C coding practices" plus "code review and static analysis will catch issues" trips both Mitigation and Enforcement placeholder bans (no specific lint, no §4.2 hook, no §7 row); the SQL-injection row is dilution in pure form — three lines that read "N/A" only steal attention from real concerns, and the right fix is to delete the row entirely (or, since every row here is non-applicable, invoke the Skip clause for the whole section); "Denial of service from very large inputs" fabricates scope — `O(log(min(a, b)))` on `uint32_t` runs in under 50 iterations even on the Fibonacci-pair worst case, so no caller can DoS `gcd`, and "A performance test will be added" is the banned future-tense `we will document` pattern with no §7 row to cite; this RFC's correct §6 is the Good example's Skip clause.

**TEMPLATE EXAMPLE END**

## 7. Testing Strategy

{**Hard cap: 3–10 scenarios.** Each row is one executable test case tied to a §3 Goal (and, when applicable, a §5 entry or §6 concern). >10 means rows conflate sibling scenarios — collapse inputs that share one Goal and one setup pattern into one row.

**Per-row structure.** One markdown table, columns in this order:

| # | Scenario | Input / Setup | Expected Result | Covers | Level |
|---|----------|---------------|-----------------|--------|-------|

- **#** — stable row ID (`T1`, `T2`, ...) so §8 phases can cite rows by ID.
- **Scenario** — one phrase naming the behavior under test; not the test-function name.
- **Input / Setup** — concrete input values or fixture state. "Valid input" / "typical case" are placeholders, not setups.
- **Expected Result** — the observable outcome the test asserts (exact return value, exact error variant, exact wire bytes, log line). "Works correctly" / "behaves as expected" are banned.
- **Covers** — the `G#` (and optionally `§5` / `§6` entry) the row exercises. One row may cover multiple items; use `,` to separate.
- **Level** — `unit`, `integration`, or `e2e`, matching the codebase's harness vocabulary. Pick the cheapest level that still exercises the contract; escalate only when the contract crosses a boundary the cheaper level cannot reach.

**Don't:**

- List rows that only re-assert the type-checker ("signature compiles", "header parses") — the build already proves that.
- Quote implementation detail ("branch X is taken", "loop iterates N times") instead of observable outcome — tests must survive refactors that preserve the contract.
- Inflate rows per Goal to satisfy `Covers` — if one setup exercises multiple Goals, one row covers all of them.
- Defer with "will add later" / "to be determined" — an undecided row is not a strategy.

**Cross-section consistency:** every `G#` in §3 appears in at least one `Covers` cell (untestable Goals per §3 excepted). Every §5 entry pairs with at least one row exercising the new behavior on previously-succeeding input. Every §6 concern pairs with at least one row firing the trigger input and asserting the safe outcome.}

| # | Scenario | Input / Setup | Expected Result | Covers | Level |
|---|----------|---------------|-----------------|--------|-------|
| T1 | TEMPLATE SAMPLE ROW - delete before submitting | ... | ... | G1 | unit |

**TEMPLATE EXAMPLE BEGIN**

**Good:**

> | # | Scenario | Input / Setup | Expected Result | Covers | Level |
> |---|----------|---------------|-----------------|--------|-------|
> | T1 | Zero-input contract | `gcd(0, 0)`, `gcd(0, 7)`, `gcd(11, 0)` | Returns `0`, `7`, `11` respectively | G2 | unit |
> | T2 | Equal inputs | `gcd(42, 42)` | Returns `42` | G1 | unit |
> | T3 | Coprime pair | `gcd(17, 31)` | Returns `1` | G1 | unit |
> | T4 | One side multiple of the other | `gcd(12, 36)`, `gcd(100, 25)` | Returns `12`, `25` respectively | G1 | unit |
> | T5 | Worst-case Fibonacci pair (F47, F46) | `gcd(2971215073u, 1836311903u)` | Returns `1` | G1, G3 | unit |

Why it works: five rows line up one-to-one with the five edge cases G3 enumerates (zero-input, equal, coprime, one-side-multiple, worst-case), each with exact inputs and exact return values a future author can port straight into a test function; T1 folds the three zero-input sub-cases the header pins into one row because they share Goal (G2) and setup pattern — splitting them into three rows only to pad `Covers` is exactly what the "Don't inflate" rule forbids; T5 picks the actual Fibonacci-pair worst case named in §4.2.1's Mechanism paragraph (consecutive `UINT32_MAX` values would be coprime and finish in 2 steps, missing the bound entirely), so G3's "passes when the project's local test suite is run" clause gets a concrete hook on the worst-case input and G1 rides along for free; every `G#` from §3 appears in at least one `Covers` cell (G1 in T2/T3/T4/T5, G2 in T1, G3 in T5), clearing the cross-section consistency rule; Level stays `unit` throughout because a pure function has no integration boundary — escalating to `integration` or `e2e` would be ceremony the "pick the cheapest level" rule forbids.

**Bad:**

> - Unit tests will verify that `gcd` works correctly across all valid inputs.
> - Integration tests will ensure compatibility with downstream consumers.
> - Performance benchmarks will confirm optimal runtime.
>
> Comprehensive edge-case coverage will be added before release.

Why it fails: prose instead of the required table — no `#`, no `Covers`, no row IDs for §8 phases to cite, and the checklist parser has no rows to match against §3; "works correctly", "ensure compatibility", "optimal runtime", and "comprehensive edge-case coverage" tick every vague phrase the writing instructions ban, all in one paragraph; "Integration tests" invents a downstream consumer — `numkit` is a brand-new leaf with no callers today, so the cheapest level that exercises the contract is `unit`, and the "pick the cheapest level" rule forbids the escalation; "Performance benchmarks" repeats the §6 Bad example's fabricated DoS scope — `O(log(min(a, b)))` on `uint32_t` completes in under 50 iterations, so no runtime gate is worth asserting; "will be added before release" is the banned future-tense pattern with no concrete input, no expected result, and no `G#` traceability — the reviewer has nothing to approve.

**TEMPLATE EXAMPLE END**

## 8. Implementation Plan

{**Hard cap: 2–5 phases.** Each phase is a mergeable unit that leaves the project's local test suite green on its own — not a calendar week, not a sprint, not an OKR milestone. Add a phase only when the prior one must land and be verified before the next can start (public API before the tests that import it, flag-off deploy before flag-on default, schema migration before code that depends on the new columns). >5 phases means the RFC bundles too much — split it.

**TDD red→green is the default phase ordering.** Each §7 row lands first in a "red" phase (test committed in failing state, but kept off the project's local test suite via `xfail`/`expected-fail` markers, `skip`/`pending` markers, a feature flag that gates the test, or a separate test target not yet wired into the local test runner) before a later "green" phase that implements the behavior and removes the marker so the test asserts for real when the local test suite is run. Name the chosen red-phase mechanism in the red phase's `Scope` so a reviewer can verify the gate from the diff. This guarantees ≥2 phases for any RFC that adds behavior — a single phase shipping a `T#` row alongside its own implementation erases the verifiable transition the workflow exists to enforce.

**Per-phase structure (3 lines):**

- **Scope:** the concrete deliverables this phase alone ships — files added or modified, migrations run, flags flipped, test binaries registered. "Implement the feature" / "add tests" / "polish" are placeholders, not scope.
- **Depends on:** the prior phase IDs (`P#`) this phase requires to land first, or `None` for the first phase. Name phases by ID, never by week or date.
- **Done when:** the observable acceptance criteria that close the phase — cite §7 rows by `T#` and §3 Goals by `G#` so a reader can verify completion by grepping the referenced artifacts. "Code review passes" / "tests green" without naming which rows are placeholders.

**Don't:**

- List dates or owners — an RFC reviews the plan, not the calendar or staffing, and both drift the moment schedules slip; phase IDs (`P1`, `P2`) survive re-ordering but "Week 3" does not.
- Open with a "planning" phase or close with a "monitoring" phase — this RFC *is* the planning artifact, and a monitoring-only phase has no mergeable deliverable to close on.
- Fold a real ordering constraint into one phase — if tests need the header to exist first, that is two phases with a `Depends on` edge, not one "implement + test" blob that erases the merge gate.
- Ship a §7 row alongside its own implementation in one phase — that collapses TDD red→green into one commit, the project's local test suite never observes the row in red state, and the cross-section rule that each `T#` traces through both a red and a green phase fails.
- Defer the hard part with a bare placeholder — "P3: handle edge cases later" without a concrete `Scope` and `Done when` is the future-tense "we will document" pattern the writing instructions ban.

**Cross-section consistency:** every `G#` from §3 appears in at least one phase's `Done when` clause (non-testable Goals per §3 included). Every `T#` from §7 transitions red→green across phases — an earlier phase's `Done when` names the row as red (committed but skipped/`xfail`/flag-gated, so the local test suite stays green), a later phase's `Done when` names it as green (un-marked, asserting for real). Every §5 migration entry lands in the phase that introduces the contract break — never later, so un-migrated callers never see the new symptom without the migration shipping in the same phase. Every §6 security concern lands in the phase that exposes the trigger surface, so the mitigation merges no later than the code it protects.}

- **P1. {Phase name — one phrase naming the deliverable}.**
  - **Scope:** {concrete files, migrations, flags this phase ships}
  - **Depends on:** None
  - **Done when:** {observable criteria citing `T#` and `G#`}

{Add P2 through P5 only when the prior phase is a genuine merge gate the next phase cannot cross.}

**TEMPLATE EXAMPLE BEGIN**

**Good:**

> - **P1. Land `numkit` skeleton with red `T1`–`T5`.**
>   - **Scope:** add `numkit/include/numkit/gcd.h` per §4.2.1 (signature + zero-input doc-comment pinned in the **Unstated contract** paragraph); add `numkit/src/gcd.c` with a stub body `return 0;` so the project links; add `numkit/test/gcd_test.c` containing the five rows from §7 as separate test cases, each wrapped in the harness's `TEST_SKIP("pending RFC-NNN P2")` macro so the binary builds, runs, and reports the rows as skipped — the local test suite stays green; register `numkit` as a new leaf in the top-level project list per §4.1; register the test binary in the project's local test runner so it runs whenever the local test suite is invoked.
>   - **Depends on:** None.
>   - **Done when:** the `uint32_t gcd(uint32_t, uint32_t)` signature from §4.2.1 exports from the header (G1); the header doc-comment pins `gcd(0, 0) = 0` and `gcd(0, n) = gcd(n, 0) = n` verbatim (G2); `T1`–`T5` are committed in skipped (red) state and the local test suite passes after the test binary registers (G3 staged red).
>
> - **P2. Implement `gcd` and turn `T1`–`T5` green.**
>   - **Scope:** replace the stub body in `numkit/src/gcd.c` with the iterative Euclidean loop pinned in §4.2.1's Mechanism paragraph; remove the `TEST_SKIP` wrappers from `T1`–`T5` so they assert for real on every local test run.
>   - **Depends on:** P1.
>   - **Done when:** `T1`–`T5` all pass un-skipped on the first clean local test run after the wrappers come off (G3 green).

Why it works: two phases — matching the TDD red→green default and the simple-proposal minimum — because P1 lands the contract surface (header + linkable stub + test rows) with every §7 row committed but skipped (red), and P2 turns the rows green by replacing the stub with the real loop and removing the skip wrappers; P1 ships the header alongside a stub `.c` so the project still builds and links, satisfying the "phase leaves the local test suite green" rule even though no real implementation has landed yet; the red phase keeps the local test suite green via the `TEST_SKIP` macro the rule requires, and the wrapper is named in P1's `Scope` so a reviewer can verify the gate from the diff alone; P2 names P1 as a `Depends on` because the skip wrappers can only come off once the implementation exists, so the edge is a real merge gate instead of ceremony; each `T#` traces through both phases — red in P1's `Done when`, green in P2's — satisfying the §7 → §8 red→green cross-section rule; `Done when` clauses cite the exact §7 row IDs (`T1`–`T5`) and §3 Goal IDs (`G1`, `G2`, `G3`) so every Goal has a traceable completion hook a reviewer can verify by grepping the §7 table and the header doc-comment; P1's `Done when` references §4.2.1's **Unstated contract** paragraph by name, so the zero-input contract cannot get dropped between design and merge; no dates, no owners, no "phase 0: design review", no "phase N: monitor" — matches every item on the Don't list.

**Bad:**

> - **Phase 1 (Week 1):** Design and kickoff. Owner: Alice.
> - **Phase 2 (Week 2–3):** Implement `gcd` and write comprehensive tests. Owner: Bob.
> - **Phase 3 (Week 4):** Code review and address review feedback. Owner: TBD.
> - **Phase 4 (Week 5+):** Monitor production and iterate on edge cases as they surface.

Why it fails: every phase is a calendar week (`Week 1`, `Week 2–3`, …) — the "a phase is a mergeable unit, not a timebox" rule forbids this directly, and any slip on Phase 1 cascades into a meaningless "Phase 2 is late" status with no merge gate a reviewer can point at; lists owners (`Alice`, `Bob`, `TBD`) — the Don't list bans staffing because it drifts and an RFC reviews the plan, not who holds the pager; "Phase 1: Design and kickoff" is the banned open-with-planning pattern — this RFC *is* the design artifact, so Phase 1 cannot also be design; "Implement `gcd` and write comprehensive tests" collapses two phases with a real ordering edge (tests cannot compile before the header merges) into one, erasing the verification point a reviewer needs to grant or refuse Phase 2 — and worse, ships every §7 row alongside its own implementation, so the local test suite never observes any `T#` in red state and the TDD red→green transition the rule requires never occurs; "comprehensive tests" is the banned vague phrase with no `T#` row to cite and no `G#` to prove complete; Phase 3 ("code review and address review feedback") is not a deliverable — every phase ships through code review, so promoting it to a phase pads the count without adding a merge gate; Phase 4 ("monitor production and iterate") has no `Scope`, no `Depends on`, no `Done when`, and no end state — a monitoring-only phase has no mergeable deliverable to close on; no phase declares a `Done when` line, so none of `G1`–`G3` has a traceable completion hook and the cross-section consistency rule fails on every Goal.

**TEMPLATE EXAMPLE END**

## Pre-submit Checklist (delete before submitting)

Walk every box before submitting; any unchecked item is a blocker. The categories below mirror the Writing Instructions at the top of this template — if a check fails, the fix is in the rule it cites.

**Template scaffolding removed.**

- [ ] No `TEMPLATE SAMPLE ROW - delete before submitting` row remains in §7.
- [ ] Every `**TEMPLATE EXAMPLE BEGIN**` through `**TEMPLATE EXAMPLE END**` block is removed from the body of §1–§8.
- [ ] No `{...}` placeholder token remains.
- [ ] The Writing Instructions section at the top and this Pre-submit Checklist are both removed; the body ends after §8 Implementation Plan with no Future Work, References, Changelog, History, or Revision section appended.

**Header metadata intact.**

- [ ] Header date is absolute (e.g., `2026-04-23`); no `{YYYY-MM-DD}`, no "today", no other relative form.
- [ ] The two trailing spaces (`  `) after `**Version:**`, `**Author:**`, and `**Date:**` are preserved so each field renders on its own line; `**Status:**` does not carry them.

**No fabricated content.**

- [ ] No invented metrics, user quotes, bug IDs, incident dates, benchmark numbers, error messages, or commit SHAs.
- [ ] Every in-repo path, URL, and prior-RFC citation has been verified to resolve; every cited commit SHA passes `git cat-file -e <sha>^{commit}`.
- [ ] Every prior-RFC citation has been cross-checked against the current code (signature, file path, schema, flag, behavior); where the code disagrees with the RFC, this document follows the code, not the RFC.
- [ ] When quantitative/supporting data is missing, §2 still describes the observable problem and concrete value, and includes the exact phrase `No data available at this time`; §5 and §6 use the exact phrase `Not applicable — {one-sentence reason}`. Neither section pads with speculation.

**Section shape and hard caps.**

- [ ] §5 and §6 remain numbered — neither deleted nor renumbered — and are either filled or carry the prescribed `Not applicable — {one-sentence reason}` fallback.
- [ ] Total length ≤ 4,000 words (most RFCs fit in 800–2,500).
- [ ] §1 is one paragraph ≤ 150 words, stating the proposal and its core idea without backstory.
- [ ] §2 ≤ 200 words (problem + value combined).
- [ ] §3 has 1–5 Goals numbered `G1`, `G2`, …; 0–5 Non-Goals (empty → `None`); at most one Goal carries `non-testable: {one-sentence reason}`. Goals name outcomes, not implementations.
- [ ] §4.1 supplies a diagram or uses the exact `N/A — textual description above is sufficient` fallback.
- [ ] §4.2 has 1–5 subsections with descriptive aspect names, each ≤ 300 words; each subsection pins in order the **contract surface**, the **unstated contract**, and the **mechanism**, then closes with a `Satisfies: G# via {one phrase}` line (use `;` to separate when one subsection covers multiple goals).
- [ ] §4.3 documents 0–3 decisions (or the literal `None`), each as `Chosen` / `Reason` / `Ruled out`; when no real alternative was considered, the `Ruled out` line reads `N/A — no viable alternative considered`.
- [ ] §5 has 1–5 break entries (or the Skip fallback), each as `Breaks` / `Symptom on un-migrated caller` / `Migration`, each citing the §4.2 subsection that pins the new contract.
- [ ] §6 has 1–4 concern entries (or the Skip fallback), each as `Threat` / `Mitigation` / `Enforcement`; no padding with generic threats (XSS, SQLi, buffer overflow, …) the code path does not actually expose.
- [ ] §7 uses the single prescribed `# | Scenario | Input / Setup | Expected Result | Covers | Level` table with 3–10 rows; row IDs (`T1`, `T2`, …) are stable so §8 phases can cite them; each `Level` names the cheapest harness that exercises the contract.
- [ ] §8 has 2–5 phases, each as `Scope` / `Depends on` / `Done when`; phases are mergeable units — never calendar weeks, never a "planning" phase, never a monitoring-only phase; no owners or dates appear; `Depends on` uses phase IDs (`P1`, `P2`, …) or `None`.
- [ ] §8 follows TDD red→green: every §7 row lands first in a red phase (test committed via `xfail`/`skip`/feature-flag so the local test suite stays green) before a later green phase that removes the marker; the red-phase mechanism is named in the red phase's `Scope`. No phase ships a §7 row alongside its own implementation.

**Cross-section traceability.**

- [ ] Every `G#` in §3 is named in at least one §4.2 subsection's `Satisfies:` line.
- [ ] Every testable `G#` in §3 appears in at least one §7 row's `Covers` cell; Goals tagged `non-testable:` may skip §7 but still need §4.2 and §8 hooks.
- [ ] Every `G#` in §3 (testable and non-testable alike) appears in at least one §8 phase's `Done when` clause.
- [ ] Every `T#` in §7 transitions red→green across §8 phases: an earlier phase's `Done when` names the row as red (committed but skipped/`xfail`/flag-gated, so the local test suite stays green), a later phase's `Done when` names it as green (un-marked, asserting for real).
- [ ] Every §5 entry is paired with at least one §7 row exercising the new behavior on previously-succeeding input, and the migration lands in the §8 phase that introduces the break.
- [ ] Every §6 concern is paired with at least one §7 row firing the trigger input and asserting the safe outcome, and the mitigation lands in the §8 phase that exposes the trigger surface.

**Language discipline.**

- [ ] No vague substitute phrases ("we will monitor", "we will document", "comprehensively improve", "significantly enhance", "robust and scalable", "works correctly", "behaves as expected", "best practices", "industry standard"); each claim names a specific behavior, metric, action, owner, threshold, or enforcement point.
