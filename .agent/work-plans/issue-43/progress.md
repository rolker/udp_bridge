---
issue: 43
---

# Issue #43 — Adaptive per-connection rate control — back off when a link's delivered capacity drops

## Issue Review
**Status**: complete
**When**: 2026-08-05 00:00 +00:00
**By**: Claude Code Agent (Claude Sonnet)

**Issue**: #43
**Comment**: (best-effort post follows this entry; not recorded inline)
**Scope verdict**: well-scoped

### Summary

Issue #43 proposes AIMD-style congestion control on total admission per connection: when a
link's delivered capacity drops (detected via congestion signals), scale `data_rate_limit_`
— or an effective cap derived from it — down, and recover additively when clean.

This is the *total-admission* counterpart to issue #44 (merged), which bounds the *resend
category only*. The post-#44 codebase provides `bytes_in_window()`, the ack-starvation
backoff infrastructure, and `doc/resend_budget_design.md` as the design-doc precedent — all
of which reduce the scope of #43.

The issue lists three candidate congestion signals (ack rate/latency, delivered-vs-sent
ratio, resend-request rate) without committing to one. The implementation must pick one
signal; that choice and the AIMD parameters need to be captured in a design doc
(analogous to `doc/resend_budget_design.md`).

### Scope

**Right repo?** Yes — `Connection` class lives in udp_bridge.

**Dependencies:**
- #44 (merged ✓) — `bytes_in_window()`, ack-starvation backoff, `config_mutex_` plumbing
  already in place; prerequisite is satisfied.
- #19 (open) — per-topic priority determines *what* to shed; #43 determines *how much* to
  admit. These are complements, not blockers; #43 can land independently.
- #18 (open) — the bench harness is the intended end-to-end verification vehicle.
  If #18 is not ready when #43 lands, unit tests should carry the invariant (following #44's
  pattern with `test/test_resend_budget.cpp`).
- #36 (umbrella) — context only.

### Principle Alignment

| Principle | Status | Notes |
|---|---|---|
| Human control and transparency | Watch | AIMD internal state (effective rate, backoff level) should be visible in BridgeInfo/DataRates telemetry so operators can correlate link-quality events with bridge behaviour. The #44 precedent is `SendResult::dropped` in per-connection DataRates for shed traffic — apply the same pattern here. |
| Capture decisions, not just implementations | Action needed | Three candidate signals listed; the design choice (signal type, AIMD constants, wiring path) must be recorded in a `doc/admission_control_design.md` (or equivalent) in the same PR, analogous to `doc/resend_budget_design.md`. |
| Only what's needed | OK | Tightly motivated by two concrete field incidents (2026-05-01 #124, 2026-08-04 #411) and the known RF instability of the SXTsq link. |
| Test what breaks | Watch | Bench harness (#18) is the end-to-end gate but may not be ready. Follow #44's pattern: unit tests cover AIMD reduction steps, recovery, and steady-state before #18 lands. |
| Improve incrementally | OK | Logical step 2 after #44; resend budget infrastructure makes this PR smaller than it would otherwise be. |
| A change includes its consequences | Action needed | New config parameters (AIMD step sizes, window, floor) must follow the same wiring pattern as `resend_budget_fraction_` (per-connection file config, `on_configure`, no `AddRemote.srv` extension required initially). Package README/API docs and `.agents/review-context.yaml` must reflect any new params in the same PR. |

### ADR Applicability

| ADR | Triggered | Notes |
|---|---|---|
| ADR-0001 (Adopt ADRs) | Yes | Design decision on congestion signal type and AIMD parameters must be recorded — a `doc/admission_control_design.md` file satisfies this; an ADR in `docs/decisions/` is not required unless the decision affects the workspace broadly. |
| ADR-0002 (Worktree isolation) | Yes | Worktree `issue-udp_bridge-43` already exists — satisfied. |
| ADR-0008 (ROS 2 conventions) | Yes | New `Connection` config parameters must follow ROS 2 param conventions; `resend_budget_fraction_` / `setResendBudgetFraction()` is the model. |
| ADR-0013 (progress.md vocabulary) | Yes | This `## Issue Review` entry. |

### Consequences

- New config parameters → update package README/API docs in the same PR.
- New effective-rate state → surface in `BridgeInfo` DataRates so field analysis tools see
  it (stale docs must land in the same PR per the AGENTS.md quality standard).
- Implementation surfaces a congestion-control pattern → propose a `.agent/knowledge/` note
  as a candidate for the operator to approve (never auto-applied).

### Actions
- [ ] Pick one congestion signal for this PR (recommend starting with `last_receive_time` +
  `data_receive_rate` comparison against offered rate — least invasive, no new receiver-side
  protocol) and record the choice with rationale in `doc/admission_control_design.md`.
- [ ] Make the effective admission rate visible in BridgeInfo/DataRates telemetry (same
  pattern as #44's shed-traffic accounting) so operators can observe backoff events.
- [ ] Write unit tests covering AIMD reduction steps, floor/ceiling clamping, and additive
  recovery — do not wait for bench harness #18.
- [ ] Update package README and any `.agents/review-context.yaml` param table for new
  config parameters in the same PR.
- [ ] Verify new config parameter wiring follows the `resend_budget_fraction_` / `on_configure`
  pattern (per-connection file config only; no `AddRemote.srv` extension unless a caller
  needs it).

## Plan Authored
**Status**: complete
**When**: 2026-08-05 18:43 +00:00
**By**: Claude Code Agent (Claude Sonnet)

**Plan**: `.agent/work-plans/issue-43/plan.md` at `48b422d`
**Branch**: feature/issue-43 at `48b422d`
**Phases**: single

### Open questions
- [ ] No open questions — plan is review-plan-ready. Operator checkpoint (2026-08-05) resolved all five open actions from the issue review.

## Plan Review
**Status**: complete
**When**: 2026-08-05 18:50 +00:00
**By**: Claude Code Agent (Claude Opus)

**Plan**: `.agent/work-plans/issue-43/plan.md` at `48b422d`
**PR**: PR-less (`--issue` / file-path mode; feature/issue-43 worktree)
**Verdict**: approve-with-suggestions

Plan is sound, well-scoped (10 tightly-coupled files, mirrors #44's PR shape),
and addresses all five issue-review actions: signal choice + design doc (1),
`effective_rate_limit` telemetry field (2), 7 AIMD unit tests (3), README/param
docs (4 — see finding 4), and `resend_budget_fraction`-pattern config wiring (5).
Verified against source: `data_sent_rate()` exposes `success_bytes_per_second`;
`connection_info.received_bytes_per_second` in `update(BridgeInfo)` genuinely is
"bytes the remote received from us on this connection" (udp_bridge.cpp:1732-1733);
both `send()` overloads meter on `data_rate_limit_` today (connection.cpp:203,281);
`setRateLimit` callers are config/CONNECT/addRemote only, not steady-state periodic.
Suggestions below are refinements for the design doc (step 7) + one doc file; none
block implementation.

### Findings
- [ ] (suggestion) Stale-feedback fallback is only reachable in the multi-connection redundancy case. `updateAdmissionControl` is called *only* on BridgeInfo receipt (step 3), and BridgeInfo arrival refreshes `last_receive_time` via `unwrap()` (remote_node.cpp:337). So on the connection that delivered the BridgeInfo the fallback is never stale; it can only fire for *other* connections the remote still lists. A single connection in total inbound collapse (no BridgeInfo at all) gets no AIMD decrease from either path. The primary field scenario (congestion, not blackout) is covered because BridgeInfo keeps flowing under congestion — but the design doc must state this scope boundary honestly — `plan.md:44` / step 7
- [ ] (suggestion) `setRateLimit()` resetting `effective_rate_limit_` wipes accumulated AIMD backoff whenever CONNECT/adopt (udp_bridge.cpp:1292) or addRemote re-applies a rate limit — even an unchanged one. Harmless in steady state (not periodic), but a flapping link that re-sends CONNECT would reset the cap to full and force re-backoff each reconnect. Consider gating the reset on an actual value change (as `setHostAndPort` does) or clamping to `min(effective, new_limit)`; document the choice — `plan.md:36` / step 2
- [ ] (suggestion) Congestion signal is an approximation, not an exact loss measure: `received_bytes_per_second` includes duplicate bytes (`receive_rates.first + receive_rates.second`, udp_bridge.cpp:1733) and our `success_bytes_per_second` includes resend traffic, so both sides inflate under loss+resend; the two rates also use different smoothing windows (our ~0-10s `get()` vs the remote's 5s `data_receive_rate`) plus BridgeInfo propagation delay. Fine for a trend-based AIMD trigger — record these caveats in the design doc so a future reader doesn't treat the ratio as precise — `plan.md:17` / step 7
- [ ] (suggestion) Consequences/docs gap: the agent-facing `.agents/README.md` "Key Parameters (verified in on_configure)" table (line 91) enumerates per-connection params but is missing the new `admission_floor_fraction` — and is in fact already stale for `resend_budget_fraction` (#44 never added it). Plan updates `README.md` + `example_params.yaml` but omits `.agents/README.md`. Add the param there (and optionally backfill `resend_budget_fraction`) — `plan.md:90-104`
- [ ] (note) Preserve lock ordering in `updateAdmissionControl`: read `data_sent_rate()` and `last_receive_time()` (each takes its own mutex) *before* acquiring `config_mutex_`, never nesting `sent_packet_statistics_mutex_` inside `config_mutex_`. `send()` acquires them sequentially (config then stats, non-nested); the plan's "two separate lock acquisitions" intent is correct — keep it — `plan.md:41`

### Next step
Lifecycle: **Plan Review** → **implement** → **review-code**. Verdict is
approve-with-suggestions: no must-fix blockers. Implementer should fold findings
1-3 into `doc/admission_control_design.md` (step 7), pick up finding 4's
`.agents/README.md` edit, and honor finding 5's lock-ordering note. Plan does not
need re-approval before implementation.

## Local Review (Pre-Push)
**Status**: complete
**When**: 2026-08-05 19:34 +00:00
**By**: Claude Code Agent (Claude Opus)
**Verdict**: approved

**Branch**: feature/issue-43 at `73f6e70`
**Mode**: pre-push
**Depth**: Deep (reason: RemoteConnection.msg interface change + Connection concurrency/lock-ordering + cross-layer telemetry)
**Must-fix**: 0 | **Suggestions**: 3
**Round**: 1 | **Ship**: recommended — no must-fix; both high-severity adversarial claims were verified false positives

### Findings
- [ ] (suggestion) Idle-link/stale-feedback nuance: `congested = feedback_stale || ...` can throttle a previously-active, now-idle link (sent_bps==0), contradicting design-doc "idle links cannot be congested" — guard feedback_stale with sent_bps>0 OR fix the doc — `src/connection.cpp:159` / `doc/admission_control_design.md:50`
- [ ] (suggestion) Field is inserted mid-message, not "appended" — fix wording — `udp_bridge_interfaces/msg/RemoteConnection.msg:34`
- [ ] (suggestion) Plan Consequences table stale vs shipped follow/clamp + resend-on-effective — `.agent/work-plans/issue-43/plan.md:164`

### Notes (verified false positives, not counted above)
- Lock-ordering "deadlock" (Lens B): config_mutex_ and sent_packet_statistics_mutex_ are acquired sequentially in separate `{}` scopes, never held nested — no cycle. Lens A concurred.
- NeverReceivedSentinel test does exercise the 0.0 guard (feedback_stale is the independent first || operand).
- resend_budget_design.md already updated to "admission cap".
- effective→0 truncation only at sub-10-B/s limits (below single-packet size) — below threshold.
- Static analysis (ament_cpplint + cppcheck): no PR-introduced findings; repo has no cpplint CI gate and pre-existing style nits on context lines only.
- Local Adversarial skipped: no Ollama server at localhost:11434.

## Integrated Review
**Status**: complete
**When**: 2026-08-05 16:05 -04:00
**By**: Claude Code Agent (Claude Fable 5)

**PR**: #47 at `0896bca` (wording fixes pushed as `5be82f2`)
**Sources**: 3 (Copilot review @ `0896bca`, Local Review (Pre-Push) @ round 1, CI rollup)
**Cross-source confirmations**: 0
**CI**: all-pass (build-and-test ✅)

### Findings
- [x] (minor, Copilot) README `admission_floor_fraction` entry omitted the stale-feedback backoff trigger — `udp_bridge/README.md:92`. Fixed in `5be82f2`.
- [x] (minor, Copilot) `updateAdmissionControl` locking comment (and the design-note echo) implied a lock-ordering constraint that doesn't exist — mutexes are never held simultaneously — `src/connection.cpp:135`. Reworded in `5be82f2`.
- [x] (minor, Copilot) Stale plan Consequences row said resend budget still uses `data_rate_limit_`, contradicting the shipped effective-cap base — `.agent/work-plans/issue-43/plan.md:166`. Row removed in `5be82f2`.
- [x] (minor, Copilot) on_configure wiring comment described only `resend_budget_fraction` though the block now applies both fractions — `src/udp_bridge.cpp:424`. Updated in `5be82f2`.

All four valid, all wording-only; no behavior change. 127/127 tests green after fixes.

### False positives
- (none)
