# Plan: Relay support: receive-time remote→remote forwarding (cloud-hub topology)

## Issue

https://github.com/rolker/udp_bridge/issues/51

## Context (RE-PLAN — direction changed)

The prior plan relayed via `ignore_local_publications=false` (ROS loopback)
and tried to carry an `origin_remote_node` field on `PublishItem` across that
hop. Plan Review (`91393ec`) rejected it: `callback()` (`src/udp_bridge.cpp:722`)
— the only place the loop check could run — receives only
`(topic_name, topic_type, shared_ptr<SerializedMessage>)` from the ROS
subscription; the pub→sub round trip carries no provenance side-channel. A
second must-fix showed the opt-in leaked across connections sharing a
subscription.

**New direction:** relay entirely inside `decodeData()` at receive time, no
ROS loopback, no `PublishItem` change. `decodeData()`
(`src/udp_bridge.cpp:902`) already has `topic` and `outer_message.data` (raw
bytes as received) plus `source_info.node_name` (immediate sender, from
`unwrap()`/`wrapped_packet->source_node` — same key used for
`remote_nodes_.find(...)`, ~1330). The routing table
`subscribers_[topic].remote_details` (`include/udp_bridge/types.h:35`) is what
`callback()` already iterates (722-765) and is populated by
`addSubscriberConnection` regardless of whether a local ROS publisher exists
(`updateLocalSubscriptions()`, 1236, no-ops when `get_publishers_info_by_topic`
is empty — the boat-topic-on-a-hub case). The prior plan's "provenance storage
granularity" decision is moot and dropped.

## Approach

1. **Per-destination opt-in (default off).** Add `bool relay = false;` to
   `ConnectionRateInfo` (`types.h:10`, same granularity as `period`). Add
   `remotes.<r>.connections.<c>.topics.<t>.relay` in the `topics_list` loop
   (`src/udp_bridge.cpp` ~470-520), `declareIfMissing(..., false)`, threaded
   through a new trailing `bool relay = false` on `addSubscriberConnection`
   (`udp_bridge.h:284`, `udp_bridge.cpp:1172`) into
   `rd.connection_rates[connection_id].relay`. `decodeSubscribeRequest`
   (1308) keeps the default — relay is static-config only for now. Living on
   the same per-connection map `callback()` reads means a `relay: false`
   connection can never receive forwarded traffic regardless of a sibling's
   setting (fixes must-fix #2); no `ignore_local_publications` change at all.

2. **Forwarding, called from `decodeData()`, not `callback()`.** Add
   `relayToOtherRemotes(topic, outer_message, source_info)`, called near the
   top of `decodeData()` (902, after `topic` is computed, before the
   stale/reorder gate — every accepted packet is offered for relay
   independent of local publish/drop decisions, since a downstream bridge
   assigns its own fresh sequence on forward, `send()` at 1540). It locks
   `subscribers_mutex_`, looks up `subscribers_[topic]`, and for each
   `remote_details` entry whose key **!= `source_info.node_name`** (the loop
   rule — plain string compare) with `relay == true` on a connection, applies
   the same period/`last_sent_time` gating `callback()` already does
   (748-765) — extract that block into a shared private helper
   `selectRateLimitedConnections(remote_details, now, exclude_remote)` used
   by both, so rate limiting is identical for local and relayed traffic, not
   a parallel path. For each selected `(remote, connection)`, copy
   `outer_message` (same bytes, no re-serialization) with
   `destination_topic`/`reliability`/`durability`/`history_depth` swapped per
   destination, and call the existing `send(message_internal, connections,
   false)` (1506) — `Connection::send()` (`connection.cpp:404`) applies its
   own AIMD byte-rate admission control (#43/#52) identically either way.

3. **Threading: hand off to a worker, don't call `send()` inline on the
   drain thread.** `decodeData()` runs on `socket_drain_group_` (`spin_once`,
   541, `MutuallyExclusive`) — the thread #10 forbids blocking.
   `Connection::send()` (`connection.cpp:514`) is UDP `sendto()`, not ROS
   `publish()` — no subscriber block — but its own comment (545-560)
   documents `MSG_DONTWAIT` with a 20×10ms poll (~200ms worst case) when the
   send buffer is full. Today that cost is paid on `republish_group_`, a
   *different* group, specifically so a stalled send can't starve `recvfrom`
   (1280). Calling it inline in `decodeData()` reintroduces the #10 hazard
   (N stalled destinations stack ~200ms×N ahead of the next socket read).
   Mirror the existing `PublishQueue` fix for the same problem: new
   `include/udp_bridge/relay_queue.h` (`RelayItem`/`RelayQueue`, same shape
   as `publish_queue.h` — bounded, drop-oldest, single worker, injected
   `Sink`). `decodeData()` pushes a cheap `RelayItem{topic, outer_message,
   source_info.node_name}` and returns; a new `relay_queue_` worker
   (configure/start/stop mirroring `publish_queue_`, 564-611) drains it and
   runs `relayToOtherRemotes` off both the drain thread and the publish
   worker.

4. **Tests** (`test/`, `test_reorder_buffer.cpp`/`test_admission_control.cpp`
   style): three-node relay (A→hub→B, hub also locally subscribed — local
   subscriber and B both get it); echo regression (symmetric pair, `relay:
   true` both ways, A never gets its own message back); per-destination
   opt-in isolation (two destinations on one source topic, only one `relay:
   true` — replaces the old provenance-retention test, moot under this
   design); rate-limit parity (a `period` on a relay destination throttles
   identically to a local-origin destination with the same `period`,
   exercising `selectRateLimitedConnections`).

5. **Docs** (same PR): new `doc/relay_design.md` (format of
   `qos_design.md`/`admission_control_design.md`) — default-off rationale;
   the exact loop rule and that it is direct-neighbor-only (`source_info.node_name`
   is always the last hop, so a relay-enabled 3-node cycle A-B-C-A duplicates
   indefinitely — star topology only, boat→hub→operators, cycles/mesh
   unsupported); **Security** section cross-referencing open #53
   (unauthenticated transport — a hub widens blast radius across 3+ parties).
   `config/example_params.yaml`: commented `relay: false` near existing
   `period`/`queue_size` (~85-93). `README.md`/`doc/conceptual_overview.md`:
   describe the capability and star-only constraint. `.agents/README.md`
   line 92: append `relay` (default `false`) to the per-topic list.

Commit sequence (atomic): opt-in flag → `relay_queue_` scaffolding (unused) →
`relayToOtherRemotes` + helper extraction + wiring → tests → docs.

## Files to Change

| File | Change |
|------|--------|
| `include/udp_bridge/types.h` | `bool relay = false;` on `ConnectionRateInfo` |
| `include/udp_bridge/udp_bridge.h` | `relay` param on `addSubscriberConnection`; declare `relayToOtherRemotes`, `selectRateLimitedConnections`, `relay_queue_` |
| `include/udp_bridge/relay_queue.h` | New: `RelayItem`/`RelayQueue`, mirrors `publish_queue.h` |
| `src/udp_bridge.cpp` | `relay` param parsing; `addSubscriberConnection` stores it; extract `selectRateLimitedConnections`; add `relayToOtherRemotes`; call from `decodeData()`; `relay_queue_` lifecycle wiring |
| `test/` (new) | Three-node relay, echo regression, opt-in isolation, rate-limit parity |
| `doc/relay_design.md` | New design doc |
| `config/example_params.yaml` | Document `relay` flag |
| `README.md`, `doc/conceptual_overview.md` | Describe relay + star-only scope |
| `.agents/README.md` | Add `relay` to per-topic parameter list |

## Principles Self-Check

| Principle | Consideration |
|---|---|
| Capture decisions | `doc/relay_design.md`: default-off, loop rule, why `PublishItem` provenance was rejected, star-only scope |
| Consequences included | `example_params.yaml`, `README.md`, `conceptual_overview.md`, `.agents/README.md` all in this PR |
| Test what breaks | Tests target opt-in leakage (must-fix #2) and drain-thread blocking (#10), plus echo/rate-limit parity |
| Only what's needed | Default `relay: false` leaves every config byte-for-byte unaffected; no `ignore_local_publications` change |
| Human control | Opt-in per destination connection via existing explicit config |
| Improve incrementally | One PR, atomic commits |

## ADR Compliance

| ADR | Triggered | How addressed |
|---|---|---|
| (none) | No | Project-repo change; follows `udp_bridge`'s `doc/*_design.md` convention |

## Consequences

| If we change... | Also update... | Included? |
|---|---|---|
| `ConnectionRateInfo` gains `relay` | `BridgeInfo` diagnostics (~1801) could surface it | No — follow-up, not required for correctness |
| New `relay_queue_` worker | Lifecycle wiring (configure/activate/deactivate/cleanup) | Yes |
| New per-topic `relay` param | `example_params.yaml`, `README.md`, `conceptual_overview.md`, `.agents/README.md` | Yes |
| Relay widens hub blast radius | `doc/relay_design.md` Security section, #53 | Yes |
| `callback()`'s gating logic extracted into shared helper | `callback()` must keep identical behavior | Yes — existing tests + new parity test |

## Documentation & Instruction Impact

- **Stale docs** (this PR): `README.md`/`doc/conceptual_overview.md` describe
  only one-directional flow today; `.agents/README.md`'s parameter table.
- **Agent-instruction candidates**: None — codebase-specific engineering
  choice, not a generalizable pattern.

## Open Questions

- None. Both must-fix findings from the prior review are resolved by
  construction: the loop check runs where provenance is already in hand (no
  ROS round trip), and opt-in is enforced per destination connection at the
  exact forwarding decision point.

## Estimated Scope

Single PR, five atomic commits (opt-in flag → relay queue scaffolding →
forwarding logic + refactor + wiring → tests → docs).
