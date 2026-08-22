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

Replication is already wired structurally in `three_path.yaml` (Critical on
all three connections, Telemetry on WiFi+Starlink, Bulk on WiFi only); Phase 2
activates the full traffic mix and makes the orchestrator launch one publisher
per tier plus one subscriber per destination topic.

**Tier specs** (also `TIER_DEFAULTS` in `pub.py`; Phase 1 landed Critical and
Telemetry at these values, Bulk needs to be bumped from the Phase 1 placeholder
of 5 Hz × 512 KiB):

| Tier | Msg type | Source topic | Rate | Payload | Net rate | Rationale |
|------|----------|--------------|------|---------|----------|-----------|
| Critical  | `std_msgs/String`   | `/boat/critical/heartbeat` | 1 Hz  | 64 B           | ~64 B/s    | 1 Hz heartbeat matches udp_bridge `bridge_info` cadence |
| Telemetry | `nav_msgs/Odometry` | `/boat/telemetry/odom`     | 10 Hz | fixed (~720 B) | ~7.2 kB/s  | 10 Hz nav-rate convention for marine vehicles |
| Bulk      | `sensor_msgs/Image` | `/boat/bulk/image`         | 10 Hz | 480 KB         | ~4.8 MB/s  | ~120% of WiFi 4 MB/s budget → forces drop-by-tier when WiFi degrades (Phase 4 invariant) |

Bulk image stays 1×N synthetic mono8 (no resolution semantics, payload sized by
bytes) — no ffmpeg dep. Telemetry+Critical combined are <0.2% of the WiFi
budget, so Bulk effectively owns the link.

**Topic naming** (already enumerated in `three_path.yaml`; stated here as the
convention):

- Source (boat side): `/boat/<tier>/<name>`
- Destination (operator side): `/operator/boat/<tier>/<name>` — explicit
  per-topic destination string in the YAML, matching production's `<operator>/<boat-name>/...` shape
- Replication is done by the bridge, not by duplicate publishers: ONE
  publisher per source topic; udp_bridge fans out across the connections that
  list the topic in their `topics_list`

**Launch shape**: `run_scenario.py` extends to spawn three `pub.py` processes
(one per tier, parameterized via the existing `--tier` flag) and three `sub.py`
processes (one per destination topic). Each sub already emits a
`BENCH_SUB_COUNT=` stderr line; the orchestrator collects all six counters and
reports per-tier delivery counts. Smoke remains a 1-publisher/1-subscriber
Critical-only subset; the full mix is opt-in via a new `--scenario full-mix`.

### Phase 3: Range-degradation trajectory + base assertions

Phase walker drives WiFi `tc` through the 6-phase trajectory. Capture
`bridge_info` + `recv_q_trace` bag during the run into a fresh
`tempfile.mkdtemp(prefix='udp_bridge_bench_')` (respects `$TMPDIR`; override
via the `--outdir` flag — the planned `UDP_BRIDGE_BENCH_OUTDIR` env var was
not implemented, and `BENCH_OUTDIR=` in the output is a marker line the tests
parse, not an input). Cleanup on pass; preserve on fail.
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

## Implementation Notes (2026-05-25)

Phases 0–3 landed earlier (HEAD `191e5e1`). This session completed Phases 4–6
plus an added scenario:

- **Phase 4 — multi-link invariants** (in `test_range_degradation.py`, same
  run): topic-list confinement, cross-path non-poisoning (Y), critical gap
  over-horizon (G), drop-by-tier (measure-and-report, #19). Added the
  republished Critical heartbeat to the recorded bag and a generalized
  `bag_reader.get_connection` / `topic_connection_ids`. All 9 range_degradation
  invariants pass (~112 s run under FastDDS).
- **Phase 5 — retire mininet**: dead ROS-1 scripts + the two
  `test_mininet_*.launch` files moved under `test/mininet/.archived/`; pointer
  README added; `mainpage.dox` repointed to `test/bench/`.
- **Phase 6 — CI/docs**: this repo has no CI workflow; documented that the
  skip-guards + opt-in gating are the protection (smoke skips cleanly without
  unprivileged userns). README updated for the new scenario + invariants.

- **Added scenario — `subscriber_death` (issue #10 wedge), not in the original
  Phase 0–6 plan.** Freezes (SIGSTOP) the Bulk operator subscriber mid-stream
  and traces the operator Recv-Q + survivor delivery; forces `rmw_zenoh_cpp` +
  a Zenoh router. **Finding:** the wedge did NOT reproduce at bench scale under
  FastDDS, CycloneDDS, or Zenoh — even at ~58 MB/s into a frozen consumer,
  Recv-Q/Send-Q stayed flat at 0. `publish()` doesn't block the drain at bench
  scale (Zenoh async hand-off). So `test_subscriber_death` is a **no-wedge
  regression guard**, not a red→green reproduction; the component-level proof
  of #28's fix remains the `PublishQueue` unit tests. Full rationale in the
  test's module docstring and `test/bench/README.md`.
