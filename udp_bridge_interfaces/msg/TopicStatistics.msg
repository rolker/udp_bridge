builtin_interfaces/Time stamp
string source_node
string destination_node
string connection_id
string source_topic
string destination_topic

float32 messages_per_second
float32 message_bytes_per_second

# Mean number of wire fragments per SERIALIZED message in the window,
# averaged over the records that carry a fragment count rather than over
# every record (#75). Records with no count are messages that were never
# serialized for this destination — shed by a rate limit, or dropped —
# and including them as zeros dragged the average down in exactly the
# windows where fragmentation matters most. 0 means nothing in the window
# was serialized, which is also what the older divide-by-everything form
# reported for that case.
#
# A value near 1 on a large topic means fragmentation is not happening;
# test/bench/README.md's T_fragment_floor invariant is derived from this
# field.
float32 average_fragment_count

DataRates send
