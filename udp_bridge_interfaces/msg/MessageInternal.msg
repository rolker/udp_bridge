# Internal message used to transmit a message from a specific topic
string source_topic
string destination_topic
string md5sum
string datatype
string message_definition
uint8[] data

# Per-topic QoS for the destination publisher. See udp_bridge/doc/qos_design.md.
# Empty/zero values are treated as defaults so a new sender can interoperate
# with an old receiver (and vice versa) — the trailing-field-tolerance story.
string reliability    # "best_available" (default), "reliable", or "best_effort"
string durability     # "volatile" (default) or "transient_local"
uint32 history_depth  # KEEP_LAST(N); 0 is treated as default 1
