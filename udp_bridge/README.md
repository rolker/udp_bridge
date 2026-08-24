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
-   `remotes_list`: (string array) List of labels for initial remote nodes.

For each remote in `remotes_list`:
-   `remotes.<remote_label>.name`: (string, default empty) The name that
    remote calls *itself* on the wire — its own `name` parameter, which it
    stamps into every packet it sends. Leave it unset (the default) when the
    `remotes_list` label already equals that name, which is the usual case;
    set it when your local label for a remote differs from the name that
    remote uses. Two remotes resolving to the same name fail `on_configure`
    (validated before the socket is opened). The resolved name — not the
    label — is what the bridge keys remotes by; see the upgrade note below.

    > **Upgrade note (#51) — check for a stale `name:` key before upgrading.**
    > Before #51 this parameter was never declared, so a `name:` key in a
    > params file was silently ignored and the `remotes_list` label was used
    > everywhere. The shipped `config/example_params.yaml` set
    > `label: robot_a` with `name: "robot_a_bridge"`, so a config copied from
    > it carries an inert key that #51 makes live. **If any of your remotes
    > has a `remotes.<label>.name` whose value differs from the label**, then
    > on upgrade everything keyed by that remote is renamed from the label to
    > the `name:` value:
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
    > Anything that consumes those (dashboards, scripts, launch-time service
    > calls) must use the new name. If you did **not** intend the rename,
    > delete or comment out the `name:` key and the label is used as before.
    > If the key was correct all along, note that the loop rule and relay
    > routing now actually work for that remote — that is the fix #51 shipped.

    > **Upgrade note (#51) — node names longer than 23 characters now fail
    > `on_configure`.** The on-wire `source_node` field is 24 bytes, so 23
    > characters is the longest name that survives a round trip. This used to
    > be handled by silently truncating the *local* `name` (with a WARN) while
    > leaving configured remote names untruncated — which is what made the two
    > unequal, put the relay loop rule to sleep, and let the hub echo. An
    > over-long name is now **rejected, not shortened**, wherever one can be
    > supplied: the `name` parameter and any `remotes.<label>.name` (or a
    > `remotes_list` label used as the identity) fail the configure transition
    > with a message naming the parameter, the value, its length and the limit;
    > the `add_remote` service refuses the request with an ERROR and creates no
    > remote. A config that previously started with a truncation warning will
    > now refuse to configure — that config only ever worked if you had
    > hand-truncated the peer's name to match. Shorten the name on both bridges.
    > Names arriving *from the wire* are not rejected (a long one arrives
    > already shortened and is indistinguishable from a short one); they are
    > read with an explicit length bound instead.
-   `remotes.<remote_label>.connections_list`: (string array) List of named connections.

For each connection in `connections_list`:
-   `remotes.<remote_label>.connections.<connection_id>.host`: (string) IP or hostname.
-   `remotes.<remote_label>.connections.<connection_id>.port`: (integer) Target port.
-   `remotes.<remote_label>.connections.<connection_id>.maximum_bytes_per_second`: (int) Rate limit.
-   `remotes.<remote_label>.connections.<connection_id>.resend_budget_fraction`: (double, 0.0–1.0, default 0.25) Maximum fraction of *measured goodput* that resend traffic may consume. Halves per second of ack starvation, floored at 1/16 of the fraction. See `doc/resend_budget_design.md`.
-   `remotes.<remote_label>.connections.<connection_id>.admission_floor_bytes_per_second`: (double, default 8192) Minimum AIMD-adjusted admission cap, in absolute bytes per second — clamped at use to the connection's own rate limit, so it can never raise a cap. The effective cap drops on congestion (the remote reporting <90% delivery, or nothing received for ~1 s while actively sending) and recovers additively when clean. See `doc/admission_control_design.md`.
-   `remotes.<remote_label>.connections.<connection_id>.link_headroom_fraction`: (double, `[0.0, 1.0)`, default 0.2; values are clamped to 0.99 — a headroom of exactly 1.0 would target zero throughput on every congested sample) Fraction of measured goodput deliberately left unused so a co-tenant on the same path (SSH, operator management traffic) is not starved. Applied as the target of a congested backoff. See `doc/admission_control_design.md`.
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
