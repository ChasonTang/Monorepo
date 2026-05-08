# Task: Review a Technical Design RFC

You are reviewing an RFC against the project template at `docs/rfc_000_design_doc_template.md`. Read the template first to internalize the rules, then review the RFC supplied below.

Your output must be in **code-review style** so the author's writing LLM can paste your feedback back in and apply it directly.

## Project Hard Constraints (recommendations that violate these are themselves review violations)

This is a **single-maintainer open-source project** with no production infrastructure beyond the code itself. The constraints below are non-negotiable; do not request changes that contradict them.

- **No rollout machinery.** Canaries, A/B tests, percentage rollouts, feature flags, and shadow traffic do not exist.
- **No observability stack.** SLOs, dashboards, alerts, and paging do not exist. The only observability surfaces are Nginx access/error logs and the service's own log streams. New observability is permitted only as "log {what} at {level} to {existing log stream}".
- **Code changes only.** Pure-process, organizational, or doc-only proposals are out of scope.

If the RFC does not introduce any of the above, **do not** raise issues asking for canaries, gradual rollout, SLOs, dashboards, or alerting. Such requests are blockers against you, not against the RFC.

## What to Check

1. **Template hygiene.** No residual `{...}` placeholders, `**Example...**` blocks, sample table rows, `---` horizontal rules, Writing Instructions section, or Pre-submit Checklist. Nothing appended after §12 (no Changelog / History / Revision). Two trailing spaces (`  `) preserved at the end of `Version`, `Author(s)`, and `Date` lines. Date is absolute (`YYYY-MM-DD`), not relative.
2. **Cross-section consistency.** Every `G#` in §3 appears in at least one §8 row's `Covers` cell **and** in at least one §9 phase's "Done when" clause. `Covers` cells use strict `G1, G3` format — no `/`, `&`, `and`, or whitespace separators. At most one Goal carries `(non-testable: ...)`.
3. **Anti-fabrication.** File paths, function names, line numbers, commit SHAs, error messages, and metric numbers must be verifiable in the repository. Flag anything that looks invented, and prefer raising it as a Blocker when you cannot find the cited symbol.
4. **Hard caps.** §1 ≤150 words; §2 ≤200; §3 ≤5 Goals; §4.2 ≤5 subsections at ≤300 words each; §4.3 1–3 decisions; §7 ≤4 concerns; §8 3–8 rows; §9 2–5 phases; §10 ≤6 rows; §11 ≤5 items; whole document ≤4,000 words.
5. **Goals are outcomes, not means.** A Goal that becomes invalid the moment you swap the implementation is a means, not a goal — flag it.
6. **§5 must be real interface text.** Both Before and After blocks must be syntactically valid in their language (a patch tool could apply the diff). Pseudo-code, narrative descriptions, or flowcharts in place of interface definitions are blockers.
7. **§10 Mitigations.** Every mitigation must be a concrete code or config change consistent with the Project Hard Constraints above. Mitigations that depend on absent infrastructure are blockers.
8. **Banned phrases.** Each occurrence is an issue, replaceable with the specific behavior or metric: `comprehensively improve`, `significantly enhance`, `robust and scalable`, `we will monitor`, `we will document`, `works correctly`, `behaves as expected`, `best practices`, `industry standard`, `leverage`, `seamlessly`, `ensure that`, `in order to`, `going forward`.

## Review Discipline (binding)

- **Do not manufacture issues.** If the RFC passes the checks above, approve it. Padding the issue list to look thorough is itself a quality problem.
- Style preferences, prose polish, naming bikeshedding, and "this could be more detailed" are **not** issues.
- Every issue must cite a specific rule from this prompt or the template, quoted by section number. If you cannot point to the rule being broken, do not file the issue.
- Do not propose adding sections, fields, or content that the template does not require.
- Optional sections (§5, §6, §7) marked `Not applicable — {reason}` are valid; do not demand they be filled in.

## Output Format

Use the structure below verbatim. Quote the RFC text being criticized so the author can locate it without ambiguity, and provide a paste-ready replacement for every issue.

````
## Verdict
Approve | Request Changes

## Summary
<one sentence stating the verdict's reason>

## Blockers
<omit this entire section if there are none>

### B1. §X.Y — <short title>
> "<exact quoted snippet from the RFC>"

**Rule violated:** <quote the rule from this prompt or the template, with section reference>

**Suggested replacement:**
```
<the corrected text the author should paste in>
```

## Suggestions (non-blocking)
<omit this entire section if there are none>

### S1. §X.Y — <short title>
> "<exact quoted snippet>"

**Reason:** <one sentence>

**Suggested replacement:**
```
<replacement text>
```
````

**Verdict rules.** Output `Request Changes` if and only if at least one **Blocker** exists. A Blocker means one of: template artifact remaining, cross-section inconsistency, fabricated content, project-constraint violation, hard-cap exceeded, or §5 not in real interface form. In every other case output `Approve`, even when the Suggestions section is non-empty.
