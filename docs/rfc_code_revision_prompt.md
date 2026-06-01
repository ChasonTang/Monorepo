You are the author of the staged code change. Revise it to pass the next CR round while keeping the RFC satisfied.

## Inputs

- RFC under implementation:
- Change under revision: `git diff --staged`
- CR feedback:

<review>
</review>

## Revision Discipline

1. Resolve every `Blocker` and `Major` finding; `Minor` is at your discretion.
2. The revised diff must still satisfy the RFC — every `G#`, `T#`, §3.2 contract surface, `B#` migration, and `S#` security mitigation.
3. A finding may be rejected only if it conflicts with the RFC or verifiable project / codebase facts, or if its evidence does not hold against the current diff.
4. Do not make changes outside the scope of the review — no "while I'm here" cleanups, refactors, comment polish, or file moves.
5. Findings with `Origin: Upheld` or `Origin: Reopened` are binding — address them or flag for human review. For `Upheld`, do not re-reject on the listed prior Basis. For `Reopened`, do not repeat the prior Change that the reviewer judged inadequate.
6. For `T#` rows touched by this revision, ensure the test remains ungated and asserts against the spec. Run the local test suite before reporting.
7. After editing, `git add` the touched files so `git diff --staged` reflects the revised cumulative state. Do not commit until CR returns `Pass`.

## Output Format

1. Apply revisions directly to the source files. Do not paste the full revised diff.
2. Findings response — for each finding, in review order:
   - Disposition: `Accepted` / `Rejected` / `Partially accepted`
   - Location: same as the original finding (file:line)
   - Change: one-line summary of what changed in the diff, or why it was not
   - Basis (required only for `Rejected` / `Partially accepted`): RFC text / project fact / diff hunk that contradicts the evidence
