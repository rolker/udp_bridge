cmake_minimum_required(VERSION 3.8)

project(udp_bridge_interfaces)

set(CMAKE_CXX_STANDARD 17)

find_package(ament_cmake REQUIRED)
find_package(rosidl_default_generators REQUIRED)

set(MSG_FILES
  msg/BridgeInfo.msg
  msg/ConnectionInternal.msg
  msg/DataRates.msg
  msg/MessageInternal.msg
  msg/Remote.msg
  msg/RemoteConnection.msg
  msg/RemoteSubscribeInternal.msg
  msg/ResendRequest.msg
  msg/TopicInfo.msg
  msg/TopicRemoteConnectionDetails.msg
  msg/TopicRemoteDetails.msg
  msg/TopicStatistics.msg
  msg/TopicStatisticsArray.msg
  srv/Subscribe.srv
  srv/AddRemote.srv
  srv/ListRemotes.srv
)

rosidl_generate_interfaces(
    ${PROJECT_NAME}
    ${MSG_FILES}
    DEPENDENCIES
    builtin_interfaces
)

ament_export_dependencies(rosidl_default_runtime)

ament_package()


