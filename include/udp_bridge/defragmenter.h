#ifndef UDP_BRIDGE_DEFRAGMENTER_H
#define UDP_BRIDGE_DEFRAGMENTER_h

#include <ros/ros.h>
#include "packet.h"

namespace udp_bridge
{

class Defragmenter
{
    struct Fragments
    {
        uint8_t fragment_count;
        ros::Time first_arrival_time;
        std::map<uint8_t, std::vector<uint8_t> > fragment_map;
    };
public:
    /// returns true if supplied fragment completed a packet
    bool addFragment(std::vector<uint8_t> fragment, const std::string &remote_address);

    /// returns a list of complet packets
    std::vector<std::pair<std::vector<uint8_t>, std::string> > getPackets();

    /// Discard incomplete packetss older than maxAge
    /// returns number of discarded packets
    int cleanup(ros::Duration maxAge);
private:
    /// map address to map of packet id
    std::map<std::string,std::map<uint32_t, Fragments> > m_fragment_map;
    std::vector<std::pair<std::vector<uint8_t>, std::string> > m_complete_packets;
};

} // namespace udp_bridge

#endif
