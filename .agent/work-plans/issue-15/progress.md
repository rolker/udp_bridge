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
- [ ] **N1/N2 (doc wording)** — Reword two comment sites that came out of the prior doc sweep and inadvertently mislead:
  - `udp_bridge_node.cpp:13-14` says the operational RELIABLE policy "bounds this exposure" to the socket-drain-wedge concern. RELIABLE actually *aggravates* the wedge per `qos_design.md`; the RELIABLE default is a graph-visibility workaround we're accepting alongside, not because of, the wedge. Replace "bounds this exposure" with language that says the exposure is currently accepted.
  - `qos_design.md:249-251` says "no per-topic `reliability: reliable` overrides; CAMP-style mitigation is entirely subsumed by BEST_AVAILABLE matching" — directly contradicts the operational callout at lines 240-247 that names RELIABLE as the current default. Add a one-line clarifier distinguishing design intent from operational reality.
- [ ] **N3 (concurrent-send test)** — Add a focused multi-thread test for `Connection::send` rate-limit under contention. Spin N threads, all calling `Connection::send` on one Connection with bytes ≥ `maximum_bytes_per_second / N`; assert `sent_packet_statistics_.get().success_bytes_per_second ≤ data_rate_limit_`. New file `udp_bridge/test/test_connection_rate_limit.cpp`. Locks in the B1 invariant against future check-then-record regressions.
- [ ] **N4 (connection.cpp:182-189 comment)** — The "coherent snapshot" wording for the aggregate pre-check overstates its synchronization guarantee. On the success path the lock is released without reserving bytes, so concurrent callbacks can both proceed; "coherent snapshot" only holds in the drop case. Reword to make this explicit and identify the per-packet inner `send()` as the sole atomic enforcement point.
- [ ] **N5 (udp_bridge.cpp:181-184)** — "the lock audit in PR #12 already protects shared state" — PR #12 missed the rate-limit TOCTOU; this PR's commit `1489cfb` is what made it complete. Amend to credit both audits.
- [ ] **N6 (udp_bridge_node.cpp:27-33)** — Same as N5: the invariant block implies #12's audit covered everything; needs to acknowledge this PR's `sent_packet_statistics_mutex_` atomicity fix.
