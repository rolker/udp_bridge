#include "udp_bridge/statistics.h"

namespace udp_bridge
{

void MessageStatistics::add(const MessageSizeData& data)
{
  ROS_DEBUG_STREAM_NAMED("statistics", "msg size: " << data.message_size << " sent size: " << data.sent_size);
  for(auto result: data.send_results)
  {
    ROS_DEBUG_STREAM_NAMED("statistics", "  remote: " << result.first);
    for(auto connection: result.second)
      switch (connection.second)
      {
      case SendResult::success:
        ROS_DEBUG_STREAM_NAMED("statistics", "    " << connection.first << " send result: success");
        break;
      case SendResult::failed:
        ROS_DEBUG_STREAM_NAMED("statistics", "    " << connection.first << " send result: failed");
        break;
      case SendResult::dropped:
        ROS_DEBUG_STREAM_NAMED("statistics", "    " << connection.first << " send result: dropped");
        break;
      }
  }

  Statistics<MessageSizeData>::add(data);
}

std::vector<TopicStatistics> MessageStatistics::get()
{
  struct Totals
  {
    int total_message_size = 0;
    int total_fragment_count = 0;
    int total_ok_sent_bytes = 0;
    int total_failed_sent_bytes = 0;
    uint32_t total_dropped_bytes = 0;
    int total_data_point_count = 0;
    ros::Time earliest;
    ros::Time latest;
  };

  std::map<std::pair<std::string, std::string>, Totals> totals_by_connection;
  
  for(auto data_point: data_)
    for(auto remote_send_result: data_point.send_results)
      for(auto connection_send_result: remote_send_result.second)
      {
        Totals& totals = totals_by_connection[std::make_pair(remote_send_result.first, connection_send_result.first)];
        totals.total_message_size += data_point.message_size;
        totals.total_fragment_count += data_point.fragment_count;
        switch(connection_send_result.second)
        {
        case SendResult::success:
          totals.total_ok_sent_bytes += data_point.sent_size;
          break;
        case SendResult::failed:
          totals.total_failed_sent_bytes += data_point.sent_size;
          break;
        case SendResult::dropped:
          totals.total_dropped_bytes += data_point.sent_size;
          break;
        }
        totals.total_data_point_count++;
        if(data_point.timestamp > totals.latest)
          totals.latest = data_point.timestamp;
        if(totals.earliest.isZero() || data_point.timestamp < totals.earliest)
          totals.earliest = data_point.timestamp;
      }

  std::vector<TopicStatistics> ret;

  for(auto totals: totals_by_connection)
  {
    TopicStatistics ts;
    ts.destination_node = totals.first.first;
    ts.connection_id = totals.first.second;
    auto& data = totals.second;
    ts.stamp = data.latest;
    float count = data.total_data_point_count;
    ts.average_fragment_count = data.total_fragment_count/count;

    if(data.latest > data.earliest)
    {
      double time_span = (data.latest-data.earliest).toSec();

      // account for the time until the next sample
      time_span *= (count+1)/count;

      ts.messages_per_second = count/time_span;
      ts.message_bytes_per_second = data.total_message_size/time_span;
      ts.send.success_bytes_per_second = data.total_ok_sent_bytes/time_span;
      ts.send.failed_bytes_per_second = data.total_failed_sent_bytes/time_span;
      ts.send.dropped_bytes_per_second = data.total_dropped_bytes/time_span;
    }

    ret.push_back(ts);

  }
  
  return ret;
}

DataRates PacketSendStatistics::get() const
{
  return get(nullptr);
}

DataRates PacketSendStatistics::get(PacketSendCategory category) const
{
  return get(&category);
}

DataRates PacketSendStatistics::get(PacketSendCategory* category) const
{
  DataRates ret;
  ros::Time earliest;
  ros::Time latest;

  for(auto data_point: data_)
    if(category==nullptr || data_point.category == *category)
    {
      switch(data_point.send_result)
      {
      case SendResult::success:
        ret.success_bytes_per_second += data_point.size;
        break;
      case SendResult::failed:
        ret.failed_bytes_per_second += data_point.size;
        break;
      case SendResult::dropped:
        ret.dropped_bytes_per_second += data_point.size;
        break;
      }
      if(data_point.timestamp > latest)
        latest = data_point.timestamp;
      if(earliest.isZero() || data_point.timestamp < earliest)
        earliest = data_point.timestamp;
    }

  // If time span is less than 1 sec, assume it's 1 sec
  // to avoid data rate spikes at the start of sampling
  if(latest > earliest)
  {
    double time_span = (latest-earliest).toSec();
    if(time_span > 1.0)
    {
      ret.success_bytes_per_second /= time_span;
      ret.failed_bytes_per_second /= time_span;
      ret.dropped_bytes_per_second /= time_span;
    }
  }
  return ret;
}

bool PacketSendStatistics::can_send(uint32_t data_size, uint32_t bytes_per_second_limit, ros::Time time) const
{
  auto one_second_ago = time - ros::Duration(1.0);
  auto start = data_.begin();
  while(start != data_.end() && start->timestamp < one_second_ago)
    start++;
  uint32_t total_sent = 0;
  while(start != data_.end())
  {
    if(start->send_result != SendResult::dropped)
      total_sent += start->size;
    start++;
  }
  return (total_sent+data_size) < bytes_per_second_limit;
}


} // namespace udp_bridge
