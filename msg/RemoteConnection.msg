# Represents a connection from a node to a remote node

# ID of the connection
string  connection_id

# host name or ip used to reach the remote node
string  host

# port to use to reach the remote node
uint16  port

# resolved ip address used to reach the remote node
string  ip_address

# return host and port may be useful when a remote udp_bridge
# node is behind a NAT

# optional host remote should use to reach us
string  return_host

# optional port remote should use to reach us
uint16  return_port

# received data rate via this connection
float32 received_bytes_per_second
