#ifndef UDP_BRIDGE_DEFRAGMENTER_H
#define UDP_BRIDGE_DEFRAGMENTER_H

#include <ros/ros.h>
#include "packet.h"

namespace udp_bridge
{

class Defragmenter
{
  struct Fragments
  {
    uint16_t fragment_count;
    ros::Time first_arrival_time;
    std::map<uint16_t, std::vector<uint8_t> > fragment_map;
  };
public:
  /// returns true if supplied fragment completed a packet
  bool addFragment(std::vector<uint8_t> fragment);

  /// returns a list of complet packets
  std::vector<std::vector<uint8_t> > getPackets();

  /// Discard incomplete packets older than maxAge
  /// returns number of discarded packets
  int cleanup(ros::Duration maxAge);
private:
  /// map of packet id
  std::map<uint32_t, Fragments> fragment_map_;
  std::vector<std::vector<uint8_t> > complete_packets_;
};

} // namespace udp_bridge

#endif
