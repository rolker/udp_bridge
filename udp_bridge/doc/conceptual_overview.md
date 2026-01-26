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

## Key Classes

### `udp_bridge::UDPBridge`
The main node class. It handles:
-   Lifecycle management (configure, activate, etc.).
-   Parameter declaration and validation.
-   Socket creation and management.
-   The main receive loop (or callback).

### `udp_bridge::Connection`
Represents a logic connection to a remote. It tracks:
-   Remote host and port.
-   Communication statistics (data rate, drop rate).
-   Connection state (good, lost).

### `udp_bridge::Defragmenter`
Handles the logic for splitting large messages into smaller UDP datagrams and reassembling them. It handles out-of-order delivery by tracking fragment indices.

### `udp_bridge::Packet` and `WrappedPacket`
These classes define the wire format. A `WrappedPacket` typically contains the raw data plus metadata (topic name, sequence number).
