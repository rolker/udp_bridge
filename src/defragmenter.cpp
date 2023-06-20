#include "udp_bridge/defragmenter.h"

namespace udp_bridge
{

bool Defragmenter::addFragment(std::vector<uint8_t> fragment)
{
  Fragment* fragment_packet = reinterpret_cast<Fragment*>(fragment.data());
  if(fragment_packet->type == PacketType::Fragment)
  {
    ROS_DEBUG_STREAM("Fragment " << int(fragment_packet->fragment_number) << " of " << int(fragment_packet->fragment_count) << " for packet " << fragment_packet->packet_id);
    if(fragment_map_.find(fragment_packet->packet_id) == fragment_map_.end())
    {
      fragment_map_[fragment_packet->packet_id].first_arrival_time = ros::Time::now();
      fragment_map_[fragment_packet->packet_id].fragment_count = fragment_packet->fragment_count;
    }
    fragment_map_[fragment_packet->packet_id].fragment_map[fragment_packet->fragment_number] = std::vector<uint8_t>(fragment.begin()+sizeof(FragmentHeader), fragment.end());
    if(fragment_map_[fragment_packet->packet_id].fragment_map.size() < fragment_packet->fragment_count)
      return false;
    ROS_DEBUG_STREAM("assembling packet from " << int(fragment_packet->fragment_count) << " fragments");
    complete_packets_.push_back(std::vector<uint8_t>());
    for(auto f: fragment_map_[fragment_packet->packet_id].fragment_map)
      complete_packets_.back().insert(complete_packets_.back().end(),f.second.begin(),f.second.end());
    fragment_map_.erase(fragment_packet->packet_id);
    return true;
  }
  else
    ROS_WARN_STREAM("Trying to add a fragment from a packet of type " << int(fragment_packet->type));
  return false;
}

std::vector<std::vector<uint8_t> > Defragmenter::getPackets()
{
  return std::move(complete_packets_);
}

int Defragmenter::cleanup(ros::Duration maxAge)
{
  ros::Time now = ros::Time::now();
  if(!now.is_zero())
  {
    std::vector<uint32_t> discard_pile;
    ros::Time discard_time = now-maxAge;
    for(auto fragments: fragment_map_)
      if(fragments.second.first_arrival_time < discard_time)
        discard_pile.push_back(fragments.first);
    for(auto id: discard_pile)
      fragment_map_.erase(id);
    return discard_pile.size();
  }
  return 0;
}

}
