#!/usr/bin/env python3
"""Bench subscriber: counts received messages on a topic and prints the
total to stderr on exit.

Used by `run_scenario.py` to verify bench traffic reached the operator
side. The smoke test asserts the count is non-zero.

Optionally writes a `--count-trace` CSV (`unix_time,count`) sampled
periodically. The subscriber_death scenario uses this on the surviving
tiers to measure delivery rate before vs. after a peer subscriber is
killed (the head-of-line question in issue #29) — sub_count alone only
gives a final total, not the rate change across the kill.
"""

import argparse
import csv
import sys
import time

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
    parser.add_argument('--count-trace', default=None,
                        help='Write a CSV of (unix_time,count) sampled at '
                             '--trace-interval. Used by subscriber_death to '
                             'measure delivery rate across a peer kill.')
    parser.add_argument('--trace-interval', type=float, default=0.5,
                        help='Count-trace sampling interval in seconds.')
    args = parser.parse_args(argv)

    rclpy.init()
    node = BenchSubscriber(args.topic, args.msg_type)

    trace_fp = None
    trace_writer = None
    if args.count_trace:
        trace_fp = open(args.count_trace, 'w', newline='')
        trace_writer = csv.writer(trace_fp)
        trace_writer.writerow(['unix_time', 'count'])

    def sample_trace():
        if trace_writer is not None:
            trace_writer.writerow([f'{time.time():.3f}', node._count])
            trace_fp.flush()

    try:
        if args.duration_s > 0:
            end = node.get_clock().now().nanoseconds + int(args.duration_s * 1e9)
            next_sample = time.monotonic()
            while rclpy.ok() and node.get_clock().now().nanoseconds < end:
                rclpy.spin_once(node, timeout_sec=0.1)
                now = time.monotonic()
                if trace_writer is not None and now >= next_sample:
                    sample_trace()
                    next_sample = now + args.trace_interval
        else:
            rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    finally:
        sample_trace()  # final point so the trace covers the full run
        if trace_fp is not None:
            trace_fp.close()
        print(f'BENCH_SUB_COUNT={node._count}', file=sys.stderr, flush=True)
        node.destroy_node()
        rclpy.shutdown()


if __name__ == '__main__':
    main()
