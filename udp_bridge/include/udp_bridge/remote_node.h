#ifndef UDP_BRIDGE_REMOTE_NODE_H
#define UDP_BRIDGE_REMOTE_NODE_H

#include "udp_bridge_interfaces/msg/remote.hpp"
#include "udp_bridge_interfaces/msg/resend_request.hpp"
#include "udp_bridge/packet.h"
#include "udp_bridge/defragmenter.h"
#include "udp_bridge_interfaces/msg/topic_statistics_array.hpp"
#include "udp_bridge_interfaces/msg/bridge_info.hpp"
#include "udp_bridge/types.h"
#include "udp_bridge/publish_queue.h"

#include <deque>
#include <mutex>
#include <set>
#include <unordered_set>

#include "rclcpp/rclcpp.hpp"
#include "rclcpp/node_interfaces/node_interfaces.hpp"

namespace udp_bridge
{

class Connection;

// Represents a remote udp_bridge node.
// Multiple connections to the remote node
// may be specified.
class RemoteNode
{
  public:

  using NodeInterfaces = rclcpp::node_interfaces::NodeInterfaces<
    rclcpp::node_interfaces::NodeTopicsInterface,
    rclcpp::node_interfaces::NodeLoggingInterface,
    rclcpp::node_interfaces::NodeClockInterface,
    rclcpp::node_interfaces::NodeBaseInterface
  >;
  

  RemoteNode(std::string remote_name, std::string local_name, NodeInterfaces node);

  void update(const udp_bridge_interfaces::msg::Remote& remote_message);
  void update(const udp_bridge_interfaces::msg::BridgeInfo& bridge_info, const SourceInfo& source_info);

  const std::string &name() const;

  // returns a name sutible for use as a topic name.
  std::string topicName() const;

  std::shared_ptr<Connection> connection(std::string connection_id);
  std::vector<std::shared_ptr<Connection> > connections();

  std::shared_ptr<Connection> newConnection(std::string connection_id, std::string host, uint16_t port);

  // Insert an externally-constructed Connection (e.g. one created on the
  // outbound CONNECT path before the remote acknowledged) into this
  // RemoteNode's connections_ map. No-op if a connection with the same
  // id is already registered.
  void adoptConnection(std::shared_ptr<Connection> connection);

  std::vector<uint8_t> unwrap(std::vector<uint8_t> const &message, const SourceInfo& source_info);

  std::vector<std::vector<uint8_t> > getPacketsToResend(const udp_bridge_interfaces::msg::ResendRequest& resend_request);

  // Route a ResendRequest to the single connection it was stamped for
  // (issue #23). Does a strict-equality lookup on `rr.connection_id`
  // in `connections_` and forwards to the matching connection's
  // `resend_packets`. The caller is responsible for ensuring
  // `rr.connection_id` is already in the canonical wire form —
  // production callers go through `UDPBridge::decodeResendRequest`,
  // whose receive-side clamp normalizes the incoming string before
  // dispatch. (Unit tests that exercise dispatch directly may pass
  // either a canonical id, or a non-canonical id to assert a miss; see
  // `DispatchDoesNotFuzzyMatchConnectionId`.) If the id is not in
  // connections_, no-op and emit a log naming both the stamped id and
  // the set of currently known connection ids: WARN on the first miss
  // per stamped id
  // (a genuine coordinated-redeploy mismatch needs to be diagnosable
  // at default log levels), DEBUG on subsequent misses for the same id
  // (legitimate CONNECT-cycle races shouldn't spam). The known-ids
  // payload gives an operator enough context to distinguish the two
  // operational scenarios that share this path:
  //   - Legitimate race: the receiver tore the connection down for a
  //     CONNECT cycle or config reload between the sender stamping
  //     the request and us decoding it; resolves on the next BridgeInfo.
  //   - Coordinated-redeploy mismatch: the two bridges' connection-id
  //     strings don't agree; does not self-resolve.
  // First-miss bookkeeping lives in dispatch_miss_warned_ids_ (see
  // field comment below).
  //
  // Caller MUST NOT hold state_mutex_. The method takes it briefly for
  // the lookup, then releases it before invoking
  // Connection::resend_packets (which does socket I/O) — mirrors the
  // snapshot-then-iterate locking discipline elsewhere in this class.
  void dispatchResendRequest(const udp_bridge_interfaces::msg::ResendRequest& rr,
                             int socket, rclcpp::Time now);

  Defragmenter& defragmenter();

  void publishTopicStatistics(const udp_bridge_interfaces::msg::TopicStatisticsArray& statistics);

  void clearReceivedPacketTimesBefore(rclcpp::Time time);

  udp_bridge_interfaces::msg::ResendRequest getMissingPackets();

#ifdef UDP_BRIDGE_BUILD_TESTING
  // Time-injection overload used by unit tests so the gap-scan and
  // backoff logic can be exercised against a controlled clock without
  // bringing up a sim-time-driven node. Production callers should use
  // the no-arg variant above; this overload is here purely for the
  // gtest suite.
  //
  // Gated behind UDP_BRIDGE_BUILD_TESTING so it doesn't leak into the
  // package's installed public ABI — CMakeLists installs `include/`
  // and defines the package-namespaced UDP_BRIDGE_BUILD_TESTING macro
  // privately only when CMake's BUILD_TESTING variable is on, so
  // downstream consumers see only the no-arg variant. The macro is
  // namespaced (`UDP_BRIDGE_BUILD_TESTING` rather than the generic
  // `BUILD_TESTING`) to avoid an ODR hazard with downstream packages
  // that define `BUILD_TESTING` for their own tests. Both variants
  // forward to the private `getMissingPacketsAt(rclcpp::Time)` worker
  // so the implementation is single-sourced regardless of which entry
  // point is built in.
  udp_bridge_interfaces::msg::ResendRequest getMissingPackets(rclcpp::Time now);

  // Test helper: record a received packet at a caller-supplied time
  // without going through unwrap(). Used by the gtest suite to seed
  // received_packet_times_ at known times for the debounce / backoff /
  // give-up scenarios. UDP_BRIDGE_BUILD_TESTING-gated for the same reason as the
  // overload above.
  void recordReceivedPacketTimeForTest(uint64_t packet_number, rclcpp::Time time);

  // Test accessor: returns the current count of ids in the
  // dispatch-miss WARN bookkeeping. Used by the cap-enforcement test
  // to verify that pumping > kDispatchMissWarnedCap distinct ids
  // through dispatchResendRequest doesn't grow the table without
  // bound. Takes state_mutex_ for the read.
  // UDP_BRIDGE_BUILD_TESTING-gated for the same reason as the helpers
  // above.
  std::size_t dispatchMissWarnedIdCountForTest() const;

  // Test accessor: returns true iff `id` is currently in the
  // dispatch-miss WARN bookkeeping (i.e., the next miss for this id
  // would log at DEBUG, not WARN). Used by the FIFO-order assertion
  // in the cap-enforcement test to verify that the eviction policy
  // is in-order (oldest dropped first, not random/newest). Takes
  // state_mutex_ for the read.
  // UDP_BRIDGE_BUILD_TESTING-gated.
  bool isDispatchMissWarnedForTest(const std::string& id) const;

  // Test accessor: returns the dispatch-miss WARN bookkeeping cap.
  // Lets the cap-enforcement test assert against the exact cap value
  // (catching a "doubled cap" regression) without making
  // kDispatchMissWarnedCap part of the public API.
  // UDP_BRIDGE_BUILD_TESTING-gated.
  static constexpr std::size_t dispatchMissWarnedCapForTest()
  { return kDispatchMissWarnedCap; }

  // Test accessor: the per-topic stale-gate high-water mark, or -1 if the
  // topic has no mark yet. Lets the reorder-buffer suite assert that a
  // Buffer decision does NOT advance the mark. Takes state_mutex_.
  // UDP_BRIDGE_BUILD_TESTING-gated.
  int64_t highestPublishedForTest(const std::string& topic) const;

  // Test accessor: the packet number currently buffered for `topic`, or
  // -1 if nothing is buffered. Lets the reorder-buffer suite assert the
  // at-most-one slot contents directly. Takes state_mutex_.
  // UDP_BRIDGE_BUILD_TESTING-gated.
  int64_t bufferedPacketNumberForTest(const std::string& topic) const;
#endif  // UDP_BRIDGE_BUILD_TESTING

  // Count of missing-packet resends this RemoteNode has given up on
  // because the sender's TTL expired before the packet arrived. Read
  // by UDPBridge::sendBridgeInfo() to populate
  // Remote.msg.resend_giveup_count. Lock-friendly with
  // sendBridgeInfo's scoped_lock(subscribers_, remote_nodes_): this
  // getter takes only the recursive state_mutex_ and does NOT
  // re-acquire remote_nodes_mutex_.
  uint32_t resendGiveupCount() const;

  // Reorder-buffer observability (issue #35 review follow-up), read by
  // UDPBridge::diagnoseReorderBuffer() at the ~1 Hz diagnostic tick.
  // Lock-friendly like resendGiveupCount(): each takes only the recursive
  // state_mutex_ and does NOT re-acquire remote_nodes_mutex_.
  //   - reorderBufferOccupancy: topics currently holding a packet. At a
  //     1 Hz sample against a <=500 ms hold window this is usually 0, so
  //     the cumulative counters below are the observable signal.
  //   - reorderBufferedTotal: cumulative Buffer decisions since this
  //     RemoteNode was created.
  //   - reorderExpiredTotal: cumulative held packets released by window
  //     expiry rather than by a gap-filler — each one waited the full
  //     window and still published without its gap being filled, so a
  //     rising value means the wire is losing (or long-delaying) the
  //     gap packets, not just jittering them.
  std::size_t reorderBufferOccupancy() const;
  uint32_t reorderBufferedTotal() const;
  uint32_t reorderExpiredTotal() const;

  // Discard any held reorder-buffer packets without publishing them and
  // without touching the per-topic high-water marks. Called from
  // UDPBridge::on_deactivate: spin_once (and with it the flushExpiredBuffer
  // tick) does not run while INACTIVE, so a packet held at deactivation
  // would otherwise sit out the whole INACTIVE period and be published —
  // long stale — by the first flush tick after reactivation. The high-water
  // marks deliberately persist, matching their existing cross-activation
  // behavior; a discarded packet's gap-filler arriving post-reactivation is
  // still admitted in order by the normal admitOrBuffer rules. Takes
  // state_mutex_.
  void clearReorderBuffer();

  // Stale-packet gate (drop_stale_packets). Returns true if a message
  // for `topic` carrying wrapped sequence number `packet_number` should
  // be published, false if it is stale — i.e. a strictly-newer message
  // for the same destination topic has already been admitted. The first
  // message seen for a topic is always admitted. On admit, the per-topic
  // high-water mark is advanced to `packet_number`.
  //
  // packet_number is the sender's per-remote-node monotonic sequence, so
  // the high-water marks share this RemoteNode's number space and are
  // cleared together with received_packet_times_ on remote-restart
  // detection in update(BridgeInfo) — otherwise every post-restart packet
  // (numbers reset to 0) would be wrongly dropped as stale. Takes
  // state_mutex_.
  bool admitForPublish(const std::string& topic, uint64_t packet_number);

  // Reorder/jitter buffer (reorder_hold_window_ms, issue #35). A 3-way
  // extension of admitForPublish for the case where two near-simultaneous
  // packets arrive out of order a few ms apart: instead of hard-dropping
  // the one that arrived marginally late, hold the newer one briefly so
  // the gap-filler (if it arrives within the window) can be published
  // first, in order.
  enum class AdmitDecision
  {
    Admit,   // the incoming packet is admitted for immediate publish
    Drop,    // the incoming packet is stale (superseded) — drop it
    Buffer,  // the incoming packet opened a gap and is now held; nothing
             // to publish for it yet (it will be released either by a
             // later gap-filler or by flushExpiredBuffer on window expiry)
  };

  // Result of admitOrBuffer. `to_publish` carries the PublishItems the
  // caller must push to PublishQueue, already in wire order (a gap-filler
  // is ordered before the buffered packet it releases). It is non-empty
  // on Admit (the incoming item, plus any buffered item its arrival
  // released) and may be non-empty even when `decision` is Buffer — an
  // at-most-one overflow flushes the previously-held item while buffering
  // the new one. It is empty on Drop.
  struct AdmitResult
  {
    AdmitDecision decision;
    std::vector<PublishItem> to_publish;
  };

  // Reorder-buffer hot-path entry point, used by decodeData in place of
  // admitForPublish when reorder_hold_window_ms > 0. `item` is the
  // already-built PublishItem for the incoming packet (the payload copy
  // must happen before this call on the enabled path, since a Buffer
  // decision stores the item). `now` is injected (production passes
  // clock_->now(); tests pass fake time) and, on a Buffer decision, is
  // recorded as the entry's arrival time for the window-expiry check in
  // flushExpiredBuffer.
  //
  // Decision rules against the per-topic high-water mark H (see
  // admitForPublish) and the at-most-one buffered packet B for the topic:
  //   - P < H            -> Drop (stale; superseded by a newer publish).
  //   - first-seen / P==H / P==H+1 -> Admit (advances H; identical to
  //                          admitForPublish's admit path, which also
  //                          admits equal-to-mark — an exact-mark P is
  //                          not a duplicate, since duplicates are
  //                          filtered upstream in unwrap()).
  //   - P >= H+2         -> Buffer (gap below P; H is NOT advanced).
  // When a packet is already buffered for the topic:
  //   - P fills toward B (H<=P<B): Admit P; if that makes B contiguous
  //     (B==P+1) release B too, publishing P then B in order.
  //   - P newer than B (P>B): flush B first (at-most-one overflow),
  //     then resolve P against the advanced H.
  // The high-water mark is advanced only on actual publish, never on a
  // Buffer decision — so a gap-filler that arrives in time is still
  // admitted by the rules above. Takes state_mutex_.
  AdmitResult admitOrBuffer(const std::string& topic, uint64_t packet_number,
                            PublishItem&& item, rclcpp::Time now);

  // Release any buffered packets whose age (now - arrival) has reached
  // hold_window, advancing the per-topic high-water mark to each released
  // packet's number as it goes out. Returns the released PublishItems for
  // the caller to push to PublishQueue. Called from UDPBridge::spin_once
  // on every 10 ms drain tick (even when the link is idle) so a held
  // packet on a briefly-idle topic ages out on schedule rather than
  // stalling until the next arriving packet. `now` is injected (production
  // passes clock_->now(); tests pass fake time). Takes state_mutex_.
  std::vector<PublishItem> flushExpiredBuffer(rclcpp::Time now,
                                              rclcpp::Duration hold_window);

private:
  // Single on-arrival point: record the receive time AND clear any
  // pending resend tracking for that packet. Both `unwrap()` (the
  // production socket-drain path) and `recordReceivedPacketTimeForTest`
  // (the unit-test injection helper) route through here, so the
  // invariant "an arrived packet has no resend_state_ entry" is
  // impossible to drop from one path without dropping it from both —
  // dropping the call from `unwrap()` would also drop it from the
  // test path, surfacing the regression in the gtest suite.
  // Caller must hold state_mutex_.
  void recordPacketArrival(uint64_t packet_number, rclcpp::Time time);

  // Worker for both public `getMissingPackets()` entry points (the
  // no-arg production variant and the UDP_BRIDGE_BUILD_TESTING-gated time-
  // injection overload). Always present in the library, even when
  // UDP_BRIDGE_BUILD_TESTING is unset — keeping it private means downstream
  // consumers can't reach it through the installed header. The
  // zero-time guard lives here as the single source of truth so both
  // public wrappers stay trivial pass-throughs.
  udp_bridge_interfaces::msg::ResendRequest getMissingPacketsAt(rclcpp::Time now);

  // name of the remote udp_bridge node
  std::string name_;

  // name of the local udp_bridge node;
  std::string local_name_;

  // Guards all mutable state below: connections_, received_packet_times_,
  // resend_state_, given_up_packet_numbers_, resend_giveup_count_,
  // next_packet_number_, last_packet_time_. Under MultiThreadedExecutor,
  // public methods on RemoteNode are reachable from multiple callback
  // groups (socket-drain via decode/unwrap; periodic via bridge_info /
  // stats / diagnostic timers; service handlers in periodic group).
  //
  // recursive_mutex chosen because some public methods call each other —
  // update(BridgeInfo) calls connection() and newConnection(); unwrap()
  // calls connection() and newConnection(); getMissingPackets() calls
  // clearReceivedPacketTimesBefore(). recursive_mutex avoids the
  // boilerplate of inner non-locking helpers.
  //
  // defragmenter_ is intentionally NOT guarded by this mutex: it is
  // accessed via defragmenter() which returns a reference. Concurrency
  // contract: defragmenter_ is only touched from socket-drain callbacks
  // (UDPBridge::decode for Fragment packets and the spin_once tail
  // cleanup loop), which are mutually-exclusive within socket_drain_group_
  // by construction. If a future caller reaches defragmenter() from a
  // different group, that contract breaks; document and lock then.
  mutable std::recursive_mutex state_mutex_;

  std::map<std::string, std::shared_ptr<Connection> > connections_;

  // Ids we've already emitted a WARN for in dispatchResendRequest's
  // lookup-miss path. The first miss for an id surfaces as WARN (a
  // genuine coordinated-redeploy mismatch needs to be diagnosable);
  // subsequent misses for the same id drop to DEBUG to avoid spamming
  // during legitimate CONNECT-cycle races.
  //
  // Bounded FIFO with O(1) lookup. The ids come from network-supplied
  // ResendRequest.connection_id strings. Well-behaved peers send them
  // pre-canonicalized to the wire form (PR #25 runs every connections_
  // insertion through truncate_connection_id, so connection->id() is
  // ≤ maximum_connection_id_size - 1 and the sender stamps that), and
  // a defensive clamp in UDPBridge::decodeResendRequest enforces the
  // same shape on misbehaving peers. Per-entry size is therefore
  // bounded by 7 bytes, but the *count* of distinct ids a peer can
  // stamp isn't bounded by anything in the wire protocol. A
  // misbehaving peer or a stream of bit-flipped-but-deserializable
  // packets could otherwise grow the set unboundedly. We cap the
  // bookkeeping at kDispatchMissWarnedCap (256) entries:
  // dispatch_miss_warned_order_ holds insertion order (FIFO), and the
  // unordered_set holds the same ids for O(1) presence checks. When
  // the cap is hit, we evict the oldest id from both — that id
  // becomes WARN-eligible again the next time it's seen, which is the
  // right behavior for a rate-limit-the-WARN bookkeeping table (vs
  // permanently silencing it). The cap is comfortably above any
  // plausible configured id space (typical deployments have 2–4
  // connections, never more than a few dozen).
  //
  // Both containers are guarded by state_mutex_.
  static constexpr std::size_t kDispatchMissWarnedCap = 256;
  std::deque<std::string> dispatch_miss_warned_order_;
  std::unordered_set<std::string> dispatch_miss_warned_ids_;

  Defragmenter defragmenter_;

  std::map<uint64_t, rclcpp::Time> received_packet_times_;

  // Per-destination-topic high-water mark of the wrapped packet_number
  // last admitted for publication (the stale-packet gate; see
  // admitForPublish). Keyed on destination topic. A decoded message
  // whose packet_number is strictly below its topic's mark is a
  // late-arriving resend that a newer message superseded, so it is
  // dropped instead of republished out of order. Guarded by state_mutex_.
  // Cleared on remote-restart detection in update(BridgeInfo) alongside
  // received_packet_times_ — the sender's number space resets to 0 on
  // restart, so a stale mark would otherwise reject every fresh packet.
  std::map<std::string, uint64_t> highest_published_packet_number_;

  // Reorder/jitter buffer (reorder_hold_window_ms, issue #35). At most one
  // held packet per destination topic — the one that opened a gap above
  // the high-water mark and is waiting for the gap-filler to arrive within
  // the hold window. Each entry stores its arrival timestamp so
  // flushExpiredBuffer can age it out on an injected clock. Guarded by
  // state_mutex_. Cleared on remote-restart detection in update(BridgeInfo)
  // alongside highest_published_packet_number_ — the sender's number space
  // resets to 0 on restart, so a held packet from the previous run must not
  // linger. See admitOrBuffer / flushExpiredBuffer.
  struct ReorderBufferEntry
  {
    uint64_t packet_number = 0;
    rclcpp::Time arrival_time;
    PublishItem item;
  };
  std::map<std::string, ReorderBufferEntry> reorder_buffer_;

  // Cumulative reorder-buffer counters backing reorderBufferedTotal /
  // reorderExpiredTotal. Guarded by state_mutex_. Deliberately NOT cleared
  // on remote-restart detection (unlike reorder_buffer_ itself): they are
  // lifetime observability counters, matching resend_giveup_count_.
  uint32_t reorder_buffered_total_ = 0;
  uint32_t reorder_expired_total_ = 0;

  // Per-missing-packet state used by getMissingPackets to apply
  // exponential backoff and the TTL-bounded give-up condition (issue
  // #9 failure modes B and the give-up gap). Entries are created when
  // a packet is first observed missing and cleared by one of three
  // paths:
  //   (1) the give-up sweep at the top of getMissingPackets when
  //       first_request_time ages past kSentPacketTTL (entry moves into
  //       given_up_packet_numbers_ and bumps resend_giveup_count_),
  //   (2) recordPacketArrival when the previously-missing packet
  //       actually arrives (prevents the give-up sweep from later
  //       firing on a lingering entry for a recovered packet — see
  //       Case 6 in test_remote_node_resend.cpp), or
  //   (3) update(BridgeInfo) on remote-restart detection, which clears
  //       the whole map alongside received_packet_times_ and
  //       given_up_packet_numbers_.
  struct ResendState
  {
    rclcpp::Time first_request_time;
    rclcpp::Time last_request_time;
    uint32_t attempts = 0;
  };
  std::map<uint64_t, ResendState> resend_state_;

  // Packet numbers we have already given up on (sender TTL expired
  // before the packet arrived). Prevents the backoff loop's
  // resend_state_[m] access — which uses operator[] and would
  // default-construct a fresh entry — from re-requesting a packet we
  // already gave up on. Entries are added by the give-up sweep at the
  // top of getMissingPackets and pruned by
  // clearReceivedPacketTimesBefore when the packet number falls below
  // received_packet_times_.begin()->first (no longer in the gap-scan
  // range, so no future getMissingPackets call can re-flag it). Also
  // cleared by update(BridgeInfo)'s remote-restart detection, since
  // the remote's packet number space resets.
  std::set<uint64_t> given_up_packet_numbers_;

  // Per-remote count of resend give-ups (sender TTL expired before the
  // packet arrived). Bumped in getMissingPackets when a missing
  // packet's first-request time is older than kSentPacketTTL. Exposed
  // via resendGiveupCount(). Cumulative since RemoteNode construction;
  // survives remote-restart detection (the counter is operator-facing
  // and meant to be read as "loss across this RemoteNode's lifetime").
  // ~225 days at 220 give-ups/s before uint32 wraparound; not a
  // realistic concern for any deployment.
  uint32_t resend_giveup_count_ = 0;

  rclcpp::Publisher<udp_bridge_interfaces::msg::BridgeInfo>::SharedPtr bridge_info_publisher_;
  rclcpp::Publisher<udp_bridge_interfaces::msg::TopicStatisticsArray>::SharedPtr topic_statistics_publisher_;

  uint64_t next_packet_number_ = 0;
  rclcpp::Time last_packet_time_;

  rclcpp::Logger logger_;
  rclcpp::Clock::SharedPtr clock_;
};

}  // namespace udp_bridge

#endif
