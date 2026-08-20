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

## Implementation
**Status**: complete
**When**: 2026-08-05 22:13 +00:00
**By**: Claude Code Agent (Claude Opus)

**Issue**: #35
**Branch**: feature/issue-35 at `b091f71`

### Summary
Implemented the reorder/jitter buffer for out-of-order packets, an opt-in
extension of the `drop_stale_packets` gate. All of the Plan Review's 1
must-fix + 5 suggestions were incorporated.

- **`RemoteNode::admitOrBuffer`** (new): 3-way decision (`Admit` / `Drop` /
  `Buffer`) returning an `AdmitResult` = decision + a `to_publish` vector in
  wire order. A gap (`packet_number >= high_water + 2`) buffers the packet
  **without advancing the high-water mark**; at most one held packet per
  topic; a gap-filler is admitted ahead of the released held packet; a newer
  arrival flushes the held one (overflow). `admitForPublish` is reused as the
  admit+advance primitive and its signature is unchanged.
- **`RemoteNode::flushExpiredBuffer(now, hold_window)`** (new): releases held
  packets whose injected-clock age reaches the window, advancing the mark to
  each. Hooked in **`UDPBridge::spin_once`** (must-fix) — extended the
  existing per-remote defragmenter-cleanup loop (`udp_bridge.cpp:638-652`) to
  call it every 10 ms and push released items to `PublishQueue`, so held
  packets age out even on an idle link.
- Both new methods take an **explicit injected `rclcpp::Time now`** (prod
  `clock_->now()`, tests fake time); each buffer entry stores its arrival
  timestamp. `reorder_buffer_` cleared on remote-restart in `update(BridgeInfo)`.
- **`decodeData`**: `PublishItem` build factored into a lambda so the reorder
  path builds it **before** the admit call (a `Buffer` stores it); the
  "gate before payload copy costs nothing" comment updated. Disabled path
  (window `0.0`, or gate off) is byte-for-byte the pre-#35 fast path.
- **`reorder_hold_window_ms`** parameter: global `double`, default `0.0`
  (off), `declareIfMissing` in `on_configure`, clamped 0–500 ms with a WARN.
  Recorded as a deliberate deviation from Issue Review #5's per-topic nesting
  (plan.md + README + example_params).

### Tests
- New `test/test_reorder_buffer.cpp` — 12 gtest cases, injected fake time, no
  real-time sleeps. Covers every scenario in plan step 7 plus partial fill,
  contiguous overflow, stale-below-mark, and per-topic independence.
- Full suite: `cd core_ws && ./build.sh udp_bridge_interfaces udp_bridge`
  then `./test.sh udp_bridge` → **140 tests, 0 errors, 0 failures, 0 skipped**
  (was 128; +12 new). Verified on the committed tree at `b091f71`.

### Deviations from plan
- None beyond the Plan Review items, which were all folded in and the plan
  was updated inline to match (commit `b091f71`): flush in `spin_once` not
  `decodeData`; injected `now`; `AdmitResult.to_publish`; gtest count
  corrected to 14 → 15 (the README baseline was stale — said 13, listed 12,
  CMakeLists had 14); global-param and 0–500 ms clamp recorded as decisions.

### Commits (feature/issue-35, not pushed)
- `f21dd44` remote_node: reorder buffer core (admitOrBuffer, flushExpiredBuffer, restart clear)
- `50cd62b` udp_bridge: decodeData/spin_once integration + reorder_hold_window_ms param + docs
- `fd33e5b` test: test_reorder_buffer suite + stale gtest-inventory fix
- `b091f71` plan: sync plan.md with committed code

## Local Review (Pre-Push)
**Status**: complete
**When**: 2026-08-06 00:28 +00:00
**By**: Claude Code Agent (Claude Opus)
**Verdict**: approved

**Branch**: feature/issue-35 at `55760e6`
**Mode**: pre-push
**Depth**: Deep (reason: concurrency + networking on the socket-drain hot path, plus lifecycle/remote-restart interaction)
**Must-fix**: 0 | **Suggestions**: 3
**Round**: 1 | **Ship**: recommended — no must-fix findings; clean, well-tested, opt-in change; disabled default path is byte-for-byte pre-#35.

### Findings
- [ ] (suggestion) `admitOrBuffer` decision-rule docstring omits the equal-to-mark `P==H` admit case (code covers it inline) — `udp_bridge/include/udp_bridge/remote_node.h:236`
- [ ] (suggestion) No occupancy diagnostic for `reorder_buffer_` (bounded ≤1/topic, ≤500ms; observability nice-to-have) — `udp_bridge/include/udp_bridge/remote_node.h:369`
- [ ] (suggestion) `reorder_buffer_` not cleared on `on_deactivate`; a held packet would release as a slightly-stale publish on next activation (consistent with existing cross-activation state persistence — noting for an explicit decision) — `udp_bridge/src/udp_bridge.cpp:574`

### Notes
- Two Claude Adversarial passes (Lens A logic/correctness, Lens B systemic/safety). Lens A: no bugs. Lens B raised 6 candidates; all verified as false-positive or out-of-scope:
  - race on `reorder_hold_window_ms_`: written only in `on_configure` (166/174/182); `on_set_parameters` handles only the giveup-rate params → no ACTIVE-time write.
  - flush while INACTIVE: `spin_once` early-returns when state != ACTIVE (631-637) before the flush loop.
  - held packets starve resend: resend uses `received_packet_times_`, independent of the frozen publish high-water mark; `getMissingPackets` still requests the gap.
  - `reorder_buffer_` unbounded growth: self-draining (erased on publish or ≤500ms flush), at-most-one/topic; arbitrary-topic per-topic state is pre-existing (`publishers_`, `#33` high-water map), not introduced here.
  - `highest_published_packet_number_` growth: pre-existing from #33, not in this diff.
- Static analysis: package registers no `ament_lint` hooks; ~234 vanilla cpplint items are unenforced style consistent with the codebase (siblings carry no copyright header). cppcheck's 3 hits are false positives (RCLCPP `<<` as shift; `TEST_F` unparseable; one pre-existing perf nit outside the diff). Added lines have no trailing whitespace/CRLF → pre-commit/CI clean.
- Governance: consequences satisfied (README gtest count 15 == 15 real targets; example_params updated; restart-clear + test present). Plan drift: none (files match plan; 12 gtest cases ⊇ 6 planned scenarios).
- Local model adversarial skipped: Ollama not installed on this host.
- Test attestation from the Implementation entry (140 tests, 0 failures at b091f71) relied upon; this review did not rebuild.

## Integrated Review
**Status**: complete
**When**: 2026-08-20 13:53 -04:00
**By**: Claude Code Agent (Claude Fable 5)

**PR**: #48 at `d9c2c28`
**Sources**: 3 (Copilot R1 @ `d9c2c28`, Local Review (Pre-Push) @ `55760e6`, CI rollup)
**Cross-source confirmations**: 1
**CI**: all-pass (build-and-test success)

### Findings
- [ ] (cross-confirmed: Copilot + Local Review (Pre-Push)) `admitOrBuffer` decision-rule docstring omits the `P == H` admit case — the implementation admits `packet_number <= H+1` (remote_node.cpp:459, inline comment covers it) but the header contract lists only `first-seen / P==H+1 -> Admit`. Doc-only fix: add `P == H` to the Admit rule line — `udp_bridge/include/udp_bridge/remote_node.h:237`
- [ ] (suggestion, Local Review (Pre-Push)) No occupancy diagnostic for `reorder_buffer_` (bounded ≤1/topic, ≤500 ms; observability nice-to-have) — `udp_bridge/include/udp_bridge/remote_node.h:369`
- [ ] (suggestion, Local Review (Pre-Push)) `reorder_buffer_` not cleared on `on_deactivate`; a held packet would release as a slightly-stale publish on next activation (consistent with existing cross-activation state persistence — needs an explicit keep/clear decision) — `udp_bridge/src/udp_bridge.cpp:574`

### False positives
- (none)

## Implementation
**Status**: complete
**When**: 2026-08-20 14:05 -04:00
**By**: Claude Code Agent (Claude Fable 5)

**Branch**: feature/issue-35 (addresses all three Integrated Review findings for PR #48)

- `7881db7` docs: document the P==H admit case in admitOrBuffer decision rules (cross-confirmed finding)
- `f6549ee` diagnostics: per-remote reorder-buffer occupancy + cumulative counters (buffered_total / expired_released_total; task registered only when the buffer is enabled) + new ObservabilityCountersTrackBufferAndExpiry test + .agents/README.md diagnostics inventory update
- `87b8b2f` lifecycle: clear the reorder buffer on deactivate (decision: discard held packets, keep high-water marks — matches existing cross-activation state behavior) + new ClearReorderBufferDiscardsHeldKeepsMark test

Verified: package builds clean (no new warnings) and full suite passes — 142 tests, 0 failures (was 140 before the two new tests).
