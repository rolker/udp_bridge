#include "udp_bridge/udp_bridge.h"

// Callback group invariants for the MultiThreadedExecutor:
//
//   socket_drain_group_ (MutuallyExclusive)
//     Owns: spin_timer_ only (drains the UDP socket every 10 ms via
//     spin_once -> recvfrom).
//     Invariant: this group must never be starved. Anything that runs
//     here is hot-path; if it blocks, the kernel SO_RCVBUF (500 KB) fills
//     and packets are silently dropped — the wedge symptom in #10.
//
//   republish_group_ (MutuallyExclusive)
//     Owns: forwarding-subscription callbacks (UDPBridge::callback) and
//     the per-topic generic-publisher work triggered from decodeData.
//     Republished publishes can stall under back-pressure (e.g., a dead
//     remote subscriber on a RELIABLE-matched publisher); isolating them
//     here keeps the socket-drain group running.
//
//   periodic_group_ (MutuallyExclusive)
//     Owns: stats_report_timer_, bridge_info_timer_, diagnostic_timer_,
//     subscription_update_timer_, and the four service handlers
//     (subscribe / advertise / add_remote / list_remotes). Admin-style
//     work that must not race the hot path but can be safely serialized
//     against itself.
//
// Shared state crossing groups is guarded by mutexes (publishers_,
// subscribers_, remote_nodes_, pending_connections_, Connection::sent_packets_,
// statistics). See PR #12 / issue #11 for the full audit.

int main(int argc, char **argv)
{
  rclcpp::init(argc, argv);

  rclcpp::executors::MultiThreadedExecutor executor;

  std::shared_ptr<udp_bridge::UDPBridge> udp_bridge_node = std::make_shared<udp_bridge::UDPBridge>("udp_bridge");
  executor.add_node(udp_bridge_node->get_node_base_interface());
  executor.spin();
  rclcpp::shutdown();
  return 0;
}

