# Plan: Resend amplification under real link degradation

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
(`now - last_admission_decrease_time_ < refractory`), a congested sample is
**not acted on** — no decrease, and no additive recovery either (the
controller holds its last decision rather than pretending the sample didn't
arrive). Once the window elapses, the next sample is evaluated normally.

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
something to hand-derive on paper.** Implementation must build and run
`test_admission_field_replay.cpp` iteratively to land on values that satisfy
both tests' documented tolerances (default starting point: the RCA's own
"≥ 2 BridgeInfo intervals" for the base period). Record the final values and
the reasoning (which constraint from which test drove which constant) in
`doc/admission_control_design.md`, the way `resend_constants.h`'s existing
comments cite the field episodes that produced their values.

**A2. Decrease target: stop deriving it from a possibly self-inflicted
goodput reading.**

Per RCA direction C, the `(1 - headroom) x goodput` clamp on the decrease
branch (`src/connection.cpp:284-286`) is only trustworthy when the
goodput reading reflects real link capacity — not our own admission gate
already limiting what we offered. `PacketSizeData`/`DataRates` already
expose this locally: `sent_packet_statistics_.get()` (used a few lines
above at `sent = sent_packet_statistics_.get()`) carries
`dropped_bytes_per_second` — bytes *we* dropped at our own `can_send` gate
this window. Extend the existing `feedback_unusable` reasoning: only apply
the goodput clamp when, in addition to the current finite/non-negative/
duplicate-consistency checks, `sent.dropped_bytes_per_second == 0` for this
interval (we were not self-limiting). When we *were* self-limiting, the
decrease stays the plain `effective_rate_limit_ x kAdmissionDecreaseFactor`
— multiplicative from the controller's own last known-good state, not from
a number the controller's own throttling helped produce.

This directly targets the field trace: after the first decrease, the
sender's own recorded rate falls (whether from the real pre-fix controller
or, going forward, from ours), so subsequent goodput readings are exactly
the contaminated case this guards against.

**A3. Detector: dampen a single-sample skewed reading instead of comparing
raw instantaneous rates.**

RCA option B (loss detection from sequence gaps or matched byte counters
over a matched interval) requires new wire fields — `updateAdmissionControl`'s
signature (`remote_received_bps`, `remote_duplicate_bps`, `now`) is fixed by
the already-committed regression tests, and a schema change here means a
type-hash bump requiring coordinated redeploy of both bridge ends (the same
consequence already documented for `RemoteConnection.msg`'s
`effective_rate_limit` field). That is out of scope for this cycle — record
it as a follow-up (see Consequences), not a silent gap.

What's achievable without a wire change: apply local smoothing (EWMA) to
`remote_received_bps` before the congestion comparison, using a time
constant closer to what produces `sent_bps`'s own smoothing, so a single
skewed/lagged sample doesn't independently cross the threshold. This
narrows — doesn't eliminate — defect 2, and is complementary to A1/A3: A1
already stops the *response* to repeated crossings within a window; this
reduces spurious *first* crossings. Keep the existing, already-tested
rationale for using raw (not goodput) received in the comparison
(`CongestionDetectionUsesRawReceivedNotGoodput`, pinned by the prior
cycle) — this is a smoothing fix to the existing predicate, not the
predicate swap that test already rejects.

**Tests (A):**
- `test_admission_field_replay.cpp`'s two committed tests pass (the gate).
- New unit tests in `test_admission_control.cpp`:
  - refractory suppresses a second decrease within the window, and allows
    one after it elapses (direct unit test of A1, independent of the field
    replay).
  - refractory grows on a repeated decrease within an unresolved episode
    and resets after a clean sample (the backoff/reset halves of A1).
  - decrease target ignores the goodput clamp when
    `dropped_bytes_per_second > 0` this interval, and uses it when 0 (A2).
  - a new `admission_refractory_period_seconds` setter test at default,
    non-default, and boundary values (NaN/negative fall back to default,
    matching the existing parameter pattern).
  - EWMA smoothing test: a single transiently-skewed sample surrounded by
    healthy samples does not cross the congestion threshold post-smoothing
    where it would have pre-smoothing (A3).

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
   completes. A private overload or a bool parameter both work; prefer
   whichever keeps the two call sites (message-batch vs. everything else —
   overhead pings, individual resends, which keep using the existing
   per-packet check-and-reserve path unchanged) easiest to tell apart at a
   glance.
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

### Doc/config sweep (scoped up front — the prior cycle needed 3 review
rounds to close this same cluster after a smaller change to the same law)

Files to update alongside the code, in the same PR:
- `doc/admission_control_design.md` — add the refractory mechanism, the
  self-limiting-aware goodput clamp, and the smoothing note; record the
  tuned constants and why.
- `doc/resend_budget_design.md` — check for any cross-references to the
  decrease-target/goodput text that this changes.
- `include/udp_bridge/connection.h` — comments on `updateAdmissionControl`
  and the new state/setters.
- `include/udp_bridge/resend_constants.h` — comments for every new/changed
  constant, in the same style as the existing entries (cite the field
  episode, not just the value).
- `.agents/README.md` — verified-parameter table: add
  `admission_refractory_period_seconds` (and its backoff/cap constants if
  surfaced as parameters), update the decrease-target description.
- `test/bench/README.md` and `test/bench/test_range_degradation.py` module
  docstring — if the bench invariant count or basis changes.
- `config/example_params.yaml` — new parameter with a **correctly-typed
  double literal** (`8192.0`, not `8192` — the exact trap the prior cycle
  hit and fixed at `config/example_params.yaml:61`).

## Files to Change

| File | Change |
|------|--------|
| `udp_bridge/include/udp_bridge/connection.h` | New refractory state/config, updated comments |
| `udp_bridge/include/udp_bridge/resend_constants.h` | New refractory constants (base, growth, cap) |
| `udp_bridge/src/connection.cpp` | A1 refractory gate, A2 self-limiting-aware clamp, A3 EWMA smoothing, C batch reserve-then-record |
| `udp_bridge/src/udp_bridge.cpp` | Declare + apply `admission_refractory_period_seconds` parameter (configure-time-only, matching existing admission params) |
| `udp_bridge/test/test_admission_control.cpp` | New unit tests for A1/A2/A3 |
| `udp_bridge/test/test_admission_field_replay.cpp` | No edits — this is the gate |
| `udp_bridge/test/test_connection_rate_limit.cpp` or a new `test_message_atomicity.cpp` | New behavioral + concurrency tests for C |
| `udp_bridge/config/example_params.yaml` | New parameter, correctly-typed double literal |
| `udp_bridge/doc/admission_control_design.md` | Refractory/self-limiting-clamp/smoothing sections, tuned constants |
| `udp_bridge/doc/resend_budget_design.md` | Cross-reference check |
| `udp_bridge/.agents/README.md` | Parameter table update |
| `udp_bridge/test/bench/README.md`, `test/bench/test_range_degradation.py` | Docstring/count sync if touched |

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
  partially addressed (A3 dampens it; it doesn't eliminate it).
- Per the issue-review's recommendation: after this lands, this is the
  third distinct root-cause pass on the same control loop in ~2 weeks
  (cap-scaling → this instability fix), touching #9, #45, #71 as well.
  Worth a durable design record (ADR-equivalent in this project repo)
  once it lands, rather than only `doc/admission_control_design.md` prose
  — flagged as a recommendation, not committed to in this PR's scope.

## Documentation & Instruction Impact

- **Stale docs** (must land in this PR): `doc/admission_control_design.md`
  (mechanism changed — refractory gate, self-limiting-aware clamp,
  smoothing), `include/udp_bridge/connection.h` and
  `include/udp_bridge/resend_constants.h` comments (new state/constants),
  `.agents/README.md` parameter table (new parameter), `config/example_params.yaml`
  (new parameter with correctly-typed literal), and — only if their content
  is actually invalidated by this change — `doc/resend_budget_design.md`,
  `test/bench/README.md`, and the `test_range_degradation.py` module
  docstring (checked, not assumed).
- **Agent-instruction candidates** (proposals only): none identified beyond
  what's already flagged in Consequences (a possible project-repo ADR for
  the admission-control design, and a follow-up issue for the wire-protocol
  detector fix) — both are recommendations for the operator to decide, not
  instruction-file edits this PR would make.

## Open Questions

- Exact refractory base period / growth factor / cap — tuning constants,
  to be derived during implementation against
  `test_admission_field_replay.cpp` (see Approach A1). Not blocking plan
  approval; blocking only the specific numeric choice.
- Where to add the C concurrency/TOCTOU regression test — a new
  `test_message_atomicity.cpp`, or extend `test_connection_rate_limit.cpp`?
  Lean toward a new file given the distinct concern (message-level
  admission vs. per-connection rate limiting), but not a strong preference.
- Should the `admission_refractory_period_seconds` growth/cap also become
  per-connection-configurable, or stay fixed constants with only the base
  period configurable? The existing precedent (`kResendBackoffBase` is
  fixed, only higher-level knobs like `resend_budget_fraction` are
  parameters) suggests fixed is fine, but worth confirming during
  implementation once the tuned values are known.
- After A lands, #71 (idle-link cap collapse) and #45 (2026-08-04 RCA,
  unstarted) should be revisited — RCA states A/B/C should reduce or close
  their severity. Not part of this PR; flagged so it isn't dropped.
- Whether to file the RCA-option-B (sequence-gap/matched-byte-counter
  detector) follow-up issue now or after this PR lands and its effect is
  measured. Recommend after, so the follow-up issue can cite what A3's
  partial mitigation left unresolved rather than guessing in advance.

## Estimated Scope

Single PR with atomic commits, following the pattern the prior #52 cycle
used successfully (one logical fix per commit, code fixes carry their own
regression tests). Two independently-testable clusters (A1–A3 for the
control-loop fix, C for message-level drop granularity) plus the doc/config
sweep. Not split into stacked PRs — both clusters touch the same file
(`src/connection.cpp`) and were operator-approved as one issue's scope.
