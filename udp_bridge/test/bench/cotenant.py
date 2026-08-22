#!/usr/bin/env python3
"""Co-tenant management flow for the bench harness (issue #52).

A small, steady UDP flow across one impaired path, standing in for the
traffic that must survive whatever udp_bridge is doing to the link: an
interactive SSH session, the operator reaching a boat host, a ping.

This exists because `maximum_bytes_per_second` was added after a
udp_bridge saturating a link locked an operator out of a remote machine.
An absolute ceiling only restrains the bridge while the link is healthy
-- on a degraded path the cap sits far above real capacity and the bridge
saturates anyway. The property that actually protects the operator is
headroom against *measured* throughput, and the only way to know whether
the bridge honours it is to put a co-tenant on the wire and watch.

Two modes, one per namespace:

    cotenant.py --mode recv --port P --output CSV
    cotenant.py --mode send --host H --port P [--rate-hz R] [--size B]
                --duration-s D

Datagrams carry `seq` and the sender's wall-clock send time, so the
receiver's CSV (`seq,sent_unix,recv_unix`) gives both delivery ratio and
one-way latency without any cross-process clock correction -- both ends
run on the same kernel.
"""

from __future__ import annotations

import argparse
import socket
import struct
import sys
import time

# seq (uint64) + send timestamp (double)
_HEADER = struct.Struct('!Qd')

# Defaults chosen to look like an interactive session rather than a
# transfer: small packets, steady, ~4 kB/s. On the trajectory's worst
# phase (62.5 kB/s) that is ~6% of the link -- comfortably inside the
# 20% the bridge is supposed to leave, so a failure means the bridge
# took the headroom, not that the co-tenant was greedy.
DEFAULT_RATE_HZ = 20.0
DEFAULT_SIZE_BYTES = 200


def run_recv(port: int, output: str) -> int:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(('0.0.0.0', port))
    sock.settimeout(1.0)
    count = 0
    with open(output, 'w') as f:
        f.write('seq,sent_unix,recv_unix\n')
        # Runs until SIGTERM; the orchestrator stops it after the walk.
        while True:
            try:
                data, _ = sock.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                break
            now = time.time()
            if len(data) < _HEADER.size:
                continue
            seq, sent = _HEADER.unpack_from(data)
            f.write(f'{seq},{sent!r},{now!r}\n')
            count += 1
            # Flush as we go: the orchestrator kills this process, so a
            # buffered tail would be lost exactly when the run ends.
            f.flush()
    print(f'BENCH_COTENANT_RECV_COUNT={count}', file=sys.stderr)
    return 0


def run_send(host: str, port: int, rate_hz: float, size: int,
             duration_s: float) -> int:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    payload_pad = b'\x00' * max(0, size - _HEADER.size)
    period = 1.0 / rate_hz if rate_hz > 0 else 0.05
    end = time.monotonic() + duration_s
    seq = 0
    next_tick = time.monotonic()
    while time.monotonic() < end:
        seq += 1
        try:
            sock.sendto(_HEADER.pack(seq, time.time()) + payload_pad,
                        (host, port))
        except OSError:
            # A full socket buffer or an unreachable host is itself a
            # data point; keep the sequence advancing so the receiver's
            # delivery ratio counts the attempt.
            pass
        next_tick += period
        sleep_for = next_tick - time.monotonic()
        if sleep_for > 0:
            time.sleep(sleep_for)
        else:
            next_tick = time.monotonic()
    print(f'BENCH_COTENANT_SENT_COUNT={seq}', file=sys.stderr)
    return 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description='Bench co-tenant flow')
    parser.add_argument('--mode', choices=('send', 'recv'), required=True)
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, required=True)
    parser.add_argument('--output')
    parser.add_argument('--rate-hz', type=float, default=DEFAULT_RATE_HZ)
    parser.add_argument('--size', type=int, default=DEFAULT_SIZE_BYTES)
    parser.add_argument('--duration-s', type=float, default=0.0)
    args = parser.parse_args(argv)

    if args.mode == 'recv':
        if not args.output:
            parser.error('--output is required in recv mode')
        return run_recv(args.port, args.output)
    return run_send(args.host, args.port, args.rate_hz, args.size,
                    args.duration_s)


if __name__ == '__main__':
    sys.exit(main())
