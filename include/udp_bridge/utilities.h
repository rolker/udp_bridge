#ifndef UDP_BRIDGE_UTILITIES_H
#define UDP_BRIDGE_UTILITIES_H

#include <string>

namespace udp_bridge
{

// Converts a string to a legal ROS topic name by replacing non-legal characters with legal_char
// and prepending legal_first if necessary.
std::string topicString(const std::string& topic, bool keep_slashes = false, char legal_first = 'r', char legal_char = '_');

} // namespace udp_bridge

#endif
