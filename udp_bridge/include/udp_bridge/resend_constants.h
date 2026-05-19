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

// To convert a constant to seconds-as-double at a call site, use
// `kFoo.count()`. To build an rclcpp::Duration, pass the constant
// directly to rclcpp::Duration's chrono::duration constructor:
//   auto cutoff = now - rclcpp::Duration(kSentPacketTTL);

}  // namespace udp_bridge

#endif  // UDP_BRIDGE_RESEND_CONSTANTS_H
