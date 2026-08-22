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
   with an absolute floor parameter (`admission_floor_bytes_per_second`) sized
   to the control/telemetry traffic that must always flow plus the BridgeInfo
   feedback loop itself. Keep the fraction parameter accepted for one release,
   applying `max(absolute, fraction x cap)` only if the operator set it
   explicitly — see Open Questions.
3. **Rebase the resend budget on delivered throughput.** `resend_packets()`
   currently takes `resend_budget_fraction x effective_rate_limit_`. Change the
   basis to the remote's reported delivery (received minus duplicates), which
   is the only number in the system that describes the link. Keep the existing
   ack-starvation backoff and the one-packet probe guarantee unchanged.
4. **Subtract duplicates from the delivery signal.** `received_bytes_per_second`
   counts resends and duplicates, so it overstates goodput exactly when
   amplification is worst. `duplicate_bytes_per_second` is already reported per
   connection; use `received - duplicate` as the goodput estimate for both the
   congestion test and step 3.
5. **New bench invariant — management-flow survivability.** A low-rate
   co-tenant flow on the veth, sized like an interactive SSH session, asserted
   to keep flowing with bounded added latency through every phase of the
   trajectory including `critical` and both `over_horizon` edges. This is the
   acceptance criterion for relaxing the cap operationally, and it is only
   testable because of PR #27's two-namespace fix.
6. **Un-xfail `test_invariant_resend_amplification`** and remove the
   `xfail(strict=True)` marker added in PR #27. `F_resend_multiplier` is not to
   be loosened; if the invariant cannot pass on its own terms, that is a
   finding, not a threshold to retune.
7. **Update both design notes** to describe measured-throughput scaling and to
   record why configured-cap scaling failed, with the bench numbers.

## Files to Change

| File | Change |
|------|--------|
| `udp_bridge/include/udp_bridge/resend_constants.h` | Rescale AIMD constants; add absolute-floor default; document why cap-relative scaling failed |
| `udp_bridge/src/connection.cpp` | `updateAdmissionControl()` step/floor; `resend_packets()` budget basis; goodput = received − duplicate |
| `udp_bridge/include/udp_bridge/connection.h` | Absolute-floor member + setter; goodput accessor |
| `udp_bridge/src/udp_bridge.cpp` | Declare/read `admission_floor_bytes_per_second`; deprecation path for the fraction parameter |
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

## Open Questions

- **Absolute floor value.** What rate must always be admitted? Suggest sizing
  from the critical-tier topics plus BridgeInfo (order 4–8 kB/s), but the boat's
  real control set should set it.
- **Headroom target.** Should the controller aim for a fraction of measured
  capacity (leaving room for SSH by construction), or only avoid overshooting
  it? The first is what actually prevents lockout; it is a behaviour change
  beyond fixing the scaling, and may belong in a follow-up.
- **Deprecation of `admission_floor_fraction`.** Drop outright (workspace
  preference: remove obsolete, not opt-in) or keep one release?
- **PR split.** One PR with atomic commits, or admission / resend-budget /
  bench as three stacked PRs?
- **Stacking.** This branch is stacked on `feature/issue-18` (PR #27), which is
  unmerged. If #27 merges first, rebase onto `jazzy` before opening the PR.

## Estimated Scope

Multiple PRs likely — admission scaling, resend-budget basis, and the bench
invariant are separately reviewable and separately verifiable. Default to one
PR with atomic commits unless review depth argues for splitting.
