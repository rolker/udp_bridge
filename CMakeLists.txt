cmake_minimum_required(VERSION 2.8.3)
project(udp_bridge)

## Compile as C++11, supported in ROS Kinetic and newer
add_compile_options(-std=c++11)

find_package(catkin REQUIRED COMPONENTS
  roscpp
  message_generation
  topic_tools
)

find_package(ZLIB REQUIRED)

add_message_files(
  FILES
  BridgeInfo.msg
  ChannelInfo.msg
  ChannelInfoArray.msg
  ChannelStatistics.msg
  ChannelStatisticsArray.msg
  MessageInternal.msg
  Remote.msg
  RemoteSubscribeInternal.msg
  TopicInfo.msg
  TopicRemoteDetails.msg
)

add_service_files(
  FILES
  Subscribe.srv
  AddRemote.srv
  ListRemotes.srv
)

generate_messages(
)

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

set(UDP_BRIDGE_SOURCES
    src/packet.cpp
    src/connection.cpp
    src/udpbridge.cpp
    src/defragmenter.cpp
)

add_executable(udp_bridge_node src/udp_bridge_node.cpp ${UDP_BRIDGE_SOURCES})
add_executable(udp_bridge_ui src/udpbridge_ui.cpp)

add_dependencies(${PROJECT_NAME}_node ${PROJECT_NAME}_generate_messages_cpp)
add_dependencies(${PROJECT_NAME}_ui ${PROJECT_NAME}_generate_messages_cpp)

target_link_libraries(udp_bridge_node ${catkin_LIBRARIES} ${ZLIB_LIBRARIES})
target_link_libraries(udp_bridge_ui ${catkin_LIBRARIES})

if(CATKIN_ENABLE_TESTING)
    find_package(rostest REQUIRED)
    catkin_add_gtest(utest test/utest.cpp ${UDP_BRIDGE_SOURCES})
    target_link_libraries(utest ${catkin_LIBRARIES} ${ZLIB_LIBRARIES})
endif()
