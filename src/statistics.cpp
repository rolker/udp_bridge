#include "udp_bridge/statistics.h"

namespace udp_bridge
{

void Statistics::add(const SizeData& data)
{
  ROS_DEBUG_STREAM_NAMED("statistics", "msg size: " << data.message_size << " sent size: " << data.sent_size << " packet size: " << data.packet_size);
  for(auto result: data.send_results)
  {
    ROS_DEBUG_STREAM_NAMED("statistics", "  remote: " << result.first);
    for(auto connection: result.second)
      ROS_DEBUG_STREAM_NAMED("statistics", "    " << connection.first << " sent ok: " << connection.second.sent_success << " dropped: " << connection.second.dropped);
  }
  if(!data.timestamp.isZero())
    data_.push_back(data);

  // only keep 10 seconds of data
  while(!data_.empty() && data_.front().timestamp < data.timestamp - ros::Duration(10.0))
    data_.pop_front();
}

std::vector<TopicStatistics> Statistics::get()
{
  struct Totals
  {
    int total_message_size = 0;
    int total_packet_size = 0;
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
        totals.total_packet_size += data_point.packet_size;
        totals.total_fragment_count += data_point.fragment_count;
        if(connection_send_result.second.sent_success)
          totals.total_ok_sent_bytes += data_point.sent_size;
        else
          if(connection_send_result.second.dropped)
            totals.total_dropped_bytes += data_point.sent_size;
          else
            totals.total_failed_sent_bytes += data_point.sent_size;

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
      ts.packet_bytes_per_second = data.total_packet_size/time_span;
      ts.ok_sent_bytes_per_second = data.total_ok_sent_bytes/time_span;
      ts.failed_sent_bytes_per_second = data.total_failed_sent_bytes/time_span;
      ts.dropped_bytes_per_second = data.total_dropped_bytes/time_span;
    }

    ret.push_back(ts);

  }
  
  return ret;
}

} // namespace udp_bridge
