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
- [x] **Q1 (regression from P1)** — Replaced switch with if/else chain so EAGAIN/EWOULDBLOCK bump tries and fall through to the budget check. Post-sendto comparisons are `else if`-chained off `if (bytes_sent == -1)`, so they only run on a real syscall success. Also cast `bytes_sent` to `size_t` in the partial-send comparison. **→ commit 1e174c4**
- [x] **Q2 (wedge reintroduction via sent_packet_statistics_mutex_)** — Switched to reserve-then-record pattern. Added `Connection::reserved_bytes_in_flight_`; extended `PacketSendStatistics::can_send` to take a `reserved_bytes` parameter. Under-mutex steps (check + reserve, then add + release) are microseconds; sendto runs outside the mutex. RAII `ReservationGuard` handles throw-paths. Rationale comments at `connection.cpp:266-294`, `udp_bridge.cpp:181-190`, `udp_bridge_node.cpp:25-44` rewritten to describe the new contract. **→ commit 0c8b75f**
- [x] **Q3 (test socket fd leak under ASSERT)** — Added local `SocketFd` RAII helper in `test_connection_rate_limit.cpp` anonymous namespace; replaced raw fds and trailing close() calls. **→ commit dc5dd90**

## External Review
**Status**: complete
**When**: 2026-05-18 19:30
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))

**PR**: #16 — 5 review(s) total (5th fresh Copilot @ `ee788cc`), 3 fresh inline comments, 3 valid, 0 false positives
**CI**: all-pass (4 checks)

### Actions
- [x] **R1 (Q2 broke can_send's monotone-order assumption)** — Replaced the skip-prefix walk with a per-entry timestamp filter over the whole deque. Bounded to ~10 s of records by Statistics::add eviction; correct regardless of insertion order. **→ commit 47c7106**
- [x] **R2 (test passes on drop-everything regression)** — Added `EXPECT_GT(success, 0)` and `EXPECT_GE(success, 0.5 * cap)` to `ConcurrentSendsStayUnderLimit`. Test now catches both under-throttle (already covered) and over-throttle (new) regressions. **→ commit 47c7106**
- [x] **R3 (Q2 comment misrepresents call-site behavior)** — Reworded `connection.cpp:288-300` to acknowledge no caller catches `ConnectionException` (throw terminates the node via the executor); reframed `ReservationGuard`'s value as consistent accounting before process death rather than reporting delegation. Pre-existing throw behavior unchanged; follow-up to add a catch at the call site is noted. **→ commit 47c7106**

## External Review
**Status**: complete
**When**: 2026-05-18 20:05
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))

**PR**: #16 — 6 review(s) total (6th fresh Copilot @ `6f132a3`), 2 fresh inline comments, 2 valid, 0 false positives
**CI**: all-pass (4 checks)

### Actions
- [x] **S1 (test file describes superseded contract)** — Rewrote `test_connection_rate_limit.cpp` header (lines 1-23) to walk through both stages of the concurrency-fix evolution (`1489cfb` closed the TOCTOU; `0c8b75f` moved sendto outside the mutex via reservations). Assertion-failure message at lines 174-177 now points at the reservation contract (`reserved_bytes_in_flight_` visible to `can_send`). **→ commit 901a766**
- [x] **S2 (PR body stale on locking design + commit list)** — Updated via `gh api PATCH`. Body now lists all post-`2b7241a` commits organized by theme; explicitly marks `0c8b75f` as superseding `1489cfb`'s locking design; updates "Risks to verify" to reflect the post-Q2 final shape; updates test count to 34. **→ PATCH on `repos/rolker/udp_bridge/pulls/16`**
