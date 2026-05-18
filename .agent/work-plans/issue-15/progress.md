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
- [x] **P1 (blocking sendto under mutex)** — My B1 fix at `connection.cpp:269` holds `sent_packet_statistics_mutex_` across `sendto()` on the bridge socket. The rationale comment claimed the socket was non-blocking but it isn't — only `SO_RCVTIMEO` is set. Passed `MSG_DONTWAIT` to the `sendto()` call so the existing `EAGAIN` retry path handles buffer-full state; worst-case mutex hold is now bounded by the poll loop's ~200 ms budget. Rationale comment at `connection.cpp:259-273` rewritten to describe the actual mechanism. **→ commit 6e9ffdd**
- [x] **P2 (test: rclcpp shutdown)** — Added custom `main()` to `test_connection_rate_limit.cpp` matching `test_qos_matching_integration.cpp:158-165`. **→ commit d34a0eb**
- [x] **P3 (test: missing includes)** — Added explicit `<cerrno>` and `<cstring>` includes. **→ commit d34a0eb**

## External Review
**Status**: complete
**When**: 2026-05-18 18:55
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))

**PR**: #16 — 4 review(s) total (4th fresh Copilot @ `efec6c8`), 3 fresh inline comments, 3 valid, 0 false positives
**CI**: all-pass (4 checks)

### Actions
- [ ] **Q1 (regression from P1)** — `MSG_DONTWAIT` activated a previously-unreached EAGAIN path that's structurally broken. `case EAGAIN: tries+=1; break;` exits the switch and falls through to `if(bytes_sent < data.size())` — but `bytes_sent == -1` (signed int) vs `data.size()` (size_t) promotes `-1` to ~UINT_MAX, comparison is **false**, success-recording else branch runs. **Result: under buffer pressure, the code records SUCCESS for a packet that wasn't sent.** Fix: replace `break;` with `continue;` in the EAGAIN case so the outer `while(true)` restarts. Also defensively guard the `bytes_sent < data.size()` comparison so a `-1` can't be mistaken for "all sent".
- [ ] **Q2 (wedge reintroduction via sent_packet_statistics_mutex_)** — Holding `sent_packet_statistics_mutex_` across the poll/sendto loop (up to ~200 ms) can stall `sendBridgeInfo` (`udp_bridge.cpp:1220`) which calls `data_sent_rate()` while holding `subscribers_mutex_` + `remote_nodes_mutex_` (`:1233`). The blocked `sendBridgeInfo` then stalls the socket-drain path's `remote_nodes_mutex_` lookups (`:425/554/680/698/849/898/928`) — same mechanism as the #10 wedge. Fix: reserve-then-record pattern. Add `uint32_t reserved_bytes_in_flight_` to `Connection` (guarded by `sent_packet_statistics_mutex_`); extend `PacketSendStatistics::can_send` to include reserved bytes in its sum. Under the mutex briefly: check can_send including reserved, then increment reservation OR record dropped + return. Release mutex. Do sendto outside the mutex. Re-acquire mutex briefly: decrement reservation, add real record. RAII guard for throw-safety. Mutex hold becomes microseconds, not 200 ms.
- [ ] **Q3 (test socket fd leak under ASSERT)** — `test_connection_rate_limit.cpp` owns raw socket fds and closes them only at the happy-path end. If `send_sock` creation fails after `listener_sock` opened, the listener fd leaks. Wrap each in a tiny RAII helper.
