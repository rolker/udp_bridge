# List of local topics and remote connections.

# Name of the node in the udp_bridge network
string      name

# Time this info was collected/valid
time        stamp

# Local topics available
TopicInfo[] topics

# Remote nodes known to this node
Remote[]    remotes

int64       next_packet_number
time        last_packet_time

uint16      local_port
int32       maximum_packet_size
