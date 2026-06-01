# RFC-{NNN}: {Feature Title}

## Writing Instructions (delete before submitting)

These rules override the instinct to "fill every section." Everything below is a hard constraint. The one exception is the per-section counts and lengths, and only upward: the lower bound of every stated range (e.g. `Recommended: 3–10`) is a hard minimum — a section with a Skip/None clause may invoke that instead — while the upper figure is a recommendation you may exceed when the proposal genuinely warrants it. Lengths have no hard floor; shorter is fine for simple changes.

**Do not fabricate.** Do not invent facts, code paths, APIs, behavior, data, test results, citations, or rationale. If evidence is missing, state that it is unknown or needs verification, then name the source that must be checked.

**Code is authoritative; prior RFCs are not.** RFCs capture the design at write-time, but later changes routinely skip RFC review, so the docs drift from the implementation. Before citing a prior RFC's signature, path, schema, flag, or behavior, verify against the current code (read the file, grep the symbol, run the test). When the code disagrees with the RFC, trust the code — either skip the citation or note the divergence in this RFC. Never copy a contract surface from a prior RFC without re-reading the file it documents.

**Strip template artifacts before submitting:**
- Every `{...}` token is a hint, not content — replace or delete the entire `{...}` block (which may span multiple lines). Zero `{Name}`, `{Input}`, etc. should remain.
- Delete every block from `**TEMPLATE EXAMPLE BEGIN**` through `**TEMPLATE EXAMPLE END**` — they are author guidance, not document content.
- Delete sample table rows in §6 (marked `TEMPLATE SAMPLE ROW - delete before submitting`).
- **Delete this Writing Instructions section** — the final RFC ends after §7 Implementation Plan.

**Scope discipline:**
- **Match scope to change.** Simple proposals use the minimum counts (1 Goal, 1 §3.2.1 subsection, 3 §6 scenarios, 2 §7 phases) — these are floors, not targets; exceed any one when the change has genuinely distinct content to cover (e.g., a function with five real edge-case classes needs five §6 rows, as the GCD example below does).
- **Length (recommended):** most RFCs fit in 800–2,500 words; shorter is fine for simple changes. Aim to stay ≤4,000 words; exceeding that usually means padding or over-scope, so consider splitting the proposal.
- **Sections §4 and §5 stay numbered.** Never delete or renumber them; when content does not apply, use the template's exact fallback text (`Not applicable — {one-sentence reason}`) instead of filler or speculation. When evidence is unavailable, state what is unknown and what source must be verified.

**Cross-section consistency:**
- Every `G#` in §2 must appear in at least one §3.2 subsection's `Satisfies:` line; except for §2 non-testable Goals, it must also appear in at least one §6 row's `Covers` cell **and** at least one §7 phase's "Done when" clause.
- Every `T#` in §6 must transition red→green across §7 phases — at least one earlier phase's "Done when" lists the row as red (present but skipped/`xfail`/flag-gated so the project's local test suite stays green), a later phase's "Done when" lists it as green (un-marked, asserting for real).
- Every `B#` in §4 (when not Skipped) must cite the §3.2 subsection that pins the new contract, appear in at least one §6 row's `Covers` cell, **and** land in the §7 phase that introduces the break.
- Every `S#` in §5 (when not Skipped) must cite the §3.2 subsection (or §3.1 component) that pins the enforcement point, appear in at least one §6 row's `Covers` cell, **and** land in the §7 phase that exposes the trigger surface.
- Non-testable Goals: default is 0 — append `non-testable: {one-sentence reason}` in §2 only for genuinely subjective/qualitative outcomes (DX gain, readability, judgment-based observability) that cannot be expressed as a §6 row. Such goals may be omitted from §6 and from §7 "Done when" but must still appear in §3.2 `Satisfies:`.

**Avoid vague phrases.** Do not use broad claims as a substitute for specific behavior, metrics, or actions. Rewrite phrases such as "comprehensively improve", "significantly enhance", "robust and scalable", "works correctly", "behaves as expected", "best practices", and "industry standard" into concrete acceptance criteria. "We will monitor" and "we will document" are not valid mitigations unless they name the exact signal, threshold, owner, artifact, or enforcement point.

## 1. Summary

{**At most ~150 words; shorter is better.** One paragraph. State the proposal and core idea; omit speculative backstory. A reader should understand the full picture in 30 seconds.}

**TEMPLATE EXAMPLE BEGIN**

**Good:**

> Add `numkit`, a new project exposing `uint32_t gcd(uint32_t a, uint32_t b)`, which computes the greatest common divisor using the iterative Euclidean algorithm with `gcd(0, 0) = 0` and `gcd(0, n) = gcd(n, 0) = n`.

Why it works: a single sentence names the proposal concretely — project, signature, algorithm, and zero-input contract — with no backstory, so a reader grasps the core in well under 30 seconds; ~30 words, far under the ~150-word ceiling. The algorithm (iterative Euclidean) belongs here precisely because §1's job is to convey the RFC's core idea fast — and that same means must *not* resurface as a §2 Goal, where only the outcome lives.

**Bad:**

> We have long lacked a comprehensive math library, so this RFC proposes building a robust, scalable, industry-standard numerical computing platform under the Monorepo to significantly enhance our algorithmic infrastructure. The first milestone is GCD; later milestones will cover primality testing, matrix operations, FFT, and ML-ready primitives. Centralizing GCD will modernize our codebase.

Why it fails: opens with unverifiable backstory; vague claims (`comprehensive`, `robust`, `scalable`, `industry-standard`, `significantly enhance`, `modernize`); pads with out-of-scope future work (FFT, ML); never states the actual signature, algorithm, or zero-input contract — a reader cannot tell what ships.

**TEMPLATE EXAMPLE END**

## 2. Goals

{**Recommended: 1–5 Goals, 0–1 non-testable Goals (default 0).** At least one Goal must be testable — every RFC's purpose is proven through the §6 → §7 red→green flow, which validates Goals via their `T#` rows; an all-non-testable §2 leaves the RFC's own outcomes unverified, regardless of whether §4/§5 supply `B#`/`S#` anchors. (When §4/§5 are also Skipped, §6 would additionally have no legal `Covers` anchor at all.)

**Goals:** numbered `G1`, `G2`, ... so §6 and §7 can cite them by ID. Each must be concrete and verifiable.

**Means vs Goals.** Goals describe outcomes (e.g., "P95 < 50 ms"), not implementations (e.g., "use Redis"). If swapping implementations invalidates the goal, rewrite it. Test infrastructure ("ships with a unit-test binary") and build-graph integration ("registers the project in the GN `tests` group") are means, not outcomes — they belong in §7 phase Scope, verified by Done-when clauses, never as Goals.

**Non-testable Goals (fallback, default 0):** reserved for genuinely subjective/qualitative outcomes — e.g., DX gain, readability, judgment-based observability — that cannot be expressed as a §6 row. If a Goal is testable in principle, write a §6 row for it; do not hide it behind this annotation. Test-binary scaffolding and build-graph integration are §7 deliverables verified by Done-when clauses, never non-testable Goals. Append `non-testable: {one-sentence reason}` only when no §6 expression is possible. Recommended: at most 1 per RFC; needing 2+ usually means Goals are vague — rewrite as testable outcomes.}

- **G1.** {Concrete, measurable outcome}

{Add G2 through G5 only when each names a genuinely distinct outcome.}

**TEMPLATE EXAMPLE BEGIN**

**Good:**

> - **G1.** Provide a public `numkit` function that returns the greatest common divisor for every pair of `uint32_t` inputs, including zero inputs.

Why it works: one Goal fits a proposal this simple (the "simple proposals use minimum counts" rule), and `G1` captures the *outcome* a caller wants — a correct GCD for every input pair — while saying nothing about *how*. The algorithm (iterative Euclidean) appears in §1 Summary, whose job is to convey the core idea fast; keeping it out of §2 means swapping to binary GCD never invalidates the Goal. What is deliberately *not* a Goal matters just as much: the header file and unit tests are §7 phase deliverables, and the exact zero-input values (`gcd(0, 0) = 0`) are a §3.2 contract detail — promoting any of them here would dress up a means or a deliverable as a purpose. `G1` stays verifiable because a §6 row can assert the returned value directly.

**Bad:**

> - **G1.** Build a comprehensive, scalable numeric utility library that significantly improves the Monorepo's math story.
> - **G2.** Implement `gcd` using the iterative Euclidean algorithm in C for optimal performance.
> - **G3.** Add tests.
> - **G4.** Lay the groundwork for future `lcm`, fraction reduction, FFT, and ML primitives.

Why it fails: G1 is vague (`comprehensive`, `scalable`, `significantly improves`) with nothing to verify; G2 prescribes an implementation, not an outcome — swapping to binary GCD should not invalidate the goal, so the algorithm choice belongs in §1 Summary or §3 Design, not §2; G3 is both unmeasurable (which tests? what coverage?) and miscategorized — test work is a §7 phase deliverable, never a Goal, so no rewording rescues it; G4 lists out-of-scope future work that does not belong as a goal of this RFC.

**TEMPLATE EXAMPLE END**

## 3. Design

### 3.1 Overview

{**Describe the components touched and how data/control flows between them.** Contract surfaces (signatures, schemas, transition tables, wire formats) and mechanism sketches belong in §3.2 — keep this section above that level. Which files are added or modified is a §7 Scope detail — keep §3.1 at the component/module level (the boxes that change and the control/data flow between them), never a file inventory. Include an inline ASCII or Mermaid diagram when the component/flow structure is easier to read as a picture than as prose.

*Skip the diagram* (write `N/A — textual description above is sufficient`) when (a) the change is a single-function, algorithm, or data-structure change whose structure is already conveyed by §1 Summary and the §3.2 description; or (b) the structure cannot be expressed cleanly in ASCII/Mermaid (e.g., dense graphs). Don't invent hierarchy to fill the slot.}

**TEMPLATE EXAMPLE BEGIN**

**Good:**

> `numkit` is a new leaf component. Callers depend on it only through the public header and issue an in-process call into the implementation. Data flows in as the two integer arguments and back as the return value; control returns synchronously to the caller. `numkit` does not call other Monorepo components, perform I/O, allocate memory, or touch shared state.
>
> N/A — textual description above is sufficient.

Why it works: names the touched component and caller boundary; describes the data entering and leaving the component plus the synchronous control flow; records the absent outbound calls and side effects that matter at overview level; defers the signature and mechanism to §3.2 as the template directs; invokes the prescribed `N/A` fallback because prose covers this single-function flow.

**Bad:**

> `numkit` sits at the foundation of a new layered architecture, providing comprehensive numeric primitives upward through a robust, scalable interface. The new function `uint32_t gcd(uint32_t a, uint32_t b)` runs the iterative Euclidean loop `while (b) { uint32_t t = b; b = a % b; a = t; } return a;` to produce the result. The end-to-end data flow:
>
> ```
> +--------+      +-----+      +--------+
> | Caller | ---> | gcd | ---> | Result |
> +--------+      +-----+      +--------+
> ```

Why it fails: leaks §3.2 content into §3.1 — both the signature and the mechanism belong one section down; substitutes vague filler (`layered architecture`, `foundation`, `comprehensive`, `robust, scalable`) for the concrete components and flow a reviewer needs to see; invents a three-box "Caller → gcd → Result" diagram for a single pure function, the exact case the template tells you to skip with the `N/A` fallback.

**TEMPLATE EXAMPLE END**

### 3.2 Detailed Design

{**Recommended: 1–5 subsections, one aspect per subsection (~300 words each).** Most §3.2 sections have only `#### 3.2.1` — add 3.2.2 and beyond only when the change genuinely spans multiple distinct aspects, each with its own contract worth documenting separately. Distinct aspects include: data model, public API, internal algorithm, state machine, wire/serialization format, concurrency/locking model, storage layout, failure/retry policy. Two aspects that share one contract (e.g., a new endpoint and its trivial DTO) stay in one subsection, not two.

**Each subsection pins, in this order:**

1. **The contract surface** — the concrete artifact being defined (signature / schema fragment / state-transition table / wire format / CLI flag), in a code block using the notation the codebase already uses.
2. **The unstated contract** — what a reader could get wrong from the artifact alone: zero/empty-input semantics, ordering, idempotency, error and failure modes, threading, versioning.
3. **The mechanism** — pseudocode, flow sketch, or transition rules.

Each subsection ends with `Satisfies: G# via {the design hook(s)}` so every Goal in §2 has a traceable design hook in at least one subsection. The `via` clause may name more than one hook for a single Goal (as the §3.2.1 example does for `G1`). Use `;` to separate when one subsection covers multiple goals (e.g., `Satisfies: G1 via …; G3 via …`).

**Boundary rules:**

- **One aspect per subsection.** Do not split one aspect across `3.2.1` and `3.2.2`.
- **If a subsection runs much past ~300 words,** the aspect is probably not actually one aspect — consider splitting it into finer sub-aspects (contract / algorithm / state / wire format), or scoping the RFC down.
- **>5 subsections usually means the RFC is too large;** consider splitting it.
- **Use descriptive aspect names** — `#### 3.2.1 Wire Format`, not `#### 3.2.1 Details`.

**Content rules:**

- *Contract surface is verbatim, not pseudocode.* The signature/schema/wire format renders as a minimum self-explanatory code block — including load-bearing prelude (types' includes like `<stdint.h>`/`<stddef.h>`, cross-language linkage qualifiers like `extern "C"`, schema preludes). Skip codebase-uniform sugar that carries no contract content: include guards, copyright headers, formatter directives, and per-parameter doc-comments (load-bearing semantics live in the **Unstated contract** prose, not the code block).
- *Mechanism is pseudocode, one level above implementation.* For Proposed code that does not yet exist, write near-pseudocode in the language of the affected codebase — no error-handling boilerplate, no language-specific sugar. Name every input, every output, and every observable side effect; `// handle the error` and `// process input` are placeholders, not pseudocode.
- *Source and length.* For code that already exists in the repo, replace the snippet with a `path/to/file.ext:line` reference instead of pasting it. ≤30 lines per code block.
- *Notation matches the affected codebase.* SQL DDL for relational schema changes; JSON Schema / Protobuf / OpenAPI for wire formats; BNF for grammars; transition tables when the machine has >2 states or >3 transitions. Pick what the rest of the project uses; do not introduce a new notation for one RFC.
- *Stay inside the component.* §3.1 says which boxes change; §3.2 says what happens inside one box. Do not redraw component diagrams (that belongs to §3.1) or enumerate which files are added (that belongs to §7 Scope).
- *Cite, don't paste, large artifacts.* Full schema files, full state diagrams, and 100-line algorithms belong in the diff or a linked file — quote only the smallest fragment that pins the contract.}

#### 3.2.1 {Aspect: see distinct-aspects list above}

{One aspect of the design at implementation level. End with `Satisfies: G# via {the design hook(s)}` (use `;` to separate when one subsection covers multiple goals).}

{Add §3.2.2 through §3.2.5 only when each covers a genuinely distinct aspect.}

**TEMPLATE EXAMPLE BEGIN**

**Good:**

> #### 3.2.1 Public API and Algorithm
>
> ```c
> /* numkit/include/numkit/gcd.h */
> #include <stdint.h>
>
> uint32_t gcd(uint32_t a, uint32_t b);
> ```
>
> **Unstated contract.** `gcd(0, 0)` returns `0`; `gcd(0, n)` and `gcd(n, 0)` return `n`. These are pinned in the header doc-comment — they cannot be inferred from the signature and are the canonical reference for reviewers and callers. Pure function: no I/O, no allocation, no global state, and no failure path (so no return-code overload, no `errno` write). Thread-safe because both arguments pass by value and the body touches no shared state.
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
> Satisfies: G1 via the exported signature, the zero-input contract pinned in the header doc-comment, and the pure, allocation-free design that lets each §6 edge-case row assert on a single return value.

Why it works: one subsection because signature, unstated contract, and mechanism share one aspect (the public function), as the rules direct; the contract surface uses C — the codebase's notation — and shows only the signature, deferring the body to pseudocode; the **Unstated contract** paragraph names the four things a reader could miss from the signature alone (zero-input semantics, purity, no error path, thread-safety); the pseudocode is one level above the implementation with every variable named; the termination paragraph confirms the zero-input contract falls out of the same loop, so no special-case branch is needed (a subtlety the Bad example below gets wrong); leaves algorithm-choice rationale (e.g., why not binary GCD?) for §3.3 rather than muddling it into the Mechanism paragraph; closes with a `Satisfies:` line that traces back to the single Goal `G1` from §2; well under the recommended ~300 words per subsection.

**Bad:**

> #### 3.2.1 Function
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
> #### 3.2.2 Edge Cases
>
> Zero inputs are handled by the early return above. The function works correctly for all valid `uint32_t` pairs.
>
> #### 3.2.3 Performance
>
> Runs in O(log n) time, which is fast enough.

Why it fails: aspect name `Function` is generic — the rule requires descriptive names like `Public API and Algorithm`; the opening paragraph enumerates the files added (`gcd.h`, `gcd.c`), which belong in §7 Scope — §3.2 stays inside the box, and the only overview-level fact (that `numkit` is a new leaf component) already lives in §3.1; pastes the full `.c` instead of pseudocode, so the snippet ships with an `if (a == 0 && b == 0) return 0;` early return that is dead code (when `b == 0` the `while` loop is skipped and `return a` already yields `0` for the `(0, 0)` input) — pseudocode would have surfaced that the loop alone covers the zero-input contract; splits one aspect across `3.2.1`/`3.2.2`/`3.2.3` for no reason — `Edge Cases` and `Performance` are notes about the same function, not distinct aspects with their own contracts; "works correctly", "fast enough", and "optimal performance" are exactly the vague phrases the writing instructions ban; no `Unstated contract` paragraph, so the zero-input semantics live only in the (redundant) code; missing the closing `Satisfies: G# via …` line on every subsection, so §2 goals have no traceable design hook.

**TEMPLATE EXAMPLE END**

### 3.3 Design Rationale

{**Recommended: 0–3 decisions.** Document only decisions a future reader would actually question — concretely, an entry is worth recording only if (a) swapping the choice would invalidate at least one `G#`, or (b) a reviewer would predictably ask "why not X". Neither passes → micro-choice, do not record.

When §3.2's Mechanism, contract surface, or notation silently carries a "we picked X over Y", lift the comparison to §3.3 and leave only the chosen artifact in §3.2.

For each, use a 3-line structure:
- **Chosen:** ...
- **Reason:** ...
- **Ruled out:** ...

`None` is a common legitimate state, not a failure signal — when §2 Goals and §3.2 contract surface have already locked every within-scope choice, write `None` and stop. Do not invent decisions to fill the slot. If an entry passes the worth-recording test above but no real alternative was considered, write `Ruled out: N/A — no viable alternative considered` rather than inventing a strawman.

§3.3 entries do not need to trace to §2 Goals, §6 rows, or §7 phases — decisions stand independently.}

**TEMPLATE EXAMPLE BEGIN**

**Good:**

> - **Chosen:** Iterative Euclidean (the loop pinned in §3.2.1).
> - **Reason:** Worst case is `O(log(min(a, b)))` — under 50 iterations for any `uint32_t` input pair (Fibonacci-pair worst case bounded by `~1.44 · log₂(n)`), well under any caller's budget. The `%` operator compiles to a single hardware-divide instruction on every platform this Monorepo targets, so the loop body is already at the noise floor; trading it for shifts and subtractions would not measurably help.
> - **Ruled out:** Binary GCD (Stein's algorithm) replaces `%` with shifts and subtractions; the speedup matters only on targets without a hardware divider (older microcontrollers). Revisit when such a target lands in the Monorepo — until then the simpler algorithm with the easier termination proof is the safer default.

Why it works: one decision is enough — signature, zero-input contract, and file layout are pinned by §2 and §3.2 with no real alternative, so they do not belong here; **Reason** quantifies the budget (under 50 iterations on `uint32_t`, single hardware-divide instruction) instead of hand-waving about "good performance"; **Ruled out** names the real alternative *and* the specific condition (no hardware divide) that would flip the decision, so the next author has a checklist to revisit against — not a strawman; lifts the binary-GCD verdict out of §3.2.1's Mechanism paragraph (where it conflated "how the loop works" with "why we picked this loop") and lands it where within-scope alternatives belong.

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

Why it fails: `Reason` uses every vague phrase the writing instructions ban ("clean, elegant, robust", "optimal performance", "works correctly") with no numbers or named platform — a future reader cannot tell *why* this algorithm beats the alternatives; `Ruled out` invents strawmen (recursive Euclidean is not a stack-overflow risk at the few-dozen-frame depth `uint32_t` inputs allow, trial division is so absurd no reviewer would ask about it) and skips the one alternative a real reviewer would actually raise — binary GCD; pads with two micro-choices (return type matching the input, function naming) that no future reader would re-litigate — they belong in §3.2's contract surface, not §3.3, and exist here only to fill the slot; the 0–3 recommendation exists to prevent exactly this kind of noise — burying the one real decision (binary GCD) inside a list of cosmetic ones.

**TEMPLATE EXAMPLE END**

## 4. Backward Compatibility & Migration

{**Skip when** previously-working callers see no new failure (additive APIs, brand-new exports, or refactors invisible at the public boundary) → write `Not applicable — {one-sentence reason}` and stop.

**Recommended: 1–5 entries.** Each entry pins one observable way a previously-working caller stops working; fan-out from one root change collapses into one entry, independent breaks split. >5 usually means the RFC bundles too much — consider splitting it.

**Per-break structure.** Number each entry `B1`, `B2`, … so §6 `Covers` and §7 "Done when" can cite it by ID; each entry is one labeled block of 3 lines:

- **B1.**
  - **Breaks:** the exact signature, schema, wire format, or semantic that changes — cite the §3.2 subsection that pins the new contract.
  - **Symptom on un-migrated caller:** the exact compile error, runtime exception, log line, wire mismatch, or wrong-but-silent output the caller observes. "It will fail" / "callers must update" / "behavior changes" are placeholders, not symptoms.
  - **Migration:** the concrete command, codemod, version bump, or config flip that resolves the symptom. If non-mechanical, say so and cite the existing runbook — never "we will document".

**Don't:**

- Invent a break to fill the slot — if the RFC is additive, Skip.
- List breaks still hidden behind a deprecation alias — record them the cycle the alias is removed.
- List internal refactors or cosmetic renames invisible at the public boundary.

**Cross-section consistency:** every `B#` entry pairs with at least one §6 row that cites it in `Covers` and exercises the new behavior on previously-succeeding input.}

**TEMPLATE EXAMPLE BEGIN**

**Good:**

> Not applicable — `numkit/gcd` is a brand-new export with no prior callers; nothing that compiled or ran before this RFC changes behavior.

Why it works: invokes the Skip clause for the genuine reason — a newly added export breaks nothing that previously existed — instead of manufacturing a migration story to fill the slot; uses the prescribed `Not applicable — {one-sentence reason}` wording verbatim; one short sentence and stops, exactly as the rule directs.

**Bad:**

> - **B1.**
>   - **Breaks:** Existing math code may need updates.
>   - **Symptom on un-migrated caller:** Build errors or unexpected results.
>   - **Migration:** We will document the upgrade path before release.

Why it fails: the GCD RFC adds a new file to a project that previously had no GCD function — there is *nothing* to break, so the right answer is the Skip clause, not invented rows; "Existing math code may need updates" names no signature, no §3.2 hook, and no observable change — the vague placeholder the rule forbids; "Build errors or unexpected results" wraps a vague second clause around an over-broad first ("build errors") and joins them by `or`, so a caller cannot tell which to grep for — a real Symptom names one observable per entry, e.g., `error: too few arguments to function 'gcd'`; "We will document the upgrade path before release" is the banned `we will document` pattern, with the future-tense framing making the §4 entry depend on an artifact that does not yet exist when the RFC is reviewed.

**TEMPLATE EXAMPLE END**

## 5. Security

{**Skip if** the code path does not cross a trust boundary — no external or attacker-controlled input, no credentials/keys, no headers/URLs derived from input, no upstream responses, no authentication or authorization, no persisted data → write `Not applicable — {one-sentence reason}` and stop.

**Recommended: 1–4 concerns.** Each entry pins one specific attack or failure mode plus its mitigation; variants of the same attack (e.g., several malformed-input shapes the same parser rejects) collapse into one entry, distinct attacks split. >4 usually means the RFC bundles a security-sensitive surface large enough to consider splitting.

**Per-concern structure.** Number each entry `S1`, `S2`, … so §6 `Covers` and §7 "Done when" can cite it by ID; each entry is one labeled block of 3 lines:

- **S1.**
  - **Threat:** the specific attack/failure mode plus the trigger (input shape, operation, or caller) that exposes it. "Possible vulnerability" / "may be unsafe" / "input must be validated" are placeholders, not threats.
  - **Mitigation:** the code-level check, library call, schema constraint, or config flag that prevents it — cite the §3.2 subsection (or §3.1 component) that pins the enforcement point. "Validate input" / "sanitize before use" without naming the validator are placeholders.
  - **Enforcement:** the §6 row (`T#`) that fires the trigger input and asserts the rejection/sanitization/safe outcome — every S# must have one, and it transitions red→green through §7 like any other test (its mitigation lands in the phase that exposes the trigger surface, per the cross-section rule). A build-time guard (static-analysis lint, repo config value) may *back up* that row but never replaces it. "We will harden" / "code review will catch it" / "monitoring will alert" are placeholders, not enforcement.

**Don't:**

- Pad with generic threats (XSS, SQLi, CSRF, buffer overflow, path traversal, ...) the code path does not actually expose — every "N/A" row a reviewer reads to dismiss steals attention from the real concern. If every entry would be non-applicable, invoke the Skip clause instead.
- List threats already neutralized upstream (framework escaping, platform sandbox, transport TLS) unless this RFC introduces a new path that bypasses them — record the bypass, not the upstream defense.
- Cite a Mitigation that does not yet exist in code — "we will add a sanitizer", "we plan to enforce ..." — the enforcement point must be in this RFC's diff or already in the repo before §5 cites it.

**Cross-section consistency:** every `S#` entry pairs with at least one §6 row that cites it in `Covers` and exercises the trigger input, asserting the rejection, sanitization, or safe outcome.}

**TEMPLATE EXAMPLE BEGIN**

**Good:**

> Not applicable — `numkit/gcd` takes two `uint32_t` arguments by value, performs no I/O or allocation, reads no external state, and exposes no credentials, so no input crosses a trust boundary that did not exist before this RFC.

Why it works: invokes the Skip clause for the genuine reason — every category named in the Skip rule (external input, credentials, headers, upstream responses, authentication, persisted data) is absent because two pass-by-value integers carry no attacker-controlled bytes and the function touches no external state; uses the prescribed `Not applicable — {one-sentence reason}` wording verbatim; one short sentence and stops, exactly as the rule directs.

**Bad:**

> - **S1.**
>   - **Threat:** Buffer overflow.
>   - **Mitigation:** Use safe C coding practices.
>   - **Enforcement:** Code review and static analysis will catch issues before merge.
>
> - **S2.**
>   - **Threat:** SQL injection.
>   - **Mitigation:** N/A — gcd does not interact with databases.
>   - **Enforcement:** N/A.
>
> - **S3.**
>   - **Threat:** Denial of service from very large inputs.
>   - **Mitigation:** Iterative Euclidean is O(log n).
>   - **Enforcement:** A performance test will be added.

Why it fails: `gcd` takes two `uint32_t` arguments by value with no array access, pointer arithmetic, or `memcpy`, so "Buffer overflow" is impossible — the "code path actually exposes that surface" trigger from the Don't rule is not met, and "safe C coding practices" plus "code review and static analysis will catch issues" trips both Mitigation and Enforcement placeholder bans (no specific lint, no §3.2 hook, no §6 row); the SQL-injection row is dilution in pure form — three lines that read "N/A" only steal attention from real concerns, and the right fix is to delete the row entirely (or, since every row here is non-applicable, invoke the Skip clause for the whole section); "Denial of service from very large inputs" fabricates scope — `O(log(min(a, b)))` on `uint32_t` runs in under 50 iterations even on the Fibonacci-pair worst case, so no caller can DoS `gcd`, and "A performance test will be added" is the banned future-tense `we will document` pattern with no §6 row to cite; this RFC's correct §5 is the Good example's Skip clause.

**TEMPLATE EXAMPLE END**

## 6. Testing Strategy

{**Recommended: 3–10 scenarios.** Each row is one executable test case tied to at least one of a §2 Goal `G#`, a §4 break `B#`, or a §5 concern `S#` — a row may cover only a `B#` or `S#` with no `G#` (e.g., a malformed-input rejection that serves a §5 concern but no functional Goal). >10 usually means rows conflate sibling scenarios — collapse inputs that share one Goal and one setup pattern into one row.

**Per-row structure.** One markdown table, columns in this order:

| # | Scenario | Input / Setup | Expected Result | Covers | Level |
|---|----------|---------------|-----------------|--------|-------|

- **#** — stable row ID (`T1`, `T2`, ...) so §7 phases can cite rows by ID.
- **Scenario** — one phrase naming the behavior under test; not the test-function name.
- **Input / Setup** — concrete input values or fixture state. "Valid input" / "typical case" are placeholders, not setups.
- **Expected Result** — the observable outcome the test asserts (exact return value, exact error variant, exact wire bytes, log line). "Works correctly" / "behaves as expected" are banned.
- **Covers** — the `G#` / `B#` / `S#` the row exercises — at least one ID, but not necessarily a `G#`; a row may cover only a §4 break or §5 concern. One row may cover multiple items; use `,` to separate.
- **Level** — `unit`, `integration`, or `e2e`, matching the codebase's harness vocabulary. Pick the cheapest level that still exercises the contract; escalate only when the contract crosses a boundary the cheaper level cannot reach.

**Don't:**

- List rows that only re-assert the type-checker ("signature compiles", "header parses") — the build already proves that.
- Quote implementation detail ("branch X is taken", "loop iterates N times") instead of observable outcome — tests must survive refactors that preserve the contract.
- Inflate rows per Goal to satisfy `Covers` — if one setup exercises multiple Goals, one row covers all of them.
- Defer with "will add later" / "to be determined" — an undecided row is not a strategy.

**Cross-section consistency:** every `G#` in §2 appears in at least one `Covers` cell (non-testable Goals per §2 excepted). Every `B#` in §4 appears in at least one `Covers` cell whose row exercises the new behavior on previously-succeeding input. Every `S#` in §5 appears in at least one `Covers` cell whose row fires the trigger input and asserts the safe outcome.}

| # | Scenario | Input / Setup | Expected Result | Covers | Level |
|---|----------|---------------|-----------------|--------|-------|
| T1 | TEMPLATE SAMPLE ROW - delete before submitting | ... | ... | G1 | unit |

**TEMPLATE EXAMPLE BEGIN**

**Good:**

> | # | Scenario | Input / Setup | Expected Result | Covers | Level |
> |---|----------|---------------|-----------------|--------|-------|
> | T1 | Zero-input contract | `gcd(0, 0)`, `gcd(0, 7)`, `gcd(11, 0)` | Returns `0`, `7`, `11` respectively | G1 | unit |
> | T2 | Equal inputs | `gcd(42, 42)` | Returns `42` | G1 | unit |
> | T3 | Coprime pair | `gcd(17, 31)` | Returns `1` | G1 | unit |
> | T4 | One side multiple of the other | `gcd(12, 36)`, `gcd(100, 25)` | Returns `12`, `25` respectively | G1 | unit |
> | T5 | Worst-case Fibonacci pair (F47, F46) | `gcd(2971215073u, 1836311903u)` | Returns `1` | G1 | unit |

Why it works: five rows cover the distinct edge classes of a GCD function (zero-input, equal, coprime, one-side-multiple, worst-case) — five rather than the 3-scenario floor because the function genuinely has five distinct edge classes, not because a simple proposal targets five — all tracing to the one Goal `G1`, each with exact inputs and exact return values a future author can port straight into a test function; T1 folds the three zero-input sub-cases the header pins into one row because they share `G1` and one setup pattern — splitting them into three rows only to pad `Covers` is exactly what the "Don't inflate" rule forbids; T5 picks the actual Fibonacci-pair worst case named in §3.2.1's Mechanism paragraph (two consecutive integers near `UINT32_MAX` would be coprime and finish in 2 steps, missing the bound entirely), giving the worst-case input a concrete hook instead of a vague "large numbers" row; the single Goal `G1` appears in every `Covers` cell (T1–T5), clearing the cross-section consistency rule that each §2 Goal be exercised by a test; Level stays `unit` throughout because a pure function has no integration boundary — escalating to `integration` or `e2e` would be ceremony the "pick the cheapest level" rule forbids.

**Bad:**

> - Unit tests will verify that `gcd` works correctly across all valid inputs.
> - Integration tests will ensure compatibility with downstream consumers.
> - Performance benchmarks will confirm optimal runtime.
>
> Comprehensive edge-case coverage will be added before release.

Why it fails: prose instead of the required table — no `#`, no `Covers`, no row IDs for §7 phases to cite, and no rows to trace against §2's Goals; "works correctly", "ensure compatibility", "optimal runtime", and "comprehensive edge-case coverage" tick every vague phrase the writing instructions ban, all in one paragraph; "Integration tests" invents a downstream consumer — `numkit` is a brand-new leaf with no callers today, so the cheapest level that exercises the contract is `unit`, and the "pick the cheapest level" rule forbids the escalation; "Performance benchmarks" repeats the §5 Bad example's fabricated DoS scope — `O(log(min(a, b)))` on `uint32_t` completes in under 50 iterations, so no runtime gate is worth asserting; "will be added before release" is the banned future-tense pattern with no concrete input, no expected result, and no `G#` traceability — the reviewer has nothing to approve.

**TEMPLATE EXAMPLE END**

## 7. Implementation Plan

{**Recommended: 2–5 phases.** Each phase is an independently shippable increment that leaves the project's local test suite green on its own — not a calendar week, not a sprint, not an OKR milestone. Add a phase only when the prior one must land and be verified before the next can start (public API before the tests that import it, flag-off deploy before flag-on default, schema migration before code that depends on the new columns). >5 phases usually means the RFC bundles too much — consider splitting it.

**TDD red→green is the required phase ordering.** Each §6 row lands first in a "red" phase (the test is added in failing form, but the local test suite stays green — `xfail`/`expected-fail` markers run it and tolerate the failure, while `skip`/`pending` markers, a feature flag that gates the test, or a separate test target not yet wired into the local test runner keep it from running at all) before a later "green" phase that implements the behavior and clears the gate so the test asserts for real when the local test suite is run. Name the chosen red-phase mechanism in the red phase's `Scope` so a reviewer can verify the gate from the diff. This guarantees ≥2 phases for any RFC — a single phase shipping a `T#` row alongside its own implementation erases the verifiable transition the workflow exists to enforce.

**Per-phase structure (3 lines):**

- **Scope:** the concrete deliverables this phase alone ships — files added or modified, migrations run, flags flipped, test binaries registered. "Implement the feature" / "add tests" / "polish" are placeholders, not scope.
- **Depends on:** the prior phase IDs (`P#`) this phase requires to land first, or `None` for the first phase. Name phases by ID, never by week or date.
- **Done when:** the observable acceptance criteria that close the phase — cite §6 rows by `T#`, §2 Goals by `G#`, and any §4 break (`B#`) or §5 concern (`S#`) this phase lands, so a reader can verify completion by grepping the referenced artifacts. "Code review passes" / "tests green" without naming which rows are placeholders.

**Don't:**

- List dates or owners — an RFC reviews the plan, not the calendar or staffing, and both drift the moment schedules slip; phase IDs (`P1`, `P2`) survive re-ordering but "Week 3" does not.
- Open with a "planning" phase or close with a "monitoring" phase — this RFC *is* the planning artifact, and a monitoring-only phase has no mergeable deliverable to close on.
- Fold a real ordering constraint into one phase — if tests need the header to exist first, that is two phases with a `Depends on` edge, not one "implement + test" blob that erases the merge gate.
- Ship a §6 row alongside its own implementation in one phase — that collapses TDD red→green into one phase, the project's local test suite never observes the row in red state, and the cross-section rule that each `T#` traces through both a red and a green phase fails.
- Defer the hard part with a bare placeholder — "P3: handle edge cases later" without a concrete `Scope` and `Done when` is the future-tense "we will document" pattern the writing instructions ban.

**Cross-section consistency:** every `G#` from §2 appears in at least one phase's `Done when` clause (§2 non-testable Goals excepted). Every `T#` from §6 transitions red→green across phases — an earlier phase's `Done when` names the row as red (present but skipped/`xfail`/flag-gated, so the local test suite stays green), a later phase's `Done when` names it as green (un-marked, asserting for real). Every `B#` migration entry from §4 lands in the phase that introduces the contract break — never later, so un-migrated callers never see the new symptom without the migration shipping in the same phase. Every `S#` security concern from §5 lands in the phase that exposes the trigger surface, so the mitigation merges no later than the code it protects.}

- **P1. {Red phase — lands the contract surface with the §6 rows present but skipped}.**
  - **Scope:** {files this phase ships; name the red-phase marker (`xfail`/`skip`/flag-gate) that keeps the local test suite green}
  - **Depends on:** None
  - **Done when:** {`T#` rows present in red (skipped) state with the local suite still green; cite the `G#` staged red}

- **P2. {Green phase — implements the behavior and turns the §6 rows green}.**
  - **Scope:** {the implementation that replaces the stub; remove the red-phase markers so the `T#` rows assert for real}
  - **Depends on:** P1
  - **Done when:** {`T#` rows pass un-skipped (`G#` green)}

{Add P3 through P5 only when the prior phase is a genuine merge gate the next phase cannot cross.}

**TEMPLATE EXAMPLE BEGIN**

**Good:**

> - **P1. Land `numkit` skeleton with red `T1`–`T5`.**
>   - **Scope:** add `numkit/include/numkit/gcd.h` per §3.2.1 (signature + zero-input doc-comment pinned in the **Unstated contract** paragraph); add `numkit/src/gcd.c` with a stub body `return 0;` so the project links; add `numkit/test/gcd_test.c` containing the five rows from §6 as separate test cases, each wrapped in the harness's `TEST_SKIP("pending RFC-NNN P2")` macro so the binary builds, runs, and reports the rows as skipped — the local test suite stays green; register `numkit` as a new leaf in the top-level project list per §3.1; register the test binary in the project's local test runner so it runs whenever the local test suite is invoked.
>   - **Depends on:** None.
>   - **Done when:** the `uint32_t gcd(uint32_t, uint32_t)` signature from §3.2.1 exports from the header and its doc-comment pins `gcd(0, 0) = 0` and `gcd(0, n) = gcd(n, 0) = n` verbatim — both halves of `G1`'s contract; `T1`–`T5` are present in skipped (red) state and the local test suite passes after the test binary registers (`G1` staged red, turned green in P2).
>
> - **P2. Implement `gcd` and turn `T1`–`T5` green.**
>   - **Scope:** replace the stub body in `numkit/src/gcd.c` with the iterative Euclidean loop pinned in §3.2.1's Mechanism paragraph; remove the `TEST_SKIP` wrappers from `T1`–`T5` so they assert for real on every local test run.
>   - **Depends on:** P1.
>   - **Done when:** `T1`–`T5` all pass un-skipped on the first clean local test run after the wrappers come off (`G1` green).

Why it works: two phases — matching the required TDD red→green ordering and the simple-proposal minimum — because P1 lands the contract surface (header + linkable stub + test rows) with every §6 row present but skipped (red), and P2 turns the rows green by replacing the stub with the real loop and removing the skip wrappers; P1 ships the header alongside a stub `.c` so the project still builds and links, satisfying the "phase leaves the local test suite green" rule even though no real implementation has landed yet; the red phase keeps the local test suite green via the `TEST_SKIP` macro the rule requires, and the wrapper is named in P1's `Scope` so a reviewer can verify the gate from the diff alone; P2 names P1 as a `Depends on` because the skip wrappers can only come off once the implementation exists, so the edge is a real merge gate instead of ceremony; each `T#` traces through both phases — red in P1's `Done when`, green in P2's — satisfying the §6 → §7 red→green cross-section rule; `Done when` clauses cite the exact §6 row IDs (`T1`–`T5`) and the single §2 Goal ID (`G1`) so the Goal has a traceable completion hook a reviewer can verify by grepping the §6 table and the header doc-comment; P1's `Done when` references §3.2.1's **Unstated contract** paragraph by name, so the zero-input contract cannot get dropped between design and merge; no dates, no owners, no "phase 0: design review", no "phase N: monitor" — matches every item on the Don't list.

**Bad:**

> - **Phase 1 (Week 1):** Design and kickoff. Owner: Alice.
> - **Phase 2 (Week 2–3):** Implement `gcd` and write comprehensive tests. Owner: Bob.
> - **Phase 3 (Week 4):** Code review and address review feedback. Owner: TBD.
> - **Phase 4 (Week 5+):** Monitor production and iterate on edge cases as they surface.

Why it fails: every phase is a calendar week (`Week 1`, `Week 2–3`, …) — the "a phase is an independently shippable increment, not a timebox" rule forbids this directly, and any slip on Phase 1 cascades into a meaningless "Phase 2 is late" status with no merge gate a reviewer can point at; lists owners (`Alice`, `Bob`, `TBD`) — the Don't list bans staffing because it drifts and an RFC reviews the plan, not who holds the pager; "Phase 1: Design and kickoff" is the banned open-with-planning pattern — this RFC *is* the design artifact, so Phase 1 cannot also be design; "Implement `gcd` and write comprehensive tests" collapses two phases with a real ordering edge (tests cannot compile before the header merges) into one, erasing the verification point a reviewer needs to grant or refuse Phase 2 — and worse, ships every §6 row alongside its own implementation, so the local test suite never observes any `T#` in red state and the TDD red→green transition the rule requires never occurs; "comprehensive tests" is the banned vague phrase with no `T#` row to cite and no `G#` to prove complete; Phase 3 ("code review and address review feedback") is not a deliverable — every phase ships through code review, so promoting it to a phase pads the count without adding a merge gate; Phase 4 ("monitor production and iterate") has no `Scope`, no `Depends on`, no `Done when`, and no end state — a monitoring-only phase has no mergeable deliverable to close on; no phase declares a `Done when` line, so `G1` has no traceable completion hook and the cross-section consistency rule fails.

**TEMPLATE EXAMPLE END**
