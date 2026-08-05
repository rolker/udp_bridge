---
issue: 44
---

# Issue #44 — Cap/back off resend bandwidth — resend storms self-amplify into degraded links

## Issue Review
**Status**: complete
**When**: 2026-08-05 00:00 +00:00
**By**: Claude Code Agent (Claude Sonnet)

**Issue**: #44
**Comment**: (best-effort post follows this entry; not recorded inline)
**Scope verdict**: well-scoped

### Scope Assessment

**What is proposed**: Two complementary mechanisms to prevent resend storms on
degraded links:
1. Budget resends as a bounded fraction of each connection's `maximum_bytes_per_second`
   (fresh data first; fraction configurable per connection).
2. Exponential backoff on the resend path when ack stream from a remote collapses
   (ack starvation → retrying harder cannot help).
3. Open design question: per-connection resend admission vs. fanning out to all
   connections (field data from 2026-08-04 supports per-connection scoping — storm
   resent on all three connections including the SIM-less cell path).

**Well-scoped?** Mostly yes. The rate-cap and backoff mechanisms address the same
root cause (unbounded retry load under degraded links) and can ship together. The
per-connection vs. global admission question is a design decision for plan-task to
resolve. Could optionally be split into two PRs (cap first, backoff second), but
joint delivery is defensible.

**Right repo?** Yes — behavior change lives in `rolker/udp_bridge`.

**Dependencies**:
- #36 (transport rework umbrella) — parent; no blocking dependency.
- #19 (priority scheduling) — governs what is shed once a resend budget binds;
  must be verified compatible or sequenced before this issue.
- #18 (bench harness) — regression gate ("assert resend rate stays bounded") depends
  on this harness existing; plan-task should assess whether to depend on #18 or add
  the gate inline.
- #21 (give-up counter semantics) — informational context from the same 08-04
  dataset; no blocking dependency.

### Principle Alignment

| Principle | Status | Notes |
|---|---|---|
| Human control and transparency | OK | Configurable fraction per connection; explicit behavior documented in field incidents |
| Capture decisions, not just implementations | Watch | Design choices (fraction value, per-connection vs. global, backoff parameters) should be recorded in an ADR or design note before merge |
| A change includes its consequences | Watch | New configurable parameters → README/API docs must land in same PR; #18 bench harness status needs confirming |
| Only what's needed | OK | Backed by two field incidents with specific measurements; no speculative scope |
| Improve incrementally | Watch | Two mechanisms combined; splitting (cap / backoff) is an option if PR becomes large |
| Test what breaks | OK | Issue explicitly calls out #18 bench harness and a regression gate; bench and degraded-link tests are the right target |
| Safety First (project) | OK | Resend storms degrade operator link; capping them is safety-positive |
| Modularity and Decoupling (project) | OK | Configurable per-connection limits align with community-reuse design goal |
| Iterative, Validated Evolution (project) | OK | Field validation already documented; bench harness provides pre-field gate |

### ADR Applicability

| ADR | Triggered | Notes |
|---|---|---|
| ADR-0001 — Adopt ADRs | Yes | Design decisions about resend fraction, backoff strategy, and per-connection vs. global admission should be captured in an ADR or equivalent record |
| ADR-0002 — Worktree isolation | N/A | Already in correct worktree (`feature/issue-44`) |
| ADR-0008 — Follow ROS 2 conventions | Yes (standard) | Package changes; follow ROS 2 parameter and interface conventions |

### Consequences

From the consequences map — changes to this issue will require:
- **Package parameters** (new `resend_budget_fraction`, backoff config): README/API docs
  must reflect them in the same PR; check whether
  `.agents/review-context.yaml` maps connection parameters.
- **Connection behavior change**: verify compatibility with #19 (priority scheduling)
  before or during implementation.
- **Bench harness (#18)**: if not yet merged, decide whether to land #18 first or
  embed the regression gate inline.

### Actions
- [ ] Record the resend-budgeting design decision in an ADR (fraction model, backoff strategy, per-connection vs. global admission) — before or in the same PR as implementation.
- [ ] Clarify ordering/compatibility with #19 (priority scheduling) — the issues intersect where a binding budget determines what gets shed.
- [ ] Update README/API docs for all new configurable parameters in the same PR.
- [ ] Confirm #18 bench harness status; if not available, add the regression test ("resend rate stays bounded under ack starvation") inline.

## Plan Authored
**Status**: complete
**When**: 2026-08-05 00:00 +00:00
**By**: Claude Code Agent (Claude Sonnet)

**Plan**: `.agent/work-plans/issue-44/plan.md` at `13f5680`
**Branch**: feature/issue-44 at `13f5680`
**Phases**: single

### Open questions
- [ ] No open questions — all operator checkpoint decisions resolved; plan is review-plan-ready.

## Plan Review
**Status**: complete
**When**: 2026-08-05 15:47 +00:00
**By**: Claude Opus

**Plan**: `.agent/work-plans/issue-44/plan.md` at `13f5680`
**PR**: PR-less (dispatched via `/review-plan` for issue #44; `gh` unauthenticated in this environment — issue/review-issue context read from this progress.md's prior entries)
**Verdict**: changes-requested

### Findings
- [ ] (must-fix) Resend-budget accounting reads the 0–10 s smoothed `data_sent_rate(resend).success_bytes_per_second` (`PacketSendStatistics::get`) and compares it to a per-second budget; once the stats deque spans >1 s the divisor grows and the value lags a burst, so a *sustained* storm (the documented until-restart failure mode) is under-shed. Reuse the existing 1 s-window pattern in `PacketSendStatistics::can_send` (sum only `resend`-category bytes with `timestamp >= now-1s`) — `plan.md:42-45`, `plan.md:74-79`
- [ ] (must-fix) `last_receive_time_ == 0.0` is the "no packet received yet" sentinel (`connection.h:72-76`); `starvation_steps = floor((now − last_receive_time)/threshold)` then yields a huge value → immediate max backoff (~1.6% floor) on a connection that has simply not received yet. Formula must treat the 0.0 sentinel as zero starvation (full budget) — `plan.md:40-41`
- [ ] (suggestion) Wiring mis-targeted: `udp_bridge.cpp:400` is a parameter *read* into a config struct (`connection.maximum_bytes_per_second`, `udp_bridge.cpp:404`), not a live `Connection` — there is no `connection->` there. `setRateLimit` is actually applied at `udp_bridge.cpp:1277` (CONNECT/adopt path), `:1805` and `:1830` (addRemote). To mirror `maximum_bytes_per_second`, thread the fraction through the same config-struct/`ConnectionInternal` path and apply it at all setter sites; the Consequences row `plan.md:118` ("no change needed") understates this — `plan.md:51-57`
- [ ] (suggestion) Test case 2 off-by-one: "2 × kAckStarvationThreshold ago" gives `floor(2/1)=2` steps → 4× reduction, not the "(one backoff step)" / 0.5× the note claims. The `≤ 0.5×` assert still passes but is loose; use ~1.x× threshold for exactly one step and pin the floor boundary to avoid FP flakiness — `plan.md:76-79`
- [ ] (suggestion) "Fresh data is never affected — only resend packets" overstates: the shared `can_send`/`reserved_bytes_in_flight_` still meters all categories together; the new budget leaves *headroom* for fresh data rather than strictly prioritizing it. Reword in the design note. Clock domain is fine — both `update_last_receive_time` (`remote_node.cpp:337`) and the `now` passed to `resend_packets` (`remote_node.cpp:263`) use `clock_`, so the subtraction is valid; note that dependency in the design note — `plan.md:46`

## Local Review (Pre-Push)
**Status**: complete
**When**: 2026-08-05 17:01 +00:00
**By**: Claude Code Agent (Claude Opus)
**Verdict**: approved

**Branch**: feature/issue-44 at `83b89da`
**Mode**: pre-push
**Depth**: Deep (reason: concurrency + lifecycle + network resend path)
**Must-fix**: 0 | **Suggestions**: 2
**Round**: 1 | **Ship**: recommended — no must-fix; both suggestions are safe-direction refinements

### Findings
- [ ] (suggestion) Probe-trickle floor not honored on low-rate links: when floored budget < one packet size, the resend loop drops everything (zero probe) — affects the ~25 kB/s / default-50000 links the design targets; force >=1 packet at the floor or correct the design-note claim — `udp_bridge/src/connection.cpp` (resend budget loop)
- [ ] (suggestion) NaN `resend_budget_fraction` clamps to 1.0 (most-permissive) instead of the default, contradicting the setter's "nearest sane bound" intent; add an isnan guard mapping to kDefaultResendBudgetFraction — `udp_bridge/src/connection.cpp:76`

Static analysis: cppcheck clean; cpplint findings are pervasive house-style (package has no ament_lint), dropped. Governance: all principles Pass; ADR-0001/0008 compliant (design note serves as the decision record). Plan adherence: close; both plan-review must-fixes (1s-window accounting, 0.0 sentinel) resolved in code. Concurrency/lifecycle verified safe by Lens B (MutuallyExclusive callback group, no lock nesting, caller releases state_mutex_ before dispatch). Local Adversarial skipped (Ollama unavailable); Copilot off (default).
