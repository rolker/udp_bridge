# QoS Design

This document describes how `udp_bridge` chooses Quality-of-Service (QoS) policies
on its publishers and subscribers. It exists so the next maintainer doesn't have
to re-derive the trade-offs from scratch — every QoS question in this package
should have a written answer here.

> **Operational note (2026-05-01)**: the destination publisher's reliability
> default has been temporarily switched from `BEST_AVAILABLE` to `RELIABLE`
> in `qos_resolution.h` because `rmw_zenoh_cpp` 0.2.9 fails to encode
> `BEST_AVAILABLE` into its liveliness keyexpr, leaving the publisher
> invisible to graph-discovery clients (e.g. CAMP's `NavSource`, which
> gates on `get_topic_names_and_types`). The rest of this document
> describes the *design intent*; the operational default flips back to
> `BEST_AVAILABLE` once the rmw issue is resolved. To opt into
> `BEST_AVAILABLE` per topic before then, set `reliability:
> best_available` explicitly in the connection config.

## Mental model: the bridge is a UDP transport

The bridge sits between two ROS 2 graphs and carries serialized ROS 2
messages across a UDP wire. End-to-end reliability across that wire is
necessarily **best-effort with loss reduction**:

- The resend layer (see `RemoteNode::getMissingPackets` and
  `Connection::resend_packets`) reduces loss by re-requesting missing packet
  numbers within a bounded TTL window.
- It does not — and cannot — make the wire RELIABLE in the QoS sense. A
  packet that is dropped after its sender-side TTL expires is permanently
  lost; no amount of QoS configuration on either endpoint changes that.

**Consequence**: any QoS claim more aggressive than "best-effort with loss
reduction" on the destination publisher is a lie. Specifically, declaring
the destination publisher RELIABLE means it reports write success on data
that was dropped mid-flight, and it inherits the destination-side cost of
RELIABLE (the publisher buffers messages and retries delivery until
acknowledged; this can stall under back-pressure if a subscriber goes
away mid-stream — see issue #10's hypothesized wedge mechanism, observed
under `rmw_zenoh_cpp` but not specific to it).

The bridge's job is to *deliver what survived the wire* and let local ROS 2
endpoints decide what contract they want with each other. It is not the
bridge's job to assert a contract about the wire that the wire cannot
honor.

## Receive-path publish decoupling (issue #10)

The wedge described above is **not** fixed by any QoS choice, and the fix is
deliberately independent of the reliability policy:

- `BEST_EFFORT` on the destination publisher would refuse `RELIABLE`
  subscribers (CAMP goes dark — see the worked example below), so it is not
  an option.
- `BEST_AVAILABLE` is the design intent but (a) is currently blocked by the
  `rmw_zenoh_cpp` 0.2.9 graph-visibility bug, and (b) still behaves
  `RELIABLE` *for the local hop* whenever a `RELIABLE` subscriber matches —
  so it can still block on a dead-but-still-matched subscriber.

So the wedge is fixed at the **transport layer instead**: the rmw-touching
tail of `decodeData` — find-or-create the destination publisher, the
first-arrival `sendBridgeInfo`, and `publish()` — no longer runs on the
socket-drain thread. `decodeData` deserializes the `MessageInternal` and
enqueues a `PublishItem` on a bounded, single-worker `PublishQueue`
(`include/udp_bridge/publish_queue.h`); the worker does the create/publish.
A `RELIABLE` publish stalling on an uncleanly-dead subscriber — or a
first-arrival `create_generic_publisher()` blocking on rmw discovery — now
blocks only the worker. `recvfrom` keeps draining; the kernel `Recv-Q` does
not back up. This holds regardless of whether the destination publisher is
`RELIABLE` (current operational default) or `BEST_AVAILABLE` (design intent).

**Queue drops are unrecoverable, and distinct from wire loss.** When a local
subscriber stalls the publish path long enough to exceed the queue's byte
budget (`publish_queue_max_bytes`), the queue drops the **oldest** items
(KEEP_LAST(1)-consistent: newest wins) and counts them in a diagnostic. This
is loss that happens *after* successful wire delivery and reassembly, so the
resend layer — which operates on packet numbers *upstream* of `decodeData` —
cannot recover it. It is the local-hop expression of the same
"best-effort with loss reduction" model: the bridge would rather drop a
republished message than let one stalled subscriber wedge the reader for
every topic. The drop count surfaces as a `publish queue` diagnostic
(WARN on recent drops) so a stalled subscriber is visible to operators
rather than silent.

A single worker means a stalled publish can still delay *other* republished
topics queued behind it (head-of-line) until they age out of the byte
budget — bounded and observable, but not isolated. Per-publisher isolation
(a worker per topic, so a dead costmap subscriber can't delay video) is a
deferred follow-up; the reported field wedges were all the reader-thread
wedge this decoupling removes.

## How `BEST_AVAILABLE` matching works

`BEST_AVAILABLE` is a special QoS value (see
[REP-2003](https://github.com/ros-infrastructure/rep/blob/master/rep-2003.rst))
that means **"I don't care; match whatever the other endpoint asks for."**
It's available on three QoS dimensions: reliability, durability, and
liveliness. This package leans on it for reliability on the destination
publisher — it is why the publisher matches both RELIABLE and BEST_EFFORT
subscribers without forcing the bridge to choose for them.

The matching mechanism, simplified:

1. An endpoint declares `BEST_AVAILABLE` on a dimension. The rmw layer
   marks it as a placeholder rather than a concrete request.
2. During discovery, when matching against a counterparty, the
   `BEST_AVAILABLE` side adopts the counterparty's choice for that
   dimension.
3. If both sides are `BEST_AVAILABLE`, the result falls back to the
   lenient default — `BEST_EFFORT` for reliability, `VOLATILE` for
   durability.

The reliability matching cases:

| Subscriber declares | Publisher declares | Match? | Publisher's effective behavior |
|---|---|---|---|
| `RELIABLE` | `RELIABLE` | yes | `RELIABLE` |
| `RELIABLE` | `BEST_EFFORT` | **no** — subscriber requires reliability the publisher won't provide | — |
| `RELIABLE` | `BEST_AVAILABLE` | yes | `RELIABLE` (publisher buffers + retries on its local hop) |
| `BEST_EFFORT` | `RELIABLE` | yes | `RELIABLE` (subscriber accepts the stronger contract) |
| `BEST_EFFORT` | `BEST_AVAILABLE` | yes | `BEST_EFFORT` |
| `BEST_AVAILABLE` | `BEST_AVAILABLE` | yes | `BEST_EFFORT` (lenient default) |

`BEST_AVAILABLE` is implemented at the rmw layer, so it works across rmw
implementations. `rmw_cyclonedds_cpp` and `rmw_fastrtps_cpp` map it onto
the corresponding DDS QoS feature; `rmw_zenoh_cpp` maps it onto Zenoh's
session-level QoS negotiation. The contract surface is the same — what
differs between rmws is the underlying transport mechanism that
implements RELIABLE delivery once a match is established.

It was introduced in Iron Irwini and is fully usable in Jazzy
(verified at `/opt/ros/jazzy/include/rclcpp/rclcpp/qos.hpp:190` —
`reliability_best_available()` is the C++ setter on `rclcpp::QoS`).

**Caveat specific to `udp_bridge`**: when a `RELIABLE` subscriber matches
the bridge's `BEST_AVAILABLE` destination publisher, the publisher does
behave `RELIABLE` — *for its local hop only*, between the bridge process
and the destination subscriber. It cannot make the upstream UDP wire
reliable; data that fell off the wire before `decodeData` was ever called
is gone. The "transparency lie" warned about in the mental model isn't
fixed by `BEST_AVAILABLE`; it's just kept from becoming a hard
QoS-matching wall on top of it.

## Per-dimension policy

ROS 2 QoS has several dimensions. Not all of them translate meaningfully
across a UDP wire. The package's policy per dimension:

### Reliability

- **Destination publisher** (`udp_bridge.cpp::decodeData`, where messages
  arriving over UDP are republished into the local ROS 2 graph): design
  intent is `BEST_AVAILABLE`, so the publisher matches whatever the local
  subscriber declares — RELIABLE subscribers connect, BEST_EFFORT
  subscribers connect, and the destination side's reliability is owned by
  the subscriber rather than asserted by the bridge.
  > **Current operational default: `RELIABLE`** — see the operational
  > note at the top of this document for the rmw_zenoh_cpp 0.2.9
  > graph-visibility workaround. RELIABLE matches both RELIABLE and
  > BEST_EFFORT subscribers, so the matching outcomes described in the
  > rest of this section remain correct; only the "publisher behavior"
  > column collapses to RELIABLE rather than being subscriber-driven.
- **Source-side subscription** (`udp_bridge.cpp::updateLocalSubscriptions`,
  where the bridge subscribes locally to topics it forwards): uses
  `BEST_EFFORT`. A `BEST_EFFORT` subscriber accepts both `RELIABLE` and
  `BEST_EFFORT` source publishers, so this is the universally compatible
  choice. Switching it to `BEST_AVAILABLE` is possible but adds no
  functional value here — `BEST_EFFORT` already accepts everything.

### Durability

- **Default**: `VOLATILE` on both ends.
- **Opt-in `TRANSIENT_LOCAL`** via per-topic configuration. Set when the
  topic has latching semantics that should survive across the bridge
  (static maps, configuration topics, mission specs). When enabled:
  - The source-side subscription uses `TRANSIENT_LOCAL` durability so the
    bridge actually receives a latched value when it joins late.
  - The destination publisher uses `TRANSIENT_LOCAL` durability so
    late-joining destination subscribers see the latest value.
  - The bridge already caches the most recent message per topic for
    resend purposes, so the implementation overhead is small.
- Durability is **not auto-mirrored** from the source publisher. The
  bridge does not introspect source QoS to set destination QoS — see
  "Future work" below for why.

### History / depth

- **Always `KEEP_LAST(N)`** on the destination publisher.
- **N defaults to 1** and is configurable per topic via the
  `history_depth` parameter.
- `KEEP_ALL` is not an option — it does not match the bridge's role
  (UDP-bridged data is inherently lossy; "keep all" semantics are
  meaningless for data that may not arrive).

### Deadline / Lifespan / Liveliness

- **Not translated across the wire.**
- These are local-process contracts: a deadline says "I expect a message
  every X seconds from this publisher"; a lifespan marks messages as
  expired after Y seconds; liveliness asserts the publisher is alive on
  some cadence. None of these properties survive serialization,
  fragmentation, network transit, and reassembly in a meaningful way.
- The "is the wire alive?" question is answered by the bridge's own
  `bridge_info` heartbeat topic on a 2-second cadence, not by rmw-level
  liveliness assertions on user topics.

## How configuration works

QoS is communicated end-to-end via three fields on
`udp_bridge_interfaces/msg/MessageInternal`:

- `string reliability` — `"reliable"` (current operational default — see
  the operational note at the top of this document; design intent is
  `"best_available"`), `"best_effort"`, or explicit `"best_available"`.
- `string durability` — `"volatile"` (default), `"transient_local"`.
- `uint32 history_depth` — defaults to 1 if unset (zero is treated as
  default).

The sender populates these fields from per-topic parameters declared
under `remotes.<remote>.connections.<connection>.topics.<topic>.*`
(reliability, durability, history_depth — alongside the existing
queue_size, period, source, destination). The receiver applies them
when it first creates the destination publisher in `decodeData`.

For source-side subscription QoS (the bridge's local subscriber), the
relevant per-topic parameter is `durability` — when set to
`transient_local`, the bridge subscribes with `TRANSIENT_LOCAL`
durability so it actually receives latched messages.

### Mismatched-pair compatibility

`MessageInternal` historically did not carry QoS fields. The PR that
added them extends the on-wire schema with three trailing fields
(`reliability`, `durability`, `history_depth`). New senders and old
receivers, or vice versa, may briefly run on opposite sides of the
bridge during a partial deployment or a field-mode hotfix on one host.

The package's official supported configuration is **coordinated
redeploy**: when `MessageInternal` changes, both endpoints must be
redeployed together. The schema does not guarantee wire-level
interoperability across the two versions — `rclcpp::Serialization` may
fail to deserialize a buffer whose type hash, encapsulation header, or
field layout doesn't match what the receiver expects, and the exact
behavior depends on the active rmw / CDR implementation. We have not
verified the trailing-field-tolerance behavior under any specific rmw
in this package's CI.

What the package *does* guarantee:

- The receiver's resolver (`resolveDestinationPublisherQos`) treats
  empty/zero values as "use the package defaults", so a successfully-
  deserialized message from any version produces a coherent QoS — that
  is what `test_qos_resolution.cpp::OldSenderAllFieldsMissing*`
  exercises. This test exercises the resolver in isolation; it does
  NOT assert anything about wire-level cross-version interoperability.
- `UDPBridge::decode()` catches `std::exception` from any per-packet
  decoder and logs + drops the bad packet. So a transient mismatched-
  pair scenario produces logged decode errors and dropped messages,
  not a crashed bridge process.

Operators bringing one endpoint forward without the other should expect
elevated `decoding error` log lines and missed traffic until both sides
match.

## A worked example: the CAMP RELIABLE-subscriber concern

The operator-side display (CAMP, in `layers/main/ui_ws/src/camp/`)
subscribes to many bridged topics with the default `rclcpp::QoS(N)` — i.e.,
`RELIABLE` reliability:

```cpp
// camp/src/camp2/ros/grids/grid_map.cpp:25
rclcpp::QoS qos(1);
qos.durability_best_available();
// ...not setting reliability, so reliability defaults to RELIABLE
```

If the bridge's destination publisher were declared `BEST_EFFORT`,
ROS 2's QoS-matching rules would refuse to connect a `RELIABLE`
subscriber to it — the operator-side view would silently go dark
on every topic that came through the bridge. With
**`BEST_AVAILABLE` on the destination publisher**, the publisher
matches whatever the subscriber asks for, including `RELIABLE`, and
the connection succeeds.

> **Operationally today**: the destination publisher defaults to
> `RELIABLE` (per the note at the top of this document). RELIABLE
> matches both RELIABLE and BEST_EFFORT subscribers, so CAMP's
> default-`rclcpp::QoS(N)` subscribers still connect; the matching
> guarantee is preserved by the temporary default. The trade-off is
> the "transparency lie" on UDP-fed publishers (publisher reports
> write-success on data dropped on the wire), accepted while the
> rmw_zenoh_cpp graph-visibility issue is in effect.

This package ships **no per-topic `reliability: reliable` overrides** in
the initial deployment of this design. Under the **design-intent default**
(`BEST_AVAILABLE`), the CAMP-style mitigation is entirely subsumed by
`BEST_AVAILABLE` matching. Under the **current operational default**
(`RELIABLE`, per the operational note at the top of this document), every
destination publisher already asserts RELIABLE for the rmw_zenoh_cpp
graph-visibility reason — so the "no overrides" claim above is about the
post-workaround design state, not the current operational reality. If a
future use case emerges that genuinely requires the destination publisher
to assert `RELIABLE` semantics on the destination side (independent of
what the subscriber declares, and independent of the current workaround),
the per-topic `reliability: reliable` override is the right knob — but
the bar is high, because asserting RELIABLE on a UDP-fed publisher is the
lie this design avoids.

## Future work (deliberately out of scope)

### Source-QoS advertisement in `bridge_info`

The sender could query `topic_endpoint_info` for each forwarded topic
and advertise the source publisher's QoS in its `bridge_info` heartbeat.
This would let the receiver:

- Log a warning when the source claims `RELIABLE` but the bridge
  delivered `BEST_AVAILABLE` (i.e., make the wire's lossiness explicit
  in operational logs).
- Inform per-topic configuration recommendations to a future operator
  tool ("you might want `transient_local` on this topic — the source is
  latching it").

This is *informational*. It does not change the destination publisher's
QoS choice — that remains driven by per-topic config (or the
`BEST_AVAILABLE` default).

### Source-QoS auto-mirror

A more aggressive version of the above would auto-derive the destination
publisher's QoS from the observed source publisher's QoS. This is
deliberately *not* part of the contract because the obvious-looking
auto-mirror algorithm has no good answer for several edge cases:

- Multiple source publishers on the same topic with different QoS
  (which one wins?).
- Source publisher restarts mid-stream with a different QoS (how does
  the destination publisher's contract change without breaking
  subscribers that have already matched?).
- Multiple destination subscribers with conflicting QoS expectations
  (which subscriber's preference wins?).

The explicit per-topic configuration (`reliability` / `durability` /
`history_depth` parameters) handles the realistic 90% case correctly
today and surfaces the edge cases at configuration time rather than at
runtime.

## References

- Issue [#11](https://github.com/rolker/udp_bridge/issues/11) — robustness
  changes that introduced this design.
- Issue [#10](https://github.com/rolker/udp_bridge/issues/10) — bridge
  wedge (RELIABLE publisher stalling on a dead subscriber) that motivated
  the reliability policy and is fixed by the receive-path publish
  decoupling described above.
- Issue [#9](https://github.com/rolker/udp_bridge/issues/9) — resend
  amplification, the package's actual loss-reduction layer.
- Plan: `.agent/work-plans/PLAN_ISSUE-11.md`.
- ROS 2 QoS overview:
  [Quality of Service](https://docs.ros.org/en/jazzy/Concepts/About-Quality-of-Service-Settings.html)
  — defines reliability, durability, history, deadline, lifespan,
  liveliness.
- `BEST_AVAILABLE` policy:
  [REP 2003](https://github.com/ros-infrastructure/rep/blob/master/rep-2003.rst)
  — formal definition of the matching rules.
