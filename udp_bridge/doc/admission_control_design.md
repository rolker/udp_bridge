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
- **Decrease**: when reported delivery < (1 − `kAdmissionLossThreshold`,
  i.e. 90%) of sent, or when this connection's own feedback is stale
  (nothing received for `kAckStarvationThreshold`), the effective cap
  drops to the **lower** of a `kAdmissionDecreaseFactor` (0.5)
  multiplicative step and the **headroom target**
  `(1 − link_headroom_fraction) × goodput` (default headroom 0.2),
  floored at `admission_floor_bytes_per_second` (default 8192) — the
  floor keeps control/telemetry topics and the feedback loop itself
  flowing, and is clamped at use to the configured limit so it can never
  raise a cap.

  The headroom target applies **only** on this branch. Measured goodput
  is bounded above by offered load, so it is a *lower bound* on capacity
  and never an estimate of it; clamping to it on the clean branch too
  would ratchet a healthy, lightly-loaded link down to nothing. Stale
  feedback carries no usable goodput reading — that is what stale means
  — so it falls back to the multiplicative step alone.
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

## Headroom, and what the rate limit is actually for

`maximum_bytes_per_second` was added after a udp_bridge saturating a link
locked an operator out of a remote machine with no other way in. The
measurements above show a ceiling in absolute bytes does not provide that
protection where it counts: the cap was 64× above real capacity at
`critical` and the bridge still saturated the path. A degraded link is a
saturated link, and that is exactly the condition a lockout happens in.

Hence `link_headroom_fraction`: the controller targets a fraction of what
the link is *measured* to deliver, so a co-tenant — an SSH session, the
operator's own management traffic — keeps a share on a 62.5 kB/s path as
well as on a 30 Mbit one. The bench asserts this directly; see the
management-flow survivability invariant in `test/bench/`.

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
  duplicates; our sent rate includes resend traffic; the two sides use
  different smoothing windows; BridgeInfo adds up to ~1 s of propagation
  delay. The 10% loss threshold absorbs this inflation — do not treat
  the ratio as a precise loss percentage.
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

`test/test_admission_control.cpp` pins: decrease-on-congestion, additive
recovery, floor/ceiling clamps, stale-feedback fallback, never-received
sentinel, effective-cap metering in `send()`, and the `setRateLimit`
clamp semantics. The
[#18](https://github.com/rolker/udp_bridge/issues/18) bench harness's
range-degradation scenario is the eventual end-to-end gate. Field
validation: watch `effective_rate_limit` track WiFi loss episodes on the
next BizzyBoat deployment.
