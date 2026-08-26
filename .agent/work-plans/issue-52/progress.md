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
