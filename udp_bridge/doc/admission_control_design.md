# Adaptive Per-Connection Admission Control (issue #43)

Design note for the AIMD effective-rate-limit mechanism in
`Connection::updateAdmissionControl()`. Companion to
`doc/resend_budget_design.md` (issue #44) in format and intent — that
mechanism bounds the *resend category*; this one scales *total admission*
to what the link is actually delivering.

## Problem

Each connection's `maximum_bytes_per_second` is a static cap sized for the
link's nominal capacity. Field links — above all the boat↔shore WiFi RF
bridge — deliver a *fluctuating* fraction of that. The 2026-08-04
Broadkill deployment
([unh_echoboats_project11#411](https://github.com/rolker/unh_echoboats_project11/issues/411))
measured WiFi loss oscillating between 0% and 96% in episodes only weakly
correlated with range (corr 0.38; 96% loss at 284 m vs 7% at 362 m),
while the bridge kept offering the full configured rate — and, through
resend amplification (bounded since #44), *raised* offered load exactly
when delivered capacity dropped. Deployments without a WiFi link
(Massabesic, VPN/Starlink-only) never see the failure mode: the
fluctuating link is the igniter.

## Mechanism

AIMD (additive-increase / multiplicative-decrease) on a per-connection
`effective_rate_limit_` that `send()` meters against instead of the
static cap:

- **Signal — delivered vs sent** (operator decision, 2026-08-05): the
  remote's `BridgeInfo` already echoes
  `RemoteConnection.received_bytes_per_second` per connection — what it
  actually received from us. On every BridgeInfo receipt,
  `RemoteNode::update()` feeds that to `updateAdmissionControl()`, which
  compares it to our own sent rate. **No wire-protocol change.** This
  directly measures forward-path delivery — the chronic 08-04 failure was
  boat→op WiFi at 95% loss *while the return path stayed healthy*
  (17:31 EDT), which an inbound-only signal cannot see.
- **Goodput, not received bytes** (issue #52): the signal is the remote's
  `received_bytes_per_second` **minus** its `duplicate_bytes_per_second`.
  The raw received figure counts resends and duplicates, so it flatters
  the link exactly when amplification is worst.
- **Window-matched comparison** (issue #52, 2026-08-25): "our own sent
  rate" is measured over `kAdmissionSendRateWindowSeconds` (5 s), the
  same window the remote uses to produce the figure it echoes back
  (`Connection::data_receive_rate`). It used to come from
  `PacketSendStatistics::get()`, a variable-span 1–10 s filter, and
  comparing two differently-shaped filters is not a delivery
  measurement. See "The 2026-08-25 collapse" below.
- **Decrease**: when reported delivery < (1 − `kAdmissionLossThreshold`,
  i.e. 90%) of sent, or when this connection's own feedback is stale
  (nothing received for `kAckStarvationThreshold`), the effective cap
  takes one `kAdmissionDecreaseFactor` (0.5) multiplicative step from
  **its own current value**, floored at
  `admission_floor_bytes_per_second` (default 8192) — the floor keeps
  control/telemetry topics and the feedback loop itself flowing, and is
  clamped at use to the configured limit so it can never raise a cap.

  The decrease reads **no goodput term at all**. Until 2026-08-25 it
  targeted the lower of the multiplicative step and a *headroom target*
  `(1 − link_headroom_fraction) × goodput`; that clamp was removed —
  see "`link_headroom_fraction` after the clamp removal" below.
- **Refractory period — one decision per congestion epoch** (issue #52,
  2026-08-25): after a decrease, the controller **freezes** for
  `admission_refractory_period_seconds` (default
  `kDefaultAdmissionRefractoryPeriodSeconds` = 5.0). Inside the window
  no sample is acted on — **not a congested one, and not a clean one
  either**. The window grows by `kAdmissionRefractoryGrowthFactor` (2×)
  on each successive decrease within an *unresolved* episode, capped at
  `kAdmissionRefractoryMaximumMultiple` (2×) the configured base, and
  resets to the base on the first clean sample — which is what "the
  episode resolved" means.
- **Increase**: on clean feedback, recover by
  `kAdmissionAdditiveStepFraction` (0.1) of the **current effective cap**,
  with an absolute minimum of
  `kAdmissionAdditiveStepMinimumBytesPerSecond` (2048) so escaping the
  floor is not asymptotically slow. Ceilinged at the configured limit.
- **Idle links** (sent rate 0) cannot be congested and recover normally;
  the `last_receive_time == 0.0` never-received sentinel is *not* stale
  feedback — new connections start at the full cap.

## Interactions

- **Resend budget (#44) bases on measured goodput** (changed by #52): the
  budget is `resend_budget_fraction × goodput`. It used to be a fraction
  of the effective cap, which inherited every scaling error in that cap —
  see `doc/resend_budget_design.md`.
- **`setRateLimit()` follows or clamps, never resets**: with no
  accumulated backoff (effective at the old limit, including a
  freshly-constructed connection) the effective cap follows the new
  limit, so configuring a larger cap binds immediately. With backoff
  accumulated it clamps to `min(effective, new)` — CONNECT/adopt and
  addRemote re-apply rate limits even when unchanged, and a reset would
  wipe the backoff and burst a congested link at full rate on every
  reconnect flap. A backed-off connection reaches a raised limit via
  additive recovery.
- **What gets shed** within the reduced cap remains FIFO — per-topic
  priority is [#19](https://github.com/rolker/udp_bridge/issues/19);
  per-topic delivery classes are the
  [#36](https://github.com/rolker/udp_bridge/issues/36) umbrella.

## Why the original scaling failed (issue #52)

Every quantity above was originally a fraction of `data_rate_limit_` —
the configured `maximum_bytes_per_second`. That is a declaration of
intent, not a measurement, and under range degradation it is the least
reliable number in the system. The 2026-08-22 bench run (the #18 harness,
once [PR #27](https://github.com/rolker/udp_bridge/pull/27) made its
`tc netem` impairment actually apply) measured, per trajectory phase, on
a connection configured at 4 MB/s:

| phase | real link | effective cap | msg | resend | resend/msg |
|---|---|---|---|---|---|
| in_range_clean | 3750 kB/s | 3346 | 7.7 | 0.3 | 0.04 |
| lossy | 375 | 2480 | 8.5 | 13.9 | 1.64 |
| critical | 62.5 | **2291** | 8.5 | 26.4 | **3.11** |

The controller never converged. Its additive step was
`0.1 × 4 MB/s = 400 kB/s` — six times the entire `critical` link — so one
clean feedback sample undid three halvings and the cap oscillated around
2291 kB/s, never even reaching its own 400 kB/s floor. The bridge
retransmitted three times more than it ever originally sent, took 61% of
a 62.5 kB/s path with 22% of that being useful payload, and reported
`dropped = 0` and `failed = 0` throughout.

The lesson generalises beyond this file: **a controller whose step sizes
are scaled by a configured constant cannot converge on a physical
quantity that constant does not describe.**

## The 2026-08-25 collapse, and the refractory period (issue #52)

The scaling fix above landed, and the same control loop failed again on
the very next deployment — differently, and worse. On the Isles of Shoals
transit and station work
([unh_echoboats_project11#459](https://github.com/rolker/unh_echoboats_project11/issues/459))
the `vpn` connection's cap collapsed to its 8192 B/s floor **35 times in
9.5 hours**. Every episode bottomed at exactly the floor and every one
lasted 102–110 s — a constancy that identifies the outage length as a
property of *this controller*, not of the link. 1.95 GB of video and
telemetry was discarded by the boat at its own gate before reaching the
radio, while Starlink reported `pop_ping_drop_rate` 0.0–0.05, no
obstruction and flat 17–24 ms latency throughout. Whenever the cap was
left at maximum — 80% of the transit, 90% on station — median and p95
drops were zero. The trigger was ~1.11% loss. The response was a 183×
cap reduction, and the operator hand-drove the boat alongside with no
RGB video.

Three compounding defects, and what each got:

1. **No rate limit on the controller itself.** `updateAdmissionControl`
   runs on every BridgeInfo arrival (~2 s) and applied another halving
   on every congested sample. One marginal reading (442300/513563 = 0.86
   against the 0.9 threshold) became seven halvings in 12 s. → the
   **refractory period**.

   The freeze is deliberately unconditional on sample type. Letting a
   clean sample recover inside the window re-opens the loop from the
   other side — recover, re-cross the threshold on the next
   differently-filtered sample, halve again — and it changes the outcome
   of both field-replay regression tests.

   The window *grows* because the recorded onset reads congested on the
   raw ratio for its full ~14 s: each decrease shrinks our own send
   rate, which widens the filter skew that triggered the detector in the
   first place. A fixed window still permits a third and fourth halving
   inside one episode. The growth mirrors the exponential backoff the
   resend re-request path already uses (`kResendBackoffBase` /
   `kResendBackoffCap`) rather than introducing a second idiom.

2. **The detector compared two differently-smoothed rates.** Our own
   rate came from a variable-span 1–10 s box filter; the remote's came
   from its 5 s box filter, delivered one BridgeInfo period later.
   16.1% of the field samples show `remote_rx > 1.05 × sent_on_wire` —
   physically impossible if the two figures described the same interval,
   and proof that the ratio was dominated by filter skew during any
   transient rather than by loss. → the **window-matched comparison**
   (`PacketSendStatistics::success_rate_in_window`), which narrows the
   skew but does not eliminate it: propagation delay remains, which is
   why the refractory period is the primary defence and not the
   detector fix.

3. **The decrease target was derived from post-gate goodput.** →
   the clamp removal, next section.

### Tuning constants, and what constrained each

| constant | value | what fixed it there |
|---|---|---|
| `kDefaultAdmissionRefractoryPeriodSeconds` | 5.0 s | The length of the receive-rate measurement window both ends use. One full window is the shortest interval after which *both* filters describe traffic sent at the new cap rather than the old one; reacting sooner is reacting to our own previous decrease. Empirically it holds the recorded onset to two decreases (`lowest = 375000` against the test's `> 187500` bound). |
| `kAdmissionRefractoryGrowthFactor` | 2.0 | Matches the resend backoff idiom. One growth step is what the recorded onset needs; without it a third and fourth halving land inside the same episode (`lowest = 187500`, exactly at the bound). |
| `kAdmissionRefractoryMaximumMultiple` | 2.0 (→ 10 s at the default base) | Bounded above by the bench harness's 10 s phase holds — a window grown past a phase could not react inside it — and by the statistics deque's own 10 s retention: freezing longer than the whole measurement record means deciding on evidence the controller can no longer see. Bounded below by `SustainedRealLossMustConverge`, which fails if convergence on a genuine sustained drop takes longer than 40 s. |
| `kAdmissionSendRateWindowSeconds` | 5.0 s | Not a tuning value: it is `Connection::data_receive_rate`'s window, which is the number it has to match. |

The refractory base is per-connection configurable
(`admission_refractory_period_seconds`, a **double** —
`5.0`, not `5`). The growth factor and cap stay fixed constants, matching
the precedent that `kResendBackoffBase` is fixed while higher-level knobs
like `resend_budget_fraction` are parameters. Like every other
admission parameter it is **configure-time only** — which is the exact
property RCA item D names as what blocked live mitigation on 2026-08-25
(the floor could not be raised from the boat while it was the problem).
[#76](https://github.com/rolker/udp_bridge/issues/76) is in flight to
make connection parameters runtime-reconfigurable; this knob joins that
set and should be revisited with it.

## `link_headroom_fraction` after the clamp removal (issue #52)

The decrease used to target
`min(cap × 0.5, (1 − link_headroom_fraction) × goodput)`. That clamp was
**removed** on 2026-08-25, which leaves `link_headroom_fraction` with no
call site in the control law.

**Why it had to go.** `goodput` is depressed by the very throttling the
formula is computing — a positive feedback loop. On the recorded field
onset the clamp's first step alone landed at
`0.8 × (254615 − 51715) = 162320`, already below the regression bound,
*before a second decrease existed for the refractory gate to suppress*;
no refractory tuning rescues it. Nor is goodput a capacity estimate in
the regime where the clamp bites hardest: as the "Decrease" bullet's own
reasoning says, goodput is bounded above by *offered* load, and the field
day's operating point was ~495 kB/s offered against a 1,500,000 B/s cap
— so the clamp there was a 3.8× cut carrying no capacity information at
all. When the gate *is* limiting, goodput tracks the cap and the clamp is
circular. There is no regime in which it is trustworthy.

**Where that leaves the parameter.** The plan for this cycle required
resolving this by *measurement* — not by inspection — against the bench
invariant that is the parameter's operational claim,
`test_invariant_cotenant_management_flow_survives` (issue #52's own
acceptance criterion: "this invariant should pass before the cap is
relaxed on anything that goes to sea"). It **passes** with the clamp
removed, on every phase the path can carry traffic in. The
refractory-gated multiplicative decrease alone satisfies the co-tenant
guarantee on this harness.

So the parameter is **retained but inert**: existing configs set it, and
removing it would break them, so it is still declared, clamped and
reported — but nothing reads it. That is a state worth flagging rather
than leaving to be rediscovered:
`AdmissionControl.HeadroomFractionDoesNotAffectDecreaseTarget` pins the
inertness, so re-attaching headroom to the control law has to be a
deliberate act with its own basis (one that is *not* post-gate goodput)
and its own re-run of the field-replay gate. **Follow-up:** decide
whether headroom gets a new basis or the parameter is removed outright,
after post-deployment field data exists — the same follow-up that should
carry RCA option B (a sequence-gap / matched-byte-counter detector, which
needs new wire fields and so a coordinated redeploy) and the
evaluated-but-not-adopted purely-local resend-ratio detector.

## Headroom, and what the rate limit is actually for

`maximum_bytes_per_second` was added after a udp_bridge saturating a link
locked an operator out of a remote machine with no other way in. The
measurements above show a ceiling in absolute bytes does not provide that
protection where it counts: the cap was 64× above real capacity at
`critical` and the bridge still saturated the path. A degraded link is a
saturated link, and that is exactly the condition a lockout happens in.

Hence `link_headroom_fraction`, which used to make the controller target
a fraction of what the link is *measured* to deliver, so a co-tenant — an
SSH session, the operator's own management traffic — keeps a share on a
62.5 kB/s path as well as on a 30 Mbit one. **That clamp was removed on
2026-08-25** (see the section above); what now provides the co-tenant
guarantee is the refractory-gated multiplicative decrease itself, and
the bench's management-flow survivability invariant in `test/bench/` —
the thing that actually asserts the property — passes on it.

The rate limit is **not** obsolete. It remains a hard ceiling and the
cost control on metered cell links. What it is no longer asked to be is
the system's model of link capacity.

## Scope boundaries and signal caveats (stated honestly)

- **Single-connection total blackout is out of scope.**
  `updateAdmissionControl` runs on BridgeInfo receipt, and receipt
  refreshes `last_receive_time` on the delivering connection — so the
  stale-feedback fallback can only fire for connections *other than* the
  one that delivered the BridgeInfo. A remote whose every connection is
  in inbound blackout produces no calls at all and no AIMD decrease;
  that regime is the #45 RCA territory, and the #44 resend budget's own
  ack-starvation backoff still bounds the storm there. The primary field
  scenario — congestion with feedback still flowing — is fully covered.
- **The ratio is a trend signal, not an exact loss measure.** The
  remote's `received_bytes_per_second` includes cross-connection
  duplicates; our sent rate includes resend traffic; BridgeInfo adds up
  to ~1 s of propagation delay. The two sides' smoothing windows now
  match (5 s on both, since #52) — they did not, and the mismatch was
  the 2026-08-25 detector defect — but matching the windows does not
  make them the *same* interval, so skew during a transient is narrowed,
  not eliminated. The 10% loss threshold absorbs the residual inflation
  — do not treat the ratio as a precise loss percentage, and note that
  the refractory period, not the threshold, is what bounds the damage
  when the ratio is wrong.
- **Locking**: `updateAdmissionControl` reads the sent-stats and
  receive-history values under their own mutexes, then takes
  `config_mutex_` for the AIMD update — the mutexes are never held
  simultaneously (as everywhere else in `Connection`), so no
  lock-ordering constraint exists or is created.

## Telemetry

`RemoteConnection.msg` gains `effective_rate_limit`, populated in
`sendBridgeInfo()` (and the ListRemotes service) alongside
`maximum_bytes_per_second` — operators watch backoff events live in
`bridge_info` and retroactively in bags. Appending the field changes the
ROS 2 type hash: coordinated redeploy of both bridge ends, per the
schema-evolution contract documented in `Remote.msg`.

## Verification

`test/test_admission_control.cpp` pins: decrease-on-congestion (purely
multiplicative, no goodput term), additive recovery, floor/ceiling
clamps, stale-feedback fallback, never-received sentinel, effective-cap
metering in `send()`, the `setRateLimit` clamp semantics, the refractory
gate (freeze of both branches, exponential growth, reset on a clean
sample, setter contract) and the window-matched comparison.

`test/test_admission_field_replay.cpp` is the acceptance gate for the
2026-08-25 fix, and it is deliberately **two-sided**:

- `FieldOnsetMustNotCascadeToFloor` replays the recorded 07:20 sender /
  receiver telemetry. The cap must not run to the floor: `lowest =
  375000` against a `> 187500` bound (pre-fix: 8192).
- `HandoverBlipMustNotCostTwoMinutesOfVideo` models the day's operating
  point through a single ~4 s Starlink handover. Delivered fraction
  `1.000` against a `> 0.90` bound, final cap back at 1500000 (pre-fix:
  0.398 and 633091).
- `SustainedRealLossMustConverge` is the counter-test that keeps the
  other two honest. Both of them assert only that the cap does *not*
  fall; a controller that has stopped detecting congestion altogether
  passes both. (The `review-plan` pass on this cycle demonstrated
  exactly that: EWMA smoothing at α = 0.2 leaves the cap pinned at
  1500000 for the whole recorded onset, both tests green.) This one
  replays a *sustained* 10× capacity drop and requires the cap to find
  it within 40 s and then stay inside [0.5×, 2×] real capacity. It is
  also what bounds how far `kAdmissionRefractoryMaximumMultiple` may
  grow.

`test/test_message_atomicity.cpp` covers the send-path half of #52 — see
the file header.

The [#18](https://github.com/rolker/udp_bridge/issues/18) bench harness's
range-degradation scenario is the end-to-end gate and must be re-run for
any change to this control loop:

```bash
UDP_BRIDGE_BENCH_SCENARIOS=1 colcon test --packages-select udp_bridge \
    --pytest-args -k test_bench_range_degradation
```

Field validation: watch `effective_rate_limit` track loss episodes on
the next BizzyBoat deployment — specifically, that the 102–110 s
floor-to-recovery excursions do not recur.
