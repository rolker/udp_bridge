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

## Local Review
**Status**: complete
**When**: 2026-05-19 09:00
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))
**Verdict**: approved

**PR**: #13 at `e4e5f3f`
**Mode**: post-PR
**Depth**: Standard (reason: ~800 LOC source/test + Remote.msg append + concurrency-adjacent)
**Must-fix**: 0 | **Suggestions**: 3

### Findings
- [ ] (suggestion) Test-only `getMissingPackets(rclcpp::Time)` overload is public but lacks the `now.nanoseconds() == 0` guard the no-arg variant has — `remote_node.h:75`, `remote_node.cpp:260`
- [ ] (suggestion) Test gap: remote-restart with active resend state — no case exercises `update(BridgeInfo)`'s clear of `received_packet_times_` / `resend_state_` / `given_up_packet_numbers_` at `remote_node.cpp:73-85` after the give-up reorg — `test_remote_node_resend.cpp`
- [ ] (suggestion) Test gap: debounce anchor walking across consecutive gaps — the "runs of consecutive gaps" design intent at `remote_node.cpp:308-312` isn't exercised; burst-loss debounce could regress silently — `test_remote_node_resend.cpp`

## External Review (Combined Triage)
**Status**: complete
**When**: 2026-05-19 09:30
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))

**PR**: #13 — 3 Copilot review(s) (15 inline comments) + 1 conversation comment + 1 local review (this session), triaged together
**CI**: all-pass (4 checks at `e4e5f3f`)

### Actions
- [ ] (optional/cosmetic) Add `rclcpp::init` guard to `test_connection_cleanup.cpp` for consistency with sister test file (Copilot R3)
- [ ] (optional/cosmetic) Append per-test suffix to `ResendFixture` node name to silence duplicate-node warnings (Copilot R3)
- [ ] (fix) Guard or hide test-only `getMissingPackets(rclcpp::Time)` overload — `remote_node.h:75` (local review)
- [ ] (fix) Add remote-restart-with-active-resend-state test case (local review)
- [ ] (fix) Add burst-loss debounce test case (local review)
- [ ] (optional) Dismiss the 6 addressed Copilot R1/R2 inline comments on the PR

### Classification breakdown
- **11/15 Copilot comments addressed** at HEAD by commits `ff32f58` + `e4e5f3f` (R1/R2 ran against stale SHAs `7a1a8fa4` / `49ca9a1d`).
- **4/15 Copilot R3 comments** still apply: 1 intentionally-kept design choice (`assert(s.attempts > 0)` belt-and-suspenders, defended in code comment), 1 adequately-documented test-helper caveat (`record_sent_packet_for_test`), 2 cosmetic test-hygiene items.
- **3 new findings** from this session's `/review-code 13` local review — all suggestions targeting test-coverage hardening + one small API-surface tightening. No must-fix.
- Self plan-review conversation comment: all 7 findings absorbed into plan revisions pre-implementation.

## External Review (R4 follow-up)
**Status**: complete
**When**: 2026-05-19 13:55
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))

**PR**: #13 at `c9b5820` — Copilot R4 fired after the local-review-fixes push; 2 inline comments, 2 valid, 0 false positives
**CI**: all-pass (4 checks at `c9b5820`)

### Actions
- [ ] (fix) Update `state_mutex_` doc comment to drop the stale `resend_request_times_` reference and list current guarded fields — `remote_node.h:110-111`
- [ ] (fix) Update `resend_state_` member doc to reflect actual cleanup paths (give-up sweep + recordPacketArrival + remote restart; not `clearReceivedPacketTimesBefore` anymore) — `remote_node.h:138-144`
- [ ] (optional) Dismiss 9 already-addressed Copilot R1/R2 inline comments for dashboard hygiene

### Classification breakdown
- Both R4 findings are doc-staleness from the C1/F1 fix landed in `ff32f58` (the `clearReceivedPacketTimesBefore` sweep removal and the field rename from `resend_request_times_` to `resend_state_` + `given_up_packet_numbers_`).
- No new algorithmic or code-correctness findings — the local-review fixes (overload guard + 2 new test cases + cosmetic hygiene) didn't introduce new issues.

## External Review (R5 follow-up)
**Status**: complete
**When**: 2026-05-19 14:10
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))

**PR**: #13 at `67594ab` — Copilot R5 fired after the R4 doc-refresh push; 4 inline comments, 4 valid, 0 false positives
**CI**: all-pass (4 checks at `67594ab`)

### Actions
- [ ] (fix) Reword `clearReceivedPacketTimesBefore` and give-up-sweep comments to drop the over-claim that `kReceiveHistoryWindow == kSentPacketTTL` is enforced by static_assert — `remote_node.cpp:199-201, 276`. The static_assert only enforces `<=`; the two are equal today by default but narrowing is allowed.
- [ ] (fix) Gate the 4 test-only helpers behind `#ifdef BUILD_TESTING` so they don't leak into the installed public ABI — `getMissingPackets(rclcpp::Time)` + `recordReceivedPacketTimeForTest` in `remote_node.h`, `record_sent_packet_for_test` + `sent_packet_count_for_test` in `connection.h` (plus matching `.cpp` definitions). CMakeLists.txt:63 installs `include/` so these are currently public.

### Classification breakdown
- R5 #1+#2 are doc-staleness in pre-existing (not session-introduced) comments — the over-claim was present before today's work and only surfaced when R5 re-read the area after the R4 doc-refresh.
- R5 #3+#4 are a real API-surface concern. Test-only helpers were added in earlier commits (test_remote_node_resend + ff32f58); R5 is the first round to flag the installed-header exposure. `#ifdef BUILD_TESTING` is the lightest-touch fix and matches the pattern CMakeLists already uses to gate test registration.

## External Review (R6 follow-up)
**Status**: complete
**When**: 2026-05-19 14:30
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))

**PR**: #13 at `ce19363` — Copilot R6 fired ~22 min after the R5 follow-up push; 3 inline comments, 3 valid, 0 false positives
**CI**: all-pass (4 checks at `ce19363`)

### Actions
- [ ] (fix) Add `#include <cstdint>` to `resend_constants.h` (R6 #1) — `uint32_t kMaxBackoffShift` declared without the header; compiles today via transitive include from `<chrono>` but breaks downstream consumers that include `resend_constants.h` first
- [ ] (fix) Rewrite the over-eager R5 doc reword in `remote_node.cpp:196-208`: the inequality is inverted. With `kReceiveHistoryWindow <= kSentPacketTTL`, `giveup_cutoff <= time`, so the give-up sweep evicts a *subset* (not a superset) of what the old eviction sweep would have. Under narrowing, this allows bounded orphaned resend_state_ entries — acceptable but the reasoning needs to be honest (R6 #2)
- [ ] (fix) Restructure `GiveUpIsNotReArmedByOngoingArrivals` to re-inject packet 1 at each tick so it stays in receive history past give-up; current test passes whether the `given_up_packet_numbers_` guard exists or not because packet 1 ages out and packet 2 falls outside the gap-scan range (R6 #3) — the most substantive R6 finding

### Classification breakdown
- R6 #1: real portability bug, trivial fix
- R6 #2: self-inflicted in the R5 reword — Copilot caught my inverted math within 22 min
- R6 #3: weaker test coverage than the test name implies; my Case 5 doesn't actually exercise the guard it claims to. This is the highest-value R6 finding
