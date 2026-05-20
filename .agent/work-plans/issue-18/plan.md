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

Land concrete values in `test/bench/README.md` before Phase 3:

- **F = 2×** (anchored: ~13× pathological from `2026-05-01_findings.md`; ~30%
  healthy from `2026-05-18_bandwidth_test.md`).
- **N**, **X**, **T**, **G**, **R** — extract via queries against the SQLite
  bag-analysis DBs (`~/data/logs/analysis/2026-05-01_deployment.db`,
  `~/data/logs/analysis/2026-05-19/bizzyboat.db`, plus a fresh extraction from
  the 2026-04-21 bags). Document the query for each.
- **Y** bootstrapped from MVP clean-link baseline; no field source.

### Phase 1: Orchestrator + smoke

- `test/bench/run_scenario.py` — Python orchestrator using `unshare -rn` to
  create operator+boat netns connected by N veth pairs; applies `tc qdisc
  netem` per path.
- `test/bench/configs/three_path.yaml` — bridge config with WiFi/Cell/Starlink
  Connections.
- `test/bench/pub.py`, `test/bench/sub.py` — rclpy publisher/subscriber driven
  by a topic-mix spec.
- `test/bench/test_smoke.py` — pytest smoke (clean link, 10s pub/sub, asserts
  `success_bytes_per_second > 0`). Skip if `unshare -rn` unavailable.

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
`test/bench/test_range_degradation.py` — opt-in via `UDP_BRIDGE_BENCH_SCENARIOS=1`,
asserts the 5 single-path invariants. Bag parsed via
`bag_analysis/extractors/udp_bridge.py`.

### Phase 4: Multi-link assertions

Four multi-link invariants: topic-list confinement, cross-path non-poisoning,
Critical-topic gap during over-horizon, drop-by-tier (the `requires #19` one
measures-and-reports, does not fail).

### Phase 5: Documentation + retire mininet

- `test/bench/README.md` — decision capture (netns vs mininet, walk-WiFi-only),
  run instructions, threshold values + rationale.
- Move `test/mininet/` to `test/mininet/.archived/` with a `README.md` pointer
  to `test/bench/`; keep `recv_q_trace.py` (it works standalone) by moving it
  to `test/bench/recv_q_trace.py`.

### Phase 6: CI runner verification

Confirm `unshare -rn` + `tc qdisc` on `ubuntu-latest`. If unsupported, the smoke
`@pytest.mark.skipif` is the load-bearing fallback — document in the README.

## Files to Change

| File | Change |
|---|---|
| `test/bench/run_scenario.py` | new — orchestrator |
| `test/bench/configs/three_path.yaml` | new — bridge config |
| `test/bench/pub.py`, `sub.py` | new — pub/sub scripts |
| `test/bench/test_smoke.py` | new — pytest smoke |
| `test/bench/test_range_degradation.py` | new — scenario (opt-in) |
| `test/bench/README.md` | new — decisions + thresholds |
| `test/bench/recv_q_trace.py` | move from `test/mininet/` |
| `test/mininet/` → `test/mininet/.archived/` | retire dead ROS 1 scripts |
| `package.xml` | `<test_depend>` adds: `python3-pytest`, `sensor_msgs`, `nav_msgs` |
| `CMakeLists.txt` | register pytest tests via `ament_add_pytest_test` |

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
| Add `test/bench/` | `package.xml` test_depend + `CMakeLists.txt` test registration | Yes |
| Smoke under `colcon test` | Skip-on-no-netns guard | Yes |
| Retire mininet | `test/mininet/README.md` (replaced) | Yes |

## Estimated Scope

Single PR, ~6 commits (one per phase). Phase 0 (threshold extraction) is the
gating one — the rest depends on landed numbers.
