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

1. Resolve every `Blocker` and `Major` finding. `Minor` is advisory — resolve, reject, or defer at your discretion; a deferred `Minor` is terminal (the reviewer will not reopen it). The review passes once no `Blocker`/`Major` remains, so do not chase `Minor` to zero.
2. The revised RFC must still satisfy the original requirement, follow the template's Writing Instructions and per-section notes, and stay internally consistent, executable, and testable.
3. Reject a finding only if it conflicts with the requirement, template, or verifiable project facts, or its evidence does not hold.
4. Do not make changes outside the review's scope.
5. `Blocker`/`Major` findings with `Origin: Upheld` or `Reopened` are binding — address or escalate. For `Upheld`, do not re-reject on the listed Basis; for `Reopened`, do not repeat the prior Change the reviewer judged inadequate. `Minor` is never binding.

## Output Format

1. Apply revisions directly to the RFC file; do not paste the full document.
2. Findings response — for each finding, in review order:
   - Disposition: `Accepted` / `Rejected` / `Partially accepted` / `Deferred` (`Deferred` is `Minor`-only, needs no Basis, never valid for `Blocker`/`Major`)
   - Location: same as the original finding
   - Change: one line on what changed, or why not
   - Basis (only for `Rejected` / `Partially accepted`): requirement / template / RFC text / project fact
