Write an RFC for the requirement below using docs/rfc_000_design_doc_template.md. Every rule in that template is a hard constraint. If anything is unclear while drafting, ask the user instead of fabricating details.

<requirement>
</requirement>

## Local Execution Constraint

The dev environment can build and run only macOS host-arch binaries. macOS alternate arch, Linux x86_64, iOS Simulator, and iOS device targets cross-compile but cannot execute, and there is no CI. Only host-runnable `T#` rows produce executable red/green evidence; alternate-platform branches are verified by successful cross-compile + code review.

For any RFC whose §3.2 has platform-conditional mechanism (`#if defined(__linux__)`, kqueue-only/epoll-only, iOS-only entitlements, ...):

1. **Tests target the platform-agnostic contract.** Write `T#` rows against the abstract API, not the backend syscall, so the host-arch backend exercises the contract on every row.
2. **§6 `Done when` must enumerate host-runnable vs cross-compile-only `T#` branches.** Name the binary that runs each row and the binaries that are built-but-not-executed (e.g., *"the Linux binary is cross-compiled but not executed in this RFC; T14's timerfd branch is compiled but not run"*). `rfc_010_event_loop.md` §7 is the reference pattern.
3. **§3.2 Mechanism prose must acknowledge alternate-platform behaviors that are not runtime-verified in this env** — one sentence in the relevant subsection's **Unstated contract** paragraph. Do not route this through §4 Security; §4 is trust-boundary attacks only.

**Misfit:** an RFC whose `T#` rows are all cross-compile-only (purely Linux/iOS behavior with no host-runnable contract surface) fails the Pre-Draft Fit Check under "no executable red→green split in this env" — stop and propose deferring or carving out a host-runnable contract row.

## Pre-Draft Fit Check

Run **before drafting**. The template hard-wires TDD red→green — §2 needs ≥1 testable Goal, §5 needs rows asserting observable behavior, §6 needs every `T#` to transition red→green across ≥2 phases. Requirements with no observable behavior change force fabricated `T#` rows or abused non-testable Goal annotations.

**Fit test — both must hold:**

1. The requirement names (or implies) ≥1 concrete observable outcome a `T#` could assert on (return value, error variant, wire bytes, schema field, log line, rendered artifact).
2. The implementation can split so the assertion fails first (red) and passes later (green) while the default local test suite stays green in between.

**Common misfits:** pure docs/explanation with no code-path change; mechanical dependency upgrades; pure renames, file moves, or formatter sweeps; research/spike work with no committed deliverable; ADRs, deprecation notices, postmortems; UI/UX changes whose acceptance is purely visual.

**If the requirement is a misfit, do not draft.** Stop and report:

- Which axis fails (no observable outcome, no red→green split, or both), citing the triggering phrase.
- One or two concrete reformulations if plausible (e.g., "improve docs" → "add §X with sections Y, Z plus an example covered by a `compiles-clean` lint row"; "refactor X" → "land characterization tests T1–T3 in red, then refactor to keep them green").
- The option to write a different document type (ADR, deprecation notice, design note).

Wait for the user. Do not fabricate a testable Goal or invoke the §2 non-testable fallback to force a fit.

Save to docs/rfc_NNN_{slug}.md, where NNN is the lowest unused three-digit index in docs/.

## Pre-Submission Self-Check

Run each check before declaring the draft ready.

1. **Coverage matrix.** Build the matrix §3.2's branches generate; every reachable cell needs a §5 row. Axes:
   - **G#** — every testable Goal.
   - **State** — every `(state, event)` cell §3.2 branches on; same fault class under two different states is two rows.
   - **Completion mode** — happy single-call vs. happy staged-AGAIN-resume (from `s.write_off` / `s.buf_used`) are distinct even though both end in `OK`.
   - **Decoder branch** — every distinct decoder error variant gets its own row; "all map to EPROTO" is not coverage.
   - **Benign-vs-fatal split** — `if err != 0: handle else: continue` needs rows for both arms; the benign arm catches regressions that drop the guard.
   - **Constructor / factory precondition** — synchronous validation in §3.2.1 contract surface needs rows independent of the wire-decode rows that exercise the same input downstream.
   - **Callback-safe lifecycle hand-off** — behaviors documented as "legal inside `on_done`" (destroy/re-arm from inside callback) need a row firing them, ideally under a sanitizer §6 already configures.
   - **Post-syscall sub-branch** — when §3.2 shows a syscall succeeding then a follow-up failing (e.g., `accept(2)` OK then `fcntl(F_SETFL)` failing on macOS), each follow-up failure class is its own cell. A `fail_next_<syscall>` hook that bypasses the real syscall cannot reproduce this cell — add both the hook and the row.
2. **Paired-mode symmetry.** For paired roles (Client/Server, Encoder/Decoder, Read/Write), each §5 row on one side has its analogue on the other under the *equivalent* state. If state machines diverge, analogues live under equivalent states, not "any state with the same fault". Unjustified asymmetry → add the row or justify in §3.2 prose.
3. **Code grounding.** Verify every claim citing a prior RFC's signature, struct field, file path, line number, function name, errno, flag, or behavior against current code (read the file, grep the symbol). Code, not the cited RFC, is authoritative. Keep the grep commands and hit counts on hand (`grep -n io_handles odin/event_loop_internal_test.h → line 54`) — "I read the code" without the command is the failure mode that ships `io_count` when the header defines `io_handles`.
4. **Internal contradiction grep.** For every "X cannot Y" / "X must Y" / "X is Y" claim, grep the entire RFC for the load-bearing noun and synonyms; one stale recurrence produces a second-round finding. Keep the grep commands on hand.
5. **Numeric consistency.** §5 table row count, §5 narrative range references (`"Rows T1–T21 are unit tests"`), and every §6 `"Done when"` range reference all agree.
6. **Template traceability.** Re-confirm the template's Cross-section consistency rules: every G# appears in some §3.2 `Satisfies:` and (if testable) ≥1 §5 `Covers`; every non-Skipped S# cites §3.2/§3.1, appears in ≥1 §5 `Covers`, and lands by its trigger-surface §6 phase; every T# transitions red→green across §6 phases with named red-verification evidence.
