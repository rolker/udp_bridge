#ifndef UDP_BRIDGE_UDP_BRIDGE_H
#define UDP_BRIDGE_UDP_BRIDGE_H

// C system headers
#include <netinet/in.h>

// C++ standard library
#include <atomic>
#include <mutex>
#include <set>

// Other library / project includes
#include "rclcpp_lifecycle/lifecycle_node.hpp"
#include "rclcpp/generic_publisher.hpp"
#include "rclcpp/generic_subscription.hpp"
#include "diagnostic_updater/diagnostic_updater.hpp"

#include "udp_bridge_interfaces/srv/subscribe.hpp"
#include "udp_bridge_interfaces/srv/add_remote.hpp"
#include "udp_bridge_interfaces/srv/list_remotes.hpp"
#include "udp_bridge_interfaces/msg/connection_internal.hpp"
#include "udp_bridge_interfaces/msg/bridge_info.hpp"
#include "udp_bridge_interfaces/msg/topic_statistics_array.hpp"

#include "connection.h"
#include "giveup_diagnostic.h"
#include "packet.h"
#include "defragmenter.h"
#include "udp_bridge/destination_selection.h"
#include "udp_bridge/publish_queue.h"
#include "udp_bridge/relay_queue.h"
#include "udp_bridge/remote_identity.h"
#include "udp_bridge/types.h"
#include "udp_bridge/wrapped_packet.h"
//#include "std_msgs/msg/int32.hpp"

namespace udp_bridge
{

class RemoteNode;

/// The top level class containing all the functionality of the udp_bridge_node.
/// Once the constructor has read initial parameters and created the network socket,
/// the \ref spin method then sets up the services and reads the initial remote
/// connections and topics from the parameter server before going into a loop to read
/// incoming UDP packets and decode them.
///
/// Meanwhile, the \ref callback method handles sending ROS messages from locally
/// subscribed topics to remote nodes.
///
/// Additional timer callbacks are used to locally publish and send to remotes
/// information about the local bridge (udp_bridge::UDPBridge::bridgeInfoCallback) and
/// data statistics (udp_bridge::UDPBridge::statsReportCallback).
class UDPBridge: public rclcpp_lifecycle::LifecycleNode
{
public:
  using CallbackReturn = rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn;

  UDPBridge(const std::string &node_name = "udp_bridge");


  CallbackReturn on_configure(const rclcpp_lifecycle::State &);

  CallbackReturn on_activate(const rclcpp_lifecycle::State & state);

  CallbackReturn on_deactivate(const rclcpp_lifecycle::State & state);

  CallbackReturn on_cleanup(const rclcpp_lifecycle::State &);

  CallbackReturn on_shutdown(const rclcpp_lifecycle::State & state);

  /// Listens in a loop for incoming UDP packets and decodes them.
  /// The decode(std::vector<uint8_t> const &message, const SourceInfo& source_info) is
  /// called when a packet is received.
  void spin_once();

private:
  /// Sets the node name as seen by other udp_bridge nodes
  /// Warns if truncated to size specified in packet header.
  void setName(const std::string &name);

  /// Callback method for locally subscribed topics.
  /// ShapeShifter is used to be agnostic of message type at compile time.
  void callback(std::string topic_name, std::string topic_type, std::shared_ptr<rclcpp::SerializedMessage> message);


  /// Decodes outer layer of packets received over the UDP link and calls appropriate
  /// handlers based on packet type.
  /// @param message packet data as vector of bytes
  /// @param source_info info about the packet sender
  ///
  /// The main packet sorting method. More specific packet decoding routines get
  /// selected based on Packet::type.
  void decode(std::vector<uint8_t> const &message, const SourceInfo& source_info);

  template <typename MessageType>
  MessageType deserialize(std::vector<uint8_t> const &message)
  {
    const Packet* packet = reinterpret_cast<const Packet*>(message.data());
    auto data_size = message.size()-sizeof(PacketHeader);
    rclcpp::SerializedMessage serialized_message;
    serialized_message.reserve(data_size);
    memcpy(serialized_message.get_rcl_serialized_message().buffer, packet->data, data_size);
    serialized_message.get_rcl_serialized_message().buffer_length = data_size;
    MessageType deserialized_message;
    rclcpp::Serialization<MessageType> serializer;
    serializer.deserialize_message(&serialized_message, &deserialized_message);
    return deserialized_message;
  }

  /// Decodes data from a remote subscription received over the UDP link.
  /// @param message bytes representing a serialized MessageInternal message
  /// @param source_info info about the packet sender
  ///
  /// Runs on the socket-drain thread. It only deserializes the outer
  /// MessageInternal and enqueues a PublishItem on publish_queue_; all
  /// rmw-touching work (find-or-create the destination publisher,
  /// first-arrival sendBridgeInfo, publish) happens on the publish worker
  /// via publishItem(). See issue #10 — the drain thread must not make a
  /// call that can block on a single subscriber.
  void decodeData(std::vector<uint8_t> const &message, const SourceInfo& source_info);

  /// The single admit-to-publish point: hands an item the stale/reorder
  /// gate admitted to publish_queue_, and its attached relay form (issue
  /// #51), if any, to relay_queue_. Every path that publishes a received
  /// message goes through here — decodeData's immediate path, the items a
  /// gap-filler releases, and the items flushExpiredBuffer releases on
  /// window expiry — so relay is gated exactly as local publication is: a
  /// packet judged stale or superseded is never forwarded.
  void enqueuePublish(PublishItem&& item);

  /// publish_queue_ sink: runs on the publish worker thread. Finds or
  /// creates the destination GenericPublisher (first-arrival also triggers
  /// sendBridgeInfo) and publishes. Any blocking here (RELIABLE publish to a
  /// dead-but-matched subscriber; first-arrival rmw discovery) stalls only
  /// the worker, never the socket-drain thread.
  void publishItem(PublishItem&& item);

  /// True when some remote other than `source_node` lists `topic` in its
  /// routing table — i.e. when this received message has somewhere to be
  /// relayed (issue #51). Cheap enough for the socket-drain thread: one
  /// subscribers_ lookup under subscribers_mutex_, no copy, no I/O. Lets
  /// decodeData skip the payload copy entirely in the ordinary
  /// single-remote deployment, where relay is unreachable by construction.
  bool hasRelayDestinations(const std::string& topic,
                            const std::string& source_node) const;

  /// relay_queue_ sink: runs on the relay worker thread (issue #51).
  /// Forwards a message received from one remote to the OTHER remotes whose
  /// per-remote topics_list carries the same topic — the topic list is the
  /// routing table, there is no relay parameter. The loop rule (never send
  /// back to item.source_node) is applied by
  /// selectRateLimitedConnections's exclude_remote, which also throttles
  /// relayed traffic against exactly the same last_sent_time state as
  /// locally published traffic. Sends go through the existing send() path,
  /// so fragmenting, sequencing and AIMD admission control (#43/#52) apply
  /// unchanged. Direct-neighbour only: cycles are unsupported — see
  /// doc/relay_design.md.
  void relayToOtherRemotes(RelayItem&& item);

  /// Decodes topic info from remote.
  void decodeBridgeInfo(std::vector<uint8_t> const &message, const SourceInfo& source_info);

  /// Decodes statistics from remote.
  void decodeTopicStatistics(std::vector<uint8_t> const &message, const SourceInfo& source_info);

  /// Decodes a request from a remote node to subscribe to a local topic.
  void decodeSubscribeRequest(std::vector<uint8_t> const &message, const SourceInfo& source_info);

  /// Decodes a request from a remote node to resend packets.
  void decodeResendRequest(std::vector<uint8_t> const &message, const SourceInfo& source_info);

  /// Unwraps and decodes a packet.
  void unwrap(std::vector<uint8_t> const &message, const SourceInfo& source_info);

  /// Decodes connection management messages.
  void decodeConnection(std::vector<uint8_t> const &message, const SourceInfo& source_info);

  /// Service handler for local request to subscribe to a remote topic.
  void remoteSubscribe(
    const std::shared_ptr<udp_bridge_interfaces::srv::Subscribe::Request> request,
    std::shared_ptr<udp_bridge_interfaces::srv::Subscribe::Response> response);

  /// Service handler to advertise on a remote node a local topic.
  void remoteAdvertise(
    const std::shared_ptr<udp_bridge_interfaces::srv::Subscribe::Request> request,
    std::shared_ptr<udp_bridge_interfaces::srv::Subscribe::Response> response);

  /// Service handler for local request to cancel a remote subscription: tells
  /// the remote to stop pushing source_topic to us (mirror of remoteSubscribe).
  void removeSubscribe(
    const std::shared_ptr<udp_bridge_interfaces::srv::Subscribe::Request> request,
    std::shared_ptr<udp_bridge_interfaces::srv::Subscribe::Response> response);

  /// Service handler to stop advertising a local topic to a remote: removes the
  /// local source->remote forwarding (mirror of remoteAdvertise).
  void removeAdvertise(
    const std::shared_ptr<udp_bridge_interfaces::srv::Subscribe::Request> request,
    std::shared_ptr<udp_bridge_interfaces::srv::Subscribe::Response> response);

  /// Service handler to add a named remote
  void addRemote(
    const std::shared_ptr<udp_bridge_interfaces::srv::AddRemote::Request> request,
    std::shared_ptr<udp_bridge_interfaces::srv::AddRemote::Response> response);

  /// Service handler to list named remotes
  void listRemotes(
    const std::shared_ptr<udp_bridge_interfaces::srv::ListRemotes::Request> request,
    std::shared_ptr<udp_bridge_interfaces::srv::ListRemotes::Response> response);


  /// Map of remotes and connections
  using RemoteConnectionsList = std::map<std::string, std::vector<std::string> >;

  /// Convert a message to a packet and send it to remotes.
  template <typename MessageType>
  MessageSizeData send(MessageType const &message, const RemoteConnectionsList& remotes, bool is_overhead);

  /// Convert a message to a packet and send it to remote using all connections.
  template <typename MessageType>
  MessageSizeData send(MessageType const &message, const std::string& remote, bool is_overhead);

  /// Return a list of all remotes.
  RemoteConnectionsList allRemotes() const;

  /// Timer callback where data rate stats are reported
  void statsReportCallback();

  /// Periodic tick: sync per-connection diagnostic tasks with current remotes
  /// and force the diagnostic_updater to publish.
  void diagnosticTick();

  /// Register a diagnostic task for any (remote, connection) pair not yet tracked.
  void syncDiagnosticTasks();

  /// Populate a diagnostic status message for one (remote, connection) pair.
  void diagnoseConnection(const std::string& remote_name,
                          const std::string& connection_id,
                          diagnostic_updater::DiagnosticStatusWrapper& stat);

  /// Populate a diagnostic status message summarizing resend give-up
  /// activity for one remote. Computes rate from a window since the
  /// previous publish (per-remote state kept in giveup_rate_state_, a
  /// std::map<std::string, GiveupRateState> guarded by
  /// remote_nodes_mutex_; step + threshold-evaluation live in
  /// stepGiveupDiagnostic() / computeGiveupDiagnostic() in
  /// giveup_diagnostic.h so they can be unit-tested without a
  /// UDPBridge instance.
  void diagnoseRemoteGiveups(const std::string& remote_name,
                             diagnostic_updater::DiagnosticStatusWrapper& stat);

  /// Populate a diagnostic status message for one remote's reorder/jitter
  /// buffer (issue #35 review follow-up): current occupancy plus the
  /// cumulative buffered / expired-release counters from RemoteNode.
  /// Always OK — the buffer is bounded (<=1 packet/topic, each held <= the
  /// hold window), so this task is pure observability. Registered per
  /// remote in syncDiagnosticTasks() only when the reorder buffer is
  /// enabled (drop_stale_packets + reorder_hold_window_ms > 0).
  void diagnoseReorderBuffer(const std::string& remote_name,
                             diagnostic_updater::DiagnosticStatusWrapper& stat);

  /// Populate the publish-queue DiagnosticStatus: queued depth + total
  /// drops. WARNs when drops increased since the previous tick — drops mean
  /// a local destination subscriber stalled the publish path long enough to
  /// overflow the byte budget, so republished data was lost (unrecoverable;
  /// distinct from wire loss — see doc/qos_design.md). Registered once in
  /// on_configure (a single global task, not per-remote).
  void diagnosePublishQueue(diagnostic_updater::DiagnosticStatusWrapper& stat);

  /// Populate the relay-queue DiagnosticStatus (issue #51): queued depth +
  /// total drops, WARNing on drops since the previous tick. A relay drop is
  /// unrecoverable — the message never reached a Connection, so the resend
  /// layer cannot retransmit it. Registered once in on_configure.
  void diagnoseRelayQueue(diagnostic_updater::DiagnosticStatusWrapper& stat);

  /// Lifecycle-safe wrapper around `declare_parameter`. Parameters
  /// declared during `on_configure` persist across a
  /// cleanup→configure transition (`on_cleanup` does not
  /// `undeclare_parameter`), so a second `declare_parameter` for the
  /// same name throws `ParameterAlreadyDeclaredException` and breaks
  /// reconfiguration. Use this everywhere in `on_configure` instead
  /// of bare `declare_parameter` to keep the lifecycle reentrant.
  template <typename T>
  void declareIfMissing(const std::string& name, const T& default_value)
  {
    if(!has_parameter(name))
      declare_parameter(name, default_value);
  }

  /// Timer callback where info on available topics are periodically reported
  void bridgeInfoCallback();


  /// Send topics and remotes info to remotes and publishes locally
  void sendBridgeInfo();

  /// Send pending connection requests
  void sendConnectionRequests();

  /// Splits up a packet, if necessary.
  /// Returns a vector of fragment packets, or a vector with the data if fragmenting is not necessary.
  std::vector< std::vector<uint8_t> > fragment(const std::vector<uint8_t>& data);

  /// Remove old buffered packets
  void cleanupSentPackets();

  /// Find missing packet and request resend
  void resendMissingPackets();

  /// @brief Adds a local subscription.
  /// @param source_topic Local message topic
  /// @param destination_topic Message topic to publish at remote
  /// @param queue_size local queue size
  /// @param period Minimum delay between messages sent in seconds
  /// @param remote_node destination udp_bridge
  /// @param connection_id  connection to remote to use
  /// @param reliability per-topic destination publisher reliability
  ///                    ("reliable" default — see qos_resolution.h header
  ///                    comment for the rmw_zenoh_cpp workaround; design
  ///                    intent is "best_available". Also accepts
  ///                    "best_effort" and explicit "best_available".)
  /// @param durability per-topic durability ("volatile" default, "transient_local")
  /// @param history_depth KEEP_LAST(N); 0 means default 1
  void addSubscriberConnection(std::string const &source_topic,
                               std::string const &destination_topic,
                               uint32_t queue_size, float period,
                               std::string remote_node,
                               std::string connection_id,
                               std::string reliability = "",
                               std::string durability = "",
                               uint32_t history_depth = 0);

  /// @brief Remove a source->remote forwarding added by addSubscriberConnection.
  ///
  /// Drops the (connection_id) rate entry for (source_topic, remote_node); when
  /// that leaves the remote with no connections it is removed, and when the
  /// source topic has no remaining remotes its local generic subscription is
  /// torn down. Removing a non-existent forwarding is a no-op (idempotent). The
  /// subscription is destroyed off the subscribers_ lock so it cannot race the
  /// forwarding callback.
  void removeSubscriberConnection(std::string const &source_topic,
                                  std::string const &remote_node,
                                  std::string const &connection_id);

  /// @brief Checks configured local topics and attempts to subscribe
  void updateLocalSubscriptions();



  //void maximumPacketSizeCallback(const std_msgs::Int32::ConstPtr& msg);

  /// Name used to identify this node to other udp_bridge nodes
  std::string name_;

  // socket_, port_, max_packet_size_ are set once in on_configure and
  // are read-only afterward — no synchronization needed.
  int socket_;
  uint16_t port_ {4200};

  // Default sized for the links this bridge actually runs over, not for
  // the IPv4/UDP maximum (issue #58). Everything here is tunnelled:
  // WireGuard over Starlink or cellular. The cell tunnel runs MTU 1280,
  // leaving 1280 - 20 (IPv4) - 8 (UDP) = 1252 bytes of usable payload;
  // 1200 fits with headroom for any additional encapsulation, and is the
  // same conservative figure QUIC uses for the same reason.
  //
  // The previous 65500 default was not merely oversized, it was
  // counterproductive: the kernel IP-fragments such a datagram into ~45
  // pieces, and IP fragments are NOT individually recoverable, so
  // udp_bridge's own resend machinery cannot repair one. Losing any single
  // IP fragment discards the whole datagram — on a lossy radio link, most
  // of them. Letting the bridge fragment at 1200 is strictly better than
  // letting IP fragment at 65500. See the MTU black-hole diagnosis in
  // unh_echoboats_project11 (bizzyboat_deployment_log.md) and the sizing
  // decision in that repo's issue #389.
  //
  // Raise this only for a path whose end-to-end MTU has been verified.
  //
  // The static_assert is the enforcement: a comment alone would not survive
  // someone raising the constant back toward the UDP maximum.
  static constexpr int kTunnelMtuBytes = 1280;   // cellular WireGuard path
  static constexpr int kIpUdpHeaderBytes = 28;   // 20 (IPv4) + 8 (UDP)
  static constexpr int kDefaultMaxPacketSize = 1200;
  static_assert(
    kDefaultMaxPacketSize <= kTunnelMtuBytes - kIpUdpHeaderBytes,
    "Default maximum_packet_size must fit inside the cellular WireGuard "
    "tunnel's MTU without IP fragmentation (issue #58). IP fragments are not "
    "individually recoverable by the resend machinery, so one lost fragment "
    "discards the whole datagram. If a deployment has a verified larger "
    "end-to-end MTU, raise it in that deployment's config -- not here.");
  int max_packet_size_ {kDefaultMaxPacketSize};

  // Protocol counters — incremented from multiple callback groups via
  // send<>(), so atomic is required. fetch_add(1) gives a unique value
  // per call.
  std::atomic<uint32_t> next_fragmented_packet_id_ {0};

  rclcpp::Service<udp_bridge_interfaces::srv::Subscribe>::SharedPtr subscribe_service_;
  rclcpp::Service<udp_bridge_interfaces::srv::Subscribe>::SharedPtr advertise_service_;
  rclcpp::Service<udp_bridge_interfaces::srv::Subscribe>::SharedPtr remove_subscribe_service_;
  rclcpp::Service<udp_bridge_interfaces::srv::Subscribe>::SharedPtr remove_advertise_service_;
  rclcpp::Service<udp_bridge_interfaces::srv::AddRemote>::SharedPtr add_remote_service_;
  rclcpp::Service<udp_bridge_interfaces::srv::ListRemotes>::SharedPtr list_remotes_service_;

  rclcpp_lifecycle::LifecyclePublisher<udp_bridge_interfaces::msg::TopicStatisticsArray>::SharedPtr topic_statistics_publisher_;
  rclcpp_lifecycle::LifecyclePublisher<udp_bridge_interfaces::msg::BridgeInfo>::SharedPtr bridge_info_publisher_;

  //rclcpp::Subscriber  maximum_packet_size_subscriber_;

  /// Callback groups for the MultiThreadedExecutor.
  /// Invariants documented in udp_bridge_node.cpp.
  rclcpp::CallbackGroup::SharedPtr socket_drain_group_;
  rclcpp::CallbackGroup::SharedPtr republish_group_;
  rclcpp::CallbackGroup::SharedPtr periodic_group_;

  std::map<std::string, SubscriberDetails> subscribers_;
  mutable std::mutex subscribers_mutex_;

  std::map<std::string, rclcpp::GenericPublisher::SharedPtr> publishers_;
  mutable std::mutex publishers_mutex_;

  rclcpp::TimerBase::SharedPtr stats_report_timer_;
  rclcpp::TimerBase::SharedPtr bridge_info_timer_;
  rclcpp::TimerBase::SharedPtr spin_timer_;
  rclcpp::TimerBase::SharedPtr subscription_update_timer_;
  rclcpp::TimerBase::SharedPtr diagnostic_timer_;

  std::unique_ptr<diagnostic_updater::Updater> diagnostic_updater_;
  /// Names of diagnostic tasks already registered, so we don't double-add as
  /// remotes/connections appear.
  std::set<std::string> diagnostic_task_names_;

  // Protocol counters touched from multiple callback groups; atomic is
  // required to prevent duplicate packet numbers from corrupting the
  // resend protocol. last_packet_number_assign_time_ is stored as
  // nanoseconds so it can be a primitive atomic; readers reconstruct
  // an rclcpp::Time on access. The two writes (next_packet_number_++
  // and the time stamp) aren't atomic together — diagnostic skew
  // between bi.next_packet_number and bi.last_packet_time is acceptable
  // (both are diagnostic display).
  std::atomic<uint64_t> next_packet_number_ {0};
  std::atomic<int64_t> last_packet_number_assign_time_ns_ {0};

  std::atomic<uint64_t> next_connection_internal_message_sequence_number_ {0};

  struct PendingConnection
  {
    udp_bridge_interfaces::msg::ConnectionInternal message;
    std::shared_ptr<Connection> connection;
  };

  /// Map pending remote connections to their message sequence_number.
  std::map<uint64_t, PendingConnection> pending_connections_;
  mutable std::mutex pending_connections_mutex_;

  std::map<std::string, std::shared_ptr<RemoteNode> > remote_nodes_;
  mutable std::mutex remote_nodes_mutex_;

  /// Wire identities of the statically-configured remotes (issue #51) —
  /// `remotes.<label>.name` where set, else the `remotes_list` label; see
  /// remote_identity.h. Used only to recognise, and warn about, traffic
  /// from a sender that matches no configured remote: on a static config
  /// that is the visible symptom of a label/`name` mismatch, which makes
  /// the relay loop rule compare two different namespaces and lets the hub
  /// echo a remote's own traffic back to it. Populated in on_configure
  /// (before any timer exists, so no reader can race the write), cleared
  /// in on_cleanup, and read under remote_nodes_mutex_.
  std::set<std::string> configured_remote_names_;

  // Per-remote diagnostic state for the resend-give-up rate
  // computation (issue #22). Guarded by remote_nodes_mutex_ —
  // extended scope, not a new mutex; entries are small
  // integer/timestamp structs and the diagnostic callback's read is
  // brief. Cleared in on_cleanup alongside diagnostic_task_names_ so
  // an activate→deactivate→activate cycle starts from a known state.
  // Consolidating the (count, time, has_previous) tuple into one
  // struct (vs. two parallel maps) makes the wrapper's update logic
  // testable via the stepGiveupDiagnostic helper in
  // giveup_diagnostic.h — see test_giveup_diagnostic.cpp's
  // wrapper-side cases.
  std::map<std::string, GiveupRateState> giveup_rate_state_;

  // Thresholds for the resend-give-up DiagnosticStatus level. Declared
  // as ROS 2 parameters in on_configure so deployments can override
  // without rebuild; defaults calibrated against the 2026-05-19
  // BizzyBoat storm (~3.9/s steady, ~700/s burst). Live-updated via
  // an OnSetParameters callback (handle below), so operators can tune
  // mid-storm without restarting the bridge. Reads/writes are guarded
  // by remote_nodes_mutex_ — diagnoseRemoteGiveups holds the lock when
  // it samples these values, the callback holds the lock when it
  // updates them.
  double resend_giveup_warn_rate_per_s_ {5.0};
  double resend_giveup_error_rate_per_s_ {50.0};

  // Handle for the parameter-change callback. Reset in on_cleanup so
  // re-configure cycles don't accumulate stale handles.
  rclcpp::Node::OnSetParametersCallbackHandle::SharedPtr
    on_set_parameters_handle_;

  // Byte budget for publish_queue_ (issue #10). Declared as a ROS
  // parameter in on_configure (declareIfMissing) so deployments can size
  // it without rebuild; set once per configure and read by configure().
  size_t publish_queue_max_bytes_ {kDefaultPublishQueueMaxBytes};
  static constexpr size_t kDefaultPublishQueueMaxBytes = 64u * 1024u * 1024u;
  static constexpr size_t kMinPublishQueueMaxBytes = 1u * 1024u * 1024u;
  // Sanity ceiling (2 GiB). Both-bounds clamping matches the port /
  // maximum_packet_size / history_depth pattern and keeps the int64 → size_t
  // cast in on_configure well within range on 32-bit size_t platforms.
  static constexpr size_t kMaxPublishQueueMaxBytes = 2ull * 1024u * 1024u * 1024u;

  // Stale-packet gate (drop_stale_packets parameter, default true).
  // When set, decodeData drops a decoded message whose wrapped
  // packet_number is older than the newest already published for its
  // destination topic — a late-arriving resend the resend protocol
  // delivered out of order. Declared in on_configure (declareIfMissing)
  // so a deployment can disable it without a rebuild for topics that
  // need every message regardless of order.
  bool drop_stale_packets_ {true};

  // Reorder/jitter hold window in milliseconds (reorder_hold_window_ms,
  // issue #35). Global parameter (a recorded deviation from the
  // remotes.<r>.topics.<t>... nesting suggested in the issue review;
  // per-topic tuning is deferred to #34's opt-in infrastructure — matches
  // the drop_stale_packets / maximum_packet_size precedent). Default 0.0 =
  // disabled: the reorder buffer is off and decodeData takes the exact
  // pre-#35 stale-gate fast path (RemoteNode::admitForPublish before the
  // payload copy). When > 0, decodeData routes sequenced packets through
  // RemoteNode::admitOrBuffer instead, holding a gap-opening packet up to
  // this window so an out-of-order gap-filler can be published first.
  // Only active when drop_stale_packets_ is also true (the buffer is an
  // extension of that gate). Clamped 0–500 ms in on_configure. Set once
  // per configure; read on the socket-drain path.
  double reorder_hold_window_ms_ {0.0};

  // Cumulative count of messages dropped by the stale-packet gate.
  // Touched only on the socket-drain thread (decodeData); used for the
  // throttled visibility log so the operator can see the gate working.
  uint64_t stale_dropped_count_ {0};

  // Last publish-queue drop total observed by diagnosePublishQueue, so the
  // diagnostic can WARN on *recent* drops (increase since last tick) rather
  // than latching WARN forever after a single historical drop. Touched only
  // from the diagnostic callback (periodic_group_, mutually exclusive).
  uint64_t last_reported_publish_drops_ {0};

  // Same, for the relay queue (issue #51).
  uint64_t last_reported_relay_drops_ {0};

  // Byte budget for relay_queue_ (issue #51). A ROS parameter for the same
  // reason publish_queue_max_bytes is one: it is the memory this node may
  // hold when an outgoing link stalls, and on a hub the relay queue carries
  // the whole downstream fan-out, so the right size depends on the
  // deployment — a fixed constant would be a capability limit no operator
  // could lift without a rebuild. Same default and the same clamps as the
  // publish queue (see kDefaultPublishQueueMaxBytes); relay drops are
  // unrecoverable, so the budget wants headroom for a link stall of a few
  // seconds rather than a tight fit. Set once per configure, read by
  // relay_queue_.configure().
  size_t relay_queue_max_bytes_ {kDefaultPublishQueueMaxBytes};

  // Decouples the rmw-touching tail of decodeData from the socket-drain
  // thread (issue #10). configure()'d in on_configure, start()'ed in
  // on_activate, stop()+join()'ed in on_deactivate (so it runs only while
  // ACTIVE) — with a backstop stop() in on_cleanup. Declared LAST so that on
  // the non-lifecycle teardown path (node destroyed without on_cleanup) its
  // destructor (stop() + join()) runs before publishers_ / subscribers_ /
  // remote_nodes_ are destroyed — the worker's sink (publishItem /
  // sendBridgeInfo) touches all three.
  PublishQueue publish_queue_;

  // Remote->remote forwarding worker (issue #51). Keeps Connection::send()
  // — whose sendto() has a ~200 ms worst-case retry loop per stalled
  // destination — off the socket-drain thread, exactly as publish_queue_
  // keeps rmw publish work off it (issue #10). Separate from
  // publish_queue_ so a stalled local subscriber and a stalled remote link
  // are independent failure domains. Lifecycle mirrors publish_queue_:
  // configure()'d in on_configure, start()'ed in on_activate,
  // stop()+join()'ed in on_deactivate, with a backstop stop() in
  // on_cleanup. Declared LAST for the same destruction-order reason: on a
  // non-lifecycle teardown its destructor (stop() + join()) must run
  // before subscribers_ / remote_nodes_ — which its sink touches — are
  // destroyed.
  RelayQueue relay_queue_;
};

} // namespace udp_bridge

#endif
