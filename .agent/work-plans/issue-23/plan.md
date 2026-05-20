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
2. **`UDPBridge::resendMissingPackets` — stamp per connection (truncated).**
   Replace the single `send(rr, remote.first, true)` broadcast with a
   per-connection loop: build a fresh `ResendRequest` for each live connection,
   stamp `connection_id` **truncated to `maximum_connection_id_size - 1`**
   (the same limit `WrappedPacket` enforces on the on-wire char array at
   `wrapped_packet.cpp:31-32`), send through that connection only via a
   `RemoteConnectionsList` carrying that one id. The truncation is the
   load-bearing detail: the receiver's `connections_` map is keyed on the
   already-truncated id (populated from packet headers in
   `RemoteNode::unwrap`), so stamping the full string would cause silent
   resend-routing loss for any configured id ≥ 8 chars (`starlink`,
   `cellular`, …). Hold the existing snapshot-then-iterate pattern (the
   locking shape is unchanged).
3. **Move routing into `RemoteNode`.** Add
   `RemoteNode::dispatchResendRequest(const ResendRequest&, int socket, rclcpp::Time now)`
   that looks up `connection(rr.connection_id)` and calls `resend_packets` on
   exactly that connection. If the id is not in `connections_`, no-op and
   emit a log that names both the stamped id and the set of currently
   known connection ids. Use **WARN the first time** we see an unrecognized id
   on this RemoteNode (so a real config mismatch is diagnosable at default log
   levels) and **DEBUG on subsequent misses for the same id** (so legitimate
   CONNECT-cycle races don't spam). Bookkeeping is a `std::set<std::string>`
   on RemoteNode guarded by `state_mutex_` — never pruned, bounded by the
   configuration space of legitimate-then-removed ids. Keeps the routing
   logic on the class that owns the connection map and is already directly
   constructed in `test_remote_node_resend.cpp`'s fixture.
4. **`UDPBridge::decodeResendRequest` becomes a thin shim with try/catch.**
   Wrap `deserialize<ResendRequest>(message)` in a `try { … } catch(const
   std::exception&)` block that logs+drops on failure — makes good on the
   "logs+drops on deserialization failure" promise the new `ResendRequest.msg`
   comment makes. Then snapshot the matching `RemoteNode` under
   `remote_nodes_mutex_` and call `dispatchResendRequest` outside the lock
   (same pattern as today — network I/O outside `remote_nodes_mutex_`). The
   parallel gap in `decodeMessageInternal` (which `MessageInternal.msg` also
   wrongly promises is guarded) is pre-existing and gets its own follow-up
   issue rather than expanding scope here.
5. **Test seam: per-connection resend-call counter.** Add a
   `UDP_BRIDGE_BUILD_TESTING`-gated `resend_call_count_for_test()` accessor on
   `Connection`. `resend_packets` increments the counter at entry (before the
   network I/O). Mirrors the existing `record_sent_packet_for_test` /
   `sent_packet_count_for_test` pattern at `connection.h:102-127`.
6. **Unit tests.** Four new `TEST_F`s in `test_remote_node_resend.cpp`.
   Common setup: `RemoteNode` with `Connection`s constructed via
   `newConnection`; `ScopedFd` RAII helper opens a real UDP socket via
   `socket(AF_INET, SOCK_DGRAM, 0)` (kernel accepts the loopback datagram
   even with no listener — `Connection::resend_packets` increments the
   counter at function entry before any sendto, so I/O outcome doesn't
   affect the assertion). No `record_sent_packet_for_test` seeding —
   the test counter is bumped pre-lookup, so seeding wouldn't change
   anything and would be misleading scaffolding.
   - **Matched-id routing.** Two connections (`"wifi"`, `"vpn"`); stamp
     for `"wifi"`; assert wifi's counter is 1 and vpn's is 0.
   - **Stamped id absent (transient state).** Register only `"vpn"`;
     stamp for `"wifi"`. Models the post-restart / pre-BridgeInfo-
     reconciliation window where the receiver hasn't re-registered the
     connection yet. Assert vpn's counter stays at 0 (must not fall
     back to broadcast).
   - **Unknown id (config mismatch).** Register both `"wifi"` and
     `"vpn"`; stamp for `"iridium"` (an id this RemoteNode has never
     seen). Same lookup-miss code path as the absent-id case; differs
     in the `known_ids` payload of the WARN log, which documents the
     two operator scenarios. Assert both counters stay at 0.
   - **Truncated-id contract.** Register a connection keyed on the
     truncated form (`"starlin"`, what the receiver would have after
     packets from a configured-as-`"starlink"` connection arrive).
     Two-part assertion: stamp with `"starlin"` (the post-fix sender
     behavior) — counter goes to 1; stamp with `"starlink"` (the
     pre-fix bug) — counter stays at 1 (lookup misses). Documents the
     wire contract and catches any regression that reintroduces the
     full-id stamp.
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
| `udp_bridge/test/test_remote_node_resend.cpp` | Four new `TEST_F`s (matched-id routing; stamped-id-absent; unknown-id; truncated-id contract) + a `ScopedFd` RAII helper for the dispatch dummy socket |
| `udp_bridge/CMakeLists.txt` | Add `add_udp_bridge_gtest()` helper that wraps `ament_add_gtest` + `target_compile_definitions(UDP_BRIDGE_BUILD_TESTING)` + `target_link_libraries`. Extend the macro to every test target (previously only on two) — see Implementation Notes for the ODR rationale |

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

## Implementation Notes

- **ODR pitfall surfaced in CMakeLists.** The plan called for a
  `UDP_BRIDGE_BUILD_TESTING`-gated counter on `Connection`. The existing
  test-only helpers (`record_sent_packet_for_test`,
  `sent_packet_count_for_test`) are method-only and don't change class
  layout; my counter, however, is a real data member (`std::size_t
  resend_call_count_for_test_`) and therefore changes `sizeof(Connection)`
  when the macro is defined. Before the CMakeLists change, only two test
  targets defined the macro — the rest (notably
  `test_connection_rate_limit`, which constructs `Connection` directly)
  compiled against a smaller layout while linking to a library compiled
  with the larger layout. That ODR mismatch reproduced as a clean
  segfault in `ConcurrentSendsStayUnderLimit`. The fix wraps every test
  target in `add_udp_bridge_gtest()` so they all carry the macro; the
  existing namespaced-macro comment already anticipated the downstream-
  consumer hazard but didn't extend the discipline to in-package tests.
  Documented in CMakeLists so a future agent adding another counter
  doesn't have to re-discover this.

- **Connection-id 8-char wire limit (round-2 fix).** The first
  implementation pass stamped `connection->id()` (the full configured
  string) into `ResendRequest.connection_id`. Review caught that the
  on-wire `SequencedPacketHeader` carries a fixed `char[8]` connection
  id (per `maximum_connection_id_size = 8`, originally added 2023-06-20
  when the bridge moved off UDP-return-address routing). The receiver's
  `connections_` map is keyed on the truncated id (populated from
  packet headers in `RemoteNode::unwrap`), so the full-string stamp
  would have caused silent resend-routing loss for any configured id
  ≥ 8 chars — and `"starlink"` is exactly 8. Fix: truncate the stamp
  to `maximum_connection_id_size - 1` in `resendMissingPackets`,
  mirroring what `WrappedPacket` already does at
  `wrapped_packet.cpp:31-32`. Round-2 also added the
  `DispatchHonorsTruncatedConnectionId` test as a regression guard.
  Not addressed here: raising the 8-byte limit (would require a
  wire-format change to every data packet header — separate issue).

- **try/catch on `decodeResendRequest` (round-2 fix).** The new
  `ResendRequest.msg` comment originally claimed the receiver wrapped
  decode in try/catch (mirroring `MessageInternal.msg`'s comment) —
  review caught that no such wrap actually existed at
  `decodeResendRequest`, *or* at `decodeMessageInternal`. Round-2 added
  the wrap at the `decodeResendRequest` site this PR introduces and
  tightened the message comment to be specific (not a systemic claim).
  The pre-existing `decodeMessageInternal` gap is left for a follow-up
  issue rather than expanding scope.

- **Dispatch-miss WARN once-per-id (round-2 fix).** Review surfaced
  that the original DEBUG-only log on dispatch lookup miss was too
  quiet for genuine coordinated-redeploy mismatches (operator chasing
  "resends stopped working" wouldn't see it at default log levels).
  Promoted to WARN on first miss per id (state bookkeeping in a small
  set on RemoteNode), DEBUG on subsequent — surfaces real config
  errors without spamming during legitimate CONNECT-cycle races.

- **Receive-side connection-id clamp (round-3 fix).** Copilot's PR-25
  review flagged that `RemoteNode::dispatch_miss_warned_ids_` is
  populated from a network-supplied string with no enforced upper
  bound. The send side already truncates to
  `maximum_connection_id_size - 1` (the sender-stamp truncation
  introduced in round-2 above), but the receive side did no
  validation — `ResendRequest.msg` declares `string connection_id`
  unbounded. Two failure modes followed: (a) memory growth of the
  WARN-once bookkeeping set under sustained corrupted-but-
  deserializable packets or a misbehaving peer; (b) silent
  resend-routing miss for any older or buggy peer that didn't apply
  the truncation contract (its full-length id would never match the
  truncated key in `connections_`). Round-3 fix: clamp
  `rr.connection_id` to `maximum_connection_id_size - 1` in
  `decodeResendRequest` right after the deserialize, mirroring the
  sender-side `assign(..., 0, maximum_connection_id_size - 1)` at
  `udp_bridge.cpp:1034-1035`. The clamp is a one-line invariant; the
  existing `DispatchHonorsTruncatedConnectionId` test already covers
  the downstream behavior the clamp leans on (truncated-form lookup
  matches; untruncated misses). A dedicated test for the clamp itself
  would require exposing the private `decodeResendRequest` or
  refactoring it into a free helper, which is disproportionate for a
  one-line resize on a value field.

- **Doc-comment + test-name polish (round-3).** Two stale-wording
  findings from the same Copilot review: the `dispatchResendRequest`
  header comment in `remote_node.h` still said "emit a DEBUG log" even
  though round-2 promoted it to WARN-first/DEBUG-subsequent; updated
  to match the impl. And two dispatch-miss tests in
  `test_remote_node_resend.cpp` were named `DispatchSilentlyDrops*`
  with prose saying "must no-op silently", which wrongly implied no
  diagnostic output. Renamed to `DispatchDropsWithoutBroadcast*` and
  reworded the surrounding comments — the invariant under test is
  "no broadcast fallback," not "no log output."

- **Header hygiene + in-package ABI alignment (round-5 fix).** Two
  precise findings from Copilot's third review (against the round-4
  push at `5225c2e`):
  1. **Missing `<set>` include.** Round-4 dropped `#include <set>`
     from `remote_node.h` when replacing the WARN-bookkeeping set,
     but `given_up_packet_numbers_` at line 260 still uses
     `std::set<uint64_t>`. The build was passing only because
     `udp_bridge.h:10` transitively pulled in `<set>`. Re-added the
     direct include alongside the new `<deque>` / `<unordered_set>`.
  2. **In-package ABI mismatch on `udp_bridge_node`.** The library
     target was being compiled with `UDP_BRIDGE_BUILD_TESTING`
     defined under `BUILD_TESTING=ON` (larger `Connection` /
     `RemoteNode` layouts), but the `udp_bridge_node` executable
     target was not — and its translation unit includes `udp_bridge.h`
     which directly includes `connection.h`, so the executable
     compiled against the production layout while linking a
     test-augmented library. The original CMake comment block called
     out the same hazard for **downstream-package** consumers but
     didn't extend the discipline to **in-package** consumers
     (executable + tests). Test targets were already covered by the
     `add_udp_bridge_gtest()` helper added in round-2; the executable
     was the remaining gap. Fix: add
     `target_compile_definitions(udp_bridge_node PRIVATE UDP_BRIDGE_BUILD_TESTING)`
     inside the existing `if(BUILD_TESTING)` block, with a new
     comment paragraph documenting why the executable needs the same
     discipline as the test targets. The mismatch hadn't bitten us
     in practice because the executable never allocates `Connection`
     by value and no inline methods on Connection touch conditional
     members, but it's UB per the standard and is exactly the kind
     of latent ABI hazard the existing CMake comment was already
     designed to prevent.

  This also walks back the "false positive" classification I'd
  attached to Copilot's original CMake/ODR finding in the round-1
  triage. The finding's framing — "downstream targets that link
  against this library" — was technically correct only for the
  downstream-package case I covered (where no consumers exist
  outside udp_bridge), but the underlying principle (layout-changing
  macros in installed headers are an ODR hazard for any consumer)
  applied to the in-package executable I had failed to audit.

- **Bounded FIFO for dispatch-miss WARN bookkeeping (round-4 fix).**
  Copilot's second review on PR #25 (against the round-3 push) flagged
  that the round-3 receive-side clamp only bounded each entry's *size*
  (7 bytes), not the *count* of distinct ids. A peer could still
  drive `dispatch_miss_warned_ids_` count upward without bound, and the
  field comment still claimed boundedness it didn't enforce. Round-4
  replaces the `std::set<std::string>` with a bounded FIFO: a
  `std::deque<std::string>` for insertion order plus an
  `std::unordered_set<std::string>` for O(1) presence checks, capped at
  `kDispatchMissWarnedCap = 256` entries. On insertion past the cap,
  the oldest entry is evicted from both containers — that id becomes
  WARN-eligible again the next time it's seen, which is the right
  behavior for a rate-limit-the-WARN table (versus permanently
  silencing a stale id). Cap value is chosen to be comfortably above
  any plausible configured id space (typical deployments have 2–4
  connections). A `UDP_BRIDGE_BUILD_TESTING`-gated
  `dispatchMissWarnedIdCountForTest()` accessor mirrors the existing
  test-only-accessor pattern and is used by a new
  `DispatchMissWarnedIdsAreBounded` test that pushes 1024 distinct ids
  and asserts the bookkeeping size converges well below the push count.
  Field comment on `dispatch_miss_warned_ids_` rewritten to describe
  what's actually enforced and why.
