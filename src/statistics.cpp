#include "udp_bridge/statistics.h"

namespace udp_bridge
{

void Statistics::add(const SizeData& data)
{
  if(!data.timestamp.isZero())
    data_.push_back(data);

  // only keep 10 seconds of data
  while(!data_.empty() && data_.front().timestamp < data.timestamp - ros::Duration(10.0))
    data_.pop_front();
}

std::vector<ChannelStatistics> Statistics::get()
{
  struct Totals
  {
    int total_message_size = 0;
    int total_packet_size = 0;
    int total_compressed_packet_size = 0;
    int total_fragment_count = 0;
    int total_sent_size = 0;
    int total_sent_success = 0;
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
        totals.total_compressed_packet_size += data_point.compressed_packet_size;
        totals.total_fragment_count += data_point.fragment_count;
        totals.total_sent_size += data_point.sent_size;
        if(connection_send_result.second.sent_success)
          totals.total_sent_success++;
        if(connection_send_result.second.dropped)
          totals.total_dropped_bytes += data_point.sent_size;
        totals.total_data_point_count++;
        if(data_point.timestamp > totals.latest)
          totals.latest = data_point.timestamp;
        if(totals.earliest.isZero() || data_point.timestamp < totals.earliest)
          totals.earliest = data_point.timestamp;
      }

  std::vector<ChannelStatistics> ret;

  for(auto totals: totals_by_connection)
  {
    ChannelStatistics cs;
    cs.destination_node = totals.first.first;
    cs.channel_id = totals.first.second;
    auto& data = totals.second;
    cs.stamp = data.latest;
    float count = data.total_data_point_count;
    cs.message_average_size_bytes = data.total_message_size/count;
    cs.packet_average_size_bytes = data.total_packet_size/count;
    cs.compressed_average_size_bytes = data.total_compressed_packet_size/count;
    cs.sent_average_size_bytes = data.total_sent_size/count;
    cs.average_fragment_count = data.total_fragment_count/count;
    cs.send_success_rate = data.total_sent_success/count;

    if(data.latest > data.earliest)
    {
      double time_span = (data.latest-data.earliest).toSec();

      // account for the time until the next sample
      time_span *= (count+1)/count;

      cs.messages_per_second = count/time_span;
      cs.message_bytes_per_second = data.total_message_size/time_span;
      cs.packet_bytes_per_second = data.total_packet_size/time_span;
      cs.compressed_bytes_per_second = data.total_compressed_packet_size/time_span;
      cs.sent_bytes_per_second = data.total_sent_size/time_span;
      cs.fragments_per_second = data.total_fragment_count/time_span;
      cs.dropped_bytes_per_second = data.total_dropped_bytes/time_span;
    }

    ret.push_back(cs);

  }
  
  return ret;
}

} // namespace udp_bridge
