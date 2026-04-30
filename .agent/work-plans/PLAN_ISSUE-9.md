# Plan: Resend loop amplifies traffic rather than tracking loss

## Issue

https://github.com/rolker/udp_bridge/issues/9

## Context

Field analysis of the 2026-04-21 BizzyBoat session showed `udp_bridge` was
generating ~22% resend overhead on both WiFi and VPN paths even though only
WiFi had link outages. VPN's `rx_duplicate_bytes/s` was 126% of `rx_bytes/s` —
multiple copies of the same data arriving where one would suffice. The resend
algorithm is amplifying rather than tracking loss.

Three concrete failure modes pinned in the issue body:

- **A. No debounce.** `RemoteNode::getMissingPackets` (`remote_node.cpp:170`)
  fires a resend request the same `spin_once` tick a gap is observed.
  Legitimate reordering — common over cellular/VPN — triggers spurious
  resends.
- **B. No backoff.** `remote_node.cpp:177` uses a flat `0.2 s` re-request
  cooldown. When a real RTT spikes to 10+ s during a dropout, the receiver
  re-requests the same packets ~50× before the first response arrives.
  This is the mechanism behind VPN's 126% rx_duplicate.
- **D. TTL/window mismatch.** Sender cleans `sent_packets_` at 3 s
  (`udp_bridge.cpp:742`); receiver windows at 5 s (`remote_node.cpp:175`).
  Receiver can request packet numbers the sender has already forgotten,
  silently lost.

Phase 2 items (C: RTT-aware cooldown, E: separate budget, F: per-topic
no_resend) are explicitly deferred.

## Approach

**Sequencing**: this PR rebases onto #11 once #11 lands. #11 adds mutexes
to `Connection::sent_packets_` (which this PR's TTL-alignment commit also
touches); rebasing #9 onto a thread-safe `Connection` is cheaper than the
reverse. Decided during planning.

**As-you-go cleanup**: every commit that edits a file in this PR also
renames any `m_foo` member to `foo_` in that file, with full
grep-and-replace across the codebase to update every reference (header +
all `.cpp` + tests). The current package mixes both conventions — newer
additions use `*_`, older ROS-1-port code uses `m_*`. Standardize on `*_`
(ROS 2 / Google style). Never half-rename. Per-commit grep before push to
verify zero remaining hits of the old name.

Six commits on `feature/issue-9`:

1. **Plan commit** — this file.
2. **D — TTL/window alignment + `BridgeInfo` schema extension.**
   - Single shared constant `kSentPacketTTL = 5.0s` in a new
     `udp_bridge/include/udp_bridge/resend_constants.h` header. Both
     `cleanupSentPackets` (`udp_bridge.cpp:742`) and
     `clearReceivedPacketTimesBefore` (`remote_node.cpp:175`) reference
     it. Receiver-window > sender-TTL becomes structurally impossible.
   - Extend `udp_bridge_interfaces/msg/BridgeInfo.msg` with
     `uint32 resend_giveup_count` (per-remote aggregate, populated by the
     receiver when it gives up on missing packets). New field is
     backward-compatible — old consumers ignore it; new consumers see 0
     from old senders. `rqt_udp_bridge` can surface it with a one-line
     addition.
   - Rationale on TTL: the `bridge_info`-advertised-TTL alternative is
     more flexible but adds a wire-format dependency that doesn't earn
     its complexity at this phase. Single shared constant is the boring
     right answer; revisit advertisement if Phase 2 needs per-connection
     tuning.
3. **A — Debounce.** Add a fixed 100 ms debounce hold to
   `getMissingPackets`: a packet is only considered "missing" if the
   newest received packet's timestamp is at least 100 ms newer than the
   gap. Implementation: track `received_packet_times_.rbegin()->second`
   (already available); when scanning for gaps, skip any number whose
   slot's expected arrival time (interpolated as
   `prev_received_time + 100ms` or simpler: just compare against the
   newest packet's time) is within the debounce window. Concrete check:
   `if (newest_received_time - now_minus_debounce < 0) continue;` —
   refined during implementation. 100 ms hold is the issue-suggested
   fixed value (RTT-aware is Phase 2).
4. **B — Exponential backoff with TTL-bounded give-up.** Replace the flat
   0.2 s re-request cooldown with: track `attempt_count_[packet_number]`
   alongside `resend_request_times_`. Cooldown =
   `min(base * 2^(attempts-1), cap)` where `base = 0.2 s`,
   `cap = 1.6 s` (8× base — issue suggests "0.2 → 0.4 → 0.8 → …, capped").
   **Give-up condition: TTL only.** When a missing packet's first-request
   time is older than `kSentPacketTTL`, stop requesting it (the sender has
   forgotten it; further requests are pointless). No separate
   `max_attempts` cap — TTL is one principled boundary instead of two
   redundant magic numbers. Under the cap-1.6-s schedule, this gives ~6
   request attempts in the 5 s window. Log at WARN + bump
   `resend_giveup_count` (see commit 2 below for schema location) so
   silent loss is visible.
5. **Tests.** Four `TEST_F` cases in a new
   `udp_bridge/test/test_remote_node_resend.cpp`, exercising
   `getMissingPackets` directly:
   - **reorder-within-debounce**: insert packets in order
     `1, 3, 2`, all timestamped within 100 ms; assert no resend
     requested for `2`.
   - **debounce-then-request**: insert `1, 3` with `3`'s timestamp
     150 ms later; assert exactly one resend for `2` on the first call,
     none on a follow-up call before backoff expires.
   - **backoff-schedule**: missing packet over many ticks; capture the
     `(tick_time, requested?)` series; assert intervals follow
     `0.2, 0.4, 0.8, 1.6, 1.6, ...` until TTL kills it.
   - **TTL give-up**: simulate `kSentPacketTTL+ε` of failed requests;
     assert `resend_giveup_count_` increments by exactly 1 for that
     packet and no further requests are issued for it.
   `RemoteNode` constructor already takes `NodeInterfaces` — tests
   construct a mock node (or use `rclcpp::Node` directly) and inject a
   fake `Clock` to control time. Verify pattern via
   `test_defragmenter.cpp` (existing test in the repo) for how the
   package mocks node interfaces.
6. **Validation.** Build + run `colcon test --packages-select udp_bridge`.
   Manual sanity: verify `make build` from workspace root still passes,
   no compile-time regressions in dependent packages (`rqt_udp_bridge`).
   PR description includes before/after expectations: "WiFi resend rate
   should drop closer to actual loss rate; VPN rx_duplicate should
   approach 0." Field validation deferred to next BizzyBoat deployment.

## Files to Change

| File | Change |
|------|--------|
| `udp_bridge/include/udp_bridge/resend_constants.h` | New: `kSentPacketTTL`, `kResendDebounceHold`, `kResendBackoffBase`, `kResendBackoffCap` (no `kResendMaxAttempts` — TTL is the sole give-up condition) |
| `udp_bridge/src/udp_bridge.cpp` | `cleanupSentPackets` uses `kSentPacketTTL`; populate `BridgeInfo.resend_giveup_count` aggregate before publishing |
| `udp_bridge/src/remote_node.cpp` | `getMissingPackets` uses constants header; debounce check; backoff + TTL-bounded give-up; bump `resend_giveup_count_`; WARN log on give-up |
| `udp_bridge/include/udp_bridge/remote_node.h` | New `attempt_count_` map; `resend_giveup_count_` counter; getter for the counter so `udp_bridge.cpp` can populate `BridgeInfo` |
| `udp_bridge_interfaces/msg/BridgeInfo.msg` | Append `uint32 resend_giveup_count` |
| `udp_bridge/test/test_remote_node_resend.cpp` | New gtest with 4 cases |
| `udp_bridge/CMakeLists.txt` | Register new test |

## Principles Self-Check

| Principle | Consideration |
|---|---|
| Human control and transparency | Give-up bumps a stat and logs at `WARN` level — silent loss is visible operationally. Constants live in a header with comments explaining the "why" of each value. |
| Capture decisions, not just implementations | Constants header carries a comment block explaining the algorithm rationale (debounce + backoff + give-up + TTL alignment) so a future "simplification" doesn't accidentally revert. |
| A change includes its consequences | 4 unit tests ship with the algorithm change. No schema changes. Workspace grep done in review-issue (only `rqt_udp_bridge` consumes resend stats; field names unchanged). |
| Only what's needed | Phase 1 only (A, B, D). Phase 2 (C, E, F) deliberately deferred per issue body. |
| Improve incrementally | Three independent algorithmic tweaks → three commits. |
| Test what breaks | All four `getMissingPackets` failure modes from the issue have a corresponding test case. |

## ADR Compliance

| ADR | Triggered | How addressed |
|---|---|---|
| 0002 — Worktree isolation | Yes | Working in `feature/issue-9` worktree on udp_bridge project repo. |
| 0008 — Follow ROS 2 conventions | Mild | No package-layout / launch / `package.xml` changes. New constants header is package-internal C++. No ROS 2 parameter additions in this PR (Phase 2 may add per-topic params — out of scope here). |
| 0001 — Adopt ADRs | No | Package-internal algorithm decision; in-code comment in `resend_constants.h` is sufficient. |

## Consequences

| If we change... | Also update... | Included in plan? |
|---|---|---|
| `getMissingPackets` algorithm | Tests for the new behavior | Yes — commit 5 |
| Sender TTL constant | Receiver window (must remain ≥ TTL) | Yes — single shared constant in commit 2 makes this structural |
| Resend stat schema | `rqt_udp_bridge` consumer | Not changed — `data_rates.msg` field names preserved; only adding new optional `give_up_count` if it doesn't already exist |
| Algorithm constants | `QOS_DESIGN.md` (in #11) | No — these are resend-loop tuning constants, not part of the QoS contract |

## Open Questions

None remaining at plan time. All three questions resolved during planning
(see `## Decisions made during planning` below).

## Estimated Scope

Single PR, six commits. ~150-250 lines of code change plus ~200-300 lines
of new tests. Smaller than #11. Some additional churn from `m_*` → `*_`
renames in any file touched.

## Decisions made during planning

These resolved the original `Open Questions` set; recorded here so the
rationale survives.

- **Sequencing**: #11 lands first; this PR rebases onto its mutex
  additions in `Connection::sent_packets_`. #11 is structural; #9's
  algorithm changes are easier to rebase onto thread-safe primitives
  than the reverse.
- **Give-up condition is TTL only** — no separate `max_attempts` cap.
  TTL is one principled boundary (when the sender has forgotten the
  packet, asking again is pointless). Removes a redundant magic number.
  Can re-add if field data ever shows amplification despite TTL.
- **`resend_giveup_count` lives on `BridgeInfo`** as a per-remote
  aggregate `uint32`. Surfaces in operator-side bag captures
  (`bizzyboat.yaml` / `izzyboat.yaml` already record `bridge_info`) and
  is one line for `rqt_udp_bridge` to display. Schema add is
  backward-compatible. Rejected alternatives: private counter only
  (no operational visibility), per-`DataRates` field (semantically
  mismatched — give-up is per-RemoteNode receiver-side, not
  per-Connection sender-side).
- **Backoff defaults**: `base = 0.2 s`, `cap = 1.6 s` (8× base).
  Schedule `0.2 → 0.4 → 0.8 → 1.6 → 1.6 …` covers cellular RTT
  (50-200 ms) up through degraded-satellite RTT (1-2 s). Yields ~6
  attempts in the 5 s TTL window. Tune later if field data demands.
- **Naming convention**: trailing-underscore (`*_`) is the target.
  Rename `m_*` → `*_` as we go, with full grep-and-replace per
  variable; never half-rename.
