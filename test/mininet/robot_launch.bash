#!/bin/bash

source ./robot_setup.bash
xterm -e roscore &
xterm -e rosrun rosmon rosmon --name=rosmon_robot_bridge udp_bridge test_mininet_multilink_robot.launch &
