#include "udp_bridge/udp_bridge.h"

int main(int argc, char **argv)
{
  rclcpp::init(argc, argv);
    
  rclcpp::executors::SingleThreadedExecutor executor;

  std::shared_ptr<udp_bridge::UDPBridge> udp_bridge_node = std::make_shared<udp_bridge::UDPBridge>("udp_bridge");
  executor.add_node(udp_bridge_node->get_node_base_interface());
  executor.spin();
  rclcpp::shutdown();
  return 0;
}

