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

// Default fraction of measured goodput deliberately left unused, so a
// co-tenant on the same path (an SSH session, the operator's own
// management traffic) is not starved by the bridge.
//
// This is the property that actually prevents the lockout
// `maximum_bytes_per_second` was originally added for. A ceiling in
// absolute bytes binds only while the link is healthy: the same bench
// run had a 4 MB/s cap and still saw the bridge take 61% of a 62.5 kB/s
// path (100% in individual samples), because a degraded link is a
// saturated link. A headroom target scales with whatever the link is
// actually delivering, so it holds in exactly the case the ceiling
// abandons.
//
// Overridable per connection via `link_headroom_fraction`.
inline constexpr float kDefaultLinkHeadroomFraction = 0.2f;

// To convert a constant to seconds-as-double at a call site, use
// `kFoo.count()`. To build an rclcpp::Duration, pass the constant
// directly to rclcpp::Duration's chrono::duration constructor:
//   auto cutoff = now - rclcpp::Duration(kSentPacketTTL);

}  // namespace udp_bridge

#endif  // UDP_BRIDGE_RESEND_CONSTANTS_H
