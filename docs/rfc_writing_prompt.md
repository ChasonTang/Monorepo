# Task: Write a Technical Design RFC

Generate an RFC using the project's RFC template at `docs/rfc_000_design_doc_template.md`.

## Project Context (highest priority — equal force to the template's Writing Instructions)

This is a **single-maintainer open-source project** with limited operational capacity. The following constraints are non-negotiable; designs and mitigations that violate them are unacceptable:

- **No production rollout infrastructure.** No canaries, A/B tests, percentage rollouts, feature flags, or shadow traffic exist.
- **No production observability stack.** No SLOs, dashboards, alerting, or paging. The only observability surfaces are Nginx access/error logs and the service's own log streams.
  - **Forbidden** in §4 Design, §9 Implementation Plan, and §10 Risk Mitigations: any reference to SLOs, dashboards, alerts, gradual rollout, canaries, feature flags, or any infrastructure listed above.
  - **Permitted observability changes:** only "log {what} at {level} to {existing log stream}".
- **Code changes only.** Pure-process, organizational, or documentation-only proposals are out of scope. §5 Interface Changes always describes real code/config/protocol — no pseudo-code or flowcharts in place of real interfaces.

## Inputs

```
[Requirement]
{One or more paragraphs: the problem to solve, the feature to add, or the desired
 outcome. Describe observable behavior, real error messages, and known constraints.
 Avoid subjective judgments.}

[Related Code or Background]
{e.g., "Changes concentrated in frigga/src/proxy.ts; references RFC-006 and RFC-007"
 or "No relevant prior changes". Tell the LLM which real files to read.}

[Author]
{Real name}
```

## Workflow

1. **Internalize the template's two meta-sections.** Open `docs/rfc_000_design_doc_template.md` and absorb:
   - **Writing Instructions** — anti-fabrication, strip template artifacts (`{...}`, `**Example...**`, sample rows, `---` rules, the meta-sections themselves), preserve metadata `<br>` tags, scope discipline, cross-section consistency, banned phrases, no Changelog after §12.
   - **Pre-submit Checklist** — pre-delivery self-check.

   Treat the **Project Context** above with equal force to the Writing Instructions.

2. **Read the real code.** Use Read/Glob/Grep on the paths in `[Related Code or Background]`. Every file path, function name, and interface signature cited in §4, §5, §8, §12 must come from real code — no fabrication.

3. **Allocate the next RFC number.** Identify the project's RFC directory (e.g., `docs/`, `frigga/docs/`); glob `rfc_*.md` and take the next unused number (if 001–007 exist, this is 008).

4. **Ask before guessing.** If you can't fill a section truthfully (e.g., compatibility surface unclear, success criteria undefined), **stop and ask the user**. Do not fall back to template Example content or speculation.

5. **Fill the template.** Apply both Writing Instructions and Project Context as you go. In particular:
   - Strip every `{...}` placeholder, every `**Example...**` block, every sample table row, and every `---` horizontal rule.
   - Preserve the `<br>` at the end of `Version`, `Author(s)`, and `Date` lines (keeps the metadata block from collapsing). `Status` does not need one.
   - Use today's absolute date (format `YYYY-MM-DD`).
   - Optional sections (§5, §6, §7) that don't apply: keep the heading, write `Not applicable — {one-sentence reason}` in the body. Never delete or renumber.
   - **Cross-section consistency:** every §3 `G#` appears in §8's `Covers` (comma-separated, e.g., `G1, G3`) AND in §9's "Done when". Untestable goals carry `(non-testable: {reason})` in §3 — max 1 per RFC.
   - §10 Mitigations are concrete code/config changes consistent with the Project Context (no SLOs, dashboards, canaries, etc.).
   - Empty answers: `No quantitative data available at this time` (data), `Ruled out: N/A — no viable alternative considered` (alternatives), `No significant risks identified.` (risks), `None` (Non-Goals / Future Work / References).
   - The final document starts at `# RFC-{NNN}: ...` and ends at §12 References. Do **not** include the Writing Instructions or Pre-submit Checklist sections, and do **not** append Changelog / History / Revision / Version History after §12.

6. **Self-check.** Walk every Pre-submit Checklist item. Pay extra attention to:
   - Zero `{YYYY-MM-DD}`, `{Name}`, etc. residue.
   - Zero banned phrases.
   - Every Goal covered in §8 (or marked non-testable in §3).
   - Word count in the 1,000–2,500 typical band; >4,000 → consider splitting.
   - `<br>` tags after `Version`/`Author(s)`/`Date` intact; zero `---` rules; document ends at §12 with no Changelog.

## Output

Write the RFC to `{rfc-dir}/rfc_{NNN}_{slug}.md` (`{slug}` = lowercase-underscore short topic name, e.g., `rfc_008_request_body_buffering.md`). Then report:

- Final file path
- Word count
- Pre-submit Checklist results, item by item (✓ / ✗ with explanation for any ✗)
