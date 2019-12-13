cmake_minimum_required(VERSION 2.8.3)
project(udp_bridge)

## Compile as C++11, supported in ROS Kinetic and newer
add_compile_options(-std=c++11)

find_package(catkin REQUIRED COMPONENTS
  geographic_msgs
  geometry_msgs
  marine_msgs
  roscpp
  std_msgs
  project11
  diagnostic_msgs
  geographic_visualization_msgs
)

find_package(ZLIB REQUIRED)

catkin_package(
#  INCLUDE_DIRS include
#  LIBRARIES udp_bridge
#  CATKIN_DEPENDS geographic_msgs geometry_msgs mission_plan roscpp std_msgs
#  DEPENDS system_lib
)

include_directories(
 include
  ${catkin_INCLUDE_DIRS}
  ${ZLIB_INCLUDE_DIRS}
)

add_executable(operator_node src/operator.cpp)
add_executable(vehicle_node src/vehicle.cpp)
add_executable(remote_server_node src/remote_server.cpp)
add_executable(remote_client_node src/remote_client.cpp)
add_executable(johnny5_server_node src/johnny5_server.cpp)
add_executable(johnny5_client_node src/johnny5_client.cpp)
add_executable(position_server_node src/position_server.cpp)
add_executable(position_client_node src/position_client.cpp)

add_dependencies(operator_node geographic_visualization_msgs_generate_messages_cpp)
add_dependencies(vehicle_node geographic_visualization_msgs_generate_messages_cpp)
add_dependencies(remote_server_node geographic_visualization_msgs_generate_messages_cpp)
add_dependencies(remote_client_node geographic_visualization_msgs_generate_messages_cpp)

target_link_libraries(operator_node ${catkin_LIBRARIES} ${ZLIB_LIBRARIES})
target_link_libraries(vehicle_node ${catkin_LIBRARIES} ${ZLIB_LIBRARIES})
target_link_libraries(remote_server_node ${catkin_LIBRARIES} ${ZLIB_LIBRARIES})
target_link_libraries(remote_client_node ${catkin_LIBRARIES} ${ZLIB_LIBRARIES})
target_link_libraries(johnny5_server_node ${catkin_LIBRARIES} ${ZLIB_LIBRARIES})
target_link_libraries(johnny5_client_node ${catkin_LIBRARIES} ${ZLIB_LIBRARIES})
target_link_libraries(position_server_node ${catkin_LIBRARIES} ${ZLIB_LIBRARIES})
target_link_libraries(position_client_node ${catkin_LIBRARIES} ${ZLIB_LIBRARIES})
