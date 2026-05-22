# Bench-test harness

Linux-netns + `tc qdisc netem` bench reproducing field failure modes
of `udp_bridge` (resend amp, wedge, stats stall, rate-limit overshoot)
off the boat. See [issue #18](https://github.com/rolker/udp_bridge/issues/18)
for the design contract.

## Prerequisites

Ubuntu 24.04+ kernels default `apparmor_restrict_unprivileged_userns`
to 1, which blocks `unshare -Urn` for unprivileged users. The bench
needs that to work. One-time host setup:

```bash
echo 0 | sudo tee /proc/sys/kernel/apparmor_restrict_unprivileged_userns
echo "kernel.apparmor_restrict_unprivileged_userns = 0" \
    | sudo tee /etc/sysctl.d/60-apparmor-userns.conf
```

The pytest smoke test detects whether `unshare -Urn` is available and
skips with a clear reason if not — so a fresh clone on a hardened
machine does not false-fail. The full scenario (opt-in via
`UDP_BRIDGE_BENCH_SCENARIOS=1`) also skips.

## Scenarios

`run_scenario.py` supports three scenarios:

| Scenario | Tiers active | How it runs |
|---|---|---|
| `smoke`              | Critical only                  | pytest under `colcon test`; asserts sub_count > 0 |
| `full-mix`           | Critical + Telemetry + Bulk    | manual `run_scenario.py` invocation; exits 0 iff every tier sub_count > 0 |
| `range_degradation`  | Critical + Telemetry + Bulk    | pytest opt-in via `UDP_BRIDGE_BENCH_SCENARIOS=1`; 5 single-path invariants |

`full-mix` actively generates the pinned three-tier mix
(Critical 1 Hz × 64 B, Telemetry 10 Hz × ~720 B, Bulk 10 Hz × 480 KB)
on a clean link. Bulk is sized at ~120% of the WiFi rate budget, so
the bridge's rate limiter trims it even on a clean link — that's
intentional (Phase 4 drop-by-tier invariant); Phase 2 only confirms
each tier delivers something. To run:

```bash
python3 udp_bridge/test/bench/run_scenario.py --scenario full-mix --duration-s 10
# set UDP_BRIDGE_BENCH_DEBUG=1 to keep child stderr visible and preserve the outdir
```

Per-tier counts are printed as `BENCH_RESULT_<TIER>_{PUB,SUB}_COUNT=...`.

`range_degradation` walks the WiFi path through the sail-out-and-return
trajectory (in_range_clean → fringe → lossy → critical → over_horizon
→ critical → lossy → fringe → in_range_clean), holding each phase for
`--hold-s` seconds (default 10), while the full three-tier mix runs.
A `ros2 bag record` captures `/operator_bridge/bridge_info` and
`/operator_bridge/topic_statistics` (plus the operator-side mirrors
of the boat-side equivalents); `recv_q_trace.py` writes a CSV of the
operator UDP socket's Recv-Q over the run; per-bridge stderr is tee'd
so the test can scan for ERROR-severity lines during the over-horizon
window. The orchestrator emits marker lines so the test can find each
artifact:

```
BENCH_BAG_DIR=<path>
BENCH_PHASE_LOG=<path>
BENCH_RECVQ_OPERATOR=<path>
BENCH_BRIDGE_STDERR_OPERATOR=<path>
BENCH_BRIDGE_STDERR_BOAT=<path>
BENCH_RESULT_<TIER>_{PUB,SUB}_COUNT=<n>
```

To run the scenario by hand:

```bash
python3 udp_bridge/test/bench/run_scenario.py --scenario range_degradation --hold-s 10
```

To run the pytest invariants:

```bash
UDP_BRIDGE_BENCH_SCENARIOS=1 colcon test --packages-select udp_bridge \
    --pytest-args -k test_bench_range_degradation
```

The five single-path invariants checked are:

1. **Recv-Q stability** — no monotonic climb longer than N seconds at any phase boundary.
2. **No ERROR/FATAL log during over_horizon** — graceful disconnect is a mode, not a fault.
3. **Recovery completeness** — `message.success_bytes_per_second` reaches ≥ X% of the pre-event in_range baseline (over the final in_range phase, which serves as the T-second convergence window).
4. **Resend amplification** — `resend.success_bytes_per_second / message.success_bytes_per_second` ≤ F × applied netem loss rate in each lossy phase.
5. **Stats publication rate floor** — `bridge_info` and `topic_statistics` publication rate stays ≥ R% of the in-range baseline in every phase. Catches #20-class stalls in the stats path while the data plane keeps working.

Threshold values N/X/T/F/R live in the "Values" table below.

## Threshold values

Concrete values for the invariants defined in
[issue #18](https://github.com/rolker/udp_bridge/issues/18). Feeds the
assertions in `test_range_degradation.py`.

## Values

| Symbol | Value | Source |
|---|---|---|
| **F** | 2× | ~13× pathological (2026-05-01: `resend_dropped_bps` 13 MB/s on 1.0 MB/s VPN) vs. ~30% structural overhead (2026-05-18 pier). 2× is well below pathological. |
| **G** | 6 s | 2× the observed non-degraded worst-case heartbeat inter-arrival in 2026-05-01 (~3.0s; 18,900 samples; 99.97% under 2s). |
| **R** | 50% over 10s | Baseline 1Hz publication; #20 stall (sustained 0% for ~77 min) is well below. Tolerates brief transients; trips on sustained stall. |
| **N** | 10 s | No isolated wedge bag exists. Starting value from physical reasoning. Refine to 2× harness 95th-percentile recovery-edge drain time after first runs. |
| **X** | 90% | 2026-04-21 bags are MCAP-format; no SQLite DB yet. Starting value: post-recovery DataRates must be within 10% of pre-event baseline. Refine after 2026-04-21 import. |
| **T** | 30 s | As X. Recovery convergence target; refine. |
| **Y** | 10% | No field bag isolates cross-path poisoning (2026-05-01 was Starlink-only). Bootstrap from harness clean-link rates. |

## Methodology

**G** — `t_bizzy_marine_heartbeat` inter-arrivals from
`~/data/logs/analysis/2026-05-01_deployment.db`. 18,900 samples,
mean 1.028s. Top non-pathological gap is 2.99s; the 547s catastrophic
event (#124 Event 4) is excluded. G = 2 × 3.0s ≈ 6s.

**R** — `t_bizzy_udp_bridge_topic_statistics` and
`t_bizzy_udp_bridge_remotes_operator_topic_statistics` rates from
`~/data/logs/analysis/2026-05-19/bizzyboat.db`. Baseline ~60 msgs/min
(= 1Hz). #20 stall window 12:53–14:10 EDT shows sustained 0%.

> **Note from this query**: the operator-side stats topic that the boat
> *received* stayed steady through the 2026-05-19 WiFi-cut. This answers
> #20's open question (A vs B): the operator's local subscriber view
> stalled, but the bridge on both sides kept exchanging stats. The
> mechanism is operator-side downstream of the bridge, not in the bridge
> itself.

**F** — Anchored from existing analysis files; no fresh DB query needed.

**N**, **X**, **T**, **Y** — starting values from physical reasoning;
refinement plan below.

## Refinement plan

1. **N**: after several clean harness recoveries, set to 2× 95th-percentile recovery-edge Recv-Q drain time.
2. **X**, **T**: import 2026-04-21 bags to SQLite (or query via `rosbag2_py`); extract pre-event vs. post-recovery DataRates.
3. **Y**: after first harness run, set to observed variance of cell-path rate with WiFi idle.
4. **G**: tighten if subsequent deployments show consistently sub-2s worst case.
5. **R**: loosen if false-trips occur on legitimate brief stalls.

Each refinement lands as its own commit alongside the assertion change that consumes it.
