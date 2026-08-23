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
   payload copy. On the reorder-disabled fast path the probe runs *after*
   the stale-packet gate, preserving that path's property that a dropped
   packet costs nothing: no payload copy and no `subscribers_mutex_`
   acquisition. The reorder path must probe before building, because a
   Buffer decision stores the built item and it has to carry its relay
   form with it. It also answers false for an **empty** `source_node` (a
   packet that never passed through `unwrap()`): the loop rule is a
   comparison against the sender's name, so an unnamed sender cannot be
   excluded from anything, and forwarding such a packet would fan it out to
   every remote listing the topic. On an unauthenticated transport (#53)
   that is an injection point; see [Security](#security).
2. **Handoff.** When true, the `RelayItem` (topic, sender's node name, the
   received `MessageInternal`, moved rather than copied) is attached to the
   `PublishItem` and handed to `relay_queue_` by `enqueuePublish()` — the
   single point through which every packet the stale/reorder gate admits
   passes. See [the gate ordering](#relay-follows-the-stalereorder-gate)
   and [Threading](#threading-why-a-worker).
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

`source_topic` is rewritten to the **hub-local** topic — the key into
`subscribers_`, i.e. the topic name this bridge itself knows the data by.
The receiver resolves a message to `destination_topic`, falling back to
`source_topic` when it is empty, and an empty `destination_topic` is
ordinary (`remoteAdvertise` / `decodeSubscribeRequest` pass it through with
no fallback of their own). Leaving `source_topic` as the upstream
publisher's name would send that fallback to the wrong topic: two boats
each publishing `/nav`, remapped by the hub to `/boat_a/nav` and
`/boat_b/nav`, would collide back onto `/nav` downstream. The rewrite also
makes a relayed message identical in shape to a locally-originated one,
whose `source_topic` is likewise the sending bridge's own topic
(`callback()`). Provenance is not lost — the immediate sender travels in
the wrapped packet's `source_node`, which is where the receiver reads it
from anyway.

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
packet's `source_node`.

### That comparison needs one identity namespace, and it has one

The rule is a plain string compare, so it is only correct if both sides
spell a remote the same way. There is exactly one namespace for that: the
name a bridge calls itself on the wire (its own `name` parameter, which it
stamps into every `WrappedPacket::source_node`). `remote_nodes_` is keyed
by it, and every routing-table key a static config creates is keyed by it
too.

`remotes_list` entries are **labels**, not identities — they exist to spell
parameter paths (`remotes.<label>.connections_list`). The identity of a
configured remote is `remotes.<label>.name`, falling back to the label when
unset (see `include/udp_bridge/remote_identity.h`). Two labels resolving to
the same name fail `on_configure`, and a sequenced packet from a sender
that matches no configured remote logs a throttled WARN naming the
parameter to set.

This is worth stating because it was wrong until it was fixed as part of
#51: `on_configure` keyed everything by the label and never read
`remotes.<label>.name`, so a remote whose own name differed from the hub's
label for it matched nothing in the loop rule and had its traffic relayed
straight back to it — with a **single** remote configured, the shape relay
is supposed to leave inert. If you are reading this to check whether relay
can perturb an existing deployment, that invariant (label == the remote's
own `name`, or `name` set explicitly) is the thing to verify.

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

## Relay follows the stale/reorder gate

**A message is relayed if and only if this bridge admits it for local
publication.** Anything the `drop_stale_packets` gate or the reorder buffer
(#35) drops as stale or superseded is not forwarded either.

The reason is the downstream bridge's own gate. Forwarding assigns **fresh**
wrapped-packet sequence numbers, so the upstream numbering — the only thing
that says "this packet is older than one you already have" — does not
survive the hop. A downstream operator receiving a relayed resend of a
superseded message therefore has no way to recognise it as stale: it arrives
with the newest sequence number on the link and republishes over newer data.
Older data would land as newest, which is precisely the failure the gate
exists to prevent, reintroduced one hop later.

Relaying only admitted packets keeps the hub's judgement and the operator's
view consistent: what the hub publishes is what the hub forwards.

### How a held packet keeps its relay form

The reorder buffer holds a gap-opening packet as a `PublishItem`, releasing
it later — when a gap-filler closes the gap, or when the hold window
expires. Two things follow:

- A held packet is **not** relayed at buffer time. It may still be dropped,
  and forwarding it early would be exactly the behaviour above.
- Its relay form must therefore survive the wait. `PublishItem` carries an
  optional `std::unique_ptr<RelayItem>` (null whenever relay is
  unreachable, which is every configuration in this repo today), so the
  `MessageInternal` the relay needs travels with the buffered item rather
  than being reconstructed, and the buffer needs no knowledge of relay.
- **A held packet's payload is resident once, not twice.** While the relay
  form is attached it is the sole holder of the payload:
  `PublishItem::message` is left empty and materialized from it
  (`materializePublishPayload`) in `enqueuePublish()`, immediately before
  the relay form is handed to `relay_queue_`. This matters specifically on
  a hub — the node that concentrates traffic — because the reorder buffer
  holds one packet per topic per remote for up to
  `reorder_hold_window_ms`; holding both forms would double those bytes,
  and would pay for a payload copy on packets the gate then drops. The
  copy is deferred, not added: it still happens on the same thread, once,
  and only for a packet that is actually published.

`UDPBridge::enqueuePublish()` is the single point where an admitted item is
handed to `publish_queue_` and its relay form, if any, to `relay_queue_`.
Every admit path goes through it: `decodeData`'s immediate path, the items a
gap-filler releases, and the items `flushExpiredBuffer` releases on window
expiry — in wire order in each case. `clearReorderBuffer` (on deactivation)
discards held packets without publishing them, so those are not relayed
either.

The cost of this ordering is that a downstream link which missed a message
does not get a second chance at it from an upstream resend the hub itself
discarded. The bridge's model is best-effort-with-loss-reduction
(`doc/qos_design.md`), and the per-link resend protocol (#44) still covers
loss on the hub→operator hop itself; what is not covered is loss downstream
of a message the hub has already superseded locally — which, being
superseded, is data the operator would be showing over newer data anyway.

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

Its byte budget is the `relay_queue_max_bytes` parameter (default 64 MiB,
clamped 1 MiB–2 GiB — the same default and clamps as
`publish_queue_max_bytes`). It is a parameter, not a constant, for the same
reason its sibling is: it bounds the memory this node holds when a consumer
stalls, and on a hub the relay queue carries the whole downstream fan-out,
so the right size is a deployment question. The two are separate knobs
because the queues are separate failure domains. Drops are
visible: the `relay queue` diagnostic task reports depth and drop totals and
WARNs on drops since the previous tick. A relay drop is **unrecoverable** —
the message never reached a `Connection`, so the resend layer has nothing
to retransmit.

The reported total sums **two** sources, because loss happens on both sides
of the worker:

- what `RelayQueue` itself discards — overflow and push-after-stop;
- what the sink (`relayToOtherRemotes`) abandons after dequeuing it: the
  node left ACTIVE, the sender carries no `source_node`, or the topic's
  routing table was torn down between the drain thread's probe and the sink
  call. These are broken out per reason in `sink_drop_reasons`
  (`include/udp_bridge/relay_drops.h`).

An item held back only by the per-connection `period` rate limit is counted
apart (`rate_limited`) and is **not** loss — it is the limit working as
configured, and folding it into the loss total would leave the diagnostic
permanently in WARN on any rate-limited topic.

## Statistics

Relayed traffic is added to the topic's `MessageStatistics` exactly as
locally-originated traffic is (`~/topic_statistics`, `BridgeInfo`), so
relayed bytes are visible rather than invisible. Two things to know when
reading those numbers on a hub:

- **Local-origin and relayed traffic are summed per topic**, in the
  aggregate row and the per-destination rows alike. Both senders seed the
  empty `send_results[""][""]` pair once per message, which is what
  produces the `destination_node == ""` row — the topic's message count,
  independent of how many destinations the message reached. The message
  counts and sizes for a relayed topic therefore mix the two sources, so a
  hub's `topic_statistics` for `/boat_a/nav` is not a measure of what its
  own publishers produced. The per-destination `send_results` breakdown
  *is* per remote, so where the traffic went is still distinguishable;
  where it came from is not.
- `message_size` for a relayed message is the received payload size
  (`MessageInternal::data`), the same quantity `callback()` records for a
  locally published one.

Splitting the two would mean a per-source dimension in `MessageStatistics`
— a wire-format change to `TopicStatistics`, out of scope here.

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
  other remote that lists it.
- **The topic lists are not an access-control list, because the hub
  operator does not solely control them.** `decodeSubscribeRequest` feeds
  the wire-supplied `source_topic`, `destination_topic` and `period`
  straight into `addSubscriberConnection`, which writes
  `subscribers_[source_topic].remote_details[source_node]` — creating topic
  keys that were never configured. So any host that can reach the port can
  **self-enrol as a relay destination** for a topic the hub carries, and,
  because `source_node` is attacker-chosen (next point), can overwrite a
  legitimate remote's entry in that table: a spoofed request with a
  negative `period` silently cuts that operator's relayed feed. The
  configured topic lists describe the intended routing, not an enforced
  bound on it. This compounds
  [#53](https://github.com/rolker/udp_bridge/issues/53) rather than being
  separate from it — the tunnel is what bounds who can reach the port, and
  nothing below the tunnel bounds what they can ask for.
- **`source_node` is an attacker-controllable wire field.** The loop rule's
  only input is `SourceInfo::node_name`, taken from the wrapped packet's
  `source_node` — a string the sender chooses. An injected packet can
  therefore claim to come from any remote, which suppresses relay to that
  one remote (naming a remote as the sender excludes it) but reaches every
  other remote listing the topic. A packet that is *not* wrapped at all
  carries no `source_node`; those are refused outright — the probe answers
  false for an empty sender, precisely because a rule that excludes by name
  excludes nobody when there is no name. Neither behaviour is
  authentication, and neither is a substitute for the tunnel.
- **Relayed packets occupy each `Connection`'s `sent_packets_` resend
  buffer.** Forwarding goes through the ordinary `send()` path, so a
  relayed message is tracked for retransmission on every outgoing
  connection it went to, for up to `kSentPacketTTL` (5 s), and is subject to
  the same resend budget (#44). Injected traffic accepted by a hub
  therefore consumes not just downstream bandwidth but downstream *resend
  state*, on every link, until it ages out.

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
