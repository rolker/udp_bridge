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

A second revision then removed the relay opt-in flag altogether (operator
direction, ROS 1 precedent): the per-remote topic lists already *are* the
routing table, and the flag guarded an echo the loop rule prevents on its
own. See Approach step 1.

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

1. **No relay flag — the topic lists are the routing table.** This is ROS 1
   semantics restored: if remote B's `topics_list` contains topic X, B has
   asked for X, and the hub delivers it whether X was published locally or
   arrived from remote A. An earlier draft added a per-destination `relay`
   opt-in defaulting off; it was removed as protection against nothing:

   - The echo it guarded against cannot occur here. This design does **not**
     touch `ignore_local_publications`, so a bridge still never hears its own
     republish; and the loop rule independently refuses to send back to the
     sender. A symmetric two-host pair is bit-for-bit unchanged.
   - Relay only alters behaviour where a *second* remote lists the same
     topic. Every config in the repo declares exactly one remote per bridge
     (`config/example_params.yaml:33` `["robot_a"]`;
     `test/bench/configs/three_path.yaml:30,56` `["boat"]`/`["operator"]`),
     so a default-off flag would have been guarding configs in which relay
     is unreachable regardless.
   - Cost of keeping it: a new parameter across four docs, a threading path
     through `addSubscriberConnection` that a dynamic `decodeSubscribeRequest`
     could silently reset back to `false` (round-2 review finding 1), and an
     extra filter in the rate-limiting helper whose ordering is easy to get
     wrong (finding 2). Both findings disappear with the flag.

   Consequence to state in the docs: adding a second remote that lists an
   already-relayed topic now starts that traffic flowing. That is the
   feature, and the topic list is the request — but it is a behaviour change
   for any future multi-remote config, so `doc/relay_design.md` must say so.

2. **Forwarding, called from `decodeData()`, not `callback()`.** Add
   `relayToOtherRemotes(topic, outer_message, source_info)`, called near the
   top of `decodeData()` (902, after `topic` is computed, before the
   stale/reorder gate — every accepted packet is offered for relay
   independent of local publish/drop decisions, since a downstream bridge
   assigns its own fresh sequence on forward, `send()` at 1540).

   *Revised after pre-push review (operator decision): relay follows the
   gate, not precedes it.* Because forwarding assigns fresh sequence
   numbers, a downstream bridge cannot recognise a relayed stale packet as
   stale and would republish older data as newest. `decodeData()` now only
   PROBES for relay destinations; the relay form rides inside the
   `PublishItem` (an optional `RelayItem`, so a reorder-buffered packet
   keeps it) and `UDPBridge::enqueuePublish()` — the single admit-to-publish
   point — hands it to `relay_queue_` for exactly the packets the gate
   admits. Moving the handoff there also removed the payload copy that ran
   on the socket-drain thread (`outer_message` is moved, not copied). It locks
   `subscribers_mutex_`, looks up `subscribers_[topic]`, and for each
   `remote_details` entry whose key **!= `source_info.node_name`** (the loop
   rule — plain string compare), applies
   the same period/`last_sent_time` gating `callback()` already does
   (748-765) — extract that block into a shared private helper
   `selectRateLimitedConnections(remote_details, now, exclude_remote)` used
   by both, so rate limiting is identical for local and relayed traffic, not
   a parallel path.

   For each selected `(remote, connection)`, copy
   `outer_message` (same bytes, no re-serialization) with
   `destination_topic`/`reliability`/`durability`/`history_depth` swapped per
   destination, and call the existing `send(message_internal, connections,
   false)` (1506) — `Connection::send()` (`connection.cpp:404`) applies its
   own AIMD byte-rate admission control (#43/#52) identically either way.

   *As built:* the helper is a free function in a new header
   `include/udp_bridge/destination_selection.h`, not a private member of
   `UDPBridge`. It needs no member state (its inputs are the routing table
   and `now`), and as a free function the routing decision — the whole of
   what relay does — is unit-testable without a node or a socket, matching
   the `qos_resolution.h` / `giveup_diagnostic.h` / `subscriber_registry.h`
   precedent in this repo. The shared `DestinationConfig` struct
   (previously local to `callback()`) moved there with it. A companion free
   function `hasRelayDestination(remote_details, source_node)` is the
   read-only form of the same loop rule: `decodeData` calls it (through the
   private `UDPBridge::hasRelayDestinations`, which adds the
   `subscribers_mutex_` lookup) to skip the payload copy entirely when no
   other remote lists the topic — which is every configuration in this repo
   today.

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

   *As built:* `relayToOtherRemotes(RelayItem&& item)` takes the queue item
   (which carries exactly the planned `topic` / `outer_message` /
   `source_node`) so it can serve directly as the queue's sink, mirroring
   `publishItem`. Two additions the plan did not name, both consequences of
   adding a worker: the queue's byte budget, and a `relay queue` diagnostic task
   (`diagnoseRelayQueue`, mirroring `diagnosePublishQueue`) makes relay
   drops visible, since a dropped relay is unrecoverable: the message never
   reached a `Connection`, so the resend layer has nothing to retransmit.
   The budget was first built as a compile-time constant
   (`kRelayQueueMaxBytes`, 64 MiB) on the reasoning that relay adds no
   configuration surface; pre-push review found that a fixed value is a
   capability limit on the very queue that carries a hub's whole downstream
   fan-out, so it is now the `relay_queue_max_bytes` parameter, with the
   same default and clamps as `publish_queue_max_bytes`.

4. **Tests** (`test/`, `test_reorder_buffer.cpp`/`test_admission_control.cpp`
   style): three-node relay (A→hub→B, hub also locally subscribed — local
   subscriber and B both get it); echo regression (symmetric pair, A never
   gets its own message back — this is the test that earns the deletion of
   the relay flag, so it must fail if the loop rule is removed); routing-table
   fidelity (a remote whose `topics_list` omits X receives no relayed X even
   while a sibling remote receives it — relay follows the routing table and
   nothing else); rate-limit parity (a `period` on a relay destination
   throttles identically to a local-origin destination with the same
   `period`, exercising `selectRateLimitedConnections`).

5. **Docs** (same PR): new `doc/relay_design.md` (format of
   `qos_design.md`/`admission_control_design.md`) — why there is no relay
   flag (the topic lists are the routing table; the flag would have guarded
   an echo the loop rule already prevents, in configs where relay is
   unreachable anyway) and the resulting behaviour change for any future
   multi-remote config; the exact loop rule and that it is
   direct-neighbor-only (`source_info.node_name` is always the last hop, so a
   3-node cycle A-B-C-A duplicates indefinitely — star topology only,
   boat→hub→operators, cycles/mesh unsupported); that relay precedes the
   stale/reorder gate, so an upstream resend is forwarded downstream
   (bounded by admission control, but stated); **Security** section
   cross-referencing open #53 (unauthenticated transport — a hub widens blast
   radius across 3+ parties). `README.md`/`doc/conceptual_overview.md`:
   describe the capability and the star-only constraint.
   `config/example_params.yaml`: add a
   commented second-remote example showing what now relays and what does not.
   `.agents/README.md`: *as built,* one new row — `relay_queue_max_bytes`
   (see the revision noted above).

Commit sequence (atomic): `relay_queue_` scaffolding (unused) →
`relayToOtherRemotes` + helper extraction + wiring → tests → docs.

## Files to Change

| File | Change |
|------|--------|
| `include/udp_bridge/udp_bridge.h` | Declare `relayToOtherRemotes`, `hasRelayDestinations`, `diagnoseRelayQueue`, `relay_queue_` |
| `include/udp_bridge/relay_queue.h` | New: `RelayItem`/`RelayQueue`, mirrors `publish_queue.h` |
| `include/udp_bridge/destination_selection.h` | New (as built): free `selectRateLimitedConnections` / `hasRelayDestination` + shared `DestinationConfig`, so the routing decision is testable without a node |
| `src/udp_bridge.cpp` | Extract `selectRateLimitedConnections`; add `relayToOtherRemotes`; call from `decodeData()`; `relay_queue_` lifecycle wiring |
| `test/` (new) | Three-node relay, echo regression, routing-table fidelity, rate-limit parity |
| `doc/relay_design.md` | New design doc |
| `config/example_params.yaml` | Commented second-remote example showing what relays |
| `README.md`, `doc/conceptual_overview.md` | Describe relay + star-only scope |

No new parameter: `types.h` is untouched, `addSubscriberConnection` keeps its
signature, and `.agents/README.md`'s verified-parameter table needs no row.

## Principles Self-Check

| Principle | Consideration |
|---|---|
| Capture decisions | `doc/relay_design.md`: why no relay flag, loop rule, why `PublishItem` provenance was rejected, star-only scope |
| Consequences included | `example_params.yaml`, `README.md`, `conceptual_overview.md` all in this PR |
| Test what breaks | Tests target echo (the loop rule is now the *only* thing preventing it), routing-table fidelity, drain-thread blocking (#10), rate-limit parity |
| Only what's needed | No new parameter, no `types.h` change, no `ignore_local_publications` change; every existing config is single-remote, so behaviour is unchanged |
| Human control | Routing stays explicit — the operator's `topics_list` is the routing table, exactly as in ROS 1 |
| Improve incrementally | One PR, atomic commits |

## ADR Compliance

| ADR | Triggered | How addressed |
|---|---|---|
| (none) | No | Project-repo change; follows `udp_bridge`'s `doc/*_design.md` convention |

## Consequences

| If we change... | Also update... | Included? |
|---|---|---|
| New `relay_queue_` worker | Lifecycle wiring (configure/activate/deactivate/cleanup) | Yes |
| Relay follows the topic lists with no opt-in | A future config adding a second remote that lists an already-carried topic starts relaying it | Documented in `doc/relay_design.md`; no existing config is multi-remote |
| ~~Relay precedes the stale/reorder gate~~ → **relay follows the gate** (revised after pre-push review) | Only admitted packets are forwarded; a buffered packet carries its relay form and is relayed when released | Documented in `doc/relay_design.md`; pinned by `test_reorder_buffer` |
| Relay widens hub blast radius | `doc/relay_design.md` Security section, #53 | Yes |
| `callback()`'s gating logic extracted into shared helper | `callback()` must keep identical behavior | Yes — existing tests + new parity test |

## Documentation & Instruction Impact

- **Stale docs** (this PR): `README.md`/`doc/conceptual_overview.md` describe
  only one-directional flow today; `.agents/README.md`'s parameter table.
- **Agent-instruction candidates**: None — codebase-specific engineering
  choice, not a generalizable pattern.

## Open Questions

- None. Both round-1 must-fix findings are resolved by construction: the loop
  check runs where provenance is already in hand (no ROS round trip), and the
  opt-in leak is moot because there is no opt-in flag — relay follows the
  topic lists, which are per-destination by definition. Round-2 findings 1
  and 2 were both flag-plumbing concerns and disappeared with it; finding 3
  (relay vs. the stale/reorder gate) was carried as a doc line, then decided
  by the operator after the pre-push review: relay only what the gate
  admits. See the revision under step 2.

## Estimated Scope

Single PR, four atomic commits (relay queue scaffolding → forwarding logic +
helper extraction + wiring → tests → docs).
