# Plan: Bench-test harness for udp_bridge field failure modes

## Issue
https://github.com/rolker/udp_bridge/issues/18

## Context

Single MVP scenario (range-degradation) reproducing the WiFi sail-out-and-return
trajectory under three-path topology (WiFi/Cell/Starlink), with a three-tier
topic mix (Critical/Telemetry/Bulk). Catches resend amplification (#9), wedge
(#10), stats-path stall (#20), and rate-limit overshoot together. Issue body is
the scope contract.

## Approach

### Phase 0: Threshold extraction (~1 hr, blocking)

Land concrete values in `udp_bridge/test/bench/README.md` before Phase 3:

- **F = 2×** (anchored: ~13× pathological from `2026-05-01_findings.md`; ~30%
  healthy from `2026-05-18_bandwidth_test.md`).
- **N**, **X**, **T**, **G**, **R** — extract via queries against the SQLite
  bag-analysis DBs (`~/data/logs/analysis/2026-05-01_deployment.db`,
  `~/data/logs/analysis/2026-05-19/bizzyboat.db`, plus a fresh extraction from
  the 2026-04-21 bags). Document the query for each.
- **Y** bootstrapped from MVP clean-link baseline; no field source.

### Phase 1: Orchestrator + smoke

- `udp_bridge/test/bench/run_scenario.py` — Python orchestrator using
  `unshare -Urn` to create a single user+net namespace; inside it,
  creates N veth pairs (one per simulated path) with distinct IPs per
  endpoint and applies `tc qdisc netem` per path. Both bridges run
  co-resident in the namespace and reach each other via the per-path
  IPs. (Nested `ip netns add` inside the user namespace doesn't work
  on Ubuntu 24.04+ — needs CAP_SYS_ADMIN on `/run/netns` mount — so
  two-namespace designs are off the table.)
- **Prerequisite**: Ubuntu 24.04+ defaults `kernel.apparmor_restrict_unprivileged_userns`
  to 1, which blocks `unshare -Urn` for unprivileged users. Documented
  one-time setup in `udp_bridge/test/bench/README.md` sets it to 0.
- `udp_bridge/test/bench/configs/three_path.yaml` — bridge config with WiFi/Cell/Starlink
  Connections.
- `udp_bridge/test/bench/pub.py`, `udp_bridge/test/bench/sub.py` — rclpy publisher/subscriber driven
  by a topic-mix spec.
- `udp_bridge/test/bench/test_smoke.py` — pytest smoke (clean link, 10s pub/sub, asserts
  `success_bytes_per_second > 0`). Skip if `unshare -rn` unavailable.
- `udp_bridge/test/bench/bag_reader.py` — small in-tree sqlite/`rosbag2_py`
  reader that pulls the few `BridgeInfo` / `TopicStatistics` fields the
  assertions need. Avoids a cross-repo runtime dep on `marine_tools`'
  `bag_analysis` package, which isn't reachable via rosdep.

### Phase 2: Topic mix + replication

Three-tier mix via `topics_list` replication in `three_path.yaml`. Critical
replicated on all three paths; Telemetry on WiFi+Starlink; Bulk on WiFi only.
Message types per tier: Critical / Telemetry use small fixed-shape messages
(`std_msgs/String` for heartbeat-equivalent, `nav_msgs/Odometry` for odom
stand-in); Bulk uses `sensor_msgs/Image` filled with synthetic random bytes
at a calibrated size × rate that lands at the WiFi link cap. No ffmpeg dep.

### Phase 3: Range-degradation trajectory + base assertions

Phase walker drives WiFi `tc` through the 6-phase trajectory. Capture
`bridge_info` + `recv_q_trace` bag during the run into a fresh
`tempfile.mkdtemp(prefix='udp_bridge_bench_')` (respects `$TMPDIR`; override
via `UDP_BRIDGE_BENCH_OUTDIR`). Cleanup on pass; preserve on fail.
`udp_bridge/test/bench/test_range_degradation.py` — opt-in via `UDP_BRIDGE_BENCH_SCENARIOS=1`,
asserts the 5 single-path invariants. Bag parsed via the in-tree
`bag_reader.py` from Phase 1 (no cross-repo dep).

### Phase 4: Multi-link assertions

Four multi-link invariants: topic-list confinement, cross-path non-poisoning,
Critical-topic gap during over-horizon, drop-by-tier (the `requires #19` one
measures-and-reports, does not fail).

### Phase 5: Documentation + retire mininet

- `udp_bridge/test/bench/README.md` — decision capture (netns vs mininet, walk-WiFi-only),
  run instructions, threshold values + rationale.
- Move the dead ROS 1 mininet scaffolding under
  `udp_bridge/test/mininet/.archived/` with a top-level `README.md` pointing
  to `udp_bridge/test/bench/`. Includes the directory's `*.bash`, `*.launch`,
  and `multilink.py`. Also move the two ROS-1-vintage launch files
  `udp_bridge/launch/test_mininet_multilink_operator.launch` and
  `.../test_mininet_multilink_robot.launch` under the same `.archived/`
  tree — they reference the dead scaffolding and have no callers.
- Keep `recv_q_trace.py` (it works standalone) by moving it to
  `udp_bridge/test/bench/recv_q_trace.py`.

### Phase 6: CI runner verification

Confirm `unshare -rn` + `tc qdisc` on `ubuntu-latest`. If unsupported, the smoke
`@pytest.mark.skipif` is the load-bearing fallback — document in the README.

## Files to Change

| File | Change |
|---|---|
| `udp_bridge/test/bench/run_scenario.py` | new — orchestrator |
| `udp_bridge/test/bench/configs/three_path.yaml` | new — bridge config |
| `udp_bridge/test/bench/pub.py`, `sub.py` | new — pub/sub scripts |
| `udp_bridge/test/bench/test_smoke.py` | new — pytest smoke |
| `udp_bridge/test/bench/test_range_degradation.py` | new — scenario (opt-in) |
| `udp_bridge/test/bench/README.md` | new — decisions + thresholds |
| `udp_bridge/test/bench/recv_q_trace.py` | move from `udp_bridge/test/mininet/` |
| `udp_bridge/test/mininet/` → `udp_bridge/test/mininet/.archived/` | archive-with-pointer for dead ROS 1 scripts |
| `udp_bridge/launch/test_mininet_multilink_{operator,robot}.launch` → `udp_bridge/test/mininet/.archived/` | archive ROS-1-vintage launch files (no callers) |
| `udp_bridge/package.xml` | `<test_depend>` adds: `python3-pytest`, `sensor_msgs`, `nav_msgs`, `util-linux` (for `unshare`), `iproute2` (for `tc`) |
| `udp_bridge/CMakeLists.txt` | register pytest tests via `ament_add_pytest_test` |

## Principles Self-Check

| Principle | Consideration |
|---|---|
| Test what breaks | Targets the exact field failure modes (#9/#10/#20). |
| Only what's needed | Single scenario; netns over mininet; opt-in scenario gate. |
| A change includes its consequences | Phase 5 retires mininet in same PR. |
| Capture decisions | README documents netns-vs-mininet, walk-WiFi-only. |
| Enforcement over docs | Smoke under `colcon test`; scenario opt-in. |

## ADR Compliance

| ADR | Triggered | How addressed |
|---|---|---|
| 0008 (ROS 2 conventions) | Yes | Pytest via `ament_add_pytest_test`; bridge via `ros2 launch`. |
| 0009 (Python deps) | Yes (in spirit, project repo per ADR-0003) | `python3-pytest` via rosdep test_depend. No bare pip. |

## Consequences

| If we change... | Also update... | Included? |
|---|---|---|
| Add `udp_bridge/test/bench/` | `udp_bridge/package.xml` test_depend + `udp_bridge/CMakeLists.txt` test registration | Yes |
| Smoke under `colcon test` | Skip-on-no-netns guard | Yes |
| Retire mininet | `udp_bridge/test/mininet/.archived/` with new `README.md` pointer to `udp_bridge/test/bench/`; sweep the two `launch/test_mininet_*.launch` files too | Yes |
| Use bag fields | In-tree `bag_reader.py` (no cross-repo dep on `marine_tools/bag_analysis`) | Yes |

## Estimated Scope

Single PR, ~6 commits (one per phase). Phase 0 (threshold extraction) is the
gating one — the rest depends on landed numbers. Phase 0 lands as its own
reviewable commit so the chosen values for N/X/T/F/G/R/Y are visible and
discussable before any harness code is written against them.
