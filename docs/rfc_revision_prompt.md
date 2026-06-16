You are the original author of this RFC. Revise it to pass the next review round while keeping the original requirement and template satisfied.

- Template: docs/rfc_000_design_doc_template.md
- RFC under revision:
- Original requirement:

<requirement>
</requirement>

- Review feedback:

<review>
</review>

## Revision Discipline

1. Resolve every `Blocker` and `Major` finding. `Minor` is advisory — resolve, reject, or defer at your discretion. A deferred `Minor` is terminal **within this cycle** (the immediate re-review will not reopen it), but a subsequent fresh `first` review may independently re-derive it, possibly at higher severity if it traces stronger evidence. Defer only when you accept that re-derivation risk. Do not chase `Minor` to zero — the review passes once no `Blocker`/`Major` remains.
2. The revised RFC must still satisfy the original requirement, follow the template's Writing Instructions and per-section notes, and stay internally consistent, executable, and testable.
3. Reject a finding only if it conflicts with the requirement, template, or verifiable project facts, or its evidence does not hold.
4. Do not make changes outside the review's scope.
5. `Blocker`/`Major` findings with `Origin: Upheld` or `Reopened` are binding — address or escalate. For `Upheld`, do not re-reject on the listed Basis; for `Reopened`, do not repeat the prior Change the reviewer judged inadequate. `Minor` is never binding.
6. **Patch fatigue.** When a `Blocker` / `Major` is `Reopened` for the third time on the same code site (same §3.2 mechanism, same struct field, same lifecycle hazard, same trust boundary), treat the reviewer's escalate-to-redesign Recommendation as binding by default. To reject the escalation, the revision response must show that the prior three Change attempts addressed distinct root causes — not three variations of the same patch shape — and articulate why the contract surface itself is not the problem. Three patches on one mechanism is the signal to step back and ask whether the contract (not the mechanism) needs to change. A fourth patch of the same shape almost always re-fails the next round on the same call chain.

## Post-Revision Self-Check

Run before declaring the revision ready. Re-review restricts the reviewer to changed text — regressions outside the finding's scope survive to the next fresh `first` review (one round wasted).

1. **Code grounding for new references.** For every Change line introducing or modifying a code identifier (struct field, function, file path, line number, errno, flag), grep the source to confirm exact spelling. A plausible-looking rename (e.g., `io_count` where the header defines `io_handles`) compiles in the reviewer's head and ships to the next `first` review as `Major`. Keep grep commands and hit counts on hand.
2. **Numeric consistency on changed ranges.** If a revision split, merged, added, or removed §5 rows, re-check the §5 table row count, every §5 narrative range (`"Rows T1–T18 are unit tests"`), and every §6 `"Done when"` range against the new total. A stale `"T1–T15"` after splitting T15 into T15/T16 is the classic instance.
3. **Internal contradiction grep on changed claims.** When a revision rewords a "must Y" / "cannot Y" / "is X" claim, grep the full RFC for the prior wording and synonyms; one stale recurrence reopens the finding.
4. **Cross-section side effects.** If §3.2 mechanism changed, re-check §3.2.1 **Unstated contract** paragraphs and §5 row **Expected Result** cells for stale prose. If §5 row Setup changed (e.g., rlimit math, watchdog vs destroy-trigger), re-check **Expected Result** matches the new Setup. Side effects in adjacent paragraphs survive the "only changed text" discipline as next-round regressions.

A self-check finding is not a Minor to defer — fix it before declaring ready. A `Major` regression the re-reviewer finds in your change is `New` with no Origin protection.

## Output Format

1. Apply revisions directly to the RFC file; do not paste the full document.
2. Findings response — for each finding, in review order:
   - Disposition: `Accepted` / `Rejected` / `Partially accepted` / `Deferred` (`Deferred` is `Minor`-only, needs no Basis, never valid for `Blocker`/`Major`. A `Minor` that was `Deferred` in a prior cycle and re-derives in a fresh `first` review may not be `Deferred` a second time — Disposition must be `Accepted` or `Rejected` with Basis. Two-cycle deferral debt is the workflow's accumulating-tech-debt mode; closing the loop forces either fix or explicit rejection rather than indefinite carry.)
   - Location: same as the original finding
   - Change: one line on what changed, or why not
   - Basis (only for `Rejected` / `Partially accepted`): requirement / template / RFC text / project fact
