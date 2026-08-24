# Agent Guide: udp_bridge

Reliable-ish UDP transport of selected ROS 2 topics between hosts on
unreliable networks — the boat ↔ operator-station telemetry link (WiFi, mesh
radio, cellular, satellite). Topics are explicitly selected, rate-limited,
compressed (zlib), fragmented to fit UDP datagrams, and re-requested on loss
within a bounded resend window. Multiple named connections per remote give
path redundancy.

## Workflow

GitHub-origin repo (`rolker/udp_bridge`). The default branch is **`jazzy`**,
not `main` — branch protection and the `no-commit-to-branch` pre-commit hook
both target `jazzy`. Standard workspace worktree/PR workflow applies when
developed inside the `ros2_agent_workspace` (see the workspace `AGENTS.md`;
the thin root `AGENTS.md` here is the Copilot-review pointer).

CI (`.github/workflows/ci.yml`: colcon build + test on `ros:jazzy-ros-core`)
and pre-commit (`.pre-commit-config.yaml`) are configured. **Linter-labeled
tests are excluded in CI** (`--ctest-args -LE linter`, `--pytest-args -m "not
linter"`) until the pre-existing code gets license headers; re-enable by
dropping those exclusions.

## Package Inventory

| Package | Language | Build targets (from CMakeLists.txt) |
|---------|----------|-------------------------------------|
| `udp_bridge` | C++17 (ament_cmake) | `udp_bridge` library, `udp_bridge_node` executable, 20 gtest targets (`utest`, `test_utilities`, `test_defragmenter`, `test_qos_resolution`, `test_qos_matching_integration`, `test_connection_rate_limit`, `test_remote_node_resend`, `test_connection_cleanup`, `test_giveup_diagnostic`, `test_resend_budget`, `test_admission_control`, `test_publish_queue`, `test_subscriber_registry`, `test_stale_packet_gate`, `test_reorder_buffer`, `test_relay_routing`, `test_relay_queue`, `test_relay_send`, `test_unaddressed_connection`, `test_message_statistics`) |
| `udp_bridge_interfaces` | rosidl | 13 messages + 3 services (`Subscribe`, `AddRemote`, `ListRemotes`) |

`udp_bridge` depends on `rclcpp`, `rclcpp_lifecycle`, `diagnostic_updater`,
`lifecycle_msgs`, `udp_bridge_interfaces`, and ZLIB. `std_msgs` is
test-only in CMake (REQUIRED under `BUILD_TESTING` for the QoS-matching
integration test) though listed as a full `<depend>` in `package.xml`.

## Repository Layout

```
udp_bridge/
├── include/udp_bridge/   # public headers (installed) — heavily commented,
│                         #   incl. locking + callback-group invariants
├── src/                  # library sources + udp_bridge_node.cpp (main)
├── test/                 # gtest suites (colcon test)
├── test/mininet/         # manual-run integration scripts — NOT wired into CI
├── launch/               # udp_bridge_launch.py + legacy ROS 1 test launches
├── config/example_params.yaml
└── doc/                  # conceptual_overview.md, qos_design.md (read this),
                          #   admission_control_design.md, resend_budget_design.md,
                          #   relay_design.md
udp_bridge_interfaces/    # msg/ + srv/ definitions
```

## Architecture Overview

`UDPBridge` is a **lifecycle node** (`rclcpp_lifecycle::LifecycleNode`): it
does nothing until transitioned Unconfigured → Inactive → Active.
`udp_bridge_launch.py` auto-configures and auto-activates; running the node
manually requires `ros2 lifecycle set ... configure` + `activate`.

`udp_bridge_node` runs a `MultiThreadedExecutor` (8 threads) with three
callback groups whose invariants are documented at the top of
`src/udp_bridge_node.cpp`:

- `socket_drain_group_` (mutually exclusive) — the hot path: a 10 ms
  `spin_once` timer drains the UDP socket and decodes packets. Must never be
  starved or the 500 KB kernel SO_RCVBUF fills and packets drop silently.
- `republish_group_` (reentrant) — forwarding-subscription callbacks that
  serialize local messages and send them over UDP.
- `periodic_group_` (mutually exclusive) — stats (1 s), bridge info (2 s),
  subscription update (1 s), diagnostics (1 s), and all six service handlers.
- A separate **publish worker thread** (`PublishQueue`, issue #10) does the
  rmw-touching republish of received data, so a stalled destination
  publisher can't wedge the socket drain. Bounded by
  `publish_queue_max_bytes`; drops oldest under back-pressure.
- A separate **relay worker thread** (`RelayQueue`, issue #51) forwards a
  message received from one remote to the OTHER remotes whose `topics_list`
  carries the same topic, for hub topologies (`boat → hub → operators`).
  Same reason for the thread: `Connection::send()`'s `sendto()` can retry for
  ~200 ms, which must not happen on the socket drain. Bounded by the
  `relay_queue_max_bytes` (default 64 MiB, clamped 1 MiB–2 GiB);
  drops oldest under back-pressure and reports them in the `relay queue`
  diagnostic. Only messages the stale-packet / reorder gate ADMITS are
  relayed: the relay form rides along inside the `PublishItem` (an optional
  `RelayItem`, null when relay is unreachable) so a buffered packet is
  forwarded when the buffer releases it, and never if it is dropped.
  `UDPBridge::enqueuePublish()` is the single admit-to-publish point. See
  `doc/relay_design.md`.

Wire format: `Packet`/`WrappedPacket` (packet.h, wrapped_packet.h), zlib
compression on send (`src/packet.cpp`), fragmentation above
`maximum_packet_size`, reassembly in `Defragmenter`. Missing packets are
re-requested for up to `kSentPacketTTL` (5 s, `resend_constants.h`); after
that the receiver gives up and the loss is permanent — the wire is
best-effort with loss reduction, never RELIABLE (see `doc/qos_design.md`).

## Key Parameters (verified in `on_configure`, src/udp_bridge.cpp)

| Parameter | Default | Notes |
|---|---|---|
| `name` | node name | Bridge name as seen by remote bridges (unique per network). **Max 23 chars** — the on-wire `source_node` field is 24 bytes. Longer fails `on_configure` (#51); it is NOT truncated, because a truncated name stops matching what remotes are configured with and silently disables the relay loop rule |
| `port` | `4200` | UDP listen port; clamped 0–65535 |
| `maximum_packet_size` | `1200` | Clamped 256–65500; default sized for a tunnelled link (WireGuard-over-cellular MTU), not the IPv4/UDP maximum (#58) |
| `drop_stale_packets` | `true` | Gate dropping late out-of-order resends per destination topic |
| `reorder_hold_window_ms` | `0.0` | Reorder/jitter buffer hold window (ms), global (#35); `0.0` = disabled. When > 0 (and `drop_stale_packets` on), a gap-opening packet is held up to this window so an out-of-order gap-filler publishes first; clamped 0–500 ms |
| `resend_giveup_warn_rate_per_s` / `..._error_rate_per_s` | `5.0` / `50.0` | Diagnostic thresholds; live-tunable via `ros2 param set` |
| `publish_queue_max_bytes` | 64 MiB | Clamped 1 MiB–2 GiB |
| `relay_queue_max_bytes` | 64 MiB | Byte budget for the relay worker queue (#51); clamped 1 MiB–2 GiB, same default and clamps as `publish_queue_max_bytes` |
| `remotes.<label>.name` | `""` (empty) | **Removed (#67)** — still declared (rclcpp surfaces a YAML override only for a declared parameter; this node does not set `automatically_declare_parameters_from_overrides`), never read for identity, and a **non-empty value fails `on_configure`** with an ERROR naming the parameter, the entry in use and the value — validated before the socket is opened. No carve-out when the value equals its label: presence is the error. An explicitly empty value is indistinguishable from an absent key (the declared default is `""`) and is accepted. Was, in #51 only, the wire name overriding the label |
| `remotes_list` | `[]` | Each entry IS the remote's wire name (#67), not just a parameter-path label: your label for a peer must be the name that peer calls itself. An entry that is empty, over 23 chars, or equal to this bridge's own `name` fails `on_configure` (validated before the socket is opened; `""` is a reserved sentinel on the send path); a repeated entry is idempotent. The `add_remote` service applies the same over-long and self-name refusals at runtime (ERROR, no remote created). Then per-remote `remotes.<r>.connections_list`, per-connection `host`, `port`, `return_host`, `return_port`, `maximum_bytes_per_second` (0 → default **50000** B/s, `Connection::default_rate_limit`), `resend_budget_fraction` (0.25 — max fraction of measured **goodput** resends may consume; the basis moved from the admission cap to goodput in #52, #44, `doc/resend_budget_design.md`), `admission_floor_bytes_per_second` (**8192** B/s — absolute floor of the AIMD-adjusted admission cap, clamped at use to the connection's own `maximum_bytes_per_second`; negative/NaN fall back to the default; replaced the removed cap-relative `admission_floor_fraction` in #52), `link_headroom_fraction` (0.2 — on a congested sample the cap targets `(1 − this) × goodput`, leaving a share for co-tenant/operator traffic, #52, `doc/admission_control_design.md`), `topics_list`, and per-topic `source` (default: topic label), `destination` (default: source), `queue_size` (10), `period` (0.0), `reliability`, `durability`, `history_depth` (0, clamped ≤ 10000) |

See `config/example_params.yaml` for the nested structure.

## Topics & Services (verified in src/udp_bridge.cpp, src/remote_node.cpp)

Names are built as `<node_name>/...` (relative, so they resolve under the
node's namespace). With the default node name: `udp_bridge/bridge_info`
(`BridgeInfo`, latched transient_local keep-last-1), `udp_bridge/topic_statistics`
(`TopicStatisticsArray`, depth 10), and per-remote latched
`udp_bridge/remotes/<remote>/bridge_info` + `.../topic_statistics`.
Diagnostics go to `/diagnostics` via `diagnostic_updater` at ~1 Hz
(per-connection status, per-remote resend give-ups, publish-queue health,
and — only when `reorder_hold_window_ms` > 0 with `drop_stale_packets` on —
per-remote reorder-buffer occupancy/counters).
Forwarded topics use dynamically created generic publishers/subscriptions.

Services (all `udp_bridge_interfaces/srv`, on `<node_name>/...`):

| Service | Type | Direction |
|---|---|---|
| `remote_subscribe` | `Subscribe` | Ask the **remote** to forward its `source_topic` to us |
| `remote_advertise` | `Subscribe` | Forward a **local** `source_topic` to the remote |
| `remove_subscribe` / `remove_advertise` | `Subscribe` | Mirrors of the above (idempotent removal) |
| `add_remote` | `AddRemote` | Add/update a named remote + connection at runtime |
| `list_remotes` | `ListRemotes` | Enumerate known remotes/connections |

## Key Files to Read First

1. `udp_bridge/src/udp_bridge_node.cpp` — callback-group + threading invariants (top comment)
2. `udp_bridge/src/udp_bridge.cpp` — locking convention (top comment) + `on_configure`
3. `udp_bridge/doc/qos_design.md` — QoS policy and the rmw_zenoh_cpp RELIABLE workaround
   (and `doc/relay_design.md` before touching the receive path — relay runs there)
4. `udp_bridge/include/udp_bridge/resend_constants.h` — resend-protocol timing contract
5. `udp_bridge/config/example_params.yaml` — parameter structure

## Build & Test

```bash
# From the layer workspace directory (e.g., layers/main/core_ws/)
colcon build --symlink-install --packages-select udp_bridge_interfaces udp_bridge
# setup.bash must be sourced in the same shell
colcon test --packages-select udp_bridge udp_bridge_interfaces && colcon test-result --verbose
```

The gtest suite runs in plain `colcon test`. The scripts under
`udp_bridge/test/mininet/` are **manual-run only** — not registered in
CMakeLists at all (nothing gates them; they simply aren't test targets):
mininet needs root + netns and the multilink scaffold is still ROS 1
(`roscore`/`rosmon`) — see `test/mininet/README.md`. Only `recv_q_trace.py`
there works today.

## Common Pitfalls

- **`UDP_BRIDGE_BUILD_TESTING` ODR discipline** (CMakeLists.txt comments):
  test-only members gated by this macro (e.g.
  `Connection::resend_call_count_for_test_`) change `sizeof(Connection)`.
  Every in-package target that compiles against those headers — the library,
  `udp_bridge_node`, and each test — must define the macro identically, or
  layouts mismatch and tests segfault. Register new tests via the
  `add_udp_bridge_gtest()` helper; never hand-roll `ament_add_gtest` here.
  The macro is package-namespaced (not plain `BUILD_TESTING`) so downstream
  consumers of the installed headers can't trip the same hazard.
- **Lifecycle:** a plain `ros2 run` leaves the node Unconfigured and silent.
  Parameters are read in `on_configure` (via the reentrancy-safe
  `declareIfMissing` — bare `declare_parameter` breaks cleanup→configure).
- **Destination-publisher reliability is RELIABLE, not BEST_AVAILABLE**, as
  a temporary rmw_zenoh_cpp 0.2.9 workaround (`qos_resolution.h`,
  `doc/qos_design.md`). Design intent is BEST_AVAILABLE; per-topic
  `reliability:` config overrides.
- **Rate fields are bytes per second, not bits**; the per-connection default
  when `maximum_bytes_per_second` is 0 is **50000 B/s** — the
  `example_params.yaml` comment claiming 500000 is wrong (that number is the
  socket buffer size).
- **A `remotes_list` entry IS the remote's wire name (#67)** — there is one
  identity namespace, not two. The entry both spells the remote's parameter
  paths (`remotes.<label>.*`) and is what `remote_nodes_`,
  `subscribers_[topic].remote_details`, the per-remote topics/diagnostics
  and the service `remote`/`name` arguments key on. So **your label for a
  peer must be the name that peer calls itself** — every config in this
  workspace already does this. Two consequences worth knowing:
  - `remotes.<label>.name` is **removed, and a config that still sets it
    fails `on_configure`**. It is still declared (that is the only way
    rclcpp surfaces a YAML override, since the node does not set
    `automatically_declare_parameters_from_overrides`) purely so the stale
    key can be *detected*; a non-empty value is then an ERROR and a
    `CallbackReturn::FAILURE`, returned ahead of the socket bind. Warning
    instead would leave the one shape that reintroduces the #51 echo — a
    value differing from its label — announced and then ignored, so it is
    rejected on presence, with no carve-out for a value equal to its label.
    Do not add it back as an identity input; see the upgrade note in
    `udp_bridge/README.md`. It briefly *was* authoritative, between #51 and
    #67 — a `name:` in an old config is not a typo, it is a removed
    feature, and the node will tell the operator so by refusing to start.
  - **A rename does not survive a live re-configure.** `on_cleanup` clears
    neither `remote_nodes_` nor `subscribers_`, so renaming an entry and
    cycling deactivate→cleanup→configure leaves the old `RemoteNode`
    resident on the same host/port and doubles every send to that peer.
    Restart the node instead. Pruning on cleanup is an open follow-up.
  - An entry is now both a ≤23-byte wire identity **and** a ROS 2
    parameter-path segment, and ROS 2 uses `.` to separate segments. A peer
    whose wire name contains a `.` is therefore unconfigurable — not
    truncated, not warned about, inexpressible. No config in this workspace
    hits this.
- **Node names are capped at 23 characters and over-long ones are REJECTED,
  not truncated (#51).** `setName`, `resolveRemoteIdentities` and the
  `add_remote` service all refuse; only `WrappedPacket`'s constructor still
  clamps, as a last line of defence a validated config cannot reach.
  Truncation is what manufactures the identity mismatch the relay loop rule
  depends on not having, and it would also let two `remotes_list` entries
  differing only after char 23 collapse onto one wire identity, silently
  merging two remotes' connections and rate limits. Wire-side names
  get bounded reads (`wire_field_to_string`), not length policy — a long one
  arrives already shortened. The read stops one byte short of the field (the
  trailing byte is the terminator), so decode is the exact inverse of encode
  and an unterminated field cannot yield a 24-character name.
- **A remote may not be named as this bridge is named, and may not be
  unnamed (#51).** Both fail `on_configure` — since #67 that means a
  `remotes_list` entry equal to our `name`, or an empty one; `add_remote`
  also refuses the
  self-name at runtime. Our own name in `remote_nodes_` puts `unwrap()`'s
  self-packet refusal out of reach (it only runs on a lookup miss), and the
  empty name is a send-path sentinel for "connection request", so a remote
  filed under it can never be routed to.
- **Connection config is one complete per-connection block**: partial
  overrides silently break return-path routing (boat replies to the wrong
  host, data flow drops to zero).
- **Relay has no parameter, and no cycle protection.** The per-remote
  `topics_list` is the relay routing table: a second remote listing an
  already-carried topic starts that traffic flowing to it. The only loop
  protection is "never send back to the immediate sender", so **star
  topologies only** — `A → B → C → A` would circulate a message
  indefinitely (`doc/relay_design.md`).
- **Threading:** never add blocking work to the socket-drain path; respect
  the lock-briefly-copy-out convention documented at the top of
  `udp_bridge.cpp` and the group assignments in `udp_bridge_node.cpp`.
- **Appending fields to `Remote.msg`/`MessageInternal.msg` changes the ROS 2
  type hash** — mixed-version bridges need a coordinated redeploy (see the
  schema-evolution note in `Remote.msg`).

## Instructions for Use

Read the threading/locking comments and `doc/qos_design.md` before touching
the send, receive, or QoS paths. Verify any documentation claim against the
source before relying on it.
