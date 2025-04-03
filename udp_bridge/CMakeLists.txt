cmake_minimum_required(VERSION 3.5.1)

project(udp_bridge)

set(CMAKE_CXX_STANDARD 17)
if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(ament_cmake REQUIRED)
find_package(lifecycle_msgs REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_lifecycle REQUIRED)
find_package(udp_bridge_interfaces REQUIRED)

find_package(ZLIB REQUIRED)

add_executable(udp_bridge_node
  src/connection.cpp
  src/defragmenter.cpp
  src/packet.cpp
  src/remote_node.cpp
  src/statistics.cpp
  src/udp_bridge_node.cpp
  src/udp_bridge.cpp
  src/utilities.cpp
  src/wrapped_packet.cpp
)

target_include_directories(udp_bridge_node
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include/${PROJECT_NAME}>"
)

target_link_libraries(
  udp_bridge_node
  rclcpp::rclcpp
  rclcpp_lifecycle::rclcpp_lifecycle
  ${std_msgs_TARGETS}
  ${udp_bridge_interfaces_TARGETS}
  ${ZLIB_LIBRARIES}
)


# Mark executables and/or libraries for installation
install(TARGETS  udp_bridge_node
  DESTINATION lib/${PROJECT_NAME}
)

install(DIRECTORY launch DESTINATION share/${PROJECT_NAME})

ament_package()

