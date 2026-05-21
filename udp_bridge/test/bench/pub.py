#!/usr/bin/env python3
"""Bench publisher: emits a configurable topic mix for the bench harness.

Used by `run_scenario.py` inside the bench's user+net namespace to
generate the three-tier traffic (Critical/Telemetry/Bulk). The smoke
scenario only enables Critical; Phase 2 enables the full mix.
"""

import argparse
import sys

import rclpy
from nav_msgs.msg import Odometry
from rclpy.node import Node
from sensor_msgs.msg import Image
from std_msgs.msg import String


TIER_DEFAULTS = {
    'critical': ('std_msgs/String', '/boat/critical/heartbeat', 1.0, 64),
    'telemetry': ('nav_msgs/Odometry', '/boat/telemetry/odom', 10.0, 0),
    'bulk': ('sensor_msgs/Image', '/boat/bulk/image', 5.0, 524288),
}


class BenchPublisher(Node):
    def __init__(self, topic: str, msg_type: str, rate_hz: float, payload_bytes: int):
        super().__init__('bench_pub')
        self._payload_bytes = payload_bytes
        self._count = 0
        if msg_type == 'std_msgs/String':
            self._pub = self.create_publisher(String, topic, 10)
            self._make_msg = self._make_string
        elif msg_type == 'nav_msgs/Odometry':
            self._pub = self.create_publisher(Odometry, topic, 10)
            self._make_msg = self._make_odom
        elif msg_type == 'sensor_msgs/Image':
            self._pub = self.create_publisher(Image, topic, 10)
            self._make_msg = self._make_image
        else:
            raise ValueError(f'Unsupported msg type: {msg_type}')
        period = 1.0 / rate_hz if rate_hz > 0 else 1.0
        self._timer = self.create_timer(period, self._tick)

    def _tick(self):
        self._pub.publish(self._make_msg())
        self._count += 1

    def _make_string(self):
        return String(data=f'tick={self._count}')

    def _make_odom(self):
        m = Odometry()
        m.header.stamp = self.get_clock().now().to_msg()
        m.header.frame_id = 'bench'
        return m

    def _make_image(self):
        m = Image()
        m.header.stamp = self.get_clock().now().to_msg()
        m.header.frame_id = 'bench'
        m.height = 1
        m.width = self._payload_bytes
        m.encoding = 'mono8'
        m.step = self._payload_bytes
        m.data = bytes(self._payload_bytes)
        return m


def main(argv=None):
    parser = argparse.ArgumentParser(description='Bench publisher')
    parser.add_argument('--tier', choices=TIER_DEFAULTS.keys(), required=True)
    parser.add_argument('--topic', help='Override topic name')
    parser.add_argument('--rate-hz', type=float, help='Override rate')
    parser.add_argument('--payload-bytes', type=int, help='Override payload size')
    parser.add_argument('--duration-s', type=float, default=0.0,
                        help='Exit after N seconds (0 = run forever)')
    args = parser.parse_args(argv)

    msg_type, topic, rate, payload = TIER_DEFAULTS[args.tier]
    topic = args.topic or topic
    rate = args.rate_hz if args.rate_hz is not None else rate
    payload = args.payload_bytes if args.payload_bytes is not None else payload

    rclpy.init()
    node = BenchPublisher(topic, msg_type, rate, payload)
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
        print(f'BENCH_PUB_COUNT={node._count}', file=sys.stderr, flush=True)
        node.destroy_node()
        rclpy.shutdown()


if __name__ == '__main__':
    main()
