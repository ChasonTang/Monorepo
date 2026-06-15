Write an RFC for the requirement below using docs/rfc_000_design_doc_template.md. Treat every rule in that template as a hard constraint. If anything is unclear or uncertain while drafting, ask the user instead of fabricating details.

<requirement>
</requirement>

## Pre-Draft Fit Check

Run this gate **before drafting**. The template hard-wires a TDD red→green workflow — §2 needs ≥1 testable Goal, §5 needs executable rows asserting observable behavior, §6 needs every `T#` to transition red→green across ≥2 phases. Requirements without an observable behavior change force fabricated `T#` rows or abused non-testable Goal annotations.

**Fit test — both must hold:**

1. The requirement names (or implies) at least one concrete, observable outcome a `T#` row could assert on (return value, error variant, wire bytes, schema field, log line, rendered artifact).
2. The implementation can split so the assertion fails first (red) and passes later (green) while the default local test suite stays green in between.

**Common misfits:** pure documentation/explanation changes with no code-path change; mechanical dependency upgrades with no behavior change; pure renames, file moves, or formatter sweeps that preserve behavior; research/exploration/spike work with no committed deliverable; ADRs, deprecation notices, or postmortems; UI/UX changes whose acceptance is purely visual or subjective.

**If the requirement looks like a misfit, do not draft.** Stop and report to the user, naming:

- Which axis of the fit test fails (no observable outcome, no red→green split, or both), citing the specific phrase that triggered it.
- One or two concrete reformulations that would fit, if plausible (e.g., "improve docs" → "add §X with sections Y, Z plus an example covered by a `compiles-clean` lint row"; "refactor X" → "land characterization tests T1–T3 in red, then refactor to keep them green").
- The option to write a different document type (ADR, deprecation notice, design note) instead of an RFC.

Wait for the user's decision before drafting. Do not fabricate a testable Goal or invoke the §2 non-testable Goal fallback to force a fit.

Save to docs/rfc_NNN_{slug}.md, where NNN is the lowest unused three-digit index in docs/.

## Pre-Submission Self-Check

Run each check before declaring the draft ready. Each maps to a real failure mode that has cost a full review round.

1. **Coverage matrix.** Build the matrix §3.2's branches generate; every reachable cell needs a §5 row. Axes:
   - **G#** — every testable Goal.
   - **State** — every `(state, event)` cell §3.2 branches on; same fault class under two different states is two rows.
   - **Completion mode** — happy single-call vs. happy staged-AGAIN-resume (from `s.write_off` / `s.buf_used`) are distinct branches even though both end in `OK`.
   - **Decoder branch** — every distinct decoder error variant (`ERR_BAD_VERSION`, `ERR_BAD_FRAME_TYPE`, …) gets its own row; "all map to EPROTO" is not coverage.
   - **Benign-vs-fatal split** — `if err != 0: handle else: continue` needs rows for both arms; the benign arm catches regressions that drop the guard.
   - **Constructor / factory precondition** — synchronous validation in §3.2.1 contract surface needs rows independent of the wire-decode rows that exercise the same input downstream.
   - **Callback-safe lifecycle hand-off** — behaviors documented as "legal inside `on_done`" (destroy-from-inside-callback, re-arm-from-inside-callback) need a row firing them, ideally under a sanitizer §6 already configures.
2. **Paired-mode symmetry.** For paired roles (Client/Server, Encoder/Decoder, Read/Write), each §5 row on one side has its analogue on the other under the *equivalent* state. If state machines diverge, analogues live under equivalent states, not "any state with the same fault". Unjustified asymmetry → add the row or justify in §3.2 prose.
3. **Code grounding.** Verify every claim citing a prior RFC's signature, behavior, or constraint against current code (read the file, grep the symbol). Code, not the cited RFC, is authoritative.
4. **Internal contradiction grep.** For every "X cannot Y" / "X must Y" claim, grep the entire RFC and fix every occurrence; one stale recurrence produces a second-round finding.
5. **Numeric consistency.** §5 table row count, §5 narrative range references (`"Rows T1–T21 are unit tests"`), and every §6 `"Done when"` range reference all agree.
6. **Template traceability.** Re-confirm the template's Cross-section consistency rules: every G# appears in some §3.2 `Satisfies:` and (if testable) ≥1 §5 `Covers`; every non-Skipped S# cites §3.2/§3.1, appears in ≥1 §5 `Covers`, and lands by its trigger-surface §6 phase; every T# transitions red→green across §6 phases with named red-verification evidence.
