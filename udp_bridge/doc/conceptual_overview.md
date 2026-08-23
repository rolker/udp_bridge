# Conceptual Overview

The `udp_bridge` package is designed to bridge ROS 2 topics across unreliable network links using UDP. Unlike standard ROS 2 transport (DDS) which can be heavy or unreliable over poor links, `udp_bridge` allows for explicit control over what is sent, at what rate, and using a lightweight UDP packet structure.

## Architecture

The core of the package is the `udp_bridge::UDPBridge` class, which runs as a `rclcpp_lifecycle::LifecycleNode`.

### Data Flow

1.  **Subscription**: The node subscribes to local ROS 2 topics specified in the configuration.
2.  **Serialization**: Received messages are serialized into a byte buffer.
3.  **Packet Construction**:
    -   The serialized data is wrapped in a `udp_bridge::Packet`.
    -   If the message is larger than `maximum_packet_size`, it is passed to the `udp_bridge::Defragmenter` (conceptually a "Fragmenter" in this direction) to be split into multiple `udp_bridge::Fragment` packets.
4.  **Transmission**: Packets are sent via the `send()` method to all configured remotes using a UDP socket.

### Reception

1.  **Listening**: The node listens on a configured UDP port.
2.  **Packet Decoding**: Incoming bytes are cast to a `udp_bridge::PacketHeader` to determine the type.
3.  **Reassembly**:
    -   If the packet is a fragment, it is stored in the `udp_bridge::Defragmenter`.
    -   Once all fragments for a message ID are received, the original packet is reconstructed.
4.  **Deserialization**: The payload is deserialized back into a ROS 2 message.
5.  **Publication**: The message is published to the local ROS 2 topic mapped to the remote source.
6.  **Relay**: Every message admitted for publication is also offered to every *other* remote whose `topics_list` carries the same topic, and forwarded to those that do. A message the stale-packet gate or the reorder buffer drops is not relayed either. See below.

### Relay (remote → remote)

A bridge is not limited to two parties. When a message arrives from remote A on a topic that remote B also asked for, the bridge forwards it to B as well as publishing it locally. That is what makes a hub topology work — `boat → hub → operators` — without running a second bridge process on the hub to re-publish everything through ROS.

Three properties define the behaviour:

-   **The `topics_list` is the routing table.** There is no relay parameter. If a remote lists a topic, it receives that topic, whether the traffic originated locally or on another remote.
-   **A message is never sent back to the remote it came from** (the loop rule). Because the bridge only knows the *last hop*, this protects against direct echo but not against cycles: configure star topologies only. `A → B → C → A` would circulate indefinitely.
-   **Only admitted messages are relayed.** Relay follows the stale-packet / reorder gate, not precedes it: forwarding assigns fresh sequence numbers, so a downstream bridge cannot tell a relayed stale packet from fresh data and would republish older data as newest.
-   **Forwarding happens on its own worker thread**, never on the socket-drain thread, and goes out through the normal send path — so fragmenting, sequencing, resends and the per-connection admission control apply to relayed traffic exactly as to locally published traffic.

Full rationale, including why there is no relay flag, why relay follows the stale-packet gate, and how a hub widens the blast radius of the unauthenticated transport, is in [`relay_design.md`](relay_design.md).

## Key Classes

### `udp_bridge::UDPBridge`
The main node class. It handles:
-   Lifecycle management (configure, activate, etc.).
-   Parameter declaration and validation.
-   Socket creation and management.
-   The main receive loop (or callback).

### `udp_bridge::Connection`
Represents a logical connection to a remote. It tracks:
-   Remote host and port.
-   Communication statistics (data rate, drop rate).
-   Connection state (good, lost).

### `udp_bridge::Defragmenter`
Handles the logic for splitting large messages into smaller UDP datagrams and reassembling them. It handles out-of-order delivery by tracking fragment indices.

### `udp_bridge::Packet` and `WrappedPacket`
These classes define the wire format. A `WrappedPacket` typically contains the raw data plus metadata (topic name, sequence number).
