# Remote-to-Remote Relay (issue #51)

Design note for the receive-time forwarding path in
`UDPBridge::relayToOtherRemotes()`. Companion to `doc/qos_design.md` and
`doc/admission_control_design.md` in format and intent.

## Problem

Until #51 a bridge was a two-party device: it forwarded *locally published*
topics to its remotes, and published what its remotes sent it. A message
that arrived from remote A was published locally and stopped there, even
when remote B had asked the same bridge for the same topic.

That makes the cloud-hub topology impossible without hand-built
double-bridging. The wanted shape is:

```
   boat ──────► hub ──────► operator 1
                  └───────► operator 2
```

The hub is a cheap always-on host (a cloud VM, an operator station) that
every party can reach, so no operator needs a route to the boat and the
boat maintains exactly one uplink. Before #51 the hub had to run a second
bridge process re-publishing every relayed topic through ROS, which
doubles the local DDS traffic, doubles the per-hop latency, and needs
per-topic configuration in two places.

## Mechanism

`decodeData()` — the receive path — offers every accepted message to the
other remotes that carry the same topic:

1. **Probe (socket-drain thread).** `hasRelayDestinations(topic,
   source_node)` takes `subscribers_mutex_` briefly and answers whether any
   remote *other than the sender* lists this topic. When false — every
   configuration in this repo today — relay costs one map lookup and no
   payload copy.
2. **Handoff.** When true, `decodeData()` pushes a `RelayItem` (topic,
   sender's node name, the received `MessageInternal`) onto `relay_queue_`
   and returns. See [Threading](#threading-why-a-worker).
3. **Selection (relay worker).** `relayToOtherRemotes()` looks up
   `subscribers_[topic].remote_details` — the routing table — and calls
   `selectRateLimitedConnections(remote_details, now, source_node)`, the
   same function `callback()` uses for locally published messages, with the
   sender excluded.
4. **Send.** For each selected `(remote, connection)`, the message is
   forwarded with `destination_topic` and the QoS fields rewritten for that
   destination and everything else byte-identical — no re-serialization of
   the payload — through the existing `send()` path. Fragmenting,
   wrapped-packet sequencing, the resend protocol and the per-connection
   AIMD admission control of #43/#52 all apply to relayed traffic exactly as
   they do to locally published traffic.

`source_topic` is left as the original publisher's topic, so the far end
still reports where the data actually came from.

## There is no relay parameter

The per-remote `topics_list` **is** the routing table. If remote B lists
topic X, B has asked for X, and the hub delivers it whether X was published
locally or arrived from remote A. This is the ROS 1 bridge's behaviour,
restored.

An earlier draft of this work added a per-destination `relay` opt-in
defaulting to off. It was removed as protection against nothing:

- **The echo it guarded against cannot occur.** Relay does not touch
  `ignore_local_publications`, so a bridge still never hears its own
  republish, and the loop rule below independently refuses to send back to
  the sender. A symmetric two-host pair is bit-for-bit unchanged.
- **It would have guarded configurations where relay is unreachable.**
  Relay only changes behaviour where a *second* remote lists the same
  topic, and every configuration in this repo declares exactly one remote
  per bridge (`config/example_params.yaml`, `test/bench/configs/*.yaml`).
- **It cost real complexity.** A flag reachable through
  `addSubscriberConnection` can be silently reset to `false` by the dynamic
  `decodeSubscribeRequest` path, and it needed an extra filter in the
  rate-limiting helper whose ordering is easy to get wrong. Both hazards
  disappear with the flag.

### Behaviour change to be aware of

**Adding a second remote that lists an already-carried topic now starts
that traffic flowing.** That is the feature — the topic list is the request
— but it is a behaviour change for any future multi-remote configuration,
and it is the one thing to check when adding a remote to a bridge that
already carries traffic: the new remote's `topics_list` is what it will
receive, from local publishers *and* from other remotes alike. Size the new
connection's `maximum_bytes_per_second` for that combined load.

## The loop rule

**A message is never sent back to the remote it came from.** The
destination is skipped when its `remote_details` key equals
`SourceInfo::node_name` — the immediate sender, taken from the wrapped
packet's `source_node`, the same key `remote_nodes_` is keyed by, so the
comparison is a plain string compare in a single namespace.

The exclusion is applied *before* any `last_sent_time` is stamped, so a
relay never consumes the excluded remote's rate-limit budget: the sender's
rate state after a relay is exactly as if the relay had not happened.

### Direct-neighbour only — star topologies, no cycles

`source_info.node_name` is the **last hop**, not the original publisher.
The bridge carries no path history, so the loop rule can only refuse to
echo to the immediate sender. In a cycle:

```
   A ──► B ──► C ──► A ──► B ──► ...
```

each hop legitimately forwards to a remote that is not its sender, and the
message circulates indefinitely — amplifying until the admission control on
each link is saturated. **Cycles and meshes are unsupported.** Configure
star topologies only: one hub, spokes that connect to the hub and to
nothing else (boat → hub → operators).

This is a configuration contract, not something the bridge enforces. If a
future deployment needs cycle tolerance, the mechanism is a hop counter or
an origin stamp in `MessageInternal` — a wire-format change, deliberately
out of scope here.

## Relay precedes the stale/reorder gate

The relay push happens *before* the `drop_stale_packets` / reorder-buffer
gate in `decodeData()`. Relaying is independent of this bridge's own
publish/drop decision:

- The downstream bridge assigns **fresh** wrapped-packet sequence numbers
  on forward and runs its own gate, so upstream sequencing tells it
  nothing.
- The gate's question is "have I already published something newer on this
  topic *locally*" — a question about this hub's publishers, not about what
  a downstream operator has seen.

The consequence to be explicit about: **an upstream resend the hub would
drop locally is still forwarded downstream.** That is deliberate — the
downstream link may not have received the message the resend supersedes —
but it means a lossy upstream link's resend traffic reaches the downstream
links. It is bounded, not unbounded: every forwarded packet goes through
`Connection::send()`, so the per-connection AIMD admission control
(#43/#52) and the resend budget (#44) meter it exactly as they meter
locally-originated traffic.

## Threading: why a worker

Forwarding must not run on the socket-drain thread. `Connection::send()`'s
`sendto()` uses `MSG_DONTWAIT` with a 20 × 10 ms poll — ~200 ms worst case
— when the send buffer is full, so relaying inline in `decodeData()` would
stack ~200 ms per stalled destination ahead of the next `recvfrom`. That is
the hazard of issue #10, on the send side.

`relay_queue_` (`include/udp_bridge/relay_queue.h`) is the same component
as `publish_queue_`: bounded in bytes, drop-oldest (KEEP_LAST — newest
wins, matching the bridge's best-effort model, see `doc/qos_design.md`),
one worker, injected sink, `push()` never blocks. Its lifecycle mirrors the
publish queue's: configured in `on_configure`, started in `on_activate`,
stopped and joined in `on_deactivate`, with a backstop stop in
`on_cleanup`.

It is a **separate** queue rather than a reuse of `publish_queue_` because
a stalled local subscriber and a stalled remote link are independent
failure domains; sharing one worker would put `Connection::send()`'s retry
loop in front of local republishes.

Its byte budget is the compile-time constant `kRelayQueueMaxBytes` (64 MiB)
rather than a parameter — relay adds no configuration surface. Drops are
visible: the `relay queue` diagnostic task reports depth and drop totals and
WARNs on drops since the previous tick. A relay drop is **unrecoverable** —
the message never reached a `Connection`, so the resend layer has nothing
to retransmit.

## Rate limiting is shared, not parallel

`selectRateLimitedConnections()`
(`include/udp_bridge/destination_selection.h`) is the single implementation
of the per-connection `period` rule, used by both senders: `callback()` for
a locally published message (no remote to exclude) and
`relayToOtherRemotes()` for a forwarded one (the sender excluded). A relay
destination with `period` P is therefore throttled identically to a
local-origin destination with `period` P, against the *same*
`last_sent_time` state — a topic that reaches a remote by both routes is
shaped once, not twice.

## Security

**udp_bridge's transport is unauthenticated and unencrypted** — see
[#53](https://github.com/rolker/udp_bridge/issues/53). Any host that can
reach the listening port can inject packets that the bridge will decode,
publish, and — as of #51 — **relay onward**.

Relay widens the blast radius, and it is worth stating plainly:

- Before #51, a spoofed packet reached one bridge's local ROS graph. With a
  hub, a spoofed packet accepted by the hub is forwarded to every other
  party that lists that topic — three or more organizations' ROS graphs
  from one injection point.
- A hub is, by design, the one host every party can reach. It is therefore
  the most exposed node in the topology and the most valuable to
  compromise.
- Relay is driven by the routing table alone, with no per-source
  authorization: any remote that can send the hub a topic can reach every
  other remote that lists it. The topic lists are the access-control list,
  such as it is.

**The deployment requirement is unchanged and is load-bearing:** run
udp_bridge only inside a tunnel (WireGuard, in this workspace's
deployments) and never expose the port to an untrusted network. For a hub
that requirement now protects more than one party's data, so the hub's
tunnel configuration and its exposure are a shared concern, not just the
hub operator's.

## References

- Issue [#51](https://github.com/rolker/udp_bridge/issues/51) — relay support
- Issue [#53](https://github.com/rolker/udp_bridge/issues/53) — trust model / unauthenticated transport
- Issue [#10](https://github.com/rolker/udp_bridge/issues/10) — the socket-drain thread must not block
- `doc/admission_control_design.md` (#43/#52) — what bounds relayed traffic
- `doc/qos_design.md` — the best-effort-with-loss-reduction model these queues follow
