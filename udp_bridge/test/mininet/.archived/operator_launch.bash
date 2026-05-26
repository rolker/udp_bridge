#!/bin/bash

source ./operator_setup.bash
xterm -e roscore &
xterm -e rosrun rosmon rosmon --name=rosmon_operator_bridge udp_bridge test_mininet_multilink_operator.launch &
