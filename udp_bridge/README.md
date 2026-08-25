# udp_bridge

![Build Status](https://img.shields.io/badge/build-unknown-gray) <!-- Update if CI exists -->

**The udp_bridge package**

## Overview
The `udp_bridge` package connects multiple ROS 2 environments over an unreliable network using UDP datagrams. The main design goal is to resume the transmission of select ROS topics as quickly as possible once a telemetry link is re-established after a lost connection.

Initial remote nodes, connections, and transmitted topics may be specified as ROS parameters. Service calls can be used to manage remotes and transmitted topics at runtime.

A bridge also **relays** between its remotes: a message received from one remote is forwarded to every *other* remote whose `topics_list` carries that topic. This is what makes a hub possible — `boat → hub → operators` — with no second bridge process on the hub and no per-hop reconfiguration. There is no relay parameter: the per-remote `topics_list` is the routing table, and a message is never sent back to the remote it came from. Relay is **direct-neighbour only**, so configure star topologies (one hub, spokes that connect only to the hub); a cycle such as `A → B → C → A` would circulate a message indefinitely. See [`doc/relay_design.md`](doc/relay_design.md), including its Security section — a hub widens the blast radius of the unauthenticated transport ([#53](https://github.com/rolker/udp_bridge/issues/53)).

> **Note on Lifecycle**: Valid for ROS 2, `udp_bridge_node` is a Lifecycle Node. It starts in the `Unconfigured` state. You must transition it to `Active` for it to begin working.

## Installation

### Dependencies
-   `lifecycle_msgs`
-   `rclcpp`
-   `rclcpp_lifecycle`
-   `std_msgs`
-   `udp_bridge_interfaces`
-   `ZLIB`

### Building
```bash
colcon build --symlink-install --packages-select udp_bridge
```

## Usage

### Launch Files

#### `udp_bridge_launch.py`
Launches the node with default configuration and automatically transitions it to the `Active` state.
```bash
ros2 launch udp_bridge udp_bridge_launch.py ros_namespace:=my_robot
```

### Running Nodes
If you run the node manually, you **must** manage the lifecycle states yourself.

1.  **Start the node**:
    ```bash
    ros2 run udp_bridge udp_bridge_node --ros-args -r __ns:=/my_robot
    ```

2.  **Transition to Active** (in a separate terminal):
    ```bash
    ros2 lifecycle set /my_robot/udp_bridge configure
    ros2 lifecycle set /my_robot/udp_bridge activate
    ```

## Nodes

### udp_bridge_node
The main node that handles UDP communication.

#### Subscriptions
- Topics defined in the configuration are subscribed to dynamically.

#### Publications
| Topic | Type | Description |
|---|---|---|
| `~/bridge_info` | `udp_bridge_interfaces/msg/BridgeInfo` | Diagnostic info about bridge state. |
| `~/topic_statistics` | `udp_bridge_interfaces/msg/TopicStatisticsArray` | Traffic statistics. |
| *Dynamic* | *Various* | Dynamically publishes topics received from remotes. |

#### Parameters
The node uses a nested parameter structure to define remotes and connections. It is highly recommended to use a YAML configuration file (e.g., `config/example_params.yaml`).

To load parameters at startup:
```bash
ros2 run udp_bridge udp_bridge_node --ros-args --params-file src/udp_bridge/udp_bridge/config/example_params.yaml
```

**Parameter Reference**:

-   `name`: (string) Name used when communicating with remotes. Should be unique. Defaults to the node's name.
-   `port`: (integer) UDP port number used for listening for incoming data. Defaults to 4200.
-   `maximum_packet_size`: (integer, 256–65500, default **1200**) Maximum packet size used when sending data. The default is sized for a tunnelled link rather than the IPv4/UDP maximum: WireGuard over cellular commonly runs MTU 1280, leaving 1252 bytes of usable UDP payload, and 1200 fits with headroom. Anything larger is IP-fragmented by the kernel before it reaches the link — and IP fragments are not individually recoverable, so udp_bridge's resend machinery cannot repair one: a single lost IP fragment discards the entire datagram. Let the bridge fragment instead. Raise this only for a path whose end-to-end MTU has been verified.

    > **Behavior change (#58):** the unconfigured default dropped from `65500` to `1200`. This is strictly safer (avoids unrecoverable IP fragmentation) and affects only deployments that relied on the implicit default — every in-workspace deployment already sets this explicitly and is unaffected. Raise it back in your own config if you have a verified larger end-to-end MTU.
-   `remotes_list`: (string array) List of initial remote nodes. Each entry
    is both the label spelling that remote's parameter paths below **and**
    the name that remote calls *itself* on the wire, so: **your label for a
    peer must be the name that peer calls itself** (its own `name`
    parameter, which it stamps into every packet it sends). An entry that is
    empty, longer than 23 characters, or equal to this bridge's own `name`
    fails `on_configure`, validated before the socket is opened; a repeated
    entry is idempotent, as it always was.

For each remote in `remotes_list`:
-   `remotes.<remote_label>.name`: **removed (#67), and setting it for a
    remote listed in `remotes_list` is a configuration error.** A non-empty
    value fails `on_configure` with one ERROR listing every offending key and
    its value — all of them, not just the first, since recovery costs a
    process restart. Only the labels in
    `remotes_list` are checked — a `remotes.<label>.name` under a block that
    `remotes_list` does not name is never declared and never read, and that
    whole block is inert for the same reason. The parameter is still
    *declared* — rclcpp surfaces a YAML override only for a parameter the
    node declares, so declaring it is the only way a config still carrying
    the key can be detected at all — but it is never read for identity. Delete it from your config; the
    `remotes_list` entry is the remote's wire name. (An explicitly empty
    value, `name: ""`, is indistinguishable from an absent key — the
    declared default is the empty string — so it configures normally.)

    > **Upgrade note (#67) — the second identity namespace is gone.** Two
    > ways of naming a remote (the `remotes_list` label for parameter paths,
    > `remotes.<label>.name` for the wire) were an artefact of porting ROS
    > 1's nested `remotes` dictionary — where the dictionary key *was* the
    > name, with an optional `name:` override inside the block — onto ROS
    > 2's flat parameter model, which had to manufacture a label to spell
    > `remotes.<label>.*` paths. #51 made the override authoritative to fix
    > the echo bug below; #67 retires it, so there is one string per remote
    > again. This is a deliberate retirement of a working feature, not the
    > repair of an accident.
    >
    > **If any remote in your `remotes_list` sets `remotes.<label>.name` at
    > all**, the bridge refuses to configure until you delete the key —
    > including when the value happens to equal its label and would have
    > changed nothing. The parameter is removed and unsupported; it is
    > rejected on presence, not on whether it would have altered an
    > identity. It is a hard failure rather than a warning because the one
    > shape a warning would leave running — a value *differing* from its label — is precisely the echo
    > bug below, silently reintroduced on upgrade.
    >
    > Where the old value was the peer's real wire name, rename the
    > `remotes_list` entry (and its `remotes.<label>.*` paths) to it, which
    > renames everything keyed by that remote:
    >
    > - the per-remote topics `~/remotes/<remote>/bridge_info` and
    >   `~/remotes/<remote>/topic_statistics`;
    > - `BridgeInfo.remotes[].name` and `BridgeInfo.topics[].remotes[].remote`;
    > - `TopicStatistics.destination_node`;
    > - the per-remote diagnostic task names;
    > - the `remote` / `name` arguments of `remote_subscribe`,
    >   `remote_advertise`, `remove_subscribe`, `remove_advertise` and
    >   `add_remote`.
    >
    > **Restart the node after a rename — do not re-configure a running
    > one.** Nothing in the package ever closes the bound socket
    > (`on_cleanup` does not touch `socket_`, there is no destructor, and
    > `SO_REUSEADDR` is never set), so on every fixed-port deployment a
    > deactivate→cleanup→configure cycle re-binds the same port, gets
    > `EADDRINUSE` and `exit(1)`s at the bind — before the rename can reach
    > the routing table at all (issue
    > [#66](https://github.com/rolker/udp_bridge/issues/66)). The shipped
    > `launch/udp_bridge_launch.py` sets `respawn=True, respawn_delay=2`,
    > so the process does come back; the outcome is an unplanned restart
    > rather than a graceful re-configure. A fresh process starts from an
    > empty table, which is what the rename needs.
    >
    > Anything that consumes those (dashboards, scripts, launch-time service
    > calls) must use the entry. A remote left under the wrong entry is the
    > #51 bug again: the relay loop rule compares the wire `source_node`
    > against the configured identity, matches nothing, and the hub can
    > relay that remote's own traffic back to it. The bridge WARNs on the
    > first sequenced packet from a sender matching no configured remote —
    > but only when `remotes_list` is non-empty, since on a purely dynamic
    > bridge every sender is unknown by definition. See
    > [`doc/relay_design.md`](doc/relay_design.md).
    >
    > **What is retired with it.** An entry is now both a wire identity
    > (≤23 bytes) *and* a ROS 2 parameter-path segment, and ROS 2 uses `.`
    > to separate segments. A peer whose wire name contains a `.` is still
    > configurable, but only by nesting its parameters as if the dot were a
    > level boundary — `remotes_list: ["boat.one"]` with `remotes: boat:
    > one: …`, which resolves correctly but reads as a nested structure
    > rather than one name. Before #67 it could be given flatly via
    > `remotes.<label>.name`. No known configuration uses such a name;
    > prefer names without `.`.

    > **Upgrade note (#51) — node names longer than 23 characters now fail
    > `on_configure`.** The on-wire `source_node` field is 24 bytes, so 23
    > characters is the longest name that survives a round trip. This used to
    > be handled by silently truncating the *local* `name` (with a WARN) while
    > leaving configured remote names untruncated — which is what made the two
    > unequal, put the relay loop rule to sleep, and let the hub echo. An
    > over-long name is now **rejected, not shortened**, wherever one can be
    > supplied: the `name` parameter and any `remotes_list` entry fail the
    > configure transition with a message naming the parameter, the value,
    > its length and the limit; the `add_remote` service refuses the request
    > with an ERROR and creates no remote. (A `remotes.<label>.name` key is
    > not among them since #67 — whatever its length, it is refused on
    > *presence*, because the parameter is removed rather than merely
    > length-checked.) A config that previously
    > started with a truncation warning will
    > now refuse to configure — that config only ever worked if you had
    > hand-truncated the peer's name to match. Shorten the name on both bridges.
    > Names arriving *from the wire* are not rejected (a long one arrives
    > already shortened and is indistinguishable from a short one); they are
    > read with an explicit length bound instead.
    >
    > Two further names are refused for the same reason — they cannot mean
    > what the config says they mean. A `remotes_list` entry equal to **this
    > bridge's own name** fails `on_configure`, and is refused by
    > `add_remote` with an ERROR: it would install this bridge in
    > `remote_nodes_` as its own peer. An **empty** entry fails
    > `on_configure` too: the empty name is reserved on the send path to
    > mark a connection request, so such a remote could never be routed to.
-   `remotes.<remote_label>.connections_list`: (string array) List of named connections.

For each connection in `connections_list`:
-   `remotes.<remote_label>.connections.<connection_id>.host`: (string) IP or hostname.
-   `remotes.<remote_label>.connections.<connection_id>.port`: (integer) Target port.
-   `remotes.<remote_label>.connections.<connection_id>.maximum_bytes_per_second`: (int, bytes per second; 0 means the built-in default of 50000) Rate limit for the whole connection. This is a hard ceiling on *wire* bytes and the cost control on a metered link; it is not the bridge's model of link capacity — see `admission_floor_bytes_per_second` below and `doc/admission_control_design.md`.
-   `remotes.<remote_label>.connections.<connection_id>.resend_budget_fraction`: (double, 0.0–1.0, default 0.25) Maximum fraction of *measured goodput* that resend traffic may consume. Halves per second of ack starvation, floored at 1/16 of the fraction. See `doc/resend_budget_design.md`.
-   `remotes.<remote_label>.connections.<connection_id>.admission_floor_bytes_per_second`: (double, default 8192) Minimum AIMD-adjusted admission cap, in absolute bytes per second — clamped at use to the connection's own rate limit, so it can never raise a cap. The effective cap drops on congestion (the remote reporting <90% delivery, or nothing received for ~1 s while actively sending) and recovers additively when clean. See `doc/admission_control_design.md`.
-   `remotes.<remote_label>.connections.<connection_id>.link_headroom_fraction`: (double, `[0.0, 1.0)`, default 0.2; values are clamped to 0.99 — a headroom of exactly 1.0 would target zero throughput on every congested sample) Fraction of measured goodput deliberately left unused so a co-tenant on the same path (SSH, operator management traffic) is not starved. Applied as the target of a congested backoff. See `doc/admission_control_design.md`.

    > **These four are live-tunable.** `maximum_bytes_per_second`,
    > `resend_budget_fraction`, `admission_floor_bytes_per_second` and
    > `link_headroom_fraction` apply to the running connection when set with
    > `ros2 param set` — no restart, no re-configure. Until this was fixed
    > they were read once in `on_configure` and never again, so a runtime set
    > was accepted, read back with the new value, and changed nothing; that
    > cost a wrong conclusion and about three minutes of link outage on the
    > 2026-08-25 BizzyBoat transit.
    >
    > A runtime set is **validated and refused** where a config file is
    > clamped, because `ros2 param set` has somewhere to print the reason and
    > a YAML file does not. The accepted ranges are exactly the ranges the
    > setters store unchanged, so an accepted value cannot read back as
    > something other than what is in force:
    >
    > | Parameter | Accepted at runtime |
    > |---|---|
    > | `maximum_bytes_per_second` | integer `0` – `4294967295` (0 = the 50000 default) |
    > | `resend_budget_fraction` | finite double in `[0, 1]` |
    > | `admission_floor_bytes_per_second` | finite double `>= 0` (clamped at use to this connection's own rate limit, so it can never raise a cap) |
    > | `link_headroom_fraction` | finite double in `[0, 0.99]` |
    >
    > A batch is validated as a whole: `ros2 param set` of several values
    > applies all of them or none. Setting a value for a connection that is no
    > longer registered is **refused**, not silently ignored — "accepted,
    > reads back, does nothing" is the failure mode being removed.
    >
    > **Type the value the way the parameter was declared.** rclcpp enforces a
    > parameter's declared type before the node ever sees the set, so
    > `ros2 param set … admission_floor_bytes_per_second 20000` fails on the
    > type; write `20000.0`. The three fractions and the floor are doubles;
    > both `maximum_bytes_per_second` parameters are integers.

-   `remotes.<remote_label>.connections.<connection_id>.topics_list`: (string array) List of topics to sync. This is also the **relay routing table**: a topic listed here is delivered to this remote whether it was published locally or received from another remote. Adding a second remote that lists an already-carried topic therefore starts that traffic flowing to it — size the connection's rate limit for the combined load. See [`doc/relay_design.md`](doc/relay_design.md).

For each topic in `topics_list`:
-   `source`: (string) Local topic name.
-   `destination`: (string) Remote topic name.
-   `period`: (double) Minimum period between messages (shaping/rate limiting). 0.0 = no limit.
-   `queue_size`: (int) Subscriber queue size.
-   `maximum_bytes_per_second`: (int, default **0** = unlimited) Per-topic
    send cap for this topic on this connection, so one topic cannot crowd
    out the rest of the connection. The connection-level cap is metered
    first-come-first-served with no notion of topic, so without this a
    single busy topic can take the whole admitted budget and every other
    topic on the connection is shed as a side effect. Absent or 0 is
    today's behaviour, so no existing configuration changes.

    Units are **offered message payload bytes per second**, not the wire
    bytes the connection-level `maximum_bytes_per_second` meters: the
    decision is made before serialization, compression and fragmentation,
    so a wire figure does not exist yet. Payload bytes are also what
    `TopicStatistics.message_bytes_per_second` reports for the topic,
    which is the number to size this against — watch the topic's offered
    rate in `~/topic_statistics` and set the cap below it.

    Enforced as a token bucket one second deep. A message larger than one
    second of the cap is still sent when the bucket is empty, so an
    over-tight cap makes a topic slow rather than permanently silent; the
    sustained rate is still the cap. A topic over its cap is **dropped and
    counted**: the drop appears in that topic's `TopicStatistics` `send`
    DataRates for the destination it was shed for, as a connection-level
    drop does. (Those `dropped_bytes_per_second` figures therefore mix
    payload bytes shed here with wire bytes shed by the connection-level
    limiter.) On a relay hub, an item whose every due destination was shed
    is reported on the `relay queue` diagnostic's
    `rate_limited_by_topic_cap` row — a rate limit, not loss.

    Live-tunable with `ros2 param set` on the same terms as the
    per-connection tunables above: integer `0`–`4294967295`, where 0
    removes the cap. A negative value in the **config file** is warned
    about and treated as no cap; a negative value at **runtime** is
    refused with a reason.

#### Services
| Service | Type | Description |
|---|---|---|
| `~/add_remote` | `udp_bridge_interfaces/srv/AddRemote` | Add a new remote at runtime. |
| `~/list_remotes` | `udp_bridge_interfaces/srv/ListRemotes` | Get current list of remotes. |
| `~/remote_advertise` | `udp_bridge_interfaces/srv/Subscribe` | Request remote to send a topic (advertised on local, subscribed on remote). |
| `~/remote_subscribe` | `udp_bridge_interfaces/srv/Subscribe` | Request remote to subscribe to a topic (subscribed on local, published from remote). |

## License
BSD
