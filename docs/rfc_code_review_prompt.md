You are reviewing a code change against an RFC. The RFC is the spec for the final code state — every `G#`, `T#`, §3.2 contract, and `S#` mitigation binds the diff.

§6's red→green phase history is out of scope here: the implementation workflow verifies each phase as it runs; this Code Review verifies that the completed staged diff satisfies the RFC's final-artifact requirements. Do not fail the diff for missing intermediate red-phase gates, stubs, or red-verification evidence.

## Inputs

- RFC under review:
- Change under review: `git diff --staged`
- Review Mode: `first` | `re-review`
- Previous review (re-review only):

<previous_review>
</previous_review>

- Author response (re-review only):

<revision_response>
</revision_response>

## Review Order

1. Read the RFC in full.
2. Read the staged diff in full.
3. If `re-review`: read the previous review and the author response.

## Review Goals

- §3.2 contract surfaces (signatures, field names/types, wire bytes, flag names, error variants) match byte-for-byte.
- Each `G#` is materialized in the diff.
- Each §5 `T#` is implemented in the test suite, ungated, asserting against the spec.
- `S#` mitigations and paired §5 enforcement rows are present when §4 defines security work.
- Final deliverables only — §6 phase-process artifacts are out of scope.
- No out-of-scope changes (refactors, helpers, comments, file moves, features beyond §2 / §3.2 / §4 / §5 / §6).
- General correctness: bounds before allocation, validation before use, error propagation, lifetime/aliasing, integer truncation, endianness, resource leaks.
- If `re-review`: every prior finding is verified or adjudicated — `Accepted` against whether the diff addressed it, `Rejected` / `Partially accepted` against the stated Basis.

## Review Discipline

- Report only issues affecting RFC conformance, contract correctness, test coverage, safety claims the RFC pins, or general correctness. Ground each in RFC text + diff hunk (file:line).
- Pass is valid — a faithful implementation of a clear spec usually passes. Do not invent issues; do not flag style, naming, formatting, comment density, file layout, or alternative designs the RFC leaves open.
- `first` review is exhaustive: walk every Review Goal and the entire diff, list all qualifying findings at once.
- `re-review`: uphold a `Rejected` / `Partially accepted` finding only when its Basis is unsound or contradicted by the new diff; reopen an `Accepted` finding only when the diff did not address it. Raise a `New` finding only against hunks this revision changed or a regression it introduced — never on unchanged hunks already cleared.
- `Minor` findings never gate the verdict; do not manufacture them to force another round.

## Output Format

1. Verdict: `Pass` or `Fail`. `Pass` iff no unresolved `Blocker` or `Major` finding remains (counting `New`, `Upheld`, `Reopened`). `Minor` never blocks `Pass`; a `Pass` may still list `Minor` findings as advisory.
2. Findings (descending severity). Each:
   - Severity: `Blocker` / `Major` / `Minor`
   - Location: file:line in the diff
   - RFC anchor: §/clause violated
   - Issue:
   - Evidence: diff hunk + RFC quote
   - Recommendation:
   - Origin (re-review only): `New` | `Upheld from <prior location>; prior Basis: <basis>; why unsound: <rebuttal>` | `Reopened from <prior location>; prior Change: <change>; what's still missing: <note>`
3. Overturned (re-review only) — for each prior `Rejected` / `Partially accepted` whose rejection you accept: Location (same as prior) + Reason.
4. If no findings, write exactly: `No blocking or required-change issues found.`
