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
-   `remotes.<remote_label>.connections.<connection_id>.maximum_bytes_per_second`: (int) Rate limit.
-   `remotes.<remote_label>.connections.<connection_id>.resend_budget_fraction`: (double, 0.0–1.0, default 0.25) Maximum fraction of *measured goodput* that resend traffic may consume. Halves per second of ack starvation, floored at 1/16 of the fraction. See `doc/resend_budget_design.md`.
-   `remotes.<remote_label>.connections.<connection_id>.admission_floor_bytes_per_second`: (double, default 8192) Minimum AIMD-adjusted admission cap, in absolute bytes per second — clamped at use to the connection's own rate limit, so it can never raise a cap. A floor at or **above** that rate limit makes adaptive admission control inert on the connection — the cap can never be reduced — and `on_configure` logs a WARN naming the connection when it is. The WARN tests the **applied** floor, so a negative or NaN value (which falls back to 8192) is caught too. The same condition can also be reached at *runtime*, when a peer's `CONNECT` or an `add_remote` call sets a rate limit at or below the floor; that route logs its own (throttled) WARN. The effective cap **halves from its own current value** on congestion (the remote reporting <90% delivery over a matched 5 s window, or nothing received for ~1 s while actively sending) and recovers additively when clean. Configure-time only (see `admission_refractory_period_seconds` below). See `doc/admission_control_design.md`.
-   `remotes.<remote_label>.connections.<connection_id>.admission_refractory_period_seconds`: (double, default 5.0, 0 disables) Minimum interval between admission-control decisions. While the window is open the controller **freezes** — no decrease *and* no recovery, whatever the arriving sample says — so one congestion episode costs at most one backoff. The window doubles on each further decrease within an unresolved episode and resets on the first clean sample. Two bounds apply to it: the configured base is clamped to **60 s** (values above that, `+Inf` included, clamp to the ceiling rather than falling back to the default), and the grown window is bounded by both 2× the base and that same 60 s ceiling — so the longest the controller can ever be frozen is 60 s, whatever is configured. Without it, the 2026-08-25 Appledore transit turned a single marginal delivery sample into seven halvings in 12 s, 35 times in 9.5 hours, discarding 1.95 GB at the boat's own gate on a link reporting 0.0–0.05% loss. Note the **double** literal (`5.0`, not `5`). **Configure-time only**, like every other admission parameter: it is read once during `on_configure` and applied to the live `Connection` then. `ros2 param set` after that changes the parameter but not the controller — the node must be reconfigured (or restarted) for a new value to take effect. This is the property RCA item D names as what blocked live mitigation during the 2026-08-25 Appledore transit; live re-tuning is tracked as [#75](https://github.com/rolker/udp_bridge/issues/75). See `doc/admission_control_design.md`.
-   `remotes.<remote_label>.connections.<connection_id>.link_headroom_fraction`: (double, default 0.2) **RETIRED (#52) — a declared tripwire, nothing more.** It was the fraction of measured goodput deliberately left unused so a co-tenant on the same path (SSH, operator management traffic) was not starved, and its only call site was the `(1 − headroom) × goodput` clamp on the congested backoff. That clamp was removed in the 2026-08-25 pass because goodput is depressed by the throttling the clamp was computing. **All behaviour is gone**: no `Connection` stores it, no message carries it, nothing in the control law reads it. The parameter is still *declared* so that a stale key in an existing config is **visible** — rclcpp surfaces a YAML override only for a declared parameter, so undeclaring it would make the key silently ignored, which is the same "set it, nothing happens" shape #52 exists to remove. Setting it to anything other than the default logs a WARN naming the connection at `on_configure`; the node still comes up (unlike the retired `remotes.<label>.name`, a stale value here misroutes nothing). Remove the key from your config. The co-tenant guarantee is now carried by the refractory-gated multiplicative decrease, verified against the bench's management-flow invariant. See `doc/admission_control_design.md`, "`link_headroom_fraction` after the clamp removal".
-   `remotes.<remote_label>.connections.<connection_id>.topics_list`: (string array) List of topics to sync. This is also the **relay routing table**: a topic listed here is delivered to this remote whether it was published locally or received from another remote. Adding a second remote that lists an already-carried topic therefore starts that traffic flowing to it — size the connection's rate limit for the combined load. See [`doc/relay_design.md`](doc/relay_design.md).

For each topic in `topics_list`:
-   `source`: (string) Local topic name.
-   `destination`: (string) Remote topic name.
-   `period`: (double) Minimum period between messages (shaping/rate limiting). 0.0 = no limit.
-   `queue_size`: (int) Subscriber queue size.

#### Services
| Service | Type | Description |
|---|---|---|
| `~/add_remote` | `udp_bridge_interfaces/srv/AddRemote` | Add a new remote at runtime. |
| `~/list_remotes` | `udp_bridge_interfaces/srv/ListRemotes` | Get current list of remotes. |
| `~/remote_advertise` | `udp_bridge_interfaces/srv/Subscribe` | Request remote to send a topic (advertised on local, subscribed on remote). |
| `~/remote_subscribe` | `udp_bridge_interfaces/srv/Subscribe` | Request remote to subscribe to a topic (subscribed on local, published from remote). |

## License
BSD
