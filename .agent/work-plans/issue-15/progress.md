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
- [ ] **B1 (concurrency)** — `Connection::send` rate-limit check + statistics add are in separate `sent_packet_statistics_mutex_` critical sections (`connection.cpp:184-188` / `:257-261` vs `:303-304`). Under the new Reentrant `republish_group_` (`udp_bridge.cpp:186`), multiple forwarding callbacks can all observe capacity then collectively exceed `maximum_bytes_per_second`. Make the reserve atomic per connection.
- [ ] **Doc sync (H1+H2+H3+H4+B2+B4+B6)** — single commit covers:
  - `udp_bridge_node.cpp:3-35` callback-group invariant block (republish=Reentrant, drop BEST_AVAILABLE mention, document lock audit + 8-thread sizing)
  - `udp_bridge.h:207` `addSubscriberConnection` Doxygen reliability default
  - `types.h:27` `RemoteDetails` "package default" comment
  - `MessageInternal.msg:16` `reliability` field comment (not flagged by conversation review)
  - `qos_design.md` — operational-note callouts at §Reliability (line 107), §How configuration works (line 160), worked example (line 229)
- [ ] **PR body (H5/B5)** — rewrite risk #1: BEST_EFFORT subscriber + RELIABLE publisher is a valid match per ROS 2 QoS rules; the actual cost is the "transparency lie" on UDP-fed publishers.
- [ ] **B7 (suppressed but valid)** — `udp_bridge_node.cpp:45` comment claims "Tune via env if needed" but there is no env-var handling; executor threads is hard-coded `8`. Remove the sentence (simpler) or add env handling.
- [ ] **B3 (test gap)** — `test_qos_matching_integration.cpp` lacks coverage for (a) empty-default → RELIABLE matching against both subscriber types and (b) RELIABLE publisher → BEST_EFFORT subscriber matching. Acceptable to defer to a follow-up issue per H7's reasoning; flag explicitly if deferred.
