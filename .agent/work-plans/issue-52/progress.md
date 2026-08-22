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
- [ ] (must-fix, Copilot) **A negative received/duplicate pair passes validation and slams the cap to the floor** — `src/connection.cpp:225-231`. `feedback_unusable` rejects non-finite values and `duplicate > received`, but not negative rates: with `received = -100, duplicate = -200`, `duplicate > received` is **false** (-200 > -100), so the sample is accepted. **Copilot's stated consequence is wrong** — it claims goodput inflates above received; the downstream `std::clamp(raw, 0, max(0, received))` catches that and yields 0 (verified numerically). The real harm is different and worse: the sample is never marked unusable, so `congested` is true (a negative received is below any threshold), the headroom target becomes `0.8 x 0 = 0`, and `effective = max(floor, min(halving, 0))` = **the floor, in a single sample**. That is precisely the pathology the received- and duplicate-channel guards were added to prevent, re-entered a third way — and reachable by any peer over an unauthenticated transport (#53). Fix: add `remote_received_bps < 0.0f || remote_duplicate_bps < 0.0f` to `feedback_unusable`; negative rates are nonsensical and should fall back to the plain halving.
- [ ] (valid, Copilot) **Stale basis in the starvation-floor comment** — `include/udp_bridge/resend_constants.h:110` still reads "~1.6% of the rate limit at the default fraction"; the basis moved to measured goodput. Notable as a **partially-fixed cluster**: the Round-2 `Local Review` cited `resend_constants.h:83-99` and the fix addressed exactly those lines, leaving line 110 stale. Same failure mode as the `.agents/README.md` drop earlier in this PR — a cluster fixed to the letter of its line citation rather than its intent.
- [ ] (valid, Copilot) **`link_headroom_fraction` has no test at a non-default or boundary value** — `src/connection.cpp:139`. No test calls `setLinkHeadroomFraction` at all (grepped), so the clamp to `[0.0, 0.99]`, the NaN fallback, and the effect of a non-default headroom on the congested-branch target are unexercised. `DecreaseTargetsHeadroomOfGoodput` only covers the default.
- [ ] (valid, Copilot) **Plan contradicts the delivered congestion-detection behaviour** — `.agent/work-plans/issue-52/plan.md:89` still says to use `received - duplicate` "for **both the congestion test** and step 3". The implementation deliberately uses the raw received rate for detection, and Round-1 `review-code` explicitly rebutted the goodput-swap as one that would false-trigger. The plan must record the delivered decision.
- [ ] (valid, Copilot) **PR description test count is wrong** — the body still claims `range_degradation`: "8 pass, 1 xfail". The scenario now has 10 tests (6 single-path + 4 multi-link) and the recorded run is **9 pass + 1 xfail**. Host error, flagged twice now and still uncorrected.

### False positives
- (Copilot) `doc/resend_budget_design.md:44` "Fraction of the cap" section still cap-relative — **already fixed**; that section no longer exists (grepped, no match).
- (Copilot) `connection.h:55` `setResendBudgetFraction` doc still says "of the rate limit" — **already fixed**; it now reads "maximum fraction [0, 1] of measured goodput ... rebased from the configured rate limit to goodput by issue #52".
- (Copilot) `test/bench/README.md` still says five invariants — **already fixed**; line 107 reads "The six single-path invariants checked are:".
- (Copilot) `test_resend_budget.cpp:123` fixture sets goodput == rate limit so no test detects a regression to the cap basis — the fixture equality is **deliberate** (it preserves the pre-#52 arithmetic for the other cases), and the underlying concern is **addressed** by the dedicated `TracksGoodputNotCap` test added at line 212, which seeds goodput != cap specifically to catch that regression.
- (Copilot, OUTDATED) plan.md xfail item, `.agents/README.md:92` parameter table — both fixed in the Round-2 fix pass.
- (Copilot, OUTDATED) congestion predicate should use goodput — **deliberately rejected**, not missed. `sent_bps` aggregates all categories including resends, so the raw received/sent ratio is a true link-delivery measure and duplicate inflation cancels on both sides; swapping in goodput would declare congestion on a lossless link with heavy resending, and the error is self-reinforcing. Pinned by `CongestionDetectionUsesRawReceivedNotGoodput`.
