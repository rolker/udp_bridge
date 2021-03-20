#include "udp_bridge/defragmenter.h"

namespace udp_bridge
{

bool Defragmenter::addFragment(std::vector<uint8_t> fragment, const std::string &remote_address)
{
  Fragment* fragment_packet = reinterpret_cast<Fragment*>(fragment.data());
  if(fragment_packet->type == PacketType::Fragment)
  {
    if(m_fragment_map[remote_address].find(fragment_packet->packet_id) == m_fragment_map[remote_address].end())
    {
      m_fragment_map[remote_address][fragment_packet->packet_id].first_arrival_time = ros::Time::now();
      m_fragment_map[remote_address][fragment_packet->packet_id].fragment_count = fragment_packet->fragment_count;
    }
    m_fragment_map[remote_address][fragment_packet->packet_id].fragment_map[fragment_packet->fragment_number] = std::vector<uint8_t>(fragment.begin()+sizeof(FragmentHeader), fragment.end());
    if(m_fragment_map[remote_address][fragment_packet->packet_id].fragment_map.size() < fragment_packet->fragment_count)
      return false;
    ROS_DEBUG_STREAM("assembling packet from " << int(fragment_packet->fragment_count) << " fragments");
    m_complete_packets.push_back(std::make_pair(std::vector<uint8_t>(),remote_address));
    for(auto f: m_fragment_map[remote_address][fragment_packet->packet_id].fragment_map)
      m_complete_packets.back().first.insert(m_complete_packets.back().first.end(),f.second.begin(),f.second.end());
    m_fragment_map[remote_address].erase(fragment_packet->packet_id);
    return true;
  }
  return false;
}

std::vector<std::pair<std::vector<uint8_t>, std::string> > Defragmenter::getPackets()
{
  return std::move(m_complete_packets);
}

int Defragmenter::cleanup(ros::Duration maxAge)
{
  std::vector<std::pair<std::string, uint32_t> > discard_pile;
  ros::Time discard_time = ros::Time::now()-maxAge;
  for(auto fm: m_fragment_map)
    for(auto f: fm.second)
      if(f.second.first_arrival_time < discard_time)
        discard_pile.push_back(std::make_pair(fm.first, f.first));
  for(auto id: discard_pile)
    m_fragment_map[id.first].erase(id.second);
  return discard_pile.size();
}


}