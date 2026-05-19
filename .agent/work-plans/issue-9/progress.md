---
issue: 9
---

# Issue #9 — Resend loop amplifies traffic rather than tracking loss

## Plan Review
**Status**: complete
**When**: 2026-05-18 13:00
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))

**Plan file**: `.agent/work-plans/PLAN_ISSUE-9.md` (legacy path; migration to `issue-9/plan.md` recommended during rebase)
**PR**: #13 (DRAFT)
**Verdict**: mostly ready; 3 substantive findings + 1 structural rebase step before implementation starts.

### Findings

1. **[Consequences] `Remote.msg` schema append — bag-replay implication missing.**
   Plan claims "appending is backward-compatible" but `BridgeInfo` is recorded
   in `bizzyboat.yaml` / `izzyboat.yaml` bag specs and ROS 2 Jazzy uses
   strict type-hash discovery. Old bags played against new code may emit
   type-hash warnings; mixed-version bridges may fail to discover each
   other on the `bridge_info` topic. Plan should add a consequences-table
   row documenting bag-replay behavior and the coordinated bridge-update
   expectation.

2. **[Consequences] Internal inconsistency: `BridgeInfo.resend_giveup_count` aggregate vs per-`Remote`.**
   Plan body and "Decisions made during planning" both place the field on
   `Remote.msg` (per-remote). But the Files-to-Change table says
   `udp_bridge.cpp` will "populate `BridgeInfo.resend_giveup_count` aggregate
   before publishing". `BridgeInfo` has `Remote[]` but no top-level
   aggregate field. Reword the table row to "populate each
   `Remote.resend_giveup_count` from the corresponding RemoteNode counter".

3. **[File targeting] `static_assert(receiver_window >= kSentPacketTTL)` risks tautology.**
   If both sites literally reference `kSentPacketTTL`, the assert collapses
   to `kSentPacketTTL >= kSentPacketTTL`. Either (a) introduce a separate
   `kReceiveHistoryWindow` constant defaulted to `kSentPacketTTL` and
   assert the relationship, or (b) drop the static_assert and rely on
   the single-constant structural enforcement (which the prose already
   argues is sufficient). Decide and document.

4. **[Scope, structural] Branch is 20 commits behind `origin/jazzy`.**
   Plan's "Approach" assumes only #11 lands first. Since the plan was
   written, #11, #12, #14, #15, and #16 have all merged. Rebase will need
   to absorb: executor split + Copilot fixes (#12), executor thread
   pinning (#14), test infra (#15), and the atomic rate-limit fix in
   `connection.cpp` (#16 / commit `1489cfb`). Expand the rebase note to
   "rebase onto current jazzy"; grep for residual `m_*` in touched files
   post-merge.

### Stale line-number references (informational, not blocking)

Plan file:line references drifted with #11 + #12. Implementer should
re-locate before editing:

| Plan reference | Current (jazzy) |
|---|---|
| `remote_node.cpp:170` (`getMissingPackets`) | `remote_node.cpp:200` |
| `remote_node.cpp:175` (receiver window 5.0s) | `remote_node.cpp:207` |
| `remote_node.cpp:177` (0.2s cooldown) | `remote_node.cpp:209` |
| `udp_bridge.cpp:742` (`cleanupSentPackets`) | `udp_bridge.cpp:1077-1082` |

### Recommended Actions (in order)

- [x] Rebase `feature/issue-9` onto current `origin/jazzy` (2026-05-18, post-PR-#16-merge). 4 plan commits replayed cleanly; no conflicts. Build + tests verified after cleaning the stale `udp_bridge_interfaces` install. `git grep "m_[a-z]"` in `udp_bridge/` returns zero hits — the rename completed during #11 / #12 / #16; no rename work remains for #9. Plan file migrated to `.agent/work-plans/issue-9/plan.md` via `git mv`.
- [x] Update the plan: bag-replay consequences row added; `BridgeInfo` aggregate wording fixed to per-`Remote` populate; static_assert resolved by introducing a separate `kReceiveHistoryWindow` constant (defaulted to `kSentPacketTTL`). Also refreshed stale line numbers in the Context block to current post-#16 lines. (Note: assert direction had `>=` here — corrected to `<=` in the round-3 pass below.)
- [x] **Third-round sub-agent review** (2026-05-18). An independent `general-purpose` sub-agent flagged 7 items, all valid; all absorbed inline. Highlights:
  - **Critical**: `static_assert` direction inverted (`>=` → `<=`). Failure mode D from the issue is "receiver > sender", so the bug-prevention invariant runs the other way. With both constants defaulted equal, the wrong direction passes today, but a future widening of `kReceiveHistoryWindow` would re-introduce the exact bug this plan is meant to close.
  - **Substantive**: third 5.0 s magic number at `udp_bridge.cpp:431` (defragmenter cleanup) added to Files-to-Change; `attempt_count_` map eviction wired into `clearReceivedPacketTimesBefore` so it doesn't leak under sustained loss; debounce formula pinned to `now - prev_neighbor_time > kResendDebounceHold` (avoiding the idle-stream pathology of `newest_received_time`).
  - **Polish**: spin-rate bundling invariant, `sendBridgeInfo` lock-interaction note, `/review-code` pre-push step.
- [x] **Implementation (4 commits, 2026-05-18)**:
  - Commit 1 (`abdb3b0`): D — TTL alignment + Remote.msg schema + plumbing
  - Commit 2 (`0eff305`): A — debounce in getMissingPackets
  - Commit 3 (`00f282d`): B — backoff + TTL give-up (using `ResendState` struct in place of the plan's `attempt_count_` map; semantically equivalent, cleaner data model)
  - Commit 4 (`74d53df`): tests + unreachable-give-up bug fix (test discovery surfaced that the give-up branch inside the backoff loop was unreachable; restructured into a give-up sweep at the top of `getMissingPackets`)

## Local Review
**Status**: complete
**When**: 2026-05-18 22:30
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))
**Verdict**: changes-requested

**Branch**: `feature/issue-9` at `74d53df`
**Mode**: pre-push
**Depth**: Deep (reason: ~580 LOC of mutex-protected concurrency logic across lifecycle paths)
**Must-fix**: 2 | **Suggestions**: 5

### Findings
- [x] (must-fix) Re-request after give-up via `operator[]` default-construct — `remote_node.cpp:293`. **→ commit 8c9b21d** added `given_up_packet_numbers_` set; backoff loop skips members; pruning runs in `clearReceivedPacketTimesBefore` once packet number falls below `received_packet_times_.begin()->first`.
- [x] (must-fix) `1u << (attempts - 1)` UB at attempts >= 33 — `remote_node.cpp:302`. **→ commit 8c9b21d** clamps `shift = min(state.attempts - 1, 7)` before the shift.
- [x] (suggestion) Regression test for must-fix 1: gap kept visible past TTL, counter must not climb. **→ commit 8c9b21d** added `GiveUpIsNotReArmedByOngoingArrivals` test case.
- [ ] (suggestion) Drop dead `resend_state_` sweep in `clearReceivedPacketTimesBefore` — `remote_node.cpp:251`. Now non-dead: pass 1 erases the resend_state_ entry but the sweep here still runs and is a no-op for the give-up case. Worth removing or asserting; deferred to a follow-up.
- [ ] (suggestion) Document lock-ordering invariant on `RemoteNode::state_mutex_` — `remote_node.h:116-122`. Deferred.
- [x] (suggestion) Document `resend_giveup_count` cumulative semantics (survives remote restart) — `Remote.msg:28`. **→ commit 8c9b21d** added the cumulative-semantics note alongside the new given_up tracking comment in `remote_node.h`. (Remote.msg's existing field comment already says "Cumulative per remote since the receiver started.")
- [ ] (suggestion) Rename `seconds()` to `to_seconds()` to avoid ADL collisions — `resend_constants.h:76`. Deferred.

## External Review
**Status**: complete
**When**: 2026-05-19 00:45
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))

**PR**: #13 — 2 Copilot review(s) (stale @ `7a1a8fa` + fresh @ `49ca9a1`), 11 inline comments, 11 valid, 0 false positives
**CI**: all-pass (4 checks)

### Actions
- [x] **C1 (false give-up on recovered packets)** — `unwrap()`'s `if(!duplicate)` branch now calls `resend_state_.erase(packet->packet_number)` so an arrived packet can never falsely trigger give-up later. The test helper `recordReceivedPacketTimeForTest` was updated to mirror this so unit tests exercise the production behavior. **→ commit ff32f58**
- [x] **C2 (regression test)** — `RecoveredPacketDoesNotTriggerGiveUp` added. Drives a tick to detect gap, injects the recovered packet, drives past TTL, asserts counter unchanged. **→ commit ff32f58**
- [x] **F1 (dead sweep)** — Dropped the redundant `resend_state_` eviction loop in `clearReceivedPacketTimesBefore`; comment notes the give-up pass handles it. **→ commit ff32f58**
- [x] **F2 (Connection helpers naming)** — Renamed `recordSentPacketForTest` / `sentPacketCountForTest` to snake_case to match `cleanup_sent_packets`. `RemoteNode`'s test helpers stayed camelCase (consistent with its existing camelCase style — `getMissingPackets`, `clearReceivedPacketTimesBefore`, etc.). **→ commit ff32f58**
- [x] **F3 + S5 (`seconds()` helper + Duration round-trip)** — Deleted the `seconds()` helper. All call sites switched to `rclcpp::Duration(kFoo)` for Duration construction or `kFoo.count()` for double extraction. ADL concern sidestepped. **→ commit ff32f58**
- [x] **F4 (`attempts > 0` redundant)** — Converted to `assert(s.attempts > 0)` so a future invariant break is loud. **→ commit ff32f58**
- [x] **F5 (test helper empty vector)** — Comment added at `record_sent_packet_for_test` noting only `packet_number` + `timestamp` are populated; not safe for tests that exercise `resend_packets()` on seeded data. **→ commit ff32f58**
