#ifndef UDP_BRIDGE_RESEND_CONSTANTS_H
#define UDP_BRIDGE_RESEND_CONSTANTS_H

// Tuning constants for the resend / cleanup loops.
//
// These values were tuned during issue #9 (resend amplification) based
// on the 2026-04-21 BizzyBoat deployment, which showed ~22% resend
// overhead and 126% rx_duplicate on VPN because the receiver was
// requesting packets the sender had forgotten AND the re-request
// cooldown was shorter than realistic RTTs under load.
//
// The algorithm: a missing packet is requested when it has been
// observed missing for at least kResendDebounceHold (avoids spurious
// requests on legitimate reorder). On repeated misses, the re-request
// cooldown grows as min(base * 2^(attempts-1), cap), giving exponential
// backoff. A missing packet is abandoned when its first-request time is
// older than kSentPacketTTL — the sender has by then evicted the
// packet from its sent_packets_ cache, so further requests are
// pointless. Give-ups bump a per-remote counter exposed via
// Remote.msg.resend_giveup_count so silent loss is operationally
// visible.
//
// A future "simplification" pass MUST NOT collapse this back to a
// flat-cooldown single-constant design without re-reading issue #9.

#include <chrono>
#include <cstdint>

namespace udp_bridge
{

// Sender-side: how long Connection::sent_packets_ retains a sent
// packet so resends are possible. UDPBridge::cleanupSentPackets uses
// this.
inline constexpr std::chrono::duration<double> kSentPacketTTL{5.0};

// Receiver-side: how far back the receiver retains gap-detection
// state. RemoteNode::clearReceivedPacketTimesBefore and the
// defragmenter cleanup in UDPBridge use this. Defaulted to
// kSentPacketTTL so the two are aligned today.
//
// **Invariant (bug-prevention)**: kReceiveHistoryWindow must be `<=`
// kSentPacketTTL. The original bug (issue #9 failure mode D) was the
// other way round (receiver 5 s > sender 3 s) which let the receiver
// legally request packets the sender had already forgotten. The
// static_assert below pins this so a future widening of the receive
// window can't silently re-introduce that bug.
inline constexpr std::chrono::duration<double> kReceiveHistoryWindow = kSentPacketTTL;

static_assert(
  kReceiveHistoryWindow <= kSentPacketTTL,
  "kReceiveHistoryWindow must be <= kSentPacketTTL — see issue #9 "
  "failure mode D. Widening the receive window beyond the sender's "
  "memory lets the receiver request packets the sender has forgotten, "
  "which is the original bug this constant pair was introduced to close.");

// Debounce hold: a packet is only considered "missing" if the spin
// tick's current time exceeds the received time of the gap's previous
// neighbor by at least this. Avoids spurious resend requests on
// legitimate reordering, which is common over cellular and VPN paths.
inline constexpr std::chrono::duration<double> kResendDebounceHold{0.100};

// Exponential-backoff base for repeat resend requests on the same
// missing packet number. Cooldown = min(base * 2^(attempts-1), cap).
inline constexpr std::chrono::duration<double> kResendBackoffBase{0.200};

// Cap for the exponential backoff (8 x base). Yields a backoff
// schedule of 0.2 -> 0.4 -> 0.8 -> 1.6 -> 1.6 ... which covers
// cellular RTT (50-200 ms) up through degraded-satellite RTT
// (1-2 s), and gives ~6 attempts in the kSentPacketTTL window.
inline constexpr std::chrono::duration<double> kResendBackoffCap{1.600};

// Defensive clamp on the shift used to compute the per-packet
// cooldown: cooldown = kResendBackoffBase * (1u << shift), with
// shift = min(attempts - 1, kMaxBackoffShift). Bounds the shift
// expression so it can't reach UB at attempts >= 33. The chosen
// value is the smallest shift that already produces a cooldown
// strictly greater than kResendBackoffCap (0.2 * 2^7 = 25.6s >> cap),
// so the clamp only kicks in when the cap clause would already pin
// the cooldown to kResendBackoffCap — observable behavior is unchanged.
inline constexpr uint32_t kMaxBackoffShift = 7;

// Sender-side resend budget (issue #44). Resend storms self-amplify:
// under ack starvation the sender treats all in-flight packets as lost
// and retries at multiples of link capacity (2026-05-01: 13 MB/s of
// retries vs a 1.0 MB/s VPN link; 2026-08-04: ~1.5 MB/s dropped +
// 220 kB/s sent resends vs a 1.2 MB/s cap, sustained until restart).
// Connection::resend_packets bounds the resend category to a fraction
// of measured goodput (issue #52; was the connection's rate limit under
// #44), and shrinks that fraction exponentially while nothing is being
// received (no packets from the remote implies no acks — retrying harder
// cannot help).

// Default maximum fraction of measured goodput that resend traffic may
// consume, measured over the same strict 1-second window can_send uses.
// Overridable per connection via the `resend_budget_fraction` parameter.
// 0.25 keeps 75% of what the link is delivering for fresh data while
// still allowing meaningful recovery on lossy-but-alive links.
inline constexpr float kDefaultResendBudgetFraction = 0.25f;

// How long the connection must have received nothing before the
// resend budget starts halving (one halving per elapsed multiple).
// 1 s is several heartbeat/ack periods — long enough that ordinary
// jitter never triggers it, short enough to bite within seconds of a
// real inbound collapse.
inline constexpr std::chrono::duration<double> kAckStarvationThreshold{1.0};

// Clamp on the starvation halvings: budget floor is
// fraction / 2^kMaxAckStarvationBackoffShift (4 -> 16x reduction,
// ~1.6% of measured goodput at the default fraction). Keeps a trickle
// of resends flowing as a probe so recovery is possible the moment
// the inbound path returns.
inline constexpr uint32_t kMaxAckStarvationBackoffShift = 4;

// Adaptive admission control (issue #43). AIMD on a per-connection
// effective rate cap, driven by the delivered-vs-sent ratio computed
// from the remote's echoed BridgeInfo received_bytes_per_second (see
// doc/admission_control_design.md). Complements the resend budget
// above: that bounds the resend category; this scales what fresh data
// is offered onto a connection whose delivered capacity has dropped.

// Multiplicative decrease applied to the effective rate cap on a
// congestion signal (delivery below threshold, or stale feedback).
inline constexpr float kAdmissionDecreaseFactor = 0.5f;

// Additive recovery per clean feedback interval, as a fraction of the
// CURRENT effective cap — not of the configured maximum_bytes_per_second.
//
// It was cap-relative until issue #52. On a link delivering a small
// fraction of its configured cap that made the probe step enormous
// relative to reality: the 2026-08-22 bench measured a 4 MB/s configured
// cap against a 62.5 kB/s path, so each clean sample added 400 kB/s —
// six times the whole link, undoing three multiplicative halvings. The
// effective cap oscillated around 2291 kB/s and never even reached its
// own floor. A controller's step size has to be measured against where
// the controller currently is, not against a number an operator typed.
inline constexpr float kAdmissionAdditiveStepFraction = 0.1f;

// Absolute lower bound on the additive step. Relative-only recovery is
// asymptotically slow near the floor (10% of 8 kB/s is 800 B/s), so a
// link that has genuinely healed would crawl back. Keeps recovery from
// the floor to a usable rate on the order of tens of seconds.
inline constexpr float kAdmissionAdditiveStepMinimumBytesPerSecond = 2048.0f;

// Congestion trigger: decrease when the remote reports receiving less
// than (1 - threshold) of what we sent. 0.1 tolerates the signal's
// known inflation sources (duplicates, resend traffic, differing
// smoothing windows) without triggering on noise.
inline constexpr float kAdmissionLossThreshold = 0.1f;

// Default minimum effective cap, in absolute bytes per second.
// Overridable per connection via `admission_floor_bytes_per_second`.
//
// Absolute since issue #52. As a fraction of the configured cap this was
// 400 kB/s against a 62.5 kB/s bench link — the floor itself was six
// times the path, so backing off "to the floor" still meant offering
// several times what the link could carry. A floor exists to keep the
// control tier and the BridgeInfo feedback loop alive; that is a real
// quantity of bytes, not a proportion of an operator's declared cap.
//
// 8192 covers the measured overhead tier (BridgeInfo + topic_statistics
// ran ~5.7 kB/s on the bench) plus the critical tier, and is still only
// 13% of that same degraded 62.5 kB/s link.
inline constexpr float kDefaultAdmissionFloorBytesPerSecond = 8192.0f;

// Refractory period between admission decreases (issue #52, the
// 2026-08-25 Appledore RCA). The controller runs on every BridgeInfo
// arrival (~2 s in the field), and before this constant existed each
// arriving congested sample applied another kAdmissionDecreaseFactor.
// On the Isles of Shoals transit that turned ONE marginal delivery
// sample (442300/513563 = 0.86 against a 0.9 threshold) into seven
// halvings in 12 s: a 183x cap reduction, 35 times in 9.5 hours,
// 1.95 GB discarded at the boat's own gate while Starlink reported
// 0.0-0.05% drop and flat 17-24 ms latency throughout.
//
// While the window is active the controller freezes: NO sample is acted
// on, congested or clean. That is deliberate and unconditional on
// sample type — letting a clean sample recover inside the window would
// re-open the same feedback path from the other side (recover, re-trigger,
// halve) and, per the review model, changes both field-replay gate
// outcomes.
//
// 5.0 s is not "a couple of feedback intervals" picked by feel: it is
// the length of the receive-rate measurement window both ends use
// (Connection::data_receive_rate, and the sender-side window the
// congestion detector now matches against it). One full window is the
// shortest interval after which BOTH filters describe traffic sent at
// the new cap rather than the old one — reacting again before that is
// reacting to our own previous decrease.
inline constexpr double kDefaultAdmissionRefractoryPeriodSeconds = 5.0;

// Upper bound on the configurable refractory base period, seconds
// (issue #52 review round 1).
//
// Every other admission setter clamps both ends; this one guarded only
// NaN and negatives, so `+Inf` — or any large finite typo — was accepted
// verbatim. One congested sample then opened a window that never
// expires: no decrease, no recovery, and no log line, for the life of
// the node. A control loop that can be switched off by a config typo is
// worse than one tuned badly.
//
// 60 s is the ceiling, not a recommendation. Above it the controller
// cannot answer a link change inside any operator-observable timescale:
// BridgeInfo arrives about every 2 s, `SustainedRealLossMustConverge`
// requires convergence within 40 s, and a base of 30 s was measured
// (review round 1, deliberately deafened controller) to converge only at
// 92.8 s. Out-of-range values clamp to this bound rather than falling
// back to the default, so an operator asking for a long window still
// gets the longest one that leaves the loop alive.
inline constexpr double kMaximumAdmissionRefractoryPeriodSeconds = 60.0;

// Growth factor applied to the refractory window on each successive
// decrease within an UNRESOLVED congestion episode.
//
// "Unresolved" means no clean sample has been ACTED ON since the last
// decrease. A clean sample arriving while the window is still open does
// NOT resolve the episode: the gate returns before the clean branch, so
// the flag stays set and the next decrease still grows the window. That
// is deliberate and follows from the same argument as the freeze itself
// — a sample taken inside the window describes traffic sent at the OLD
// cap, so it is no more trustworthy as evidence the episode ended than
// it would be as grounds for recovering. Only a clean sample the
// controller was free to act on clears it.
//
// Mirrors the exponential backoff the
// resend re-request path already uses (kResendBackoffBase /
// kResendBackoffCap) rather than inventing a second idiom. The recorded
// onset stays "congested" on the raw ratio for its full ~14 s — the
// sender's own rate keeps falling, which is itself an artifact of the
// pre-fix controller that produced the trace — so a single fixed window
// still permits a third and fourth halving inside one episode.
inline constexpr double kAdmissionRefractoryGrowthFactor = 2.0;

// Cap on the grown window, as a multiple of the configured base — so an
// operator who retunes the base retunes the ceiling with it.
//
// 2x (10 s at the default base) is bounded on both sides. Below: one
// growth step is what the field-replay onset needs to hold the recorded
// cascade to two decreases. Above: the range-degradation bench holds
// each phase for 10 s, so a window grown past that could not react
// inside a phase at all, and the smoothed statistics deque only retains
// 10 s of history — freezing longer than the whole measurement record
// means deciding on evidence the controller can no longer see.
inline constexpr double kAdmissionRefractoryMaximumMultiple = 2.0;

// Window, in seconds, over which the congestion detector measures OUR
// OWN send rate (issue #52).
//
// The detector compares what the remote reports receiving against what
// we sent. Before this, the two sides of that ratio came from
// differently-shaped filters: the remote's figure from a 5 s box filter
// (Connection::data_receive_rate), ours from PacketSendStatistics::get(),
// a variable-span 1-10 s filter. 16.1% of samples in the field data
// showed remote_rx > 1.05 x sent_on_wire — physically impossible if both
// described the same interval — proving the ratio was dominated by
// filter skew during any transient rather than by loss. Matching the
// sender's window to the receiver's 5 s window narrows that skew; it
// does not eliminate it (propagation delay remains), which is why the
// refractory period above is the primary defence.
inline constexpr double kAdmissionSendRateWindowSeconds = 5.0;

// Window, in seconds, over which Connection::data_receive_rate measures
// the rate of data ARRIVING from a remote — the figure echoed back in
// BridgeInfo and used as the other side of the detector's ratio.
//
// This used to be a bare `time - 5` literal in data_receive_rate with
// nothing tying it to the sender-side window. The whole premise of the
// detector fix is that the two windows MATCH, so leaving one of them as
// an unnamed literal meant a future edit to either could silently
// re-open the 16.1%-impossible-samples skew described above (#52, review
// round 2). Named, and pinned by the static_assert below.
inline constexpr double kReceiveRateWindowSeconds = 5.0;

static_assert(
  kAdmissionSendRateWindowSeconds == kReceiveRateWindowSeconds,
  "kAdmissionSendRateWindowSeconds must equal kReceiveRateWindowSeconds "
  "— see issue #52. The congestion detector divides what the remote "
  "reports receiving over ITS window by what we sent over OURS; if the "
  "two differ, the ratio measures filter skew rather than loss, which is "
  "what turned a 1% link into 35 cap collapses on 2026-08-25.");

// RETIRED: `link_headroom_fraction` and its default constant (issue #52,
// 2026-08-25). Recorded here because the mechanism it provided is gone
// and nothing replaced it — a reader reaching for a headroom knob should
// find out why there is not one, not find a live-sounding rationale.
//
// What it DID: it named a fraction of measured goodput to leave unused,
// so a co-tenant on the same path (an SSH session, the operator's own
// management traffic) was not starved by the bridge. That was a
// STRUCTURAL property: it reserved a share of measured throughput
// whether or not the bridge itself was losing, which an absolute
// ceiling never does. A ceiling in bytes binds only while the link is
// healthy — one bench run had a 4 MB/s cap and still saw the bridge take
// 61% of a 62.5 kB/s path (100% in individual samples), because a
// degraded link is a saturated link.
//
// Why it WENT: its only call site was the congested branch's
// `min(cap x 0.5, (1 - headroom) x goodput)` clamp, and measured goodput
// is depressed by the very throttling the formula was computing, so the
// clamp was a positive feedback loop. On the recorded 2026-08-25 onset
// its first step alone landed at 0.8 x (254615 - 51715) = 162320 — below
// the regression bound — before any second decrease existed for the
// refractory gate to suppress, and no refractory value rescued it. It
// was measurably harmful and cannot return in that form.
//
// What protects a co-tenant NOW: the refractory-gated multiplicative
// decrease, and only insofar as the bridge SHARES the loss — it fires
// when `remote_received < 0.9 x sent`, i.e. when the bridge's own
// delivery degrades. On a saturated path where the bridge wins the queue
// and the co-tenant is the one starved, the bridge reads clean and never
// backs off. No headroom mechanism remains; a replacement basis is
// argued on issue #61.
//
// The parameter itself is detected on PRESENCE from the node's parameter
// overrides and WARNed about (`UDPBridge::on_configure`) — there is no
// declared default to compare against, deliberately: a value comparison
// against `static_cast<double>(0.2f)` matched YAML `0.2` only by a
// float-to-double widening accident. See
// doc/admission_control_design.md, "link_headroom_fraction after the
// clamp removal".

// To convert a constant to seconds-as-double at a call site, use
// `kFoo.count()`. To build an rclcpp::Duration, pass the constant
// directly to rclcpp::Duration's chrono::duration constructor:
//   auto cutoff = now - rclcpp::Duration(kSentPacketTTL);

}  // namespace udp_bridge

#endif  // UDP_BRIDGE_RESEND_CONSTANTS_H
