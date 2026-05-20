# Bench-test threshold values

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
