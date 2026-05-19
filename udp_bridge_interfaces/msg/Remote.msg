# Message used by BridgeInfo message and ListRemotes service.
# Describes a remote node and connections to it
#
# Schema-evolution note: appending a field here changes the ROS 2 type
# hash. Mixed-version bridges on one DDS graph briefly fail to discover
# each other on the bridge_info topic during a partial deployment; bag
# replay of old captures emits a type-hash mismatch warning. Both
# behaviors mirror the existing MessageInternal coordinated-redeploy
# expectation (see udp_bridge/doc/qos_design.md).
# Introspection-based consumers (bag_analysis/extractors/udp_bridge.py,
# rqt_udp_bridge) read the bag's own type definition and stay
# forward-compatible.

# name of the node, each udp_bridge node should have a unique name
string             name

# name sanitized if necessary to be used as a topic name
string             topic_name

RemoteConnection[] connections

# Count of missing-packet resends that this remote (as a receiver) has
# given up on because the sender's TTL (kSentPacketTTL in
# resend_constants.h) expired before the packet arrived. Gives
# operational visibility into silent loss: a non-zero value here means
# packets dropped on the wire weren't recovered within the bridge's
# resend window. Cumulative per remote since the receiver started.
uint32             resend_giveup_count
