#include "udp_bridge/udp_bridge.h"

// Callback group invariants for the MultiThreadedExecutor:
//
//   socket_drain_group_ (MutuallyExclusive)
//     Owns: spin_timer_ and all work executed inline from spin_once()
//     (recvfrom, decode, decodeData, the per-topic generic-publisher
//     publish, and the resend / cleanup tail loops). Decoded
//     destination-side publishes currently happen on this hot path — if
//     a destination publisher stalls under back-pressure, the socket
//     drain stalls with it. A future change can move the publish work
//     to republish_group_ via an internal queue; until that lands, the
//     mitigation is the BEST_AVAILABLE default + per-topic reliability
//     overrides documented in udp_bridge/doc/qos_design.md.
//     Invariant: this group must never be starved. Anything that runs
//     here is hot-path; if it blocks, the kernel SO_RCVBUF (500 KB) fills
//     and packets are silently dropped — the wedge symptom in #10.
//
//   republish_group_ (MutuallyExclusive)
//     Owns: forwarding-subscription callbacks (UDPBridge::callback) —
//     the source-side path that takes a locally-published message and
//     pushes it out over UDP. Isolating these keeps a stalled outbound
//     send (rate-limit, blocked send buffer) from blocking the
//     socket-drain group.
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

  // Pin to one thread per callback group (3 groups). Default would be
  // hardware_concurrency(), which adds context-switch overhead without
  // benefit since the groups are MutuallyExclusive and there is at most
  // one callback per group runnable at a time.
  rclcpp::executors::MultiThreadedExecutor executor(rclcpp::ExecutorOptions(), 3);

  std::shared_ptr<udp_bridge::UDPBridge> udp_bridge_node = std::make_shared<udp_bridge::UDPBridge>("udp_bridge");
  executor.add_node(udp_bridge_node->get_node_base_interface());
  executor.spin();
  rclcpp::shutdown();
  return 0;
}

