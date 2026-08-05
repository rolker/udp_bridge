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
