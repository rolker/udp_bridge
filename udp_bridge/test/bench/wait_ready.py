#!/usr/bin/env python3
"""Bench readiness gate: block until the boat bridge is actually carrying
the full three-tier mix, then exit 0.

`run_scenario.py` starts the phase walk immediately after launching the
publishers, which races Bulk's discovery and first-fragment path. Bulk is
10 Hz x 480 KB, so it needs several seconds before it contributes to the
wire; Critical and Telemetry are flowing almost at once. When the walk wins
that race, the first `in_range_clean` window -- which the recovery invariant
uses as its PRE-EVENT BASELINE -- records ~2 kB/s of Critical+Telemetry
instead of the real ~2 MB/s, and the invariant then compares a post-recovery
rate against a baseline that never happened.

That made `test_invariant_recovery_completeness` a coin flip: measured across
ten runs, the post-recovery rate was ~2,050 B/s every single time, while the
baseline came out anywhere between 1,601 B/s and 2,076,200 B/s depending
purely on whether Bulk had started. Two pre-event windows from consecutive
runs of identical code:

    started:     [0, 3553451, 2446090, 2077341, 1815681]   ratio 0.001
    not started: [0,    1965,    2004,    2016,    2020]   ratio 1.288

A fixed `sleep` would paper over this at the cost of being wrong on a slow
machine and wasteful on a fast one, so this gate waits for the condition
itself and fails loudly if it never arrives -- a harness that cannot reach
its own starting state must say so rather than silently measure nonsense.

Exits 0 once the gate is satisfied, 1 on timeout.
"""

import argparse
import sys

import rclpy
from rclpy.node import Node
from udp_bridge_interfaces.msg import BridgeInfo


# Well above the ~2 kB/s that Critical + Telemetry produce on their own, and
# well below the ~2 MB/s of the full mix -- so it can only be satisfied by
# Bulk genuinely contributing, without demanding a particular steady-state
# rate the link may not have reached yet.
DEFAULT_MIN_BPS = 100_000.0

# Consecutive qualifying samples required. One sample can be a transient
# spike as the first Bulk image bursts onto the wire; two consecutive means
# the tier is actually streaming.
DEFAULT_CONSECUTIVE = 2


class ReadinessGate(Node):
    def __init__(self, topic: str, connection_id: str, remote_name: str,
                 min_bps: float, consecutive: int):
        super().__init__('bench_wait_ready')
        self._connection_id = connection_id
        self._remote_name = remote_name
        self._min_bps = min_bps
        self._needed = consecutive
        self._streak = 0
        self.ready = False
        self.best_seen = 0.0
        self._sub = self.create_subscription(BridgeInfo, topic, self._cb, 10)

    def _cb(self, msg):
        rate = None
        for remote in msg.remotes:
            if remote.name != self._remote_name:
                continue
            for conn in remote.connections:
                if conn.connection_id == self._connection_id:
                    rate = conn.message.success_bytes_per_second
        if rate is None:
            return
        self.best_seen = max(self.best_seen, rate)
        if rate >= self._min_bps:
            self._streak += 1
            if self._streak >= self._needed:
                self.ready = True
        else:
            self._streak = 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--topic',
                    default='/operator_bridge/remotes/boat/bridge_info')
    ap.add_argument('--connection-id', default='wifi')
    ap.add_argument('--remote-name', default='operator')
    ap.add_argument('--min-bps', type=float, default=DEFAULT_MIN_BPS)
    ap.add_argument('--consecutive', type=int, default=DEFAULT_CONSECUTIVE)
    ap.add_argument('--timeout-s', type=float, default=30.0)
    args = ap.parse_args()

    rclpy.init()
    node = ReadinessGate(args.topic, args.connection_id, args.remote_name,
                         args.min_bps, args.consecutive)
    try:
        deadline = node.get_clock().now().nanoseconds + int(
            args.timeout_s * 1e9)
        while rclpy.ok() and not node.ready:
            if node.get_clock().now().nanoseconds >= deadline:
                print(
                    f'BENCH_READY_TIMEOUT after {args.timeout_s:.0f}s: '
                    f'{args.connection_id} message.success_bytes_per_second '
                    f'peaked at {node.best_seen:.0f} B/s, never held '
                    f'>= {args.min_bps:.0f} B/s for {args.consecutive} '
                    'consecutive samples. The Bulk tier never reached the '
                    'wire, so any baseline measured now would be wrong.',
                    file=sys.stderr)
                return 1
            rclpy.spin_once(node, timeout_sec=0.2)
        print(f'BENCH_READY=1 peak={node.best_seen:.0f}')
        return 0
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == '__main__':
    sys.exit(main())
