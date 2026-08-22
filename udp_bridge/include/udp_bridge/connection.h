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

  /// Set the maximum fraction [0, 1] of the rate limit that resend
  /// traffic may consume (issue #44). Values outside [0, 1] are
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

  /// Set the fraction [0, 1) of measured goodput deliberately left
  /// unused for co-tenant traffic (issue #52). Values outside the range
  /// are clamped; NaN falls back to the default. See
  /// kDefaultLinkHeadroomFraction for why a headroom target and not
  /// only an absolute ceiling.
  void setLinkHeadroomFraction(float fraction);
  float linkHeadroomFraction() const;

  /// Most recent goodput estimate for this connection, bytes/second:
  /// the remote's reported received rate minus its reported duplicate
  /// rate, from the last updateAdmissionControl() sample. 0.0 before
  /// any sample. This is the only quantity in the connection that
  /// describes the physical link, so it is what the admission floor's
  /// congested branch and the resend budget are measured against.
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
  /// decrease to the lower of a multiplicative halving and the headroom
  /// target (1 - link_headroom_fraction_) * goodput, floored at
  /// admission_floor_bytes_per_second_; on clean feedback recover by a
  /// step relative to the CURRENT effective cap.
  ///
  /// The headroom target is applied only on the congested branch.
  /// Measured goodput is bounded above by offered load, so it is a
  /// lower bound on capacity and never an estimate of it — clamping to
  /// it unconditionally would ratchet a healthy, lightly-loaded link
  /// down to nothing.
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
#endif  // UDP_BRIDGE_BUILD_TESTING

private:
  // Caller must hold config_mutex_; resolveHost mutates addresses_ and
  // ip_address_. Called from the constructor and from setHostAndPort.
  void resolveHost();

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

  /// Maximum fraction [0, 1] of data_rate_limit_ that resend traffic
  /// may consume (issue #44); scaled down further under ack starvation
  /// — see resend_packets(). Guarded by config_mutex_ like the other
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

  /// Fraction of measured goodput left unused for co-tenant traffic
  /// (issue #52). Guarded by config_mutex_.
  float link_headroom_fraction_ = kDefaultLinkHeadroomFraction;

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
