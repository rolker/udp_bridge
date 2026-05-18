---
issue: 15
---

# Issue #15 — Field import: udp_bridge (2026-05-01)

## External Review
**Status**: complete
**When**: 2026-05-18 14:30
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))

**PR**: #16 — 2 review(s) (1 Copilot bot, 1 conversation self-review), 13 findings, 13 valid, 0 false positives
**CI**: all-pass

### Actions
- [x] **B1 (concurrency)** — `Connection::send` rate-limit check + statistics add are in separate `sent_packet_statistics_mutex_` critical sections (`connection.cpp:184-188` / `:257-261` vs `:303-304`). Under the new Reentrant `republish_group_` (`udp_bridge.cpp:186`), multiple forwarding callbacks can all observe capacity then collectively exceed `maximum_bytes_per_second`. Make the reserve atomic per connection. **→ commit 1489cfb**
- [x] **Doc sync (H1+H2+H3+H4+B2+B4+B6)** — single commit covers:
  - `udp_bridge_node.cpp:3-35` callback-group invariant block (republish=Reentrant, drop BEST_AVAILABLE mention, document lock audit + 8-thread sizing)
  - `udp_bridge.h:207` `addSubscriberConnection` Doxygen reliability default
  - `types.h:27` `RemoteDetails` "package default" comment
  - `MessageInternal.msg:16` `reliability` field comment (not flagged by conversation review)
  - `qos_design.md` — operational-note callouts at §Reliability (line 107), §How configuration works (line 160), worked example (line 229)
  - **→ commit 2b7241a**
- [x] **PR body (H5/B5)** — rewrite risk #1: BEST_EFFORT subscriber + RELIABLE publisher is a valid match per ROS 2 QoS rules; the actual cost is the "transparency lie" on UDP-fed publishers. **→ PATCHed via `gh api` (gh pr edit blocked by classic-Projects deprecation)**
- [x] **B7 (suppressed but valid)** — `udp_bridge_node.cpp:45` comment claims "Tune via env if needed" but there is no env-var handling; executor threads is hard-coded `8`. Remove the sentence (simpler) or add env handling. **→ folded into commit 2b7241a (sentence removed)**
- [x] **B3 (test gap)** — `test_qos_matching_integration.cpp` lacks coverage for (a) empty-default → RELIABLE matching against both subscriber types and (b) RELIABLE publisher → BEST_EFFORT subscriber matching. Acceptable to defer to a follow-up issue per H7's reasoning; flag explicitly if deferred. **→ commit ef542ff (added 3 cases; 32 tests pass)**

## External Review
**Status**: complete
**When**: 2026-05-18 17:15
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))

**PR**: #16 — 2 review(s) (fresh Copilot @ `7728f6b`, plus stale prior round), 6 fresh inline comments, 6 valid, 0 false positives
**CI**: all-pass (4 checks)

### Actions
- [x] **N1/N2/N4/N5/N6 (doc/comment wording)** — Reworded five comment sites that the prior doc sweep had left misleading. Specifics:
  - `udp_bridge_node.cpp:13-14` — replaced "bounds this exposure" with language that attributes the wedge-bound to the BEST_AVAILABLE design intent and flags the current RELIABLE default as an accepted, separate risk.
  - `qos_design.md:249-251` — distinguished design-intent (BEST_AVAILABLE subsumes CAMP) from operational-reality (RELIABLE is the active default) scope of the "no overrides" claim.
  - `connection.cpp:182-189` — clarified that the aggregate pre-check's single lock covers only the over-budget path; on the success path two callbacks can both observe capacity and both proceed, and the per-packet inner `send()` is the sole atomic enforcement point.
  - `udp_bridge.cpp:181-184` and `udp_bridge_node.cpp:27-33` — both now credit PR #12's lock audit *and* PR #16's `sent_packet_statistics_mutex_` atomicity fix in `Connection::send`; describe the current `sent_packet_statistics_mutex_` contract ("check, sendto, and result-add all under one acquisition").
  - **→ commit 37a9903**
- [x] **N3 (concurrent-send test)** — Added `ConnectionRateLimit.ConcurrentSendsStayUnderLimit` in new `test/test_connection_rate_limit.cpp`. 8 threads × 200 sends contending on one rate-limited Connection (10 KB/sec), shared go-flag for max contention. Asserts `success_bytes_per_second < rate_limit` AND `dropped_bytes_per_second > 0` (so the test cannot silently pass on a degenerate setup). Locks in B1 against future check-then-record regression. 33 tests now pass; 0 failures.
  - **→ commit 0016362**

## External Review
**Status**: complete
**When**: 2026-05-18 18:15
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))

**PR**: #16 — 3 review(s) total (3rd fresh Copilot @ `d35badb`), 3 fresh inline comments, 3 valid, 0 false positives
**CI**: all-pass (4 checks)

### Actions
- [ ] **P1 (blocking sendto under mutex)** — My B1 fix at `connection.cpp:269` holds `sent_packet_statistics_mutex_` across `sendto()` on the bridge socket. The rationale comment claims the socket is non-blocking but it isn't — only `SO_RCVTIMEO` is set (`udp_bridge.cpp:159-162`), nothing makes the send-side non-blocking. UDP doesn't do partial writes, so if the kernel send buffer can't accept our full packet, `sendto()` blocks indefinitely, holding the mutex and stalling all same-Connection forwarding callbacks + stats readers. The `poll(POLLOUT, 10ms)` guard only confirms *some* space, not "enough for our packet". Fix: pass `MSG_DONTWAIT` to `sendto()` (`connection.cpp:293`); existing `EAGAIN` retry path handles the buffer-full case. Worst-case mutex hold becomes bounded by the 20-tries × 10ms poll budget (~200ms). Also update the rationale comment at `connection.cpp:259-268` to describe the actual non-blocking mechanism (`MSG_DONTWAIT` on the syscall) rather than claiming the socket is non-blocking.
- [ ] **P2 (test: rclcpp shutdown)** — `test_connection_rate_limit.cpp` initializes `rclcpp` in fixture `SetUp` but uses default gtest main, so `rclcpp::shutdown()` never runs. Match `test_qos_matching_integration.cpp:158-165` pattern: custom `main()` that calls `testing::InitGoogleTest`, `RUN_ALL_TESTS`, and `rclcpp::shutdown()` if `rclcpp::ok()`.
- [ ] **P3 (test: missing includes)** — `test_connection_rate_limit.cpp` uses `errno` and `strerror()` without explicit `<cerrno>` / `<cstring>` includes; compiles today via transitive pulls but should declare its dependencies. Add both.
