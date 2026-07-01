Write an RFC for the requirement below using docs/rfc_000_design_doc_template.md. Every rule in that template is a hard constraint. If anything is unclear while drafting, ask the user instead of fabricating details.

<requirement>
</requirement>

## Local Execution Constraint

The dev environment can build and run only macOS host-arch binaries. macOS alternate arch, Linux x86_64, iOS Simulator, and iOS device targets cross-compile but cannot execute, and there is no CI. Only host-runnable `T#` rows produce executable red/green evidence; alternate-platform branches are verified by successful cross-compile + code review.

For any RFC whose §3.2 has platform-conditional mechanism (`#if defined(__linux__)`, kqueue-only/epoll-only, iOS-only entitlements, ...):

1. **Tests target the platform-agnostic contract.** Write `T#` rows against the abstract API, not the backend syscall, so the host-arch backend exercises the contract on every row.
2. **§6 `Done when` must enumerate host-runnable vs cross-compile-only `T#` branches.** Name the binary that runs each row and the binaries that are built-but-not-executed (e.g., *"the Linux binary is cross-compiled but not executed in this RFC; T14's timerfd branch is compiled but not run"*). `rfc_010_event_loop.md` §7 is the reference pattern.
3. **§3.2 Mechanism prose must acknowledge alternate-platform behaviors that are not runtime-verified in this env** — one sentence in the relevant subsection's **Unstated contract** paragraph. Do not route this through §4 Security; §4 is trust-boundary attacks only.

**Misfit:** an RFC whose `T#` rows are all cross-compile-only (purely Linux/iOS behavior with no host-runnable contract surface) fails the Pre-Draft Fit Check under "no executable red→green split in this env" — stop and propose deferring or carving out a host-runnable contract row.

## Single-RFC Hard Caps

Run this triage **before drafting** and again before declaring the draft ready. These are hard caps for a single RFC, not style preferences:

- Body length after deleting template instructions and excluding `## Revision Notes`: **<= 750 lines**.
- §5 test rows: **<= 20** rows matching `T#`.
- Each §5 `T#` row: **<= 2500 characters** and **<= 8 named subcases**.
- §3.2 detailed-design mechanism surfaces: **<= 5** subsections matching `#### 3.2.#`.
- Independent touched modules / ownership boundaries: **<= 5**.

If the requirement is likely to exceed any cap, **stop before drafting**. Do not produce a partial oversized RFC. Return a split proposal with 2-5 smaller RFCs, each independently reviewable, implementable, and testable, and ask which one to draft first.

Use this refusal shape:

```text
This request exceeds the single-RFC hard cap.
Reason: estimated <cap exceeded>.
Proposed split:
1. RFC A: ...
2. RFC B: ...
3. RFC C: ...
Please choose which RFC to draft first.
```

A 1000-line RFC is a process failure unless the user explicitly approves an exception after seeing the split proposal. Even with approval, the draft must carry an `Exception:` paragraph before §1 explaining why the work cannot be split and which cap is exceeded.

## Pre-Draft Fit Check

Run **before drafting**. The template hard-wires TDD red→green — §2 needs ≥1 testable Goal, §5 needs rows asserting observable behavior, §6 needs every `T#` to transition red→green across ≥2 phases. Requirements with no observable behavior change force fabricated `T#` rows or abused non-testable Goal annotations.

**Fit test — both must hold:**

1. The requirement names (or implies) ≥1 concrete observable outcome a `T#` could assert on (return value, error variant, wire bytes, schema field, log line, rendered artifact).
2. The implementation can split so the assertion fails first (red) and passes later (green) while the default local test suite stays green in between.

**Common misfits:** pure docs/explanation with no code-path change; mechanical dependency upgrades; pure renames, file moves, or formatter sweeps; research/spike work with no committed deliverable; ADRs, deprecation notices, postmortems; UI/UX changes whose acceptance is purely visual.

**If the requirement is a misfit, do not draft.** Stop and report:

- Which axis fails (no observable outcome, no red→green split, or both), citing the triggering phrase.
- One or two concrete reformulations if plausible (e.g., "improve docs" → "add §X with sections Y, Z plus an example covered by a `compiles-clean` lint row"; "refactor X" → "land characterization tests T1–T3 in red, then refactor to keep them green").
- The option to write a different document type (ADR, deprecation notice, design note).

Wait for the user. Do not fabricate a testable Goal or invoke the §2 non-testable fallback to force a fit.

Save to docs/rfc_NNN_{slug}.md, where NNN is the lowest unused three-digit index in docs/.

## Pre-Submission Self-Check

Run each check before declaring the draft ready.

0. **Hard-cap lint.** Run `python3 docs/rfc_lint.py <rfc-path>` and fix every failure. If the lint fails because the requirement genuinely needs more than one RFC, stop and return the split proposal instead of continuing to revise the same oversized draft. If lint emits warning-level cap pressure, include a short split-risk note in the final response: either state why the RFC is still cohesive, or propose the smallest follow-up RFC boundary that should absorb future expansion.
1. **Coverage matrix.** Persist the matrix as a `### 5.0 Coverage Matrix` subsection in the RFC (before the §5 test-row table) — a `(axis-value) → [T#, …]` table. Every reachable cell needs a §5 row; an empty `[]` is a missing-row finding. Persistence (not transient scratch) is mandatory: a later reviewer building the matrix from scratch routinely misses different cells than the writer, accumulating "missed (state, event) cell" `Major` findings across review rounds. Update §5.0 in the same revision that adds, splits, or removes a §5 row — a stale §5.0 is a form defect. Axes:
   - **G#** — every testable Goal.
   - **State** — every `(state, event)` cell §3.2 branches on; same fault class under two different states is two rows.
   - **Completion mode** — happy single-call vs. happy staged-AGAIN-resume (from `s.write_off` / `s.buf_used`) are distinct even though both end in `OK`.
   - **Decoder branch** — every distinct decoder error variant gets its own row; "all map to EPROTO" is not coverage.
   - **Benign-vs-fatal split** — `if err != 0: handle else: continue` needs rows for both arms; the benign arm catches regressions that drop the guard.
   - **Constructor / factory precondition** — synchronous validation in §3.2.1 contract surface needs rows independent of the wire-decode rows that exercise the same input downstream.
   - **Callback-safe lifecycle hand-off** — behaviors documented as "legal inside `on_done`" (destroy/re-arm from inside callback) need a row firing them, ideally under a sanitizer §6 already configures.
   - **Post-syscall sub-branch** — when §3.2 shows a syscall succeeding then a follow-up failing (e.g., `accept(2)` OK then `fcntl(F_SETFL)` failing on macOS), each follow-up failure class is its own cell. A `fail_next_<syscall>` hook that bypasses the real syscall cannot reproduce this cell — add both the hook and the row.
2. **Paired-mode symmetry.** For paired roles (Client/Server, Encoder/Decoder, Read/Write), each §5 row on one side has its analogue on the other under the *equivalent* state. If state machines diverge, analogues live under equivalent states, not "any state with the same fault". Unjustified asymmetry → add the row or justify in §3.2 prose.
3. **Code grounding.** Verify every claim citing a prior RFC's signature, struct field, file path, line number, function name, errno, flag, or behavior against current code (read the file, grep the symbol). Code, not the cited RFC, is authoritative. Keep the grep commands and hit counts on hand (`grep -n io_handles odin/event_loop_internal_test.h → line 54`) — "I read the code" without the command is the failure mode that ships `io_count` when the header defines `io_handles`.
4. **Internal contradiction grep.** For every "X cannot Y" / "X must Y" / "X is Y" claim, grep the entire RFC for the load-bearing noun and synonyms; one stale recurrence produces a second-round finding. Keep the grep commands on hand.
5. **Numeric consistency.** §5 table row count, §5 narrative range references (`"Rows T1–T21 are unit tests"`), and every §6 `"Done when"` range reference all agree.
6. **Phase-gate map.** Build `T# -> red phase -> gate / red trigger -> implementation dependencies -> implementation phase -> ungate phase` before declaring ready. Dependencies include public APIs, test-only declarations/counters, sibling-module hooks, cleanup/lifecycle paths, scope-check actions, GN configs, and fixture behavior. No row may be claimed executable-red before its declarations / stubs / fixtures exist; no row may be ungated before the mechanisms it asserts are implemented and wired; no production cleanup or security path may ship in an ungated phase while the row proving its lifecycle / scope safety remains gated.
7. **Template traceability.** Re-confirm the template's Cross-section consistency rules: every G# appears in some §3.2 `Satisfies:` and (if testable) ≥1 §5 `Covers`; every non-Skipped S# cites §3.2/§3.1, appears in ≥1 §5 `Covers`, and lands by its trigger-surface §6 phase; every T# transitions red→green across §6 phases with named red-verification evidence.
8. **Trust-boundary enumeration.** List every peer-supplied byte that reaches a syscall (`connect`, `exec*`, `open`, `dlopen`) or drives an outbound resource selection (which host to dial, which file to read, which binary to execute). Each entry needs either an `S#` with a code-level enforcement point in §3.2 plus a paired §5 row, or a §4 Skip whose justification names the specific upstream layer that owns the policy **and** shows this RFC's public API exposes the hook that layer needs. A Skip pointing at "the upstream caller" with no API hook is unsound — add the hook (and the `S#`) before declaring ready. SSRF via peer-supplied destination addresses, command injection via peer-supplied argv bytes, and path traversal via peer-supplied filenames are the classic instances.
9. **Signal-handling enumeration.** For every §5 row whose Setup performs an out-of-band-signal-generating action (close-while-writer-active → `SIGPIPE`, fork-and-exit-without-`waitpid` → `SIGCHLD`, raise `SIGINT`/`SIGTERM` without a handler), confirm the Setup or the §5-narrative fixture installs `signal(SIGPIPE, SIG_IGN)` / equivalent suppression. An unguarded signal terminates the test process by signal before any assertion runs — the row fails by signal, not by assertion, and the failure mode masquerades as an unrelated harness defect.
10. **Runtime contract.** For every §5 row, walk every API call from test code (`odin_*_*`, libc, kernel syscall) and check the runtime contract: (a) **Thread-affinity** — APIs asserting owner thread (`assert_owner` / "owner thread only" — grep the API's source) must be called from the matching frame; cross-thread coordination uses an owner-thread polling-inspector timer (`T11InspectorTimer` at `odin/server_session_unittests.cpp:944` is the reference: a recurring owner-thread `odin_event_timer` polls an `std::atomic<bool>` flag set by the test-side thread, then calls the owner-thread-only API once observed). A test-side `std::thread` calling `odin_event_loop_stop(loop)` directly aborts in debug builds (default `is_debug = true` leaves `NDEBUG` undefined). (b) **POSIX I/O semantics** — `read() == 0` assertions must drain known-pending bytes first (e.g., RFC-020's 4-byte OK CONNECT_RESP from `S_WRITING_OK_RESP → S_RELAY`) before probing for EOF; POSIX `read()` returns pending data before signalling EOF. "Connect blackholed" / "stuck in S_DIALING" claims against an unattended `listen(2)`'d local port are wrong — the kernel completes SYN/SYN-ACK/ACK into its backlog regardless of application `accept(2)`. (c) **Kernel ordering** — `std::thread` spawn order does not match `accept(2)` arrival order on loopback; multi-peer rows asserting per-peer payload mapping need per-peer listener / fixed sentinel / echo-back tag, not per-iteration `"upstream-N"` payloads.
11. **Doc hygiene.** Re-read §2, §3.2 internals, and §5 assertions: (a) every G# in §2 describes an outcome ("for every X, Y happens"; "the public API exposes hook H"), not a function-call sequence, struct field list, or sibling-RFC `§A.B.C` citation chain — the template's §2 Means-vs-Goals rule rejects Goals that an implementation swap invalidates. (b) Every "only X" / "no other Y" / "all Z" claim in §3.2 grep-verified against the surface it quantifies (every `set_*` on the public API, every callback slot on owned sub-objects); one missed entry point invalidates load-bearing safety arguments. (c) Every public-API parameter and every defensive branch has an observable consumer or §5 row triggering it — drop decorative `errnum`/`flags` parameters and remove `if no_match: cleanup()` arms with no triggering row (CLAUDE.md §1/§2). (d) Every NULL-receiver / boundary-input branch documented in §3.2.1 Unstated contract has ≥1 §5 row (often foldable into an existing synchronous row); a regression dropping the NULL-guard only surfaces in production callers. (e) Every Expected Result asserts an observable state change, not just "no crash" / "no ASan report" — extend "doesn't crash" assertions with a follow-up that proves the state change is observable (a counter on the cleared callback stays at zero while a sibling increments; a subsequent peer reaches the default fallback path).
12. **Test adequacy.** For every §5 row, ask "what buggy implementation would still pass this assertion?" The existing self-check items 1–11 target *specific* gaps; this item is the meta-question that catches the long tail. For each row, keep one concrete rejected implementation on hand in the form `T#: rejects an implementation that <buggy behavior>`; if you cannot name one, strengthen the row before declaring ready. Four patterns recur across RFCs and surface only in later rounds when missed in `first`: (a) **Range / CIDR representative** — a row asserting behavior for a range (CIDR, port range, timestamp window) listing only one mid-range cell falls to a regression that hard-codes only that representative; for each named CIDR in §3.2 the matrix needs the low, the high, and one allow-side neighbor; for each port / timestamp range, low + high + adjacent. `169.254.169.254` alone for `169.254.0.0/16` (a single metadata IP that a special-case branch could cover) and `10.1.2.3` alone for `10.0.0.0/8` are the classic instances; do not forget `255.255.255.255` when §3.2 calls it out. (b) **Mirror vs integrated path** — when a row exercises a code path through a test-only mirror / probe / setter (e.g., the row calls `odin_*_test_set_*` directly rather than driving the path through a live wire-protocol message that traverses the production entry), add ≥1 row driving the same path through the production entry; a regression that wires a stricter production callback in the installed runtime path passes the mirror row but breaks the integrated row, and the per-component mirror is exactly the surface most likely to drift from the production wiring. The same shape recurs for "test asserts behavior X via a side channel that fires regardless of whether the path was actually exercised": the parent-side post-exit reachability probe that passes even if the child never opened the listener, the in-process helper that returns early without ever installing the handler under test. (c) **Blocking I/O deadlines** — every row whose Setup reads from a pipe, socket, or fd while a child remains alive needs a deadline (poll / select with timeout, `SO_RCVTIMEO`, or a parent-side `waitpid` deadline backed by a deadlined read); a `waitpid` deadline alone does not protect a parent blocked in `read()` on stderr / progress / probe pipes or client / upstream sockets — the row fails by hang, not by assertion, and the failure mode masquerades as harness flakiness. "Child intentionally paused" rows (paused before exit to assert mid-life state) are the highest-risk: the parent will block indefinitely instead of timing out cleanly. (d) **Cross-module test-hook contract** — when a row reaches into a sibling module through a test-only hook, the RFC must declare (i) a concrete header symbol exposed in a `*_internal_test.h` (or equivalent), (ii) the sibling's documented call site gated by the matching `#define` (with the call site pinned in §3.2 of the relevant module, not implied), and (iii) the §6 build-target / GN-config activating the gate target-wide so the sibling's test translation unit sees the gated declarations; missing any of (i)–(iii) makes the row non-buildable when implementation reaches the hook, and the gap surfaces as a `Major` finding in a later round.
