# This is an internal message is used to manage
# connections between nodes

# ID of the connection
string  connection_id

# increasing number used to match acknowlegements
# with reqests as well as detecting out of order requests
uint64 sequence_number

uint8 OPERATION_CONNECT                = 0
uint8 OPERATION_CONNECT_ACKNOWLEDGE    = 1
uint8 OPERATION_DISCONNECT             = 2
uint8 OPERATION_DISCONNECT_ACKNOWLEDGE = 3

uint8 operation

# return host and port may be useful when a remote udp_bridge
# node is behind a NAT

# optional host remote should use to reach us
string  return_host

# optional port remote should use to reach us
uint16  return_port
