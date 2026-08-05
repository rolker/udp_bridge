---
issue: 35
---

# Issue #35 — Reorder/jitter buffer for out-of-order packets (instead of hard-drop)

## Issue Review
**Status**: complete
**When**: 2026-08-05 00:00 +00:00
**By**: Claude Code Agent (Claude Sonnet)

**Issue**: #35
**Comment**: (best-effort post follows this entry; not recorded inline)
**Scope verdict**: well-scoped

### Actions
- [ ] Design decision needed before plan-task: timer-based hold vs. poll-on-arrival reorder buffer — the choice has significant implications for the socket-drain hot path; document the rationale (ADR or issue note) before implementation begins.
- [ ] Confirm threading plan: the reorder buffer must not block the socket-drain callback group; verify it integrates with `PublishQueue` or runs in a safe callback group.
- [ ] Plan for timer granularity: the socket-drain spin_once cycle is 10 ms — a "few ms" hold window needs sub-cycle resolution; clarify how the timer fires (rclcpp wall timer in periodic_group, or inline age check on each drain tick).
- [ ] Note dependency risk with #34: if both issues are developed concurrently, `admitForPublish` is touched by both; sequence or co-develop explicitly.
- [ ] New parameters (hold window per topic, buffer size bound) must follow the existing `remotes.<r>.topics.<t>...` nesting pattern and use `declareIfMissing` in `on_configure`; confirm naming in plan.
- [ ] Consequences: new parameters must land in `.agents/README.md` Key Parameters table and `config/example_params.yaml` in the same PR.
- [ ] Tests must cover window boundary timing (packet arrives just before vs. just after hold window expires); note that real-time timing tests are sensitive — consider injecting a clock or using fake time via rclcpp test utilities.
