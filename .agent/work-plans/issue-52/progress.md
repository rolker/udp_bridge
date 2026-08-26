---
issue: 52
---

# Issue #52 — Resend amplification under real link degradation

## Plan Authored
**Status**: complete
**When**: 2026-08-22 11:25 -04:00
**By**: Claude Code Agent (Claude Opus 5 (1M context))

**Plan**: `.agent/work-plans/issue-52/plan.md` at `036a716`
**Branch**: feature/issue-52 at `036a716` (stacked on `feature/issue-18` / PR #27, unmerged)
**Phases**: single PR with atomic commits by default; may split into three
(admission scaling / resend-budget basis / bench invariant)

Root cause refined while planning: it is not that AIMD is too slow. **Every
control parameter is scaled by `data_rate_limit_`** — the additive increase
step (`0.1 x cap` = +400 kB/s per clean sample on a 62.5 kB/s link), the
admission floor (`0.1 x cap`), and transitively the resend budget
(`0.25 x effective`). The additive step is the immediate defect: one clean
sample undoes three halvings, which is why the measured cap oscillated around
2291 kB/s and never even reached its own 400 kB/s floor. The delivery signal
itself is sound and needs no wire-protocol change; what changes is what the
step sizes and floors are measured against.

`maximum_bytes_per_second` is explicitly NOT removed — it stays as a hard
ceiling and as cost control on metered cell links. Only its implicit second
job, acting as the model of link capacity, goes away.

### Open questions
- [ ] Absolute floor value — what rate must always be admitted? (suggest 4–8 kB/s from the critical tier + BridgeInfo; boat's real control set should decide)
- [ ] Headroom target — aim for a fraction of measured capacity (prevents lockout by construction) or merely avoid overshoot? The former is a behaviour change beyond fixing the scaling.
- [ ] Deprecate `admission_floor_fraction` outright or keep one release?
- [ ] One PR with atomic commits, or three stacked PRs?
- [ ] If PR #27 merges first, rebase this branch onto `jazzy` before opening the PR.

## Implementation
**Status**: complete (first pass) — dominant cause fixed, residual documented
**When**: 2026-08-22 11:57 -04:00
**By**: Claude Code Agent (Claude Opus 5 (1M context))

**Branch**: feature/issue-52 at `cc756a5`, stacked on `feature/issue-18`
(PR #27, unmerged). Not pushed; no PR opened.

### Operator decisions applied (2026-08-22)
Absolute floor configurable with a default (`admission_floor_bytes_per_second`,
8192); headroom in scope, configurable with a default
(`link_headroom_fraction`, 0.2); `admission_floor_fraction` dropped outright;
one PR.

### Changes (4 atomic commits after the plan)
1. `admission:` AIMD step relative to the CURRENT effective cap (absolute
   minimum 2048 B/s) instead of a fraction of the configured limit; absolute
   floor clamped at use to the configured limit; congested samples target
   `(1 - headroom) x goodput`; goodput = remote received MINUS remote
   duplicates. Headroom applied only on the congested branch — goodput is
   bounded by offered load, so it is a lower bound on capacity, not an
   estimate; clamping unconditionally would ratchet a healthy idle link to
   nothing. Stale feedback keeps the plain halving (no goodput reading).
2. `resend:` budget basis moved from `effective_rate_limit_` to goodput.
   Probe guarantee re-gated on the resend FRACTION rather than the computed
   budget — a zero budget now also means "no goodput sample yet" or "link
   delivering nothing", exactly the cases the trickle exists for.
3. `docs:` both design notes rewritten around goodput scaling, with the
   per-phase bench table and a section on what the rate limit is now for;
   example_params.yaml + README parameter table.
4. `bench:` `cotenant.py` + `test_invariant_cotenant_management_flow_survives`.

### Measured result (same trajectory, before vs after; kB/s)
| phase | link | cap before | cap after | resend before | resend after | r/m before | r/m after |
|---|---|---|---|---|---|---|---|
| in_range_clean | 3750 | 3346 | 36.4 | 0.34 | 0.37 | 0.044 | 0.048 |
| fringe | 1250 | 2600 | 33.3 | 0.38 | 0.15 | 0.045 | 0.018 |
| lossy | 375 | 2480 | 30.4 | 13.94 | **0.59** | 1.643 | **0.077** |
| critical | 62.5 | 2291 | 14.1 | 26.36 | **0.90** | 3.108 | **0.144** |
| over_horizon | — | 860 | 12.4 | 3.22 | 1.35 | 0.379 | 0.185 |

Resend traffic down **24x at lossy, 29x at critical**. Duplicates at critical
3.45 -> 0.38 kB/s. The effective cap now tracks the link instead of the
config: 14.1 kB/s on a 62.5 kB/s path, against 2291 kB/s before.

**The co-tenant invariant passes.** `test_invariant_recovery_completeness`
also still passes, so the slower recovery stays within the harness's own
X=0.90-within-T=30 s criterion.

### Residual — resend invariant still xfailed
`critical`, the worst phase, now passes its ceiling with margin. Two low-loss
phases remain just over: fringe 0.018 vs 0.010, lossy 0.077 vs 0.060.
Duplicates are ~34% of the remaining resend traffic at lossy, which points at
spurious re-requests (debounce / reorder-window interaction at short RTT)
rather than the cap-scaling defect #52 documents. `xfail(strict=True)` kept,
reason updated with these numbers. `F_resend_multiplier` untouched.

### Verification
- Default suite: **165 tests, 0 errors, 0 failures** (2 new admission tests +
  1 new bench invariant).
- Opt-in `range_degradation`: **8 pass, 1 xfail, 0 failures**.
- flake8 clean.

### Open questions
- [ ] Chase the low-loss residual in this PR, or land the 24-29x win and file it separately? It looks like a different mechanism (re-request debounce vs cap scaling).
- [ ] Push and open the PR now, or hold until PR #27 merges so this can rebase onto `jazzy` rather than stay stacked?
- [ ] Still unexplained from PR #27: `msg` sits at ~7-8 kB/s in every phase including a clean 3.75 MB/s link — the Bulk tier never reaches the wifi connection's counters. Does not affect these conclusions but deserves its own look.

## Integrated Review
**Status**: complete
**When**: 2026-08-22 13:55 -04:00
**By**: Claude Code Agent (Claude Opus 5 (1M context))

**PR**: #56 at `96d9d86`
**Sources**: 2 (Copilot R1 @ `96d9d86`, CI rollup) — **no prior `Local Review (Pre-Push)` exists**; `/review-code` was never run on this branch (sub-agent dispatch was unavailable at implementation time), so Copilot is the only review source and there is nothing to cross-confirm against.
**Cross-source confirmations**: 0
**CI**: all-pass (build-and-test SUCCESS, copilot-pull-request-reviewer SUCCESS)

### Findings
- [ ] (must-fix, Copilot) **Congestion predicate uses the raw received rate, not goodput** — `src/connection.cpp:195-197`. `congested` compares `remote_received_bps` against `0.9 * sent_bps`, and `goodput = received - duplicate` is computed *after* it, used only for the headroom target. Duplicates inflate the raw figure exactly during a resend storm, so an amplifying link can read as clean, skip the headroom target, and let the cap recover — the precise pathology this PR exists to fix. Verified in source. Fix: compute goodput before the predicate and use it there too; add a regression case where duplicates push the raw rate above the threshold while goodput is below it.
- [ ] (valid, Copilot) **Negative admission floor disables the floor** — `src/connection.cpp:117`. The documented contract (`connection.h`) says negative values and NaN fall back to the default; the implementation does `std::max(0.0f, ...)`, so a negative config silently sets the floor to 0 and removes the control/feedback protection. Fix: treat `< 0` like NaN.
- [ ] (valid, Copilot) **Shipped example config fails to load** — `config/example_params.yaml:61`. `admission_floor_bytes_per_second: 8192` is an integer YAML literal for a parameter declared `double` (`declareIfMissing(..., static_cast<double>(...))`, read with `as_double()`). ROS 2 parameter overrides are type-strict, so loading the shipped example throws at configure time. Fix: `8192.0`.
- [ ] (valid, Copilot) **Resend-budget tests cannot detect a regression to the old basis** — `test/test_resend_budget.cpp:117-123`. The fixture sets `kGoodputBytesPerSec` equal to `kRateLimitBytesPerSec` so the arithmetic matches the pre-#52 numbers; every expectation would still pass if `resend_packets()` reverted to cap-based budgeting. The new basis is therefore untested. Fix: make goodput materially different from the configured cap, or add a dedicated case asserting the budget tracks goodput.
- [ ] (valid, Copilot) **Co-tenant invariant merges repeated trajectory phases** — `test/bench/test_range_degradation.py` `_cotenant_delivery_per_phase`. `PHASE_TRAJECTORY` visits `critical`/`lossy`/`fringe`/`in_range_clean` twice, and `setdefault(phase_name, ...)` merges both visits into one ratio, so starvation on the outbound leg can be diluted by the recovery leg while the test claims to check every phase. Fix: key by window occurrence and evaluate each independently.
- [ ] (valid, Copilot) **Co-tenant p95 latency is computed but never asserted** — same helper. A flow that is queued but still delivering can satisfy the 50% delivery threshold while being useless for interactive management traffic, which is the property the invariant exists to protect. `p95_latency_s` is only surfaced in the failure message. Fix: add a documented latency/gap threshold and fail on it.
- [ ] (valid, Copilot) **Doc/contract staleness cluster** — six places still describe the superseded cap-relative contract: `include/udp_bridge/resend_constants.h` header comments; `connection.h` `setResendBudgetFraction` doc ("fraction of the rate limit"); `doc/resend_budget_design.md` later "Fraction of the cap, not an absolute rate" section (contradicts the section this PR rewrote); `.agents/README.md:92` verified-parameter table (still advertises the removed `admission_floor_fraction`, omits both new parameters); `test/bench/README.md` (says five invariants, omits `K_cotenant_delivery_pct` and the `BENCH_COTENANT` marker); `test_range_degradation.py` module docstring.
- [ ] (valid, Copilot) **Plan drift** — `.agent/work-plans/issue-52/plan.md` approach item 7 still requires un-xfailing `test_invariant_resend_amplification`, which this PR deliberately retains for the #54 residual; and the "Stacking" open question is obsolete after the rebase onto `jazzy`. Per the plan-first workflow the plan must track the delivered branch.
- [ ] (valid, Copilot) **PR description reports the wrong test count** — the body says `range_degradation`: "8 pass, 1 xfail". The scenario had 9 invariants; this PR adds a tenth, and the recorded run was **9 pass + 1 xfail**. Host error in the PR body, not a code defect.

### False positives
- None. Every comment Copilot raised was verified against the source and holds. Two of them (the congestion predicate and the example-config type) are defects a local review should have caught before this reached GitHub.

## Local Review (Pre-Push)
**Status**: complete
**When**: 2026-08-22 18:10 +00:00
**By**: Claude Code Agent (Claude Opus)
**Verdict**: changes-requested

**Branch**: feature/issue-52 at `f84bde1`
**Mode**: pre-push
**Depth**: Deep (reason: ~1143-line change to a live congestion/admission-control data plane; concurrency + cross-cutting)
**Must-fix**: 3 | **Suggestions**: 6
**Round**: 1 | **Ship**: continue — must-fix present (all mechanical, but example config throws at load)

Specialists: Static analysis — tools unavailable in review shell (flake8/cpplint/yamllint absent); impl entry recorded flake8 clean. Claude Adversarial — 2 passes (Lens A logic + Lens B systemic). Copilot — off (default). Local — skipped (local_review.sh absent; Ollama unreachable).

Note: an Integrated Review (PR #56 / Copilot) already exists with unaddressed findings; this pre-push review re-verified them against the current tree and **revised the headline severity** (see first suggestion).

### Findings
- [x] (must-fix) example_params.yaml integer `8192` for a double-declared param → shipped config throws InvalidParameterTypeException at on_configure — `udp_bridge/config/example_params.yaml:61` (fix: `8192.0`)
- [x] (must-fix) negative admission floor clamps to 0, disabling the lockout floor and contradicting the header contract ("negative/NaN fall back to default") — `udp_bridge/src/connection.cpp:117` (vs `connection.h:53`)
- [x] (must-fix) doc/comment staleness cluster still describes the old cap basis after the goodput move — `doc/resend_budget_design.md:92` (internal contradiction), `include/udp_bridge/connection.h:42-47` & `247-251`, `include/udp_bridge/resend_constants.h:83-99`, `test/bench/README.md:55,106` ("five" invariants; now six)
- [x] (suggestion) congestion DETECTION uses raw received, not goodput — `udp_bridge/src/connection.cpp:195-197`. DOWNGRADED from the prior review's must-fix: the predicate is a ratio and `sent_bps` already includes our resends, so duplicate inflation cancels; the naive goodput-swap fix would cause false congestion. Add a clarifying comment/test instead.
- [x] (suggestion) resend-budget unit tests set goodput==rate_limit, so no unit test detects a regression to the old cap basis — `udp_bridge/test/test_resend_budget.cpp:119-123`
- [x] (suggestion) resend budget basis is raw goodput with no sanity ceiling; a crafted remote inflates the authorized burst (bounded only by can_send) — `udp_bridge/src/connection.cpp:706`
- [x] (suggestion) non-finite (NaN) remote feedback defeats AIMD decrease (NaN comparison false → treated clean) — `udp_bridge/src/connection.cpp:195-203`
- [x] (suggestion) co-tenant bench merges both visits of repeated phases (delivered/expected +=) and never asserts p95 latency — `udp_bridge/test/bench/test_range_degradation.py:546-551,587`
- [x] (suggestion) headroom target on the congested branch can ratchet down within sustained congestion (bounded by floor) — `udp_bridge/src/connection.cpp:214-229` (deferred: the ratchet is real, but the fix — holding the congested cap at the headroom target instead of letting the multiplicative decrease push below it — changes the convergence of a live AIMD control loop. Existing unit tests pass unchanged, so the effect is only observable in the emergent bench behavior (co-tenant starvation margin, resend amplification), and the range_degradation bench that validates it is not runnable in this environment (`unshare -Urn`: Operation not permitted). Holding higher makes the bridge take up to its full designed share, which reduces the co-tenant invariant's margin — precisely what cannot be verified here. Bounded by the floor in the meantime; deferred to a dedicated change that can be bench-validated. Tracked in the Implementation entry below.)
- [x] (suggestion) plan drift: plan item 7 mandates un-xfailing test_invariant_resend_amplification, kept xfail(strict) for the documented residual; "Stacking" open question obsolete post-rebase — `udp_bridge/.agent/work-plans/issue-52/plan.md:96-99,171`

## Implementation
**Status**: complete
**When**: 2026-08-22 18:31 +00:00
**By**: Claude Code Agent (Claude Opus)

**Branch**: feature/issue-52 at `1517514`
**Addressed**: `Local Review (Pre-Push)`, 2026-08-22 18:10 +00:00, branch feature/issue-52 @ `f84bde1` (the latest review entry; it re-verified and revised the earlier Integrated Review, PR #56 @ `96d9d86`)
**Commits**: `f8028e4` `22d6322` `28dd8bc` `8760b74` `f83d8d7` `07c8146` `5a0ce67` `5046468` `1517514`

Addressed all 10 open findings of the source review entry: 3 must-fix + 6
suggestions fixed, 1 suggestion consciously deferred with reason (below).
One logical fix per commit; code fixes carry their own regression tests.

### Actions
- [x] (must-fix) example config integer `8192` → `8192.0` so the shipped `example_params.yaml` loads against the `double`-declared param — `udp_bridge/config/example_params.yaml:61` (`f8028e4`)
- [x] (must-fix) negative admission floor now falls back to the default instead of clamping to 0 (which disabled the lockout floor); matches the `connection.h` contract; +`NegativeFloorFallsBackToDefault` test — `udp_bridge/src/connection.cpp` (`22d6322`)
- [x] (must-fix) doc/comment staleness cluster synced to the goodput basis — `doc/resend_budget_design.md`, `include/udp_bridge/connection.h` (both sites), `include/udp_bridge/resend_constants.h`, `test/bench/README.md` (five→six invariants, `BENCH_COTENANT` marker, co-tenant K rows in the Values table) (`28dd8bc`)
- [x] (suggestion) documented WHY congestion detection uses raw received (ratio cancels resend/duplicate inflation; goodput-swap would false-trigger) + `CongestionDetectionUsesRawReceivedNotGoodput` test — `udp_bridge/src/connection.cpp:195-197` (`8760b74`)
- [x] (suggestion) resend-budget test now seeds goodput ≠ rate limit; +`TracksGoodputNotCap` guards against a regression to the cap-relative basis — `udp_bridge/test/test_resend_budget.cpp` (`f83d8d7`)
- [x] (suggestion) resend budget basis clamped to the configured rate limit (a crafted-high remote goodput can no longer inflate the burst) + `BasisCappedByConfiguredLimit` test — `udp_bridge/src/connection.cpp:706` (`07c8146`)
- [x] (suggestion) non-finite (NaN/Inf) remote feedback now counts as congestion (plain halving), instead of reading as clean and defeating the AIMD decrease; headroom target skipped when feedback is unusable; +`NanFeedbackTreatedAsCongestion` test — `udp_bridge/src/connection.cpp:195-229` (`5a0ce67`)
- [x] (suggestion) co-tenant bench evaluates each phase window independently (no longer merges repeated visits) and now asserts a p95 one-way latency ceiling (`K_cotenant_p95_latency_s` = 5.0 s, documented, initial/refinable) alongside the delivery ratio — `udp_bridge/test/bench/test_range_degradation.py` (`5046468`)
- [x] (suggestion) plan reconciled with the delivered branch: item 7 records the `xfail(strict=True)` was RETAINED for the #54 low-loss residual (F_resend_multiplier not loosened), and the "Stacking" open question marked resolved (rebased onto `jazzy`) — `udp_bridge/.agent/work-plans/issue-52/plan.md` (`1517514`)
- [x] (suggestion — deferred) headroom target can ratchet below its own target under sustained congestion (bounded by floor) — `udp_bridge/src/connection.cpp:214-229` (deferred: the ratchet is real, but the fix changes the convergence of a live AIMD control loop; its only observable effect is emergent bench behavior — co-tenant starvation margin, resend amplification — and the `range_degradation` bench that would validate it is not runnable in this environment (`unshare -Urn`: Operation not permitted). Holding the cap at its designed share makes the bridge greedier, reducing the co-tenant invariant's margin, which cannot be verified here. Bounded by the floor meanwhile; deferred to a dedicated, bench-validated change.)

### Verification
- Built `udp_bridge` (`-DUDP_BRIDGE_BUILD_TESTING=ON`) — only pre-existing `-Wpedantic` flexible-array warnings.
- `test_admission_control`: **14 tests, 0 failures** (+3 new: negative-floor, raw-received detection, NaN feedback).
- `test_resend_budget`: **9 tests, 0 failures** (+2 new: tracks-goodput, basis-capped).
- Full package suite ran green; the opt-in `range_degradation` invariants **skip** here (no `unshare -Urn`), as designed — the co-tenant p95 change and the deferred item #9 both need that bench for end-to-end confirmation.
- pre-commit hooks ran on every commit (no `--no-verify`); bench Python is clean under ament_flake8 rules (≤99 cols, no E/F/W).

### Next step
Lifecycle: **Implementation → review-code** (re-review the fixes cold). The
re-review should, if it can run the `range_degradation` bench in a
namespace-capable environment: (a) confirm the new co-tenant p95 assertion
passes with margin at the chosen 5.0 s ceiling, and (b) take up deferred
finding #9 (headroom ratchet) with proper before/after bench numbers.

## Local Review (Pre-Push)
**Status**: complete
**When**: 2026-08-22 18:50 +00:00
**By**: Claude Code Agent (Claude Opus)
**Verdict**: changes-requested

**Branch**: feature/issue-52 at `ddb3420` (code at `1517514`)
**Mode**: pre-push
**Depth**: Deep (reason: rework of a live congestion/admission-control data plane; concurrency + adversarial peer input + cross-cutting)
**Must-fix**: 2 | **Suggestions**: 4
**Round**: 2 | **Ship**: continue — Round 1's must-fixes are all resolved, but Round 2 surfaces a genuine correctness/safety concern (must-fix #1) that warrants a fix + re-read

Specialists: Static analysis — cppcheck + ament_flake8 ran; no actionable findings on changed lines (cppcheck items are pre-existing untouched lines; Python E241 is intentional alignment in flake8's default-ignore set and the repo's pre-commit disables flake8; D-docstring nits at 765/798 are pre-existing untouched). Claude Adversarial — 2 passes (Lens A logic + Lens B systemic); both independently converged on must-fix #1 (cross-pass confirmed). Copilot — off (default). Local — skipped (local_review.sh is a workspace script, absent from this project repo).

Note: the prior round's 3 must-fixes (example-config double literal, negative-floor fallback, the doc staleness cluster) are all correctly fixed; the 5 new unit tests genuinely pin the new behavior; concurrency/lock-ordering/bootstrap are clean.

### Findings
- [x] (must-fix) non-finite/oversized peer-reported `remote_duplicate_bps` is not validated (`feedback_unusable` checks only `remote_received_bps`) → `goodput = max(0, finite − NaN) = 0` slams the admission cap to the floor in one sample; benign `duplicate > received` window mismatch does the same; re-opens the pathology the NaN guard claims to prevent, via the duplicate channel — `udp_bridge/src/connection.cpp:216,225,252` (cross-pass confirmed, Lens A + Lens B)
- [x] (must-fix) `.agents/README.md:92` verified-parameter table still advertises the removed `admission_floor_fraction`, omits `admission_floor_bytes_per_second` + `link_headroom_fraction`, and states the old cap basis for `resend_budget_fraction` — missing parameter-surface consequence (flagged by Copilot in Integrated Review; dropped from the Round-1 Local Review cluster, so never fixed) — `.agents/README.md:92`
- [x] (suggestion) `goodput_bytes_per_second_` stored peer-controlled and unbounded (negative duplicate inflates above received); no current exploit (downstream re-clamps) but a storage-site clamp would localize containment — `udp_bridge/src/connection.cpp:228`
- [x] (suggestion) `NanFeedbackTreatedAsCongestion` feeds NaN only into received (duplicate=0), so the NaN-duplicate / duplicate>received slam path is untested — `udp_bridge/test/test_admission_control.cpp:384`
- [x] (suggestion) headroom target ratchets the cap geometrically to the floor under sustained congestion rather than converging to `(1−h)·goodput` (already consciously deferred in progress.md; needs the bench, not runnable here) — `udp_bridge/src/connection.cpp:245-253` (deferred: unchanged from Round 1 — the fix alters the convergence of a live AIMD loop and its only observable effect is emergent bench behavior (co-tenant starvation margin, resend amplification); the `range_degradation` bench that would validate it is not runnable here (`unshare -Urn`: Operation not permitted, re-confirmed this round). Bounded by the floor meanwhile; deferred to a dedicated, bench-validated change.)
- [x] (suggestion) `test_range_degradation.py` module docstring still says "five invariant tests below" / "six vs the five below"; there are now six single-path invariants (co-tenant #52), contradicting `test/bench/README.md` — `udp_bridge/test/bench/test_range_degradation.py` (module docstring)

## Implementation
**Status**: complete
**When**: 2026-08-22 19:00 +00:00
**By**: Claude Code Agent (Claude Opus)

**Branch**: feature/issue-52 at `f13fe59`
**Addressed**: `Local Review (Pre-Push)`, 2026-08-22 18:50 +00:00, branch feature/issue-52 @ `ddb3420` (code at `1517514`) — the latest review entry (Round 2)
**Commits**: `505f940` `9e6e886` `e0da287` `dbbe1a0` `f13fe59`

Addressed all 6 open findings of the source review entry: 2 must-fix + 4
suggestions. 5 fixed (one logical fix per commit; code fixes carry their own
regression tests), 1 suggestion consciously deferred with reason (below,
unchanged from Round 1 — it needs the bench, which is not runnable here).

### Actions
- [x] (must-fix) duplicate feedback channel now validated: `feedback_unusable` also rejects a non-finite `remote_duplicate_bps` and a `duplicate > received` window skew, so such a sample falls back to the plain multiplicative decrease instead of collapsing goodput to 0 and slamming the cap to the floor — `udp_bridge/src/connection.cpp:215-219` (`505f940`)
- [x] (must-fix) `.agents/README.md` parameter table synced to the #52 goodput basis: removed `admission_floor_fraction`, added `admission_floor_bytes_per_second` (8192) + `link_headroom_fraction` (0.2), and corrected `resend_budget_fraction` to the goodput basis — `.agents/README.md:92` (`dbbe1a0`)
- [x] (suggestion) stored goodput clamped to `[0, received]` so a peer-reported negative duplicate rate can no longer inflate it above what was received; non-finite input stores 0 — `udp_bridge/src/connection.cpp:225-234` (`9e6e886`)
- [x] (suggestion) added `NanDuplicateFeedbackTreatedAsCongestion`, `DuplicateExceedingReceivedTreatedAsCongestion`, and `NegativeDuplicateDoesNotInflateGoodput` — the duplicate-channel slam/inflation paths the existing NaN test did not exercise — `udp_bridge/test/test_admission_control.cpp` (`e0da287`)
- [x] (suggestion) bench module docstring corrected to six single-path invariants (co-tenant #52), matching `test/bench/README.md` — `udp_bridge/test/bench/test_range_degradation.py` (`f13fe59`)
- [x] (suggestion — deferred) headroom target ratchets the cap geometrically to the floor under sustained congestion rather than converging to `(1−h)·goodput` — `udp_bridge/src/connection.cpp:245-253` (deferred: unchanged from Round 1. The fix alters the convergence of a live AIMD control loop, and its only observable effect is emergent bench behavior — co-tenant starvation margin, resend amplification. The `range_degradation` bench that would validate it is not runnable in this environment (`unshare -Urn`: Operation not permitted, re-confirmed this round). Holding the cap at its designed share makes the bridge greedier, reducing the co-tenant invariant's margin, which cannot be verified here. Bounded by the floor meanwhile; deferred to a dedicated, bench-validated change.)

### Verification
- Rebuilt `udp_bridge` (`-DUDP_BRIDGE_BUILD_TESTING=ON`) — only pre-existing `-Wpedantic` flexible-array warnings.
- `test_admission_control`: **17 tests, 0 failures** (+3 new: NaN-duplicate, duplicate>received, negative-duplicate clamp).
- `test_resend_budget`: **9 tests, 0 failures** (goodput-basis behavior unchanged by the storage clamp).
- Bench module docstring: `py_compile` clean; no new flake8 findings on changed lines (the D-docstring nits are pre-existing and in flake8's default-ignore / repo pre-commit-disabled set).
- pre-commit hooks ran on every commit (no `--no-verify`); the opt-in `range_degradation` bench still cannot run here (`unshare -Urn` unavailable), so deferred finding #5 remains unverifiable in this environment as designed.

### Next step
Lifecycle: **Implementation → review-code** (re-review the fixes cold). Dispatch a
fresh-context sub-agent:

    .agent/scripts/dispatch_subagent.sh --mode in-process --issue 52 --skill review-code

The re-review should confirm the duplicate-channel guard + goodput clamp hold
and, if it can run the `range_degradation` bench in a namespace-capable
environment, take up deferred finding #5 (headroom ratchet) with before/after
bench numbers.

## Local Review (Pre-Push)
**Status**: complete
**When**: 2026-08-22 19:13 +00:00
**By**: Claude Code Agent (Claude Opus)
**Verdict**: approved

**Branch**: feature/issue-52 at `a677c42` (code at `f13fe59`)
**Mode**: pre-push
**Depth**: Deep (reason: rework of a live congestion/admission-control data plane; peer-controlled feedback input + concurrency + cross-cutting)
**Must-fix**: 0 | **Suggestions**: 1
**Round**: 3 | **Ship**: recommended — no must-fix; must-fix trend 3 → 2 → 0, all Round-2 fixes verified and pinned by passing tests

Specialists: Static analysis — cppcheck (all findings on pre-existing untouched lines; none in changed admission/resend regions) + ament_flake8 (new `cotenant.py` clean; D-code docstring nits are style-only and not enforced — `.pre-commit-config.yaml` keeps black/flake8 off). Claude Adversarial — 2 passes (Lens A logic + Lens B systemic); both converged on "peer-input handling now sound and bounded", no new must-fix. Copilot — off (default). Local — skipped (`local_review.sh` is a workspace script, absent from this project repo).

Verification: fresh build with `-DUDP_BRIDGE_BUILD_TESTING=ON` clean (only pre-existing `-Wpedantic`); `test_admission_control` 17/17 and `test_resend_budget` 9/9 pass locally. The `range_degradation` bench remains not runnable here (`unshare -Urn`: Operation not permitted, re-confirmed), so the deferred headroom-ratchet item and the co-tenant p95 assertion stay unverifiable end-to-end in this environment, as designed.

### Findings
- [ ] (suggestion) `link_headroom_fraction` documented range `0.0–1.0` disagrees with the setter clamp `[0.0, 0.99]` and the `connection.h` contract `[0, 1)`; a configured `1.0` is silently clamped to `0.99` — `udp_bridge/README.md:93` (align to `[0, 1)` or note the clamp)
- [ ] (deferred, carried from Round 1) headroom target ratchets geometrically to the floor under sustained congestion rather than converging to `(1−h)·goodput` — `udp_bridge/src/connection.cpp:245-253` (fix changes convergence of a live AIMD loop; only observable via the bench, which is not runnable here — bounded by the floor meanwhile; deferred to a dedicated bench-validated change)

## Integrated Review
**Status**: complete
**When**: 2026-08-22 16:29 -04:00
**By**: Claude Code Agent (Claude Opus 5 (1M context))

**PR**: #56 at `3056a49` (round 2 of triage; branch rebased onto `jazzy` after PR #59 merged)
**Sources**: 2 (Copilot R3 @ `3056a49`, CI rollup). The three `Local Review (Pre-Push)` entries sit at older SHAs (`f84bde1`, `ddb3420`, `a677c42`), so no finding correlates at the current head.
**Cross-source confirmations**: 0 at head — but see the partial-cluster note below.
**CI**: all-pass (build-and-test SUCCESS, state CLEAN)

12 unresolved threads: 3 GitHub-marked OUTDATED, 9 marked current. Each was
re-verified against the working tree rather than trusted by label — 4 of the 9
"current" threads are in fact already fixed.

### Findings
- [x] (must-fix, Copilot) **A negative received/duplicate pair passes validation and slams the cap to the floor** — `src/connection.cpp:225-231`. `feedback_unusable` rejects non-finite values and `duplicate > received`, but not negative rates: with `received = -100, duplicate = -200`, `duplicate > received` is **false** (-200 > -100), so the sample is accepted. **Copilot's stated consequence is wrong** — it claims goodput inflates above received; the downstream `std::clamp(raw, 0, max(0, received))` catches that and yields 0 (verified numerically). The real harm is different and worse: the sample is never marked unusable, so `congested` is true (a negative received is below any threshold), the headroom target becomes `0.8 x 0 = 0`, and `effective = max(floor, min(halving, 0))` = **the floor, in a single sample**. That is precisely the pathology the received- and duplicate-channel guards were added to prevent, re-entered a third way — and reachable by any peer over an unauthenticated transport (#53). Fix: add `remote_received_bps < 0.0f || remote_duplicate_bps < 0.0f` to `feedback_unusable`; negative rates are nonsensical and should fall back to the plain halving.
- [x] (valid, Copilot) **Stale basis in the starvation-floor comment** — `include/udp_bridge/resend_constants.h:110` still reads "~1.6% of the rate limit at the default fraction"; the basis moved to measured goodput. Notable as a **partially-fixed cluster**: the Round-2 `Local Review` cited `resend_constants.h:83-99` and the fix addressed exactly those lines, leaving line 110 stale. Same failure mode as the `.agents/README.md` drop earlier in this PR — a cluster fixed to the letter of its line citation rather than its intent.
- [x] (valid, Copilot) **`link_headroom_fraction` has no test at a non-default or boundary value** — `src/connection.cpp:139`. No test calls `setLinkHeadroomFraction` at all (grepped), so the clamp to `[0.0, 0.99]`, the NaN fallback, and the effect of a non-default headroom on the congested-branch target are unexercised. `DecreaseTargetsHeadroomOfGoodput` only covers the default.
- [x] (valid, Copilot) **Plan contradicts the delivered congestion-detection behaviour** — `.agent/work-plans/issue-52/plan.md:89` still says to use `received - duplicate` "for **both the congestion test** and step 3". The implementation deliberately uses the raw received rate for detection, and Round-1 `review-code` explicitly rebutted the goodput-swap as one that would false-trigger. The plan must record the delivered decision.
- [x] (valid, Copilot) **PR description test count is wrong** — the body still claims `range_degradation`: "8 pass, 1 xfail". The scenario now has 10 tests (6 single-path + 4 multi-link) and the recorded run is **9 pass + 1 xfail**. Host error, flagged twice now and still uncorrected. (deferred: not a code defect and not actionable in this sub-agent shell — the PR #56 body is host-owned and `gh` is unauthenticated here (no push/credentials per the handoff contract). Verified against the tree: 10 tests = 6 single-path (incl. the 1 `xfail` `test_invariant_resend_amplification`) + 4 `test_multilink_*`, so the run is **9 pass + 1 xfail**. Host action: edit PR #56's description "8 pass, 1 xfail" → "9 pass, 1 xfail".)

### False positives
- (Copilot) `doc/resend_budget_design.md:44` "Fraction of the cap" section still cap-relative — **already fixed**; that section no longer exists (grepped, no match).
- (Copilot) `connection.h:55` `setResendBudgetFraction` doc still says "of the rate limit" — **already fixed**; it now reads "maximum fraction [0, 1] of measured goodput ... rebased from the configured rate limit to goodput by issue #52".
- (Copilot) `test/bench/README.md` still says five invariants — **already fixed**; line 107 reads "The six single-path invariants checked are:".
- (Copilot) `test_resend_budget.cpp:123` fixture sets goodput == rate limit so no test detects a regression to the cap basis — the fixture equality is **deliberate** (it preserves the pre-#52 arithmetic for the other cases), and the underlying concern is **addressed** by the dedicated `TracksGoodputNotCap` test added at line 212, which seeds goodput != cap specifically to catch that regression.
- (Copilot, OUTDATED) plan.md xfail item, `.agents/README.md:92` parameter table — both fixed in the Round-2 fix pass.
- (Copilot, OUTDATED) congestion predicate should use goodput — **deliberately rejected**, not missed. `sent_bps` aggregates all categories including resends, so the raw received/sent ratio is a true link-delivery measure and duplicate inflation cancels on both sides; swapping in goodput would declare congestion on a lossless link with heavy resending, and the error is self-reinforcing. Pinned by `CongestionDetectionUsesRawReceivedNotGoodput`.

## Implementation
**Status**: complete
**When**: 2026-08-22 20:51 +00:00
**By**: Claude Code Agent (Claude Opus)

**Branch**: feature/issue-52 at `484fd74` (PR #56)
**Addressed**: `Integrated Review`, 2026-08-22 16:29 -04:00, PR #56 @ `3056a49` (the latest review entry)
**Commits**: `396bc74` `4ac4646` `403a433` `484fd74`

Addressed all 5 open findings of the source review entry: 1 must-fix + 4
valid suggestions. 4 fixed (one logical fix per commit; the code fix carries
its own regression test), 1 consciously deferred with reason (a host-owned
PR-body correction not actionable in this shell).

### Actions
- [x] (must-fix) negative delivery feedback now rejected as unusable: `feedback_unusable` also rejects `remote_received_bps < 0` / `remote_duplicate_bps < 0`, so a negative pair (which slips past finite + duplicate>received yet reads as congested with a 0 headroom target) falls back to the plain halving instead of slamming the cap to the floor; +`NegativeRatesTreatedAsCongestion` — `udp_bridge/src/connection.cpp:225-231`, `udp_bridge/test/test_admission_control.cpp` (`396bc74`)
- [x] (valid) starvation-floor comment corrected from "~1.6% of the rate limit" to "~1.6% of measured goodput" (the Round-2 fix updated the cited 83-99 lines but left line 110 on the old basis) — `udp_bridge/include/udp_bridge/resend_constants.h:110` (`4ac4646`)
- [x] (valid) `link_headroom_fraction` now exercised at non-default and boundary values: `HeadroomFractionAffectsDecreaseTarget` (0.5 moves the congested-branch target off the default) + `HeadroomFractionClampedToContract` (1.0/>1 → 0.99, negative → 0, NaN → default) — `udp_bridge/test/test_admission_control.cpp` (`403a433`)
- [x] (valid) plan reconciled with the delivered congestion-detection basis: step 5 now records that goodput drives only the decrease target and resend budget, while detection deliberately keeps the raw received-vs-sent ratio (Round-1 `review-code` rebuttal, pinned by `CongestionDetectionUsesRawReceivedNotGoodput`) — `udp_bridge/.agent/work-plans/issue-52/plan.md:85-97` (`484fd74`)
- [x] (valid — deferred) PR #56 description test count "8 pass, 1 xfail" is wrong — `range_degradation` has 10 tests (6 single-path incl. 1 `xfail` + 4 `test_multilink_*`), so the run is **9 pass + 1 xfail** (deferred: not a code defect; the PR body is host-owned and `gh` is unauthenticated in this sub-agent shell — no push/credentials per the handoff contract. Host action: edit PR #56's body "8 pass, 1 xfail" → "9 pass, 1 xfail").

### Verification
- Rebuilt `udp_bridge` (`-DUDP_BRIDGE_BUILD_TESTING=ON`) — only pre-existing `-Wpedantic` flexible-array warnings.
- `test_admission_control`: **20 tests, 0 failures** (+3 new: negative-rates, headroom-affects-target, headroom-clamp).
- `test_resend_budget`: **9 tests, 0 failures** (comment-only change to `resend_constants.h`).
- pre-commit hooks ran on every commit (no `--no-verify`). The opt-in `range_degradation` bench still cannot run here (`unshare -Urn` unavailable), unchanged from prior rounds.

### Next step
Lifecycle: **Implementation → review-code** (re-review the fixes cold). Dispatch a
fresh-context sub-agent:

    .agent/scripts/dispatch_subagent.sh --mode in-process --issue 52 --skill review-code

The re-review should confirm the negative-feedback guard holds and the new
headroom tests genuinely pin the clamp/NaN/non-default-target behavior. The
one deferred item is a host-side PR-body edit, not code.

---

**New cycle, 2026-08-26.** PR #56 (above) merged as `6dda032`. The entries
above are closed history for the goodput-basis fix. This cycle starts fresh
from a 2026-08-25 Appledore field RCA (posted as an issue comment) that finds
the *AIMD control law itself* unstable — a distinct, deeper defect than the
cap-scaling issue #52 originally documented, reproduced independently of it.

## Issue Review
**Status**: complete
**When**: 2026-08-26 02:08 +0000
**By**: Claude Code Agent (Claude Sonnet)

**Issue**: #52
**Comment**: best-effort post attempted; no GitHub auth in this container
(`gh auth status --active` fails), so this entry is the canonical record —
see the "Posting" note under Recommendations below.
**Scope verdict**: needs-splitting

### Actions
- [ ] Split direction **B** (per-topic priority/class scheduling, so critical
  telemetry is not gated behind video) to issue #19 rather than bundling it
  into #52. It is a materially different mechanism (intra-connection topic
  scheduling) from the AIMD control-loop defect the RCA documents, it
  already has its own tracking issue and a branch in flight (PR #76), and
  bundling it here couples this issue's landing to that work. #52 should ship
  A (control-law fix) and C (drop-granularity regression fix) — both operate
  on the same admission/send path the RCA and its evidence are about; B does
  not.
- [ ] Capture the 2026-08-25 operator-approved three-part direction (A static
  cap / stop AIMD collapse, B priority classes, C message-level drop
  granularity) somewhere durable and visible on the issue itself. Right now
  it exists only in this run's orchestrator dispatch note, which is not part
  of the GitHub record — the RCA comment on #52 documents A's defects in
  detail but never mentions C, and B is referenced only via the
  "Relationships" section's link to #19. Per "capture decisions, not just
  implementations": post it (or fold it into plan.md, which does get
  committed) before implementation starts, so a future reader of #52 can see
  why C's scope was pulled in.
- [ ] C (message-level drop granularity) reverts behavior changed by
  `1489cfb`/`0c8b75f` (2026-05-18), which were themselves TOCTOU and
  mutex-hold-time fixes made necessary by `republish_group_` going
  `Reentrant`. Confirmed by reading both commits: the reserve-then-record
  pattern in `Connection::send` (`src/connection.cpp:466-599`) exists
  specifically to bound `sent_packet_statistics_mutex_` hold time and avoid
  re-introducing the #10 wedge. Reverting to message-level (sum-all-fragments,
  one `can_send`) atomicity must preserve that pattern's concurrency
  properties, not just its old drop semantics — plan-task should scope
  concurrency/TOCTOU regression tests alongside the behavioral ones ("test
  what breaks" — timing/concurrency is exactly the hard-to-find-in-field
  category, and this exact area has already caused two rounds of concurrency
  bugs).
- [ ] The prior cycle's PR #56 needed 3 review rounds to close a cluster of
  doc/comment staleness (`doc/resend_budget_design.md`,
  `include/udp_bridge/connection.h`, `include/udp_bridge/resend_constants.h`,
  `.agents/README.md`, `test/bench/README.md`) after the admission-control
  basis changed. This cycle changes the same control law again (detection →
  sequence-gap or matched-byte-counters per the RCA's option B, decrease →
  refractory-gated per option A, decrease target → no longer derived from
  post-gate goodput per option C). Scope the doc sweep into the plan up
  front rather than letting review rounds rediscover it piecemeal.
- [ ] Recommendation: a refractory-period duration (the RCA's option A, the
  core fix) is a new control constant in the same family as
  `kAdmissionDecreaseFactor`/`kResendBackoffBase`. Make it configurable
  per-connection like the existing admission-control parameters (Human
  control and transparency: "Design controls to be configurable"), not a
  hardcoded constant — `kResendBackoffBase`'s stale "cellular RTT 50–200 ms"
  comment, already flagged in the prior issue-52 cycle, is exactly the kind
  of drift a fixed constant invites.
- [ ] Recommendation: this is the **third** distinct root-cause pass at this
  same admission-control loop in roughly two weeks (cap-scaling → this RCA's
  control-instability), and it is implicated in at least three other issues
  (#9, #45, #71) plus two field incidents. Once this lands, consider
  recording the settled design as a durable design note (ADR or equivalent)
  rather than only in `doc/admission_control_design.md`/
  `doc/resend_budget_design.md` prose, so the next incident's RCA does not
  have to re-derive the control law from source before it can evaluate it.

### Scope Assessment

**Well-scoped?** Partially. Direction A (control-law fix: refractory period,
loss-detection basis, decrease-target basis) and direction C (drop
granularity) both act on the connection's admission/send path and are
supported by the RCA's own evidence and by the repo's commit history
(`1489cfb`/`0c8b75f`) respectively — a single PR covering A+C is reasonable.
Direction B (priority classes) is a different mechanism with its own issue
(#19) and an in-flight PR (#76); bundling it here is scope creep the
orchestrator's own open question already flags. Recommend splitting B out
(see Actions).

**Right repo?** Yes. `udp_bridge` is a project repo; congestion/admission
control is project-specific domain logic, not generic ROS 2 workspace
infrastructure (workspace-vs-project separation). No workspace principle is
implicated by keeping this here.

**Dependencies**:
- #19 (per-topic priority/class scheduling) — direction B; recommend
  splitting out (see Actions).
- #43 / #44 (admission floor / resend budget fractions) — already resolved
  by the prior #52 cycle (goodput-basis rewrite, merged `6dda032`). This
  RCA's defect is orthogonal and survives that fix: the control-instability
  bug is present regardless of what the decrease target is scaled against.
- #71 (idle-link cap collapse) — RCA states it is "a different trigger
  reaching the same broken decrease path"; fixing A here should reduce or
  close its severity too. Worth a cross-check (not necessarily a close)
  once A lands.
- #45 (2026-08-04 saturation RCA, unstarted) — RCA calls it "likely the same
  mechanism." Should be revisited once this fix lands, either to confirm and
  close, or to identify what's left.
- #9 (resend amplification tuning / the original 126% rx_duplicate field
  data) — background/context, no new dependency beyond what the original
  issue body already establishes.
- PR #76 — delivers runtime-settable tunables (mechanism for direction D,
  the floor) and per-topic caps (partial B). Not a blocker for A/C, but if
  the floor is to be raised as part of this work, that needs #76's tunables
  mechanism (or its own equivalent) merged first — configure-time-only
  parameters can't be raised live today.

### Principle Alignment

| Principle | Status | Notes |
|---|---|---|
| Human control and transparency | Watch | New control constants (refractory period, sequence-gap loss threshold) should be configurable, matching existing AIMD parameters — see Recommendations. |
| Enforcement over documentation | OK | Not applicable — this is domain control logic, not a compliance rule. |
| Capture decisions, not just implementations | Action needed | The 2026-08-25 operator-approved A/B/C direction exists only in this run's dispatch note, not in the GitHub-visible issue record. See Actions. |
| A change includes its consequences | Action needed | Same doc/comment surface (`doc/*_design.md`, `connection.h`, `resend_constants.h`, `.agents/README.md`, `test/bench/README.md`) drifted for 3 review rounds last cycle when this same control law changed. Scope the sweep up front this time. |
| Only what's needed | Watch | Bundling B (a distinct feature, already tracked separately) into this bugfix issue is the main "more than needed" risk — see scope recommendation to split. |
| Improve incrementally | OK | A/C are a bounded, well-evidenced fix to a single control loop; not a rewrite. |
| Test what breaks | OK, with a note | The two new field-replay regression tests (`test_admission_field_replay.cpp`) pin exactly the failure mode the RCA documents, deterministically, from recorded field telemetry — strong alignment. Note: direction C's concurrency surface (reserve-then-record, TOCTOU history) needs its own regression coverage, not just behavioral tests — see Actions. |
| Workspace vs. project separation | OK | Correctly scoped to the project repo. |
| Workspace improvements cascade to projects | OK | Not applicable — no workspace-level pattern involved. |
| Primary framework first, portability where free | OK | Not applicable. |

### ADR Applicability

| ADR | Triggered | Notes |
|---|---|---|
| 0001 — Adopt ADRs | Watch | Not strictly triggered (this is project domain logic, and the workspace ADR process targets workspace/process decisions), but see the Recommendation above: given this is the third design pass on the same control loop, a durable design-decision record (ADR-equivalent, in the project repo) would pay for itself. |
| 0002 — Worktree isolation | Yes | Already satisfied — work is proceeding in `feature/issue-52` on a layer worktree. |
| 0003 — Project-agnostic workspace | No | Change is entirely within the project repo. |
| 0008 — ROS 2 conventions | Yes | Standard — no deviation anticipated; existing package structure/testing conventions (gtest, ament) are already in use and should continue. |
| 0013 — progress.md vocabulary | Yes | This entry uses `## Issue Review`; prior entries in this file already demonstrate correct use of the full vocabulary for this issue. |

### Consequences

- Any change to the admission-control detection/decrease law must update:
  `doc/admission_control_design.md`, `include/udp_bridge/connection.h`
  comments, `include/udp_bridge/resend_constants.h` comments,
  `.agents/README.md` parameter table, `test/bench/README.md`, and the
  `test_range_degradation.py` module docstring — the exact cluster that
  took 3 review rounds to close last cycle for a smaller change to the same
  law.
- If direction C reverts to message-level `can_send` accounting, the
  reserve-then-record concurrency pattern and its rationale comments
  (`src/connection.cpp:466-599`) need to be preserved or deliberately
  revised, not silently dropped — otherwise the #10 wedge class of bug is
  back in play.
- If a new admission-control parameter is added (e.g., refractory period),
  `config/example_params.yaml` needs the correctly-typed literal — the prior
  cycle's `8192.0` vs `8192` integer/double mismatch is exactly the kind of
  ROS 2 parameter-type trap likely to recur here.

### Recommendations

- See the Actions checklist above (split B to #19; capture the A/B/C
  decision on the issue/plan; scope concurrency tests for C; scope the doc
  sweep up front; make the refractory period configurable; consider a
  durable design record after this lands).
- Posting: `gh auth status --active` fails in this container (no GitHub
  read/write auth), consistent with the injected-context dispatch path
  (issue #552-style host-fetched context). This progress.md entry is the
  canonical record per the skill's best-effort posting contract; the
  comment post is skipped rather than attempted and silently failing.

## Plan Authored
**Status**: complete
**When**: 2026-08-25 22:24 -04:00
**By**: Claude Code Agent (Claude Sonnet)

**Plan**: `.agent/work-plans/issue-52/plan.md` at `26f39b8`
**Branch**: feature/issue-52 at `26f39b8`
**Phases**: single PR with atomic commits; two independently-testable
clusters (A1-A3 control-loop fix, C message-level drop granularity) plus a
doc/config sweep scoped up front

Scope: direction A (stop the AIMD collapse — refractory period with
exponential growth/reset, a decrease target that ignores goodput when the
connection was self-limiting this interval, and EWMA smoothing on the
congestion detector's received-rate input) and direction C (restore
message-level `can_send` atomicity in `Connection::send`'s batch overload
via a check-and-reserve aggregate reservation, preserving the existing
reserve-then-record lock-hold-time discipline). B (priority classes) stays
split to #19 per the operator-approved scope decision already recorded on
the issue.

RCA option B (sequence-gap / matched-byte-counter loss detection) is
explicitly deferred: `updateAdmissionControl`'s signature is fixed by the
already-committed field-replay regression tests
(`test_admission_field_replay.cpp`, `4002cad`), so a detector using new
wire-protocol fields is out of reach without a schema/type-hash change
requiring coordinated redeploy — flagged as a follow-up rather than a
silent gap. What A3 delivers instead (local EWMA smoothing of the received
signal) narrows but does not eliminate defect 2 from the RCA.

The two committed field-replay tests are the acceptance gate; exact
refractory tuning constants (base period, growth factor, cap) are left as
an implementation-time build-test-tune loop against those tests rather than
hand-derived on paper — the plan documents the mechanism and the tuning
method, not final numbers.

### Open questions
- [ ] Exact refractory base period / growth factor / cap — tuned during implementation against `test_admission_field_replay.cpp`.
- [ ] New test file for C's concurrency/TOCTOU regression test, or extend `test_connection_rate_limit.cpp`?
- [ ] Should the refractory growth/cap be per-connection-configurable too, or fixed constants like `kResendBackoffBase`?
- [ ] Revisit #71 and #45 after A lands — RCA states they should reduce/close in severity.
- [ ] File the RCA-option-B (sequence-gap/matched-byte-counter detector) follow-up issue now or after this PR lands and its effect is measured?

## Plan Review
**Status**: complete
**When**: 2026-08-25 22:35 -04:00
**By**: Claude Code Agent (Claude Opus) — independent fresh-context dispatch (not the plan author; the workspace's shared `$AGENT_NAME` makes the skill's name-match heuristic unusable here)

**Plan**: `.agent/work-plans/issue-52/plan.md` at `26f39b8`
**PR**: PR-less (`--issue` mode, `feature/issue-52`)
**Verdict**: changes-requested

### Evaluation

| Dimension | Verdict | Notes |
|---|---|---|
| Scope | Good | 11 files, one control loop + one send path. B correctly excluded per the operator-approved split; C's root cause verified accurate against `cce4401` and the `connection.cpp:466-474` comment. |
| Issue alignment | Concern | The plan never mentions issue #52's own two acceptance criteria or the opt-in bench suite that measured the prior cycle (F3). |
| File targeting | Needs work | Misses `README.md` (the prose parameter reference) and `CMakeLists.txt`; `.agents/README.md` path is wrong (F4). |
| Consequences | Needs work | Overhead traffic also flows through the batch overload (F5); reservation release paths unspecified (F6); new knob is configure-time-only (F10). |
| Documentation & instruction impact | Needs work | Section present and non-silent — good — but the stale-doc list omits `README.md:211-212`, which states the very decrease-target rule A2 changes. |
| Principle alignment | Concern | "Test what breaks": the acceptance gate is one-sided and A3 can pass it by making the controller deaf (F2). "A change includes its consequences": bench re-run unscoped (F3). |
| ADR compliance | Good | 0002 / 0008 / 0013 correctly identified and satisfied. |
| ROS conventions | Good | Parameter follows the existing `declareIfMissing` / per-connection pattern; see F10 on configure-time-only. |

### Method note

Findings F1 and F2 are empirical, not analytical. I built the branch and ran
the committed gate tests (both fail as documented: `lowest = 8192`;
`delivered_fraction = 0.39777`; final cap `633091`). I then built a Python
model of the control law plus `PacketSendStatistics` (10 s box filter,
1 s `can_send` window) and validated it by reproducing those three numbers
exactly, then ran the plan's proposed A1/A2/A3 through both replays. Script:
scratchpad `sim.py` (session-local, not committed); every number below is
reproducible from the plan text plus `test_admission_field_replay.cpp`.

### Findings
- [ ] (must-fix) A2 as specified cannot pass either gate test at ANY refractory tuning — the clamp, not the constants, is the binding constraint — `plan.md` A2 / Open Questions
- [ ] (must-fix) The acceptance gate is one-sided: A3's EWMA passes it at alpha=0.2 by never detecting congestion at all; needs a converge-under-genuine-loss counter-test — `plan.md` A3 / Tests (A)
- [ ] (must-fix) Issue #52's own acceptance criteria and the opt-in `UDP_BRIDGE_BENCH_SCENARIOS=1` bench suite are absent from the plan's verification — a refractory directly opposes the convergence those invariants need — `plan.md` Approach / Estimated Scope
- [ ] (must-fix) Doc sweep misses `README.md:211-212`; `CMakeLists.txt` missing from Files to Change; `.agents/README.md` is at the repo root, not under `udp_bridge/` — `plan.md` Doc/config sweep + Files to Change
- [ ] (must-fix) C's "two call sites" premise is wrong: overhead (BridgeInfo, resend requests) uses the SAME batch overload, so atomic reservation changes the feedback channel too — `plan.md` C step 2
- [ ] (suggestion) C needs a per-return-path spec for reservation release (success / ECONNREFUSED / `no_address` / throw) — `plan.md` C steps 2-3
- [ ] (suggestion) The stated reason for deferring RCA option B (committed tests pin the signature) is not a real constraint; the wire-schema reason is, and a purely local resend-ratio detector is never evaluated — `plan.md` A3
- [ ] (suggestion) A3's "time constant closer to sent_bps's smoothing" is unachievable as stated (variable-span box filter vs EWMA); matching the sender's window to the receiver's 5 s window is the direct fix — `plan.md` A3
- [ ] (suggestion) A1 is ambiguous about whether a CLEAN sample still increases during the refractory window; the answer changes both gate outcomes — `plan.md` A1
- [ ] (suggestion) `admission_refractory_period_seconds` is configure-time-only — the exact property RCA item D says blocked live mitigation, and which #76 addresses — `plan.md` Files to Change

### F1 — A2 keeps the `0.8 x goodput` clamp exactly where it does the damage (must-fix)

The plan defers the refractory constants to an implementation-time
build-test-tune loop, on the premise that the constants are the free
parameter. They are not. Simulated against the committed replay:

| variant | `FieldOnsetMustNotCascadeToFloor` (`lowest`, needs > 187500) | `HandoverBlip...` (delivered, needs > 0.90) |
|---|---|---|
| current code (model-validated) | 8192 FAIL | 0.398 FAIL |
| plan A1+A2, refractory base 2 intervals | 81160 FAIL | 0.880 FAIL |
| plan A1+A2, refractory base 3 intervals | 162320 FAIL | 0.880 FAIL |
| refractory base 2, clamp DROPPED | 375000 PASS | 1.000 PASS |
| refractory base 3, clamp DROPPED | 750000 PASS | 1.000 PASS |

The mechanism: A2 gates the clamp on `dropped_bytes_per_second == 0`
("we were not self-limiting"). The FIRST congested sample of an episode is
by construction a non-self-limiting interval, so the clamp is in force
exactly there. On the replay that single first step lands at
`0.8 x (254615 - 51715) = 162320` — already below the test's
`kFieldRateLimit / 8 = 187500` bound before any second decrease exists to
suppress. No refractory value can rescue it. The same step in the handover
test targets `0.8 x 198000 = 158400` against a 495 kB/s offered load,
holding delivered at 0.880.

A2's gate is also arguably inverted. Goodput is bounded above by OFFERED
load — the code comment at `connection.cpp:277-281` says so. When the gate
is not limiting (`dropped == 0`), goodput measures offered load, not
capacity: the day's operating point was ~495 kB/s offered against a
1500000 cap, so `0.8 x goodput` is a 3.8x cut carrying no capacity
information. When the gate IS limiting, goodput tracks the cap and the
clamp is mild. A2 applies the clamp in precisely the case where it is least
informative.

RCA option C's first half — "Drop the `0.8 x goodput` clamp ... Keep the
decrease multiplicative from the *current cap*, which is the controller's
own known state rather than a measurement it contaminated" — passes both
tests with margin and is simpler than what the plan proposes. If the
headroom target is kept in some form (it is the mechanism behind the
co-tenant invariant), it needs a basis other than post-gate goodput, and
that needs to be designed in the plan rather than tuned in the loop.

Two secondary notes on A2 as written: `sent_packet_statistics_.get()` is a
0-10 s span filter, so `dropped_bytes_per_second > 0` is sticky for up to
10 s — not "this interval" as the plan says; and it aggregates ALL
categories, so a resend-budget drop also disables the clamp. Whatever
survives from A2 should state the window and the category explicitly, and
compare `> 0.0f` rather than `== 0` on a float rate.

### F2 — the gate is one-sided; A3 can pass it by going deaf (must-fix)

Both committed tests assert only that the cap does NOT fall. Nothing asserts
it still falls when it genuinely should. Running the plan's A3 EWMA through
the replay:

| EWMA alpha (A1+A2+A3, base 2) | onset `lowest` | handover delivered |
|---|---|---|
| 0.5 | 40368 FAIL | 1.000 PASS |
| 0.3 | 17152 FAIL | 1.000 PASS |
| 0.2 | **1500000 PASS — cap never moved at all** | 1.000 PASS |

At alpha 0.3-0.5 smoothing makes the onset WORSE (detection is delayed until
goodput has collapsed, and the clamp then targets 0.8 x a tiny number —
another instance of F1). At alpha 0.2 the controller simply never registers
congestion, and both tests go green on a controller that has stopped
working. A build-test-tune loop whose only oracle is these two tests can
converge there.

Required before implementation: a counter-test pinning the other side —
under a genuine sustained capacity drop the cap must converge to within
some band of real capacity inside a bounded time. That bound is also the
number that decides whether the refractory backoff cap is safe: seven
halvings at an exponentially growing refractory is minutes, and issue #52's
`over_horizon` phase needs convergence in seconds.

### F3 — issue-alignment: the issue's own acceptance criteria are absent (must-fix)

Issue #52's body states two acceptance criteria: (1) the resend-amplification
invariant passes on its own terms with no loosening of `F_resend_multiplier`,
and (2) the co-tenant management-flow invariant survives every phase —
"**This invariant should pass before the cap is relaxed on anything that
goes to sea.**" The prior cycle measured itself with
`UDP_BRIDGE_BENCH_SCENARIOS=1 ... test_bench_range_degradation` and recorded
a per-phase before/after table.

This plan's only named oracle is the two field-replay unit tests. It mentions
the bench solely as a doc file to sync "if the invariant count or basis
changes". But a refractory period directly opposes convergence inside the
harness's 10 s phase holds, which is what
`test_invariant_cotenant_management_flow_survives` and
`test_invariant_recovery_completeness` depend on — and
`test_invariant_resend_amplification` is `xfail(strict=True)`, so an
unexpected improvement flips it to XPASS and fails the suite too (the
warning at `test_range_degradation.py:669-678` is explicit that raising
`F_resend_multiplier` to keep it xfailing is not acceptable). Scope the
bench run as a verification step with a before/after table.

### F4 — doc/file targeting gaps (must-fix, mechanical)

- `README.md:211-212` documents `admission_floor_bytes_per_second` and
  `link_headroom_fraction` in prose, including "Applied as the target of a
  congested backoff" — which A2 makes conditional — and it is where a new
  `admission_refractory_period_seconds` belongs. Absent from the sweep.
- `CMakeLists.txt` is absent from Files to Change; a new
  `test_message_atomicity.cpp` needs an `add_udp_bridge_gtest(...)` line
  (pattern at `CMakeLists.txt:154`).
- `.agents/README.md` lives at the REPO ROOT, not `udp_bridge/.agents/README.md`
  as the table has it.
- Minor: the `example_params.yaml` typed-literal precedent is at line 109,
  not 61. Substance is right — keep it.
- `doc/relay_design.md` and `doc/conceptual_overview.md` reference admission
  control only generically; no change expected. Fine to leave off.

### F5 — C: overhead traffic uses the same batch overload (must-fix)

The plan distinguishes "message-batch vs. everything else — overhead pings,
individual resends, which keep using the existing per-packet
check-and-reserve path unchanged". That is not the code. The batch overload
has exactly one production caller, `udp_bridge.cpp:2140`, reached with
`is_overhead` both false AND true; the single-packet overload's only other
production caller is `resend_packets` (`connection.cpp:845`). So an
aggregate check-and-reserve in the batch overload changes admission for
BridgeInfo, resend requests and topic lists as well — the feedback channel
`updateAdmissionControl` consumes, and whose disappearance `feedback_stale`
reads as congestion. Probably harmless (overhead messages are usually one
packet, and the failure path already drops them atomically today), but it
must be a decision with test coverage, not a mis-description.

### F6 — C: reservation release needs a per-path spec (suggestion)

Otherwise the C design is sound and does preserve the `1489cfb`/`0c8b75f`
discipline: the aggregate check-and-reserve is two bookkeeping operations
under one `sent_packet_statistics_mutex_` acquisition, and the per-fragment
`sendto` poll loop still runs lock-free, so the #10 wedge path
(`sendBridgeInfo` -> `data_sent_rate` -> socket drain) is not
re-established. To make it implementable without ambiguity, enumerate who
releases on each inner-send exit: success, `ECONNREFUSED`, `no_address`
(a concurrent `setHostAndPort()` can clear `addresses_` mid-loop, after the
batch's own pre-check passed), and the `ConnectionException` throws. Also
state the invariant the accounting rests on rather than "that packet's
share": `packet.packet.size() == p.packet_size` by construction
(`wrapped_packet.cpp:12`), so releasing `p.packet_size` per fragment sums
exactly to `total_size`.

The proposed real-threads TOCTOU test is the right shape and has local
precedent — `test_connection_rate_limit.cpp` already drives `std::thread`.
The open question about file placement is not worth blocking on; a new
`test_message_atomicity.cpp` is fine.

### F7 — the option-B deferral rests on the wrong reason (suggestion)

"`updateAdmissionControl`'s signature is fixed by the already-committed
regression tests" is not a design constraint. The tests call a public C++
method, they are three commits old, unmerged and changeable, and nothing
about them touches a wire schema. Recording a test as a constraint on the
design leaves a false obstacle for whoever picks up the follow-up.

The substantive reason is real and sufficient on its own: a per-connection
loss fraction over a matched interval needs counters that do not exist on
the wire — `RemoteConnection.msg` carries only `float32` rates, and
`BridgeInfo.next_packet_number` is node-level, not per-connection — so it
is a type-hash bump and a coordinated redeploy, exactly as documented for
`effective_rate_limit`. State that and drop the signature argument.

Also worth an explicit evaluate-and-reject rather than silence: the RCA
computed its own 1.11% trigger from the SENDER's own statistics —
`resend on wire 5171 / total on wire 467577`. Both terms come from
`sent_packet_statistics_` over the same window with the same filter, which
removes the skew defect outright, and `data_sent_rate(now,
PacketSendCategory::resend)` already exposes it. No wire change, no
signature change. It has its own weaknesses (roughly one RTT of lag,
confounding with resend amplification, blind when the return path is dead —
which is what `feedback_stale` covers), so it may well lose on the merits;
but it belongs in the plan's option space, which currently reads as
"EWMA or a wire change".

### F8 — A3's smoothing target is not well-defined (suggestion)

`sent_bps` comes from a variable-span box filter (1-10 s depending on deque
occupancy, `statistics.cpp:115-155`); `remote_received_bps` comes from a 5 s
box filter (`Connection::data_receive_rate`). An EWMA on the received side
is a third filter, and no alpha "matches" a variable-span box filter — the
plan's "a time constant closer to what produces `sent_bps`'s own smoothing"
cannot be made precise, which is part of why F2's tuning is unbounded. The
direct alternative is to compare like with like: compute the sender-side
rate over the same 5 s window the receiver uses.
`PacketSendStatistics::bytes_in_window` is already exactly this shape (the
resend budget uses it for the same reason — the smoothed `get()` rate lags
in precisely the transient that matters).

### F9 — A1 wording (suggestion)

"While the refractory window is active ... a congested sample is **not acted
on** — no decrease, and no additive recovery either" reads as freezing the
controller, but its subject is a congested sample. Whether a CLEAN sample
increases the cap during the window materially changes both gate outcomes
and interacts with the "reset to base on the first clean sample" rule.
State it.

### F10 — configure-time-only parameter (suggestion)

RCA item D: raising the floor "is also the one lever that would have
mitigated the problem live — and it could not be applied, because connection
parameters are configure-time only (which is exactly what PR #76 fixes)".
The plan adds `admission_refractory_period_seconds` as configure-time-only,
"matching existing admission params". That is consistent, but it makes the
new tuning knob the next one an operator cannot reach at sea. Note the
relationship to #76 explicitly, or say why configure-time is acceptable
here.

### What the plan gets right

- C's root cause is verified accurate against `cce4401` and the deliberate
  no-reserve comment at `connection.cpp:466-474`, and the fix preserves the
  lock-hold-time discipline it must.
- Scope excludes B cleanly, per the recorded operator decision.
- Consequences and Documentation & Instruction Impact sections are present,
  specific and non-silent — the `8192.0` typed-literal trap and the
  reserve-then-record preservation are both carried forward from the prior
  cycle's review rounds rather than rediscovered.
- Deferring RCA option B is the right call; only the stated reason is wrong.

### Recommended actions before implementation
- [ ] Resolve F1: drop the post-gate goodput clamp from the decrease branch
      (RCA option C as written), or give the headroom target a basis that is
      not contaminated by our own gate — and re-derive the refractory
      constants after that decision, not before.
- [ ] Resolve F2: add a converge-under-genuine-sustained-loss counter-test
      before tuning A3, so the tuning loop has a two-sided oracle.
- [ ] Resolve F3: scope the opt-in bench suite re-run (both issue acceptance
      criteria) into the plan's verification, with a before/after table.
- [ ] Fix F4's file list: add `README.md` and `CMakeLists.txt`; correct the
      `.agents/README.md` path.
- [ ] Correct F5's call-site description and decide the `is_overhead`
      behaviour explicitly.
- [ ] Amend the plan inline per plan-task's "During implementation" rules;
      this entry stays as the historical record.

## Plan Authored
**Status**: complete
**When**: 2026-08-25 22:49 -04:00
**By**: Claude Code Agent (Claude Sonnet)

**REVISION** in response to the `## Plan Review` entry above
(verdict: changes-requested, empirical model-based review).

**Plan**: `.agent/work-plans/issue-52/plan.md` at `99787b0`
**Branch**: feature/issue-52 at `99787b0`
**Phases**: single

### How each review finding was addressed

1. **[HIGH] Drop the A2 goodput clamp (F1).** Done. A2 now drops the
   `(1 - headroom) x goodput` clamp entirely; the decrease branch is
   purely multiplicative from `effective_rate_limit_`
   (`max(floor, effective_rate_limit_ * kAdmissionDecreaseFactor)`),
   matching RCA option C's first half. Carried forward the reviewer's
   table into the plan verbatim as the justification. Flagged the
   consequence the reviewer didn't ask me to resolve but that follows
   directly: `link_headroom_fraction_` loses its only call site, and
   the plan now requires implementation to either give it a new basis
   or confirm (via the bench re-run) it's redundant and flag it for
   removal — not leave it silently inert while still documented.

2. **[HIGH] Two-sided convergence counter-test (F2).** Done. Added a new
   `SustainedRealLossMustConverge` test requirement to
   `test_admission_field_replay.cpp` (Approach A3 / Tests (A)) — a
   sustained (not transient) capacity drop that the cap must converge
   toward within a bounded time. Made explicit that this is the test
   that fails EWMA alpha=0.2 and bounds how large A1's refractory
   backoff cap is allowed to grow.

3. **[MED] Evaluate the purely-local detector (F7).** Done. Added an
   explicit "Evaluate-and-reject" subsection under A3 describing the
   RCA's own local resend-ratio computation
   (`sent_packet_statistics_`-only, no wire change), its real
   weaknesses (RTT lag, confounds with resend amplification, blind
   when the return path is dead), and states plainly the operator's
   2026-08-25 decision not to widen scope to adopt it this cycle —
   recommend a follow-up issue evaluating it against A3's fix, using
   post-deployment field data. Also fixed the option-B deferral's
   stated reason while in this section (F7's secondary point): the
   real constraint is the wire-schema gap, not that the committed
   tests' signature is "fixed."

4. **[MED] Fix the C "two call sites" error (F5) + per-exit spec (F6).**
   Done. C step 2 now states the batch overload has exactly one
   production caller (`udp_bridge.cpp:2140`), used for both regular and
   `is_overhead` traffic, and flags that the aggregate reservation
   therefore also changes admission for BridgeInfo/resend
   requests/topic lists — added as an explicit test-covered decision
   (Tests (C)), not an assumption. Added a per-exit-path reservation
   release spec (success / `ECONNREFUSED` / `no_address` /
   `ConnectionException`) with the `packet.packet.size() ==
   p.packet_size` accounting invariant stated explicitly, plus a new
   test bullet exercising all four paths.

5. **[MED] Complete the doc sweep (F4).** Done. Added
   `udp_bridge/README.md:211-212` (prose parameter reference) and
   `udp_bridge/CMakeLists.txt` (test registration, pattern at line 154)
   to both the Doc/config sweep prose and the Files to Change table;
   corrected `.agents/README.md`'s path to the repo root (it is not
   under `udp_bridge/`); corrected the `example_params.yaml` typed-literal
   precedent line number (109, not 61).

6. **[MED] Issue #52's acceptance criteria + bench suite (F3).** Done.
   Added a new `### Verification` subsection in Approach naming both of
   issue #52's own acceptance criteria (resend-amplification invariant
   with no `F_resend_multiplier` loosening; the co-tenant invariant,
   already committed and non-xfail), the exact bench-suite invocation,
   a required before/after table, and the refractory-vs-10s-phase-hold
   tension the reviewer flagged — with an explicit instruction to
   resolve it by running the bench harness, not by inspection.

7. **Refractory constants framing (F8, "suggestion" but included).**
   Done. A3's "time constant closer to sent_bps's own smoothing" framing
   is replaced: `sent_bps` is a variable-span 1-10s box filter, so no
   EWMA alpha "matches" it — that indefiniteness is named as part of why
   F2's tuning loop was unbounded. The adopted fix instead compares the
   sender-side rate over the *same* 5s window
   `Connection::data_receive_rate` already uses
   (`PacketSendStatistics::bytes_in_window`), i.e. compares like
   windows rather than introducing a third, unmatched filter.

### Also addressed (reviewer "suggestion" findings, folded in for
completeness rather than left implicit)

- **F9** (A1 clean-sample ambiguity during refractory): resolved
  explicitly — the freeze is unconditional on sample type (congested or
  clean), stated inline in A1 with the rationale that this is the
  reading the F1 table's PASS results were validated against.
- **F10** (configure-time-only knob vs. #76): added an explicit
  relationship note in A1 — consistent with existing convention, not a
  blocker, but named so it isn't rediscovered as a surprise once #76
  lands.

### Open questions
- [ ] Exact refractory base/growth/cap — now gated on the two-sided
      oracle (F2's counter-test) and the bench harness's 10 s phase
      holds, not the two field-replay tests alone.
- [ ] `link_headroom_fraction`'s new basis vs. confirmed redundancy —
      must be resolved against the bench-run cotenant invariant before
      the PR is done, not before plan approval.
- [ ] `is_overhead` traffic's aggregate-reservation treatment in C —
      resolve with test coverage during implementation.
- [ ] Where to add C's concurrency/TOCTOU test (new file vs. extend
      `test_connection_rate_limit.cpp`) — no strong preference.
- [ ] Fixed vs. per-connection-configurable refractory growth/cap —
      confirm once tuned values are known.
- [ ] Follow-up issue timing for RCA option B + the evaluated-and-rejected
      local resend-ratio detector — recommend after this PR lands.
- [ ] #71 / #45 revisit timing — after A lands.
## Implementation
**Status**: complete
**When**: 2026-08-25 23:25 -04:00
**By**: Claude Code Agent (Claude Opus)

**Plan**: `.agent/work-plans/issue-52/plan.md` at `99787b0`, synced to `e08f591`
**Branch**: `feature/issue-52` at `cb1dd2f` (5 commits, unpushed)
**Scope**: directions A and C. B (priority classes) stayed out, per the operator-approved split to #19.

### What shipped

`0503f95` — **A, stop the AIMD collapse.**
- **Refractory period.** After a decrease the controller freezes for
  `admission_refractory_period_seconds` (new per-connection double
  parameter, default 5.0), unconditionally on sample type — a clean
  sample inside the window gets no recovery either, because recovering
  mid-window re-opens the loop from the other side. The window doubles
  per further decrease within an unresolved episode, capped at 2x the
  base, and resets on the first clean sample. New state under
  `config_mutex_`; a backwards clock step expires the window rather than
  freezing forever.
- **Clamp dropped.** The congested branch is now
  `max(floor, effective_rate_limit_ * 0.5)` with no goodput term.
- **Window-matched detector.** New
  `PacketSendStatistics::success_rate_in_window(time, window_seconds)`
  measures our send rate over the receiver's own 5 s window instead of
  `get()`'s variable 1-10 s span.

`03a3971` — **C, message-level granularity.** The batch overload's
aggregate `can_send` became a check-**and**-reserve under the same brief
lock; each fragment's share is handed to the inner send before the call
(now `sendPacket(..., bytes_pre_reserved)`), which releases exactly
`data.size()` on every exit including `no_address` and the throw path; a
batch-level RAII guard holds the portion for fragments never attempted.
The per-fragment `sendto` poll loop still runs lock-free — the #10 wedge
property is unchanged, and `ConcurrentSendsStayUnderLimit` still passes.

`1cfc52d` — doc sweep. `e08f591` — plan sync. `cb1dd2f` — lint fixup.

### Constants, and what bound each

| constant | value | bound by |
|---|---|---|
| `kDefaultAdmissionRefractoryPeriodSeconds` | 5.0 s | the receive-rate window both ends measure over — the shortest interval after which both filters describe traffic sent at the new cap |
| `kAdmissionRefractoryGrowthFactor` | 2.0 | the recorded onset needs one growth step; without it a third halving lands exactly at the test bound |
| `kAdmissionRefractoryMaximumMultiple` | 2.0 (10 s) | above: the bench's 10 s phase holds and the statistics deque's 10 s retention; below: `SustainedRealLossMustConverge`'s 40 s deadline |
| `kAdmissionSendRateWindowSeconds` | 5.0 s | not tuned — it is `data_receive_rate`'s window |

### Test counts

**271 tests, 0 errors, 0 failures, 14 skipped** (bench, opt-in). Branch
start was 257 with the 2 gate failures. Full suite, not a filtered run.

Gate results, matching `review-plan`'s model prediction exactly rather
than retuned into green:

| test | bound | pre-fix | after |
|---|---|---|---|
| `FieldOnsetMustNotCascadeToFloor` | `lowest` > 187500 | 8192 | **375000** |
| `HandoverBlip...` | delivered > 0.90 | 0.398 | **1.000** |
| `HandoverBlip...` | final cap > 750000 | 633091 | **1500000** |

Both new tests were verified to **fail against the defect they pin**,
not just pass against the fix:
- `SustainedRealLossMustConverge` fails on a deliberately deafened
  controller (`congested = false`) while both committed gate tests stay
  green — which is exactly the F2 hazard the reviewer demonstrated with
  EWMA α=0.2.
- `MessageAtomicity.ConcurrentMessagesAreAllOrNothing` fails on round 0
  against the pre-fix check-only aggregate. **An unbarriered 8-thread
  version passed against the pre-fix code** — it proved nothing. It
  needed a start barrier, 40-fragment messages and 25 rounds.

Bench: `UDP_BRIDGE_BENCH_SCENARIOS=1` full range-degradation scenario,
`10 passed, 2 xfailed`, no failures and no XPASS. Per-window and
per-phase tables are in the plan's Implementation Notes.

### Open resolutions closed by measurement

- **`link_headroom_fraction` (A2).** `test_invariant_cotenant_management_flow_survives`
  — issue #52's own acceptance criterion, and the parameter's operational
  claim — **passes** with the clamp removed. Resolved as the plan's
  outcome (b): the parameter is retained but **inert**, still declared,
  clamped and reported so existing configs keep working, with nothing in
  the control law reading it. Documented in five places and pinned by
  `HeadroomFractionDoesNotAffectDecreaseTarget` so re-attaching it has to
  be deliberate. Removal is a follow-up for post-deployment data.
- **Resend amplification (criterion 1).**
  `test_invariant_resend_amplification` still xfails (strict, no XPASS),
  so the marker stays and `F_resend_multiplier` was not touched.
- **`is_overhead` (F5).** Same atomic treatment, no separate path, with
  its own test.

### Deviations from the plan

1. `bytes_in_window` could not be reused for A3 (fixed 1 s window, single
   category) — added `success_rate_in_window` alongside it.
2. **Existing unit tests pinning the removed clamp had to be rewritten,
   not extended.** `DecreaseTargetsHeadroomOfGoodput` and
   `HeadroomFractionAffectsDecreaseTarget` asserted precisely what A2
   removes; `DuplicatesExcludedFromGoodput` moved its assertion to the
   goodput quantity (still live as the resend budget's basis); two more
   drove their loops by feeding N samples at ONE timestamp, which the
   refractory gate collapses to one decision. The plan listed the file
   but not that its contents were partly invalidated.
3. The single-threaded whole-message-drop test is **not** a regression
   discriminator (see above). Kept as a contract test, with the finding
   recorded rather than left implying coverage it does not provide.
4. `ECONNREFUSED` release is not unit-testable on an unconnected UDP
   socket — stated in the test file and the plan instead of leaving a
   silent gap in F6's four-path list.
5. Doc files beyond the plan's sweep: `test/bench/README.md` and the
   cotenant invariant's docstring both asserted `link_headroom_fraction`
   is the mechanism that invariant checks, which the clamp removal made
   false. Also a note in `doc/resend_budget_design.md` that goodput is
   still ITS basis, and that the refractory freeze deliberately does not
   withhold the measurement.

### Worth a reviewer's eye

- **`lossy` resend/msg moved the wrong way**, 0.077 → 0.141, while
  `fringe` (0.018 → 0.011) and `critical` (0.144 → 0.118) improved. Not
  a gate result — the invariant's verdict is unchanged — and run-to-run
  variance on this harness is material (two runs of the *same* build gave
  `critical#3` co-tenant delivery of 0.904 and 0.840). Reported rather
  than buried; worth repeated runs under #54.
- The refractory knob is **configure-time only**, like every other
  admission parameter — the exact property RCA item D names as what
  blocked live mitigation on 2026-08-25. Noted in the code, the docs and
  the plan; #76 is the fix.

### Not done

- Nothing from the plan's A/C scope is outstanding.
- No `git push` (host performs pushes). No PR opened.
- Follow-up issues not filed (plan recommends filing after this lands):
  RCA option B's wire-schema detector plus the evaluated-and-rejected
  local resend-ratio detector; `link_headroom_fraction` removal-or-rebasis;
  the `lossy` resend movement; #71 / #45 revisit; a project-repo ADR for
  this control loop, now on its third root-cause pass.

## Local Review (Pre-Push)
**Status**: complete
**When**: 2026-08-25 23:41 -04:00
**By**: Claude Code Agent (Claude Opus)
**Verdict**: changes-requested

**Branch**: feature/issue-52 at `5563726`
**Mode**: pre-push
**Depth**: Deep (reason: ~1900 lines of non-plan change across a concurrency-critical send path and a live control loop; two prior rounds of concurrency bugs in this same code)
**Must-fix**: 4 | **Suggestions**: 14
**Round**: 1 | **Ship**: continue — two precise correctness bugs (an unguarded `+Inf` refractory period and a `0.0`-sentinel that silently disables the gate at clock zero) plus two operator-facing doc claims that are now false; each is a small fix, but four is above the ship-now bar at round 1.

Specialists: Static Analysis (pre-commit clean; cpplint/cppcheck deltas are pre-existing repo style), Governance, Plan Drift, Claude Adversarial x2 (Lens A + Lens B). Copilot and local-model passes off by default.

Verified rather than trusted: full suite re-run on a clean tree — **271 tests, 0 errors, 0 failures, 14 skipped** (matches the claim). Gate values confirmed. `SustainedRealLossMustConverge` was independently shown to fail on a deafened controller (base refractory 5.0 -> 30.0 s: converges at 92.8 s against its 40 s bound) while both original gate tests stay green — the claimed two-sided guard holds for the base period. `ConcurrentMessagesAreAllOrNothing` was shown to fail against a hand-rolled pre-fix emulation (check-only aggregate, per-fragment reserve) while the other five atomicity cases stayed green — it is the one real discriminator, exactly as the test file itself states, and it passed 20/20 repeat runs against the fix.

### Findings
- [x] (must-fix) `setAdmissionRefractoryPeriodSeconds` guards NaN and negatives but accepts `+Inf` (and any unbounded finite value) — one congested sample then freezes the controller for the life of the node, with no decrease, no recovery and nothing logged; it is the only admission setter with no upper clamp — `udp_bridge/src/connection.cpp:155`
- [x] (must-fix) `last_admission_decrease_time_` overloads `0.0` as both "no decrease outstanding" and a legal clock value; under `use_sim_time` before the first `/clock`, or any ROS-time clock at t=0, the refractory gate is silently inert and every congested sample halves again — the exact pre-#52 cascade this branch exists to stop (cross-confirmed by both adversarial lenses) — `udp_bridge/src/connection.cpp:316,336,342`
- [x] (must-fix) `config/example_params.yaml` still tells the operator the now-inert `link_headroom_fraction` is "what keeps a degraded link usable by others"; the two public accessor docs in `connection.h` likewise still describe the removed clamp as live — `udp_bridge/config/example_params.yaml:111-115`, `udp_bridge/include/udp_bridge/connection.h:59-64,89-95`
- [x] (must-fix) the new knob's configure-time-only limitation — the property RCA item D names as what blocked live mitigation on 2026-08-25 — is recorded in `.agents/README.md`, the design doc and the code, but not on either operator-facing surface — `udp_bridge/README.md:212`, `udp_bridge/config/example_params.yaml:117-127`
- [x] (suggestion) neither `kAdmissionRefractoryGrowthFactor` nor `kAdmissionRefractoryMaximumMultiple` is pinned by any test in either direction: `RefractoryGrowsWithinEpisodeAndResetsOnCleanSample` derives its expectation from the symbols, and the field-replay suite stays fully green with the multiple set to 1.0 (no growth) *and* to 1000 (a 5000 s ceiling) — verified empirically, so the file comment claiming this test bounds the cap is false — `udp_bridge/test/test_admission_field_replay.cpp:311-313`, `udp_bridge/test/test_admission_control.cpp:822-824,843`
- [x] (suggestion) the aggregate reservation is held across the whole fragment loop (up to ~200 ms per fragment under kernel back-pressure) and `reserved_bytes_in_flight_` has no time dimension, while overhead traffic shares the same budget through the same overload — a slow or oversized data message can black out BridgeInfo, which sets `feedback_stale`, which forces a decrease; the escape (all-dropped reads `sent_bps == 0` -> clean) is accidental rather than designed and the design doc does not bound it — `udp_bridge/src/connection.cpp:584,600,634-648`
- [x] (suggestion) `FieldOnsetMustNotCascadeToFloor` uses `send_at_rate`, which under-measures the detector's send rate ~24%, so the "single marginal sample that started the cascade" reads clean and the first decrease lands two rows later; the test still discriminates but not by the mechanism its name, comment and failure message describe — use `send_over_interval` as `SustainedRealLossMustConverge` does — `udp_bridge/test/test_admission_field_replay.cpp:159-165,204`
- [x] (suggestion) `HandoverBlipMustNotCostTwoMinutesOfVideo`'s headline metric cannot move until the cap has halved twice, so it is blind to the first two decreases — `udp_bridge/test/test_admission_field_replay.cpp:264-265`
- [x] (suggestion) file header says "Two tests" and names `SteadyLightLossKeepsTheLinkUp`, which does not exist; there are three and the second is `HandoverBlipMustNotCostTwoMinutesOfVideo` — `udp_bridge/test/test_admission_field_replay.cpp:17-27`
- [x] (suggestion) the growth condition does not implement the "no clean sample seen since the last decrease" rule its constant documents: the gate returns before the clean branch, so clean samples arriving inside a window never resolve the episode and the window grows anyway — either resolve on a frozen clean sample or correct the constant's text — `udp_bridge/include/udp_bridge/resend_constants.h:193-196`, `udp_bridge/src/connection.cpp:316-321,336`
- [x] (suggestion) `conn->send()` is unguarded inside the worker lambda, so a `ConnectionException` (reachable on `ENOBUFS` with 16 threads against an undrained loopback socket) calls `std::terminate` instead of failing the test; the sibling churn test already wraps its call — `udp_bridge/test/test_message_atomicity.cpp:337`
- [x] (suggestion) AIMD is silently inert on any connection whose `maximum_bytes_per_second` is at or below the 8192 floor (the floor clamps to the limit, so the decrease is a no-op forever) with nothing warning — `udp_bridge/src/connection.cpp:326-327`
- [x] (suggestion) four sites say the inert parameter is "still reported"; it is declared and clamped, but appears in no message and has no non-test reader — say "readable via `ros2 param get`" or point at where it is reported — `udp_bridge/README.md:213`, `.agents/README.md:109`, `udp_bridge/include/udp_bridge/resend_constants.h:254`, `udp_bridge/include/udp_bridge/connection.h:337`
- [x] (suggestion) `currentAdmissionRefractoryWindowSeconds()` is documented "for tests and diagnostics" but has no diagnostic consumer; a freeze is not distinguishable from a healthy flat cap in `RemoteConnection.msg` — either drop the claim or surface the in-force window while the schema is already being reasoned about — `udp_bridge/include/udp_bridge/connection.h:82-87`
- [x] (suggestion) the bench `xfail` reason string still records the previous cycle's residuals (lossy 0.077); this branch measured 0.141 — `udp_bridge/test/bench/test_range_degradation.py:679-693`
- [x] (suggestion) the `lossy` resend/msg regression (0.077 -> 0.141) is reported as possible harness variance, but the demonstrated variance (co-tenant delivery 0.904 vs 0.840, ~7%) is an order of magnitude smaller than an 83% move; it is more likely a real consequence of holding the cap higher on a lossy link — record the mechanism or run n>=3 before attributing it to noise. Not a blocker: the strict `xfail` gate is unchanged — `.agent/work-plans/issue-52/progress.md` Implementation entry
- [x] (suggestion) plan `Files to Change` never gained `statistics.h` / `statistics.cpp` (the deviation is explained twice elsewhere), and four settled Open Questions still read as open; the growth-constants-configurability question has no recorded resolution — `.agent/work-plans/issue-52/plan.md`
- [x] (suggestion) new parameter declared with no `ParameterDescriptor` (`description`, `read_only: true` would make the configure-time contract machine-visible) — reasonable to carry into #76 rather than fix here — `udp_bridge/src/udp_bridge.cpp:622-624` (deferred: carried to #76. `read_only: true` is the load-bearing half and it is a behaviour change — it belongs with the work that decides which of these parameters STOP being configure-time-only; adding a bare `description` to 4 of ~30 parameters that have none is inconsistency without the machine-visible contract.)
- [x] (suggestion) the design doc's "Decrease" bullet omits the `feedback_unusable` triggers (non-finite, negative, duplicate > received) that the code also treats as congestion — `udp_bridge/doc/admission_control_design.md:50-57`

### Notes for the next reviewer
- Two findings raised by specialists were **artifacts of this review's own experiments** and are NOT branch defects: a working-tree edit to `kAdmissionRefractoryMaximumMultiple` (my deafening experiment) was observed by two specialists mid-run. The committed constants are 5.0 / 2.0 / 2.0 and the tree is clean at `5563726`.
- Plan drift: none. Every Approach item carries an inline resolution note and an explicit deviations list matching the `## Implementation` entry. Priority classes (#19) correctly out of scope.
- Governance: the clamp-removal decision is coherent and unusually well recorded; the gaps are placement, not reasoning. ADR-0018 local-CI attestation is not yet on HEAD (a merge-time gate, not a pre-push one). Commit hygiene, identity and the test-first commit (`4002cad`) all clean.

## Implementation
**Status**: complete
**When**: 2026-08-26 00:16 -04:00
**By**: Claude Code Agent (Claude Opus)

**Branch**: `feature/issue-52` at `a06f0af` (12 commits on top of the review, unpushed)
**Addressed**: the `## Local Review (Pre-Push)` of 2026-08-25 23:41 -04:00, branch at `5563726` (verdict changes-requested, round 1) — 4 must-fix + 15 suggestions, all 19 consciously handled
**Commits**: `f9245d6` `a09b117` `e7babf4` `b91f820` `ad4764e` `516037c` `21545f2` `5765efc` `48ad5a7` `37b8ca1` `3713c9c` `a06f0af`

### Actions

Must-fix:

- [x] `setAdmissionRefractoryPeriodSeconds` accepted `+Inf` — `udp_bridge/src/connection.cpp:155`.
  New `kMaximumAdmissionRefractoryPeriodSeconds` (60 s); the setter clamps down to it
  rather than falling back to the default, so an operator asking for a slower loop still
  gets the slowest one that leaves the loop alive. The contract test now covers `+Inf`
  and a large finite value, and pins that the clamped window really expires. `f9245d6`
- [x] `0.0` sentinel silently disabled the gate at clock zero — `udp_bridge/src/connection.cpp:316,336,342`.
  Split into its own `admission_decrease_outstanding_` flag; the timestamp is now only
  read while that flag is set. `a09b117`
- [x] Operator-facing docs described the removed clamp as live — `config/example_params.yaml`,
  `include/udp_bridge/connection.h`. Rewritten to lead with CURRENTLY INERT and to say
  that setting the parameter changes nothing. The "still reported" overclaim corrected at
  all four sites to "declared, clamped and readable via `ros2 param get`, appears in no
  message". `e7babf4`
- [x] Configure-time-only limitation missing from the two operator-facing surfaces —
  `README.md`, `config/example_params.yaml`. Both now state it, name `ros2 param set` as
  the thing that will NOT work, and cite RCA item D and #76. `b91f820`

Suggestions (pre-push review actions all unchecked findings, per the skill):

- [x] Growth constants unpinned, and the test file's claim to bound them was false —
  `test_admission_field_replay.cpp`, `test_admission_control.cpp`. New
  `AdmissionControl.RefractoryWindowGrowthIsBounded` asserts the two properties the
  constants exist to provide against absolute values from their own rationale (the window
  must grow; the grown window must stay inside the statistics deque's 10 s retention).
  The false claim is corrected and points at the test that does the job. `ad4764e` `516037c`
- [x] The aggregate reservation has no time dimension and shares its budget with overhead —
  `src/connection.cpp:584,600,634-648`. Documented at the call site and in
  `doc/admission_control_design.md`: the escape is real but falls out of the idle-link
  rule rather than being designed, nothing bounds the hold time except the send-poll
  budget, and the fix it would call for is a separate overhead ledger — explicitly NOT a
  longer refractory period. Not fixed in code: a second ledger is a design change with
  its own concurrency argument, and no field observation motivates it yet. `37b8ca1`
- [x] `FieldOnsetMustNotCascadeToFloor` under-measured the send rate ~24% — switched to
  `send_over_interval`. Gate value unchanged at 375000. `516037c`
- [x] `HandoverBlip...` was blind to the first two decreases — now counts decreases
  directly and asserts the ~4 s blip costs at most one. Measured: 1. `516037c`
- [x] File header said "Two tests" and named one that does not exist — corrected to three
  with their real names. `516037c`
- [x] The growth condition does not implement the rule its constant documented — corrected
  the constant's text, and gave the reason the code is right: a sample taken inside the
  window describes traffic sent at the OLD cap, so it is no better as evidence the episode
  ended than as grounds to recover. `21545f2`
- [x] `conn->send()` unguarded inside the worker lambda — wrapped, with a throw counter and
  an assertion that none threw, so a round that did not exercise the contention says so
  instead of calling `std::terminate`. `5765efc`
- [x] AIMD silently inert when the floor is at or above the connection's cap — `on_configure`
  now WARNs naming the connection, and the property is recorded in the README, the design
  doc and `.agents/README.md`. `48ad5a7`
- [x] "Still reported" overclaim at four sites — corrected. `e7babf4`
- [x] `currentAdmissionRefractoryWindowSeconds()` claimed a diagnostic consumer it does not
  have — claim dropped; #76 named as where surfacing it belongs. `21545f2`
- [x] The bench `xfail` reason string recorded the previous cycle's residuals — rewritten
  with this round's n=3 numbers. `a06f0af`
- [x] The `lossy` resend/msg regression — **ran n=3 rather than concluding.** See below. `a06f0af`
- [x] Plan `Files to Change` missing `statistics.h`/`.cpp`, four Open Questions unresolved —
  all synced, including the growth-constants-configurability question that had no recorded
  resolution. `3713c9c`
- [x] New parameter has no `ParameterDescriptor` — `udp_bridge/src/udp_bridge.cpp:622-624`
  (deferred: carried to #76. `read_only: true` is the load-bearing half and is a behaviour
  change; it belongs with the work that decides which of these parameters stop being
  configure-time-only. Adding a bare `description` to 4 of ~30 parameters that have none
  is inconsistency without the machine-visible contract.)
- [x] Design doc's Decrease bullet omitted the `feedback_unusable` triggers — added
  (non-finite, negative, duplicate > received). `21545f2`

### The `lossy` resend/msg movement: measured, not argued

The review rejected "harness variance" because the demonstrated variance was ~7%. Three
full `range_degradation` runs of one build show that ~7% figure was measured on a
different metric (co-tenant delivery). On resend/msg itself:

| phase | run 1 | run 2 | run 3 | ceiling | spread |
|---|---|---|---|---|---|
| `fringe` | 0.0053 | 0.0061 | 0.0092 | 0.010 | 1.7x |
| `lossy` | 0.0131 | 0.0184 | 0.0705 | 0.060 | 5.4x |
| `critical` | 0.2838 | 0.4735 | 0.1115 | 0.200 | 4.2x |

The 0.077 -> 0.141 move sits well inside a 5.4x spread, so it cannot be attributed to a
mechanism — and by the same token nothing here rules one out. The conclusion worth keeping
is about the measurement: **single-run values from this invariant do not support the three
decimals they were quoted at**, in the earlier Implementation entry or in the `xfail`
reason. Both now say so, with the numbers.

The strict `xfail` held in all three runs — at least one phase violates every time, so no
XPASS risk was observed and `F_resend_multiplier` was not touched. *Which* phase violates
is not stable: `critical` was over the ceiling in two runs of three, while the single run
recorded in the earlier entry had it passing with margin.

### Verification

- **273 tests, 0 errors, 0 failures, 14 skipped** — full suite on a clean tree (was 271;
  `RefractoryGateHoldsAtClockZero` and `RefractoryWindowGrowthIsBounded` are the two new).
- Gate values re-measured on the final tree, unchanged: onset `lowest` **375000**
  (bound > 187500), handover delivered **1.000** (bound > 0.90), final cap **1500000**
  (bound > 750000). Handover decreases: **1**.
- Both new tests were shown to fail against the defect they pin, not merely to pass
  against the fix: `RefractoryGateHoldsAtClockZero` fails against the `> 0.0` sentinel
  form; `RefractoryWindowGrowthIsBounded` fails with `kAdmissionRefractoryMaximumMultiple`
  at 1.0 (no growth) and at 1000 (a 5000 s ceiling) — the two settings the review showed
  the whole suite was previously green at. With the `send_over_interval` fix,
  `FieldOnsetMustNotCascadeToFloor` now also fails at multiple 1.0, which is the growth
  becoming load-bearing for the test that motivated it.
- `pre-commit run --from-ref origin/jazzy --to-ref HEAD`: clean.

### Notes for the re-review

- Reaching clock zero with a send history to be congested about requires a **backwards
  clock step**, because `Statistics::add` drops records stamped exactly 0 — so a
  freshly-started sim clock has no history and cannot be congested. A looping bag replay
  and a sim reset both produce that step, and the regression test drives it. The finding
  is therefore real but was masked by an incidental property of a different class; the
  flag makes it not depend on that.
- `Connection` has no logger, so the `+Inf` finding's "nothing logged" half cannot be
  fixed at that layer. The clamp is the fix; the new WARN for the inert-AIMD case lives in
  `udp_bridge.cpp`, which does have one.
- Three behaviour changes this round (the upper clamp, the outstanding flag, the WARN) are
  recorded in `plan.md` under "Review round 1", with the new constant's bound.

## Local Review (Pre-Push)
**Status**: complete
**When**: 2026-08-26 00:41 -04:00
**By**: Claude Code Agent (Claude Opus)
**Verdict**: changes-requested

**Branch**: feature/issue-52 at `003c766`
**Mode**: pre-push
**Depth**: Deep (reason: ~4200 lines across a live control loop and a concurrency-critical send path; three prior root-cause passes on this same code)
**Must-fix**: 7 | **Suggestions**: 17
**Round**: 2 | **Ship**: continue — round 1's four must-fixes are genuinely closed and verified, but three NEW correctness defects surfaced, two of them in code round 2 itself added (the grown refractory window escaping its own 60 s ceiling; the inert-AIMD WARN reading the raw parameter instead of the applied floor) and one in the new send-rate window. Must-fix went 4 -> 7, which is rising, and they are not all mechanical. The three code fixes are each precisely located and small, so round 3 should be short.

Specialists: Static Analysis (pre-commit clean over `origin/jazzy..HEAD`), Governance, Plan Drift, Claude Adversarial x2 (Lens A + Lens B). Copilot and local-model passes off (default).

### Round-1 fixes: verified, not trusted

- **`+Inf` refractory** — CLOSED at the setter. `kMaximumAdmissionRefractoryPeriodSeconds` (60 s) clamps down rather than defaulting, and `RefractoryPeriodSetterContract` covers `+Inf`, a large finite value, and real expiry. The "nothing logged" half was declared unfixable because `Connection` has no logger — **half right**: `Connection` genuinely has none (`RemoteNode` does, `Connection` does not), but `on_configure` is one call up, has a logger, and received the sibling inert-AIMD WARN in this very round. See must-fix 1.
- **`0.0` sentinel** — CLOSED. `admission_decrease_outstanding_` is written and read only under `config_mutex_`, and `RefractoryGateHoldsAtClockZero` was **shown to fail** against a hand-restored `last_admission_decrease_time_ > 0.0` gate (cap 25000 vs 50000) while `RefractoryGrows...` and `RefractoryPeriodSetterContract` stayed green. The fixer's refinement is correct: `Statistics::add` (`statistics.h:57`) drops records stamped exactly 0, so reaching clock zero with a send history needs a backwards clock step. But see must-fix 3 — the test's setup only reaches "congested at t=0" **because** `success_rate_in_window` has no upper time bound, so the fix and the test move together.
- **`RefractoryWindowGrowthIsBounded`** — CLAIM TRUE. Rebuilt at `kAdmissionRefractoryMaximumMultiple = 1.0`: fails ("never grew, 5 s at the base 5 s"), and `FieldOnsetMustNotCascadeToFloor` fails too (the growth is now load-bearing for the test that motivated it). At 1000: fails. Tree restored and rebuilt; `git status` clean.
- **Inert-AIMD WARN** — correct in the common case (unset `maximum_bytes_per_second` gives `data_rate_limit_ = 50000` > the 8192 default floor, so no spurious fire) but wrong in the corrected-value case — must-fix 2.
- **Docs** — the configure-time-only limitation IS now on both operator surfaces (`README.md:211-212`, `example_params.yaml:143-156`), and the removed clamp is gone from every `.md`/`.yaml`/`.h`/`.cpp` **except** four sites — must-fix 5.
- **Suite** — re-run on a clean tree: **273 tests, 0 errors, 0 failures, 14 skipped**. Gates re-measured, all as claimed: onset `lowest` 375000 (> 187500), handover delivered 1.000 (> 0.90), final cap 1500000 (> 750000), handover decreases 1.

### Findings
- [x] (must-fix) the grown refractory window is capped at `base x kAdmissionRefractoryMaximumMultiple` and never re-clamped to the 60 s ceiling, so a connection configured at the maximum base freezes for **120 s** on the second decrease — twice the bound whose own rationale says above it the controller cannot answer a link change in any operator-observable timescale, and 12x the 10 s statistics retention the design doc's constants table gives as the cap's upper bound. `RefractoryPeriodSetterContract` encodes the 120 s. Also: the clamp is silent and appears on no operator surface (`README.md`, `example_params.yaml`, `.agents/README.md` all omit it; only the design doc has it) — `udp_bridge/src/connection.cpp:353-356`, `udp_bridge/include/udp_bridge/resend_constants.h:210`
- [x] (must-fix, cross-confirmed by both adversarial lenses) the new inert-AIMD WARN tests the raw `admission_floor_bps` parameter, not the value the connection stored; `setAdmissionFloorBytesPerSecond` maps NaN and negatives to 8192, so `admission_floor_bytes_per_second: -1` on a connection capped below 8192 is permanently inert with no warning — the one case the WARN exists for. Read `live_connection->admissionFloorBytesPerSecond()` back instead — `udp_bridge/src/udp_bridge.cpp:666-667`
- [x] (must-fix) `success_rate_in_window` bounds the scan below but not above, and `dt` floors at 1.0 s, so after a backwards clock step up to 10 s of future-stamped successes are summed over dt = 1.0 and `sent_bps` inflates ~10x, reading every following sample as congested on a link that never degraded. `RefractoryGateHoldsAtClockZero` currently DEPENDS on this (80 kB stamped at t=2 reports 80000 B/s at t=0 — which is what makes its setup congested), so the guard and that test have to be reworked together — `udp_bridge/src/statistics.cpp:180-206`
- [x] (must-fix) two comment blocks state that no caller in the workspace catches `ConnectionException`, so a throw terminates the node and the reservation guard is only cosmetic "before the process dies". False: `callIsolated` (`include/udp_bridge/send_isolation.h:38`) catches it explicitly and wraps the batch send at `udp_bridge.cpp:2171`. The Timeout throw is a recurrent survivable path, so a leak would accumulate and permanently wedge the connection — the guard is load-bearing, and the comment invites relaxing it — `udp_bridge/src/connection.cpp:762-771`, `udp_bridge/test/test_message_atomicity.cpp:405-407`
- [x] (must-fix, cross-confirmed by Governance) four survivors of the clamp-removal doc sweep still describe the removed mechanism as live: `test/bench/cotenant.py:11-14` ("the property that actually protects the operator is headroom against measured throughput") and `:41-43` ("inside the 20% the bridge is supposed to leave") — the file a future agent opens when the co-tenant invariant fails, and the sibling of `test/bench/README.md:116` which WAS corrected; plus `test/test_admission_control.cpp:253` ("still reported") and `:298` ("target == goodput"); plus `doc/admission_control_design.md:279` ("still declared, clamped and reported")
- [x] (must-fix) `doc/admission_control_design.md:279` justifies retaining the inert parameter with "existing configs set it, and removing it would break them" — false by this repo's own verified behaviour: `udp_bridge.cpp:363-368` records that rclcpp surfaces a YAML override only for a DECLARED parameter (this node does not set `automatically_declare_parameters_from_overrides`), so an override for an undeclared parameter is silently ignored, not an error. This is the stated basis for the retain-vs-delete decision going to the operator, so it must be accurate before that decision is put
- [x] (must-fix, Governance) the new parameter is read with `as_double()` inside `on_configure`, which has no try/catch anywhere in its 82-789 span; a `5` literal instead of `5.0` throws `ParameterTypeException` out of a lifecycle transition callback, where rclcpp_lifecycle swallows it and leaves a node that silently failed to configure. The trap is documented three times (`README.md:212`, `example_params.yaml:139-141`, `.agents/README.md:109`) and guarded zero — and this is its third instance in this node. Coerce at the read site (accept `PARAMETER_INTEGER`) or attach a descriptor — `udp_bridge/src/udp_bridge.cpp:625`
- [x] (suggestion, Lens A) `HandoverBlipMustNotCostTwoMinutesOfVideo` still uses `send_at_rate`, which reads ~24% low over the detector's 5 s window — the same distortion `FieldOnsetMustNotCascadeToFloor` was fixed for this round. Here the bias is toward FEWER decreases, weakening `EXPECT_LE(decreases, 1)`, the assertion that actually pins the gate — `udp_bridge/test/test_admission_field_replay.cpp:279`
- [x] (suggestion, Lens A) the whole premise of the detector fix is that the two windows match, yet `data_receive_rate`'s is a bare literal (`time - 5`) with no reference to `kAdmissionSendRateWindowSeconds` and no assertion tying them; the file already sets the precedent (`static_assert(kReceiveHistoryWindow <= kSentPacketTTL, ...)`) — `udp_bridge/src/connection.cpp:879`
- [x] (suggestion, both lenses) the reservation-lifetime note's causal chain is wrong: dropping our OWN outbound BridgeInfo does not set our `feedback_stale` (that is inbound-driven), and the large message's fragments succeed so `sent_bps > 0` — the harmed loops are the remote's controller and our own resend requests. Same risk class, wrong mechanism, in both the code comment and the design doc — `udp_bridge/src/connection.cpp:602-610`, `udp_bridge/doc/admission_control_design.md:105-123`
- [x] (suggestion, both lenses) `EXPECT_EQ(send_throws, 0)` turns an environmental condition (poll-budget exhaustion under CI load) into a red test whose message says it is "not itself the property under test", and a throwing round then falls through to `ASSERT_EQ(sent % message_size, 0u)`, misattributing it to the reservation logic. `GTEST_SKIP()` the round instead — `udp_bridge/test/test_message_atomicity.cpp:362-366`
- [x] (suggestion, Lens B) the churn thread calls `setHostAndPort` in a tight loop; `resolveHost` throws `std::runtime_error` on `getaddrinfo` failure, and an exception escaping a `std::thread` calls `std::terminate` — the exact hazard the sibling test was wrapped for this round — `udp_bridge/test/test_message_atomicity.cpp:442-449`
- [x] (suggestion, Lens B) the inert-AIMD condition is more reachable at RUNTIME than at configure time and that route is unwarned: `setRateLimit` is called from the `CONNECT` decode with a peer-advertised cap and from `add_remote`, neither re-checking the floor, so a peer advertising a cap below 8192 silently switches adaptive admission off for the life of the node — `udp_bridge/src/connection.cpp:57`, `udp_bridge/src/udp_bridge.cpp:1957`
- [x] (suggestion, Lens B) a message refused at the aggregate gate consumes its packet numbers (`udp_bridge.cpp:2112`) but stores no fragment in `sent_packets_`, so the receiver requests resends that can never be served and `resend_giveup_count` — which an operator reads as "the link lost packets" — now climbs on every deliberate admission drop; the old per-packet shed left fragments recoverable. The atomic drop is right; the diagnostic conflation needs saying where an operator reads — `udp_bridge/src/connection.cpp:623-642`
- [x] (suggestion, Lens B) the gate reduces the RATE of decisions but each is still made on one sample, and the ~4 samples suppressed inside a grown window are discarded rather than folded in — so at the floor, where the ratio is mostly noise, recovery depends on which sample happens to land first after expiry. Name it as a known limit of this design — `udp_bridge/src/connection.cpp:314-338`
- [x] (suggestion, Governance, verified with `gh`) five references call **#76** the tracking issue; #76 is a PR ("Field import: runtime connection tunables ... UNCOMPILED") and the issue is **#75**. The `/issues/76` URL only resolves by redirect, and once #76 merges an operator following README lands on merged work while #75 stays open — `udp_bridge/README.md:212`, `config/example_params.yaml:155`, `src/udp_bridge.cpp:646`, `doc/admission_control_design.md:244`, `.agents/README.md:109`. The `ParameterDescriptor` deferral should also name #75
- [x] (suggestion, Governance) message-level drop granularity is an operator-visible behaviour change (bytes shift from `success` to `dropped`; partial messages stop appearing) with no README note, though the repo has exactly that pattern at `README.md:84` ("Behavior change (#58)") — `udp_bridge/README.md`
- [x] (suggestion, Governance) the `link_headroom_fraction` follow-up is the only one in the change with no tracked number, while its neighbours all carry #19 / #61 / #75 / #45 — `udp_bridge/doc/admission_control_design.md:286-291`
- [x] (suggestion) `BatchReservationGuard::unreserved` is a `uint32_t` decremented by a quantity whose equality with the reserved total is an invariant the comment argues for rather than checks; if it ever breaks, the underflow corrupts `reserved_bytes_in_flight_` and wedges the connection permanently. A clamp or assert is cheap — `udp_bridge/src/connection.cpp:685`
- [x] (suggestion) `success_rate_in_window`'s doc claims the same `dt = max(1.0, time - oldest sample in window)` divisor as `data_receive_rate`, but it measures from the oldest SUCCESSFUL sample; when successes cluster late in the window (the throttled regime) this over-reports `sent_bps`, biasing toward false congestion — `udp_bridge/include/udp_bridge/statistics.h:100-112`, `udp_bridge/src/statistics.cpp:189-196`
- [x] (suggestion) `earliest.nanoseconds() == 0` in `success_rate_in_window` is the same 0-as-sentinel pattern round 1 removed from the gate, and is safe only because `Statistics::add` drops zero-stamped records — the incidental property round 2 deliberately stopped depending on elsewhere — `udp_bridge/src/statistics.cpp:191`
- [x] (suggestion, Plan Drift) `connection.cpp:327-331` states the clock-zero reachability more loosely than is true ("under `use_sim_time` before the first `/clock`" alone would not reach it); `plan.md:788-793` and the design doc both get it right — tighten the comment to match
- [x] (suggestion, Plan Drift) plan hygiene: `plan.md:939` points at a constants table the Implementation Notes do not contain (the real one is `doc/admission_control_design.md:227-235`); `:536-547` still reads verification criterion 1 as a hard gate that the recorded xfail non-pass would block, with no clause recording why it was accepted; `:92-99` A1 prose still describes the sentinel design round 1 removed; `:841` points "below" at a section above it; bench line refs at `:538` and `:552` drifted by this branch's own edits
- [x] (suggestion) the design doc's constants table bounds `kAdmissionRefractoryMaximumMultiple` by the statistics deque's 10 s retention and the bench's 10 s phase holds, but a configured base up to 60 s makes the grown window 120 s — the two rationales need reconciling (see must-fix 1) — `udp_bridge/doc/admission_control_design.md:230`

### The two judgement calls I was asked to weigh

**The bench `lossy` reversal is SOUND, with one overstatement.** Round 1's variance figure was measured on co-tenant delivery, not on resend/msg, and transferring it was an unjustified move — round 2 is right to reject it and right to run n=3 on the metric itself rather than argue. The operative conclusions all hold: the strict xfail is unbroken in all three runs (run 1 and 2 fail via `critical`, run 3 via `lossy` — I checked the arithmetic against the recorded table), `F_resend_multiplier` was untouched, and the durable finding — single-run values from this invariant do not support three decimals — is the right thing to have kept. The overstatement: neither 0.077 nor 0.141 appears inside the n=3 range [0.0131, 0.0705], so "the move sits well inside a 5.4x spread" conflates the magnitude of the spread with containment in it. What the data actually shows is that three tries reproduced neither endpoint, which supports the conclusion more strongly than the sentence does. Reword; do not re-litigate.

**Deferring the `ParameterDescriptor` to #76 is RIGHT for the load-bearing half, thin for the other.** `read_only: true` genuinely changes behaviour — a `ros2 param set` that currently succeeds-and-does-nothing would start failing — and that decision belongs with the work deciding which parameters stop being configure-time-only. But a bare `description` is not a behaviour change and costs nothing, and "inconsistent with 26 other undescriptored parameters" is an argument for describing more of them, not fewer. Accept the deferral; ask #75 to cover descriptors for ALL admission parameters so it is not lost. Note that the deferral, like five other references, names #76 (a PR) rather than #75 (the issue).

### On the `link_headroom_fraction` retain-vs-delete decision (NOT decided here)

Recording a technical view for the operator, as asked. **There is a third option, and this repo already uses it.** For the retired `remotes.<label>.name` the node keeps the parameter DECLARED specifically so a stale key is visible — "rclcpp surfaces a YAML override only for a declared parameter" (`udp_bridge.cpp:363-368`) — and then FAILS `on_configure` on a non-empty value, with the reasoning that a value "announced and then ignored" is the shape that caused the #51 echo bug. That is the same shape `link_headroom_fraction` is in now, and the same shape that produced a false conclusion during the 2026-08-25 incident.

Two consequences for the choice:
1. The stated justification for retaining ("removing it would break existing configs") is **factually wrong** — see must-fix 6. Undeclaring it would make existing configs silently ignored, not broken. That is arguably worse, and it is exactly why the repo declares its retired parameters.
2. So the safest option is neither "retain silent" nor "delete": keep it declared and **WARN once at `on_configure` when it is set to a non-default value**, saying it does nothing. That preserves config loading, makes the inertness impossible to miss, matches the repo's own precedent, and satisfies the workspace's remove-obsolete-rather-than-leave-opt-in instinct without a wire-visible change. Both adversarial lenses arrived at the same recommendation independently.

### Notes for the next reviewer
- Three of the seven must-fixes (1, 2, 3) are genuine correctness defects in code; four (4, 5, 6, and the doc half of 1) are false statements in the repo about the repo. Two of the three code defects were INTRODUCED by round 2's own fixes, which is why the count rose rather than fell — the loop is not stuck on the same issues.
- Experiments I ran that touched the tree, all reverted and rebuilt, `git status` clean at `003c766`: the sentinel-gate restoration, and `kAdmissionRefractoryMaximumMultiple` at 1.0 and 1000. If a later specialist reports seeing modified constants, that is this review, not the branch.
- Concurrency and the reservation ledger were the hardest-pushed area and neither lens could break them: releases are exact on all four exit paths, ownership transfers before the call so a throw releases exactly once, `p.packet.size() == p.packet_size` genuinely holds through `WrappedPacket`'s copy constructor (verified in `wrapped_packet.cpp:20-43`), and no mutex is ever nested. A suspected `rclcpp::Time` clock-type throw and a negative-window throw in `success_rate_in_window` were both chased down and discarded as non-issues.
- ADR-0018: `git notes --ref=ci-local` is empty at HEAD. Expected pre-push; it is the merge gate, not a finding.
- Governance flagged a possible carry-forward in the design doc's claim that the co-tenant invariant "passes with the clamp removed". It is backed: the range-degradation bench WAS runnable this cycle (10 passed / 2 xfailed at implementation, plus three full runs at `a06f0af`), unlike earlier cycles where `unshare -Urn` was denied.
- Pre-existing, not introduced here: `udp_bridge/.agent/work-plans/issue-52/plan.md` is a tracked stale duplicate plan for THIS issue number at a nested path, describing the superseded cycle (it still presents the headroom clamp as the intended fix). It is on `origin/jazzy` from `484fd74`. Worth deleting while #52 is the live cycle, so a future agent grepping for issue-52's plan does not find two that disagree.

## Implementation
**Status**: complete
**When**: 2026-08-26 01:15 -04:00
**By**: Claude Code Agent (Claude Opus)

**Branch**: feature/issue-52 at `a5c6895`
**Addressed**: `## Local Review (Pre-Push)`, 2026-08-26 00:41 -04:00, round 2, verdict changes-requested, at `003c766` (7 must-fix, 17 suggestions)
**Commits**: `99eb64c` `c8eca72` `33a10f1` `d5ff665` `1e864c5` `58c3f72` `1fd3831` `8f3f74a` `51e121d` `4f807c0` `9a7079c` `ffa787d` `3f82135` `9660db4` `d43e497` `ab4da08` `b162ee1` `90a16cf` `e73feb1` `c0636d9` `a5c6895`

**Suite**: 273 tests, 0 errors, 0 failures, 14 skipped (unchanged — two headroom unit tests deleted, two node-level tests added).
**Gates**, re-measured after every change: onset `lowest` **375000** (bound > 187500), handover delivered **1.000** (bound > 0.90), final cap **1500000** (bound > 750000), handover decreases **1**. All four unchanged.
**Lint**: `pre-commit run --from-ref origin/jazzy --to-ref HEAD` clean.

### The operator's `link_headroom_fraction` decision, as implemented

Superseding the earlier "delete it outright": the parameter is **RETIRED
to a declared tripwire**. All behaviour removed —
`Connection::setLinkHeadroomFraction` / `linkHeadroomFraction` and the
`link_headroom_fraction_` member are gone, and the two unit tests pinning
that API went with them. The parameter stays **declared** and
`on_configure` **WARNs**, naming the connection, on any non-default value.
It does **not** fail the transition: a stale `remotes.<label>.name`
misroutes traffic (the #51 echo bug) and so must fail; a stale headroom
value misroutes nothing, and refusing to configure would ground a boat
over a dead config key. The design doc's false justification ("removing it
would break existing configs") is replaced with the real one — an override
for an *undeclared* parameter is silently ignored, not an error, and that
silence is the hazard.

### Verification, not assertion

Three guards were checked by breaking them and rebuilding, then restored
(tree clean, suite re-run green):

- restoring `std::min(current * growth, base * multiple)` without the
  ceiling → `RefractoryPeriodSetterContract` **fails**;
- restoring `last_admission_decrease_time_ > 0.0` as the gate →
  the *reworked* `RefractoryGateHoldsAtClockZero` **still fails**, so the
  new setup pins the same property the old one did;
- restoring bare `as_double()` →
  `RetiredParameters.IntegerLiteralForADoubleParameterConfigures`
  **fails**.

### Actions

Must-fix:
- [x] grown refractory window never re-clamped to the 60 s ceiling (120 s freeze at a max-configured base); clamp silent and on no operator surface — `99eb64c`. Third bound added at the growth site; `RefractoryPeriodSetterContract` now asserts the grown window against the ceiling; both bounds documented in `README.md`, `config/example_params.yaml`, `.agents/README.md` and the design doc's constants table (whose multiple rationale holds only at the *default* base — now said) — `src/connection.cpp:353`, `doc/admission_control_design.md`
- [x] inert-AIMD WARN read the raw parameter, not the applied floor — `c8eca72`. Reads `live_connection->admissionFloorBytesPerSecond()` back and reports both figures — `src/udp_bridge.cpp:698`
- [x] `success_rate_in_window` unbounded above; `RefractoryGateHoldsAtClockZero` depended on it — `33a10f1`. Window is now `(time - w, time]`; the test stamps its traffic before the clock origin instead, which is what makes a decrease recorded at exactly 0.0 constructible now that future samples are correctly ignored — `src/statistics.cpp:180`, `test/test_admission_control.cpp:901`
- [x] two comments claimed nothing catches `ConnectionException` and the guard was cosmetic — `d5ff665`. `callIsolated` catches it explicitly; the Timeout throw is survivable and recurrent, so a leak would accumulate and permanently wedge the connection. Guard is load-bearing — `src/connection.cpp:762`, `test/test_message_atomicity.cpp:405`
- [x] four survivors describing the removed clamp as live — `8f3f74a` (`test/bench/cotenant.py` ×2) and `1fd3831` (`test/test_admission_control.cpp:253`/`:298` deleted with the tests they annotated; `doc/admission_control_design.md:279` rewritten)
- [x] design doc justified retaining the parameter with a false claim — `1fd3831`. Replaced with the verified behaviour and the real rationale (visibility of a stale key); the tripwire is described on all five surfaces
- [x] `as_double()` in `on_configure` with no try/catch, third instance of a trap documented three times and guarded zero — `58c3f72`. `getDoubleParameter` coerces `PARAMETER_INTEGER` at every double read site in `on_configure`; pinned by a new node-level test — `include/udp_bridge/udp_bridge.h:311`

Suggestions:
- [x] `HandoverBlipMustNotCostTwoMinutesOfVideo` still used `send_at_rate` — `4f807c0`. Switched to `send_over_interval`; gate values unchanged
- [x] `data_receive_rate`'s window was a bare `time - 5` literal — `9a7079c`. `kReceiveRateWindowSeconds` + a `static_assert` pinning it equal to `kAdmissionSendRateWindowSeconds`
- [x] reservation-lifetime note's causal chain wrong — `3f82135`. It does not set our own `feedback_stale` (inbound-driven) and the message is not idle (`sent_bps > 0`); what is starved is the remote's controller and our own resend re-requests. Corrected in the code comment and the design doc
- [x] `EXPECT_EQ(send_throws, 0)` made an environmental condition red, then fell through to an assertion it would misattribute — `d43e497`. `GTEST_SKIP()` the round
- [x] churn thread's `setHostAndPort` could throw out of a `std::thread` → `std::terminate` — `d43e497`. Wrapped and counted; the round skips if the interleaving could not be produced
- [x] the runtime route to inert AIMD (`CONNECT` decode, `add_remote`) was unwarned — `1e864c5`. Throttled WARN at all three runtime `setRateLimit` sites
- [x] gate-refused messages consume packet numbers, so `resend_giveup_count` now climbs on deliberate drops — `ab4da08`. Stated at the call site and, with the `success`→`dropped` byte shift, in a README "Behavior change (#52)" note following the #58 precedent
- [x] the gate reduces decision RATE, not evidence quality; suppressed samples are discarded — `b162ee1`. Named as a known limit at the gate and in a new design-doc subsection, with the aggregate/matched-counter fixes and why neither is in this change
- [x] five references called #76 the tracking issue — `51e121d`. Corrected to #75 in six places (a sixth was in `connection.h`), plus `plan.md`; the `ParameterDescriptor` deferral now names #75 and is widened to cover descriptors for *all* admission parameters
- [x] message-level drop granularity had no README note — `ab4da08`
- [x] the `link_headroom_fraction` follow-up had no tracked number — `1fd3831`. Now #61 (whether rate headroom can protect a co-tenant across a downshift is already that issue's open question). **Not #19** — that is Direction B, per-topic priority scheduling
- [x] `BatchReservationGuard::unreserved` bare-decremented a `uint32_t` — `9660db4`. Clamped; an underflow there would wedge the connection permanently
- [x] `success_rate_in_window`'s doc claimed a divisor it does not use — `33a10f1`. Documents the real divergence (measured from the oldest *successful* sample) and why that side is accepted: the alternative makes the divisor depend on failures the remote never saw, and the residual skew is bounded and conservative
- [x] `earliest.nanoseconds() == 0` was the same 0-as-sentinel pattern round 1 removed — `33a10f1`. Explicit `have_earliest` flag
- [x] `connection.cpp:327-331` stated the clock-zero reachability too loosely — `ffa787d`. Tightened to match `plan.md` and the design doc: a backwards clock step, not `use_sim_time` alone
- [x] plan hygiene (constants-table pointer, criterion-1 acceptance clause, A1's stale sentinel prose, "below" pointing above, drifted bench line refs) — `e73feb1`. All five fixed, plus a "Review round 2 — behaviour changes" section and the retirement decision
- [x] the design doc's constants table bounded the multiple against rationales that hold only at the default base — `99eb64c`. Reconciled with the absolute ceiling

Judgement calls:
- [x] bench `lossy` reversal overstated — `90a16cf`. Reworded, not re-litigated: neither 0.077 nor 0.141 falls inside [0.0131, 0.0705], so three runs reproduced *neither* endpoint — which supports the conclusion more strongly than "well inside a 5.4x spread" did
- [x] `ParameterDescriptor` deferral accepted for `read_only`, thin for `description` — `51e121d`. Deferral kept; #75 asked to cover descriptors for all admission parameters, so the cheap half is not lost

Also, from the review's notes for the next reviewer:
- [x] tracked stale duplicate plan at `udp_bridge/.agent/work-plans/issue-52/plan.md`, describing the superseded cycle — `c0636d9`. Deleted; the live plan is the repo-root one

## Local Review (Pre-Push)
**Status**: complete
**When**: 2026-08-26 01:40 -04:00
**By**: Claude Code Agent (Claude Opus)
**Verdict**: changes-requested

**Branch**: feature/issue-52 at `18d5eca`
**Mode**: pre-push
**Depth**: Deep (reason: ~5000 lines across a live control loop, a concurrency-critical send path and the node's configuration entry point; four prior root-cause passes on this same code)
**Must-fix**: 8 | **Suggestions**: 17
**Round**: 3 | **Ship**: continue — two NEW code defects at the configuration and clock-discontinuity boundaries, one of them introduced by round 2's own fix pass and certified working by a test that cannot reach the failing path; the branch's only regression guard for direction C can silently self-disable; and the opt-in bench suite is red at HEAD. Must-fix 4 -> 7 -> 8. See "Convergence" below — the recommendation is NOT another identical round.

Specialists: Static Analysis (pre-commit clean, 11 hooks, over `origin/jazzy..HEAD`), Governance, Plan Drift, Claude Adversarial x2 (Lens A + Lens B). Copilot and local-model passes off (default).

### Verified rather than trusted

- **Suite**: re-run on a clean tree at `18d5eca` — **273 tests, 0 errors, 0 failures, 14 skipped**. Matches the claim exactly. All 14 skips are the opt-in netns bench, none are new tests.
- **Gate values**: bounds confirmed in source (`kFieldRateLimit` = 1500000, so the `/8` and `/2` bounds are the claimed 187500 and 750000). The three field-replay tests pass.
- **The admission tests genuinely discriminate.** Lens A reverted `updateAdmissionControl` to the `origin/jazzy` body and rebuilt: all 3 `AdmissionFieldReplay` tests fail hard (`decreases 5 vs 1`, `delivered_fraction 0.398 vs 0.90`, `lowest_after_convergence 8192 vs 75000`) and 8 of 26 `AdmissionControl` tests fail. This is the strongest evidence in the round that the control-law half of the change is real.
- **Round 2's fix 1 (grown window re-clamped to the 60 s ceiling)** — CLAIM TRUE. Third bound present at `connection.cpp:353-356` as a `std::min` over growth, base x multiple, and `kMaximumAdmissionRefractoryPeriodSeconds`; `RefractoryPeriodSetterContract:1021-1038` asserts the grown window against the ceiling at a max-configured base. Coverage is real, not decorative.
- **Round 2's fix 3 (`success_rate_in_window` bounded above; `RefractoryGateHoldsAtClockZero` reworked)** — CLAIM TRUE, and the subtle part holds. The reworked test stamps traffic at t = -2 s, so the window `(t0-5, t0]` still contains it and `sent_bps` = 80000/2 = 40000 against a reported 20000: the t=0 sample is congested **without** depending on the unbounded-above bug it used to rely on. It therefore pins the same property (a decrease recorded at exactly 0.0 must open the window) rather than passing for a new reason. The claim is accurate.
- **Round 2's fix 2 (inert-AIMD WARN reads the applied floor)** — CLAIM TRUE and it cannot fire spuriously: `udp_bridge.cpp:747-749` reads `admissionFloorBytesPerSecond()` back from the connection, and with `maximum_bytes_per_second` unset `default_rate_limit` is 50000 > the 8192 default floor. `admission_floor_bytes_per_second: -1` on a connection capped below 8192 now warns, which is the case it exists for. The runtime routes are covered too (`warnIfAdmissionInert` at all three `setRateLimit` call sites); `RemoteNode::update(RemoteInfo)`, the fourth, is reachable only from `on_configure`, which warns directly.
- **The co-tenant bench invariant — the branch's stated basis for retiring `link_headroom_fraction` — had NOT been re-run since round 2 changed the detector window.** The branch's three recorded runs are at `a06f0af`; `33a10f1` (the `success_rate_in_window` rework) landed after them. I ran it: `test_invariant_cotenant_management_flow_survives` **PASSES at `18d5eca`, 3 for 3**. The verification gap is closed and the retirement's empirical basis holds at current HEAD. Full range-degradation run: 10 passed, 1 xfailed, 1 failed (see must-fix 8).
- **Concurrency**: both lenses pushed hard on the reservation ledger and neither could break it. Every `sendPacket` exit (success, ECONNREFUSED, mid-loop `no_address`, throw) releases exactly `data.size()`; the batch guard's hand-off happens before the call so a throw cannot double-release; a `bad_alloc` in `WrappedPacket` or the `sent_packets_` insert leaves the bytes with the guard; no mutex is held across `sendto`. No leak, double-release, or lock-ordering defect found.

### Findings

- [x] (must-fix, **cross-confirmed by BOTH adversarial lenses, independently re-verified by me**) **the integer-literal guard is at the wrong site: `admission_refractory_period_seconds: 5` in a real config still fails the whole lifecycle transition.** With static parameter typing, an INTEGER override for a parameter declared with a DOUBLE default throws at `declare_parameter`, inside `declareIfMissing` — before `getDoubleParameter` is ever reached. I compiled a standalone rclcpp probe against `/opt/ros/jazzy`: `declare_parameter THREW: InvalidParameterTypeException: parameter 'refract' has invalid type: ... parameter {refract} is of type {double}, setting it to {integer} is not allowed.` A parameter *override* is not a declared parameter, so `has_parameter` is false and `declareIfMissing` calls the throwing overload. `RetiredParameters.IntegerLiteralForADoubleParameterConfigures` passes only because its harness **pre-declares** the key as INTEGER (`test_node_name_limits.cpp:104-110`), which short-circuits `declareIfMissing` and moves the failure to `as_double()` — a state no YAML file or `-p` override can produce. The fixer's "restoring bare `as_double()` makes it fail" check was honest and is true of that path; the path is not the field path. Same exposure for `admission_floor_bytes_per_second: 9000`, `resend_budget_fraction: 1`, `period: 1`. Coerce at declaration (inspect `get_parameter_overrides()`, or catch `InvalidParameterTypeException` and re-declare coerced) and drive the test through `NodeOptions::parameter_overrides` — `udp_bridge/include/udp_bridge/udp_bridge.h:306,312-333`, `udp_bridge/test/test_node_name_limits.cpp:362`
- [x] (must-fix, Lens A with an empirical probe, Lens B independently) **a backwards clock step permanently wedges `can_send` and grows the statistics deque without bound** — the layer directly beneath the two this branch hardened for exactly that event. `Statistics::add` (`statistics.h:57-61`) evicts only from the front and only relative to the *incoming* record's timestamp, so after a backwards step the future-stamped front entries make the predicate false forever. `can_send` and `bytes_in_window` are bounded only below, so those stranded entries are summed on every call. Lens A's probe against the real library: deque 1001 -> 3001 over 20 s of post-step traffic (should stay ~1000), and `can_send(1000, 0, 1500000, t=20)` returns **0** — the connection admits nothing, permanently, presenting as "the link stopped". `Connection::data_receive_rate` has the same shape (`std::map`, large keys sort to the back, the `begin()` erase loop never reaches them). Round 2 bounded `success_rate_in_window` at both ends for precisely "a looping bag replay, a sim reset" and stopped at one of four siblings — `udp_bridge/src/statistics.cpp:159,237`, `udp_bridge/include/udp_bridge/statistics.h:57`, `udp_bridge/src/connection.cpp:931-940`
- [x] (must-fix, Lens B; Lens A independently established the premise) **`ConcurrentMessagesAreAllOrNothing` — the ONLY test that discriminates against a regression of direction C — silently disables itself entirely on one `ENOBUFS`.** Lens A's revert experiment confirms it is the sole discriminator (reverting to the check-only aggregate fails this test and no other). `GTEST_SKIP()` returns from the *test function*, not the round, so one throw in round 0 skips all 25 rounds and the closing `EXPECT_GT(total_successes, 0)`, and the run reports green. The comment says "Skip the round instead", which is not what the mechanism does. Probability rises exactly as the machine gets busier — i.e. as the race gets more catchable. Use `continue`, count skipped rounds, and fail if too few completed — `udp_bridge/test/test_message_atomicity.cpp:371-378`
- [x] (must-fix, Lens B) **the AIMD cap and the refractory window reach no diagnostic surface, so the failure this branch exists to make legible is still invisible where an over-horizon operator looks.** `diagnoseConnection` publishes `rate_limit_bytes_per_sec` — the *configured* limit, the number that never moves — and never `effectiveRateLimit()` or `currentAdmissionRefractoryWindowSeconds()`. `effective_rate_limit` exists only as a `BridgeInfo` field, requiring the operator to subscribe to a bridge topic and diff two numbers by eye: precisely what did not happen on 2026-08-25. `connection.h:92-95` concedes "a frozen controller and a healthy flat cap look identical" and defers to #75 — but that reasoning is about the **message schema**, where a field costs a type-hash break and a coordinated redeploy. A diagnostic KeyValue costs neither. Two `stat.add` lines close the operator-visibility gap for this release rather than the next. (Related, same function: the WARN at `:2882` says "tx failures/drops" identically for socket failures and for deliberate admission shedding, and a throttled connection now sits in it continuously) — `udp_bridge/src/udp_bridge.cpp:2862,2882`
- [x] (must-fix, **triple-confirmed: Lens A, Governance, and my own arithmetic check**) **the retirement tripwire is correct only by an accidental float widening, and three operator surfaces state behaviour it does not have.** The WARN fires on `link_headroom_fraction != static_cast<double>(kDefaultLinkHeadroomFraction)`. Measured: `static_cast<double>(0.2f)` = 0.20000000298023223877, YAML `0.2` = 0.20000000000000001110 — unequal, so `link_headroom_fraction: 0.2` **does** warn. That is the desirable behaviour and it is an accident: make `kDefaultLinkHeadroomFraction` a `double` — a plausible tidy-up — and the tripwire goes silent on the shipped default, the single most likely stale value in a real config, with no test failing. This is the same "correct by an incidental property of another declaration" pattern round 2 deliberately removed from `earliest.nanoseconds() == 0`. Meanwhile the cited precedent (`remotes.<label>.name`) is **presence**-based with a test pinning exactly that ("rejected on presence, not on whether it would have changed the identity"), so the divergence is wider than the WARN-vs-FAIL axis the Implementation entry names. Warn on presence via `get_parameter_overrides()`; `README.md:213`, `config/example_params.yaml:133-134`, `.agents/README.md:109` all say "anything but the default" and are false as written — `udp_bridge/src/udp_bridge.cpp:684`
- [x] (must-fix, Governance, cross-confirmed by the accuracy sweep) **seven live comments still describe the removed `(1 - headroom) x goodput` clamp as the operative decrease target.** Third round running for this class, after two dedicated sweeps. All are untouched context lines, none imply a code change: `connection.cpp:249-252` ("left in the headroom target below that slams the cap to the floor"), `connection.cpp:260-262` ("treated as congested with a headroom target of 0"), and `test_admission_control.cpp:465-469,503-506,526-529,569-572,604-611` (four "the headroom target would slam the cap" variants plus "the one congested path that does NOT apply the headroom target" — every congested path is now identical, so the stated reason for that test's construction no longer exists)
- [x] (must-fix, Governance, verified by grep) **a code comment points readers at a test that does not exist.** `test_admission_control.cpp:250` names `RetiredHeadroomParameterStillConfigures`; the only match in the repo is that comment itself. The real test is `RetiredParameters.LinkHeadroomFractionStillConfigures` (`test_node_name_limits.cpp:346`). `plan.md` gets the name right; the code comment does not — `udp_bridge/test/test_admission_control.cpp:250`
- [x] (must-fix, measured) **the strict `xfail` on `test_invariant_resend_amplification` XPASSes at HEAD, and the plan's verification record says it does not.** Four full range-degradation runs at `18d5eca`: **3 XPASS(strict) -> FAILED, 1 xfailed**. `plan.md:695` records "**Outcome**: the marker stays (strict, no XPASS)" and the xfail reason string this branch itself rewrote (`90a16cf`) asserts "What IS stable is that at least one phase violates every run, so the marker holds" — both false at HEAD, 3 times in 4. The branch's own n=3 was measured at `a06f0af`, before `33a10f1` reworked the detector window; and its own n=3 data shows the margin was always thin (one run violated only via `lossy` at 0.0705 against a 0.060 ceiling). Per the repo's own `#57` note the marker must NOT be removed and `F_resend_multiplier` must NOT be raised to keep it xfailing — so the required action is to record the measurement and route the decision to #54, not to silence it. A strict xfail on a metric with 4-5x run-to-run spread is a coin-flip red suite either way, which is itself worth saying — `udp_bridge/test/bench/test_range_degradation.py:678-699`, `.agent/work-plans/issue-52/plan.md:686,695`
- [x] (suggestion, Lens A) the co-tenant claim is stronger than the mechanism supports. The removed clamp had a **structural** property — it reserved a share of measured throughput whether or not the bridge itself was losing. A pure multiplicative decrease fires only when `remote_received < 0.9 x sent_bps`, i.e. only when the bridge's OWN delivery degrades; on a saturated path where the bridge wins the queue and the co-tenant is the one starved, the bridge reads clean and never backs off. The invariant passes (I confirmed, 3/3 at HEAD) because the recorded trajectory has the bridge losing too. Honest statement: no headroom mechanism remains; the co-tenant is protected insofar as the bridge shares the loss. Belongs on #61, where a replacement basis would be argued. (Lens A's accompanying claim that the bench invariants "skip in this environment" is wrong — they are opt-in and I ran them) — `test/bench/README.md`, `test/bench/test_range_degradation.py`, `test/bench/cotenant.py`
- [x] (suggestion, Governance) two feedback-validation tests no longer discriminate. With the clamp gone, `DuplicateExceedingReceivedTreatedAsCongestion` (received 40000 vs sent ~80000) and `NegativeRatesTreatedAsCongestion` (received -100) both satisfy `received < 0.9 x sent` on their own, so deleting the `feedback_unusable` guard entirely leaves both green. The guard IS still load-bearing in general (received 100000 / sent 100000 / duplicate 200000 reads clean without it) — these two just do not exercise it. `NanFeedbackTreatedAsCongestion` still does. Drive them from a clean-looking received rate, or label them contract tests as the atomicity file already labels its own — `udp_bridge/test/test_admission_control.cpp:519-540,560-580`
- [x] (suggestion, Lens B) `BatchReservationGuard::unreserved` was clamped in round 2, but `sendPacket`'s two releases still bare-decrement `reserved_bytes_in_flight_` — the counter whose underflow actually wedges the connection permanently, which is the failure mode the clamp's own comment describes. Clamp both, or replace the argument-by-comment with a one-time ERROR when `data.size() > reserved_bytes_in_flight_` — `udp_bridge/src/connection.cpp:723-736,782,870`
- [x] (suggestion, mine) `warnIfAdmissionInert`'s throttle rationale is wrong. `RCLCPP_WARN_STREAM_THROTTLE` keeps its state per **call site**, not per connection, so a second connection entering the inert state within 60 s of the first is suppressed — and because the call is event-driven (CONNECT decode / `add_remote`), not periodic, it is never re-emitted. The comment says throttling was chosen precisely so a second connection would not be hidden; it does not achieve that — `udp_bridge/src/udp_bridge.cpp:93-95`
- [x] (suggestion, mine) round 2's `ffa787d` tightened the clock-zero reachability statement in `connection.cpp` but not in its sibling in the header, which still carries the loose form the fix rejected ("Under `use_sim_time` before the first `/clock`, or a bag replayed from t=0") — `udp_bridge/include/udp_bridge/connection.h:353-357`
- [x] (suggestion, mine) `currentAdmissionRefractoryWindowSeconds()`'s doc still says the grown window is "capped at base x kAdmissionRefractoryMaximumMultiple" — omitting the 60 s third bound round 2 added, which is exactly the bound that matters at a max-configured base — `udp_bridge/include/udp_bridge/connection.h:87-90`
- [x] (suggestion, mine) `success_rate_in_window`'s window is documented as `(time - w, time]` but implemented as `[time - w, time]` (`entry.timestamp < window_start` keeps the equal case). `WindowMatchedSendRateSurvivesABurst` depends on the closed lower bound — its 90 kB is stamped at exactly `t1 - 5` — so the doc and the test disagree about which end is open — `udp_bridge/include/udp_bridge/statistics.h:113-115`, `udp_bridge/src/statistics.cpp:180,197`
- [x] (suggestion, Lens B) `admission_refractory_period_seconds: 0` — documented as restoring the behaviour that caused the incident — is accepted silently, while a retired parameter and an inert floor each get a WARN. Same class of capability-limiting configuration the other two WARNs exist for — `udp_bridge/src/udp_bridge.cpp:696-698`
- [x] (suggestion, Lens A) `effectiveRateLimit()` casts a `float` to `uint32_t` with no clamp, so a peer-advertised `return_maximum_bytes_per_second` near `UINT32_MAX` rounds to 4294967296.0f and the cast is out of range — on x86-64 the metered cap reads **0** and the connection admits nothing. Pre-existing (#43), but this branch made `effective_rate_limit_` load-bearing at three read sites and added peer-input clamps elsewhere for #53. Corruption/version-skew robustness, not a security claim — `udp_bridge/src/connection.cpp:175-179`
- [x] (suggestion, Lens B) `warnIfAdmissionInert` is the first caller to nest a `Connection` mutex under a `UDPBridge` one (called while `remote_nodes_mutex_` and `pending_connections_mutex_` are held; it takes `config_mutex_` twice). Safe today — `Connection` never reaches back up — but `connection.h:292-303` and `updateAdmissionControl`'s locking preamble both go out of their way to record that Connection's mutexes are never nested. One line keeps the invariant auditable — `udp_bridge/src/udp_bridge.cpp:2044,2651,2679`
- [x] (suggestion, Lens A) `FloorNeverExceedsConfiguredLimit` feeds ten congested samples at one timestamp; under the new gate only the first is acted on, so the loop is decorative. It still discriminates for its stated property, but the "repeatedly" framing no longer holds — space the samples like its siblings or drop the loop — `udp_bridge/test/test_admission_control.cpp:~404`
- [x] (suggestion, mine) `send_at_rate` now has no callers — both replays moved to `send_over_interval`. It is still referenced by two comments as the contrast; either say so at the definition or remove it — `udp_bridge/test/test_admission_field_replay.cpp:165`
- [x] (suggestion, Governance) the design doc carries review-round bookkeeping about this branch's own prior mistake ("Five references in this change pointed at it") inside a durable document; the correction belongs in `progress.md`, which already has it. The doc needs only the #75 link — `udp_bridge/doc/admission_control_design.md:279-283`
- [x] (suggestion, Governance) `resend_constants.h` keeps eight lines of present-tense headroom rationale ("A headroom target **scales** with whatever the link is actually delivering, so it **holds**...") immediately before the RETIRED block that contradicts it. Recast to past tense — `udp_bridge/include/udp_bridge/resend_constants.h:286-293`
- [x] (suggestion, Governance) `CMakeLists.txt` claims the atomicity test covers "reservation release on every exit path a fragment can take"; the test file itself states ECONNREFUSED is unreachable on an unconnected UDP socket and is covered only by inference. The test file is honest, the build file is not — `udp_bridge/CMakeLists.txt:159`
- [x] (suggestion, Governance + Lens A + mine) typo: "a fragment ... that can never be reassembled **is spend** on a degraded link" -> "is spent" — `udp_bridge/README.md:231`
- [x] (suggestion, Plan Drift) `plan.md:686` states "The two already-committed tests are **unedited** — they remain the gate." Both were edited on this branch after that line was written: `516037c` (+44/-10, touching both) and `4f807c0` (+8/-1, `send_at_rate` -> `send_over_interval` in the handover test). The edits are defensible and `progress.md` records them; the plan row does not — `.agent/work-plans/issue-52/plan.md:686`
- [x] (suggestion, Plan Drift) three changed-but-unplanned files missing from Files to Change, which already uses an "Added during implementation" convention: `include/udp_bridge/udp_bridge.h` (+23, the `getDoubleParameter` helper — a behaviour change to four existing parameter reads), `test/test_node_name_limits.cpp` (+69, the only coverage of the tripwire), `test/bench/cotenant.py` (+20/-9) — `.agent/work-plans/issue-52/plan.md:677-695`
- [x] (suggestion, Governance) this is the third root-cause pass on this control loop in ~2 weeks and the project repo still has no `docs/decisions/`. The plan recommends a project-repo ADR (`plan.md:940-945`) without committing to it. Fine to defer; worth the operator seeing it named

### The parameter decision — judging the divergence on safety grounds

**I concur with the divergence. WARN, not FAIL, is the right call, and the reasoning given for it is sound.**

The two retired keys are not alike in the way that decides this. A stale `remotes.<label>.name` changes what the system **does**: the wire identity stops matching the configured one, the relay-loop rule is a string compare and goes inert, and the hub echoes a sender's own traffic back at it (#51). That is a silent behavioural fault on a vessel over the horizon, and refusing to configure is proportionate — the alternative is a boat running with a broken routing invariant it cannot detect.

A stale `link_headroom_fraction` changes **nothing** the system does. There is no code path that reads it. The residual harm is an operator holding a wrong model of which mechanism protects the co-tenant. Real, but a harm to belief, not to behaviour — and the WARN corrects it.

Weighing the two failure modes explicitly, as asked:

- **Silent misconfiguration under WARN — bounded.** The link is still protected, and I verified that rather than assuming it: `test_invariant_cotenant_management_flow_survives` passes 3/3 at HEAD. Worst case the operator credits the wrong mechanism. Nothing is misrouted, no cap is mis-set, no traffic is lost.
- **Failure to launch under FAIL — unbounded, and it lands at the worst possible moment.** `on_configure` returning FAILURE leaves the node UNCONFIGURED, and udp_bridge **is** the boat's telemetry, video and command transport. A vessel that will not bring up its link because of a config key that changes nothing is a total loss of the operator's remote view, discovered at launch, in the field — where by this workspace's own field-mode rules there is no GitHub access and the remedy is hand-editing YAML on a pitching deck.

The asymmetry is decisive, and it runs the same direction as the Quality Standard's framing: refusing to configure is warranted when a stale key would make the system do something **wrong**, not merely when it would make an operator **think** something wrong. The fixer drew exactly that line.

Two qualifications, both already filed above:

1. The precedent cited is **presence**-based, with a test pinning that it rejects "on presence, not on whether it would have changed the identity". This implementation is **value**-based, so the divergence is wider than the WARN-vs-FAIL axis — and it lands on the wrong side for the likeliest stale value, saved only by a float-to-double widening accident (must-fix 5).
2. Warning on **presence** resolves both at once: it restores the cited precedent's shape, covers `0.2`, and costs nothing operationally, because a presence-warn never fails a transition.

Separately, and worth putting to the operator with this: the retirement gave up more than the entry says. The clamp had a structural property the multiplicative decrease does not have (suggestion 1 above). That does not reopen the retire-vs-keep decision — the clamp was measurably harmful and cannot return as-is — but "the refractory-gated multiplicative decrease carries the co-tenant guarantee now" is true of the tested trajectory, not a guarantee in general. The accurate statement is that no headroom mechanism remains.

### Convergence — the explicit read I was asked for

**No. The defect rate is not converging, and the raw counts understate it.**

Must-fix by round: 4 -> 7 -> 8. Flat-to-rising after two fix passes that closed 43 findings between them. But the count is the least informative part:

- **Are the fix passes introducing new defects, or only closing old ones? Both — at a steady rate of roughly one new code defect per pass.** Round 1's fixes introduced 2 of round 2's 3 code defects. Round 2's fixes introduced must-fix 1 of this round (the guard placed one call too late, shipped with a test that certifies it) and left must-fix 2 half-done (the both-ends bound applied to one of four sibling functions). That is not a loop stuck on the same issues; it is a loop generating new ones at about the rate it closes them.
- **The recurring class is the branch making false statements about itself**, and it has now survived three rounds and two dedicated sweeps: 7 live comments still describing the removed clamp (must-fix 6), a comment naming a test that does not exist (must-fix 7), three operator surfaces describing a WARN condition the code does not implement (must-fix 5), and two false statements in the plan's own verification record (must-fix 8 and the `unedited` row).
- **The mechanism behind the non-convergence is identifiable, and it is not carelessness.** Each fix pass is verified by tests the same pass writes, and twice now those tests have been shown not to reach the path they name. Round 2 caught it in round 1's work (`RefractoryGateHoldsAtClockZero` depended on the very bug it was pinning). Round 3 catches it in round 2's (`IntegerLiteralForADoubleParameterConfigures` pre-declares the parameter in a shape YAML cannot produce). A verification loop whose oracle is authored by the party being verified does not converge on its own.

**So: is another round worth it? Not another identical round.** The accuracy class (must-fix 5-8) is mechanical and a single focused sweep closes it. But the two code defects fell only to *executing* things — a standalone rclcpp probe against the real parameter machinery, and a probe driving the real `Statistics` deque across a clock step. Two prior Deep rounds read this code closely and missed both. A fourth round of reading has low expected yield; a fourth round that **runs the configuration entry point from a real params file and runs the clock-discontinuity path against the real deque** has high yield, and would have caught both of these in round 1.

My recommendation to the operator: fix the 8, and change one thing about how the next pass verifies — **require any test for a configuration-entry defect to drive the real entry point** (`NodeOptions::parameter_overrides`, or an actual params YAML), never a hand-declared stand-in. Without that, round 4 most likely produces another honest "verified by breaking it" claim that is true of a path the field never takes.

**What is NOT in doubt:** the control law itself. Lens A proved the admission tests discriminate against the pre-fix code (3 field-replay + 8 unit tests fail on revert), neither lens could break the reservation ledger, and the co-tenant bench invariant passes 3/3 at HEAD. Every defect in this round sits at an edge — configuration entry, clock discontinuity, diagnostics, test integrity, and self-description — not in the mechanism the branch exists to fix. That is a meaningfully better place to be than round 1, even though the must-fix count did not fall.

### Notes for the next reviewer

- **Nothing in this review modified the tree.** `git status` clean at `18d5eca` throughout and confirmed at exit; both adversarial lenses ran revert experiments and both confirmed restoration. My rclcpp probes were standalone programs in the scratchpad, outside the repo. Nothing committed except this entry, nothing pushed.
- **The bench is opt-in and was not part of the branch's own re-verification after round 2.** `UDP_BRIDGE_BENCH_SCENARIOS=1` is required; the 14 skips in the default suite are all bench. `unshare -Urn` works on this host. Four full runs are recorded above. The `test_invariant_recovery_completeness` xfail (#60) is unchanged.
- Both lenses independently reached must-fix 1 from different directions, and three sources independently reached must-fix 5. Those two are as well-corroborated as findings in this repo get.
- Lens A's claim that "the bench co-tenant invariants skip in this environment" is **wrong** and I have not carried it forward — they are opt-in, I ran them, they pass.
- Round 2's three claimed correctness fixes are all genuinely correct, including the subtle one (the reworked `RefractoryGateHoldsAtClockZero` pins the same property, not a new one). The problems are in what round 2's fixes *also* introduced, not in what they claimed.
- ADR-0018: `git notes --ref=ci-local` still empty at HEAD. Expected pre-push; it is the merge gate.

## Implementation
**Status**: complete
**When**: 2026-08-26 06:43 -04:00
**By**: Claude Code Agent (Claude Opus)

**Branch**: feature/issue-52 at `6120722`
**Addressed**: `## Local Review (Pre-Push)` (round 3), 2026-08-26 01:40 -04:00, at `18d5eca` — 8 must-fix, 17 suggestions, all 25 actioned
**Commits**: `c896320` `6960762` `de32e74` `6e4199e` `44125b3` `4dcef7a` `9d21edf` `cd4a2f6` `d5b251d` `6120722`

### The rule this pass adopted, and what applying it found

**Any test that exercises a configuration entry point must drive the REAL
entry point** — `rclcpp::NodeOptions::parameter_overrides` or an actual
params YAML — never a hand-declared stand-in, and never a harness that
pre-establishes a state the real entry point cannot produce.

**Tests converted**: the entire `Bridge` harness in
`test/test_node_name_limits.cpp` — the only configuration-entry test
harness in the package, and the one that certified round 2's fix. Every
value it takes (`name`, `port`, `remotes_list`, `remotes.<label>.name`,
per-connection `host`/`port` and arbitrary typed parameters, and the new
per-topic block) is now a `NodeOptions` parameter OVERRIDE, and the node
is not constructed until `configure()`, because overrides are fixed at
construction. Nothing is pre-declared. `UDPBridge` gained a
`NodeOptions` constructor parameter to make that possible. All 18 tests
in that file — 13 pre-existing, 5 new — now run through the real path.

**Applying it exposed a previously-"fixed" finding as still broken:
yes.** `RetiredParameters.IntegerLiteralForADoubleParameterConfigures`
passed at `18d5eca` and its subject did not work. Converting the harness
made it FAIL, exactly as the reviewer predicted: the throw is at
`declare_parameter`, and the round-2 guard sat at the read site, one
call too late. Verified in both directions — with the declaration-site
guard reverted the converted test fails; with it in place it passes.
The same conversion also exposed a second, unflagged case: the retired
`link_headroom_fraction` tripwire had **no observable assertion at all**
(nothing in this package captures log content), so the only thing any
test could check was the outcome the tripwire must NOT have. That is why
the float-widening accident could have been tidied away with no test
failing. A `retiredParameterWarningCountForTest` counter closes it.

Every other fix in this pass carries a verified-by-breaking-it check
where one is possible: the five clock-discontinuity tests were run with
each bound removed (all five fail), and the extreme-rate-limit test was
written against the unclamped cast.

### Actions

**Must-fix**

- [x] **Integer literal for a double parameter fails the whole lifecycle
  transition** — fixed at the DECLARATION site. `declareDoubleIfMissing`
  reads the parameter overrides and, for an INTEGER override, declares
  the coerced double with `ignore_override = true`, so the parameter
  stays statically typed as a DOUBLE. Applied to all eight
  double-defaulted declarations (`reorder_hold_window_ms`,
  `resend_giveup_warn/error_rate_per_s`, `resend_budget_fraction`,
  `admission_floor_bytes_per_second`,
  `admission_refractory_period_seconds`, per-topic `period`).
  `getDoubleParameter` stays for the already-declared route (a composed
  node). Tests drive `NodeOptions::parameter_overrides` and assert the
  parameter is DOUBLE-typed *and* carries the configured magnitude — a
  guard that swallowed the override and installed the default would also
  reach INACTIVE. A wrong type with no defensible reading (a string) is
  still refused — `udp_bridge/include/udp_bridge/udp_bridge.h:298-360`,
  `udp_bridge/test/test_node_name_limits.cpp:406-480` (`c896320`)
- [x] **A backwards clock step permanently wedges `can_send`** — the
  both-ends bound is applied to all four siblings, not one.
  `Statistics::add` now evicts records stamped past the window as well as
  before it (front-only eviction cannot reach a future-stamped front, so
  it stopped entirely and the deque grew without bound); `can_send` and
  `bytes_in_window` exclude records stamped after `time`; and
  `Connection::data_receive_rate` erases from the back of its map, where
  future-stamped records sort and its `begin()`-erase loop never reached
  them. Five tests, each verified to fail with its bound removed: deque
  growth, `can_send` recovery, `bytes_in_window`, `data_receive_rate`,
  and the `Connection` send path end to end —
  `udp_bridge/include/udp_bridge/statistics.h:52-110`,
  `udp_bridge/src/statistics.cpp`, `udp_bridge/src/connection.cpp:955-975`,
  `udp_bridge/test/test_admission_control.cpp:900-1050` (`6960762`)
- [x] **`ConcurrentMessagesAreAllOrNothing` silently disables itself on
  one ENOBUFS** — `GTEST_SKIP()` replaced with `continue`. Skipped rounds
  are counted, and the test FAILS after the loop if fewer than 10 of 25
  completed, naming the environment as the cause. The closing
  `total_successes > 0` check is now always reached —
  `udp_bridge/test/test_message_atomicity.cpp:316-450` (`de32e74`)
- [x] **The AIMD cap reaches no diagnostic surface** — fixed, not
  declined. `diagnoseConnection` now publishes
  `effective_rate_limit_bytes_per_sec`, `admission_refractory_window_s`
  and `admission_floor_bytes_per_sec` beside the configured
  `rate_limit_bytes_per_sec`; the summary line names the cap when one is
  in force; and the single "tx failures/drops" WARN is split into
  socket-level `tx failures` and deliberate `shedding at admission cap N
  of M B/s`. The level/summary decision is extracted to a pure
  `computeConnectionDiagnostic`, following the `computeGiveupDiagnostic`
  precedent, with 9 tests —
  `udp_bridge/include/udp_bridge/connection_diagnostic.h`,
  `udp_bridge/src/udp_bridge.cpp:2862-2905`,
  `udp_bridge/test/test_connection_diagnostic.cpp` (`6e4199e`)
- [x] **The retirement tripwire is correct only by a float widening
  accident, and three operator surfaces describe behaviour it does not
  have** — detection moved to PRESENCE in
  `get_node_parameters_interface()->get_parameter_overrides()`, which
  rclcpp carries whether or not a parameter is declared. That removed the
  reason to declare the parameter at all (the "declare it or rclcpp hides
  the override" rationale was simply wrong), so it is no longer declared
  and `kDefaultLinkHeadroomFraction` is deleted — a retired key must not
  appear in `ros2 param list` looking live. `README.md:213`,
  `config/example_params.yaml:111-136` and `.agents/README.md:109` all
  corrected, and all three now also state that **no headroom mechanism
  remains**. Five tests through the real entry point, including the old
  `0.2` default, an integer literal, absence, and one WARN per connection
  — `udp_bridge/src/udp_bridge.cpp:660-720` (`44125b3`)
- [x] **Seven live comments describe the removed clamp as operative** —
  all seven rewritten to say what the guard actually protects now (a
  collapsed goodput reads as total loss to the congestion comparison and
  is published to the resend budget). The remaining `headroom` mentions
  in these files are explicit past-tense retirement records —
  `udp_bridge/src/connection.cpp:249-262`,
  `udp_bridge/test/test_admission_control.cpp:465-611` (`44125b3`)
- [x] **A code comment names a test that does not exist** — corrected to
  `RetiredParameters.LinkHeadroomFraction*` in
  `test_node_name_limits.cpp` — `udp_bridge/test/test_admission_control.cpp:250`
  (`44125b3`)
- [x] **The strict `xfail` XPASSes at HEAD and the plan says it does
  not** — measured, corrected, and routed to #54 rather than resolved
  here. **4 runs at the round-3 fix-pass HEAD: 2 XPASS(strict) → FAILED,
  2 xfailed**; with the reviewer's 3-in-4 at `18d5eca` that is 5 of 8
  across two builds, i.e. the opt-in bench suite is red about half the
  times it runs. The marker is deliberately NOT removed and
  `F_resend_multiplier` deliberately NOT raised — the repo's own #57 note
  forbids both as a way to silence this. Both false statements
  corrected: `plan.md:699` and the round-2 verification narrative at
  `plan.md:574-590`, plus the `xfail` reason string this branch itself
  wrote —
  `udp_bridge/test/bench/test_range_degradation.py:668-700`,
  `.agent/work-plans/issue-52/plan.md` (`6120722`)

**Suggestions** (all 17 actioned; none deferred)

- [x] The co-tenant claim is stronger than the mechanism supports — the
  design doc, the bench README and `resend_constants.h` now state that
  the clamp had a STRUCTURAL property the multiplicative decrease does
  not, that the decrease fires only when the bridge's own delivery
  degrades, and that the invariant passes because the recorded trajectory
  has the bridge losing too. "No headroom mechanism remains"; #61 is
  where a replacement basis is argued (`9d21edf`, `44125b3`)
- [x] Two feedback-validation tests no longer discriminate —
  `DuplicateExceedingReceivedTreatedAsCongestion` is driven from a
  clean-looking received rate (100000 against ~80000 sent, so the
  duplicate>received guard is on the critical path);
  `NegativeRatesTreatedAsCongestion` is labelled a contract test, with
  the reason it cannot be made to discriminate, and a new
  `NegativeDuplicateWithCleanReceivedIsStillUnusable` puts the negative
  guard on the critical path (`44125b3`)
- [x] `sendPacket`'s releases still bare-decrement the counter whose
  underflow wedges the connection — all four release sites now go through
  one clamped `releaseReservedBytes` helper (`4dcef7a`)
- [x] `warnIfAdmissionInert`'s throttle rationale is wrong — corrected:
  the throttle is per CALL SITE, so it does not keep a second connection
  visible, and on an event-driven path the suppressed line is never
  re-emitted. What covers it instead is named (`4dcef7a`)
- [x] The clock-zero reachability statement in the header still carries
  the loose form — tightened to match its sibling, and both updated for
  the fact that a backwards step no longer leaves pre-step history usable
  (`4dcef7a`)
- [x] `currentAdmissionRefractoryWindowSeconds`' doc omits the 60 s third
  bound — added, and its "no diagnostic consumer yet" note replaced with
  the consumer it now has (`4dcef7a`)
- [x] `success_rate_in_window`'s window documented half-open but
  implemented closed — documented as `[time - w, time]`, naming the test
  that depends on the closed lower bound (`4dcef7a`)
- [x] `admission_refractory_period_seconds: 0` accepted silently — now
  WARNs, saying it restores the behaviour that caused the incident
  (`4dcef7a`)
- [x] `effectiveRateLimit()` casts a float to `uint32_t` with no clamp —
  clamped to the largest float below 2^32, with a non-finite guard, and
  tested at `UINT32_MAX` (`4dcef7a`)
- [x] `warnIfAdmissionInert` nests a Connection mutex under a UDPBridge
  one — recorded at the definition so the invariant stays auditable
  (`4dcef7a`)
- [x] `FloorNeverExceedsConfiguredLimit`'s loop is decorative — samples
  spaced past the refractory window, like its siblings (`4dcef7a`)
- [x] `send_at_rate` has no callers — removed; the two comments that
  referenced it now describe the bias directly (`4dcef7a`)
- [x] Design doc carries review-round bookkeeping — removed; the #75 link
  stays (`44125b3`)
- [x] `resend_constants.h` keeps present-tense headroom rationale —
  recast to past tense as a RETIRED block, and the constant itself
  deleted since presence-detection needs no default (`44125b3`)
- [x] `CMakeLists.txt` claims coverage of every fragment exit path —
  corrected to match what the test file itself says about ECONNREFUSED
  (`4dcef7a`)
- [x] Typo "is spend" → "is spent" — `udp_bridge/README.md:231` (`44125b3`)
- [x] `plan.md` says the two committed field-replay tests are unedited —
  corrected, naming both edits and their SHAs (`6120722`, `d5b251d`)
- [x] Three changed-but-unplanned files missing from Files to Change —
  added under the existing "Added during implementation" convention,
  along with the three files this round added (`6120722`)
- [x] No `docs/decisions/` in this project repo after three root-cause
  passes — the plan's existing recommendation is strengthened and the
  operator is told explicitly that it is a decision being asked of them,
  not an omission. Still not committed to in this PR's scope (`6120722`)

### Verification

- **Suite**: `./core_ws/test.sh udp_bridge` on a clean tree at `6120722`
  — **296 tests, 0 errors, 0 failures, 14 skipped** (was 273; +23 from
  this pass). All 14 skips are the opt-in netns bench, unchanged.
- **The four gate values, re-measured at this HEAD** (via temporary
  instrumentation on the field-replay tests, since they are not printed
  on pass; the instrumentation was reverted and the tree is clean):
  onset `lowest` **375000** (> 187500), handover delivered **1.000**
  (> 0.90), handover final cap **1500000** (> 750000), handover
  decreases **1** (≤ 1). No regression.
- **Bench**: `test_invariant_resend_amplification`, 4 full
  `range_degradation` runs at this HEAD — 2 XPASS(strict) → FAILED, 2
  xfailed. Recorded above and routed to #54.
- **Lint**: `pre-commit run --from-ref origin/jazzy --to-ref HEAD` — all
  11 hooks pass.
- Nothing pushed. `git notes --ref=ci-local` still empty (expected
  pre-push; it is the merge gate).

### For the next reviewer

- The one judgement call worth checking: `link_headroom_fraction` is no
  longer DECLARED. The review asked for presence-based detection; not
  declaring follows from it (the override map carries the key either
  way), and it removes a retired parameter from `ros2 param list`. If the
  operator wants the key to stay introspectable, that is a one-line
  change back.
- `UDPBridge`'s new `NodeOptions` constructor parameter is public API.
  It exists so tests can reach the real configuration entry point; the
  default preserves the previous behaviour exactly
  (`enable_logger_service(true)` is still forced on).
- `retiredParameterWarningCountForTest` is a test accessor on the node.
  It is the only way anything in this package can observe a WARN.
