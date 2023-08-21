#ifndef UDP_BRIDGE_STATISTICS_H
#define UDP_BRIDGE_STATISTICS_H

#include <deque>
#include <ros/ros.h>
#include <udp_bridge/TopicStatistics.h>
#include <udp_bridge/DataRates.h>

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

  ros::Time timestamp;
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
  ros::Time timestamp;
  u_int16_t size;
  PacketSendCategory category;
  SendResult send_result;
};

template<typename T> class Statistics
{
public:
  void add(const T& data)
  {
    if(!data.timestamp.isZero())
      data_.push_back(data);

    // only keep 10 seconds of data
    while(!data_.empty() && data_.front().timestamp < data.timestamp - ros::Duration(10.0))
      data_.pop_front();
  }
protected:
  std::deque<T> data_;
};

class MessageStatistics: public Statistics<MessageSizeData>
{
public:
  void add(const MessageSizeData& data);

  std::vector<TopicStatistics> get();
};


class PacketSendStatistics: public Statistics<PacketSizeData>
{
public:
  DataRates get() const;
  DataRates get(PacketSendCategory category) const;

  bool can_send(uint32_t data_size, uint32_t bytes_per_second_limit, ros::Time time) const;

private:
  DataRates get(PacketSendCategory *category) const;

};

} // namespace udp_bridge

#endif
