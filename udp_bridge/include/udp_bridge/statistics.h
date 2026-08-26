#ifndef UDP_BRIDGE_STATISTICS_H
#define UDP_BRIDGE_STATISTICS_H

#include <deque>
#include <map>
#include "rclcpp/time.hpp"
#include "udp_bridge_interfaces/msg/topic_statistics.hpp"
#include "udp_bridge_interfaces/msg/data_rates.hpp"

namespace udp_bridge
{

enum struct SendResult 
{
  success,
  failed, 
  dropped, ///< dropped due to rate limit
};

/// Data sizes used for calculating message data rates
struct MessageSizeData
{
  /// Size of message data before compression and packing into one or more packets
  int message_size = 0;

  /// Number of fragments if the packet had to be split to respect max packet size. 1 if the packet didn't need to be split.
  int fragment_count = 0;

  /// Number of bytes attempted to be sent.
  int sent_size = 0;

  rclcpp::Time timestamp;
  std::map<std::string, std::map<std::string, SendResult> > send_results;
};


enum struct PacketSendCategory
{
  message,
  overhead,
  resend
};

struct PacketSizeData
{
  rclcpp::Time timestamp;
  u_int16_t size;
  PacketSendCategory category;
  SendResult send_result;
};

template<typename T> class Statistics
{
public:
  void add(const T& data)
  {
    if(data.timestamp.nanoseconds() != 0)
      data_.push_back(data);

    // only keep 10 seconds of data
    while(!data_.empty() && data_.front().timestamp < data.timestamp - rclcpp::Duration::from_seconds(10.0))
      data_.pop_front();
  }
protected:
  std::deque<T> data_;
};

class MessageStatistics: public Statistics<MessageSizeData>
{
public:
  void add(const MessageSizeData& data);

  std::vector<udp_bridge_interfaces::msg::TopicStatistics> get();
};


class PacketSendStatistics: public Statistics<PacketSizeData>
{
public:
  udp_bridge_interfaces::msg::DataRates get() const;
  udp_bridge_interfaces::msg::DataRates get(PacketSendCategory category) const;

  /// @param data_size the prospective packet's byte count
  /// @param reserved_bytes bytes already reserved by other in-flight
  ///        Connection::send() calls that have not yet finalized their
  ///        records (see Connection::reserved_bytes_in_flight_). Pass 0
  ///        for callers that don't use the reservation pattern.
  bool can_send(uint32_t data_size, uint32_t reserved_bytes, uint32_t bytes_per_second_limit, rclcpp::Time time) const;

  /// Sum of non-dropped bytes recorded for `category` in the strict
  /// 1-second window ending at `time` — the same window/scan semantics
  /// as can_send (full-deque scan; dropped entries excluded). Used by
  /// the resend budget (issue #44), which must react within the same
  /// window the rate limiter uses: the 0-10 s smoothed get() rate lags
  /// a sustained burst as the deque's time span grows, which would
  /// under-shed in exactly the sustained-storm mode the budget exists
  /// to stop.
  uint64_t bytes_in_window(PacketSendCategory category, rclcpp::Time time) const;

  /// Successfully-sent bytes per second across ALL categories, measured
  /// over the `window_seconds` window ending at `time` (issue #52).
  ///
  /// Deliberately shaped like Connection::data_receive_rate rather than
  /// like get(): same fixed window, same `dt = max(1.0, time - oldest
  /// sample in window)` divisor. The admission controller compares this
  /// against the remote's echoed receive rate, which is produced by
  /// exactly that filter on the far end; get()'s variable 1-10 s span
  /// cannot be matched to it at any tuning, and the mismatch is what
  /// made 16.1% of field samples report the remote receiving MORE than
  /// we sent. Use this, not get(), for anything that is compared against
  /// a figure measured by the other end.
  float success_rate_in_window(rclcpp::Time time, double window_seconds) const;

private:
  udp_bridge_interfaces::msg::DataRates get(PacketSendCategory *category) const;

};

} // namespace udp_bridge

#endif
