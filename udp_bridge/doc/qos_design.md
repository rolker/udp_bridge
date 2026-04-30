# QoS Design

This document describes how `udp_bridge` chooses Quality-of-Service (QoS) policies
on its publishers and subscribers. It exists so the next maintainer doesn't have
to re-derive the trade-offs from scratch — every QoS question in this package
should have a written answer here.

## Mental model: the bridge is a UDP transport

The bridge sits between two DDS domains and carries serialized ROS 2 messages
across a UDP wire. End-to-end reliability across that wire is necessarily
**best-effort with loss reduction**:

- The resend layer (see `RemoteNode::getMissingPackets` and
  `Connection::resend_packets`) reduces loss by re-requesting missing packet
  numbers within a bounded TTL window.
- It does not — and cannot — make the wire RELIABLE in the DDS sense. A
  packet that is dropped after its sender-side TTL expires is permanently
  lost; no amount of QoS configuration on either DDS endpoint changes that.

**Consequence**: any QoS claim more aggressive than "best-effort with loss
reduction" on the destination publisher is a lie. Specifically, declaring the
destination publisher RELIABLE means it reports write success on data that
was dropped mid-flight, and it inherits the destination-side cost of RELIABLE
(per-subscriber writer state, history kept until acknowledged, stall risk if
a subscriber goes away mid-stream — see issue #10's hypothesized wedge
mechanism).

The bridge's job is to *deliver what survived the wire* and let local DDS
endpoints decide what contract they want with each other. It is not the
bridge's job to assert a contract about the wire that the wire cannot
honor.

## Per-dimension policy

ROS 2 QoS has several dimensions. Not all of them translate meaningfully
across a UDP wire. The package's policy per dimension:

### Reliability

- **Destination publisher** (`udp_bridge.cpp::decodeData`, where messages
  arriving over UDP are republished into the local DDS domain): uses
  `BEST_AVAILABLE`. The publisher matches whatever the local subscriber
  declares — RELIABLE subscribers connect, BEST_EFFORT subscribers
  connect, and the destination DDS hop's reliability is owned by the
  subscriber rather than asserted by the bridge.
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
  `bridge_info` heartbeat topic on a 2-second cadence, not by DDS
  liveliness assertions on user topics.

## How configuration works

QoS is communicated end-to-end via three fields on
`udp_bridge_interfaces/msg/MessageInternal`:

- `string reliability` — `"best_available"` (default), `"reliable"`,
  `"best_effort"`.
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

`MessageInternal` historically did not carry QoS fields. New senders and
old receivers, or vice versa, may run on opposite sides of the bridge
during a partial deployment.

- **New sender → old receiver**: ROS 2 message deserialization tolerates
  trailing-field absence in CDR (the schema mismatch is detected via
  type hashes, but practical compatibility for appended primitive fields
  is preserved by `rosidl` defaults). The old receiver simply ignores
  the QoS fields and continues to use its hardcoded `RELIABLE` /
  `KEEP_LAST(1)` defaults — which is the pre-PR behavior. This is a
  graceful degradation.
- **Old sender → new receiver**: missing fields deserialize as empty
  strings and `0`. The receiver's QoS-resolution logic treats empty
  strings as "default" and `history_depth == 0` as "default 1", so the
  receiver applies its `BEST_AVAILABLE` / `VOLATILE` / `KEEP_LAST(1)`
  defaults. This is also a graceful degradation.

The receiver MUST treat missing/empty/zero values as defaults rather
than rejecting the message. This is enforced in the resolver helper
exercised by `test_qos_resolution.cpp`.

In normal operation, both endpoints come from the same source tree and
are deployed together; the mismatched-pair case is a fallback for partial
deployments and field hotfixes.

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

This package ships **no per-topic `reliability: reliable` overrides** in
the initial deployment of this design. The CAMP-style mitigation is
entirely subsumed by `BEST_AVAILABLE` matching. If a future use case
emerges that genuinely requires the destination publisher to assert
`RELIABLE` semantics on the destination DDS hop (independent of what the
subscriber declares), the per-topic `reliability: reliable` override is
the right knob — but the bar is high, because asserting RELIABLE on a
UDP-fed publisher is the lie this design avoids.

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
  wedge whose hypothesized mechanism (RELIABLE publisher stalling on a
  dead subscriber) motivated the reliability policy.
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
