# Plan: Restrict resend response to the connection the request arrived on

## Issue

https://github.com/rolker/udp_bridge/issues/23

## Context

`UDPBridge::decodeResendRequest` (`udp_bridge.cpp:695-711`) currently iterates
all connections to the source remote and calls `resend_packets` on each. A single
`ResendRequest` therefore fans out to every active path. Combined with
`resendMissingPackets` (`udp_bridge.cpp:965-983`) broadcasting the *request* to all
paths, the cost under both-paths-alive is 4× resends per missing packet. The
2026-05-19 bizzy field deployment exposed this: ~65 % of wifi outbound was
resends (~950 KB/s), much of it sent into a wifi link that was rx-dead at the
time.

Per-issue review (`#23` comment) and user decision (2026-05-20) settle three
design forks:

1. **Coordinated-redeploy backward-compat**, not graceful field-add. The
   `MessageInternal.msg` precedent in this same package explicitly states CDR
   mixed-version interop is not guaranteed; we mirror that caveat on
   `ResendRequest.msg` and drop the "broadcast fallback for empty
   `connection_id`" branch entirely (it would never trigger cleanly anyway).
2. **Bench harness (#18) is out of scope for this PR.** Unit test only; a
   follow-up comment on #18 captures suggested scenarios.
3. **Default-on, no config knob.** New behavior is a strict subset of old
   coverage, but the only observed edge case (rx-only on one path) is rare and
   recovered by give-up + the next regular send.

## Approach

1. **`ResendRequest.msg` — add `string connection_id`** with a comment matching
   the tone of `MessageInternal.msg`'s coordinated-redeploy caveat.
2. **`UDPBridge::resendMissingPackets` — stamp per connection.** Replace the
   single `send(rr, remote.first, true)` broadcast with a per-connection loop:
   build a fresh `ResendRequest` for each live connection, stamp `connection_id`,
   send through that connection only via a `RemoteConnectionsList` carrying just
   that one id. Hold the existing snapshot-then-iterate pattern (the locking
   shape is unchanged).
3. **Move routing into `RemoteNode`.** Add
   `RemoteNode::dispatchResendRequest(const ResendRequest&, int socket, rclcpp::Time now)`
   that looks up `connection(rr.connection_id)` and calls `resend_packets` on
   exactly that connection. If the id is not in `connections_`, no-op and
   emit a DEBUG log that names both the stamped id and the set of currently
   known connection ids — this branch fires both on legitimate races (sender
   stamped while the receiver was tearing the connection down for a CONNECT
   cycle or config reload) and on bona-fide coordinated-redeploy mismatches,
   so the log line needs to give an operator chasing either case enough to
   diagnose. Keeps the routing logic on the class that owns the connection
   map and is already directly constructed in `test_remote_node_resend.cpp`'s
   fixture.
4. **`UDPBridge::decodeResendRequest` becomes a thin shim.** Snapshot the
   matching `RemoteNode` under `remote_nodes_mutex_`, then call
   `dispatchResendRequest` outside the lock (same pattern as today —
   network I/O outside `remote_nodes_mutex_`).
5. **Test seam: per-connection resend-call counter.** Add a
   `UDP_BRIDGE_BUILD_TESTING`-gated `resend_call_count_for_test()` accessor on
   `Connection`. `resend_packets` increments the counter at entry (before the
   network I/O). Mirrors the existing `record_sent_packet_for_test` /
   `sent_packet_count_for_test` pattern at `connection.h:102-127`.
6. **Unit tests.** New `TEST_F`s in `test_remote_node_resend.cpp`. Common
   setup: `RemoteNode` with two `Connection`s (`"wifi"`, `"vpn"`); seed a
   sent packet on each via `record_sent_packet_for_test`; dummy socket
   obtained via `socket(AF_INET, SOCK_DGRAM, 0)` left unbound and `close()`d
   on teardown — `sendto` will fail benignly but the counter is what we
   assert on, not the I/O.
   - **Matched-id routing.** Build a `ResendRequest` stamped for `"wifi"`;
     call `dispatchResendRequest`; assert
     `wifi.resend_call_count_for_test() == 1` and
     `vpn.resend_call_count_for_test() == 0`.
   - **Stamped id no longer present.** Set up both connections, then tear
     down `"wifi"` (drop it from `connections_` — exercise the production
     path via whatever method already removes connections, or via a
     `UDP_BRIDGE_BUILD_TESTING`-gated helper if none is reachable); send a
     `ResendRequest` stamped for `"wifi"`; assert both counters stay at 0
     (this is the legitimate sender/receiver race during a CONNECT cycle).
   - **Unknown id (config mismatch).** Smaller third test: stamp for a
     never-registered id; assert both counters stay at 0. Same code path
     as the stale-id case but distinct framing for the operator-facing
     diagnostic.
7. **Follow-up comment on #18.** After PR opens, post a short note listing
   suggested bench scenarios: (a) verify per-connection routing under
   normal traffic, (b) verify no resends fan out to a path with rx loss,
   (c) measure outbound bytes/sec reduction vs the broadcast baseline.

## Files to Change

| File | Change |
|---|---|
| `udp_bridge_interfaces/msg/ResendRequest.msg` | Add `string connection_id` field + coordinated-redeploy comment block |
| `udp_bridge/src/udp_bridge.cpp` | Rewrite `resendMissingPackets` (per-connection stamp+send); rewrite `decodeResendRequest` as RemoteNode dispatch shim |
| `udp_bridge/include/udp_bridge/remote_node.h` | Declare `dispatchResendRequest(const ResendRequest&, int, rclcpp::Time)` |
| `udp_bridge/src/remote_node.cpp` | Implement `dispatchResendRequest` |
| `udp_bridge/include/udp_bridge/connection.h` | Add `resend_call_count_for_test()` under `UDP_BRIDGE_BUILD_TESTING`; counter member under same gate |
| `udp_bridge/src/connection.cpp` | Increment counter at entry of `resend_packets` (gated) |
| `udp_bridge/test/test_remote_node_resend.cpp` | Two new `TEST_F`s: matched-id routing; unknown-id no-op |

## Principles Self-Check

| Principle | Consideration |
|---|---|
| Human control and transparency | Behavior change is opt-out at deployment level (coordinated redeploy); resend volume drop is observable through existing `DataRates.*_bytes_per_second` per connection |
| A change includes its consequences | Wire-format change → `ResendRequest.msg` comment documents the redeploy contract; no out-of-package consumers (internal control plane); unit test lands with the change |
| Only what's needed | No config knob, no fallback branch; ~80 LOC plus test |
| Test what breaks | Unit test asserts routing on the exact decision the field storm exposed; counter seam keeps it deterministic |

## ADR Compliance

| ADR | Triggered | How addressed |
|---|---|---|
| 0002 — Worktree isolation | Yes | Plan committed from worktree `feature/issue-23` |
| 0008 — ROS 2 official conventions | Yes | Additive trailing field on `ResendRequest.msg` is the conventional pattern; documenting the CDR coordinated-redeploy caveat (matching `MessageInternal.msg`'s existing comment) is the honest version of "ROS message defaults" |

## Consequences

| If we change... | Also update... | Included in plan? |
|---|---|---|
| `ResendRequest.msg` (interface) | Downstream consumers | Yes — verified: no external consumers; internal control-plane only |
| Resend fan-out behavior | Bench harness | Deferred: follow-up comment on #18, not blocking |
| `decodeResendRequest` routing | Tests covering the routing | Yes — two new `TEST_F`s |
| Connection method (`resend_packets`) instrumentation | Connection's other test helpers | No new accessor needed beyond the counter; matches existing seam pattern |

## Open Questions

None for implementation — the three design forks are settled. One operational
note: when this lands, bizzy and operator must be redeployed together. Capture
in the PR body so it's visible to whoever cuts the deployment.

## Estimated Scope

Single PR. ~80 LOC code + ~50 LOC test. Comparable to the resend-loop work in
PR #13.
