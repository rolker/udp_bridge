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
//     to a separate group via an internal queue; see qos_design.md
//     for the destination-publisher reliability policy that bounds
//     this exposure under the BEST_AVAILABLE design intent. The
//     current operational default is RELIABLE (rmw_zenoh_cpp 0.2.9
//     graph-visibility workaround) which does NOT bound this exposure
//     — a RELIABLE destination publisher stalled on a dead subscriber
//     buffers and can wedge the drain path. The risk is accepted
//     until BEST_AVAILABLE returns.
//     Invariant: this group must never be starved. Anything that runs
//     here is hot-path; if it blocks, the kernel SO_RCVBUF (500 KB) fills
//     and packets are silently dropped — the wedge symptom in #10.
//
//   republish_group_ (Reentrant)
//     Owns: forwarding-subscription callbacks (UDPBridge::callback) —
//     the source-side path that takes a locally-published message and
//     pushes it out over UDP. Reentrant so multiple forwarding callbacks
//     (different topics, or the same topic at high rate) can call
//     Connection::send concurrently; required for the high-rate camera
//     throughput restored in PR #16. Shared state on this path is
//     guarded by the per-Connection locks (sent_packets_mutex_,
//     sent_packet_statistics_mutex_, config_mutex_,
//     receive_history_mutex_) plus subscribers_mutex_ in the bridge.
//     PR #12 introduced and audited those locks; PR #16 then closed a
//     check-then-record TOCTOU in the rate-limit critical section via
//     a reserve-then-record pattern: a brief lock holds the can_send
//     check + reservation bump; sendto runs outside the lock; a brief
//     second lock atomically releases the reservation and adds the
//     real record. This keeps sent_packet_statistics_mutex_ hold time
//     to microseconds even under buffer back-pressure, so the periodic
//     path's sendBridgeInfo (which calls data_sent_rate while holding
//     remote_nodes_mutex_) cannot wedge the socket-drain path's
//     remote_nodes_mutex_ lookups.
//     Isolating these from socket_drain_group_ keeps a stalled outbound
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

  // 8 threads: socket_drain_group_ + periodic_group_ are MutuallyExclusive
  // (1 thread each); republish_group_ is Reentrant and benefits from
  // concurrency across the many forwarding subscriptions, hence the extra
  // headroom. 8 covers ~20 forwarding subscriptions without saturating a
  // typical 4-8 core boat host.
  rclcpp::executors::MultiThreadedExecutor executor(rclcpp::ExecutorOptions(), 8);

  std::shared_ptr<udp_bridge::UDPBridge> udp_bridge_node = std::make_shared<udp_bridge::UDPBridge>("udp_bridge");
  executor.add_node(udp_bridge_node->get_node_base_interface());
  executor.spin();
  rclcpp::shutdown();
  return 0;
}

