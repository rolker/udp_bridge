#ifndef UDP_BRIDGE_STATISTICS_H
#define UDP_BRIDGE_STATISTICS_H

#include <deque>
#include <ros/ros.h>
#include <udp_bridge/TopicStatistics.h>

namespace udp_bridge
{

struct SendResult
{
  bool sent_success = false;
  bool dropped = false;
};

struct SizeData
{
  int message_size = 0;
  int packet_size = 0;
  int fragment_count = 0;
  int sent_size = 0;
  ros::Time timestamp;
  std::map<std::string, std::map<std::string, SendResult> > send_results;
};

class Statistics
{
public:
  void add(const SizeData& data);

  std::vector<TopicStatistics> get();
private:
  std::deque<SizeData> data_;

};

} // namespace udp_bridge

#endif
