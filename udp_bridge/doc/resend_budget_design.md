# Sender-Side Resend Budget (issue #44)

Design note for the resend bandwidth cap and ack-starvation backoff in
`Connection::resend_packets()`. Companion to `doc/qos_design.md` in format
and intent: this records the decisions, not just the code.

## Problem

The resend machinery had no ceiling on the fraction of a connection's
bandwidth that retries may consume. Because loss triggers resends, a
degrading link *increases* offered load — a positive feedback loop:

- **2026-05-01** ([unh_echoboats_project11#124](https://github.com/rolker/unh_echoboats_project11/issues/124)):
  resend attempts reached 13 MB/s against a 1.0 MB/s VPN link; a 9-minute
  multi-topic outage ("all red") followed.
- **2026-08-04** ([unh_echoboats_project11#411](https://github.com/rolker/unh_echoboats_project11/issues/411)):
  after the boat's inbound path collapsed (~25 kB/s → ~6 kB/s on all
  connections, mechanism tracked in #45), ack starvation made the sender
  treat everything in flight as lost. Resend attempts on the VPN connection
  hit ~1.5 MB/s dropped plus ~220 kB/s sent against a 1.2 MB/s cap;
  reception lag rose from 5–30 ms to 0.3–1 s (84 s on the stats stream)
  and was cleared only by restarting the stack.

The give-up TTL (`kSentPacketTTL`, see `resend_constants.h`) bounds how
long an individual packet is retried, but nothing bounded the *aggregate
rate* of retry traffic — the storm sustains itself until restart.

## Mechanism

Two coupled bounds, both enforced in `Connection::resend_packets()`:

1. **Budget fraction.** Resend-category traffic may consume at most
   `resend_budget_fraction` (default `kDefaultResendBudgetFraction` = 0.25)
   of **measured goodput**, measured over the same
   strict 1-second window the rate limiter's `can_send` uses
   (`PacketSendStatistics::bytes_in_window`). Goodput is the remote's
   reported `received_bytes_per_second` minus its
   `duplicate_bytes_per_second`, from the most recent
   `updateAdmissionControl()` sample.

   The basis was the configured `maximum_bytes_per_second` under #44 and
   the AIMD `effective_rate_limit` under #43. **Issue #52 moved it to
   goodput** because both of those inherited the same flaw: they are
   scaled by a number the operator declared, not one the link
   demonstrated. The 2026-08-22 bench measured an effective cap of
   2291 kB/s on a 62.5 kB/s path, so a 25% budget authorized 573 kB/s of
   retransmission — nine times the whole link. The budget behaved exactly
   as specified and bounded nothing. Measured resend traffic reached 3.1×
   the message traffic; see `doc/admission_control_design.md` for the
   per-phase table. Sizing retransmission against what the link is
   actually delivering is the only basis that cannot be wrong by orders
   of magnitude.

   Over-budget resends are
   recorded as `SendResult::dropped` in the `resend` category, so shedding
   is visible in the existing per-connection `BridgeInfo` DataRates — the
   same fields the field analyses read.

2. **Ack-starvation backoff.** While the connection receives nothing
   (no inbound packets ⇒ no acks ⇒ retrying harder cannot help), the
   budget halves for each `kAckStarvationThreshold` (1 s) elapsed since
   `last_receive_time()`, clamped at `kMaxAckStarvationBackoffShift` (4)
   halvings — a floor of 1/16 of the fraction (~1.6% of goodput at
   the default). The floor deliberately keeps a **probe trickle** flowing
   so recovery begins the moment the inbound path returns, without waiting
   for a timer. On links whose floored budget is smaller than a single
   packet (the default 50 kB/s class floors at 781 bytes), a byte
   comparison alone would shed everything, so when nothing has been resent
   in the current window the first packet is admitted regardless of size —
   the trickle is bounded at roughly one packet per window, never zero.
   The probe is gated on the **fraction**, not on the computed budget
   (#52). A zero budget now also arises from a zero goodput basis — a
   brand-new connection before its first BridgeInfo sample, or a link
   currently delivering nothing — and both are exactly the cases the
   trickle exists for. Exception: a fraction of 0.0 is the operator's "no resends on this
   connection" switch and never probes.

   `last_receive_time() == 0.0` is the "no packet received yet" sentinel:
   a brand-new connection has no starvation history and gets the full
   budget (not the max-backoff floor).

## Decisions and rationale

- **Why a 1-second window, not the smoothed stats rate.** The 0–10 s
  smoothed `PacketSendStatistics::get()` rate divides by the deque's time
  span; during a sustained burst the divisor grows and the reported rate
  lags reality, under-shedding in exactly the sustained-storm mode this
  budget exists to stop (plan-review must-fix). `bytes_in_window` mirrors
  `can_send`'s window and full-deque scan (the deque is not monotone in
  timestamps under the reserve-then-record send pattern).

- **Fraction of the cap, not an absolute rate.** Connections already carry
  a per-link `maximum_bytes_per_second`; a fraction inherits per-link
  sizing for free and keeps one intuitive knob.

- **Default 0.25.** Keeps ≥75% of a saturated link for fresh data while
  allowing meaningful recovery on lossy-but-alive links. On the 2026-08-04
  VPN figures, the storm's ~1.7 MB/s of resend attempts would have been
  bounded at 300 kB/s (0.25 × 1.2 MB/s), and under ack starvation decayed
  to ~19 kB/s within 4 s.

- **Two-part accounting.** The window seed counts only resend bytes
  actually *sent* in the last second (success or socket-failed —
  `bytes_in_window` excludes budget-dropped entries, which consumed no
  link capacity; counting them would let one shed batch block the budget
  for the rest of the window). Within a single `resend_packets` call,
  every packet handed to `send()` is then charged locally regardless of
  its outcome — conservative, O(packets), and immune to double-querying
  races.

- **Scope honesty.** This bounds the *resend category only*. The shared
  `can_send` cap still meters all categories together, so the budget
  guarantees *headroom* for fresh data rather than strictly prioritizing
  it; per-class scheduling of what fills the link is
  [#19](https://github.com/rolker/udp_bridge/issues/19), and richer
  per-topic delivery classes are the
  [#36](https://github.com/rolker/udp_bridge/issues/36) umbrella. This
  budget lands first as the minimal bound (operator decision, 2026-08-05).

- **Clock domain.** `update_last_receive_time` and the `now` passed into
  `resend_packets` both derive from the node's clock, so the starvation
  subtraction is valid. A steady-clock migration would need to touch both
  together.

- **Parameter wiring.** `resend_budget_fraction` is read per connection in
  `on_configure` and applied to the live `Connection` after
  `RemoteNode::update()` creates it. The message/service paths that also
  set rate limits (CONNECT/adopt, `AddRemote`) carry no fraction; new
  connections take the default from the field initializer. Extending
  `AddRemote.srv` is deferred until a caller needs a non-default fraction
  on a service-created connection.

## Interaction with existing resend constants

The receiver-side re-request machinery (`kResendDebounceHold`,
`kResendBackoffBase`/`Cap`, give-up at `kSentPacketTTL`) is unchanged. A
receiver may re-request packets the sender's budget sheds; those requests
ride the existing exponential re-request backoff and eventually give up at
the TTL, bumping the per-remote give-up counter — silent loss stays
operationally visible. The budget changes *whether the sender complies*,
not how the receiver asks.

## Verification

`test/test_resend_budget.cpp` pins the four contract points inline
(bounded under load; exactly-one backoff step at 1.5× threshold;
never-received sentinel gets full budget; max-backoff clamp keeps the
probe trickle). The [#18](https://github.com/rolker/udp_bridge/issues/18)
bench harness's ack-starvation scenario is the eventual end-to-end gate;
these tests carry the invariant until it exists.
