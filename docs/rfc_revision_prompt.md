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

1. Resolve every `Blocker` and `Major` finding; `Minor` is at your discretion.
2. The revised RFC must still satisfy the original requirement, follow the template's Writing Instructions and per-section authoring notes, and remain internally consistent, executable, and testable.
3. A finding may be rejected only if it conflicts with the requirement, template, or verifiable project facts, or if its evidence does not hold.
4. Do not make changes outside the scope of the review.
5. Findings with `Origin: Upheld` or `Origin: Reopened` are binding — address them or flag for human review. For `Upheld`, do not re-reject on the listed prior Basis. For `Reopened`, do not repeat the prior Change that the reviewer judged inadequate.

## Output Format

1. Apply revisions directly to the RFC file. Do not paste the full revised document.
2. Findings response — for each finding, in review order:
   - Disposition: `Accepted` / `Rejected` / `Partially accepted`
   - Location: same as the original finding
   - Change: one-line summary of what changed, or why it was not
   - Basis (required only for `Rejected` / `Partially accepted`): requirement / template / RFC text / project fact
