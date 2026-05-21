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
  # Expose test-only helpers to the library's own compilation AND to
  # every test target. PRIVATE on the library means the macro doesn't
  # propagate to downstream consumers via target_link_libraries, so the
  # UDP_BRIDGE_BUILD_TESTING-gated declarations in remote_node.h /
  # connection.h stay hidden from the installed public ABI.
  #
  # Every test target that links against the library must define the
  # same macro: some `UDP_BRIDGE_BUILD_TESTING`-gated entries (notably
  # Connection::resend_call_count_for_test_, the per-connection counter
  # introduced for issue #23 routing tests) are data members, not just
  # methods — adding a member changes `sizeof(Connection)`. If the
  # library is compiled with the macro and a test compilation isn't,
  # the test sees a smaller layout, constructs Connection on the stack
  # at the wrong size, and the library's writes scribble past it
  # (observed as a segfault in test_connection_rate_limit before this
  # discipline was extended package-wide). Each test target needs its
  # own PRIVATE define since the library's PRIVATE flag doesn't
  # propagate to test compilations.
  #
  # The macro is package-namespaced (`UDP_BRIDGE_BUILD_TESTING`, not
  # the generic `BUILD_TESTING`) to avoid the same ODR hazard for
  # downstream packages that define `BUILD_TESTING` for their own
  # tests and include our installed headers — they would otherwise see
  # a different `RemoteNode` / `Connection` class definition than the
  # library was built with. The CMake variable `BUILD_TESTING` (the
  # `if(...)` check above) is the standard CMake/ament name and stays
  # as-is — only the C++ preprocessor symbol is package-scoped.
  target_compile_definitions(${PROJECT_NAME} PRIVATE UDP_BRIDGE_BUILD_TESTING)

  # The udp_bridge_node executable links against the library AND
  # includes udp_bridge.h (which in turn includes connection.h
  # directly), so its translation unit sees Connection / RemoteNode
  # class definitions. Without the macro on this target too, the
  # executable would compile against the smaller (production) layout
  # while linking against a library compiled with the larger
  # (test-augmented) layout — the same ODR hazard the test-target
  # discipline above is designed to prevent, just for an in-package
  # executable instead of a test target. In practice no Connection /
  # RemoteNode is allocated by value in udp_bridge_node and no inline
  # methods touch the conditional members, so the mismatch hasn't
  # bitten us; but it is UB per the standard and tooling
  # (sanitizers, ABI checkers) will flag it. Defining the macro on
  # the executable too aligns all in-package translation units to a
  # single layout. (Originally caught by Copilot in PR #25 review 3.)
  target_compile_definitions(udp_bridge_node PRIVATE UDP_BRIDGE_BUILD_TESTING)

  find_package(ament_cmake_gtest REQUIRED)
  # std_msgs is required for the QoS-matching integration test, which
  # validates the load-bearing BEST_AVAILABLE matching guarantee
  # against the active rmw. Marking it REQUIRED here (instead of QUIET
  # at the top level + a silent-skip below) prevents minimal-dependency
  # CI builds from quietly losing that coverage.
  find_package(std_msgs REQUIRED)

  # Helper: register a gtest, define UDP_BRIDGE_BUILD_TESTING on it,
  # and link the library. Keeps the per-test boilerplate from
  # forgetting the macro and reintroducing the ODR hazard documented
  # above. Extra link libraries can be passed after the source file.
  function(add_udp_bridge_gtest target source)
    ament_add_gtest(${target} ${source})
    target_compile_definitions(${target} PRIVATE UDP_BRIDGE_BUILD_TESTING)
    target_link_libraries(${target} ${PROJECT_NAME} ${ARGN})
  endfunction()

  add_udp_bridge_gtest(utest test/utest.cpp)
  add_udp_bridge_gtest(test_utilities test/test_utilities.cpp)
  add_udp_bridge_gtest(test_defragmenter test/test_defragmenter.cpp)
  add_udp_bridge_gtest(test_qos_resolution test/test_qos_resolution.cpp)
  add_udp_bridge_gtest(test_qos_matching_integration
    test/test_qos_matching_integration.cpp
    ${std_msgs_TARGETS})
  add_udp_bridge_gtest(test_connection_rate_limit test/test_connection_rate_limit.cpp)
  add_udp_bridge_gtest(test_remote_node_resend test/test_remote_node_resend.cpp)
  add_udp_bridge_gtest(test_connection_cleanup test/test_connection_cleanup.cpp)
  # test_giveup_diagnostic merged in from jazzy (PR #24). Registered
  # through the same helper so it gets UDP_BRIDGE_BUILD_TESTING too —
  # required for in-package ABI alignment per the helper's contract.
  add_udp_bridge_gtest(test_giveup_diagnostic test/test_giveup_diagnostic.cpp)
endif()

ament_package()

