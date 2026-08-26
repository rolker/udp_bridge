# Plan: Resend amplification under real link degradation

> **Revision note (2026-08-25):** this plan was revised in place after
> `review-plan` returned `changes-requested` (full entry: `## Plan Review`
> in `progress.md`). The reviewer built an empirical model of the control
> law, validated it against the committed replay tests' exact recorded
> numbers, and ran this plan's original A1/A2/A3 design through it. The
> headline result: A2's gated goodput clamp cannot pass either committed
> gate test at any refractory tuning. Sections below marked "revised" or
> citing a finding ID (F1-F10) reflect that pass. See the individual
> sections for what changed and why; git history preserves the original
> text.

## Issue

https://github.com/rolker/udp_bridge/issues/52

## Context

This is a new cycle on issue #52. The prior cycle (merged as `6dda032`, PR
#56) rebased the admission floor and resend budget onto measured goodput
instead of the configured cap — that fix is done and is not being revisited.

A 2026-08-25 Appledore field RCA (posted as an issue comment) found a
**second, deeper defect** in the same control loop, reproduced independently
of the cap-scaling issue: the AIMD admission controller itself is unstable.
On the Isles of Shoals deployment it collapsed to its 8192 B/s floor 35 times
in 9.5 hours, every episode bottoming out in 102–110 s, discarding 1.95 GB of
video/telemetry — while the underlying Starlink link reported 0.0–0.05 packet
loss and flat 17–24 ms latency throughout. The trigger was ~1.1% loss; the
response was a 183x cap reduction.

The RCA identified three compounding defects in
`Connection::updateAdmissionControl` (`src/connection.cpp:160-300`):

1. **No refractory period.** The method runs on every `BridgeInfo` arrival
   (~every 2 s) and applies `x kAdmissionDecreaseFactor` (0.5) again on
   every congested sample, with nothing rate-limiting how often a decrease
   can fire. One marginal reading becomes six halvings in 12 s.
2. **The congestion detector compares two differently-smoothed rate
   estimates.** `sent_bps` (our own smoothed on-wire rate) is compared
   instantaneously against `remote_received_bps` (the remote's smoothed
   receive rate, delivered one `BridgeInfo` period later). 16.1% of samples
   in the field data show `remote_rx > 1.05 x sent_on_wire` — physically
   impossible if the two figures described the same interval — proving the
   ratio is dominated by filter/skew mismatch during any transient, not by
   loss.
3. **The decrease target is derived from post-gate goodput.** `decreased =
   min(cap x 0.5, (1 - headroom) x goodput)` at `src/connection.cpp:284-286`
   uses `goodput`, which is *itself* depressed by the throttling this
   formula is computing — a positive feedback loop. In the field trace the
   first decrease step landed at `0.8 x goodput ≈ 282 kB/s` against ~494 kB/s
   offered, guaranteeing the next sample would also read congested.

A follow-up comment on the issue records the 2026-08-25 operator-approved
scope split for this cycle:

- **A — stop the AIMD collapse** (the three defects above). **In scope.**
- **B — priority classes** (critical telemetry not gated behind video
  traffic). A materially different mechanism (intra-connection topic
  scheduling), already tracked as #19 with an in-flight branch (PR #76).
  **Split out, not in scope here.**
- **C — restore message-level drop granularity.** A partially-admitted
  multi-fragment message currently burns uplink on fragments that can never
  be reassembled. **In scope.**

Two regression tests already committed to this branch
(`test/test_admission_field_replay.cpp`, commit `4002cad`) are the
acceptance gate for direction A:

- `FieldOnsetMustNotCascadeToFloor` — replays the recorded 07:20 onset.
  Requires the effective cap never drop below `kFieldRateLimit / 8`
  (the test comment explicitly tolerates up to two decreases — "allow
  two for slack" — but not the field's observed cascade to the floor).
- `HandoverBlipMustNotCostTwoMinutesOfVideo` — a closed-loop model of a
  single ~4 s Starlink handover blip (60% loss for 2 samples inside 1%
  background loss). Requires >90% of offered traffic delivered over the
  following two minutes, and the cap usable again (`> kFieldRateLimit/2`)
  by the end of the window.

Both currently fail against the code on this branch (verified by reading
the merged post-#56 `updateAdmissionControl`, which still applies the
per-sample decrease with no refractory gate and the unconditional goodput
clamp).

## Approach

### A — stop the AIMD collapse (`src/connection.cpp`, `include/udp_bridge/connection.h`, `include/udp_bridge/resend_constants.h`)

**A1. Refractory period — one decrease per congestion epoch.**

Add `last_admission_decrease_time_` (seconds, under `config_mutex_`,
alongside the other AIMD state) and a per-connection
`admission_refractory_period_seconds_` parameter (default
`kDefaultAdmissionRefractoryPeriodSeconds`, new constant in
`resend_constants.h`). While the refractory window is active
(`now - last_admission_decrease_time_ < refractory`), **no sample is acted
on, congested or clean** — the controller holds its last
`effective_rate_limit_` exactly as-is, regardless of what the arriving
sample would otherwise have triggered. (Clarified 2026-08-25 per
`review-plan` finding F9 — the earlier wording said "a congested sample is
not acted on," which left open whether a *clean* sample arriving inside
the window still gets additive recovery; it does not. The freeze is
unconditional on sample type. This is the reading the F1 model results
above were validated against.) Once the window elapses, the next arriving
sample — congested or clean — is evaluated normally.

The field data shows the raw ratio stays "congested" for the full ~14 s of
the recorded cascade (the sender's own recorded rate keeps falling in that
window, which is itself an artifact of the pre-fix controller that produced
this trace), so a single fixed short window is not guaranteed to bound the
cascade to the test's "at most two decreases" tolerance. Grow the
refractory window on each successive decrease within the same unresolved
episode (double it, capped), mirroring the existing exponential-backoff
idiom already used for resend re-requests (`kResendBackoffBase` /
`kResendBackoffCap` in `resend_constants.h`) rather than inventing a new
pattern. Reset the window to its base value the first time a clean
(non-congested) sample is observed after a decrease — that is what "the
episode resolved" means.

**The base period, growth factor, and cap are tuning constants, not
something to hand-derive on paper — but the oracle they're tuned against
must be two-sided (revised per `review-plan` finding F2).** Implementation
must build and run `test_admission_field_replay.cpp`'s two committed tests
**and** the new `SustainedRealLossMustConverge` counter-test (see Tests
below) iteratively. The counter-test exists precisely because a
tuning loop with only the two committed tests as its oracle can converge
on values that satisfy them by making the controller stop reacting to
congestion at all — `review-plan`'s model showed A3's EWMA at alpha=0.2
does exactly this: the cap never moves (`1500000`, both tests PASS) on a
controller that has stopped working. Default starting point for the base
period: the RCA's own "≥ 2 BridgeInfo intervals." Record the final values
and the reasoning (which constraint from which test drove which constant)
in `doc/admission_control_design.md`, the way `resend_constants.h`'s
existing comments cite the field episodes that produced their values.

**Also cross-check the chosen values against the bench harness's 10 s
phase holds**, not only the field-replay tests' ~2 s BridgeInfo-period
timescales (see the new Verification subsection below) — a refractory
window that grows past a phase's hold length cannot react within that
phase, which the field-replay tests alone cannot detect.

**Relationship to configure-time-only parameters (#76).** Per the RCA,
the admission floor's inability to be raised without a redeploy was the
lever that would have mitigated the 2026-08-25 episode live, and issue
#76 is already in flight to make connection parameters
runtime-reconfigurable. `admission_refractory_period_seconds` is planned
here as configure-time-only, matching the existing admission-parameter
convention (`admission_floor_bytes_per_second`,
`link_headroom_fraction`) — consistent, but it adds a new tunable to the
same "not reachable from the boat while it's the problem" set #76 exists
to close. Not a blocker for this plan; note it so it isn't rediscovered
as a surprise, and revisit once #76 lands.

**A2. Decrease target: drop the post-gate goodput clamp — don't gate it (revised
2026-08-25 per `review-plan` finding F1).**

The original draft of A2 gated the `(1 - headroom) x goodput` clamp on
`sent.dropped_bytes_per_second == 0` — the theory being the goodput
reading is only untrustworthy while we are actively self-limiting.
`review-plan`'s empirical model (built against the committed replay
tests, validated by reproducing their exact recorded numbers, `lowest =
8192`, `delivered_fraction = 0.398`, final cap `633091`) showed this
cannot pass either gate test **at any refractory tuning**:

| variant | onset `lowest` (need > 187500) | handover delivered (need > 0.90) |
|---|---|---|
| A1+A2 (gated clamp), refractory base 2 | 81160 FAIL | 0.880 FAIL |
| A1+A2 (gated clamp), refractory base 3 | 162320 FAIL | 0.880 FAIL |
| A1 + clamp DROPPED, refractory base 2 | 375000 PASS | 1.000 PASS |
| A1 + clamp DROPPED, refractory base 3 | 750000 PASS | 1.000 PASS |

The mechanism: the *first* congested sample of any episode is, by
construction, a non-self-limiting interval — nothing has decreased yet —
so the gate condition (`dropped_bytes_per_second == 0`) is true exactly
there, and the clamp fires precisely where the review found it does the
damage. On the recorded onset replay that single first step already lands
below the `kFieldRateLimit / 8` bound before a second decrease exists for
A1's refractory gate to suppress; no refractory value rescues it. The
clamp is also arguably measuring the wrong thing even when correctly
gated: `connection.cpp:277-281`'s own comment states goodput is bounded
above by *offered* load, not capacity — when the gate is not limiting,
`0.8 x goodput` reports what we offered (the field day's operating point
was ~495 kB/s offered against a 1,500,000 B/s cap, so the clamp there is
a 3.8x cut carrying no capacity information), and when the gate *is*
limiting, goodput tracks the cap and the clamp is circular. There is no
regime in which this clamp is trustworthy.

**Design (RCA option C, first half — adopted as-is): drop the clamp
entirely. The decrease is purely multiplicative from the controller's
own last known-good state, with no goodput term:**

```
effective_rate_limit_ = max(floor, effective_rate_limit_ * kAdmissionDecreaseFactor)
```

applied on every congested sample this branch is reached for (rate-limited
by A1's refractory gate). Simpler than the gated design and passes both
tests with margin per the table above.

**`link_headroom_fraction_` loses its only current call site** — it is
applied exclusively inside the clamp being dropped
(`connection.cpp:284`). It is documented (`README.md:211-212`) as the
mechanism behind issue #52's own acceptance criterion 2 and the
already-committed `test_invariant_cotenant_management_flow_survives`
bench invariant — a co-tenant flow must keep flowing under degradation —
so it cannot simply disappear silently. `review-plan`'s model covered
only the two field-replay unit tests, not the bench harness, so this
plan does not yet have a model-validated replacement basis for the
parameter. Implementation must resolve one of the following, and record
the outcome (not leave it implicit) in `doc/admission_control_design.md`:
(a) re-apply headroom against a basis that is not post-gate goodput —
e.g. as a multiplier on `floor` or on the additive-recovery target, or
(b) confirm via the bench re-run (see the new Verification subsection
below) that the refractory-gated multiplicative decrease alone already
satisfies the co-tenant invariant, in which case `link_headroom_fraction`
is redundant and should be flagged as a follow-up for removal rather than
kept as an inert, still-documented parameter. Either outcome must be
settled before the PR is considered done — the bench run is the gate, not
inspection of the field-replay tests alone.

**A3. Detector: compare like-windowed rates instead of raw instantaneous
ones (revised 2026-08-25 per `review-plan` findings F7/F8).**

**RCA option B is deferred — for the right reason this time.** The prior
draft's justification ("`updateAdmissionControl`'s signature is fixed by
the already-committed regression tests") is not a real design constraint
— the tests are three commits old, unmerged, and changeable, and nothing
about them touches a wire schema; citing them as the blocker leaves a
false obstacle for whoever picks this up. The actual, sufficient reason:
sequence-gap or matched-byte-counter loss detection over a matched
interval needs counters that do not exist on the wire —
`RemoteConnection.msg` carries only `float32` rates and
`BridgeInfo.next_packet_number` is node-level, not per-connection — so
adding them is a type-hash bump requiring coordinated redeploy of both
bridge ends, the same consequence already documented for
`RemoteConnection.msg`'s `effective_rate_limit` field. That is out of
scope for this cycle — record it as a follow-up (see Consequences), not
a silent gap.

**Evaluate-and-reject: a purely local resend-ratio detector (new, per
`review-plan` finding F7).** The RCA's own 1.11% trigger was computed
without any wire change at all — `resend on wire (5171) / total on wire
(467577)`, both terms from `sent_packet_statistics_` over the same
window with the same filter, which removes the sender/receiver
filter-skew defect (defect 2) outright rather than dampening it.
`data_sent_rate(now, PacketSendCategory::resend)` already exposes the
numerator. This option was absent from the prior draft entirely and
needed an explicit evaluation, not silence. It has real weaknesses:
roughly one RTT of lag before a resend registers, it confounds with
resend *amplification* itself (a controller reacting to a
resend-fraction signal is reacting to a quantity its own AIMD behavior
partly produces — a different flavor of the same feedback-loop risk A2
was fixing), and it is blind exactly when the return path degrades
independently of the forward path, which is what `feedback_stale`
already exists to cover. **Per the operator's 2026-08-25 decision, this
cycle does not widen scope to adopt it.** File a follow-up issue
evaluating it against A3's window-matching fix below once this PR lands
and post-deployment field data exists to compare the two against —
don't decide between them on paper.

**What's adopted this cycle: compare the sender and receiver rates over
the same window, instead of smoothing one side with a differently-shaped
filter.** The prior draft's framing — EWMA "using a time constant closer
to what produces `sent_bps`'s own smoothing" — turned out not to be
well-defined: `sent_bps` comes from a variable-span 1–10 s box filter
(`statistics.cpp:115-155`, span depends on deque occupancy), and no EWMA
alpha "matches" a variable-span box filter. That's part of why F2's
tuning loop was unbounded in the first place — there was no principled
way to pick alpha, so the search could drift to values (0.2) that just
stop detecting congestion. The direct fix is to compare like with like:
compute the sender-side rate over the *same* 5 s window
`Connection::data_receive_rate` already uses on the receive side.
`PacketSendStatistics::bytes_in_window` is already exactly this shape —
the resend budget uses it for the same reason (the smoothed `get()` rate
lags in precisely the transient that matters). This narrows — doesn't
eliminate — defect 2, and is complementary to A1: A1 stops the
*response* to repeated crossings within a window; this reduces spurious
*first* crossings. Keep the existing, already-tested rationale for using
raw (not goodput) received in the comparison
(`CongestionDetectionUsesRawReceivedNotGoodput`, pinned by the prior
cycle) — this is a window-matching fix to the existing predicate, not
the predicate swap that test already rejects.

**Tests (A):**
- `test_admission_field_replay.cpp`'s two committed tests pass (the gate).
- **New**: `SustainedRealLossMustConverge` (or similarly named) in
  `test_admission_field_replay.cpp` — the two-sided counter-test required
  by finding F2. Replays a synthetic *sustained* capacity drop (not the
  recorded onset/handover transients) where the true link capacity falls
  and stays down for the whole replay window. Assert the effective cap
  converges to within a documented band of the reduced capacity inside a
  bounded time (concrete bound to be picked during implementation — the
  RCA's framing is "seconds, not the refractory's multi-minute worst
  case"; issue #52's `over_horizon` phase needs the controller capable of
  reacting again inside a bounded window once the link returns). This is
  the test that fails EWMA alpha=0.2 (which never detects congestion) and
  gates the refractory backoff cap in A1 from growing large enough to
  defeat convergence.
- New unit tests in `test_admission_control.cpp`:
  - refractory suppresses **any** decision — decrease or additive
    recovery — for both congested and clean samples within the window,
    and evaluates the next sample normally once it elapses (direct unit
    test of A1's F9-clarified freeze semantics, independent of the field
    replay).
  - refractory grows on a repeated decrease within an unresolved episode
    and resets after a clean sample once the window has elapsed (the
    backoff/reset halves of A1).
  - decrease branch is purely multiplicative from `effective_rate_limit_`
    with no goodput term at all — the clamp is gone, not conditionally
    applied (A2, replaces the previously-planned "ignores clamp when
    dropped>0" test, which no longer applies).
  - a new `admission_refractory_period_seconds` setter test at default,
    non-default, and boundary values (NaN/negative fall back to default,
    matching the existing parameter pattern).
  - window-matched comparison test: a single transiently-skewed sample
    surrounded by healthy samples does not cross the congestion threshold
    once the sender-side rate is computed over the receiver's 5 s window,
    where it would have crossed on the raw instantaneous comparison (A3).

### C — restore message-level drop granularity (`src/connection.cpp:404-676`)

**Root cause (confirmed by reading `cce4401` and `1489cfb`/`0c8b75f`):**
`Connection::send(const std::vector<WrappedPacket>&, ...)` (the batch/message
overload, `src/connection.cpp:404-511`) runs an aggregate pre-check
(`can_send(total_size, ...)`) that can reject a message atomically — but on
the **success** path it does not reserve `total_size`, by design (the
comment at `connection.cpp:466-474` states this is deliberate, so two
concurrent batches can both pass the aggregate check and both proceed to the
per-packet loop). Each packet in the loop then makes its own independent
`can_send` call via the single-packet `send()` overload
(`src/connection.cpp:514-676`). If budget runs out partway through a
message's fragments — because a concurrent connection's traffic, or a
sibling message on the same connection, consumed it between the aggregate
check and a later fragment's per-packet check — later fragments are
individually dropped while earlier ones already went out. Those earlier
fragments can never be reassembled into a deliverable message, so their
bytes were pure waste on an already-degraded link. Before `1489cfb`/`0c8b75f`
(2026-05-18, made necessary by `republish_group_` going `Reentrant`), a
single `can_send` covered the whole message and admitted or rejected it as
one unit (`cce4401`, 2022-07-27).

**Fix — reserve the whole message atomically, keep the reserve-then-record
concurrency shape:**

1. In the batch overload, replace the check-only aggregate pre-check with a
   check-**and**-reserve done under one `sent_packet_statistics_mutex_`
   acquisition: on `can_send(total_size, ...)` passing, immediately add
   `total_size` to `reserved_bytes_in_flight_` before releasing the lock.
   This closes the TOCTOU gap the current comment calls out as intentional
   — two concurrent batches now genuinely can't both pass, because the
   first one's reservation is visible to the second one's `can_send` call.
   On failure, drop the whole message (unchanged behavior) and return.
2. Give the single-packet `send()` overload a way to skip its own
   check-and-reserve step when called from the batch loop (its bytes are
   already reserved by the caller) while still doing the record-and-release
   half (I/O outcome, `sent_packet_statistics_.add`, decrement
   `reserved_bytes_in_flight_` by that packet's share) per packet as it
   completes. A private overload or a bool parameter both work.

   **Correction (2026-08-25, `review-plan` finding F5): the call graph is
   not "message-batch vs. everything else."** The batch overload has
   exactly **one** production caller — `udp_bridge.cpp:2140` — invoked
   with `is_overhead` both `false` (regular topic traffic) *and* `true`
   (BridgeInfo, resend requests, topic lists). It is not split across a
   "message-batch" caller and a separate "overhead pings, individual
   resends" caller; the only *other* caller of the single-packet overload
   is `resend_packets` (`connection.cpp:845`), which is unaffected by
   this change. So the aggregate check-and-reserve here also changes
   admission for the overhead traffic `updateAdmissionControl` itself
   consumes as feedback — `feedback_stale` reads that traffic's
   disappearance as congestion. This is probably harmless in practice
   (overhead messages are usually one packet, and the failure path
   already drops them atomically today), but it must be an explicit,
   tested decision — does `is_overhead` traffic get the same aggregate
   reservation treatment as regular messages, or does it need its own
   path? — not an accident of a mis-described call graph.
3. Wrap the batch loop in a reservation guard (same RAII shape as the
   existing single-packet `ReservationGuard`) sized to the *remaining*
   unreleased portion of `total_size`, so a mid-loop exception (a fragment's
   `sendto` throwing `ConnectionException`) still leaves
   `reserved_bytes_in_flight_` correct for the packets that were never
   attempted.
4. The blocking `sendto` poll loop for each fragment still runs **outside**
   any lock — this preserves the property `1489cfb`/`0c8b75f` exists to
   guarantee (bounded `sent_packet_statistics_mutex_` hold time, so
   `sendBridgeInfo` and the socket-drain path never wedge on it — the #10
   class of bug). Only the *admission decision* becomes message-atomic
   again; the *lock discipline* around per-packet I/O is unchanged.

**Reservation release, per exit path (added 2026-08-25, `review-plan`
finding F6).** The C design otherwise preserves the `1489cfb`/`0c8b75f`
lock-hold discipline — the aggregate check-and-reserve is two bookkeeping
operations under one `sent_packet_statistics_mutex_` acquisition, and the
per-fragment `sendto` poll loop stays lock-free — but it must not be
ambiguous about who releases what on every way a fragment's inner send
can end:
- **success**: release that fragment's `p.packet_size` after `sendto`
  completes and its stats are recorded.
- **`ECONNREFUSED`**: release `p.packet_size`; the fragment is dropped,
  not retried inline.
- **`no_address`**: a concurrent `setHostAndPort()` can clear
  `addresses_` mid-loop, after the batch's own aggregate pre-check
  already passed — release `p.packet_size` here too.
- **`ConnectionException` thrown**: the step-3 RAII reservation guard
  must still release the remaining unreleased portion on unwind.

State the accounting invariant explicitly rather than leaving it
implicit: `packet.packet.size() == p.packet_size` by construction
(`wrapped_packet.cpp:12`), so releasing `p.packet_size` per fragment as
it completes — on any of the above paths — sums to exactly `total_size`
regardless of which exit path each fragment in the message takes.

**Tests (C):**
- Behavioral: a message whose fragments would only partially fit in the
  current per-second budget is dropped **as a whole** — zero fragments sent
  — rather than partially sent. (Direct regression test for the defect the
  issue describes.)
- Behavioral: a message that fully fits is sent complete and once (no
  regression to the aggregate-reserve accounting under-counting or
  double-counting bytes).
- Concurrency/TOCTOU regression (explicitly scoped per the operator's
  2026-08-25 comment): two concurrent `send()` calls for two different
  messages — each individually within budget, but jointly over it — invoked
  the way `republish_group_`'s `Reentrant` callback group actually invokes
  them (real concurrent threads/executor, not a single-threaded stand-in,
  mirroring how the #10 wedge tests were structured). Assert the aggregate
  reservation prevents joint over-admission: exactly the messages that fit
  are admitted whole, none partially.
- Confirm existing lock-hold-time tests / `test_connection_rate_limit.cpp`
  and any #10-lineage wedge tests are unaffected (no lock now held across
  the per-fragment `sendto` loop).
- **New (F5):** `is_overhead` traffic (BridgeInfo, resend requests, topic
  lists) through the batch overload gets the same aggregate
  check-and-reserve behavior verified explicitly — assert it either is
  admitted/rejected atomically like regular messages, or document and
  test the deliberate exception if implementation chooses a separate
  path for it.
- **New (F6):** reservation release is exercised on each of the four exit
  paths listed above (success, `ECONNREFUSED`, `no_address` mid-loop,
  thrown `ConnectionException`), asserting `reserved_bytes_in_flight_`
  returns to its pre-message value in every case — not just the success
  path the behavioral tests above already cover.

### Verification against issue #52's own acceptance criteria and the bench
suite (new section, added 2026-08-25 per `review-plan` finding F3)

Issue #52's body states two acceptance criteria that this plan's
unit-level tests do not by themselves satisfy, and the prior draft never
mentioned them:

1. **The resend-amplification invariant passes on its own terms, with no
   loosening of `F_resend_multiplier`.** `test_invariant_resend_amplification`
   (`test/bench/test_range_degradation.py:678-693`) currently carries
   `xfail(strict=True)`, with a reason string that explicitly forbids
   raising `F_resend_multiplier` to keep it xfailing. The prior #52 cycle
   already closed most of the gap (critical resend/msg 3.108 → 0.144
   against a 0.200 ceiling) but left `fringe` (0.018 vs 0.010) and
   `lossy` (0.077 vs 0.060) just over. This cycle's A1/A2/A3 changes alter
   the same control loop's behavior under loss and must be re-measured
   against this invariant, not assumed neutral. If it passes cleanly,
   **remove the `xfail` marker in this PR** (the marker's own comment
   instructs this). If it does not, that is a plan-blocking result
   requiring the same re-derivation treatment as F1/F2 — not a marker
   left in place with a wider `F_resend_multiplier`.
2. **A co-tenant management flow survives every phase.**
   `test_invariant_cotenant_management_flow_survives`
   (`test/bench/test_range_degradation.py:780-830`) is already committed
   and is *not* currently marked `xfail`. It is the bench-level
   realization of `link_headroom_fraction` — the parameter A2's revision
   above leaves without a call site (see A2's open resolution). The issue
   body states plainly: **"This invariant should pass before the cap is
   relaxed on anything that goes to sea."** Whatever A2 resolves
   `link_headroom_fraction` to (re-applied elsewhere, or confirmed
   redundant) must be checked against this invariant directly, by running
   it — not inferred from the field-replay unit tests, which don't
   exercise this path at all.

**Run the bench suite before and after this PR's changes land:**

```bash
UDP_BRIDGE_BENCH_SCENARIOS=1 colcon test --packages-select udp_bridge \
    --pytest-args -k test_bench_range_degradation
```

Record a before/after table (per-phase rate, resend/msg ratio, cotenant
delivery % and p95 latency) in this plan's Implementation Notes and in the
PR description, matching the pattern the prior #52 cycle used.

**Named tension, to resolve by measurement, not by inspection (F3):** A1's
refractory period holds the controller's decision fixed for a whole
congestion episode, growing exponentially on repeated congestion. The
bench harness's phase holds are 10 s each — an order of magnitude coarser
than the ~2 s BridgeInfo period the field-replay tests operate on. If a
refractory window grown mid-episode exceeds a phase's hold length, the
controller cannot react within that phase, which can show up as a
regression in the co-tenant invariant's per-window p95-latency bound
(`K_cotenant_p95_latency_s` / the transient latency ceiling,
`test_range_degradation.py:780-830`) even though the field-replay tests
and the new `SustainedRealLossMustConverge` counter-test (A3) all pass.
Pick the refractory growth cap with this bound in view during
implementation, and confirm with the actual bench re-run above — a
refractory tuned only against the field-replay tests' timescale can
silently violate the bench harness's. (`test_invariant_recovery_completeness`
is independently `xfail`'d for #60 and is not a gate for this PR, but a
regression there beyond what #60 already documents is still worth noting
in the before/after table.)

### Doc/config sweep (scoped up front — the prior cycle needed 3 review
rounds to close this same cluster after a smaller change to the same law)

Files to update alongside the code, in the same PR (list corrected
2026-08-25 per `review-plan` finding F4):
- `doc/admission_control_design.md` — add the refractory mechanism, the
  dropped-clamp multiplicative decrease, the window-matched comparison,
  and the resolution of `link_headroom_fraction`'s call site; record the
  tuned constants and why.
- `doc/resend_budget_design.md` — check for any cross-references to the
  decrease-target/goodput text that this changes.
- `include/udp_bridge/connection.h` — comments on `updateAdmissionControl`
  and the new state/setters.
- `include/udp_bridge/resend_constants.h` — comments for every new/changed
  constant, in the same style as the existing entries (cite the field
  episode, not just the value).
- **`udp_bridge/README.md:211-212`** (package README, prose parameter
  reference — missing from the prior draft's list entirely). This is the
  text that currently states the decrease-target rule A2 is changing
  ("Applied as the target of a congested backoff") and is where the new
  `admission_refractory_period_seconds` parameter and
  `link_headroom_fraction`'s resolved role belong.
- **`.agents/README.md`** — verified-parameter table. Corrected path: this
  file lives at the **repo root**
  (`layers/worktrees/issue-udp_bridge-52/core_ws/src/udp_bridge/.agents/README.md`),
  not under `udp_bridge/` as the prior draft's Files-to-Change table had
  it. Add `admission_refractory_period_seconds` (and its backoff/cap
  constants if surfaced as parameters), update the decrease-target
  description.
- **`udp_bridge/CMakeLists.txt`** (missing from the prior draft entirely).
  A new `test_message_atomicity.cpp` (or wherever C's concurrency/TOCTOU
  test lands) needs an `add_udp_bridge_gtest(...)` registration line,
  following the existing pattern at `CMakeLists.txt:154` (e.g. the
  `test_admission_control` / `test_admission_field_replay` entries
  immediately above it).
- `test/bench/README.md` and `test/bench/test_range_degradation.py` module
  docstring — sync if the bench invariant count or basis changes (see the
  new Verification subsection — the resend-amplification `xfail` marker
  is a likely removal).
- `config/example_params.yaml` — new parameter with a **correctly-typed
  double literal** (`8192.0`, not `8192` — the exact trap the prior cycle
  hit and fixed; the precedent is at `config/example_params.yaml:109`,
  not line 61 as the prior draft said).

## Files to Change

| File | Change |
|------|--------|
| `udp_bridge/include/udp_bridge/connection.h` | New refractory state/config, updated comments |
| `udp_bridge/include/udp_bridge/resend_constants.h` | New refractory constants (base, growth, cap) |
| `udp_bridge/src/connection.cpp` | A1 refractory gate (freezes both decrease and recovery), A2 clamp removed (pure multiplicative decrease), A3 window-matched comparison, C batch reserve-then-record + per-exit release |
| `udp_bridge/src/udp_bridge.cpp` | Declare + apply `admission_refractory_period_seconds` parameter (configure-time-only, matching existing admission params — see #76 relationship note in A1) |
| `udp_bridge/test/test_admission_control.cpp` | New unit tests for A1/A2/A3 |
| `udp_bridge/test/test_admission_field_replay.cpp` | **New**: `SustainedRealLossMustConverge` two-sided convergence counter-test (F2). The two already-committed tests are unedited — they remain the gate. |
| `udp_bridge/test/test_connection_rate_limit.cpp` or a new `test_message_atomicity.cpp` | New behavioral + concurrency tests for C, plus `is_overhead` and per-exit-path reservation-release coverage (F5/F6) |
| `udp_bridge/config/example_params.yaml` | New parameter, correctly-typed double literal (precedent at line 109) |
| `udp_bridge/doc/admission_control_design.md` | Refractory/dropped-clamp/window-matching sections, `link_headroom_fraction` resolution, tuned constants |
| `udp_bridge/doc/resend_budget_design.md` | Cross-reference check |
| `udp_bridge/README.md` | **New (F4)**: prose parameter reference at lines 211-212 — decrease-target rule, new refractory parameter, `link_headroom_fraction` role |
| `udp_bridge/CMakeLists.txt` | **New (F4)**: register new test binary(ies) via `add_udp_bridge_gtest(...)`, pattern at line 154 |
| `.agents/README.md` | **Path corrected (F4)**: repo root, not `udp_bridge/.agents/README.md`. Parameter table update. |
| `udp_bridge/test/bench/README.md`, `udp_bridge/test/bench/test_range_degradation.py` | Docstring/count sync — likely `xfail` marker removal on `test_invariant_resend_amplification` if it passes cleanly (see Verification) |

## Principles Self-Check

| Principle | Consideration |
|---|---|
| Human control and transparency | The refractory period is a new per-connection tunable, configurable like the existing AIMD parameters (not hardcoded) — matches the issue-review recommendation. |
| Test what breaks | The two committed field-replay tests are the primary acceptance oracle for A; C gets both behavioral and real-concurrency regression tests, since the defect being fixed here (per-packet enforcement) was itself introduced while fixing a concurrency bug (#10-lineage) — untested concurrency is exactly how that happened. |
| A change includes its consequences | Doc/config sweep scoped up front in the Approach, not left for review rounds to rediscover (explicit lesson from PR #56's 3 review rounds). |
| Only what's needed | B (priority classes) stays out, per the operator-approved split; this PR touches only the admission/send path A and C's evidence is about. |
| Improve incrementally | A1–A3 and C are each independently testable, bounded changes to an existing control loop — not a rewrite. The full sequence-gap/matched-byte-counter detector (RCA option B) is explicitly deferred, not attempted partially. |

## ADR Compliance

| ADR | Triggered | How addressed |
|---|---|---|
| 0002 — Worktree isolation | Yes | Work proceeds in `feature/issue-52` on the layer worktree, as it already has been. |
| 0008 — ROS 2 conventions | Yes | New parameter follows the existing `declareIfMissing`/per-connection-config pattern; no deviation. |
| 0013 — progress.md vocabulary | Yes | This plan's own progress entry and all downstream phases use the entry-type vocabulary already in use in this file's history. |

## Consequences

- If direction C reverts to message-level accounting, the reserve-then-record
  concurrency pattern and its rationale comments (`src/connection.cpp:466-599`)
  must be preserved (extended to cover the batch reservation), not silently
  dropped — otherwise the #10 wedge class of bug is back in play. Addressed
  directly in the Approach (A2/C design keeps the lock-hold-time discipline).
- Any new admission-control parameter needs a correctly-typed literal in
  `config/example_params.yaml` — the prior cycle's `8192.0` vs `8192` trap.
  Called out explicitly in the doc/config sweep.
- RCA option B (sequence-gap or matched-byte-counter loss detection) is a
  real, independently-valuable follow-up that this plan does **not**
  attempt — it needs a wire-protocol/schema change (new `BridgeInfo` /
  `RemoteConnection` fields), which means a type-hash bump and a
  coordinated redeploy of both bridge ends, the same class of consequence
  already documented for `effective_rate_limit`. File as a follow-up issue
  once A/C land and their effect on #71/#45 is evaluated (see Open
  Questions) — don't silently let the "detector" defect stay only
  partially addressed (A3 narrows it; it doesn't eliminate it).
- The purely-local resend-ratio detector (F7, evaluated and explicitly
  **not** adopted this cycle per operator decision — see A3) is the
  natural payload for the same follow-up issue, evaluated against
  post-#52 field data rather than decided on paper now.
- `link_headroom_fraction`'s call site disappears with A2's clamp removal
  (see A2). This must be resolved — new basis, or documented as redundant
  — and checked against `test_invariant_cotenant_management_flow_survives`
  before the PR is done, per the new Verification subsection; it cannot
  be left as a still-documented, now-inert parameter.
- The batch overload's aggregate check-and-reserve also governs
  `is_overhead` traffic (BridgeInfo, resend requests, topic lists) — not
  a separate call path, per F5's correction to C. This changes admission
  behavior for the feedback channel `updateAdmissionControl` itself
  depends on, and needs its own test coverage rather than an assumption
  of no effect.
- Per the issue-review's recommendation: after this lands, this is the
  third distinct root-cause pass on the same control loop in ~2 weeks
  (cap-scaling → this instability fix), touching #9, #45, #71 as well.
  Worth a durable design record (ADR-equivalent in this project repo)
  once it lands, rather than only `doc/admission_control_design.md` prose
  — flagged as a recommendation, not committed to in this PR's scope.

## Documentation & Instruction Impact

- **Stale docs** (must land in this PR): `doc/admission_control_design.md`
  (mechanism changed — refractory gate, dropped goodput clamp,
  window-matched comparison, `link_headroom_fraction` resolution),
  `include/udp_bridge/connection.h` and
  `include/udp_bridge/resend_constants.h` comments (new state/constants),
  **`udp_bridge/README.md:211-212`** (prose parameter reference — added
  2026-08-25, F4), `.agents/README.md` parameter table (new parameter;
  path corrected to repo root, F4), `config/example_params.yaml`
  (new parameter with correctly-typed literal), **`udp_bridge/CMakeLists.txt`**
  (new test registration — added 2026-08-25, F4), and — only if their
  content is actually invalidated by this change — `doc/resend_budget_design.md`,
  `test/bench/README.md`, and the `test_range_degradation.py` module
  docstring, including the likely `xfail` marker removal on
  `test_invariant_resend_amplification` (checked via the bench re-run in
  Verification, not assumed).
- **Agent-instruction candidates** (proposals only): none identified beyond
  what's already flagged in Consequences (a possible project-repo ADR for
  the admission-control design, and a follow-up issue for the
  wire-protocol detector fix and the evaluated-but-not-adopted local
  resend-ratio detector) — all are recommendations for the operator to
  decide, not instruction-file edits this PR would make.

## Open Questions

- Exact refractory base period / growth factor / cap — tuning constants,
  to be derived during implementation against
  `test_admission_field_replay.cpp`'s two committed tests **and** the new
  `SustainedRealLossMustConverge` counter-test (see Approach A1/A3, F2),
  cross-checked against the bench harness's 10 s phase holds (see
  Verification, F3). Not blocking plan approval; blocking only the
  specific numeric choice.
- Whether `link_headroom_fraction` gets a new basis (e.g. applied to the
  floor or the additive-recovery target) or is confirmed redundant and
  flagged for removal, once A2's clamp is dropped (see A2, F1). Resolve
  via the bench re-run against `test_invariant_cotenant_management_flow_survives`
  before the PR is done — not blocking plan approval, but blocking PR
  completion.
- Whether `is_overhead` traffic through the batch overload gets the same
  aggregate check-and-reserve as regular messages, or a separate path
  (see C step 2, F5). Resolve with test coverage during implementation.
- Where to add the C concurrency/TOCTOU regression test — a new
  `test_message_atomicity.cpp`, or extend `test_connection_rate_limit.cpp`?
  Lean toward a new file given the distinct concern (message-level
  admission vs. per-connection rate limiting), but not a strong preference.
- Should the `admission_refractory_period_seconds` growth/cap also become
  per-connection-configurable, or stay fixed constants with only the base
  period configurable? The existing precedent (`kResendBackoffBase` is
  fixed, only higher-level knobs like `resend_budget_fraction` are
  parameters) suggests fixed is fine, but worth confirming during
  implementation once the tuned values are known. Also note the
  configure-time-only relationship to #76 (see A1) — not blocking, but a
  known limitation to carry forward.
- After A lands, #71 (idle-link cap collapse) and #45 (2026-08-04 RCA,
  unstarted) should be revisited — RCA states A/B/C should reduce or close
  their severity. Not part of this PR; flagged so it isn't dropped.
- Whether to file the RCA-option-B (sequence-gap/matched-byte-counter
  detector) follow-up issue — and, alongside it, the evaluated-but-rejected
  purely-local resend-ratio detector (F7) — now or after this PR lands and
  its effect is measured. Recommend after, so the follow-up issue can cite
  what A3's window-matching fix left unresolved rather than guessing in
  advance.

## Estimated Scope

Single PR with atomic commits, following the pattern the prior #52 cycle
used successfully (one logical fix per commit, code fixes carry their own
regression tests). Two independently-testable clusters (A1–A3 for the
control-loop fix, C for message-level drop granularity) plus the doc/config
sweep. Not split into stacked PRs — both clusters touch the same file
(`src/connection.cpp`) and were operator-approved as one issue's scope.
