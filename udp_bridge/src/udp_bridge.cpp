// Locking convention (see udp_bridge.h members):
//
//   - publishers_mutex_, subscribers_mutex_, remote_nodes_mutex_,
//     pending_connections_mutex_ guard the four maps that cross
//     callback-group boundaries under MultiThreadedExecutor.
//   - When more than one of these is needed in a single function, use
//     std::scoped_lock to acquire them deadlock-free in one shot. No
//     manual lock-ordering required.
//   - Pattern: lock briefly, copy out shared_ptrs / values needed, then
//     release the lock before slow operations (publish, sendto, recvfrom,
//     create_generic_publisher, get_publishers_info_by_topic,
//     create_generic_subscription). For check-then-create patterns
//     (decodeData, updateLocalSubscriptions), use a three-phase
//     lookup: phase 1 check map under lock, phase 2 do the slow rmw
//     call without lock, phase 3 re-acquire and try-emplace with
//     race-guard.
//   - Helpers in this file (sendBridgeInfo, allRemotes, sendConnectionRequests,
//     addSubscriberConnection, the send() template specialization) acquire
//     their own locks; callers do NOT pre-lock. Each entry-point caller
//     (timer, subscription, service handler, decode handler) is otherwise
//     responsible for taking locks before touching the maps directly.
//   - Connection has its own per-instance mutexes (sent_packets_mutex_,
//     sent_packet_statistics_mutex_, receive_history_mutex_) for state
//     owned by the Connection.
//   - RemoteNode has its own state_mutex_ (recursive_mutex) guarding
//     connections_, received_packet_times_, resend_request_times_,
//     next_packet_number_, last_packet_time_. Its public methods acquire
//     the lock; some call other public methods, hence recursive_mutex.
//     defragmenter_ is intentionally NOT guarded — by contract it's only
//     touched from socket_drain_group_ (UDPBridge::decode for Fragment
//     packets and the spin_once tail cleanup loop).
//   - Protocol counters next_packet_number_, next_fragmented_packet_id_,
//     next_connection_internal_message_sequence_number_, and
//     last_packet_number_assign_time_ns_ are std::atomic. Each is read
//     and written from multiple callback groups via send<>(); fetch_add(1)
//     gives a unique value per call. Duplicate packet numbers would
//     corrupt the resend protocol, so atomicity is load-bearing here.
//   - name_, port_, max_packet_size_ are set once in on_configure and
//     read-only afterward — no synchronization needed.

#include "udp_bridge/udp_bridge.h"
#include "udp_bridge/qos_resolution.h"
#include "udp_bridge/destination_selection.h"
#include "udp_bridge/relay_send.h"

#include "rclcpp/rclcpp.hpp"
#include "rclcpp/serialization.hpp"

#include <sys/socket.h>
#include <netdb.h>
#include <poll.h>
#include <algorithm>
#include <iomanip>
#include <sstream>

#include "udp_bridge_interfaces/msg/remote_subscribe_internal.hpp"
#include "udp_bridge_interfaces/msg/message_internal.hpp"
#include "udp_bridge_interfaces/msg/topic_statistics_array.hpp"
#include "udp_bridge_interfaces/msg/bridge_info.hpp"
#include "udp_bridge_interfaces/msg/resend_request.hpp"
#include "udp_bridge/remote_node.h"
#include "udp_bridge/resend_constants.h"
#include "udp_bridge/giveup_diagnostic.h"
#include "udp_bridge/subscriber_registry.h"
#include "udp_bridge/types.h"
#include "udp_bridge/utilities.h"
#include "lifecycle_msgs/msg/state.hpp"

namespace udp_bridge
{

using namespace udp_bridge_interfaces::msg;
using namespace udp_bridge_interfaces::srv;
using namespace std::placeholders;
using namespace std::chrono_literals;

namespace
{

// A rate limit at or below the connection's admission floor makes AIMD
// INERT: the floor is clamped to the cap at use, so the congested
// branch's max(floor, cap x 0.5) returns the cap unchanged and the
// controller can never back off, however bad the link gets.
//
// on_configure warns about the CONFIGURED case. This covers the RUNTIME
// routes, which are the more reachable ones and were unwarned (#52,
// review round 2): setRateLimit is called from the CONNECT decode with a
// peer-advertised cap and from the add_remote service, and neither
// re-checks the floor. A peer advertising a cap below
// kDefaultAdmissionFloorBytesPerSecond would otherwise switch adaptive
// admission off for the life of the node, silently.
//
// Throttled rather than one-shot: a flapping CONNECT re-adopts on every
// reconnect, and one line per flap is noise, but a WARN that fires only
// once would hide a SECOND connection entering the same state.
void warnIfAdmissionInert(const rclcpp::Logger& logger, rclcpp::Clock& clock,
                          const std::string& remote_name,
                          const std::string& connection_id,
                          const Connection& connection)
{
  const uint32_t cap = connection.rateLimit();
  const double floor = static_cast<double>(connection.admissionFloorBytesPerSecond());
  if(floor < static_cast<double>(cap))
    return;
  RCLCPP_WARN_STREAM_THROTTLE(logger, clock, 60000,
    "Connection '" << remote_name << "/" << connection_id
    << "': the rate limit now in force (" << cap
    << " B/s) is at or below the admission floor (" << floor
    << " B/s), so adaptive admission control is INERT on it — the cap can "
    "never be reduced, whatever the link reports. This limit came from a "
    "peer's CONNECT or from add_remote, not from this node's parameters.");
}

}  // namespace

UDPBridge::UDPBridge(const std::string &node_name)
: rclcpp_lifecycle::LifecycleNode(node_name, rclcpp::NodeOptions().enable_logger_service(true))
{
}

UDPBridge::CallbackReturn UDPBridge::on_configure(const rclcpp_lifecycle::State & state)
{
  // start with the ROS2 node name
  std::string name = get_name();
  auto last_slash = name.rfind('/');
  if(last_slash != std::string::npos)
    name = name.substr(last_slash+1);
  declareIfMissing( "name", name);
  // This is the name of the UDPBridge node, not the ROS2 node name.
  // An over-long name fails the transition here — before the socket is
  // bound, so the operator's natural remedy (shorten `name`, re-configure)
  // does not hit EADDRINUSE. See the note above the remote-identity block.
  if(!setName(get_parameter("name").as_string()))
    return CallbackReturn::FAILURE;


  RCLCPP_INFO_STREAM(get_logger(), "name: " << name_);

  declareIfMissing("port", port_);
  // ROS 2 parameters are int64. port_ is uint16_t; out-of-range values
  // would silently wrap and bind to an unintended port. Clamp + warn.
  {
    int64_t configured_port = get_parameter("port").as_int();
    if(configured_port < 0)
    {
      RCLCPP_WARN_STREAM(get_logger(),
        "port " << configured_port << " is below 0; clamping to 0");
      configured_port = 0;
    }
    else if(configured_port > 65535)
    {
      RCLCPP_WARN_STREAM(get_logger(),
        "port " << configured_port << " is above 65535; clamping to 65535");
      configured_port = 65535;
    }
    port_ = static_cast<uint16_t>(configured_port);
  }
  RCLCPP_INFO_STREAM(get_logger(), "port: " << port_);

  declareIfMissing("maximum_packet_size", max_packet_size_);
  // Bound the packet size to a sensible UDP range. Lower bound is the
  // minimum that still leaves room for a Packet header + a meaningful
  // payload after fragmentation; upper bound is the IPv4/UDP maximum.
  // A negative or huge value would propagate into fragment() and
  // buffer resizes, causing bad allocations.
  {
    constexpr int64_t kMinPacketSize = 256;
    constexpr int64_t kMaxPacketSize = 65500;
    int64_t configured_packet_size = get_parameter("maximum_packet_size").as_int();
    if(configured_packet_size < kMinPacketSize)
    {
      RCLCPP_WARN_STREAM(get_logger(),
        "maximum_packet_size " << configured_packet_size
        << " is below " << kMinPacketSize << "; clamping to " << kMinPacketSize);
      configured_packet_size = kMinPacketSize;
    }
    else if(configured_packet_size > kMaxPacketSize)
    {
      RCLCPP_WARN_STREAM(get_logger(),
        "maximum_packet_size " << configured_packet_size
        << " is above " << kMaxPacketSize << "; clamping to " << kMaxPacketSize);
      configured_packet_size = kMaxPacketSize;
    }
    max_packet_size_ = static_cast<int>(configured_packet_size);
  }
  RCLCPP_INFO_STREAM(get_logger(), "maximum_packet_size: " << max_packet_size_);

  // Stale-packet gate (drop late-arriving resends the resend protocol
  // delivered out of order — see decodeData / RemoteNode::admitForPublish).
  // Default true: a superseded old packet republished after a newer one
  // makes a consumer see the topic's stamp jump backward. Disable per
  // deployment (`-p drop_stale_packets:=false`) for topics that need every
  // message regardless of order.
  declareIfMissing("drop_stale_packets", drop_stale_packets_);
  drop_stale_packets_ = get_parameter("drop_stale_packets").as_bool();
  RCLCPP_INFO_STREAM(get_logger(), "drop_stale_packets: " << std::boolalpha << drop_stale_packets_);

  // Reorder/jitter buffer hold window (issue #35). Global parameter
  // (recorded deviation from the per-topic nesting suggested in the issue
  // review; per-topic tuning deferred to #34 — see the field comment on
  // reorder_hold_window_ms_ in udp_bridge.h). Default 0.0 = disabled, so
  // decodeData keeps its exact pre-#35 behaviour. When > 0 (and
  // drop_stale_packets is on), a gap-opening sequenced packet is held up
  // to this window so an out-of-order gap-filler can publish first. Clamp
  // 0–500 ms with a WARN on out-of-range values, matching the both-bounds
  // discipline used for maximum_packet_size / publish_queue_max_bytes — a
  // large accidental value would otherwise add that much latency to every
  // gap on every topic.
  declareIfMissing("reorder_hold_window_ms", reorder_hold_window_ms_);
  reorder_hold_window_ms_ = getDoubleParameter("reorder_hold_window_ms");
  {
    constexpr double kMaxReorderHoldWindowMs = 500.0;
    if(reorder_hold_window_ms_ < 0.0)
    {
      RCLCPP_WARN_STREAM(get_logger(),
        "reorder_hold_window_ms " << reorder_hold_window_ms_
        << " is negative; clamping to 0.0 (disabled)");
      reorder_hold_window_ms_ = 0.0;
    }
    else if(reorder_hold_window_ms_ > kMaxReorderHoldWindowMs)
    {
      RCLCPP_WARN_STREAM(get_logger(),
        "reorder_hold_window_ms " << reorder_hold_window_ms_
        << " exceeds the " << kMaxReorderHoldWindowMs
        << " ms ceiling; clamping to " << kMaxReorderHoldWindowMs);
      reorder_hold_window_ms_ = kMaxReorderHoldWindowMs;
    }
  }
  RCLCPP_INFO_STREAM(get_logger(), "reorder_hold_window_ms: " << reorder_hold_window_ms_
    << (reorder_hold_window_ms_ > 0.0 ? "" : " (reorder buffer disabled)"));

  // Resend give-up rate thresholds (issue #22). Defaults calibrated
  // against the 2026-05-19 BizzyBoat storm (~3.9/s steady, ~700/s
  // burst): steady fires WARN, burst fires ERROR. Live-tunable: the
  // OnSetParameters callback below propagates `ros2 param set` to the
  // cached members under remote_nodes_mutex_, so operators can adjust
  // mid-storm without a reconfigure cycle.
  declareIfMissing("resend_giveup_warn_rate_per_s", resend_giveup_warn_rate_per_s_);
  declareIfMissing("resend_giveup_error_rate_per_s", resend_giveup_error_rate_per_s_);
  resend_giveup_warn_rate_per_s_ = getDoubleParameter("resend_giveup_warn_rate_per_s");
  resend_giveup_error_rate_per_s_ = getDoubleParameter("resend_giveup_error_rate_per_s");
  // Validate launch-time values. Launch-line overrides
  // (`-p resend_giveup_warn_rate_per_s:=100.0`) bypass the
  // OnSetParameters callback below, so a bad pair would otherwise
  // silently disable or invert the diagnostic. Fail fast and visibly
  // — same bug class as a runtime `ros2 param set`, closed at the same
  // entry point.
  {
    auto validation = validateGiveupThresholds(
      resend_giveup_warn_rate_per_s_, resend_giveup_error_rate_per_s_);
    if(!validation.ok)
    {
      RCLCPP_ERROR(get_logger(),
        "Invalid resend give-up thresholds at on_configure: %s",
        validation.reason.c_str());
      return CallbackReturn::FAILURE;
    }
  }
  RCLCPP_INFO_STREAM(get_logger(),
    "resend_giveup thresholds: warn=" << resend_giveup_warn_rate_per_s_
    << "/s, error=" << resend_giveup_error_rate_per_s_ << "/s");

  // Publish-queue byte budget (issue #10). Bounds the memory the publish
  // worker may hold when a local destination subscriber stalls; once the
  // queued bytes would exceed this, the oldest republished messages are
  // dropped (see PublishQueue / doc/qos_design.md). ROS 2 parameters are
  // int64; clamp to both a sane floor and ceiling before the size_t cast (the
  // same both-bounds discipline used for port / maximum_packet_size /
  // history_depth above) so a tiny/negative value can't make the queue drop
  // everything, and a huge value can't invite unbounded memory growth or wrap
  // the int64 → size_t cast on 32-bit size_t platforms.
  declareIfMissing("publish_queue_max_bytes",
    static_cast<int64_t>(kDefaultPublishQueueMaxBytes));
  {
    int64_t configured = get_parameter("publish_queue_max_bytes").as_int();
    if(configured < static_cast<int64_t>(kMinPublishQueueMaxBytes))
    {
      RCLCPP_WARN_STREAM(get_logger(),
        "publish_queue_max_bytes " << configured << " is below "
        << kMinPublishQueueMaxBytes << "; clamping to "
        << kMinPublishQueueMaxBytes);
      configured = static_cast<int64_t>(kMinPublishQueueMaxBytes);
    }
    else if(configured > static_cast<int64_t>(kMaxPublishQueueMaxBytes))
    {
      RCLCPP_WARN_STREAM(get_logger(),
        "publish_queue_max_bytes " << configured << " is above "
        << kMaxPublishQueueMaxBytes << "; clamping to "
        << kMaxPublishQueueMaxBytes);
      configured = static_cast<int64_t>(kMaxPublishQueueMaxBytes);
    }
    publish_queue_max_bytes_ = static_cast<size_t>(configured);
  }
  RCLCPP_INFO_STREAM(get_logger(),
    "publish_queue_max_bytes: " << publish_queue_max_bytes_);

  // Relay-queue byte budget (issue #51). Same shape, same clamps and the
  // same default as publish_queue_max_bytes above -- the two queues are the
  // same component bounding the same kind of exposure (memory held while a
  // consumer stalls), but they are independent failure domains and a hub
  // sizes its downstream fan-out separately from its local republishing.
  declareIfMissing("relay_queue_max_bytes",
    static_cast<int64_t>(kDefaultPublishQueueMaxBytes));
  {
    int64_t configured = get_parameter("relay_queue_max_bytes").as_int();
    if(configured < static_cast<int64_t>(kMinPublishQueueMaxBytes))
    {
      RCLCPP_WARN_STREAM(get_logger(),
        "relay_queue_max_bytes " << configured << " is below "
        << kMinPublishQueueMaxBytes << "; clamping to "
        << kMinPublishQueueMaxBytes);
      configured = static_cast<int64_t>(kMinPublishQueueMaxBytes);
    }
    else if(configured > static_cast<int64_t>(kMaxPublishQueueMaxBytes))
    {
      RCLCPP_WARN_STREAM(get_logger(),
        "relay_queue_max_bytes " << configured << " is above "
        << kMaxPublishQueueMaxBytes << "; clamping to "
        << kMaxPublishQueueMaxBytes);
      configured = static_cast<int64_t>(kMaxPublishQueueMaxBytes);
    }
    relay_queue_max_bytes_ = static_cast<size_t>(configured);
  }
  RCLCPP_INFO_STREAM(get_logger(),
    "relay_queue_max_bytes: " << relay_queue_max_bytes_);

  on_set_parameters_handle_ = add_on_set_parameters_callback(
    [this](const std::vector<rclcpp::Parameter>& params)
        -> rcl_interfaces::msg::SetParametersResult
    {
      rcl_interfaces::msg::SetParametersResult result;
      result.successful = true;
      // Take the reader's lock so the (read current → validate batch →
      // apply) sequence is atomic against another concurrent set call,
      // and so the reader can never observe a half-applied pair.
      std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
      // Compute the (warn, error) pair that would result from applying
      // the batch on top of the current values, then validate the
      // combined state in one pass — catches per-value problems
      // (NaN/inf, negative) and cross-value problems (warn > error)
      // together. See validateGiveupThresholds() in giveup_diagnostic.h.
      bool warn_changed = false;
      bool error_changed = false;
      double proposed_warn = resend_giveup_warn_rate_per_s_;
      double proposed_error = resend_giveup_error_rate_per_s_;
      for(const auto& p: params)
      {
        if(p.get_name() == "resend_giveup_warn_rate_per_s")
        {
          proposed_warn = p.as_double();
          warn_changed = true;
        }
        else if(p.get_name() == "resend_giveup_error_rate_per_s")
        {
          proposed_error = p.as_double();
          error_changed = true;
        }
      }
      auto validation = validateGiveupThresholds(proposed_warn, proposed_error);
      if(!validation.ok)
      {
        result.successful = false;
        result.reason = validation.reason;
        return result;
      }
      resend_giveup_warn_rate_per_s_ = proposed_warn;
      resend_giveup_error_rate_per_s_ = proposed_error;
      if(warn_changed)
        RCLCPP_INFO_STREAM(get_logger(),
          "resend_giveup_warn_rate_per_s updated to "
          << resend_giveup_warn_rate_per_s_ << "/s");
      if(error_changed)
        RCLCPP_INFO_STREAM(get_logger(),
          "resend_giveup_error_rate_per_s updated to "
          << resend_giveup_error_rate_per_s_ << "/s");
      return result;
    });

  // Remote-identity validation runs here, ahead of the socket: it needs only
  // parameters, and a CallbackReturn::FAILURE leaves the node UNCONFIGURED
  // *without* calling on_cleanup. Returning after the socket would leak the
  // bound port, so the operator's natural remedy — fix the `remotes_list`
  // entry and re-configure — would hit EADDRINUSE on the bind below and
  // exit(1). Keep every parameter-only failure path above the socket block
  // for the same reason (issue #51).
  //
  // "Ahead of the socket", not ahead of everything: on_set_parameters_handle_
  // is already acquired at :288 (on_cleanup resets it), and a mistyped YAML
  // scalar can still throw out of the connections/topics loops after the bind
  // — rclcpp_lifecycle swallows that exception, so it is not an explicit
  // failure path and this ordering does not protect it.
  declareIfMissing("remotes_list", std::vector<std::string>());
  auto remotes_list = get_parameter("remotes_list").as_string_array();

  // A `remotes_list` entry IS the name that remote calls itself on the
  // wire (issue #67): it spells the remote's parameter paths AND keys
  // everything a static config creates (remote_nodes_,
  // subscribers_[topic].remote_details), which is what must match the
  // arriving WrappedPacket::source_node for the relay loop rule to work.
  // Validate the labels before anything is keyed by them. See
  // remote_identity.h.
  //
  // `remotes.<label>.name` is retired but still DECLARED: rclcpp surfaces a
  // YAML override only for a declared parameter (this node does not set
  // automatically_declare_parameters_from_overrides), so declaring it is
  // the only way to SEE a stale key left in a config written before #67.
  // Its value is never used for identity — and a non-empty one now fails
  // the transition rather than merely warning.
  //
  // Rejecting is not tidiness. Warning would leave exactly one config shape
  // — a stale value that differs from its label — announced and then
  // ignored, and that shape is the #51 echo bug: the hub keys the routing
  // table by the label, the peer stamps the other string into
  // WrappedPacket::source_node, the relay loop rule's string compare never
  // matches, and the hub relays that remote's own traffic back to it. Every
  // other identity error below fails; this one is no different. There is
  // also no carve-out for a stale value that happens to EQUAL its label:
  // the parameter is removed and unsupported, so setting it at all is a
  // configuration error, and one uniform rule is what an operator can hold
  // in their head.
  //
  // An explicitly empty value (`name: ""`) is indistinguishable from an
  // absent key, because the declared default is the empty string. Non-empty
  // is what "present" means here.
  //
  // Report EVERY offending key, not just the first, so a hub carrying three
  // stale keys does not cost three edit-and-retry cycles to discover three
  // lines of YAML.
  //
  // Note this failure returns before socket()/bind() below, so recovery IS
  // just edit-and-reconfigure — no restart needed. (An earlier version of
  // this comment claimed a restart was required, borrowing the EADDRINUSE
  // note from the message below. That note is about RENAMING an entry on an
  // already-configured node, which is a different situation: there the
  // socket is already bound. Keep every parameter-only check above the
  // socket so that stays true.)
  std::string retired_keys;
  for(const auto& remote_label: remotes_list)
  {
    std::string name_param = "remotes." + remote_label + ".name";
    declareIfMissing(name_param, std::string());
    auto retired_name = get_parameter(name_param).as_string();
    if(retired_name.empty())
      continue;
    if(!retired_keys.empty())
      retired_keys += ", ";
    retired_keys += name_param + " = '" + retired_name + "'";
  }
  if(!retired_keys.empty())
  {
    RCLCPP_ERROR_STREAM(get_logger(), "config sets " << retired_keys
      << " but remotes.<label>.name was REMOVED in issue #67 and is no"
         " longer supported: a remotes_list entry is itself the name that"
         " remote calls itself on the wire. Delete every key listed above,"
         " and make each remote's remotes_list entry the name that peer"
         " calls itself — renaming its remotes.<label>.* parameter paths"
         " with it. Fixing this error itself needs only a re-configure —"
         " this check runs before the socket is created. But a RENAME on an"
         " already-configured node takes effect only in a fresh process:"
         " nothing ever closes the bound socket, so on a fixed (non-zero)"
         " port a deactivate/cleanup/configure cycle re-binds the same port,"
         " gets EADDRINUSE and exit(1)s at the bind (issue #66). With"
         " port: 0 the kernel picks a fresh ephemeral port and the cycle"
         " succeeds, but every peer must then be told the new port. Under"
         " the shipped launch file (respawn=True) the process comes back on"
         " its own, so on a fixed port the outcome is an unplanned restart"
         " rather than a graceful rename.");
    return CallbackReturn::FAILURE;
  }

  std::string identity_error;
  auto remote_identities = resolveRemoteIdentities(remotes_list, name_,
                                                   &identity_error);
  if(!identity_error.empty())
  {
    // Fail loud: an unrepresentable identity puts the relay loop rule to
    // sleep for that remote, and a remote keyed by our own name lets the
    // receive path accept our own packets as a peer's.
    RCLCPP_ERROR_STREAM(get_logger(), "remote configuration: " << identity_error);
    return CallbackReturn::FAILURE;
  }

  //maximum_packet_size_subscriber_ = ros::NodeHandle("~").subscribe("maximum_packet_size", 1, &UDPBridge::maximumPacketSizeCallback, this);

  socket_ = socket(AF_INET, SOCK_DGRAM, 0);
  if(socket_ < 0)
  {
    RCLCPP_ERROR(get_logger(),"Failed creating socket");
    exit(1);
  }

  sockaddr_in bind_address; // address to listen on
  memset((char *)&bind_address, 0, sizeof(bind_address));
  bind_address.sin_family = AF_INET;
  bind_address.sin_addr.s_addr = htonl(INADDR_ANY);
  bind_address.sin_port = htons(port_);

  if(bind(socket_, (sockaddr*)&bind_address, sizeof(bind_address)) < 0)
  {
    RCLCPP_ERROR(get_logger(), "Error binding socket");
    exit(1);
  }

  timeval socket_timeout;
  socket_timeout.tv_sec = 0;
  socket_timeout.tv_usec = 1000;
  if(setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &socket_timeout, sizeof(socket_timeout)) < 0)
  {
    RCLCPP_ERROR(get_logger(), "Error setting socket timeout");
    exit(1);
  }

  int buffer_size;
  unsigned int s = sizeof(buffer_size);
  getsockopt(socket_, SOL_SOCKET, SO_RCVBUF, (void*)&buffer_size, &s);
  RCLCPP_INFO_STREAM(get_logger(), "recv buffer size:" << buffer_size);
  buffer_size = 500000;
  setsockopt(socket_, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size));
  getsockopt(socket_, SOL_SOCKET, SO_RCVBUF, (void*)&buffer_size, &s);
  RCLCPP_INFO_STREAM(get_logger(), "recv buffer size set to:" << buffer_size);
  buffer_size = 500000;
  setsockopt(socket_, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size));
  getsockopt(socket_, SOL_SOCKET, SO_SNDBUF, (void*)&buffer_size, &s);
  RCLCPP_INFO_STREAM(get_logger(), "send buffer size set to:" << buffer_size);

  // Callback groups — invariants documented in udp_bridge_node.cpp.
  // republish_group_ is Reentrant so multiple forwarding-subscription
  // callbacks can run concurrently. Required for high-rate camera
  // throughput; shared state is protected by the PR #12 lock audit
  // plus PR #16's reserve-then-record pattern in Connection::send
  // (which closes a check-then-record TOCTOU in the rate-limit path
  // without holding sent_packet_statistics_mutex_ across sendto — the
  // mutex hold would have stalled sendBridgeInfo, which holds
  // remote_nodes_mutex_, which in turn would have stalled the
  // socket-drain path).
  socket_drain_group_ = create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  republish_group_    = create_callback_group(rclcpp::CallbackGroupType::Reentrant);
  periodic_group_     = create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);

  std::string node_name = get_name();
  // Service handlers are admin-style work; assign to the periodic group so they
  // serialize against other admin work but never starve the socket-drain group.
  const rclcpp::QoS service_qos = rclcpp::ServicesQoS();
  subscribe_service_ = create_service<Subscribe>(
    node_name+"/remote_subscribe",
    std::bind(&UDPBridge::remoteSubscribe, this, _1, _2),
    service_qos, periodic_group_);
  advertise_service_ = create_service<Subscribe>(
    node_name+"/remote_advertise",
    std::bind(&UDPBridge::remoteAdvertise, this, _1, _2),
    service_qos, periodic_group_);
  remove_subscribe_service_ = create_service<Subscribe>(
    node_name+"/remove_subscribe",
    std::bind(&UDPBridge::removeSubscribe, this, _1, _2),
    service_qos, periodic_group_);
  remove_advertise_service_ = create_service<Subscribe>(
    node_name+"/remove_advertise",
    std::bind(&UDPBridge::removeAdvertise, this, _1, _2),
    service_qos, periodic_group_);

  add_remote_service_ = create_service<AddRemote>(
    node_name+"/add_remote",
    std::bind(&UDPBridge::addRemote, this, _1, _2),
    service_qos, periodic_group_);
  list_remotes_service_ = create_service<ListRemotes>(
    node_name+"/list_remotes",
    std::bind(&UDPBridge::listRemotes, this, _1, _2),
    service_qos, periodic_group_);

  topic_statistics_publisher_ = create_publisher<TopicStatisticsArray>(node_name+"/topic_statistics",10);

  rclcpp::QoS latching_qos(1);
  latching_qos.transient_local();
  latching_qos.keep_last(1);

  bridge_info_publisher_ = create_publisher<BridgeInfo>(node_name+"/bridge_info", latching_qos);

  // Under remote_nodes_mutex_, matching every reader. A re-configure is
  // the case that matters: on_cleanup resets only diagnostic_timer_, so
  // spin_timer_ and friends survive and keep firing while this runs.
  {
    std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
    configured_remote_names_.clear();
    configured_remote_names_.insert(remote_identities.begin(),
                                    remote_identities.end());
  }

  // Iterate the RESOLVED identities, not the raw remotes_list: a repeated
  // entry would otherwise construct and update the same RemoteNode twice
  // (two DDS publishers created and then discarded when the second
  // assignment replaces the first, plus a duplicate getaddrinfo). The end
  // state was already correct either way, but "a repeated entry is
  // idempotent" should be true of the work as well as the result.
  for(auto remote_name: remote_identities)
  {
    Remote remote_info;
    remote_info.name = remote_name;
    // Insert under remote_nodes_mutex_, matching every reader (spin_once,
    // decode, sendBridgeInfo, the diagnostics). Benign on a first
    // configure, a data race on a re-configure: on_cleanup resets only
    // diagnostic_timer_, so spin_timer_ and friends are still firing while
    // this loop rewrites the map. Only the map write needs the lock — the
    // RemoteNode is then driven through the local shared_ptr, because
    // update() does per-connection work (getaddrinfo among it) that must
    // not be done while holding a mutex the socket-drain path takes.
    //
    // Constructed BEFORE the lock: RemoteNode's constructor creates two
    // transient-local publishers, and DDS entity creation under a mutex the
    // socket-drain path takes is exactly what this comment says we don't do.
    // The insert is an unconditional assignment, so nothing needs
    // re-checking once the lock is taken.
    auto remote_node = std::make_shared<RemoteNode>(remote_info.name, name_, *this);
    {
      std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
      remote_nodes_[remote_info.name] = remote_node;
    }
    remote_node->update(remote_info);


    std::string connections_list_param = "remotes."+remote_name+".connections_list";
    declareIfMissing(connections_list_param, std::vector<std::string>());
    auto connections_list = get_parameter(connections_list_param).as_string_array();
    for(auto connection_name: connections_list)
    {
      RemoteConnection connection;
      connection.connection_id = connection_name;

      std::string host_param = "remotes." + remote_name + ".connections." + connection_name + ".host";
      declareIfMissing(host_param, "");
      connection.host = get_parameter(host_param).as_string();

      std::string port_param = "remotes." + remote_name + ".connections." + connection_name + ".port";
      declareIfMissing(port_param, 0);
      connection.port = get_parameter(port_param).as_int();

      std::string return_host_param = "remotes." + remote_name + ".connections." + connection_name + ".return_host";
      declareIfMissing(return_host_param, "");
      connection.return_host = get_parameter(return_host_param).as_string();

      std::string return_port_param = "remotes." + remote_name + ".connections." + connection_name + ".return_port";
      declareIfMissing(return_port_param, 0);
      connection.return_port = get_parameter(return_port_param).as_int();

      std::string maximum_bytes_per_second_param = "remotes." + remote_name + ".connections." + connection_name + ".maximum_bytes_per_second";
      declareIfMissing(maximum_bytes_per_second_param, 0);
      int mbps = get_parameter(maximum_bytes_per_second_param).as_int();
      if(mbps > 0)
        connection.maximum_bytes_per_second = mbps;

      std::string resend_budget_fraction_param = "remotes." + remote_name + ".connections." + connection_name + ".resend_budget_fraction";
      declareIfMissing(resend_budget_fraction_param, static_cast<double>(kDefaultResendBudgetFraction));
      double resend_budget_fraction = getDoubleParameter(resend_budget_fraction_param);

      std::string admission_floor_bps_param = "remotes." + remote_name + ".connections." + connection_name + ".admission_floor_bytes_per_second";
      declareIfMissing(admission_floor_bps_param, static_cast<double>(kDefaultAdmissionFloorBytesPerSecond));
      double admission_floor_bps = getDoubleParameter(admission_floor_bps_param);

      // RETIRED (#52). `link_headroom_fraction` has no behaviour left:
      // the `(1 - headroom) x goodput` clamp it fed was removed on
      // 2026-08-25, and nothing stores it any more — no Connection
      // state, no message field, no control-law read.
      //
      // It stays DECLARED as a tripwire, following the same reasoning as
      // the retired `remotes.<label>.name` key above: rclcpp surfaces a
      // YAML override only for a declared parameter, so UNdeclaring this
      // would make a stale key in an existing config silently ignored —
      // set it, nothing happens, no diagnostic. Silence is precisely the
      // shape that let the 2026-08-25 operator draw a false conclusion
      // about a tunable, and it is what this whole branch exists to
      // remove. Deleting the parameter "would not break any config" is
      // true and is the problem, not the justification.
      //
      // WARN rather than FAIL, which is where this differs from
      // `remotes.<label>.name`: that key, left stale, actively
      // MISROUTES traffic (the #51 echo bug), so a config carrying it is
      // broken. A stale headroom value misroutes nothing — the link is
      // still protected, by the refractory-gated multiplicative decrease
      // — so refusing to configure would ground a boat over a dead
      // config key. The operator gets one loud line per offending
      // connection instead.
      std::string link_headroom_fraction_param = "remotes." + remote_name + ".connections." + connection_name + ".link_headroom_fraction";
      declareIfMissing(link_headroom_fraction_param, static_cast<double>(kDefaultLinkHeadroomFraction));
      const double link_headroom_fraction = getDoubleParameter(link_headroom_fraction_param);
      if(link_headroom_fraction != static_cast<double>(kDefaultLinkHeadroomFraction))
        RCLCPP_WARN_STREAM(get_logger(),
          "Connection '" << remote_name << "/" << connection_name
          << "': link_headroom_fraction is set to " << link_headroom_fraction
          << ", but the parameter is RETIRED and does nothing (#52). The "
          "`(1 - headroom) x goodput` clamp it controlled was removed on "
          "2026-08-25 because goodput is depressed by the very throttling "
          "the clamp was computing. Co-tenant traffic on this path is now "
          "protected by the refractory-gated multiplicative decrease "
          "instead. Remove the key from your config; it is still declared "
          "only so a stale one is visible rather than silently ignored.");

      std::string admission_refractory_param = "remotes." + remote_name + ".connections." + connection_name + ".admission_refractory_period_seconds";
      declareIfMissing(admission_refractory_param, kDefaultAdmissionRefractoryPeriodSeconds);
      double admission_refractory = getDoubleParameter(admission_refractory_param);

      remote_info.connections.push_back(connection);
      remote_node->update(remote_info);

      // These tunables are applied to the live Connection after
      // update() creates/refreshes it — `connection` above is a
      // RemoteConnection msg (config data), not the live object. The
      // message/service paths that also call setRateLimit
      // (CONNECT/adopt, addRemote) carry none of them, so connections
      // created there keep the field-initializer defaults
      // (kDefaultResendBudgetFraction,
      // kDefaultAdmissionFloorBytesPerSecond,
      // kDefaultAdmissionRefractoryPeriodSeconds); the parameters here
      // are the only non-default source (see doc/resend_budget_design.md
      // and doc/admission_control_design.md).
      //
      // All of these are configure-time only, which is the property RCA
      // item D names as what blocked live mitigation on 2026-08-25 — the
      // admission floor could not be raised from the boat while it was
      // the problem. issue #75 is the tracking issue for making connection
      // parameters runtime-reconfigurable;
      // admission_refractory_period_seconds joins the same set and
      // should be revisited with it.
      if(auto live_connection = remote_node->connection(connection_name))
      {
        live_connection->setResendBudgetFraction(static_cast<float>(resend_budget_fraction));
        live_connection->setAdmissionFloorBytesPerSecond(static_cast<float>(admission_floor_bps));
        live_connection->setAdmissionRefractoryPeriodSeconds(admission_refractory);

        // A floor at or above the connection's own cap makes AIMD a
        // no-op: the floor is clamped to the cap at use, so the
        // congested branch's max(floor, cap x 0.5) returns the cap
        // unchanged and this connection can never back off, however bad
        // the link gets. That is a legitimate configuration to want (a
        // link you have decided never to throttle), but it is not one
        // to arrive at by accident — say so once, at configure time,
        // rather than leaving an operator to infer it from a cap that
        // never moves (#52).
        //
        // Read the APPLIED floor back from the connection, not the raw
        // parameter. setAdmissionFloorBytesPerSecond maps NaN and
        // negatives to kDefaultAdmissionFloorBytesPerSecond, so
        // `admission_floor_bytes_per_second: -1` on a connection capped
        // below 8192 B/s is exactly the inert case this WARN exists for —
        // and testing the raw -1 against the cap would stay silent on it
        // (#52, review round 2).
        const uint32_t effective_cap = live_connection->rateLimit();
        const double applied_floor =
          static_cast<double>(live_connection->admissionFloorBytesPerSecond());
        if(applied_floor >= static_cast<double>(effective_cap))
          RCLCPP_WARN_STREAM(get_logger(),
            "Connection '" << remote_name << "/" << connection_name
            << "': admission_floor_bytes_per_second (" << applied_floor
            << ", from a configured " << admission_floor_bps
            << ") is at or above the connection's rate limit ("
            << effective_cap << " B/s), so adaptive admission control is "
            "INERT on it — the cap can never be reduced, whatever the link "
            "reports. Lower the floor, or raise maximum_bytes_per_second, "
            "if that was not intended.");
      }

      std::string topics_list_param = "remotes." + remote_name + ".connections." + connection_name + ".topics_list";
      declareIfMissing(topics_list_param, std::vector<std::string>());
      auto topics_list = get_parameter(topics_list_param).as_string_array();
      for(auto topic: topics_list)
      {
        std::string queue_size_param = "remotes." + remote_name + ".connections." + connection_name + ".topics." + topic + ".queue_size";
        declareIfMissing(queue_size_param, 10);
        int queue_size = get_parameter(queue_size_param).as_int();

        std::string period_param = "remotes." + remote_name + ".connections." + connection_name + ".topics." + topic + ".period";
        declareIfMissing(period_param, 0.0);
        double period = getDoubleParameter(period_param);

        std::string source_param = "remotes." + remote_name + ".connections." + connection_name + ".topics." + topic + ".source";
        declareIfMissing(source_param, topic);
        std::string source = get_parameter(source_param).as_string();
        source = get_node_base_interface()->resolve_topic_or_service_name(source, false, false);

        std::string destination_param = "remotes." + remote_name + ".connections." + connection_name + ".topics." + topic + ".destination";
        declareIfMissing(destination_param, source);
        auto destination = get_parameter(destination_param).as_string();

        std::string reliability_param = "remotes." + remote_name + ".connections." + connection_name + ".topics." + topic + ".reliability";
        declareIfMissing(reliability_param, std::string());
        std::string reliability = get_parameter(reliability_param).as_string();

        std::string durability_param = "remotes." + remote_name + ".connections." + connection_name + ".topics." + topic + ".durability";
        declareIfMissing(durability_param, std::string());
        std::string durability = get_parameter(durability_param).as_string();

        std::string history_depth_param = "remotes." + remote_name + ".connections." + connection_name + ".topics." + topic + ".history_depth";
        declareIfMissing(history_depth_param, 0);
        // ROS 2 parameters are int64; an unchecked static_cast<uint32_t>
        // of a negative or out-of-range value wraps to a billions-large
        // depth and triggers a huge KEEP_LAST allocation in the rmw
        // layer. Validate and clamp before casting.
        constexpr int64_t kMaxHistoryDepth = 10000;
        int64_t history_depth_value = get_parameter(history_depth_param).as_int();
        if(history_depth_value < 0)
        {
          RCLCPP_WARN_STREAM(get_logger(),
            "history_depth for " << remote_name << "/" << connection_name << "/" << topic
            << " is " << history_depth_value << "; treating as default (0)");
          history_depth_value = 0;
        }
        else if(history_depth_value > kMaxHistoryDepth)
        {
          RCLCPP_WARN_STREAM(get_logger(),
            "history_depth for " << remote_name << "/" << connection_name << "/" << topic
            << " is " << history_depth_value << "; clamping to " << kMaxHistoryDepth);
          history_depth_value = kMaxHistoryDepth;
        }
        uint32_t history_depth = static_cast<uint32_t>(history_depth_value);

        addSubscriberConnection(source, destination, queue_size, period,
                                remote_info.name, connection.connection_id,
                                reliability, durability, history_depth);
      }
    }
  }


  stats_report_timer_ = create_wall_timer(
    1s, std::bind(&UDPBridge::statsReportCallback, this), periodic_group_);
  bridge_info_timer_ = create_wall_timer(
    2s, std::bind(&UDPBridge::bridgeInfoCallback, this), periodic_group_);
  // Hot path: socket drainer must never be starved. Owns its own group.
  spin_timer_ = create_wall_timer(
    10ms, std::bind(&UDPBridge::spin_once, this), socket_drain_group_);
  subscription_update_timer_ = create_wall_timer(
    1s, std::bind(&UDPBridge::updateLocalSubscriptions, this), periodic_group_);

  diagnostic_updater_ = std::make_unique<diagnostic_updater::Updater>(this);
  diagnostic_updater_->setHardwareID(name_);
  syncDiagnosticTasks();
  // Single global publish-queue diagnostic (issue #10). diagnostic_updater_
  // is freshly created each on_configure (reset in on_cleanup), so adding
  // the task here once per configure cannot double-register.
  diagnostic_updater_->add("udp_bridge " + name_ + ": publish queue",
    [this](diagnostic_updater::DiagnosticStatusWrapper& stat)
    {
      diagnosePublishQueue(stat);
    });
  // Same for the relay queue (issue #51): a relay drop is unrecoverable
  // data loss for the downstream remotes, so it needs to be visible.
  diagnostic_updater_->add("udp_bridge " + name_ + ": relay queue",
    [this](diagnostic_updater::DiagnosticStatusWrapper& stat)
    {
      diagnoseRelayQueue(stat);
    });
  diagnostic_timer_ = create_wall_timer(
    1s, std::bind(&UDPBridge::diagnosticTick, this), periodic_group_);

  // Configure the publish worker (issue #10) but don't start it yet — the
  // worker runs only while the node is ACTIVE (started in on_activate,
  // stopped in on_deactivate), so it never publishes during the INACTIVE
  // state, matching the state-guard discipline of the other publish paths
  // (spin_once / statsReportCallback / bridgeInfoCallback).
  publish_queue_.configure(
    [this](PublishItem&& item){ publishItem(std::move(item)); },
    publish_queue_max_bytes_);

  // Configure the relay worker (issue #51) on the same terms: it forwards
  // only while ACTIVE, so it is started in on_activate and stopped in
  // on_deactivate. Its byte budget is its own parameter: a hub's downstream
  // fan-out is sized independently of its local republishing (see
  // doc/relay_design.md).
  relay_queue_.configure(
    [this](RelayItem&& item){ relayToOtherRemotes(std::move(item)); },
    relay_queue_max_bytes_);

  return LifecycleNode::on_configure(state);
}

UDPBridge::CallbackReturn UDPBridge::on_activate(const rclcpp_lifecycle::State & state)
{
  // Start the publish worker (issue #10) only for the ACTIVE state, so it
  // processes (and publishes) exactly while spin_once is enqueuing. start()
  // is a no-op if already running.
  publish_queue_.start();
  // Same lifecycle for the relay worker (issue #51).
  relay_queue_.start();
  return LifecycleNode::on_activate(state);
}

UDPBridge::CallbackReturn UDPBridge::on_deactivate(const rclcpp_lifecycle::State & state)
{
  // Stop + join the publish worker on leaving ACTIVE so no republish runs
  // while INACTIVE. stop() discards any still-queued items (best-effort) and
  // joins; the join waits at most for one in-flight publish to return (the
  // rmw max_blocking_time), not the whole backlog. A subsequent on_activate
  // restarts a fresh worker.
  publish_queue_.stop();
  // Stop + join the relay worker too (issue #51), so nothing is forwarded
  // while INACTIVE. Queued relays are discarded, matching the publish
  // queue's best-effort contract.
  relay_queue_.stop();
  // Both stop() calls fold the discarded backlog into their dropped counts.
  // That backlog is the operator deactivating, not link loss, so re-baseline
  // the diagnostic deltas here: diagnostic_timer_ keeps ticking while
  // INACTIVE (it is reset only in on_cleanup), and without this the first
  // tick after a deactivate reports "relay dropping (N since last tick) —
  // outgoing link stalled?" for drops the operator caused. The cumulative
  // dropped_total still shows them. (The transition callback runs outside
  // periodic_group_, so a tick could interleave; the worst case is that one
  // tick reports the backlog before the re-baseline lands — never a
  // double-count, since both sides only ever assign the same monotonic
  // total.)
  // Advanced, not assigned: a diagnostic tick can interleave with this
  // transition callback (it does not run in periodic_group_), and a plain
  // store would let the tick subtract this newer baseline from the older
  // total it had already read — an unsigned underflow that reports the
  // operator's own discarded backlog as ~2^64 dropped messages. See
  // drop_baseline.h.
  advanceDropBaseline(last_reported_publish_drops_,
                      publish_queue_.dropped_count());
  advanceDropBaseline(last_reported_relay_drops_,
                      relay_queue_.dropped_count() + relay_drops_.lost());
  // Discard any reorder-buffer packets held at deactivation (issue #35
  // review follow-up). spin_once returns early while INACTIVE, so the
  // window-expiry flush stops ticking; a packet held here would sit out
  // the whole INACTIVE period and then publish — long stale — on the
  // first flush tick after reactivation. Only the held packets are
  // dropped; the per-topic high-water marks persist like the rest of the
  // per-remote state (see RemoteNode::clearReorderBuffer).
  {
    std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
    for(auto& remote_entry: remote_nodes_)
      if(remote_entry.second)
        remote_entry.second->clearReorderBuffer();
  }
  return LifecycleNode::on_deactivate(state);
}

UDPBridge::CallbackReturn UDPBridge::on_cleanup(const rclcpp_lifecycle::State & state)
{
  // Backstop stop + join of the publish worker (issue #10). Normally it was
  // already stopped in on_deactivate (cleanup follows deactivate); stop() is
  // idempotent. Belt-and-suspenders for a configure→cleanup path that never
  // activated (worker never started → no-op) and so the worker is provably
  // joined before anything here touches the maps its sink uses.
  publish_queue_.stop();
  relay_queue_.stop();  // idempotent backstop, as above (issue #51)
  diagnostic_timer_.reset();
  diagnostic_updater_.reset();
  diagnostic_task_names_.clear();
  // Drop the parameter-change callback handle so a subsequent
  // on_configure cycle starts fresh; otherwise the previous handle
  // would still be live and a duplicate would accumulate.
  on_set_parameters_handle_.reset();
  // Per-remote diagnostic state: also clear, so an
  // activate→deactivate→activate cycle starts from a known baseline
  // rather than carrying stale GiveupRateState entries that would
  // compute a huge "elapsed" on the next publish. Guarded by
  // remote_nodes_mutex_ per the lifecycle-state contract for this
  // map (declaration in udp_bridge.h).
  {
    std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
    giveup_rate_state_.clear();
    // Configured wire identities (issue #51) go with them, so a
    // reconfigure starts from the new remotes_list rather than the old.
    configured_remote_names_.clear();
  }
  return LifecycleNode::on_cleanup(state);
}

UDPBridge::CallbackReturn UDPBridge::on_shutdown(const rclcpp_lifecycle::State & state)
{
  // Shutdown is reachable from ACTIVE directly, without passing through
  // on_deactivate, so both workers have to be stopped here too or a
  // shutdown-from-active leaves them running against maps that are about
  // to be destroyed. stop() is idempotent, so the ordinary
  // deactivate -> cleanup -> shutdown path pays nothing (issues #10/#51).
  publish_queue_.stop();
  relay_queue_.stop();
  return LifecycleNode::on_shutdown(state);
}


bool UDPBridge::setName(const std::string &name)
{
  // Reject rather than truncate (issue #51). This used to shorten the name
  // to the on-wire limit and WARN. That warning was the only notice an
  // operator got that their bridge had just started answering to a
  // different name than every remote is configured to expect — the relay
  // loop rule compares the wire `source_node` against configured remote
  // identities, so a truncated local name matches nothing and the hub
  // relays a sender's own traffic back to it. Truncation manufactured that
  // mismatch; refusing to start does not.
  if(!node_name_fits(name))
  {
    RCLCPP_ERROR_STREAM(get_logger(),
      node_name_too_long_error(
        "the `name` parameter (defaults to the ROS 2 node name)", name));
    return false;
  }
  name_ = name;
  return true;
}

void UDPBridge::spin_once()
{
  if (get_current_state().id() != lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE)
  {
    // If the node is not active, we don't need to process incoming packets.
    // This is important for the lifecycle node to avoid processing packets
    // when the node is not in the active state.
    return;
  }

  sockaddr_in remote_address;
  socklen_t remote_address_length = sizeof(remote_address);
  while(true)
  {
    pollfd p;
    p.fd = socket_;
    p.events = POLLIN;
    int ret = poll(&p, 1, 10);
    if(ret < 0)
      RCLCPP_WARN_STREAM(get_logger(), "poll error: " << int(errno) << " " << strerror(errno));
    if(ret > 0 && p.revents & POLLIN)
    {
      int receive_length = 0;
      std::vector<uint8_t> buffer;
      int buffer_size;
      unsigned int buffer_size_size = sizeof(buffer_size);
      getsockopt(socket_,SOL_SOCKET,SO_RCVBUF,&buffer_size,&buffer_size_size);
      buffer.resize(buffer_size);
      receive_length = recvfrom(socket_, &buffer.front(), buffer_size, 0, (sockaddr*)&remote_address, &remote_address_length);
      RCLCPP_DEBUG_STREAM(get_logger(), "received " << receive_length << " bytes");
      if(receive_length > 0)
      {
        SourceInfo source_info;
        source_info.host = addressToDotted(remote_address);
        source_info.port = ntohs(remote_address.sin_port);
        buffer.resize(receive_length);
        decode(buffer, source_info);
      }
    }
    else
      break;
  }
  // Snapshot remotes for the per-remote defragmenter cleanup.
  std::vector<std::pair<std::string, std::shared_ptr<RemoteNode>>> remotes;
  {
    std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
    remotes.assign(remote_nodes_.begin(), remote_nodes_.end());
  }
  auto now = get_clock()->now();
  auto cleanup_cutoff = now - rclcpp::Duration(kReceiveHistoryWindow);
  // Reorder buffer (issue #35): age held packets out on every 10 ms tick,
  // even when the link is idle, so a gap-opening packet on a briefly-idle
  // topic is released on schedule rather than stalling until the next
  // arriving packet. Computed once outside the loop; only walked when the
  // buffer is enabled (window > 0).
  const bool reorder_enabled = drop_stale_packets_ && reorder_hold_window_ms_ > 0.0;
  const auto reorder_hold_window =
    rclcpp::Duration::from_seconds(reorder_hold_window_ms_ / 1000.0);
  for(auto& remote: remotes)
  {
    if(remote.second)
    {
      int discard_count = remote.second->defragmenter().cleanup(cleanup_cutoff);
      if(discard_count)
        RCLCPP_INFO_STREAM(get_logger(), "Discarded " << discard_count << " incomplete packets from " << remote.first);
      if(reorder_enabled)
        for(auto& item: remote.second->flushExpiredBuffer(now, reorder_hold_window))
          enqueuePublish(std::move(item));
    }
  }
  cleanupSentPackets();
  resendMissingPackets();
}

void UDPBridge::callback(std::string topic_name, std::string topic_type, std::shared_ptr<rclcpp::SerializedMessage> message)
{
  if(get_current_state().id() != lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE)
  {
    return;
  }

  rclcpp::Time now = get_clock()->now();

  // Read remote_details / connection_rates and update last_sent_time + initial
  // statistics under subscribers_mutex_. We also capture per-destination
  // destination_topic and QoS config so the send loop below doesn't need to
  // re-lock. DestinationConfig lives in destination_selection.h because the
  // relay sender captures the same fields the same way (issue #51).
  RemoteConnectionsList destinations;
  std::map<std::string, DestinationConfig> destination_config_by_remote;
  {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    // Use find(), not operator[]: a message can still arrive on a subscription
    // that removeSubscriberConnection is tearing down. operator[] would
    // re-insert an empty entry, which updateLocalSubscriptions would then
    // resurrect as a zombie subscription forwarding to nobody.
    auto sub_it = subscribers_.find(topic_name);
    if(sub_it == subscribers_.end())
      return;
    auto& sub = sub_it->second;
    // Rate limiting lives in selectRateLimitedConnections (issue #51) so the
    // relay path throttles identically against the same last_sent_time
    // state. No remote to exclude here: this message originated locally, so
    // there is no sender to avoid echoing to.
    destinations = selectRateLimitedConnections(sub.remote_details, now);
    for(auto& remote_details: sub.remote_details)
    {
      auto& dc = destination_config_by_remote[remote_details.first];
      dc.destination_topic = remote_details.second.destination_topic;
      dc.reliability       = remote_details.second.reliability;
      dc.durability        = remote_details.second.durability;
      dc.history_depth     = remote_details.second.history_depth;
    }

    MessageSizeData size_data;
    size_data.message_size = message->size();
    size_data.timestamp = now;
    size_data.send_results[""][""];
    sub.statistics.add(size_data);
  }

  if (destinations.empty())
  {
    RCLCPP_DEBUG_STREAM(get_logger(), "No ready destination to send message");
    return;
  }

  // First, serialize message in a MessageInternal message
  MessageInternal message_internal;

  message_internal.source_topic = topic_name;
  message_internal.datatype = topic_type;
  //message.md5sum = msg->getMD5Sum();
  //message.message_definition = msg->getMessageDefinition();


  message_internal.data.resize(message->size());
  memcpy(message_internal.data.data(), message->get_rcl_serialized_message().buffer, message->size());

  // Send (slow — acquires remote_nodes_mutex_ / pending_connections_mutex_
  // internally). Updating per-destination statistics afterward locks
  // subscribers_mutex_ briefly per destination.
  for(auto destination: destinations)
  {
    auto config_it = destination_config_by_remote.find(destination.first);
    if(config_it != destination_config_by_remote.end())
    {
      message_internal.destination_topic = config_it->second.destination_topic;
      message_internal.reliability       = config_it->second.reliability;
      message_internal.durability        = config_it->second.durability;
      message_internal.history_depth     = config_it->second.history_depth;
    }
    else
    {
      message_internal.destination_topic.clear();
      message_internal.reliability.clear();
      message_internal.durability.clear();
      message_internal.history_depth = 0;
    }
    RemoteConnectionsList connections;
    connections[destination.first] = destination.second;
    auto size_data = send(message_internal, connections, false);
    size_data.message_size = message->size();
    {
      std::lock_guard<std::mutex> lock(subscribers_mutex_);
      auto sub_it = subscribers_.find(topic_name);
      if(sub_it != subscribers_.end())
        sub_it->second.statistics.add(size_data);
    }
  }
}

void UDPBridge::decode(std::vector<uint8_t> const &message, const SourceInfo& source_info)
{
  if(message.empty())
  {
    RCLCPP_DEBUG_STREAM(get_logger(), "empty message from: " << source_info.node_name << " " << source_info.host << ":" << source_info.port);
    return;
  }

  const Packet *packet = reinterpret_cast<const Packet*>(message.data());
  RCLCPP_DEBUG_STREAM(get_logger(), "Received packet of type " << int(packet->type) << " and size " << message.size() << " from '" << source_info.node_name << "' (" << source_info.host << ":" << source_info.port << ")");
  std::shared_ptr<RemoteNode> remote_node;
  {
    std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
    auto remote_node_iterator = remote_nodes_.find(source_info.node_name);
    if(remote_node_iterator != remote_nodes_.end())
      remote_node = remote_node_iterator->second;
  }
  // Catch-all so a deserialization throw on a stale/mismatched-schema
  // buffer (e.g. a partial cross-version deployment of MessageInternal
  // — see udp_bridge/doc/qos_design.md "Mismatched-pair compatibility")
  // is logged and dropped instead of propagating up the spin_once timer
  // callback and tearing the executor down.
  try
  {
    switch(packet->type)
    {
      case PacketType::Data:
        decodeData(message, source_info);
        break;
      case PacketType::Compressed:
        decode(uncompress(message), source_info);
        break;
      case PacketType::SubscribeRequest:
        decodeSubscribeRequest(message, source_info);
        break;
      case PacketType::Fragment:
        if(remote_node)
          if(remote_node->defragmenter().addFragment(message, get_clock()->now()))
            for(auto p: remote_node->defragmenter().getPackets())
              decode(p, source_info);
        break;
      case PacketType::BridgeInfo:
          decodeBridgeInfo(message, source_info);
          break;
      case PacketType::TopicStatistics:
          decodeTopicStatistics(message, source_info);
          break;
      case PacketType::WrappedPacket:
        unwrap(message, source_info);
        break;
      case PacketType::ResendRequest:
        decodeResendRequest(message, source_info);
        break;
      case PacketType::Connection:
        decodeConnection(message, source_info);
        break;
      default:
          RCLCPP_WARN_STREAM(get_logger(), "Unknown packet type: " << int(packet->type) << " from: " << source_info.node_name << " " << source_info.host << ":" << source_info.port);
    }
  }
  catch(const std::exception& e)
  {
    RCLCPP_ERROR_STREAM(get_logger(),
      "decoding error on packet of type " << int(packet->type)
      << " size " << message.size()
      << " from '" << source_info.node_name
      << "' (" << source_info.host << ":" << source_info.port << "): "
      << e.what());
  }
}

void UDPBridge::decodeData(std::vector<uint8_t> const &message, const SourceInfo& source_info)
{
  auto outer_message = deserialize<MessageInternal>(message);

  // destination_topic falls back to source_topic when unset.
  std::string topic = outer_message.destination_topic;
  if(topic.empty())
    topic = outer_message.source_topic;

  // Relay (issue #51): is there any OTHER remote whose topics_list carries
  // this topic? Only this cheap routing-table probe runs here on the
  // socket-drain thread; the relay form is attached to the PublishItem
  // below and handed to the relay worker by enqueuePublish() -- so a
  // message is relayed if and only if the stale/reorder gate admits it for
  // publication, and never from the drain thread (see relay_queue.h /
  // issue #10). In the ordinary single-remote deployment this is a brief
  // map lookup that returns false, so relay costs one lock and no copy.
  //
  // Set by whichever path below reaches build_item(). It is deliberately
  // NOT probed here: the reorder-disabled fast path runs its gate first so
  // a dropped packet costs nothing (see below), and taking
  // subscribers_mutex_ up here would make every dropped packet pay for the
  // probe. The reorder path has to probe before building, because a
  // Buffer decision stores the built item.
  bool relay_wanted = false;

  // Build the PublishItem the socket-drain thread hands to the publish
  // worker. Only the CPU-bound deserialize + payload copy runs here — no
  // publisher create / graph query / publish (see issue #10 and
  // publishItem()); the drain thread must never make a call that can block
  // on a single subscriber. Factored into a lambda because the reorder
  // path (below) needs the built item *before* the admit decision (a
  // Buffer decision stores it), whereas the historical fast path builds it
  // only after the gate.
  auto build_item = [&]() {
    PublishItem item;
    // Only the non-relay path copies the payload here. When a relay form is
    // attached below it takes ownership of the whole MessageInternal, and
    // item.message is materialized from it at enqueuePublish() time
    // (materializePublishPayload) so the payload is never resident twice
    // while the item waits in the reorder buffer -- see publish_queue.h.
    if(!relay_wanted)
    {
      // Same empty-payload guard as materializePublishPayload: reserve(0)
      // throws from the rcl uint8_array layer, and an empty payload is a
      // legal message (std_msgs/msg/Empty) as well as something a sender
      // can put on the wire. Without the guard the same packet would throw
      // and be dropped here but publish normally on the relay path, i.e.
      // the outcome would depend on unrelated remote configuration.
      if(!outer_message.data.empty())
      {
        item.message.reserve(outer_message.data.size());
        memcpy(item.message.get_rcl_serialized_message().buffer,
               outer_message.data.data(), outer_message.data.size());
      }
      item.message.get_rcl_serialized_message().buffer_length = outer_message.data.size();
    }

    item.topic = topic;
    item.datatype = outer_message.datatype;
    item.reliability = outer_message.reliability;
    item.durability = outer_message.durability;
    item.history_depth = outer_message.history_depth;

    // Attach the relay form, when there is somewhere to relay to. The move
    // is safe -- and is why this is not a plain copy of outer_message
    // (which would duplicate the whole payload on the drain thread): this
    // lambda is called at most once per decodeData call, and nothing reads
    // outer_message afterwards.
    if(relay_wanted)
    {
      item.relay = std::make_unique<RelayItem>();
      item.relay->topic = topic;
      item.relay->source_node = source_info.node_name;
      item.relay->message = std::move(outer_message);
    }
    return item;
  };

  // Stale-packet / reorder gate. Applies only to sequenced packets when
  // drop_stale_packets is on; un-sequenced packets (never wrapped) and the
  // disabled case fall straight through to the plain build-and-push below.
  if(drop_stale_packets_ && source_info.sequenced)
  {
    std::shared_ptr<RemoteNode> remote_node;
    {
      std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
      auto it = remote_nodes_.find(source_info.node_name);
      if(it != remote_nodes_.end())
        remote_node = it->second;
    }

    if(remote_node && reorder_hold_window_ms_ > 0.0)
    {
      // Probe before building, because a Buffer decision stores the item
      // and it must carry its relay form with it.
      relay_wanted = hasRelayDestinations(topic, source_info.node_name);

      // Reorder buffer enabled (issue #35). The payload copy happens
      // BEFORE the admit call because a Buffer decision stores the item —
      // so the "gate before payload copy costs nothing" property of the
      // fast path below does NOT hold here (a dropped packet still paid
      // for its copy). RemoteNode::admitOrBuffer returns the items to
      // publish in wire order (an admitted packet, plus any buffered
      // packet its arrival released); a Buffer decision holds the item and
      // returns nothing to publish now (flushed later by spin_once).
      auto result = remote_node->admitOrBuffer(
        topic, source_info.packet_number, build_item(), get_clock()->now());
      if(result.decision == RemoteNode::AdmitDecision::Drop)
      {
        ++stale_dropped_count_;
        RCLCPP_INFO_STREAM_THROTTLE(get_logger(), *get_clock(), 5000,
          "drop_stale_packets: dropped stale message for topic '" << topic
          << "' (packet_number " << source_info.packet_number
          << " older than newest published); cumulative dropped: "
          << stale_dropped_count_);
      }
      for(auto& out : result.to_publish)
        enqueuePublish(std::move(out));
      return;
    }

    // Reorder buffer disabled (window == 0): the historical fast path.
    // Runs the gate before the payload copy AND before the relay probe, so
    // a dropped packet costs nothing -- no copy, and no subscribers_mutex_
    // acquisition. Byte-for-byte identical to pre-#35 behaviour. See
    // RemoteNode::admitForPublish; the high-water marks live in the
    // RemoteNode so they reset with its sequence state on remote restart.
    if(remote_node && !remote_node->admitForPublish(topic, source_info.packet_number))
    {
      ++stale_dropped_count_;
      RCLCPP_INFO_STREAM_THROTTLE(get_logger(), *get_clock(), 5000,
        "drop_stale_packets: dropped stale message for topic '" << topic
        << "' (packet_number " << source_info.packet_number
        << " older than newest published); cumulative dropped: "
        << stale_dropped_count_);
      return;
    }
  }

  relay_wanted = hasRelayDestinations(topic, source_info.node_name);
  enqueuePublish(build_item());
}

bool UDPBridge::hasRelayDestinations(const std::string& topic,
                                     const std::string& source_node) const
{
  // Cheap probe on the socket-drain thread: is there any remote OTHER than
  // the sender listed for this topic? subscribers_mutex_ is held only for a
  // map lookup and a short walk of the per-topic remote list (one entry in
  // every current deployment), never across I/O.
  std::lock_guard<std::mutex> lock(subscribers_mutex_);
  auto sub_it = subscribers_.find(topic);
  if(sub_it == subscribers_.end())
    return false;
  return hasRelayDestination(sub_it->second.remote_details, source_node);
}

void UDPBridge::relayToOtherRemotes(RelayItem&& item)
{
  // Runs on the relay worker (issue #51), never on the socket-drain thread:
  // send() below reaches Connection::send(), whose sendto() retry loop can
  // cost ~200 ms per stalled destination.
  //
  // Catch + log here, mirroring publishItem(): send() can throw
  // (ConnectionException on socket errors), and the worker is a plain
  // std::thread with no executor catch-all behind it. RelayQueue has a
  // last-resort barrier too, but it has no logger.
  try
  {
    // No lifecycle-state check here, deliberately, and unlike an earlier
    // draft of this function. The worker runs only between on_activate's
    // start() and on_deactivate's stop(), and stop() joins it before
    // anything else in on_deactivate runs — so reaching this point already
    // means the node was ACTIVE. Reading current_state_ from this thread
    // would be an unsynchronized read of state the executor thread writes,
    // and during the deactivate transition it reports DEACTIVATING, which
    // would abandon an in-flight item the join is willing to wait for.
    // publishItem makes no such call either; the join is the guarantee in
    // both cases. Items still queued when stop() runs are discarded and
    // counted by RelayQueue itself.

    // The loop rule needs a sender to exclude. An empty source_node (a
    // packet that never passed through unwrap(), so it carries no wrapped
    // source_node) cannot be excluded from anything, so forwarding it would
    // send it to every remote listing the topic — the sender included.
    // hasRelayDestinations() already refuses these; this is the same rule
    // restated at the sink, which is reachable independently of the probe.
    if(item.source_node.empty())
    {
      relay_drops_.record(RelayDropReason::UnnamedSender);
      return;
    }

    auto now = get_clock()->now();

    RemoteConnectionsList destinations;
    std::map<std::string, DestinationConfig> destination_config_by_remote;
    {
      std::lock_guard<std::mutex> lock(subscribers_mutex_);
      // find(), not operator[]: the routing table can be torn down between
      // the drain thread's probe and this call.
      auto sub_it = subscribers_.find(item.topic);
      if(sub_it == subscribers_.end())
      {
        relay_drops_.record(RelayDropReason::TopicGone);
        return;
      }
      auto& sub = sub_it->second;
      // Same rate limiting, same last_sent_time state, as locally published
      // traffic (callback()) -- with the sender excluded. The exclusion is
      // the loop rule: never send a message back to the remote it came
      // from. It is applied inside the helper before any last_sent_time is
      // touched, so the sender's rate-limit state is left untouched.
      destinations = selectRateLimitedConnections(sub.remote_details, now,
                                                  item.source_node);
      for(auto& remote_details: sub.remote_details)
      {
        if(remote_details.first == item.source_node)
          continue;
        auto& dc = destination_config_by_remote[remote_details.first];
        dc.destination_topic = remote_details.second.destination_topic;
        dc.reliability       = remote_details.second.reliability;
        dc.durability        = remote_details.second.durability;
        dc.history_depth     = remote_details.second.history_depth;
      }

      // Seed the aggregate ("", "") statistics row, exactly as callback()
      // does for a locally published message. That row is the per-message
      // count for the topic: MessageStatistics::get() emits one
      // TopicStatistics per (destination_node, connection_id) pair it
      // sees, so without this seed a relayed message contributes only to
      // the per-destination rows and the topic's aggregate
      // messages_per_second counts local publications alone. Seeded here,
      // under the same lock and before the rate-limit check, so a relayed
      // message counts once whether or not any destination was due --
      // again matching callback().
      MessageSizeData relay_size_data;
      relay_size_data.message_size = item.message.data.size();
      relay_size_data.timestamp = now;
      relay_size_data.send_results[""][""];
      sub.statistics.add(relay_size_data);
    }

    if(destinations.empty())
    {
      // Not loss: the routing table may list other remotes and none is due
      // under its `period` yet. Counted separately from the three above so
      // the diagnostic shows it without inflating the loss figure.
      relay_drops_.record(RelayDropReason::NoDestinationDue);
      return;
    }

    // Forward the message as received -- no re-serialization of the payload.
    // Only the routing/QoS fields are rewritten per destination, by
    // applyRelayDestination: destination_topic and QoS from that remote's
    // config, and source_topic set to the hub-local topic so the receiver's
    // "destination_topic, else source_topic" fallback lands on the hub's
    // name rather than the upstream publisher's (which two boats can share).
    // See relay_item.h for why, and doc/relay_design.md.
    MessageInternal message_internal = std::move(item.message);

    // Per-destination failure isolation (see relay_send.h): a throw from one
    // remote's send -- ConnectionException on the ordinary Timeout path -- is
    // logged and the fan-out continues to the remaining remotes.
    relayToEachDestination(message_internal, item.topic, destinations,
      destination_config_by_remote,
      [&](const std::string& remote_name,
          const std::vector<std::string>& connection_ids,
          MessageInternal& rewritten)
      {
        // `rewritten` is message_internal with this remote's
        // destination_topic and QoS already applied.
        RemoteConnectionsList connections;
        connections[remote_name] = connection_ids;
        // The existing send path: fragmenting, wrapped-packet sequencing and
        // the per-connection AIMD admission control of #43/#52 all apply to
        // relayed traffic exactly as they do to locally published traffic.
        // The one difference is how a failing link is reported: a hub
        // forwards every carried topic to every spoke, so one spoke over
        // the horizon — an ordinary field condition, not a fault — would
        // otherwise emit an ERROR per topic per send. WARN keeps it visible
        // without teaching the operator to ignore ERROR.
        auto size_data = send(rewritten, connections, false,
                              SendFailureReport::warning);
        size_data.message_size = rewritten.data.size();
        {
          // Relayed bytes go into the topic's statistics alongside
          // local-origin ones, so they are visible in ~/topic_statistics
          // rather than invisible. They are summed, not separated: the
          // per-destination send_results breakdown says where the traffic
          // went, but a per-source split would need a wire-format change
          // to TopicStatistics. See doc/relay_design.md, Statistics.
          std::lock_guard<std::mutex> lock(subscribers_mutex_);
          auto sub_it = subscribers_.find(item.topic);
          if(sub_it != subscribers_.end())
            sub_it->second.statistics.add(size_data);
        }
      },
      [&](const std::string& remote_name, const std::string& what)
      {
        // Count it first, then log. The count is what makes the throttle
        // safe: a throttled WARN shows a persistent failure but not how
        // much was lost to it, and a message that fails here was never
        // handed to a Connection, so the resend layer has nothing to
        // retransmit. relay_drops_ carries it into `dropped_by_sink`,
        // `sink_drop_reasons` and the relay-queue diagnostic's WARN
        // (relay_drops.h), one count per failing destination.
        relay_drops_.record(RelayDropReason::SendFailed);
        // WARN, throttled, for the same reason send() reports relay link
        // failures at WARN (issue #51): an over-horizon spoke is a normal
        // field condition, and a hub hits it once per forwarded topic per
        // send. Unthrottled ERROR here would bury the log, and the loss it
        // stands for is now counted above rather than being inferred from
        // how often the line appears.
        RCLCPP_WARN_STREAM_THROTTLE(get_logger(), *get_clock(), 5000,
          "relay worker: send to '" << remote_name << "' failed for topic '"
          << item.topic << "' (from '" << item.source_node << "'): " << what);
      });
  }
  catch(const ConnectionException& e)
  {
    // ConnectionException has no base class, so it is not covered by the
    // std::exception handler below (connection.h).
    RCLCPP_ERROR_STREAM(get_logger(),
      "relay worker: dropping message for '" << item.topic
      << "' from '" << item.source_node << "': " << e.getMessage());
  }
  catch(const std::exception& e)
  {
    RCLCPP_ERROR_STREAM(get_logger(),
      "relay worker: dropping message for '" << item.topic
      << "' from '" << item.source_node << "': " << e.what());
  }
}

void UDPBridge::enqueuePublish(PublishItem&& item)
{
  // The single admit-to-publish point. Everything the stale/reorder gate
  // lets through comes here -- the immediate path in decodeData, the items
  // a gap-filler releases, and the items flushExpiredBuffer releases on
  // window expiry -- and nothing the gate drops does.
  //
  // Relay (issue #51) is therefore gated exactly as local publication is: a
  // packet the hub judges stale or superseded is never forwarded. It is not
  // relayed at buffer time either, since a buffered packet can still be
  // dropped; it waits, attached to its PublishItem, and goes out when (and
  // only when) the buffer releases it, in the same wire order.
  if(item.relay)
  {
    // The relay form has been the sole holder of the payload since
    // decodeData built the item, so the publish copy is taken here -- once,
    // on an admitted packet only, and never concurrently with the relay
    // form sitting in the reorder buffer (see publish_queue.h).
    materializePublishPayload(item);
    relay_queue_.push(std::move(*item.relay));
    item.relay.reset();
  }
  publish_queue_.push(std::move(item));
}

void UDPBridge::publishItem(PublishItem&& item)
{
  // Runs on the publish worker (issue #10). Find-or-create the destination
  // publisher (first arrival also sends bridge info) and publish. Any
  // blocking here — a RELIABLE publish() to a dead-but-still-matched
  // subscriber, or first-arrival rmw discovery in create_generic_publisher()
  // — stalls only this worker, never the socket-drain thread.
  //
  // Catch + log here, mirroring UDPBridge::decode's catch-all: before #10
  // this work ran inside decode() on the socket-drain executor thread, whose
  // catch-all logged and dropped bad packets instead of tearing the node
  // down. The worker is a plain std::thread (PublishQueue has a last-resort
  // barrier too, but it can't log), and every step here can throw —
  // create_generic_publisher on an unknown / cross-version datatype, publish
  // from the rcl layer, and sendBridgeInfo's ConnectionException on socket
  // errors. Reproduce that survivable-error contract here.
  try
  {
    // Three-phase lookup so create_generic_publisher() runs WITHOUT
    // publishers_mutex_ held; holding the lock across rmw discovery /
    // type-support resolution would block every concurrent publisher lookup.
    // (A single worker today means no concurrent publishItem; the pattern is
    // retained for the planned per-publisher-worker evolution — see the #10
    // plan's deferred cross-topic-isolation follow-up.)
    rclcpp::GenericPublisher::SharedPtr publisher;
    bool first_arrival = false;

    // Phase 1: check map under lock.
    {
      std::lock_guard<std::mutex> lock(publishers_mutex_);
      auto it = publishers_.find(item.topic);
      if(it != publishers_.end())
        publisher = it->second;
    }

    // Phase 2: if missing, do the slow rmw call without holding the lock.
    if(!publisher)
    {
      // Resolve per-topic QoS from MessageInternal (sender-advertised),
      // falling back to package defaults when fields are empty/zero —
      // see udp_bridge/doc/qos_design.md for the contract.
      auto qos = resolveDestinationPublisherQos(
        item.reliability,
        item.durability,
        item.history_depth);
      auto new_publisher = create_generic_publisher(item.topic, item.datatype, qos);

      // Phase 3: re-acquire and try-emplace. If a future multi-worker setup
      // raced and inserted first, we use theirs and discard ours.
      {
        std::lock_guard<std::mutex> lock(publishers_mutex_);
        auto [it, inserted] = publishers_.try_emplace(item.topic, new_publisher);
        publisher = it->second;
        first_arrival = inserted;
      }
    }

    // sendBridgeInfo and publish run without publishers_mutex_ held — both
    // are slow operations that should not block the publishers_ map.
    if(first_arrival)
      sendBridgeInfo();

    publisher->publish(item.message);
  }
  catch(const std::exception& e)
  {
    RCLCPP_ERROR_STREAM(get_logger(),
      "publish worker: dropping message for '" << item.topic
      << "' (type '" << item.datatype << "'): " << e.what());
  }
}

void UDPBridge::decodeBridgeInfo(std::vector<uint8_t> const &message, const SourceInfo& source_info)
{
  auto bridge_info = deserialize<BridgeInfo>(message);

  std::shared_ptr<RemoteNode> remote_node;
  {
    std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
    remote_node = remote_nodes_[source_info.node_name];
    if(!remote_node)
    {
      remote_node = std::make_shared<RemoteNode>(source_info.node_name, name_, *this);
      remote_nodes_[source_info.node_name] = remote_node;
    }
  }
  remote_node->update(bridge_info, source_info);
}

void UDPBridge::decodeResendRequest(std::vector<uint8_t> const &message, const SourceInfo& source_info)
{
  // Catch deserialization failure here for diagnostic clarity on
  // the coordinated-redeploy / mismatched-schema scenario. Executor
  // survival is already provided by UDPBridge::decode's outer
  // catch-all, which logs an ERROR and continues for any exception
  // thrown by a decode* path. What this
  // inner catch buys is a more specific WARN that names this site
  // ("Failed to deserialize ResendRequest from <node> (<host>:
  // <port>)") instead of the outer's generic
  // "decoding error on packet of type N from <node>" — useful when
  // an operator is chasing a known coordinated-redeploy mismatch
  // and wants to confirm it's the ResendRequest schema that's
  // out of sync. The ResendRequest.msg comment documents this
  // site-specific contract. Mirror the pattern at other decode*
  // sites in a follow-up only if the diagnostic-clarity payoff is
  // worth the duplication — see the parallel gap in
  // decodeMessageInternal (predates this change); the outer catch
  // is enough for survival there too.
  ResendRequest rr;
  try
  {
    rr = deserialize<ResendRequest>(message);
  }
  catch(const std::exception& e)
  {
    RCLCPP_WARN_STREAM(get_logger(),
      "Failed to deserialize ResendRequest from " << source_info.node_name
      << " (" << source_info.host << ":" << source_info.port
      << "): " << e.what() << " — dropping packet");
    return;
  }
  // Normalize the incoming id to the canonical wire form. Well-behaved
  // peers send canonical ids by construction: PR #25's truncation-
  // contract change runs every connection-id through
  // truncate_connection_id at insertion (RemoteNode::newConnection and
  // Connection::Connection), so connection->id() is canonical and the
  // sender's stamp at resendMissingPackets is canonical too. Clamping
  // here defends against a misbehaving peer or an older bridge that
  // stamps the full untruncated id; without it:
  //   1) The dispatch lookup against the canonical-keyed connections_
  //      map would miss for any id ≥ maximum_connection_id_size — a
  //      silent resend-routing failure. Clamping rewrites the stamp
  //      to the canonical form so dispatch finds the connection.
  //   2) RemoteNode::dispatch_miss_warned_ids_ inserts rr.connection_id
  //      on a lookup miss. Round-4 (PR #25) replaced the unbounded
  //      std::set with a FIFO capped at kDispatchMissWarnedCap, so the
  //      bookkeeping count is bounded by code. The receive-side clamp
  //      remains useful on top of that cap: it bounds each table
  //      entry's *size* to 7 bytes so the table stays small even at
  //      full capacity, and it normalizes incoming ids so the
  //      bookkeeping doesn't double-track an id under its truncated
  //      and untruncated forms.
  if(rr.connection_id.size() > maximum_connection_id_size - 1)
    rr.connection_id.resize(maximum_connection_id_size - 1);
  auto now = get_clock()->now();

  // Issue #23: route the resend response back via the single connection
  // the request was stamped for, not a broadcast across every active path.
  // The routing logic lives on RemoteNode (which owns the connection map);
  // we snapshot the RemoteNode shared_ptr here under remote_nodes_mutex_
  // and call dispatchResendRequest outside the lock so its socket I/O
  // doesn't run under our map mutex.
  std::shared_ptr<RemoteNode> remote_node;
  {
    std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
    auto it = remote_nodes_.find(source_info.node_name);
    if(it != remote_nodes_.end())
      remote_node = it->second;
  }
  if(remote_node)
    remote_node->dispatchResendRequest(rr, socket_, now);
}

void UDPBridge::decodeTopicStatistics(std::vector<uint8_t> const &message, const SourceInfo& source_info)
{
  auto topic_statistics = deserialize<TopicStatisticsArray>(message);

  std::shared_ptr<RemoteNode> remote_node;
  {
    std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
    auto it = remote_nodes_.find(source_info.node_name);
    if(it != remote_nodes_.end())
      remote_node = it->second;
  }
  if(remote_node)
    remote_node->publishTopicStatistics(topic_statistics);
}


void UDPBridge::addSubscriberConnection(std::string const &source_topic,
                                        std::string const &destination_topic,
                                        uint32_t queue_size, float period,
                                        std::string remote_node,
                                        std::string connection_id,
                                        std::string reliability,
                                        std::string durability,
                                        uint32_t history_depth)
{
  if(!remote_node.empty())
  {
    {
      std::lock_guard<std::mutex> lock(subscribers_mutex_);
      auto& sub = subscribers_[source_topic];
      sub.queue_size = std::max(queue_size, uint32_t(1));
      auto& rd = sub.remote_details[remote_node];
      rd.destination_topic = destination_topic;
      rd.connection_rates[connection_id].period = period;
      // QoS overrides — empty/zero retains existing or default behavior so
      // that re-adding a subscription doesn't accidentally clear earlier
      // per-topic QoS that was set elsewhere.
      if(!reliability.empty())
        rd.reliability = reliability;
      if(!durability.empty())
      {
        rd.durability = durability;
        // Source-side subscription durability follows the strongest
        // per-topic setting across remotes for this source topic.
        // transient_local overrides volatile so a latched source is
        // received even if only one consumer needs it.
        if(durability == "transient_local")
          sub.durability = "transient_local";
      }
      if(history_depth != 0)
        rd.history_depth = history_depth;
    }
    // sendBridgeInfo runs without subscribers_mutex_ held (it acquires
    // its own locks). Calling it inside the locked scope above would
    // deadlock since it re-acquires subscribers_mutex_.
    sendBridgeInfo();
  }
}

void UDPBridge::removeSubscriberConnection(std::string const &source_topic,
                                           std::string const &remote_node,
                                           std::string const &connection_id)
{
  // The map manipulation (extracted to removeSubscriberConnectionFrom so it can
  // be unit-tested) runs under subscribers_mutex_. The local generic
  // subscription it hands back is reset OFF the lock as hygiene — destroying a
  // subscription does rmw teardown we don't want to run under the hot
  // subscribers_mutex_. It is safe either way: an in-flight forwarding callback
  // holds its own strong ref to the subscription via the executor, so reset()
  // never frees one mid-callback and never blocks on it.
  SubscriberRemoval removal;
  {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    removal = removeSubscriberConnectionFrom(subscribers_, source_topic, remote_node, connection_id);
  }
  removal.expired_subscription.reset();  // destroys the generic subscription, off-lock
  if(removal.changed)
    sendBridgeInfo();
}

void UDPBridge::updateLocalSubscriptions()
{
  // Three-phase lookup so get_publishers_info_by_topic() and
  // create_generic_subscription() (both slow rmw calls) run WITHOUT
  // subscribers_mutex_ held. Holding the lock across them would block
  // callback() (republish-group reader) and statsReportCallback /
  // bridgeInfoCallback (periodic-group readers).
  struct PendingCreate
  {
    std::string source_topic;
    std::string durability;
    uint32_t queue_size;
  };

  // Phase 1: snapshot which topics need a new subscription, under lock.
  std::vector<PendingCreate> pending;
  {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    for(auto& subscriber: subscribers_)
      if(!subscriber.second.subscription)
        pending.push_back({subscriber.first,
                           subscriber.second.durability,
                           subscriber.second.queue_size});
  }

  // Phase 2: do the slow rmw work for each, without the lock.
  for(const auto& p: pending)
  {
    auto info = get_publishers_info_by_topic(p.source_topic);
    if(info.empty())
      continue;
    std::string topic_type = info.front().topic_type();
    auto source_topic = p.source_topic;
    auto cb = [this, source_topic, topic_type](std::shared_ptr<rclcpp::SerializedMessage> message)
    {
      this->callback(source_topic, topic_type, message);
    };

    // Source-side QoS: best_effort (accepts any publisher) plus
    // per-topic durability (transient_local when configured, so latched
    // source values reach the bridge). See qos_design.md.
    auto qos = resolveSourceSubscriptionQos(p.durability, p.queue_size);

    rclcpp::SubscriptionOptions options;
    options.ignore_local_publications = true;
    // Forwarding subscriptions run in republish_group_ so a stalled
    // remote publisher cannot starve the socket-drain group.
    options.callback_group = republish_group_;
    auto subscription = create_generic_subscription(source_topic, topic_type, qos, cb, options);

    // Phase 3: re-acquire and assign — but only if no one beat us. Race
    // guard: a concurrent updateLocalSubscriptions tick or
    // addSubscriberConnection could have already created the
    // subscription; in that case we drop ours.
    {
      std::lock_guard<std::mutex> lock(subscribers_mutex_);
      auto it = subscribers_.find(p.source_topic);
      if(it != subscribers_.end() && !it->second.subscription)
        it->second.subscription = subscription;
    }
  }
}

void UDPBridge::decodeSubscribeRequest(std::vector<uint8_t> const &message, const SourceInfo& source_info)
{
  auto remote_request = deserialize<RemoteSubscribeInternal>(message);

  if(remote_request.operation == RemoteSubscribeInternal::OPERATION_UNSUBSCRIBE)
  {
    removeSubscriberConnection(remote_request.source_topic, source_info.node_name, remote_request.connection_id);
    return;
  }
  addSubscriberConnection(remote_request.source_topic, remote_request.destination_topic, remote_request.queue_size, remote_request.period, source_info.node_name, remote_request.connection_id);
}

void UDPBridge::unwrap(const std::vector<uint8_t>& message, const SourceInfo& source_info)
{
  if(message.size() > sizeof(SequencedPacketHeader))
  {
    const SequencedPacket* wrapped_packet = reinterpret_cast<const SequencedPacket*>(message.data());

    // Read the fixed-size wire fields with an explicit bound (issue #51).
    // `source_node` is `char[maximum_node_name_size]` and nothing on the
    // wire guarantees a null terminator: our own write side memsets the
    // field before copying, so packets we sent always terminate, but a
    // corrupted or foreign packet can fill all 24 bytes with non-zero data
    // and constructing a std::string from the bare pointer would then read
    // past the field — and past the receive buffer, if no null byte
    // happens to follow it. Same for `connection_id`, which is handed
    // straight to the connection lookup below and to RemoteNode::unwrap.
    const std::string source_node = wire_field_to_string(
      wrapped_packet->source_node, maximum_node_name_size);
    const std::string connection_id = wire_field_to_string(
      wrapped_packet->connection_id, maximum_connection_id_size);

    auto updated_source_info = source_info;
    updated_source_info.node_name = source_node;
    // Carry the wrapped sequence number into the recursive decode so the
    // stale-packet gate in decodeData can compare it against the per-topic
    // high-water mark. Propagates through the Compressed and Fragment
    // re-decode paths; for a reassembled message it is the completing
    // fragment's number, which is sufficient to order whole messages.
    updated_source_info.packet_number = wrapped_packet->packet_number;
    updated_source_info.sequenced = true;

    std::shared_ptr<RemoteNode> remote;
    {
      std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
      auto remote_iterator = remote_nodes_.find(source_node);
      if(remote_iterator != remote_nodes_.end())
        remote = remote_iterator->second;

      if(!remote)
      {
        if(source_node == name_)
        {
          RCLCPP_ERROR_STREAM(get_logger(), "Received a packet from a node with our name: " << name_ << " connection: " << connection_id << " host: " << source_info.host << " port: " << source_info.port);
          return;
        }
        // Loud on the misconfiguration that silently defeats the relay
        // loop rule (issues #51, #67): a statically-configured remote
        // whose own name differs from the hub's `remotes_list` entry for
        // it arrives here as an unrecognised sender, gets a second
        // RemoteNode of its own, and — because subscribers_ keyed that
        // remote by the *entry* — is excluded from nothing when its
        // traffic is relayed, so the hub echoes it straight back. A
        // genuinely dynamic remote (CONNECT / add_remote / a subscribe
        // request from an unconfigured host) also lands here and is
        // legitimate, so this warns rather than refuses; it is skipped
        // entirely when no remote is statically configured.
        if(!configured_remote_names_.empty() &&
           !configured_remote_names_.count(source_node))
          RCLCPP_WARN_STREAM_THROTTLE(get_logger(), *get_clock(), 10000,
            "packet from '" << source_node
            << "' which matches no configured remote. If this is a remote in"
               " remotes_list, rename its remotes_list entry to '"
            << source_node
            << "' — otherwise relayed traffic can be sent back to it.");
        remote = std::make_shared<RemoteNode>(source_node, name_, *this);
        remote_nodes_[source_node] = remote;
      }
    }
    // remote->unwrap and the recursive decode() run without remote_nodes_mutex_
    // held; decode() will reacquire it for any RemoteNode lookups it needs.
    try
    {
      decode(remote->unwrap(message, updated_source_info), updated_source_info);
    }
    catch(const std::exception& e)
    {
      RCLCPP_ERROR_STREAM(get_logger(), "problem decoding packet: " << e.what());
    }
  }
  else
    RCLCPP_ERROR_STREAM(get_logger(), "Incomplete wrapped packet of size: " << message.size());
}

void UDPBridge::decodeConnection(std::vector<uint8_t> const &message, const SourceInfo& source_info)
{
  auto connection_internal = deserialize<ConnectionInternal>(message);

  switch(connection_internal.operation)
  {
    case ConnectionInternal::OPERATION_CONNECT:
      {
        // Resolve / create the RemoteNode under remote_nodes_mutex_ only.
        // Per-remote work (newConnection -> getaddrinfo, setHostAndPort,
        // setRateLimit) is done outside the lock and is serialized by
        // RemoteNode::state_mutex_ / Connection::config_mutex_. Holding
        // remote_nodes_mutex_ across blocking DNS would let a single
        // CONNECT-decode stall every other path that touches
        // remote_nodes_ — including the socket-drain group's own lookups
        // — re-introducing the wedge this PR is meant to fix.
        std::shared_ptr<RemoteNode> remote;
        {
          std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
          remote = remote_nodes_[source_info.node_name];
          if(!remote)
          {
            remote = std::make_shared<RemoteNode>(source_info.node_name, name_, *this);
            remote_nodes_[source_info.node_name] = remote;
          }
        }
        uint16_t port = source_info.port;
        if(connection_internal.return_port != 0)
          port = connection_internal.return_port;
        auto host = source_info.host;
        if(!connection_internal.return_host.empty())
          host = connection_internal.return_host;
        auto connection = remote->connection(connection_internal.connection_id);
        if(!connection)
          connection = remote->newConnection(connection_internal.connection_id, host, port);
        else
          connection->setHostAndPort(host, port);
        if(connection_internal.return_maximum_bytes_per_second > 0)
        {
          connection->setRateLimit(connection_internal.return_maximum_bytes_per_second);
          warnIfAdmissionInert(get_logger(), *get_clock(), source_info.node_name,
                               connection_internal.connection_id, *connection);
        }
        connection_internal.operation = ConnectionInternal::OPERATION_CONNECT_ACKNOWLEDGE;
        connection_internal.return_host = connection->returnHost();
        connection_internal.return_port = connection->returnPort();
        // send() acquires its own locks; call without remote_nodes_mutex_.
        send(connection_internal, source_info.node_name, true);
      }
      break;
    case ConnectionInternal::OPERATION_CONNECT_ACKNOWLEDGE:
      {
        std::scoped_lock lock(remote_nodes_mutex_, pending_connections_mutex_);
        if(pending_connections_.find(connection_internal.sequence_number) != pending_connections_.end())
        {
          auto remote = remote_nodes_[source_info.node_name];
          if(!remote)
          {
            remote = std::make_shared<RemoteNode>(source_info.node_name, name_, *this);
            remote_nodes_[source_info.node_name] = remote;
          }
          auto connection = remote->connection(connection_internal.connection_id);
          auto pending_connection = pending_connections_[connection_internal.sequence_number].connection;
          if(pending_connection)
          {
            if(!connection)
            {
              // adoptConnection registers the pending Connection on the
              // RemoteNode under its state_mutex_; without it, the
              // pending_connection would be discarded here and later
              // remote->connection(id) lookups would return nullptr.
              remote->adoptConnection(pending_connection);
              connection = pending_connection;
            }
          }
          pending_connections_.erase(connection_internal.sequence_number);
        }
      }
      break;
    default:
      RCLCPP_WARN_STREAM(get_logger(), "Unhandled ConnectionInternal operation type: " << connection_internal.operation);
  }
}


void UDPBridge::resendMissingPackets()
{
  // Snapshot remotes under the lock; getMissingPackets, connections(),
  // and send() run unlocked.
  std::vector<std::pair<std::string, std::shared_ptr<RemoteNode>>> remotes;
  {
    std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
    remotes.assign(remote_nodes_.begin(), remote_nodes_.end());
  }
  for(auto remote: remotes)
  {
    if(!remote.second)
      continue;
    auto rr_base = remote.second->getMissingPackets();
    if(rr_base.missing_packets.empty())
      continue;
    // Issue #23: stamp one copy per live connection and send each via
    // a RemoteConnectionsList that names only that connection — so the
    // remote bridge gets one ResendRequest per path and can route the
    // response back via the same path it arrived on (via
    // RemoteNode::dispatchResendRequest on the receive side). Replaces
    // the previous broadcast-to-all-connections send, which amplified
    // resend volume 2-4x in the both-paths-alive and one-path-dead
    // regimes documented in the issue.
    auto connections = remote.second->connections();
    if(connections.empty())
      continue;
    RCLCPP_DEBUG_STREAM(get_logger(),
      "Sending a request to resend " << rr_base.missing_packets.size()
      << " packets to " << remote.first << " across "
      << connections.size() << " connection(s)");
    for(const auto& connection: connections)
    {
      if(!connection)
        continue;
      ResendRequest rr = rr_base;
      // Honor the on-wire connection-id limit. SequencedPacketHeader
      // carries a fixed `char connection_id[maximum_connection_id_size]`
      // (packet.h:82) and WrappedPacket truncates to
      // `maximum_connection_id_size - 1` (wrapped_packet.cpp:31-32). As
      // of the truncation-contract canonicalization (PR #25), every
      // path that inserts into connections_ runs the id through
      // truncate_connection_id at construction (RemoteNode::newConnection
      // and Connection::Connection), so `connection->id()` is already
      // canonical here — the assign-with-substring below is a defensive
      // no-op in the canonical-id steady state. Kept so a future
      // refactor that lets a longer-than-7 id reach this point still
      // can't desync the wire stamp from the canonical key the receiver
      // is keying its connections_ map on.
      rr.connection_id.assign(connection->id(), 0,
                              maximum_connection_id_size - 1);
      RemoteConnectionsList rcl;
      // Route via the same connection on this bridge's connections_.
      // Post-PR #25 round-10, `connection->id()` is already the
      // canonical (truncated) wire form and `RemoteNode::connection()`
      // canonicalizes its lookup input, so either form works — we use
      // `connection->id()` for consistency with the rr.connection_id
      // stamp above.
      rcl[remote.first] = {connection->id()};
      send(rr, rcl, true);
    }
  }
}


template <typename MessageType> MessageSizeData UDPBridge::send(MessageType const &message, const RemoteConnectionsList& remotes, bool is_overhead, SendFailureReport failure_report)
{
  rclcpp::Serialization<MessageType> serializer;
  rclcpp::SerializedMessage serialized_message;
  serializer.serialize_message(&message, &serialized_message);

  auto serial_size = serialized_message.size();

  std::vector<uint8_t> packet_data(sizeof(PacketHeader)+serial_size);
  Packet * packet = reinterpret_cast<Packet *>(packet_data.data());
  memcpy(&packet->data, serialized_message.get_rcl_serialized_message().buffer, serial_size);
  packet->type = packetTypeOf(message);

  packet_data = compress(packet_data);

  auto fragments = fragment(packet_data);

  auto size_data = send(fragments, remotes, is_overhead, failure_report);

  size_data.message_size = serial_size;
  size_data.fragment_count = fragments.size();
  return size_data;
}

template <typename MessageType>
MessageSizeData UDPBridge::send(MessageType const &message, const std::string& remote, bool is_overhead, SendFailureReport failure_report)
{
  RemoteConnectionsList rcl;
  rcl[remote];
  return send(message, rcl, is_overhead, failure_report);
}


// wrap and send a series of packets that should be sent as a group, such as fragments.
template<> MessageSizeData UDPBridge::send(const std::vector<std::vector<uint8_t> >& data, const RemoteConnectionsList& remotes, bool is_overhead, SendFailureReport failure_report)
{
  MessageSizeData size_data;
  std::vector<WrappedPacket> wrapped_packets;

  auto now = get_clock()->now();

  for(auto packet: data)
  {
    // fetch_add(1) gives a unique packet number under concurrent calls
    // from multiple callback groups (republish/socket-drain/periodic).
    uint64_t packet_number = next_packet_number_.fetch_add(1);
    last_packet_number_assign_time_ns_.store(now.nanoseconds());
    wrapped_packets.push_back(WrappedPacket(packet_number, packet, now));
    size_data.sent_size += wrapped_packets.back().packet.size();
  }

  if(wrapped_packets.empty())
    return size_data;

  size_data.timestamp = wrapped_packets.front().timestamp;

  // Resolve all (remote, connections) under the maps' locks, copy out
  // shared_ptrs, then release before doing the actual send (which performs
  // network I/O and can stall).
  std::map<std::string, std::vector<std::shared_ptr<Connection>>> connections_by_remote;
  {
    std::scoped_lock lock(remote_nodes_mutex_, pending_connections_mutex_);
    for(auto remote: remotes)
    {
      std::vector<std::shared_ptr<Connection>> connections;
      if(remote.first.empty()) // connection request?
      {
        for(auto sequence_number_str: remote.second)
          for(auto pending_connection: pending_connections_)
            if(std::to_string(pending_connection.first) == sequence_number_str)
              connections.push_back(pending_connection.second.connection);
      }
      else
      {
        auto remote_node = remote_nodes_.find(remote.first);
        if(remote_node != remote_nodes_.end())
        {
          if(remote.second.empty())
            connections = remote_node->second->connections();
          else
            for(auto connection_id: remote.second)
              connections.push_back(remote_node->second->connection(connection_id));
        }
      }
      connections_by_remote[remote.first] = std::move(connections);
    }
  }

  // Per-CONNECTION failure isolation. Connection::send() throws
  // ConnectionException (no base class) on its ordinary Timeout path; left
  // to propagate, one throw would abandon a remote's remaining REDUNDANT
  // connections -- the very paths that exist so a single failing link does
  // not lose the message -- and abandon the statistics record for the whole
  // send, so the failure would not even show up in ~/topic_statistics.
  // Record the failure, log it, and carry on to the next connection.
  // Isolation at the caller (sendToEachDestination, for relay) is per
  // remote, which is a coarser unit than this.
  for(const auto& entry: connections_by_remote)
  {
    for(const auto& connection: entry.second)
      if(connection)
      {
        const std::string connection_id = connection->id();
        callIsolated(connection_id,
          [&]
          {
            auto result = connection->send(wrapped_packets, socket_, name_, is_overhead, now);
            size_data.send_results[entry.first][connection_id] = result;
          },
          [&](const std::string& id, const std::string& what)
          {
            size_data.send_results[entry.first][id] = SendResult::failed;
            // Severity is the caller's call (issue #51). An unreachable
            // destination is a fault for locally-originated traffic, but
            // for relay it is an over-horizon spoke — normal in the field,
            // and hit once per forwarded topic per send, so ERROR there
            // would be pure noise. Both branches are throttled and both
            // name the connection, so nothing is hidden either way.
            if(failure_report == SendFailureReport::warning)
              RCLCPP_WARN_STREAM_THROTTLE(get_logger(), *get_clock(), 5000,
                "send to '" << entry.first << "' connection '" << id
                << "' failed: " << what);
            else
              RCLCPP_ERROR_STREAM_THROTTLE(get_logger(), *get_clock(), 5000,
                "send to '" << entry.first << "' connection '" << id
                << "' failed: " << what);
          });
      }
  }

  return size_data;
}

void UDPBridge::cleanupSentPackets()
{
  auto now = get_clock()->now();
  if(now.nanoseconds() == 0)
    return;
  auto old_enough = now - rclcpp::Duration(kSentPacketTTL);

  // Snapshot connections under the lock; cleanup_sent_packets takes its own
  // per-Connection mutex.
  std::vector<std::shared_ptr<Connection>> connections;
  {
    std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
    for(auto& remote: remote_nodes_)
      if(remote.second)
        for(auto connection: remote.second->connections())
          if(connection)
            connections.push_back(connection);
  }
  for(auto& connection: connections)
    connection->cleanup_sent_packets(old_enough);
}


void UDPBridge::remoteSubscribe(
  const std::shared_ptr<udp_bridge::Subscribe::Request> request,
  std::shared_ptr<udp_bridge::Subscribe::Response> response)
{
  RCLCPP_INFO_STREAM(get_logger(), "subscribe: remote: " << request->remote << ":" << " connection: " << request->connection_id << " source topic: " << request->source_topic << " destination topic: " << request->destination_topic);

  udp_bridge::RemoteSubscribeInternal remote_request;
  remote_request.source_topic = request->source_topic;
  remote_request.destination_topic = request->destination_topic;
  remote_request.queue_size = request->queue_size;
  remote_request.period = request->period;
  remote_request.connection_id = request->connection_id;

  send(remote_request, request->remote, true);

}

void UDPBridge::remoteAdvertise(
  const std::shared_ptr<udp_bridge::Subscribe::Request> request,
  std::shared_ptr<udp_bridge::Subscribe::Response> response)
{
  (void)response;
  std::shared_ptr<Connection> connection;
  {
    std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
    auto remote = remote_nodes_.find(request->remote);
    if(remote == remote_nodes_.end())
      return;
    connection = remote->second->connection(request->connection_id);
  }
  if(!connection)
    return;

  // addSubscriberConnection acquires its own locks (and calls sendBridgeInfo
  // internally); calling sendBridgeInfo a second time here is redundant.
  addSubscriberConnection(request->source_topic, request->destination_topic, request->queue_size, request->period, request->remote, request->connection_id);
}

void UDPBridge::removeSubscribe(
  const std::shared_ptr<udp_bridge::Subscribe::Request> request,
  std::shared_ptr<udp_bridge::Subscribe::Response> response)
{
  (void)response;
  RCLCPP_INFO_STREAM(get_logger(), "remove subscribe: remote: " << request->remote << " connection: " << request->connection_id << " source topic: " << request->source_topic);

  // Mirror of remoteSubscribe: tell the remote to stop pushing source_topic to
  // us. The remote's decodeSubscribeRequest sees OPERATION_UNSUBSCRIBE and calls
  // removeSubscriberConnection on its side.
  udp_bridge::RemoteSubscribeInternal remote_request;
  remote_request.source_topic = request->source_topic;
  remote_request.connection_id = request->connection_id;
  remote_request.operation = udp_bridge::RemoteSubscribeInternal::OPERATION_UNSUBSCRIBE;

  send(remote_request, request->remote, true);
}

void UDPBridge::removeAdvertise(
  const std::shared_ptr<udp_bridge::Subscribe::Request> request,
  std::shared_ptr<udp_bridge::Subscribe::Response> response)
{
  (void)response;
  RCLCPP_INFO_STREAM(get_logger(), "remove advertise: remote: " << request->remote << " connection: " << request->connection_id << " source topic: " << request->source_topic);

  // Mirror of remoteAdvertise: stop forwarding our local source_topic to the
  // remote. Purely local; idempotent if the forwarding is already gone.
  removeSubscriberConnection(request->source_topic, request->remote, request->connection_id);
}

void UDPBridge::statsReportCallback()
{
  if(get_current_state().id() != lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE)
  {
    return;
  }

  TopicStatisticsArray tsa;
  {
    std::lock_guard<std::mutex> lock(subscribers_mutex_);
    for(auto& subscriber: subscribers_)
    {
      for(auto ts: subscriber.second.statistics.get())
      {
        ts.source_node = name_;
        ts.source_topic = subscriber.first;
        auto details = subscriber.second.remote_details.find(ts.destination_node);
        if(details != subscriber.second.remote_details.end())
          ts.destination_topic = details->second.destination_topic;
        tsa.topics.push_back(ts);
      }
    }
  }

  topic_statistics_publisher_->publish(tsa);

  send(tsa, allRemotes(), true);
}

std::vector<std::vector<uint8_t> > UDPBridge::fragment(const std::vector<uint8_t>& data)
{
  std::vector<std::vector<uint8_t> > ret;

  unsigned long max_fragment_payload_size = max_packet_size_-(sizeof(FragmentHeader)+sizeof(SequencedPacketHeader));
  if(data.size()+sizeof(SequencedPacketHeader) > max_packet_size_)
  {
    // Reserve a unique packet_id atomically. Under concurrent fragment()
    // calls from multiple callback groups, two messages could otherwise
    // both read N, both assign N to their fragments, and both increment —
    // breaking defragmentation on the receiver. fetch_add(1) gives each
    // call its own ID up front.
    const uint32_t this_packet_id = next_fragmented_packet_id_.fetch_add(1);
    for(unsigned long i = 0; i < data.size(); i += max_fragment_payload_size)
    {
      unsigned long fragment_data_size = std::min(max_fragment_payload_size, data.size()-i);
      ret.push_back(std::vector<uint8_t>(sizeof(FragmentHeader)+fragment_data_size));
      Fragment * fragment_packet = reinterpret_cast<Fragment*>(ret.back().data());
      fragment_packet->type = PacketType::Fragment;
      fragment_packet->packet_id = this_packet_id;
      fragment_packet->fragment_number = ret.size();
      memcpy(fragment_packet->fragment_data, &data.at(i), fragment_data_size);
    }
    for(auto & fragment_vector: ret)
      reinterpret_cast<Fragment*>(fragment_vector.data())->fragment_count = ret.size();
    if(ret.size() > std::numeric_limits<uint16_t>::max())
    {
      RCLCPP_WARN_STREAM(get_logger(), "Dropping " << ret.size() << " fragments, max frag count:" <<  std::numeric_limits<uint16_t>::max());
      return {};
    }
  }
  if(ret.empty())
    ret.push_back(data);
  return ret;
}

void UDPBridge::bridgeInfoCallback()
{
  if(get_current_state().id() != lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE)
  {
    return;
  }

  sendBridgeInfo();
  sendConnectionRequests();
}

void UDPBridge::sendBridgeInfo()
{
  BridgeInfo bi;
  bi.stamp = get_clock()->now();
  bi.name = name_;
  // get_topic_names_and_types() is a node-graph query; safe to call without
  // any of our maps' mutexes.
  auto topic_names_and_types = get_topic_names_and_types();

  // Build the topics + remotes sections under scoped_lock so we get a
  // consistent snapshot. Connection statistics queries take their own
  // per-Connection mutex.
  {
    std::scoped_lock lock(subscribers_mutex_, remote_nodes_mutex_);
    for(auto topic: topic_names_and_types)
    {
      TopicInfo ti;
      ti.topic = topic.first;
      ti.datatype = topic.second.front();
      auto sub_it = subscribers_.find(topic.first);
      if(sub_it != subscribers_.end())
      {
        for(auto r: sub_it->second.remote_details)
        {
          TopicRemoteDetails trd;
          trd.remote = r.first;
          trd.destination_topic = r.second.destination_topic;
          for(auto c: r.second.connection_rates)
          {
            TopicRemoteConnectionDetails trcd;
            trcd.connection_id = c.first;
            trcd.period = c.second.period;
            trd.connections.push_back(trcd);
          }
          ti.remotes.push_back(trd);
        }
      }
      bi.topics.push_back(ti);
    }

    for(auto remote_node: remote_nodes_)
      if(remote_node.second)
      {
        udp_bridge::Remote remote;
        remote.name = remote_node.first;
        remote.topic_name = remote_node.second->topicName();
        // Per-remote give-up counter (issue #9). Lock-friendly with
        // the scoped_lock above: resendGiveupCount() takes only the
        // recursive RemoteNode::state_mutex_ and does not re-acquire
        // remote_nodes_mutex_.
        remote.resend_giveup_count = remote_node.second->resendGiveupCount();
        for(auto connection: remote_node.second->connections())
          if(connection)
          {
            RemoteConnection rc;
            rc.connection_id = connection->id();
            rc.host = connection->host();
            rc.port = connection->port();
            rc.ip_address = connection->ip_address();
            rc.return_host = connection->returnHost();
            rc.return_port = connection->returnPort();
            rc.source_ip_address = connection->sourceIPAddress();
            rc.source_port = connection->sourcePort();
            rc.maximum_bytes_per_second = connection->rateLimit();
            rc.effective_rate_limit = connection->effectiveRateLimit();
            auto receive_rates = connection->data_receive_rate(rclcpp::Time(bi.stamp).seconds());
            rc.received_bytes_per_second = receive_rates.first + receive_rates.second;
            rc.duplicate_bytes_per_second = receive_rates.second;
            rc.message = connection->data_sent_rate(bi.stamp, PacketSendCategory::message);
            rc.overhead = connection->data_sent_rate(bi.stamp, PacketSendCategory::overhead);
            rc.resend = connection->data_sent_rate(bi.stamp, PacketSendCategory::resend);
            remote.connections.push_back(rc);
          }
        bi.remotes.push_back(remote);
      }
  }

  bi.next_packet_number = next_packet_number_.load();
  // Reconstruct rclcpp::Time from the stored nanoseconds. The two atomic
  // loads aren't synchronized; minor diagnostic skew between
  // bi.next_packet_number and bi.last_packet_time is acceptable (both
  // are advisory display).
  bi.last_packet_time = rclcpp::Time(last_packet_number_assign_time_ns_.load());
  bi.local_port = port_;
  bi.maximum_packet_size = max_packet_size_;

  // publish + send run without our maps' mutexes.
  bridge_info_publisher_->publish(bi);

  send(bi, allRemotes(), true);
}

void UDPBridge::sendConnectionRequests()
{
  // Snapshot pending_connections_ entries we need under the lock, then
  // call send() (which acquires its own locks) without holding ours.
  std::vector<std::pair<uint64_t, ConnectionInternal>> snapshots;
  {
    std::lock_guard<std::mutex> lock(pending_connections_mutex_);
    snapshots.reserve(pending_connections_.size());
    for(auto& pending_connection: pending_connections_)
      snapshots.emplace_back(pending_connection.first, pending_connection.second.message);
  }
  for(auto& s: snapshots)
  {
    RemoteConnectionsList rcl;
    rcl[""].push_back(std::to_string(s.first));
    send(s.second, rcl, true);
  }
}

void UDPBridge::addRemote(
  std::shared_ptr<udp_bridge::AddRemote::Request> request,
  std::shared_ptr<udp_bridge::AddRemote::Response> response)
{
  (void)response;
  // Reject an over-long name (issue #51). AddRemote.srv has no response
  // field to report failure through, so the refusal is an ERROR log and no
  // remote: creating it would be worse than refusing, because the remote's
  // own packets arrive stamped with a shortened `source_node` that never
  // matches the key we would have filed it under, so it could never be
  // recognised, never be excluded by the relay loop rule, and would
  // accumulate a second phantom RemoteNode on first contact.
  if(!node_name_fits(request->name))
  {
    RCLCPP_ERROR_STREAM(get_logger(), "add_remote refused: "
      << node_name_too_long_error("the requested remote name", request->name));
    return;
  }

  // Refuse a remote that IS us, for the same reason resolveRemoteIdentities
  // refuses one at configure time (issue #51). Installing a RemoteNode
  // under our own name puts unwrap()'s self-packet refusal out of reach:
  // that check sits in the lookup-missed branch, so once remote_nodes_
  // holds an entry keyed by our name a successful lookup walks straight
  // past it and the bridge starts treating its own traffic as a peer's.
  // RemoteNode's constructor only asserts on it, and asserts are compiled
  // out of the release build this would be met in. Same refusal shape as
  // the length rejection above: ERROR, no remote, early return, because
  // AddRemote.srv has no field to report failure through.
  if(!request->name.empty() && request->name == name_)
  {
    RCLCPP_ERROR_STREAM(get_logger(), "add_remote refused: the requested"
      " remote name '" << request->name << "' is this bridge's own name."
      " A bridge cannot be its own remote: it would be installed in"
      " remote_nodes_ under our name and the receive path would then"
      " accept our own packets as a peer's.");
    return;
  }

  auto connection_id = request->connection_id;
  if(connection_id.empty())
    connection_id = "default";

  bool should_send_bridge_info = false;
  if(!request->name.empty())
  {
    // Resolve / create the RemoteNode under remote_nodes_mutex_; do all
    // per-remote work (DNS via newConnection, setHostAndPort,
    // setReturnHostAndPort, setRateLimit) outside the lock so service
    // handlers don't stall the socket-drain group.
    std::shared_ptr<RemoteNode> remote;
    {
      std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
      remote = remote_nodes_[request->name];
      if(!remote)
      {
        // `name` here is the remote's own wire name — the same namespace
        // remote_nodes_ is keyed by everywhere else (issue #51). An
        // operator who passes their local *label* for the remote instead
        // creates a phantom that matches no incoming packet: the same
        // misconfiguration the unknown-sender WARN in unwrap() exists to
        // catch, at the one entry point that cannot detect it, since any
        // string is a legitimate new remote here.
        RCLCPP_INFO_STREAM(get_logger(), "add_remote: creating remote '"
          << request->name << "' — this must be the name that bridge calls "
          "itself (its own `name` parameter), not a local label for it");
        remote = std::make_shared<RemoteNode>(request->name, name_, *this);
        remote_nodes_[request->name] = remote;
      }
    }

    uint16_t port = 4200;
    if (request->port != 0)
      port = request->port;

    auto connection = remote->connection(connection_id);
    if(!request->address.empty())
    {
      if(!connection)
        connection = remote->newConnection(connection_id, request->address, port);
      else
        connection->setHostAndPort(request->address, port);
    }
    if(connection)
    {
      connection->setReturnHostAndPort(request->return_address, request->return_port);
      connection->setRateLimit(request->maximum_bytes_per_second);
      warnIfAdmissionInert(get_logger(), *get_clock(), request->name, connection_id,
                           *connection);
      should_send_bridge_info = true;
    }
    if(should_send_bridge_info)
      sendBridgeInfo();
  }

  if(request->name.empty() || request->return_maximum_bytes_per_second > 0) // we don't know the remote name, so send a connection request
  {
    ConnectionInternal connection_internal_message;
    {
      std::lock_guard<std::mutex> lock(pending_connections_mutex_);
      // fetch_add(1) gives a unique sequence_number under concurrent
      // service-handler invocations.
      auto sequence_number = next_connection_internal_message_sequence_number_.fetch_add(1);
      auto& connection_internal = pending_connections_[sequence_number];
      connection_internal.message.connection_id = connection_id;
      connection_internal.message.sequence_number = sequence_number;
      connection_internal.message.operation = ConnectionInternal::OPERATION_CONNECT;
      connection_internal.message.return_host = request->return_address;
      connection_internal.message.return_port = request->return_port;
      connection_internal.message.return_maximum_bytes_per_second = request->return_maximum_bytes_per_second;
      if(!request->address.empty())
      {
        connection_internal.connection = std::make_shared<Connection>(connection_id, request->address, request->port);
        connection_internal.connection->setRateLimit(request->maximum_bytes_per_second);
        warnIfAdmissionInert(get_logger(), *get_clock(), request->name, connection_id,
                             *connection_internal.connection);
      }
      connection_internal_message = connection_internal.message;
    }
    RemoteConnectionsList rcl;
    if(request->name.empty())
      rcl[""].push_back(std::to_string(connection_internal_message.sequence_number));
    else
      rcl[request->name].push_back(connection_id);
    send(connection_internal_message, rcl, true);
  }
}

void UDPBridge::listRemotes(
  std::shared_ptr<udp_bridge::ListRemotes::Request> request,
  std::shared_ptr<udp_bridge::ListRemotes::Response> response)
{
  (void)request;
  auto now = get_clock()->now();
  std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
  for(auto remote: remote_nodes_)
    if(remote.second)
    {
      Remote r;
      r.name = remote.first;
      r.topic_name = remote.second->topicName();
      for(auto connection: remote.second->connections())
        if(connection)
        {
          RemoteConnection rc;
          rc.connection_id = connection->id();
          rc.host = connection->host();
          rc.port = connection->port();
          rc.ip_address = connection->ip_address();
          rc.return_host = connection->returnHost();
          rc.return_port = connection->returnPort();
          rc.source_ip_address = connection->sourceIPAddress();
          rc.source_port = connection->sourcePort();
          rc.maximum_bytes_per_second = connection->rateLimit();
          rc.effective_rate_limit = connection->effectiveRateLimit();
          auto receive_rates = connection->data_receive_rate(now.seconds());
          rc.received_bytes_per_second = receive_rates.first + receive_rates.second;
          rc.duplicate_bytes_per_second = receive_rates.second;
          rc.message = connection->data_sent_rate(now, PacketSendCategory::message);
          rc.overhead = connection->data_sent_rate(now, PacketSendCategory::overhead);
          rc.resend = connection->data_sent_rate(now, PacketSendCategory::resend);
          r.connections.push_back(rc);
        }
      response->remotes.push_back(r);
    }
}

UDPBridge::RemoteConnectionsList UDPBridge::allRemotes() const
{
  RemoteConnectionsList ret;
  std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
  for(auto remote: remote_nodes_)
    if(remote.second)
      ret[remote.first];
  return ret;
}

void UDPBridge::diagnosticTick()
{
  if(!diagnostic_updater_)
    return;
  if(get_current_state().id() != lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE)
    return;
  syncDiagnosticTasks();
  diagnostic_updater_->force_update();
}

void UDPBridge::syncDiagnosticTasks()
{
  if(!diagnostic_updater_)
    return;
  // Snapshot (remote_name, connection_id) pairs and remote names under
  // the lock; the diagnostic_updater_->add call captures by value so
  // registered tasks don't need to outlive the lock.
  std::vector<std::pair<std::string, std::string>> remote_connection_pairs;
  std::vector<std::string> remote_names;
  {
    std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
    for(auto& remote_entry: remote_nodes_)
    {
      if(!remote_entry.second)
        continue;
      remote_names.push_back(remote_entry.first);
      for(auto& connection: remote_entry.second->connections())
      {
        if(!connection)
          continue;
        remote_connection_pairs.emplace_back(remote_entry.first, connection->id());
      }
    }
  }
  for(const auto& pair: remote_connection_pairs)
  {
    const std::string& remote_name = pair.first;
    const std::string& connection_id = pair.second;
    std::string task_name = "udp_bridge " + name_ + ": " + remote_name + ": " + connection_id;
    if(diagnostic_task_names_.count(task_name))
      continue;
    diagnostic_updater_->add(task_name,
      [this, remote_name, connection_id](diagnostic_updater::DiagnosticStatusWrapper& stat)
      {
        diagnoseConnection(remote_name, connection_id, stat);
      });
    diagnostic_task_names_.insert(task_name);
  }
  // Per-remote resend-give-up diagnostic (issue #22). Task name format
  // mirrors the per-connection convention above so the aggregator
  // groups them next to the related connection statuses. The
  // diagnostic_task_names_ gate prevents double-add if a remote of the
  // same name is removed and re-added in the same activate cycle —
  // diagnostic_updater::Updater has no per-task remove() API.
  for(const auto& remote_name: remote_names)
  {
    std::string task_name = "udp_bridge " + name_ + ": " + remote_name + ": resend give-ups";
    if(diagnostic_task_names_.count(task_name))
      continue;
    diagnostic_updater_->add(task_name,
      [this, remote_name](diagnostic_updater::DiagnosticStatusWrapper& stat)
      {
        diagnoseRemoteGiveups(remote_name, stat);
      });
    diagnostic_task_names_.insert(task_name);
  }
  // Per-remote reorder-buffer diagnostic (issue #35 review follow-up).
  // Registered only when the reorder buffer can actually run — same
  // enable condition as the spin_once flush tick — so deployments with
  // it disabled don't grow an always-zero status row. Both parameters
  // are read in on_configure before the first syncDiagnosticTasks call
  // and are not live-tunable. Same task-name convention and double-add
  // gate as the loops above.
  if(drop_stale_packets_ && reorder_hold_window_ms_ > 0.0)
  {
    for(const auto& remote_name: remote_names)
    {
      std::string task_name = "udp_bridge " + name_ + ": " + remote_name + ": reorder buffer";
      if(diagnostic_task_names_.count(task_name))
        continue;
      diagnostic_updater_->add(task_name,
        [this, remote_name](diagnostic_updater::DiagnosticStatusWrapper& stat)
        {
          diagnoseReorderBuffer(remote_name, stat);
        });
      diagnostic_task_names_.insert(task_name);
    }
  }
}

void UDPBridge::diagnoseConnection(const std::string& remote_name,
                                   const std::string& connection_id,
                                   diagnostic_updater::DiagnosticStatusWrapper& stat)
{
  std::shared_ptr<Connection> connection;
  {
    std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
    auto remote_it = remote_nodes_.find(remote_name);
    if(remote_it == remote_nodes_.end() || !remote_it->second)
    {
      stat.summary(diagnostic_msgs::msg::DiagnosticStatus::STALE, "remote not registered");
      return;
    }
    connection = remote_it->second->connection(connection_id);
  }
  if(!connection)
  {
    stat.summary(diagnostic_msgs::msg::DiagnosticStatus::STALE, "connection not registered");
    return;
  }

  auto now_time = now();
  auto totals = connection->data_sent_rate(now_time);
  auto resends = connection->data_sent_rate(now_time, PacketSendCategory::resend);
  auto rx = connection->data_receive_rate(now_time.seconds());
  double last_rx = connection->last_receive_time();
  double rx_age = (last_rx > 0.0) ? (now_time.seconds() - last_rx) : -1.0;

  stat.add("host", connection->host());
  stat.add("port", static_cast<int>(connection->port()));
  stat.add("rate_limit_bytes_per_sec", static_cast<unsigned int>(connection->rateLimit()));
  stat.add("tx_ok_bytes_per_sec", totals.success_bytes_per_second);
  stat.add("tx_failed_bytes_per_sec", totals.failed_bytes_per_second);
  stat.add("tx_dropped_bytes_per_sec", totals.dropped_bytes_per_second);
  stat.add("tx_resend_bytes_per_sec", resends.success_bytes_per_second);
  stat.add("rx_bytes_per_sec", rx.first);
  stat.add("rx_duplicate_bytes_per_sec", rx.second);
  stat.add("last_rx_age_s", rx_age);

  constexpr double kStaleWarnSeconds = 5.0;
  constexpr double kStaleErrorSeconds = 10.0;

  std::string summary = "tx " + std::to_string(static_cast<uint64_t>(totals.success_bytes_per_second))
                      + " B/s, rx " + std::to_string(static_cast<uint64_t>(rx.first)) + " B/s";

  if(last_rx > 0.0 && rx_age > kStaleErrorSeconds)
  {
    stat.summary(diagnostic_msgs::msg::DiagnosticStatus::ERROR,
                 "no rx for " + std::to_string(static_cast<uint64_t>(rx_age)) + "s");
  }
  else if(totals.failed_bytes_per_second > 0 || totals.dropped_bytes_per_second > 0)
  {
    stat.summary(diagnostic_msgs::msg::DiagnosticStatus::WARN, "tx failures/drops");
  }
  else if(last_rx > 0.0 && rx_age > kStaleWarnSeconds)
  {
    stat.summary(diagnostic_msgs::msg::DiagnosticStatus::WARN,
                 "no rx for " + std::to_string(static_cast<uint64_t>(rx_age)) + "s");
  }
  else
  {
    stat.summary(diagnostic_msgs::msg::DiagnosticStatus::OK, summary);
  }
}

void UDPBridge::diagnoseRemoteGiveups(const std::string& remote_name,
                                      diagnostic_updater::DiagnosticStatusWrapper& stat)
{
  // Snapshot the remote pointer + the give-up counter + step the
  // per-remote rate state + sample the thresholds all under
  // remote_nodes_mutex_. The lock is brief — the counter read
  // (RemoteNode::resendGiveupCount() in remote_node.cpp) takes only
  // the recursive RemoteNode::state_mutex_ and does not re-acquire
  // remote_nodes_mutex_, so it is lock-friendly here; the state-step
  // is a tiny struct copy + compute. Sampling the thresholds under
  // the same lock the OnSetParameters callback takes pins them
  // against mid-callback runtime changes from `ros2 param set`. The
  // STALE return mirrors diagnoseConnection() above: the remote may
  // have been removed between task registration and the diagnostic
  // callback firing.
  rclcpp::Time now_time = now();
  GiveupDiagnostic diag;
  double elapsed_s = 0.0;
  double warn_thresh = 0.0;
  double error_thresh = 0.0;
  {
    std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
    auto remote_it = remote_nodes_.find(remote_name);
    if(remote_it == remote_nodes_.end() || !remote_it->second)
    {
      stat.summary(diagnostic_msgs::msg::DiagnosticStatus::STALE, "remote not registered");
      return;
    }
    uint32_t current_count = remote_it->second->resendGiveupCount();
    warn_thresh = resend_giveup_warn_rate_per_s_;
    error_thresh = resend_giveup_error_rate_per_s_;
    // operator[] default-constructs a fresh GiveupRateState (with
    // has_previous=false) if this is the first call for remote_name,
    // matching stepGiveupDiagnostic's first-call contract. Capture
    // the elapsed window from the state's prior timestamp before the
    // step writes the new sample back.
    GiveupRateState& state = giveup_rate_state_[remote_name];
    if(state.has_previous)
      elapsed_s = (now_time - state.last_publish_time).seconds();
    diag = stepGiveupDiagnostic(state, current_count, now_time, warn_thresh, error_thresh);
  }

  stat.add("remote", remote_name);
  stat.add("give_ups_total", diag.total);
  stat.add("give_up_rate_per_s", diag.rate_per_s);
  // Clamp the published window to >= 0 so a backward clock jump
  // (NTP correction, sim-time rewind) doesn't surface a negative
  // window_s. computeGiveupDiagnostic already treats elapsed_s <= 0
  // as zero-rate / OK, so this just keeps the KeyValue consistent.
  stat.add("window_s", std::max(0.0, elapsed_s));
  stat.add("warn_threshold_per_s", warn_thresh);
  stat.add("error_threshold_per_s", error_thresh);

  // Two-decimal formatting keeps the summary string operator-friendly
  // in aggregator UIs; std::to_string(double) defaults to 6 fractional
  // digits which is noisy ("5.000000/s exceeds warn threshold"). The
  // structured KeyValues above still carry the raw doubles.
  std::ostringstream summary;
  summary << std::fixed << std::setprecision(2);
  if(diag.level == diagnostic_msgs::msg::DiagnosticStatus::ERROR)
    summary << "give-up rate " << diag.rate_per_s << "/s exceeds error threshold";
  else if(diag.level == diagnostic_msgs::msg::DiagnosticStatus::WARN)
    summary << "give-up rate " << diag.rate_per_s << "/s exceeds warn threshold";
  else
    summary << "give-up rate " << diag.rate_per_s << "/s (total " << diag.total << ")";
  stat.summary(diag.level, summary.str());
}

void UDPBridge::diagnoseReorderBuffer(const std::string& remote_name,
                                      diagnostic_updater::DiagnosticStatusWrapper& stat)
{
  // Snapshot under remote_nodes_mutex_; the RemoteNode getters take only
  // the recursive RemoteNode::state_mutex_ and don't re-acquire
  // remote_nodes_mutex_, so they are lock-friendly here (matching
  // diagnoseRemoteGiveups). The STALE return mirrors the other per-remote
  // tasks: the remote may have been removed between task registration and
  // this callback firing.
  std::size_t held = 0;
  uint32_t buffered_total = 0;
  uint32_t expired_total = 0;
  {
    std::lock_guard<std::mutex> lock(remote_nodes_mutex_);
    auto remote_it = remote_nodes_.find(remote_name);
    if(remote_it == remote_nodes_.end() || !remote_it->second)
    {
      stat.summary(diagnostic_msgs::msg::DiagnosticStatus::STALE, "remote not registered");
      return;
    }
    held = remote_it->second->reorderBufferOccupancy();
    buffered_total = remote_it->second->reorderBufferedTotal();
    expired_total = remote_it->second->reorderExpiredTotal();
  }

  stat.add("remote", remote_name);
  stat.add("held_packets", held);
  stat.add("buffered_total", buffered_total);
  stat.add("expired_released_total", expired_total);
  // Always OK: occupancy is capacity-safe by construction (<=1
  // packet/topic, each held <= the hold window), so there is no threshold
  // to trip — this task is pure observability. expired_released_total is
  // the number to watch: those packets waited the full window and still
  // published without their gap being filled.
  std::ostringstream summary;
  summary << held << " held (buffered total " << buffered_total
          << ", expired " << expired_total << ")";
  stat.summary(diagnostic_msgs::msg::DiagnosticStatus::OK, summary.str());
}

void UDPBridge::diagnosePublishQueue(diagnostic_updater::DiagnosticStatusWrapper& stat)
{
  // dropped_count() is atomic; depth takes the queue's brief internal lock.
  // last_reported_publish_drops_ is touched here and, to re-baseline the
  // delta across a deactivate, in on_deactivate — which does NOT run in
  // periodic_group_ and can interleave with this tick. advanceDropBaseline
  // makes that pair safe and, more importantly, unable to underflow; see
  // drop_baseline.h.
  const uint64_t dropped = publish_queue_.dropped_count();
  const uint64_t depth = static_cast<uint64_t>(publish_queue_.size());
  const uint64_t recent = dropsSinceBaseline(
    dropped, advanceDropBaseline(last_reported_publish_drops_, dropped));

  stat.add("queued_items", depth);
  stat.add("dropped_total", dropped);
  stat.add("dropped_since_last_tick", recent);
  stat.add("max_bytes", static_cast<uint64_t>(publish_queue_max_bytes_));

  // WARN on *recent* drops rather than latching forever after one historical
  // drop. A drop means a local destination subscriber stalled the publish
  // path long enough to overflow the byte budget — republished data was lost
  // (unrecoverable; the resend layer operates upstream of decodeData and
  // cannot recover it — see doc/qos_design.md).
  if(recent > 0)
    stat.summary(diagnostic_msgs::msg::DiagnosticStatus::WARN,
      "publish queue dropping (" + std::to_string(recent)
      + " since last tick) — local subscriber stalled?");
  else
    stat.summary(diagnostic_msgs::msg::DiagnosticStatus::OK,
      "queued " + std::to_string(depth) + ", " + std::to_string(dropped)
      + " dropped total");
}

void UDPBridge::diagnoseRelayQueue(diagnostic_updater::DiagnosticStatusWrapper& stat)
{
  // Mirror of diagnosePublishQueue for the relay worker (issue #51).
  // last_reported_relay_drops_ is touched here and in on_deactivate (see
  // the note there); the pair is serialized through advanceDropBaseline,
  // which also makes the delta underflow-proof (drop_baseline.h).
  //
  // Two sources of loss are summed here. The queue drops on overflow and
  // push-after-stop (RelayQueue::dropped_count); the sink drops an item it
  // already dequeued when the sender is unnamed or when the topic's
  // routing table was torn down under it (relay_drops_).
  // Both are unrecoverable, so both must be visible —
  // reporting only the queue's would under-report real relay loss. Items
  // held back purely by the `period` rate limit are counted apart and are
  // not loss.
  //
  // Every counter is read exactly once into a local: the worker keeps
  // incrementing while this runs, and re-reading dropped_count()/lost() for
  // the per-source rows could otherwise publish a status where
  // dropped_by_queue + dropped_by_sink exceeds dropped_total.
  const uint64_t dropped_by_queue = relay_queue_.dropped_count();
  const uint64_t dropped_by_sink = relay_drops_.lost();
  const uint64_t dropped = dropped_by_queue + dropped_by_sink;
  const uint64_t depth = static_cast<uint64_t>(relay_queue_.size());
  const uint64_t recent = dropsSinceBaseline(
    dropped, advanceDropBaseline(last_reported_relay_drops_, dropped));

  stat.add("queued_items", depth);
  stat.add("dropped_total", dropped);
  stat.add("dropped_since_last_tick", recent);
  stat.add("dropped_by_queue", dropped_by_queue);
  stat.add("dropped_by_sink", dropped_by_sink);
  const auto breakdown = relay_drops_.lossBreakdown();
  if(!breakdown.empty())
    stat.add("sink_drop_reasons", breakdown);
  // Reported apart from the loss reasons above, and kept out of the WARN
  // string below, because the rate limiter working as configured is not
  // loss — mixing it in produced statuses like "relay dropping (1 since
  // last tick; topic_gone=1, rate_limited=40321)".
  stat.add("rate_limited", relay_drops_.rateLimited());
  stat.add("max_bytes", static_cast<uint64_t>(relay_queue_max_bytes_));

  // A relay drop is unrecoverable loss for the downstream remotes: the
  // message was never handed to a Connection, so the resend layer has
  // nothing to retransmit. Recent drops mean an outgoing link stalled long
  // enough for the worker to fall behind.
  if(recent > 0)
    stat.summary(diagnostic_msgs::msg::DiagnosticStatus::WARN,
      "relay dropping (" + std::to_string(recent)
      + " since last tick"
      + (breakdown.empty() ? std::string() : "; " + breakdown)
      + ") — outgoing link stalled?");
  else
    stat.summary(diagnostic_msgs::msg::DiagnosticStatus::OK,
      "queued " + std::to_string(depth) + ", " + std::to_string(dropped)
      + " dropped total");
}

// void UDPBridge::maximumPacketSizeCallback(const std_msgs::Int32::ConstPtr& msg)
// {
//   max_packet_size_ = msg->data;
// }


} // namespace udp_bridge
