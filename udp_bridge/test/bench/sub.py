#!/usr/bin/env python3
"""Bench subscriber: counts received messages on a topic and prints the
total to stderr on exit.

Used by `run_scenario.py` to verify bench traffic reached the operator
side. The smoke test asserts the count is non-zero.
"""

import argparse
import sys

import rclpy
from nav_msgs.msg import Odometry
from rclpy.node import Node
from sensor_msgs.msg import Image
from std_msgs.msg import String


TYPES = {
    'std_msgs/String': String,
    'nav_msgs/Odometry': Odometry,
    'sensor_msgs/Image': Image,
}


class BenchSubscriber(Node):
    def __init__(self, topic: str, msg_type: str):
        super().__init__('bench_sub')
        cls = TYPES[msg_type]
        self._count = 0
        self._sub = self.create_subscription(cls, topic, self._cb, 10)

    def _cb(self, _msg):
        self._count += 1


def main(argv=None):
    parser = argparse.ArgumentParser(description='Bench subscriber')
    parser.add_argument('--topic', required=True)
    parser.add_argument('--msg-type', default='std_msgs/String',
                        choices=TYPES.keys())
    parser.add_argument('--duration-s', type=float, default=0.0,
                        help='Exit after N seconds (0 = run forever)')
    args = parser.parse_args(argv)

    rclpy.init()
    node = BenchSubscriber(args.topic, args.msg_type)
    try:
        if args.duration_s > 0:
            end = node.get_clock().now().nanoseconds + int(args.duration_s * 1e9)
            while rclpy.ok() and node.get_clock().now().nanoseconds < end:
                rclpy.spin_once(node, timeout_sec=0.1)
        else:
            rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    finally:
        print(f'BENCH_SUB_COUNT={node._count}', file=sys.stderr, flush=True)
        node.destroy_node()
        rclpy.shutdown()


if __name__ == '__main__':
    main()
