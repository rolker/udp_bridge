#ifndef UDP_BRIDGE_CONNECTION_H
#define UDP_BRIDGE_CONNECTION_H

//#include <ros/ros.h>
#include <vector>
#include <cstdint>
#include <string>
#include <memory>
#include <list>
#include <map>
#include <mutex>
#include <netinet/in.h>
#include <udp_bridge/packet.h>
#include "udp_bridge/resend_constants.h"
#include "udp_bridge/types.h"
#include "udp_bridge/wrapped_packet.h"
#include "udp_bridge_interfaces/msg/data_rates.hpp"

namespace udp_bridge
{

class ConnectionException
{
  public:
    ConnectionException(const std::string &msg):msg_(msg) {}
    const std::string & getMessage() const {return msg_;}
  private:
    std::string msg_;
};

/// Represents a connection to a remote node.
/// Multiple connections to a remote node may be used for redundancy.
class Connection
{
public:
  Connection(std::string id, std::string const &host, uint16_t port, std::string return_host=std::string(), uint16_t return_port=0);

  void setHostAndPort(const std::string& host, uint16_t port);
  void setRateLimit(uint32_t maximum_bytes_per_second);
  uint32_t rateLimit() const;

  /// Set the maximum fraction [0, 1] of measured goodput that resend
  /// traffic may consume (issue #44; rebased from the configured rate
  /// limit to goodput by issue #52). Values outside [0, 1] are
  /// clamped. See resend_constants.h for the default and the
  /// ack-starvation backoff that scales this down further.
  void setResendBudgetFraction(float fraction);
  float resendBudgetFraction() const;

  /// Set the minimum effective admission cap in absolute bytes per
  /// second (issue #52 — it was a fraction of the configured rate
  /// limit under #43, which made the floor scale with a number that
  /// does not describe the link). Negative values and NaN fall back to
  /// the default; the floor is additionally clamped to the configured
  /// rate limit at use, so a floor above the cap cannot raise it.
  void setAdmissionFloorBytesPerSecond(float bytes_per_second);
  float admissionFloorBytesPerSecond() const;

  // NOTE: setLinkHeadroomFraction / linkHeadroomFraction are GONE
  // (issue #52). The `(1 - fraction) x goodput` clamp on the congested
  // backoff was their only call site and was removed on 2026-08-25;
  // keeping a setter that stored a value nothing read left the
  // connection able to be "configured" into no change at all. The
  // PARAMETER is still declared at the node — see
  // kDefaultLinkHeadroomFraction and doc/admission_control_design.md,
  // "link_headroom_fraction after the clamp removal" — but it reaches no
  // Connection state.

  /// Set the minimum interval, in seconds, between admission-control
  /// decisions (issue #52). While the refractory window is open the
  /// controller freezes: no decrease AND no additive recovery, whatever
  /// the arriving sample says. Negative values and NaN fall back to
  /// kDefaultAdmissionRefractoryPeriodSeconds; values above
  /// kMaximumAdmissionRefractoryPeriodSeconds (including +Inf) clamp
  /// DOWN to it, so no configuration can freeze the controller
  /// permanently; 0 disables the gate (every sample is acted on — the
  /// pre-#52 behaviour that produced the 2026-08-25 collapse, available
  /// only because an operator may need to reproduce it).
  ///
  /// Setting this also resets the currently-open window and any
  /// accumulated exponential growth, so a reconfiguration cannot leave
  /// the controller frozen under the old value.
  void setAdmissionRefractoryPeriodSeconds(double seconds);
  double admissionRefractoryPeriodSeconds() const;

  /// The refractory window currently in force, in seconds — the base
  /// period grown by kAdmissionRefractoryGrowthFactor for each
  /// successive decrease within an unresolved congestion episode, capped
  /// at base x kAdmissionRefractoryMaximumMultiple, and reset to the base
  /// by the first clean sample the controller was free to act on.
  ///
  /// Exposed for TESTS. It has no diagnostic consumer yet: nothing
  /// publishes it, so a frozen controller and a healthy flat cap look
  /// identical in RemoteConnection.msg. Surfacing the in-force window is
  /// a candidate for #76, which is already reasoning about the schema.
  double currentAdmissionRefractoryWindowSeconds() const;

  /// Most recent goodput estimate for this connection, bytes/second:
  /// the remote's reported received rate minus its reported duplicate
  /// rate, from the last updateAdmissionControl() sample. 0.0 before
  /// any sample. This is the only quantity in the connection that
  /// describes the physical link. Since the 2026-08-25 clamp removal the
  /// admission decrease no longer reads it — the congested branch is a
  /// plain halving of the current cap — so its remaining consumer is the
  /// resend budget (doc/resend_budget_design.md).
  float goodputBytesPerSecond() const;

  /// The AIMD-adjusted admission cap in bytes/second — what send()
  /// actually meters against. Starts equal to the configured rate
  /// limit; halves on congestion, recovers additively when clean.
  uint32_t effectiveRateLimit() const;

  /// Feed one delivered-vs-sent feedback sample (issue #43, rescaled by
  /// #52). Called on receipt of the remote's BridgeInfo with that
  /// remote's received_bytes_per_second and duplicate_bytes_per_second
  /// for this connection; goodput is their difference, since duplicates
  /// and resends inflate the received figure exactly when amplification
  /// is worst.
  ///
  /// Applies AIMD to the effective rate limit: on congestion (delivery
  /// below (1 - kAdmissionLossThreshold) of our sent rate, or this
  /// connection's own feedback stale for kAckStarvationThreshold)
  /// decrease PURELY multiplicatively from the current cap —
  /// max(floor, effective_rate_limit_ * kAdmissionDecreaseFactor); on
  /// clean feedback recover by a step relative to the CURRENT effective
  /// cap.
  ///
  /// The decrease no longer targets (1 - link_headroom_fraction) *
  /// goodput (issue #52, 2026-08-25). Goodput is depressed by the very
  /// throttling the target was computing — a positive feedback loop that
  /// drove the recorded field onset below the regression bound on its
  /// FIRST step, at any refractory tuning. The controller now backs off
  /// from its own last known-good state instead of from a measurement it
  /// contaminated. The headroom fraction consequently has no call site
  /// in the control law and no longer exists as Connection state; see
  /// resend_constants.h and doc/admission_control_design.md.
  ///
  /// REFRACTORY GATE (issue #52). At most one decision per congestion
  /// epoch: while the window opened by the last decrease is still open,
  /// the sample is recorded (goodput is still updated, it is a
  /// measurement) but the cap is left exactly as-is — no decrease, and
  /// no additive recovery either. The window grows exponentially on
  /// repeated decreases within an unresolved episode and resets to the
  /// base on the first clean sample. Without it, one marginal sample
  /// became seven halvings in 12 s on 2026-08-25.
  ///
  /// The sent side of the comparison is measured over
  /// kAdmissionSendRateWindowSeconds — the same window the remote uses
  /// to produce the figure it echoes back — rather than over
  /// PacketSendStatistics::get()'s variable 1-10 s span.
  ///
  /// Reads the stats and receive-history mutexes before config_mutex_ —
  /// never nested.
  void updateAdmissionControl(float remote_received_bps,
                              float remote_duplicate_bps, rclcpp::Time now);

  std::string str() const;

  const std::string &id() const;

  // Used to tell the remote host the address to get back to us.
  // Returns by value (config_mutex_ guard would otherwise dangle a reference).
  std::string returnHost() const;
  uint16_t returnPort() const;
  void setReturnHostAndPort(const std::string &return_host, uint16_t return_port);

  // IP address and port from incoming packets. May differ from where packets are sent depending
  // on network architecture. Returns by value (config_mutex_ guard).
  std::string sourceIPAddress() const;
  uint16_t sourcePort() const;
  void setSourceIPAndPort(const std::string &source_ip, uint16_t source_port);


  // host(), port(), ip_address(), ip_address_with_port() return by value
  // because the underlying fields are guarded by config_mutex_; a
  // reference would dangle after lock release.
  std::string host() const;
  uint16_t port() const;
  std::string ip_address() const;
  std::string ip_address_with_port() const;

  // Note: a previous socket_address() returning `const sockaddr_in*`
  // into the addresses_ vector was removed. That pointer would dangle
  // if a concurrent setHostAndPort() → resolveHost() cleared the
  // vector. There were no callers, and Connection::send() now snapshots
  // the destination address internally under config_mutex_.

  /// Returns the time (seconds since epoch) the last packet was received
  /// on this connection, or 0.0 if no packet has been received yet.
  /// Returns by value (not reference) because the underlying field is
  /// guarded by a mutex; a reference would dangle after lock release.
  double last_receive_time() const;
  void update_last_receive_time(double t, int data_size, bool duplicate);

  void resend_packets(const std::vector<uint64_t> &missing_packets, int socket, rclcpp::Time now);

  SendResult send(const std::vector<WrappedPacket>& packets, int socket, const std::string& remote, bool is_overhead, rclcpp::Time now);

  PacketSizeData send(const std::vector<uint8_t> &data, int socket, PacketSendCategory category, rclcpp::Time now);

  //bool can_send(uint32_t byte_count, double time);

  /// Returns the average received data rate at the specified time.
  /// The first element of the returned pair is for unique bytes/second and the second
  /// element is for duplicated bytes per second. Duplicated bytes are for packets
  /// that had already been received on another connection.
  std::pair<double, double>  data_receive_rate(double time);

  /// Returns the average data rate.
  udp_bridge_interfaces::msg::DataRates data_sent_rate(rclcpp::Time time, PacketSendCategory category);

  /// Returns the average data rate across all packet categories.
  udp_bridge_interfaces::msg::DataRates data_sent_rate(rclcpp::Time time);

  /// Remove saved sent packets older than cutoff_time.
  void cleanup_sent_packets(rclcpp::Time cutoff_time);

#ifdef UDP_BRIDGE_BUILD_TESTING
  /// Test helper: inject a sent-packet entry at a caller-supplied
  /// timestamp without going through the send() path. Used by the
  /// gtest suite to seed sent_packets_ for cleanup-boundary tests and
  /// (with a non-zero payload_size) for resend-budget tests.
  /// Not part of any production code path. `packet_number` and
  /// `timestamp` are populated; the WrappedPacket's byte vector is
  /// filled with `payload_size` zero bytes (default 0 = empty, the
  /// original cleanup-test behavior). Tests that exercise
  /// `resend_packets` on the seeded data must pass a non-zero
  /// payload_size so the resent packets have realistic sizes.
  ///
  /// Gated behind UDP_BRIDGE_BUILD_TESTING so it doesn't leak into
  /// the package's installed public ABI — CMakeLists installs
  /// `include/` and defines the package-namespaced macro privately
  /// only when CMake's BUILD_TESTING variable is on, so downstream
  /// consumers don't see the declaration. The macro is namespaced
  /// (rather than the generic `BUILD_TESTING`) to avoid an ODR hazard
  /// with downstream packages that define `BUILD_TESTING` for their
  /// own tests.
  void record_sent_packet_for_test(uint64_t packet_number, rclcpp::Time timestamp,
                                   std::size_t payload_size = 0);

  /// Test accessor: returns sent_packets_.size() under the buffer's
  /// own mutex. Used by the gtest cleanup-boundary test to verify
  /// what evicted vs what stayed. Not for production callers.
  /// UDP_BRIDGE_BUILD_TESTING-gated for the same reason as the helper above.
  std::size_t sent_packet_count_for_test() const;

  /// Test accessor: returns how many times resend_packets has been
  /// invoked on this Connection since construction. Used by the
  /// dispatch-routing tests in test_remote_node_resend.cpp to assert
  /// that a stamped ResendRequest reaches only the matching connection
  /// (issue #23). Incremented at entry of resend_packets (before any
  /// network I/O), so an empty missing-packet list still counts as a
  /// call. UDP_BRIDGE_BUILD_TESTING-gated for the same reason as the
  /// helpers above.
  std::size_t resend_call_count_for_test() const;

  /// Test accessor: bytes currently reserved by in-flight send() calls
  /// (reserved_bytes_in_flight_), read under its own mutex. The
  /// message-atomic aggregate reservation added for issue #52 has four
  /// ways a fragment's inner send can end — success, ECONNREFUSED, a
  /// mid-loop no_address after a concurrent setHostAndPort, and a thrown
  /// ConnectionException — and every one of them must release exactly
  /// that fragment's bytes. A leak here is invisible in behaviour until
  /// the connection quietly stops admitting anything, so the tests assert
  /// the counter itself rather than a downstream symptom.
  /// UDP_BRIDGE_BUILD_TESTING-gated for the same reason as the helpers
  /// above.
  uint32_t reserved_bytes_in_flight_for_test() const;
#endif  // UDP_BRIDGE_BUILD_TESTING

private:
  // Caller must hold config_mutex_; resolveHost mutates addresses_ and
  // ip_address_. Called from the constructor and from setHostAndPort.
  void resolveHost();

  /// Implementation behind the public single-packet send(). When
  /// `bytes_pre_reserved` is true the caller has ALREADY counted
  /// data.size() into reserved_bytes_in_flight_ (the batch overload's
  /// message-atomic aggregate reservation, issue #52) and has handed
  /// ownership of those bytes to this call: the check-and-reserve step
  /// is skipped, but EVERY exit path still releases exactly
  /// data.size() — success, ECONNREFUSED, no_address, and the throw
  /// path via ReservationGuard. The public overload passes false and
  /// does its own check-and-reserve, unchanged.
  PacketSizeData sendPacket(const std::vector<uint8_t>& data, int socket,
                            PacketSendCategory category, rclcpp::Time now,
                            bool bytes_pre_reserved);

  /// connection_id of the connection
  std::string id_;

  // The fields below are guarded by config_mutex_ — written by
  // setHostAndPort / setReturnHostAndPort / setSourceIPAndPort /
  // setRateLimit (reachable from socket-drain via decode handlers AND
  // from periodic via the addRemote service handler), read by getters
  // and by Connection::send. Without the mutex, a concurrent
  // resolveHost() could clear+repopulate addresses_ while
  // Connection::send was reading addresses_.data() to hand to sendto()
  // — a use-after-free hazard.
  //
  // recursive_mutex chosen because Connection's ctor + setHostAndPort
  // call resolveHost() while already holding the lock.
  mutable std::recursive_mutex config_mutex_;
  std::string host_;
  std::string ip_address_; ///< resolved ip address
  uint16_t port_;

  std::vector<sockaddr_in> addresses_;

  /// Used by the remote to refer to us. Useful if they are behind a nat
  std::string return_host_;
  uint16_t return_port_ = 0;

  std::string source_ip_address_; ///< source ip address of incoming packets from remote
  uint16_t source_port_ = 0; ///< source port of incoming packets from remote

  /// Time in seconds since epoch that last packet was received
  double last_receive_time_ = 0.0;

  static constexpr uint32_t default_rate_limit = 50000;

  /// Maximum bytes per second to send.
  uint32_t data_rate_limit_ = default_rate_limit;

  /// Maximum fraction [0, 1] of measured goodput that resend traffic
  /// may consume (issue #44; rebased from data_rate_limit_ to goodput by
  /// issue #52); scaled down further under ack starvation — see
  /// resend_packets(). Guarded by config_mutex_ like the other
  /// setter-writable config fields above.
  float resend_budget_fraction_ = kDefaultResendBudgetFraction;

  /// AIMD-adjusted admission cap (issue #43): what send() meters
  /// against. Reduced by updateAdmissionControl on congestion, recovered
  /// additively when clean, floored at admission_floor_bytes_per_second_
  /// and ceilinged at data_rate_limit_. setRateLimit
  /// clamps it to min(effective, new limit) — never resets — so a
  /// CONNECT flap re-applying an unchanged limit cannot wipe
  /// accumulated backoff. Guarded by config_mutex_. Float (not
  /// uint32_t) so repeated halvings and fractional recovery steps
  /// don't accumulate rounding error.
  float effective_rate_limit_ = default_rate_limit;

  /// Minimum effective cap in absolute bytes/second (issue #52).
  /// Guarded by config_mutex_.
  float admission_floor_bytes_per_second_ = kDefaultAdmissionFloorBytesPerSecond;

  /// True while a refractory window opened by an admission DECREASE is
  /// still outstanding — i.e. the episode has not been resolved by a
  /// clean sample the controller was free to act on. Cleared by that
  /// clean sample and by setAdmissionRefractoryPeriodSeconds.
  ///
  /// This is a separate flag rather than `last_admission_decrease_time_
  /// > 0.0` on purpose (#52, review round 1): 0.0 is a legal clock
  /// value. Under `use_sim_time` before the first `/clock`, or a bag
  /// replayed from t=0, the sentinel form read a real decrease as "no
  /// decrease outstanding" and left the gate inert for exactly the
  /// samples that matter. Guarded by config_mutex_.
  bool admission_decrease_outstanding_ = false;

  /// Time (seconds, from the clock updateAdmissionControl is called
  /// with) of the most recent admission DECREASE. Only meaningful while
  /// admission_decrease_outstanding_ is true — read the flag, never this
  /// value, to ask whether a window is open. Guarded by config_mutex_.
  double last_admission_decrease_time_ = 0.0;

  /// Configured base refractory period, seconds (issue #52). Guarded by
  /// config_mutex_.
  double admission_refractory_period_seconds_ =
    kDefaultAdmissionRefractoryPeriodSeconds;

  /// The window actually in force after the most recent decrease: the
  /// base, grown by kAdmissionRefractoryGrowthFactor once per successive
  /// decrease within an unresolved episode and capped at
  /// base x kAdmissionRefractoryMaximumMultiple. Reset to the base by a
  /// clean sample and by setAdmissionRefractoryPeriodSeconds. Guarded by
  /// config_mutex_.
  double admission_refractory_current_seconds_ =
    kDefaultAdmissionRefractoryPeriodSeconds;

  /// Latest goodput estimate (remote received minus remote duplicates),
  /// bytes/second, from the last updateAdmissionControl() sample.
  /// Guarded by config_mutex_.
  float goodput_bytes_per_second_ = 0.0f;

  /// Info about a received packet useful for data rate statistics.
  struct ReceivedSize
  {
    /// size in bytes of a received packet
    uint16_t size = 0;

    /// indicates if this is a duplicate packet that has already been seen on a
    /// different channel.
    bool duplicate = false;
  };
  std::map<double, std::vector<ReceivedSize> > data_size_received_history_;
  /// Guards data_size_received_history_ and last_receive_time_. Written by
  /// update_last_receive_time on the receive (socket-drain) path; read by
  /// data_receive_rate / last_receive_time on the periodic-stats path.
  mutable std::mutex receive_history_mutex_;

  /// History of recently sent packets.
  /// Each connection keeps a buffer of sent packets in case a resend is needed.
  /// The same data packets are replicated for each connection to account for
  /// different source_node or connection_id.
  std::map<uint64_t, WrappedPacket> sent_packets_;
  /// Guards sent_packets_. Touched by send (writes), resend_packets
  /// (reads), and cleanup_sent_packets (writes), which can be invoked
  /// from different callback groups under MultiThreadedExecutor.
  mutable std::mutex sent_packets_mutex_;

#ifdef UDP_BRIDGE_BUILD_TESTING
  /// Test-only counter: number of times resend_packets() has been
  /// invoked. Bumped at the top of resend_packets, before any I/O,
  /// under sent_packets_mutex_ so the increment piggybacks on the
  /// lock that resend_packets already takes (no separate atomic).
  /// Read via resend_call_count_for_test() under the same mutex.
  /// Gated behind UDP_BRIDGE_BUILD_TESTING so it doesn't change the
  /// production object layout.
  std::size_t resend_call_count_for_test_ = 0;
#endif  // UDP_BRIDGE_BUILD_TESTING

  PacketSendStatistics sent_packet_statistics_;
  /// Guards sent_packet_statistics_ and reserved_bytes_in_flight_.
  /// Updated on every send (republish or socket-drain group) and read
  /// by stats / diagnostic timers (periodic group). The mutex is held
  /// only for the bookkeeping operations (check + reserve, or release +
  /// add); the blocking sendto() runs outside it via the reserve-then-
  /// record pattern in Connection::send.
  mutable std::mutex sent_packet_statistics_mutex_;

  /// Bytes reserved by in-flight Connection::send() calls that have
  /// passed the can_send check but not yet finalized their record. Read
  /// by can_send so concurrent senders see each other's pending
  /// reservations and cannot collectively exceed maximum_bytes_per_second.
  /// Guarded by sent_packet_statistics_mutex_.
  uint32_t reserved_bytes_in_flight_ = 0;
};


std::string addressToDotted(const sockaddr_in &address);

} // namespace udp_bridge

#endif
