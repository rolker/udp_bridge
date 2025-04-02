# udp_bridge package overview

The udp_bridge package connects multiple ROS cores over an unreliable network using UDP datagrams. The main design goal is to resume the transmission of select ROS topics as quickly as possible once a telemetry link is re-established after a lost connection.

Initial remote nodes, connections, and transmitted topics may be specified as parameters then service calls may be used to manage romotes and transmitted topics. An rqt plugin can be used to display udp_bridge status as well as add connections and topics.

## ROS API

List of nodes:

- udp_bridge_node

### udp_bridge_node

udp_bridge_node subscribes to topics to be sent to remote udp_bridge_nodes and publishes messages received from remote udp_bridge_nodes and provides services to manage remote connections and transmitted topics.

udp_bridge_node uses names to identify remote nodes. Make sure to set the name private parameter if the node name is not unique in the network.

#### Usage

    rosrun udp_bridge udp_bridge_node

Example of running two bridge nodes on a single ROS core for testing.

    ROS_NAMESPACE=foo rosrun udp_bridge udp_bridge_node _name:=node_a
    ROS_NAMESPACE=bar rosrun udp_bridge udp_bridge_node _name:=node_b _port:=4201

    rosservice call /foo/udp_bridge_node/add_remote
    rosservice call /foo/udp_bridge_node/remote_advertise

    rostopic pub /send_topic hello
    rostopic echo /receive_topic

#### ROS topics

Publishes to:

- ~/bridge_info [udp_bridge/BridgeInfo]
- ~/topic_statistics [udp_bridge/TopicStatisticsArray]

#### ROS Parameters

Reads the following parameters from the parameter server. Lists are encoded as labeled structures to allow updating details in launch files.

- ~name: [string] name used to when communicating with remotes. Should be unique. Defaults to the node's name.
- ~port: [integer] UDP port number used for listening for incoming data. Defaults to 4200.
- ~maxPacketSize: [integer] Maximum packet size to use to send data. Defaults to 65500.
- ~remotes: [struct] List of initial remotes nodes.
  - (remote_label):
    - name: Name of the remote. Defaults to the remote label if omitted.
    - connections: Named connections to the remote.
      - (connection_id):
        - host:
        - port:
        - returnHost:
        - returnPort:
        - maximumBytesPerSecond: Data rate limit for this connection. 0 means default (500000)
        - topics:
          - (topic_label):
            - source:
            - destination:
            - period:
            - queue_size:

#### ROS services

- add_remote: [udp_bridge/AddRemote]
- list_remotes: [udp_bridge/]
- remote_advertise: [udp_bridge/]
- remote_subscribe: [udp_bridge/]

## Developer documentation

Developer documentation can be generated with rosdoc_lite.

    roscd udp_bridge
    rosdoc_lite .

The main page will generated at doc/html/index.html
