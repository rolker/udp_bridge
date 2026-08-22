# Plan: BridgeInfo DataRates undercount fragmented messages (~350x)

## Issue

https://github.com/rolker/udp_bridge/issues/55

## Context

Measurement on the #18 bench harness shows 2.84 MB/s of 480 kB `sensor_msgs/Image`
data actually crossing the bridge over WiFi, while the bridge's own
`BridgeInfo` DataRates for that WiFi connection reports only 7.7–8.5 kB/s
(`message.success_bytes_per_second`). The 8 kB/s figure matches exactly
the Critical + Telemetry tiers (64 B + ~7.2 kB) — Bulk is absent. Dropped
and failed bytes are both 0.0 in every measurement phase.

The send path for topic messages is:
`callback()` → `UDPBridge::send<MessageInternal>` →
`UDPBridge::send<vector<vector<uint8_t>>>` (fragment wrapper) →
`Connection::send(vector<WrappedPacket>&, ...)` →
inner `Connection::send(vector<uint8_t>&, ...)` per fragment.

Each fragment should be recorded in `sent_packet_statistics_`
(a `PacketSendStatistics` member of `Connection`) by the inner
`send(vector<uint8_t>&, ...)`, and `data_sent_rate()` reads those records to
populate `BridgeInfo` DataRates. On first code read they *should* be counted,
but the bench evidence says they are not. The root cause is not yet identified.

The `can_send` gate uses the same `sent_packet_statistics_` store. If fragmented
traffic is not recorded, the rate limiter cannot see it — which is consistent with
`dropped = 0` observed at a 4 MB/s configured cap while ~4.8 MB/s of Bulk is sent.

**PR #56 interaction**: PR #56 (issue #52, open, CI-green) bases the admission
floor and resend budget on the remote's `received_bytes_per_second`. If the receive
side has a symmetric blind spot in its byte-count path (the `data_receive_rate()`
path uses `ReceivedSize::size`, also `uint16_t`), PR #56 is steering on a partial
view. Note this interaction in the PR description but do not fix it in this PR.

## Approach

1. **Write a diagnostic gtest first.** Directly exercise
   `Connection::send(vector<WrappedPacket>&, ...)` with a set of WrappedPackets
   sized to simulate fragments (multiple packets, total size >> any single packet).
   Assert that `data_sent_rate()` after the send accounts for all bytes handed in.
   This distinguishes "not recorded at all" from "recorded somewhere else" without
   touching the bench.

2. **Identify and fix the root cause.** Based on test findings, fix the accounting
   so every fragment's wire bytes appear in `sent_packet_statistics_`. Known
   candidates to investigate during implementation:
   - `PacketSizeData::size` is `uint16_t` — fine at bench's max_packet_size=65500
     (65500 ≤ 65535) but a latent hazard for larger configs; widen as part of fix.
   - Timestamp zero guard in `Statistics::add()`: skips entry if
     `timestamp.nanoseconds() == 0`; verify the `now` timestamp is always valid
     on the fragmented path.
   - The `Statistics::add()` 10-second eviction window relative to the timestamp
     of the LATEST entry: a burst of same-timestamp fragments could cause odd
     interaction if the deque was previously empty.
   - Any early-return path in `Connection::send(vector<WrappedPacket>&, ...)`
     that bypasses the inner per-packet `send()` call.

3. **Answer the `can_send` question explicitly.** After the accounting fix, add
   a test verifying that fragment traffic is metered by `can_send` — send enough
   fragment bytes to exceed the configured rate limit and assert that some are
   dropped. If pre-fix behaviour was that `can_send` never saw fragment traffic,
   document the behaviour change in the PR description and flag that operators
   with `maximum_bytes_per_second` tuned against the old (incorrect) undercount
   will see the limiter engage for the first time.

4. **Check the receive-side size field.** `ReceivedSize::size` (used by
   `data_receive_rate()` → `received_bytes_per_second` in BridgeInfo) is also
   `uint16_t`. Audit for the same accounting gap. If present, note it explicitly
   in the PR description; if it also needs a fix, include it here.

5. **Update documentation.** If the DataRates fields' semantics change (e.g., they
   now count wire bytes of all fragments), update `.agents/README.md` and any
   operator-facing references that describe what these numbers mean.

## Files to Change

| File | Change |
|------|--------|
| `test/test_fragment_accounting.cpp` | New gtest: send multi-fragment batch through `Connection::send(vector<WrappedPacket>&, ...)`, assert `data_sent_rate()` matches |
| `udp_bridge/CMakeLists.txt` | `add_udp_bridge_gtest(test_fragment_accounting test/test_fragment_accounting.cpp)` |
| `src/connection.cpp` | Fix the accounting gap (exact change TBD after test reveals root cause) |
| `src/statistics.cpp` and/or `include/udp_bridge/statistics.h` | Widen `PacketSizeData::size` from `uint16_t` to `uint32_t` (latent hazard regardless of root cause); update `can_send` and `bytes_in_window` if affected |
| `.agents/README.md` | Update DataRates description if semantics change |

## Principles Self-Check

| Principle | Consideration |
|---|---|
| Human control and transparency | DataRates is the primary operator-facing telemetry for link health. Fixing the undercount restores accurate, trustworthy numbers. If large-message delivery rates change post-fix (rate limiter engaging for the first time), the PR description must call this out explicitly so operators can re-evaluate their `maximum_bytes_per_second` configs. |
| Enforcement over documentation | The regression test must land in this PR — not deferred. `can_send` metering answer must be tested, not just asserted in a comment. |
| Capture decisions | If the fix involves a design choice (e.g., counting compressed wire bytes vs. uncompressed payload bytes), add a one-line comment explaining the decision at the recording site. |
| A change includes its consequences | Check `test_connection_rate_limit.cpp`, `test_resend_budget.cpp`, and `test_admission_control.cpp` for test validity after accounting changes. If `can_send` now sees fragment traffic, the rate-limit test's expected behaviour may shift. |
| Only what's needed | Scope: investigate → fix accounting → regression test → note PR #56 interaction. Do NOT widen scope to fix the receive-side blind spot if it's a separate mechanism (file a follow-up issue). |
| Test what breaks | The diagnostic test is the first deliverable; it pins the exact byte-accounting gap before any production code is touched. |

## ADR Compliance

| ADR | Triggered | How addressed |
|---|---|---|
| ADR-0008 — ROS 2 conventions | Yes | New test registered via `add_udp_bridge_gtest()`, not raw `ament_add_gtest` (ODR discipline, per `.agents/README.md` Common Pitfalls). |
| ADR-0013 — progress.md vocabulary | Yes | This plan is preceded by an `## Issue Review` entry; `## Plan Authored` entry follows this plan commit. |
| ADR-0018 — local CI verification | Yes | Run `ci_local.sh` before opening the PR; the stats-path changes touch concurrent send paths that benefit from full integration coverage. |

## Consequences

| If we change... | Also update... | Included in plan? |
|---|---|---|
| `PacketSizeData::size` widened to `uint32_t` | `PacketSendStatistics::can_send`, `bytes_in_window`, `get()` (accumulation type) | Yes |
| Fragment bytes now counted in `sent_packet_statistics_` | `test_connection_rate_limit.cpp` — verify rate limit test still valid; `test_resend_budget.cpp` — resend budget is based on `bytes_in_window(resend, ...)` which is unaffected, but review anyway | Yes — noted; review during implementation |
| `can_send` now meters fragment traffic | Operator `maximum_bytes_per_second` configs (bizzyboat.yaml etc.) tuned against old undercount will engage the limiter for the first time — flag in PR description | Yes — noted in approach step 3 |
| `received_bytes_per_second` if receive-side gap found | PR #56 goodput signal validity; file a follow-up issue | Yes — note in PR description; follow-up if separate |

## Documentation & Instruction Impact

- **Stale docs** (must land in this PR): `.agents/README.md` lines 99-102 describe
  `BridgeInfo` DataRates output. If the fix changes what the fields count (e.g., they
  now include all wire bytes for fragmented messages), update that description. Also
  review any per-connection DataRates usage in `doc/admission_control_design.md` if
  it exists, since the AIMD goodput signal is based on these numbers.

- **Agent-instruction candidates** (proposals only — operator decides):
  - The `PacketSizeData::size` uint16_t field is a recurring hazard: any sub-system
    that stores a packet-size in a narrow integer type should be audited. A note in
    `.agent/knowledge/` on "integer-width gotchas in stats structs" could prevent
    future bugs in new stats fields.
  - The pattern of "test before touching production code" (diagnostic gtest first,
    then fix) proved valuable for #16. A one-liner in `.agent/knowledge/` on this
    pattern for stats/accounting bugs may be worth adding.

## Open Questions

- After the diagnostic test reveals the root cause, the fix location in
  `connection.cpp` or `statistics.cpp` will be confirmed — no structural open
  question, but implementation must answer it before fixing.
- Is the receive-side `ReceivedSize::size` (uint16_t) also undercounting? Audit
  during implementation; if yes, fix here; if it requires a separate mechanism, file
  a follow-up issue.
- After the fix, does the rate limiter drop Bulk traffic that was previously passing
  unmetered? If yes, the bench delivery numbers will change — a brief bench run with
  the fix applied should be noted in the PR description (not required to block merge,
  but informative for operators re-calibrating limits).

## Estimated Scope

Single PR. The test + fix + consequence audit is bounded: one new test file, one
fix in connection.cpp/statistics.cpp (the root cause is in a well-bounded stats
recording path), and one doc update. No interface changes to public topics or
services.
