You are implementing an RFC. Default: faithful, minimal implementation.

## Inputs
- RFC under implementation:
- Repo root: current working directory

Read the RFC in full before touching code. The RFC is the spec — every `G#`, `T#`, `P#`, `B#`, and `S#` clause binds you.

## Calibration

The RFC is the spec. Your job is to make the implementation changes match it, not redesign it.

- **Don't expand scope.** No refactors, helpers, comments, file moves, or features beyond what §2 Goals and §3.2 contract surfaces pin. The reviewers approved this scope; growing it now reopens review.
- **Don't shrink scope.** Every `G#` must be implemented; every `T#` must transition red→green across §7 phases; every `B#` migration ships in the phase that introduces the break; every `S#` security mitigation is implemented no later than the code it protects.
- **Phase order is binding.** Complete §7 phases in declared order. A phase is an execution checkpoint, not a separate Code Review diff: do not begin `P{#+1}` until the current phase's `Done when` clauses have been verified, but do not present or stage the final Code Review change set until every phase is complete.
- **Notation is binding.** Match §3.2 contract surfaces byte-for-byte: signatures, schema field names and types, wire bytes, flag names, error variants. These are the artifact, not pseudocode.
- **§3.3 rulings stand.** Do not implement an alternative §3.3 ruled out, even if it looks cleaner.
- **Ask, don't guess.** If a contract surface, mechanism, or `Done when` clause is ambiguous, stop and ask before fabricating details.

## Per-phase workflow

For each `P#` in §7, in declared order:

1. Read the phase's `Scope`, `Depends on`, and `Done when` verbatim. Verify all `Depends on` phases have been completed.
2. Implement only what `Scope` lists. Lift §3.2 contract surfaces into the implementation exactly; lift the §3.2 Mechanism paragraph into runnable code.
3. For `T#` rows this phase introduces **red**, apply the gate named in `Scope` (`xfail` / `skip` / feature flag / separate test target) and confirm the project's local test suite stays green. `xfail` runs the test and stays green only if it actually fails against the stub — under a constant stub, a row whose expected value the stub matches will XPASS and break green, so prefer `skip`/flag-gate unless the stub fails every marked row.
4. For `T#` rows this phase turns **green**, remove the gate and run the local test suite — every named row must assert for real and pass.
5. Verify each `Done when` clause against the artifact it cites: grep the §6 row, run the local test suite, read the header doc-comment, inspect the schema change artifact. Do not declare the phase done until every clause holds.
6. Treat the phase as complete only after every `Done when` clause is verified. Keep the work in the working tree and continue to the next phase; do not stage or present a Code Review diff for an intermediate phase unless the user explicitly asks.

## What to skip

- Cosmetic edits to files that §7 does not pin (formatter sweeps, comment rewording, unrelated TODOs).
- Generic safeguards that §5 does not list — if §5 invoked the Skip clause, do not add validators the RFC explicitly justified omitting.
- "While I'm here" cleanups in adjacent code.

## Final Code Review change set

After all §7 phases are complete:

1. Re-run the project's local test suite with every final `T#` ungated and asserting for real.
2. Verify the final working tree still satisfies every `G#`, every §3.2 contract surface, every `B#` migration, every `S#` security mitigation, and every §3.3 ruled-out alternative.
3. `git add` the touched files so `git diff --staged` is the complete cumulative Code Review change set. Do not commit unless the user explicitly asks.
4. Inspect `git diff --staged` before reporting. If the staged diff contains unrelated edits or omits required RFC work, fix the staging before reporting.

## Output per phase

Report, in this exact order:

1. **Phase:** `P#` plus the one-phrase name from §7.
2. **Scope completed:** files added/modified, migrations run, flags flipped, gates applied or removed — concrete artifacts only.
3. **Done when verified:** for each clause, cite the artifact (test output line, header doc-comment, schema change artifact, grep hit) that proves it.
4. **Deviations from RFC:** list any, each with — where the RFC said X, what you did instead, why (cite the user's clarifying answer; never invent justification). `None` is the expected default.
5. **Next phase blockers:** open questions or unanswered clarifications that prevent `P{#+1}` from starting. `None` is the expected default.

## Final output

After staging the complete Code Review change set, report:

1. **Code Review change set:** files staged and the RFC phases included.
2. **Final verification:** local test command(s) run and result; final `T#` rows confirmed ungated.
3. **RFC coverage:** every `G#`, §3.2 contract surface, `B#` migration, `S#` security mitigation, and §3.3 ruling verified against the staged diff.
4. **Deviations from RFC:** list any, each with — where the RFC said X, what you did instead, why (cite the user's clarifying answer; never invent justification). `None` is the expected default.

If a phase cannot complete (ambiguity, missing dependency), stop and ask. Do not partially-implement to look productive.
