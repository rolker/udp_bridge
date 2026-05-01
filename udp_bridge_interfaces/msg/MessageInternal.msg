# Internal message used to transmit a message from a specific topic
string source_topic
string destination_topic
string md5sum
string datatype
string message_definition
uint8[] data

# Per-topic QoS for the destination publisher. See udp_bridge/doc/qos_design.md.
# Adding fields to this message changes the serialized wire layout. Mixed-
# version sender/receiver interoperation is NOT guaranteed by the schema
# alone — both endpoints must be redeployed together when this message
# changes. The receiver wraps decode in a try/catch and logs+drops on
# deserialization failure rather than terminating, so a transient
# mismatch does not crash the bridge.
string reliability    # "best_available" (default), "reliable", or "best_effort"
string durability     # "volatile" (default) or "transient_local"
uint32 history_depth  # KEEP_LAST(N); 0 is treated as default 1
