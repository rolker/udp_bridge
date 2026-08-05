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
- **Decrease**: when reported delivery < (1 − `kAdmissionLossThreshold`,
  i.e. 90%) of sent, or when this connection's own feedback is stale
  (nothing received for `kAckStarvationThreshold`), the effective cap is
  multiplied by `kAdmissionDecreaseFactor` (0.5), floored at
  `admission_floor_fraction` (default 0.1) of the configured limit — the
  floor keeps control/telemetry topics and the feedback loop itself
  flowing.
- **Increase**: on clean feedback, recover by
  `kAdmissionAdditiveStepFraction` (0.1) of the configured limit per
  sample, ceilinged at the configured limit (~9 s from floor to full at
  1 Hz BridgeInfo).
- **Idle links** (sent rate 0) cannot be congested and recover normally;
  the `last_receive_time == 0.0` never-received sentinel is *not* stale
  feedback — new connections start at the full cap.

## Interactions

- **Resend budget (#44) bases on the effective cap**: the budget is
  `resend_budget_fraction × effective_rate_limit`, not the configured
  limit — when admission backs off, resends shrink proportionally, else a
  25%-of-configured budget could consume the *entire* reduced admission
  and starve fresh data. With no AIMD activity the two caps are equal, so
  #44 behavior is unchanged (its tests still pass unmodified).
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
- **Lock ordering**: `updateAdmissionControl` reads the sent-stats and
  receive-history mutexes *before* acquiring `config_mutex_`, matching
  `send()`'s sequential (never nested) acquisition order.

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
