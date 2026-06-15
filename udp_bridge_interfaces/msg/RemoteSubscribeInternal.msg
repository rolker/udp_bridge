# Internal message used to request (or cancel) a topic from a remote over a
# specific connection.
string  source_topic
string  destination_topic
uint32  queue_size
float32 period
string  connection_id

# Operation: subscribe (set up the remote's push to us) or unsubscribe (tear it
# down). Same-version peers default to 0 (subscribe), so the common path is
# unchanged. NOTE: appending this field is a wire-format change, not a
# backward-compatible one. A new receiver deserializing a pre-operation (old)
# subscribe packet throws on the missing trailing byte; udp_bridge's decode()
# catch-all logs and drops it (recoverable, not a crash and not silently
# misread). Deploy the bridge fleet together rather than mixing versions across
# a link.
uint8 OPERATION_SUBSCRIBE   = 0
uint8 OPERATION_UNSUBSCRIBE = 1
uint8 operation
