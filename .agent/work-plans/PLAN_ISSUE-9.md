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

Six commits on `feature/issue-9`:

1. **Plan commit** — this file.
2. **D — TTL/window alignment.** Single shared constant
   `kSentPacketTTL = 5.0s` in a new `udp_bridge/include/udp_bridge/resend_constants.h`
   header. Both `cleanupSentPackets` (`udp_bridge.cpp:742`) and
   `clearReceivedPacketTimesBefore` (`remote_node.cpp:175`) reference it.
   Receiver-window > sender-TTL becomes structurally impossible.
   Rationale: the `bridge_info`-advertised-TTL alternative is more
   flexible but adds a wire-format dependency that doesn't earn its
   complexity at this phase. Single shared constant is the boring
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
4. **B — Exponential backoff with give-up.** Replace the flat 0.2 s
   re-request cooldown with: track `attempt_count_[packet_number]`
   alongside `resend_request_times_`. Cooldown =
   `min(base * 2^(attempts-1), cap)` where `base = 0.2 s`,
   `cap = 1.6 s` (8× base — issue suggests "0.2 → 0.4 → 0.8 → …, capped").
   Give-up condition: stop requesting a packet number when its
   `attempt_count` exceeds 5 OR its first-request time is older than the
   sender TTL (`kSentPacketTTL`); whichever comes first. Log + bump a
   "give up" stat counter so silent loss is visible. Add `give_up_count_`
   field to whatever stats struct `RemoteNode` already exposes (verify
   during implementation; issue notes "logged + stats bump").
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
     `0.2, 0.4, 0.8, 1.6, 1.6, ...` then give-up.
   - **give-up**: simulate `kSentPacketTTL+ε` of failed requests;
     assert give-up stat bump and no further requests for that number.
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
| `udp_bridge/include/udp_bridge/resend_constants.h` | New: `kSentPacketTTL`, `kResendDebounceHold`, `kResendBackoffBase`, `kResendBackoffCap`, `kResendMaxAttempts` |
| `udp_bridge/src/udp_bridge.cpp` | `cleanupSentPackets` uses `kSentPacketTTL` |
| `udp_bridge/src/remote_node.cpp` | `getMissingPackets` uses constants header; debounce check; backoff + give-up logic; stat bump |
| `udp_bridge/include/udp_bridge/remote_node.h` | New `attempt_count_` map; `give_up_count_` stat field (location TBD during implementation) |
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

1. **`give_up_count` placement**: does `RemoteNode` already expose a stats
   struct that's published as part of `bridge_info` or `topic_statistics`?
   If yes, add the field there. If no, add a private counter and log only
   for now; surfacing it in diagnostics is a small follow-up. Pin during
   implementation.
2. **Coordination with #11**: #11 is structural (executor split) and adds
   thread-safety to `Connection::sent_packets_`. #9's `cleanup_sent_packets`
   constant change is small. Recommend #11 lands first; this PR rebases
   onto its mutex additions. Confirm with reviewer before merging order.
3. **Backoff cap and max attempts**: chose `cap = 1.6 s` and
   `max_attempts = 5`. With 100 ms debounce + backoff schedule, total
   request window before give-up is ~6.4 s — slightly above the 5 s TTL,
   so give-up triggers via TTL first under typical conditions. Confirm
   these magic numbers are reasonable (or tune during plan review).

## Estimated Scope

Single PR, six commits. ~150-250 lines of code change plus ~200-300 lines
of new tests. Smaller than #11.
