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

## Plan Authored
**Status**: complete
**When**: 2026-08-05 21:49 +00:00
**By**: Claude Code Agent (Claude Sonnet)

**Plan**: `.agent/work-plans/issue-35/plan.md` at `5f23dc5`
**Branch**: feature/issue-35 at `5f23dc5`
**Phases**: single

### Open questions
- [ ] Clamp `reorder_hold_window_ms` to 0–500 ms in `on_configure` to prevent large accidental values causing latency regressions?
- [ ] At-most-one buffer slot per topic covers the primary use case; multi-packet reorder depth is a follow-up if field observations warrant it.

## Plan Review
**Status**: complete
**When**: 2026-08-05 21:56 +00:00
**By**: Claude Code Agent (Claude Opus)

**Plan**: `.agent/work-plans/issue-35/plan.md` at `5f23dc5`
**PR**: PR-less (--issue mode; independent fresh-context dispatch — not author self-review despite shared `Claude Code Agent` name, since the plan was authored by a different model/dispatch)
**Verdict**: approve-with-suggestions

### Findings
- [ ] (must-fix) Steps 3 and 6 disagree on where `flushExpiredBuffer` is hooked: step 3 says `UDPBridge::decodeData` "on every socket-drain tick", but `decodeData` is per-decoded-packet and does not run when the link is idle. Only `spin_once` (`udp_bridge.cpp:595`) runs every 10 ms. Commit to `spin_once` so a buffered packet on a briefly-idle topic ages out on schedule instead of stalling until the next arriving packet — `plan.md:42-46`
- [ ] (suggestion) Flush must iterate all remotes: the buffer is per-`RemoteNode`, and `spin_once` already snapshots `remote_nodes_` and loops them for defragmenter cleanup (`udp_bridge.cpp:638-652`). Extend that existing per-remote loop to call `flushExpiredBuffer(now)` and push returned items to `PublishQueue`, rather than "a call at the top of the drain callback" — `plan.md:66,89`
- [ ] (suggestion) PublishItem construction ordering: the stale gate runs at `udp_bridge.cpp:855` *before* the payload copy / `PublishItem` build (`udp_bridge.cpp:881+`). A `Buffer` decision must store a `PublishItem`, forcing the payload copy before the admit call on the enabled path — the "gate before payload copy costs nothing" optimization (comment at L850) no longer holds there. State where the item is built on the buffer path and update that comment — `plan.md:36-40,59-66`
- [ ] (suggestion) Injected clock / method signatures: `RemoteNode::clock_` is the real node clock (`remote_node.cpp:15`), not injectable. The existing test pattern uses explicit `rclcpp::Time now` params (`getMissingPacketsAt(rclcpp::Time)` + `t_at()`/`kClock`). `admitOrBuffer`/`flushExpiredBuffer` should take an injected `now` (prod passes `clock_->now()`, tests pass fake time) and each buffer entry must store an arrival timestamp — steps 2-3 omit the time parameter — `plan.md:36-46`
- [ ] (suggestion) gtest count baseline is stale: `.agents/README.md` says "13 gtest targets" but lists 12 names, while `CMakeLists.txt` registers 14 (`test_resend_budget`, `test_admission_control` from #43/#44 were never added to the README). The plan's "12 → 13" propagates the wrong baseline; set the count/list to the true value (14 → 15, adding the two missing names) — `plan.md:124,128`
- [ ] (suggestion) Parameter nesting deviates from review-issue action #5 (which asked for `remotes.<r>.topics.<t>...` nesting). Making `reorder_hold_window_ms` global is defensible (matches `drop_stale_packets`/`maximum_packet_size` precedent; per-topic deferred to #34), but note the deviation explicitly so it reads as a recorded decision, not an omission — `plan.md:50-53`
