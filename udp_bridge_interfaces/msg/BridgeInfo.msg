# List of local topics and remote connections.

# Name of the node in the udp_bridge network
string      name

# Time this info was collected/valid
builtin_interfaces/Time        stamp

# Local topics available
TopicInfo[] topics

# Remote nodes known to this node
Remote[]    remotes

int64       next_packet_number
builtin_interfaces/Time        last_packet_time

uint16      local_port
int32       maximum_packet_size
