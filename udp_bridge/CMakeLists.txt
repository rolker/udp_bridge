cmake_minimum_required(VERSION 3.5.1)

project(udp_bridge)

set(CMAKE_CXX_STANDARD 17)
if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(ament_cmake REQUIRED)
find_package(diagnostic_updater REQUIRED)
find_package(lifecycle_msgs REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_lifecycle REQUIRED)
find_package(udp_bridge_interfaces REQUIRED)

find_package(ZLIB REQUIRED)

option(ENABLE_COVERAGE "Enable coverage reporting" OFF)
if(ENABLE_COVERAGE)
  add_compile_options(-fprofile-arcs -ftest-coverage)
  add_link_options(-fprofile-arcs)
endif()

add_library(${PROJECT_NAME}
  src/connection.cpp
  src/defragmenter.cpp
  src/packet.cpp
  src/remote_node.cpp
  src/statistics.cpp
  src/udp_bridge.cpp
  src/utilities.cpp
  src/wrapped_packet.cpp
)

target_include_directories(${PROJECT_NAME}
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include/${PROJECT_NAME}>"
)

target_link_libraries(${PROJECT_NAME}
  PUBLIC
    rclcpp::rclcpp
    rclcpp_lifecycle::rclcpp_lifecycle
    ${diagnostic_updater_TARGETS}
    ${udp_bridge_interfaces_TARGETS}
    ${ZLIB_LIBRARIES}
)

add_executable(udp_bridge_node src/udp_bridge_node.cpp)
target_link_libraries(udp_bridge_node ${PROJECT_NAME})


# Mark executables and/or libraries for installation
install(TARGETS  udp_bridge_node ${PROJECT_NAME}
  EXPORT export_${PROJECT_NAME}
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION lib/${PROJECT_NAME}
)

install(DIRECTORY include/ DESTINATION include)

install(DIRECTORY launch DESTINATION share/${PROJECT_NAME})

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  # std_msgs is required for the QoS-matching integration test, which
  # validates the load-bearing BEST_AVAILABLE matching guarantee
  # against the active rmw. Marking it REQUIRED here (instead of QUIET
  # at the top level + a silent-skip below) prevents minimal-dependency
  # CI builds from quietly losing that coverage.
  find_package(std_msgs REQUIRED)

  ament_add_gtest(utest test/utest.cpp)
  target_link_libraries(utest ${PROJECT_NAME})

  ament_add_gtest(test_utilities test/test_utilities.cpp)
  target_link_libraries(test_utilities ${PROJECT_NAME})

  ament_add_gtest(test_defragmenter test/test_defragmenter.cpp)
  target_link_libraries(test_defragmenter ${PROJECT_NAME})

  ament_add_gtest(test_qos_resolution test/test_qos_resolution.cpp)
  target_link_libraries(test_qos_resolution ${PROJECT_NAME})

  ament_add_gtest(test_qos_matching_integration test/test_qos_matching_integration.cpp)
  target_link_libraries(test_qos_matching_integration
    ${PROJECT_NAME}
    ${std_msgs_TARGETS}
  )

  ament_add_gtest(test_connection_rate_limit test/test_connection_rate_limit.cpp)
  target_link_libraries(test_connection_rate_limit ${PROJECT_NAME})

  ament_add_gtest(test_remote_node_resend test/test_remote_node_resend.cpp)
  target_link_libraries(test_remote_node_resend ${PROJECT_NAME})

  ament_add_gtest(test_connection_cleanup test/test_connection_cleanup.cpp)
  target_link_libraries(test_connection_cleanup ${PROJECT_NAME})
endif()

ament_package()

