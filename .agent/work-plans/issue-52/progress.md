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
- [ ] (must-fix) negative admission floor clamps to 0, disabling the lockout floor and contradicting the header contract ("negative/NaN fall back to default") — `udp_bridge/src/connection.cpp:117` (vs `connection.h:53`)
- [ ] (must-fix) doc/comment staleness cluster still describes the old cap basis after the goodput move — `doc/resend_budget_design.md:92` (internal contradiction), `include/udp_bridge/connection.h:42-47` & `247-251`, `include/udp_bridge/resend_constants.h:83-99`, `test/bench/README.md:55,106` ("five" invariants; now six)
- [ ] (suggestion) congestion DETECTION uses raw received, not goodput — `udp_bridge/src/connection.cpp:195-197`. DOWNGRADED from the prior review's must-fix: the predicate is a ratio and `sent_bps` already includes our resends, so duplicate inflation cancels; the naive goodput-swap fix would cause false congestion. Add a clarifying comment/test instead.
- [ ] (suggestion) resend-budget unit tests set goodput==rate_limit, so no unit test detects a regression to the old cap basis — `udp_bridge/test/test_resend_budget.cpp:119-123`
- [ ] (suggestion) resend budget basis is raw goodput with no sanity ceiling; a crafted remote inflates the authorized burst (bounded only by can_send) — `udp_bridge/src/connection.cpp:706`
- [ ] (suggestion) non-finite (NaN) remote feedback defeats AIMD decrease (NaN comparison false → treated clean) — `udp_bridge/src/connection.cpp:195-203`
- [ ] (suggestion) co-tenant bench merges both visits of repeated phases (delivered/expected +=) and never asserts p95 latency — `udp_bridge/test/bench/test_range_degradation.py:546-551,587`
- [ ] (suggestion) headroom target on the congested branch can ratchet down within sustained congestion (bounded by floor) — `udp_bridge/src/connection.cpp:214-229`
- [ ] (suggestion) plan drift: plan item 7 mandates un-xfailing test_invariant_resend_amplification, kept xfail(strict) for the documented residual; "Stacking" open question obsolete post-rebase — `udp_bridge/.agent/work-plans/issue-52/plan.md:96-99,171`
