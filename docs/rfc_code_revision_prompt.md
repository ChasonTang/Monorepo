You are the author of the staged code change. Revise it to pass the next CR round while keeping the RFC satisfied.

## Inputs

- RFC under implementation:
- Change under revision: `git diff --staged`
- CR feedback:

<review>
</review>

## Revision Discipline

1. Resolve every `Blocker` and `Major` finding. `Minor` is advisory — resolve, reject, or defer at your discretion; a deferred `Minor` is terminal (CR will not reopen it). CR passes once no `Blocker`/`Major` remains, so do not chase `Minor` to zero.
2. The revised diff must still satisfy the RFC — every `G#`, `T#`, §3.2 contract surface, `B#` migration, and `S#` security mitigation.
3. Reject a finding only if it conflicts with the RFC or verifiable project / codebase facts, or its evidence does not hold against the current diff.
4. Do not make changes outside the review's scope — no "while I'm here" cleanups, refactors, comment polish, or file moves.
5. `Blocker`/`Major` findings with `Origin: Upheld` or `Reopened` are binding — address or escalate. For `Upheld`, do not re-reject on the listed Basis; for `Reopened`, do not repeat the prior Change the reviewer judged inadequate. `Minor` is never binding.
6. For `T#` rows this revision touches, keep the test ungated and asserting against the spec. Run the local test suite before reporting.
7. `git add` the touched files so `git diff --staged` reflects the cumulative revised state. Do not commit until CR returns `Pass`.

## Output Format

1. Apply revisions directly to the source files; do not paste the full diff.
2. Findings response — for each finding, in review order:
   - Disposition: `Accepted` / `Rejected` / `Partially accepted` / `Deferred` (`Deferred` is `Minor`-only, needs no Basis, never valid for `Blocker`/`Major`)
   - Location: same as the original finding (file:line)
   - Change: one line on what changed in the diff, or why not
   - Basis (only for `Rejected` / `Partially accepted`): RFC text / project fact / diff hunk that contradicts the evidence
