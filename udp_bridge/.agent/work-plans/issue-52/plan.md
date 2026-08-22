# Plan: Resend amplification under real link degradation

## Issue

https://github.com/rolker/udp_bridge/issues/52

## Context

The #18 bench harness, once its impairment layer actually worked (PR #27),
measured `effective_rate_limit` reporting **2291 kB/s on a 62.5 kB/s path**,
resends at **3.1x** the message traffic, and the bridge consuming 61% of a
degraded link with 22% of that being useful payload — while its own telemetry
reported zero dropped and zero failed.

Reading `updateAdmissionControl()` and `resend_packets()` together, the root
cause is narrower than "AIMD is too slow". **Every control parameter is scaled
by `data_rate_limit_`, the operator's configured cap:**

| quantity | today | on the bench's 62.5 kB/s link (cap 4 MB/s) |
|---|---|---|
| additive increase step | `0.1 x data_rate_limit_` | **+400 kB/s per clean sample — 6x the link** |
| admission floor | `0.1 x data_rate_limit_` | 400 kB/s — 6x the link |
| resend budget | `0.25 x effective_rate_limit_` | up to 573 kB/s — 9x the link |

The additive step is the immediate defect: one clean feedback sample adds
400 kB/s, undoing three multiplicative halvings, so the cap oscillated around
2291 kB/s and **never even reached its own 400 kB/s floor** during a 10 s
phase. The floor would have been the next wall had it got there.

The delivery signal itself is sound — the remote echoes
`RemoteConnection.received_bytes_per_second` and the congestion test uses it.
Nothing here needs a wire-protocol change. What needs to change is what the
controller's step sizes and floors are measured *against*.

`maximum_bytes_per_second` is **not** being removed. It stays as a hard ceiling
and as cost control on metered cell links. What goes away is its implicit
second job as the system's model of link capacity.

## Approach

1. **Rescale the additive increase.** Step from a fraction of `data_rate_limit_`
   to a fraction of the *current* `effective_rate_limit_` (with a small
   absolute minimum so recovery from the floor is not asymptotically slow).
   Recovery becomes proportional to where the controller actually is, so a
   clean sample on a collapsed link cannot leap back over the real capacity.
2. **Rescale the floor.** Replace `admission_floor_fraction x data_rate_limit_`
   with an absolute, configurable `admission_floor_bytes_per_second`, default
   **8192** — sized to cover the BridgeInfo/topic_statistics feedback loop
   (measured at ~5.7 kB/s on the bench) plus the critical tier, and still only
   13% of the bench's degraded 62.5 kB/s link. `admission_floor_fraction` is
   **removed outright**, not deprecated (operator decision 2026-08-22; workspace
   preference is to remove obsolete parameters rather than keep an opt-in).
3. **Add a headroom contract.** New configurable `link_headroom_fraction`,
   default **0.2**: on congestion the controller targets
   `(1 - headroom) x goodput` rather than merely halving, so in steady state
   the bridge deliberately leaves a fifth of what the link actually delivers
   for co-tenant traffic. This is the property that prevents lockout, and it
   holds on a 62.5 kB/s path as well as a 30 Mbit one — which an absolute
   ceiling cannot do.

   **Design trap to avoid**: measured goodput is bounded above by offered load,
   so it is a *lower bound* on capacity, never an estimate of it. Clamping the
   effective rate to `(1-headroom) x goodput` unconditionally would ratchet a
   healthy, lightly-loaded link down to nothing. It is therefore applied only
   on the congested branch, as the floor-of-two with the classic halving; the
   clean branch keeps probing upward.

   Resulting update, replacing the body of `updateAdmissionControl()`:

   ```
   goodput = max(0, remote_received_bps - remote_duplicate_bps)
   if congested:                       # incl. stale feedback (no goodput reading)
       target   = (1 - headroom) * goodput
       effective = max(floor, min(effective * 0.5, target))   # target skipped if stale
   else:
       step      = max(step_min, step_fraction * effective)   # relative, not cap-scaled
       effective = min(configured_cap, effective + step)
   ```

4. **Rebase the resend budget on delivered throughput.** `resend_packets()`
   currently takes `resend_budget_fraction x effective_rate_limit_`. Change the
   basis to the remote's reported delivery (received minus duplicates), which
   is the only number in the system that describes the link. Keep the existing
   ack-starvation backoff and the one-packet probe guarantee unchanged.
5. **Subtract duplicates from the delivery signal.** `received_bytes_per_second`
   counts resends and duplicates, so it overstates goodput exactly when
   amplification is worst. `duplicate_bytes_per_second` is already reported per
   connection; use `received - duplicate` as the goodput estimate.
   *(As delivered: goodput drives the DECREASE TARGET and the resend budget
   (step 3/step 4), but NOT the congestion test. Detection deliberately keeps
   the raw received-vs-sent ratio — both sides include resends, so the
   inflation cancels; substituting goodput on the left while `sent` still
   counts our resends would make a healthy link that merely duplicates/reorders
   read as congested and throttle itself for no loss. Round-1 `review-code`
   rebutted the goodput-swap for detection on exactly these grounds; pinned by
   `CongestionDetectionUsesRawReceivedNotGoodput`.)*
6. **New bench invariant — management-flow survivability.** A low-rate
   co-tenant flow on the veth, sized like an interactive SSH session, asserted
   to keep flowing with bounded added latency through every phase of the
   trajectory including `critical` and both `over_horizon` edges. This is the
   acceptance criterion for relaxing the cap operationally, and it is only
   testable because of PR #27's two-namespace fix.
7. **`test_invariant_resend_amplification`.** *(As delivered: the
   `xfail(strict=True)` marker was RETAINED, not removed.)* The plan
   assumed the goodput rescaling would let this invariant pass outright.
   It fixed the dominant cause — resend traffic down 24-29x and the worst
   phase (`critical`) now passes with margin (resend/msg 3.108 → 0.144
   against the 0.200 ceiling) — but two low-loss phases remain just over
   (fringe 0.018 vs 0.010, lossy 0.077 vs 0.060). ~34% of the residual is
   duplicates, which points at spurious re-requests (debounce/reorder
   interaction), a different mechanism from the cap-scaling defect #52
   documents; it is split to #54. `F_resend_multiplier` was NOT loosened
   (the plan's principle holds), and `strict=True` keeps the marker so the
   invariant flips to a failure the moment the residual is closed.
8. **Update both design notes** to describe measured-throughput scaling and to
   record why configured-cap scaling failed, with the bench numbers.

## Files to Change

| File | Change |
|------|--------|
| `udp_bridge/include/udp_bridge/resend_constants.h` | Rescale AIMD constants; add absolute-floor default; document why cap-relative scaling failed |
| `udp_bridge/src/connection.cpp` | `updateAdmissionControl()` step/floor; `resend_packets()` budget basis; goodput = received − duplicate |
| `udp_bridge/include/udp_bridge/connection.h` | Absolute-floor + headroom members and setters; goodput accessor |
| `udp_bridge/src/remote_node.cpp` | Pass the remote's `duplicate_bytes_per_second` alongside `received_bytes_per_second` |
| `udp_bridge/src/udp_bridge.cpp` | Declare/read `admission_floor_bytes_per_second` + `link_headroom_fraction`; remove `admission_floor_fraction` |
| `udp_bridge/test/test_admission_control.cpp` | Convergence-on-real-capacity cases; step no longer overshoots; floor semantics |
| `udp_bridge/test/test_resend_budget.cpp` | Budget follows delivered throughput; starvation backoff and probe unchanged |
| `udp_bridge/test/bench/run_scenario.py` | Co-tenant management flow (launch, trace, teardown) |
| `udp_bridge/test/bench/test_range_degradation.py` | New survivability invariant; remove the #52 xfail |
| `udp_bridge/doc/admission_control_design.md` | Measured-throughput scaling; the failure it replaces |
| `udp_bridge/doc/resend_budget_design.md` | New budget basis; interaction section update |
| `udp_bridge/config/example_params.yaml` | New parameter |

## Principles Self-Check

| Principle | Consideration |
|---|---|
| Test what breaks | The bench invariant is the point of the change, not an afterthought — the operational claim ("cannot lock you out") becomes a repeatable test |
| A change includes its consequences | Both design notes, example params, and the unit tests for both mechanisms land with the code |
| Capture decisions, not just implementations | The design notes record why cap-relative scaling failed, with the measured numbers |
| Enforcement over documentation | `xfail(strict=True)` already enforces; removal is gated on the invariant passing |
| Only what's needed | No wire-protocol change; the delivery signal already exists |
| Improve incrementally | Phased so admission and resend-budget changes are separately reviewable |

## ADR Compliance

| ADR | Triggered | How addressed |
|---|---|---|
| workspace ADR-0018 (local-first CI) | Yes | `ci_local.sh` full-scope attestation before merge |
| udp_bridge has no ADRs (`doc/*_design.md` only) | n/a | Design notes are the equivalent artifact; both updated |

## Consequences

| If we change... | Also update... | Included in plan? |
|---|---|---|
| AIMD scaling | `admission_control_design.md`, `test_admission_control.cpp` | Yes |
| Resend budget basis | `resend_budget_design.md`, `test_resend_budget.cpp` | Yes |
| Parameter surface | `example_params.yaml`, README parameter table | Yes |
| Effective-cap behaviour on boat/operator | Boat + operator configs may want floor values set; lockstep rebuild set | No — follow-up once values are settled on the water |
| #45 (2026-08-04 saturation RCA) | Likely explained by this; close or re-scope after | No — separate issue |

## Documentation & Instruction Impact

- **Stale docs** (must land in this PR): `doc/admission_control_design.md` and
  `doc/resend_budget_design.md` both describe configured-cap scaling as the
  design; `config/example_params.yaml` and the README parameter table gain the
  new floor parameter.
- **Agent-instruction candidates** (proposals only): the general pattern —
  *a controller whose step sizes are scaled by a configured constant cannot
  converge on a physical quantity that constant does not describe* — may be
  worth a line in `.agent/knowledge/`. Also worth recording that an impairment
  harness must prove it impairs before any result from it is trusted.

## Settled (operator decisions, 2026-08-22)

- **Absolute floor**: configurable, `admission_floor_bytes_per_second`,
  reasonable default — **8192 B/s**.
- **Headroom**: in scope for this issue, configurable
  `link_headroom_fraction`, reasonable default — **0.2**.
- **`admission_floor_fraction`**: dropped outright, no deprecation window.
- **Delivery**: one PR with atomic commits.

## Open Questions

- **Stacking.** *(Resolved.)* PR #27 (`feature/issue-18`) merged first, so
  this branch was rebased onto `jazzy` — it is no longer stacked. Its merge
  commit is in this branch's history and `jazzy` is an ancestor of HEAD.
- **Boat/operator config values** — whether the shipped defaults suit the real
  links, or the boat wants explicit floor/headroom values, is a question for
  the water, not for this PR.

## Estimated Scope

One PR with atomic commits (operator decision). Admission scaling,
resend-budget basis, and the bench invariant remain separable commits inside it
if review depth later argues for splitting.
