# udp_bridge

![Build Status](https://img.shields.io/badge/build-unknown-gray) <!-- Update if CI exists -->

**The udp_bridge package**

## Overview
The `udp_bridge` package connects multiple ROS 2 environments over an unreliable network using UDP datagrams. The main design goal is to resume the transmission of select ROS topics as quickly as possible once a telemetry link is re-established after a lost connection.

Initial remote nodes, connections, and transmitted topics may be specified as ROS parameters. Service calls can be used to manage remotes and transmitted topics at runtime.

> **Note on Lifecycle**: Valid for ROS 2, `udp_bridge_node` is a Lifecycle Node. It starts in the `Unconfigured` state. You must transition it to `Active` for it to begin working.

## Installation

### Dependencies
-   `lifecycle_msgs`
-   `rclcpp`
-   `rclcpp_lifecycle`
-   `std_msgs`
-   `udp_bridge_interfaces`
-   `ZLIB`

### Building
```bash
colcon build --symlink-install --packages-select udp_bridge
```

## Usage

### Launch Files

#### `udp_bridge_launch.py`
Launches the node with default configuration and automatically transitions it to the `Active` state.
```bash
ros2 launch udp_bridge udp_bridge_launch.py ros_namespace:=my_robot
```

### Running Nodes
If you run the node manually, you **must** manage the lifecycle states yourself.

1.  **Start the node**:
    ```bash
    ros2 run udp_bridge udp_bridge_node --ros-args -r __ns:=/my_robot
    ```

2.  **Transition to Active** (in a separate terminal):
    ```bash
    ros2 lifecycle set /my_robot/udp_bridge configure
    ros2 lifecycle set /my_robot/udp_bridge activate
    ```

## Nodes

### udp_bridge_node
The main node that handles UDP communication.

#### Subscriptions
- Topics defined in the configuration are subscribed to dynamically.

#### Publications
| Topic | Type | Description |
|---|---|---|
| `~/bridge_info` | `udp_bridge_interfaces/msg/BridgeInfo` | Diagnostic info about bridge state. |
| `~/topic_statistics` | `udp_bridge_interfaces/msg/TopicStatisticsArray` | Traffic statistics. |
| *Dynamic* | *Various* | Dynamically publishes topics received from remotes. |

#### Parameters
The node uses a nested parameter structure to define remotes and connections. It is highly recommended to use a YAML configuration file (e.g., `config/example_params.yaml`).

To load parameters at startup:
```bash
ros2 run udp_bridge udp_bridge_node --ros-args --params-file src/udp_bridge/udp_bridge/config/example_params.yaml
```

**Parameter Reference**:

-   `name`: (string) Name used when communicating with remotes. Should be unique. Defaults to the node's name.
-   `port`: (integer) UDP port number used for listening for incoming data. Defaults to 4200.
-   `maximum_packet_size`: (integer) Maximum packet size to use to send data. Defaults to 65500.
-   `remotes_list`: (string array) List of labels for initial remote nodes.

For each remote in `remotes_list`:
-   `remotes.<remote_label>.name`: (string) Name of the remote.
-   `remotes.<remote_label>.connections_list`: (string array) List of named connections.

For each connection in `connections_list`:
-   `remotes.<remote_label>.connections.<connection_id>.host`: (string) IP or hostname.
-   `remotes.<remote_label>.connections.<connection_id>.port`: (integer) Target port.
-   `remotes.<remote_label>.connections.<connection_id>.maximum_bytes_per_second`: (int) Rate limit.
-   `remotes.<remote_label>.connections.<connection_id>.topics_list`: (string array) List of topics to sync.

For each topic in `topics_list`:
-   `source`: (string) Local topic name.
-   `destination`: (string) Remote topic name.
-   `period`: (double) Minimum period between messages (shaping/rate limiting). 0.0 = no limit.
-   `queue_size`: (int) Subscriber queue size.

#### Services
| Service | Type | Description |
|---|---|---|
| `~/add_remote` | `udp_bridge_interfaces/srv/AddRemote` | Add a new remote at runtime. |
| `~/list_remotes` | `udp_bridge_interfaces/srv/ListRemotes` | Get current list of remotes. |
| `~/remote_advertise` | `udp_bridge_interfaces/srv/Subscribe` | Request remote to send a topic (advertised on local, subscribed on remote). |
| `~/remote_subscribe` | `udp_bridge_interfaces/srv/Subscribe` | Request remote to subscribe to a topic (subscribed on local, published from remote). |

## License
BSD
