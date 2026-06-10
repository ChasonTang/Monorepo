You are implementing an RFC. Default: faithful, minimal implementation.

## Inputs
- RFC under implementation:
- Repo root: current working directory

Read the RFC in full before touching code. The RFC is the spec — every `G#`, `T#`, `P#`, and `S#` clause binds you.

## Calibration

The RFC is the spec. Your job is to make the implementation changes match it, not redesign it.

- **Don't expand scope.** No refactors, helpers, comments, file moves, or features beyond what §2 Goals, §3.2 contract surfaces, §4 security entries, and §6 phase scopes pin. The reviewers approved this scope; growing it now reopens review.
- **Don't shrink scope.** Every `G#` must be implemented; every `T#` must transition red→green across §6 phases; every `S#` security mitigation and paired §5 enforcement row must be complete no later than the phase that exposes the trigger surface.
- **Phase order is binding.** Complete §6 phases in declared order. A phase is an execution checkpoint, not a separate Code Review diff: do not begin `P{#+1}` until the current phase's `Done when` clauses have been verified, but do not present or stage the final Code Review change set until every phase is complete.
- **Notation is binding.** Match §3.2 contract surfaces byte-for-byte: signatures, schema field names and types, wire bytes, flag names, error variants. These are the artifact, not pseudocode.
- **Ask, don't guess.** If a contract surface, mechanism, or `Done when` clause is ambiguous, stop and ask before fabricating details.

## Per-phase workflow

For each `P#` in §6, in declared order:

1. Read the phase's `Scope`, `Depends on`, and `Done when` verbatim. Verify all `Depends on` phases have been completed.
2. Implement only what `Scope` lists. Lift §3.2 contract surfaces into the implementation exactly; lift the §3.2 Mechanism paragraph into runnable code. When `Scope` introduces `T#` rows as red, land both the gate (`strict xfail` / `skip` / feature flag / separate test target) and any stub the gate requires in this step.
3. For `T#` rows this phase introduces **red**, run the red-verification command or mode named in `Scope`/`Done when` and confirm the row assertions fail against the current stub/behavior. Then verify the gate landed in step 2 keeps the project's default local test suite green. `skip`/flag-gate/separate-target alone do not prove red because they do not execute the assertions; `strict xfail` proves red only when the framework reports the expected failure rather than XPASS.
4. For `T#` rows this phase turns **green**, remove the gate and run the local test suite — every named row must assert for real and pass.
5. Verify each `Done when` clause against the artifact it cites: grep the §5 row, run the local test suite, read the header doc-comment, inspect the schema change artifact. Do not declare the phase done until every clause holds.
6. Treat the phase as complete only after every `Done when` clause is verified. Keep the work in the working tree and continue to the next phase; do not stage or present a Code Review diff for an intermediate phase unless the user explicitly asks.

## What to skip

- Cosmetic edits to files that §6 does not pin (formatter sweeps, comment rewording, unrelated TODOs).
- Generic safeguards that §4 does not list — if §4 invoked the Skip clause, do not add validators the RFC explicitly justified omitting.
- "While I'm here" cleanups in adjacent code.

## Final Code Review change set

After all §6 phases are complete:

1. Re-run the project's local test suite with every final `T#` ungated and asserting for real.
2. Verify the final working tree still satisfies every `G#`, every §3.2 contract surface, and every `S#` security mitigation.
3. `git add` the touched files so `git diff --staged` is the complete cumulative Code Review change set. Do not commit unless the user explicitly asks.
4. Inspect `git diff --staged` before reporting. If the staged diff contains unrelated edits or omits required RFC work, fix the staging before reporting.

## Output per phase

Report, in this exact order:

1. **Phase:** `P#` plus the one-phrase name from §6.
2. **Scope completed:** files added/modified, migrations run, flags flipped, gates applied or removed — concrete artifacts only.
3. **Done when verified:** for each clause, cite the artifact (test output line, header doc-comment, schema change artifact, grep hit) that proves it.
4. **Deviations from RFC:** list any, each with — where the RFC said X, what you did instead, why (cite the user's clarifying answer; never invent justification). `None` is the expected default.
5. **Next phase blockers:** open questions or unanswered clarifications that prevent `P{#+1}` from starting. `None` is the expected default.

## Final output

After staging the complete Code Review change set, report:

1. **Code Review change set:** files staged and the RFC phases included.
2. **Final verification:** local test command(s) run and result; final `T#` rows confirmed ungated.
3. **RFC coverage:** every `G#`, §3.2 contract surface, and `S#` security mitigation verified against the staged diff.
4. **Deviations from RFC:** list any, each with — where the RFC said X, what you did instead, why (cite the user's clarifying answer; never invent justification). `None` is the expected default.

If a phase cannot complete (ambiguity, missing dependency), stop and ask. Do not partially-implement to look productive.
