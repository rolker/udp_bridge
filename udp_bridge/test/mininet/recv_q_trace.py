#!/usr/bin/env python3
"""Poll /proc/net/udp for a UDP socket on the given port and write a CSV
trace of Recv-Q / Send-Q over time.

Useful for documenting the wedge symptom from
https://github.com/rolker/udp_bridge/issues/10 — the bridge appears alive
but the kernel receive queue backs up.

Independently useful for field debugging (no mininet required); see
README.md in this directory.

Usage::

    ./recv_q_trace.py --port 4200 --interval 0.5 --output /tmp/recv_q.csv

CSV columns: ``t_seconds_since_start``, ``recv_q_bytes``,
``send_q_bytes``, ``local_addr``. One line per poll interval.

Press ``Ctrl-C`` to finish writing the trace.
"""

import argparse
import csv
import signal
import sys
import time
from pathlib import Path


def parse_proc_net_udp_line(line: str) -> dict:
    """Parse one /proc/net/udp data line.

    The format is fixed-column whitespace-separated; the columns we care
    about are ``local_address`` (col 1, ``IP_HEX:PORT_HEX``), ``tx_queue``
    (col 4, ``HEX:HEX``), ``rx_queue`` (sub-field of col 4 — actually col 4
    is ``tx_queue:rx_queue``).
    """
    fields = line.split()
    local_address = fields[1]  # e.g. ``00000000:1068``
    tx_rx_queue = fields[4]    # e.g. ``00000000:00000000``
    tx_q_hex, rx_q_hex = tx_rx_queue.split(":")
    return {
        "local_address": local_address,
        "send_q_bytes": int(tx_q_hex, 16),
        "recv_q_bytes": int(rx_q_hex, 16),
    }


def hex_port_to_int(hex_str: str) -> int:
    """Convert the ``PORT_HEX`` half of a /proc/net/udp local_address.

    /proc/net/udp encodes the port in big-endian hex as a 16-bit value.
    """
    return int(hex_str, 16)


def hex_addr_to_dotted(hex_str: str) -> str:
    """Convert the IPv4 hex (little-endian on Linux) to dotted-quad."""
    raw = bytes.fromhex(hex_str)
    return ".".join(str(b) for b in reversed(raw))


def find_socket_for_port(port: int) -> dict | None:
    """Return queue stats for the UDP socket bound to ``port``, or None."""
    proc_path = Path("/proc/net/udp")
    if not proc_path.exists():
        return None
    with proc_path.open() as f:
        next(f)  # skip header
        for line in f:
            try:
                parsed = parse_proc_net_udp_line(line)
            except (IndexError, ValueError):
                continue
            ip_hex, port_hex = parsed["local_address"].split(":")
            if hex_port_to_int(port_hex) == port:
                return {
                    "recv_q_bytes": parsed["recv_q_bytes"],
                    "send_q_bytes": parsed["send_q_bytes"],
                    "local_addr": f"{hex_addr_to_dotted(ip_hex)}:{port}",
                }
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, required=True,
                        help="UDP port the bridge is bound to (e.g. 4200)")
    parser.add_argument("--interval", type=float, default=0.5,
                        help="Polling interval in seconds (default 0.5)")
    parser.add_argument("--output", type=Path, default=Path("/tmp/recv_q.csv"),
                        help="Path to write CSV (default /tmp/recv_q.csv)")
    parser.add_argument("--max-duration", type=float, default=0.0,
                        help="Stop after this many seconds (0 = run until "
                             "Ctrl-C, default 0)")
    args = parser.parse_args()

    print(f"Polling /proc/net/udp for port {args.port} every "
          f"{args.interval}s -> {args.output}", file=sys.stderr)
    print("Press Ctrl-C to finish.", file=sys.stderr)

    stop = {"flag": False}

    def handle_sigint(signum, frame):
        stop["flag"] = True

    signal.signal(signal.SIGINT, handle_sigint)
    signal.signal(signal.SIGTERM, handle_sigint)

    t0 = time.monotonic()
    samples_written = 0
    not_found_warned = False
    with args.output.open("w", newline="") as csvfile:
        writer = csv.DictWriter(
            csvfile,
            fieldnames=["t_seconds_since_start", "recv_q_bytes",
                        "send_q_bytes", "local_addr"])
        writer.writeheader()
        while not stop["flag"]:
            now = time.monotonic() - t0
            if args.max_duration > 0 and now >= args.max_duration:
                break
            stats = find_socket_for_port(args.port)
            if stats is None:
                if not not_found_warned:
                    print(f"  (warning) no UDP socket bound to port "
                          f"{args.port} yet; will retry", file=sys.stderr)
                    not_found_warned = True
                writer.writerow({
                    "t_seconds_since_start": f"{now:.3f}",
                    "recv_q_bytes": "",
                    "send_q_bytes": "",
                    "local_addr": "",
                })
            else:
                if not_found_warned:
                    print(f"  socket found on {stats['local_addr']}",
                          file=sys.stderr)
                    not_found_warned = False
                writer.writerow({
                    "t_seconds_since_start": f"{now:.3f}",
                    "recv_q_bytes": stats["recv_q_bytes"],
                    "send_q_bytes": stats["send_q_bytes"],
                    "local_addr": stats["local_addr"],
                })
            csvfile.flush()
            samples_written += 1
            time.sleep(args.interval)

    print(f"Wrote {samples_written} samples to {args.output}",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
