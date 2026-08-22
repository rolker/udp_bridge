# Bench-test harness

Linux-netns + `tc qdisc netem` bench reproducing field failure modes
of `udp_bridge` (resend amp, wedge, stats stall, rate-limit overshoot)
off the boat. See [issue #18](https://github.com/rolker/udp_bridge/issues/18)
for the design contract.

## Topology

Two network namespaces — **operator** (the orchestrator's own, from the
`unshare -Urn` re-exec) and **boat** (a nested one, held open by a keeper
process and addressed by PID via `nsenter -t <pid> -n`) — joined by three
veth pairs standing in for WiFi, Cell, and Starlink.

The namespace split is load-bearing, not tidiness. With both ends of a veth
pair addressed in one namespace the kernel installs a `local` route for each
address and delivers between them over `lo`: nothing egresses the veth, so
the `netem` qdisc attached to it is never traversed and **every impairment is
silently a no-op**. That was the state of this harness until 2026-08-22 —
a 100%-loss qdisc dropped nothing and the device's TX counter stayed at zero.
Anyone tempted to collapse this back to a single namespace should reproduce
that first.

`netem` shapes egress only, so each path's profile is applied to **both**
ends: boat→operator carries the bulk/telemetry flow, operator→boat carries
resend and subscribe requests, and a real radio link degrades both. Note the
consequence when reading thresholds — `delay_ms` in the profiles is a one-way
delay, so a round trip costs `2 × delay_ms`.

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

`run_scenario.py` supports four scenarios:

| Scenario | Tiers active | How it runs |
|---|---|---|
| `smoke`              | Critical only                  | pytest under `colcon test`; asserts sub_count > 0 |
| `full-mix`           | Critical + Telemetry + Bulk    | manual `run_scenario.py` invocation; exits 0 iff every tier sub_count > 0 |
| `range_degradation`  | Critical + Telemetry + Bulk    | pytest opt-in via `UDP_BRIDGE_BENCH_SCENARIOS=1`; 6 single-path + 4 multi-link invariants |
| `subscriber_death`   | Critical + Telemetry + Bulk    | pytest opt-in via `UDP_BRIDGE_BENCH_SCENARIOS=1`; **forces Zenoh**; no-wedge regression guard (see below) |

`full-mix` actively generates the pinned three-tier mix
(Critical 1 Hz × 64 B, Telemetry 10 Hz × ~720 B, Bulk 10 Hz × 480 KB)
on a clean link. Bulk is sized at ~120% of the WiFi rate budget
(10 Hz × 480 KB = 4.8 MB/s vs the 4 MB/s WiFi cap), so the bridge's
rate limiter trims it even on a clean link — that's intentional
(Phase 4 drop-by-tier invariant). Since #57 the payload is
incompressible, so that ~120% is realized on the wire; before #57 the
all-zeros payload compressed to a single packet and the limiter never
engaged. Phase 2 only confirms each tier delivers something. To run:

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
operator UDP socket's Recv-Q over the run; per-bridge stderr is captured
to a log file (not tee'd to the terminal) so the test can scan for
ERROR-severity lines during the over-horizon window. The orchestrator emits marker lines so the test can find each
artifact:

```
BENCH_BAG_DIR=<path>
BENCH_PHASE_LOG=<path>
BENCH_RECVQ_OPERATOR=<path>
BENCH_BRIDGE_STDERR_OPERATOR=<path>
BENCH_BRIDGE_STDERR_BOAT=<path>
BENCH_COTENANT=<path>
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

The six single-path invariants checked are:

1. **Recv-Q stability** — no monotonic climb longer than N seconds at any phase boundary.
2. **No ERROR/FATAL log during over_horizon** — graceful disconnect is a mode, not a fault.
3. **Recovery completeness** — `message.success_bytes_per_second` reaches ≥ X% of the pre-event in_range baseline (over the final in_range phase, which serves as the T-second convergence window).
4. **Resend amplification** — `resend.success_bytes_per_second / message.success_bytes_per_second` ≤ F × applied netem loss rate in each lossy phase.
4b. **Co-tenant management flow survives** (#52) — a small co-tenant flow sharing the impaired path (traced via the `BENCH_COTENANT` CSV) delivers ≥ `K_cotenant_delivery_pct` of its own link-survivable rate in every phase the path can carry traffic (each phase window evaluated independently), and its p95 one-way latency stays under `K_cotenant_p95_latency_s`. This is the operator-lockout protection `link_headroom_fraction` provides, made testable.
5. **Stats publication rate floor** — `bridge_info` and `topic_statistics` publication rate stays ≥ R% of the in-range baseline in every phase. Catches #20-class stalls in the stats path while the data plane keeps working.

The four multi-link invariants (same run) are:

6. **Topic-list confinement** — each forwarded topic is advertised only on its configured connections (Bulk→WiFi, Telemetry→WiFi+Starlink, Critical→all three); a leak onto a disallowed connection fails.
7. **Cross-path non-poisoning** — when WiFi is over-horizon, the always-on paths (cell, starlink) keep delivering: their received rate stays ≥ (1−Y) of baseline.
8. **Critical gap over-horizon** — no Critical inter-arrival gap overlapping an over-horizon window exceeds G (the always-on paths keep Critical flowing while WiFi is dark).
9. **Drop-by-tier (measure-and-report)** — records per-tier send success/dropped in the most-degraded phase. Does **not** fail: preferential tier dropping needs per-topic scheduling ([#19](https://github.com/rolker/udp_bridge/issues/19), not yet implemented).

The two Bulk acceptance invariants ([#57](https://github.com/rolker/udp_bridge/issues/57), same run) are:

10. **Fragmentation exercised** — the boat send-side `average_fragment_count` for `/boat/bulk/image` stays ≥ `T_fragment_floor` in the clean in-range phase. With the incompressible 480 KB payload and a 1000-byte `maximum_packet_size`, an image fragments into ≥ 480 pieces (`floor(480000/1000)`; per-fragment headers push it higher), so the resend/reassembly machinery is exercised for real. The pre-#57 single-packet bug would drive this to ~1 and fail here. (#57 Acceptance criterion 2.)
11. **Bulk wire rate** — the boat send-side per-topic Bulk *offered* rate (`send.success_bytes_per_second` + `send.dropped_bytes_per_second`) clears `W_wire_pct` × the ~4.8 MB/s nominal Bulk offered load in the clean in-range phase. Bulk is offered at ~4.8 MB/s (120% of the 4 MB/s WiFi budget) and the limiter is entitled to shed the excess toward the cap — so the invariant measures what the tier *presents*, not what survives; this catches a regression that silently drops Bulk back to the pre-#57 ~0.5 kB/s single-packet trickle. Sourced per-topic from `topic_statistics`, not the WiFi connection aggregate. (#57 Acceptance criterion 1.)

Threshold values N/X/T/F/R/Y/G, the co-tenant K thresholds
(`K_cotenant_delivery_pct`, `K_cotenant_p95_latency_s`), and the #57
Bulk thresholds (`T_fragment_floor`, `W_wire_pct`) live in the
"Values" table below.

### `subscriber_death` — stalled-subscriber (wedge) regression guard

Reproduces the issue [#10](https://github.com/rolker/udp_bridge/issues/10)
trigger: a Bulk operator-side subscriber is frozen (`SIGSTOP` — a
matched-but-not-draining reader, the back-pressure case; a cleanly-killed one
just unmatches) mid-stream while the boat keeps forwarding Bulk. The operator
bridge's UDP Recv-Q (port 4200) is traced, and the surviving Critical /
Telemetry subscribers are count-traced across the stall (the head-of-line
question, [#29](https://github.com/rolker/udp_bridge/issues/29)). **Forces
`rmw_zenoh_cpp` + a Zenoh router**, since the back-pressure publish path does
not exist under DDS.

**Finding (2026-05-25):** the wedge could **not** be reproduced at bench scale
— across FastDDS, CycloneDDS, and Zenoh, with clean-kill and freeze triggers,
with Bulk streaming into a frozen consumer, the operator Recv-Q and Send-Q
stayed flat at 0 and the surviving tiers were unaffected. At bench scale
`publish()` does not block the drain thread (under Zenoh it's an async
hand-off). **Correction (#57):** that run used the pre-#57 all-zeros Bulk
payload, which compressed to a single ~488 B packet — so the harness pushed
only ~5 kB/s of single-packet traffic, not the ~58 MB/s originally claimed.
The no-wedge finding was reached in a zero-fragmentation regime; #57 now
streams real incompressible, fragmented Bulk (~4.8 MB/s, ~480+ fragments per
image), so it must be re-validated on the host bench run. The field wedge (4× on 2026-04-27, `rmw_zenoh_cpp`) evidently needs
conditions this harness can't hit at bench scale, or a root cause other than
the back-pressure hypothesis the issue records. So `test_subscriber_death`
is a **no-wedge regression guard** — it asserts the healthy behavior (Recv-Q
bounded, survivors keep delivering) and would fail if a future change
reintroduced drain-blocking under a blocking RMW. The component-level proof of
the #10 fix is the `PublishQueue` unit tests in `test_publish_queue.cpp`.

```bash
UDP_BRIDGE_BENCH_SCENARIOS=1 python3 udp_bridge/test/bench/run_scenario.py \
    --scenario subscriber_death --hold-s 10
```

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
| **K_cotenant_delivery_pct** | 0.50 | Co-tenant management flow (#52): a co-tenant must get ≥ half of what the physical path would give it on its own (i.e. ≥ 0.5 × (1 − loss)). Generous room for queueing while still failing loudly if the bridge takes the link. |
| **K_cotenant_p95_latency_s** | 5.0 s | Co-tenant management flow (#52): p95 one-way latency ceiling per phase. Loud on real starvation (a flow queued-but-delivering can pass the delivery ratio yet be useless for interactive traffic); generous on ordinary queueing. Initial value; refine as field/bench co-tenant traces accumulate. |
| **T_fragment_floor** | 400 | Bulk fragmentation floor (#57): a 480 KB incompressible Image over a 1000-byte `maximum_packet_size` fragments into `floor(480000/1000)=480` pieces at minimum; per-fragment bridge headers cut effective payload per packet, so the real count trends **above** 480, not below (no compression discount on an incompressible payload). 400 sits safely under with margin for partial-window sampling. **Initial value — re-derive from the first host bench run's observed `average_fragment_count` (#57 part 2).** |
| **W_wire_pct** | 0.50 | Bulk wire-rate floor as a fraction of the ~4.8 MB/s nominal Bulk offered load (#57): offered load ~4.8 MB/s (120% of the 4 MB/s WiFi budget), rate limiter trims toward the cap. The invariant measures *offered* (`send.success_bytes_per_second` + `send.dropped_bytes_per_second`), since the limiter is entitled to shed the excess. 0.5 is a deliberately generous start — WiFi is shared with Telemetry/Critical and fragment-header overhead consumes budget. **Initial value — re-derive from the first host bench run's observed Bulk offered rate (#57 part 2).** |

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

1. **N**: after several clean harness recoveries, set to 2× 95th-percentile recovery-edge Recv-Q drain time. *Observation (2026-05-25): across range_degradation and subscriber_death runs the operator Recv-Q stayed at 0 throughout — the drain always kept up — so N=10s is currently very conservative. Keep as-is until a run produces a non-trivial drain edge to measure.*
2. **X**, **T**: import 2026-04-21 bags to SQLite (or query via `rosbag2_py`); extract pre-event vs. post-recovery DataRates.
3. **Y**: after first harness run, set to observed variance of cell-path rate with WiFi idle.
4. **G**: tighten if subsequent deployments show consistently sub-2s worst case.
5. **R**: loosen if false-trips occur on legitimate brief stalls.
6. **T_fragment_floor**, **W_wire_pct** (#57): re-derive both from the first host bench run's observed `average_fragment_count` and Bulk offered rate (`send.success_bytes_per_second` + `send.dropped_bytes_per_second`). That same run must also re-examine **F** (fragment resends now dominate) and re-check **X, R, Y, K, N** — real saturating Bulk exercises them for the first time (they were only ever validated at ~0.5 kB/s single-packet traffic). Note the resend xfail interaction: real fragmentation may flip `test_invariant_resend_amplification` (xfail `strict=True`) to XPASS, i.e. a CI failure; whether the marker comes off is a #54 decision made from the run, not by loosening **F**.

Each refinement lands as its own commit alongside the assertion change that consumes it.

## Continuous integration

`.github/workflows/ci.yml` runs `colcon build` + `colcon test` for this
package on every push and PR to `jazzy` (container `ros:jazzy-ros-core`,
linter-labelled tests excluded pending
[#40](https://github.com/rolker/udp_bridge/issues/40)). The bench tests are
safe under it because the protection is structural:

- The **smoke** test runs under `colcon test` and `@pytest.mark.skipif`-skips
  when `unshare -Urn` is unavailable (e.g. a hardened host or a CI runner
  without unprivileged user namespaces) — it never false-fails.
- **range_degradation** and **subscriber_death** are additionally gated behind
  `UDP_BRIDGE_BENCH_SCENARIOS=1` (they take tens of seconds to minutes), and
  `subscriber_death` also skips without `rmw_zenoh_cpp`.

So the CI job executes the smoke test where namespaces are permitted and skips
it cleanly where they aren't; the opt-in scenarios stay off unless explicitly
enabled, and CI never runs them. If unprivileged userns is disabled on the
runner, set `kernel.apparmor_restrict_unprivileged_userns=0` (see
Prerequisites) to enable the smoke path.
