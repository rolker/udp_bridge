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
| `udp_bridge` | C++17 (ament_cmake) | `udp_bridge` library, `udp_bridge_node` executable, 15 gtest targets (`utest`, `test_utilities`, `test_defragmenter`, `test_qos_resolution`, `test_qos_matching_integration`, `test_connection_rate_limit`, `test_remote_node_resend`, `test_connection_cleanup`, `test_giveup_diagnostic`, `test_resend_budget`, `test_admission_control`, `test_publish_queue`, `test_subscriber_registry`, `test_stale_packet_gate`, `test_reorder_buffer`) |
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
└── doc/                  # conceptual_overview.md, qos_design.md (read this)
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

Wire format: `Packet`/`WrappedPacket` (packet.h, wrapped_packet.h), zlib
compression on send (`src/packet.cpp`), fragmentation above
`maximum_packet_size`, reassembly in `Defragmenter`. Missing packets are
re-requested for up to `kSentPacketTTL` (5 s, `resend_constants.h`); after
that the receiver gives up and the loss is permanent — the wire is
best-effort with loss reduction, never RELIABLE (see `doc/qos_design.md`).

## Key Parameters (verified in `on_configure`, src/udp_bridge.cpp)

| Parameter | Default | Notes |
|---|---|---|
| `name` | node name | Bridge name as seen by remote bridges (unique per network) |
| `port` | `4200` | UDP listen port; clamped 0–65535 |
| `maximum_packet_size` | `65500` | Clamped 256–65500 |
| `drop_stale_packets` | `true` | Gate dropping late out-of-order resends per destination topic |
| `reorder_hold_window_ms` | `0.0` | Reorder/jitter buffer hold window (ms), global (#35); `0.0` = disabled. When > 0 (and `drop_stale_packets` on), a gap-opening packet is held up to this window so an out-of-order gap-filler publishes first; clamped 0–500 ms |
| `resend_giveup_warn_rate_per_s` / `..._error_rate_per_s` | `5.0` / `50.0` | Diagnostic thresholds; live-tunable via `ros2 param set` |
| `publish_queue_max_bytes` | 64 MiB | Clamped 1 MiB–2 GiB |
| `remotes_list` | `[]` | Then per-remote `remotes.<r>.connections_list`, per-connection `host`, `port`, `return_host`, `return_port`, `maximum_bytes_per_second` (0 → default **50000** B/s, `Connection::default_rate_limit`), `resend_budget_fraction` (0.25 — max fraction of the cap resends may consume, #44, `doc/resend_budget_design.md`), `admission_floor_fraction` (0.1 — floor of the AIMD-adjusted admission cap, #43, `doc/admission_control_design.md`), `topics_list`, and per-topic `source` (default: topic label), `destination` (default: source), `queue_size` (10), `period` (0.0), `reliability`, `durability`, `history_depth` (0, clamped ≤ 10000) |

See `config/example_params.yaml` for the nested structure.

## Topics & Services (verified in src/udp_bridge.cpp, src/remote_node.cpp)

Names are built as `<node_name>/...` (relative, so they resolve under the
node's namespace). With the default node name: `udp_bridge/bridge_info`
(`BridgeInfo`, latched transient_local keep-last-1), `udp_bridge/topic_statistics`
(`TopicStatisticsArray`, depth 10), and per-remote latched
`udp_bridge/remotes/<remote>/bridge_info` + `.../topic_statistics`.
Diagnostics go to `/diagnostics` via `diagnostic_updater` at ~1 Hz
(per-connection status, per-remote resend give-ups, publish-queue health).
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
- **`remotes.<label>.name` is not a real parameter** — the remote's name is
  the `remotes_list` label itself. The old `udp_bridge/README.md` and the
  example YAML comment claiming otherwise are stale.
- **Connection config is one complete per-connection block**: partial
  overrides silently break return-path routing (boat replies to the wrong
  host, data flow drops to zero).
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
