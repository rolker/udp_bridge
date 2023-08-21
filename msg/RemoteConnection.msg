# This message is used as part of the Remote message
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

# source ip address of incoming packets
string source_ip_address

# source port of incoming packets
uint16 source_port

# maximum rate to send on this connection
uint32 maximum_bytes_per_second

# received data rate via this connection
float32 received_bytes_per_second

# rate of data received that was already received on another connection
float32 duplicate_bytes_per_second

# data rates for message packets
DataRates message

# data rates for overhead messages
DataRates overhead

# data rates for resent packets
DataRates resend

