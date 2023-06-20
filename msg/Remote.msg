# Describes a remote node and connections to it

# name of the node, each udp_bridge node should have a unique name
string             name

# name sanitized if necessary to be used as a topic name
string             topic_name

RemoteConnection[] connections
