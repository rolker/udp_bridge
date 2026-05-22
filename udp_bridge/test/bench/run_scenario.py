#!/usr/bin/env python3
"""Bench orchestrator: stand up a Linux user+net namespace, create veth
pairs, apply `tc qdisc netem` impairment, launch the bridges + pub/sub,
drive a scenario.

Re-execs self under `unshare -Urn` on first invocation, then performs
setup and teardown inside that ephemeral namespace.

Usage:
    run_scenario.py --scenario smoke [--duration-s 10] [--outdir DIR]

Smoke runs the bench in a clean-link configuration and verifies one
Critical-tier topic flows boat → bridge → operator → subscriber. The
`full-mix` scenario actively generates all three tiers (Critical /
Telemetry / Bulk) on a clean link and reports per-tier delivery
counts; Phase 3 will add `--scenario range_degradation` with
trajectory walking and the single-path invariants.

Environment:
    UDP_BRIDGE_BENCH_DEBUG=1  — keep child stderr visible.
"""

from __future__ import annotations

import argparse
import atexit
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

_INSIDE_ENV = 'UDP_BRIDGE_BENCH_INSIDE'
_DEBUG_ENV = 'UDP_BRIDGE_BENCH_DEBUG'

# Three-path topology IPs (operator-side .1, boat-side .2; /24 per path).
PATHS = [
    ('wifi', '10.0.10.1', '10.0.10.2'),
    ('cell', '10.0.20.1', '10.0.20.2'),
    ('starlink', '10.0.30.1', '10.0.30.2'),
]

# Clean baseline profiles, drawn from the 2026-05-18 pier calibration.
CLEAN_PROFILE = {
    'wifi': dict(rate='30mbit', loss='0%', delay_ms=5, jitter_ms=1),
    'cell': dict(rate='5mbit', loss='1%', delay_ms=60, jitter_ms=20),
    'starlink': dict(rate='8mbit', loss='0.4%', delay_ms=40, jitter_ms=30),
}

# DDS domains keep operator-side and boat-side nodes from discovering
# each other directly via DDS. They communicate via the bridge protocol
# (UDP across the veth pairs) only.
OPERATOR_DOMAIN = 42
BOAT_DOMAIN = 43

# Operator-side destination topics per tier. Must match the `destination`
# strings in configs/three_path.yaml. The source topics + msg types live
# in pub.TIER_DEFAULTS; this mapping is the bench-harness view of where
# delivered messages should land.
TIER_DESTINATIONS = {
    'critical': ('std_msgs/String', '/operator/boat/critical/heartbeat'),
    'telemetry': ('nav_msgs/Odometry', '/operator/boat/telemetry/odom'),
    'bulk': ('sensor_msgs/Image', '/operator/boat/bulk/image'),
}


@dataclass
class _Child:
    proc: subprocess.Popen
    name: str
    pgid: int

    def stop(self, timeout: float = 3.0) -> None:
        if self.proc.poll() is not None:
            return
        # Kill the whole process group — `ros2 run` spawns the C++ node
        # as a grandchild and SIGTERM to the parent doesn't always
        # propagate cleanly.
        try:
            os.killpg(self.pgid, signal.SIGTERM)
        except ProcessLookupError:
            return
        try:
            self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(self.pgid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            self.proc.wait()


_children: list[_Child] = []
_cleanup_ran = False


def _cleanup_all() -> None:
    global _cleanup_ran
    if _cleanup_ran:
        return
    _cleanup_ran = True
    for c in reversed(_children):
        c.stop()


def _install_signal_handlers() -> None:
    def handler(signum, _frame):
        _cleanup_all()
        # 128 + signum is the conventional shell-style exit code.
        os._exit(128 + signum)

    signal.signal(signal.SIGTERM, handler)
    signal.signal(signal.SIGINT, handler)


def _debug() -> bool:
    return os.environ.get(_DEBUG_ENV, '') not in ('', '0')


def _run(cmd: list[str], check: bool = True, **kwargs) -> subprocess.CompletedProcess:
    if _debug():
        print(f'[bench] $ {" ".join(cmd)}', file=sys.stderr)
    return subprocess.run(cmd, check=check, **kwargs)


def setup_namespace() -> None:
    """Bring `lo` up — required for DDS loopback discovery."""
    _run(['ip', 'link', 'set', 'lo', 'up'])


def setup_veth_pair(name: str, op_ip: str, bb_ip: str) -> None:
    """Create a veth pair, address both endpoints, bring them up."""
    op_dev = f'{name}_o'
    bb_dev = f'{name}_b'
    _run(['ip', 'link', 'add', op_dev, 'type', 'veth', 'peer', 'name', bb_dev])
    _run(['ip', 'addr', 'add', f'{op_ip}/24', 'dev', op_dev])
    _run(['ip', 'addr', 'add', f'{bb_ip}/24', 'dev', bb_dev])
    _run(['ip', 'link', 'set', op_dev, 'up'])
    _run(['ip', 'link', 'set', bb_dev, 'up'])


def apply_impairment(name: str, rate: str, loss: str,
                     delay_ms: int = 0, jitter_ms: int = 0) -> None:
    """Set or replace the `netem` qdisc on the operator-end veth.

    Shaping one direction is sufficient for the failure modes we test;
    each path has a single bottleneck in the real network too.
    """
    dev = f'{name}_o'
    cmd = ['tc', 'qdisc', 'replace', 'dev', dev, 'root', 'netem']
    if delay_ms > 0:
        cmd.extend(['delay', f'{delay_ms}ms'])
        if jitter_ms > 0:
            cmd.append(f'{jitter_ms}ms')
    if loss and loss not in ('0', '0%'):
        cmd.extend(['loss', loss])
    if rate:
        cmd.extend(['rate', rate])
    _run(cmd)


def setup_topology() -> None:
    setup_namespace()
    for name, op_ip, bb_ip in PATHS:
        setup_veth_pair(name, op_ip, bb_ip)
    for name, profile in CLEAN_PROFILE.items():
        apply_impairment(name, **profile)


def _env_for(domain: int) -> dict[str, str]:
    env = os.environ.copy()
    env['ROS_DOMAIN_ID'] = str(domain)
    return env


def _spawn(cmd: list[str], env: dict, name: str,
           stderr=None) -> _Child:
    """Popen helper that detaches the child into its own session/pgid
    so `os.killpg` can take down the whole subtree on cleanup.
    """
    proc = subprocess.Popen(
        cmd, env=env, stderr=stderr, start_new_session=True, text=True,
    )
    pgid = os.getpgid(proc.pid)
    child = _Child(proc, name, pgid)
    _children.append(child)
    return child


def launch_bridge(node_name: str, params_file: Path, domain: int) -> _Child:
    """Spawn a udp_bridge lifecycle node; transition to ACTIVATE."""
    env = _env_for(domain)
    stderr = None if _debug() else subprocess.DEVNULL
    child = _spawn(
        [
            'ros2', 'run', 'udp_bridge', 'udp_bridge_node',
            '--ros-args',
            '-r', f'__node:={node_name}',
            '--params-file', str(params_file),
        ],
        env=env,
        name=node_name,
        stderr=stderr,
    )
    time.sleep(2)  # let the node register with DDS
    for transition in ('configure', 'activate'):
        _run(
            ['ros2', 'lifecycle', 'set', f'/{node_name}', transition],
            env=env,
            stdout=subprocess.DEVNULL,
        )
    return child


def launch_pub(domain: int, tier: str, duration_s: float) -> _Child:
    env = _env_for(domain)
    script = Path(__file__).parent / 'pub.py'
    return _spawn(
        [sys.executable, str(script), '--tier', tier,
         '--duration-s', str(duration_s)],
        env=env,
        name=f'pub-{tier}',
        stderr=subprocess.PIPE,
    )


def launch_sub(domain: int, topic: str, msg_type: str, duration_s: float) -> _Child:
    env = _env_for(domain)
    script = Path(__file__).parent / 'sub.py'
    return _spawn(
        [sys.executable, str(script), '--topic', topic, '--msg-type', msg_type,
         '--duration-s', str(duration_s)],
        env=env,
        name=f'sub-{topic}',
        stderr=subprocess.PIPE,
    )


def _parse_count(child: _Child) -> int:
    """Read BENCH_*_COUNT=N from a finished child's stderr."""
    if child.proc.stderr is None:
        return -1
    out = child.proc.stderr.read() or ''
    for line in out.splitlines():
        if 'COUNT=' in line:
            try:
                return int(line.split('=', 1)[1].strip())
            except ValueError:
                continue
    return -1


def run_smoke(duration_s: float) -> dict:
    """Single Critical-tier topic, clean link on all three paths."""
    config = Path(__file__).parent / 'configs' / 'three_path.yaml'
    setup_topology()

    launch_bridge('boat_bridge', config, BOAT_DOMAIN)
    launch_bridge('operator_bridge', config, OPERATOR_DOMAIN)
    time.sleep(2)  # let the bridges handshake

    sub = launch_sub(
        OPERATOR_DOMAIN, '/operator/boat/critical/heartbeat',
        'std_msgs/String', duration_s + 2,
    )
    time.sleep(0.5)  # let the subscriber's discovery catch up
    pub = launch_pub(BOAT_DOMAIN, 'critical', duration_s)

    pub.proc.wait(timeout=duration_s + 10)
    sub.proc.wait(timeout=duration_s + 15)

    return {
        'pub_count': _parse_count(pub),
        'sub_count': _parse_count(sub),
    }


def run_full_mix(duration_s: float) -> dict:
    """All three tiers active on a clean link.

    Spawns one publisher per tier on the boat side and one subscriber
    per destination topic on the operator side. Bulk is sized to ~120%
    of the WiFi rate budget, so the bridge's rate limiter will trim it
    even on a clean link — that's intentional (Phase 4 drop-by-tier
    invariant), and Phase 2 just confirms each tier delivers something.
    """
    config = Path(__file__).parent / 'configs' / 'three_path.yaml'
    setup_topology()

    launch_bridge('boat_bridge', config, BOAT_DOMAIN)
    launch_bridge('operator_bridge', config, OPERATOR_DOMAIN)
    time.sleep(2)  # let the bridges handshake

    subs = {
        tier: launch_sub(OPERATOR_DOMAIN, topic, msg_type, duration_s + 2)
        for tier, (msg_type, topic) in TIER_DESTINATIONS.items()
    }
    time.sleep(0.5)  # let subscriber discovery catch up
    pubs = {
        tier: launch_pub(BOAT_DOMAIN, tier, duration_s)
        for tier in TIER_DESTINATIONS
    }

    for tier in TIER_DESTINATIONS:
        pubs[tier].proc.wait(timeout=duration_s + 10)
        subs[tier].proc.wait(timeout=duration_s + 15)

    return {
        tier: {
            'pub_count': _parse_count(pubs[tier]),
            'sub_count': _parse_count(subs[tier]),
        }
        for tier in TIER_DESTINATIONS
    }


def _reexec_under_unshare(argv: list[str]) -> None:
    os.environ[_INSIDE_ENV] = '1'
    os.execvp('unshare', ['unshare', '-Urn', sys.executable, sys.argv[0]] + argv)


def main(argv: list[str] | None = None) -> None:
    argv = list(argv) if argv is not None else sys.argv[1:]
    parser = argparse.ArgumentParser(description='udp_bridge bench orchestrator')
    parser.add_argument('--scenario',
                        choices=['smoke', 'full-mix', 'range_degradation'],
                        required=True)
    parser.add_argument('--duration-s', type=float, default=10.0)
    parser.add_argument('--outdir', type=Path,
                        help='Output directory for bag/log artifacts. '
                             'Defaults to a fresh tempdir (preserved on fail).')
    args = parser.parse_args(argv)

    if not os.environ.get(_INSIDE_ENV):
        _reexec_under_unshare(argv)

    atexit.register(_cleanup_all)
    _install_signal_handlers()
    outdir = args.outdir or Path(tempfile.mkdtemp(prefix='udp_bridge_bench_'))
    outdir.mkdir(parents=True, exist_ok=True)
    print(f'BENCH_OUTDIR={outdir}')

    try:
        if args.scenario == 'smoke':
            result = run_smoke(args.duration_s)
            print(f'BENCH_RESULT_PUB_COUNT={result["pub_count"]}')
            print(f'BENCH_RESULT_SUB_COUNT={result["sub_count"]}')
            ok = result['sub_count'] > 0
            if ok and not _debug():
                shutil.rmtree(outdir, ignore_errors=True)
            sys.exit(0 if ok else 1)
        elif args.scenario == 'full-mix':
            result = run_full_mix(args.duration_s)
            for tier, counts in result.items():
                print(f'BENCH_RESULT_{tier.upper()}_PUB_COUNT={counts["pub_count"]}')
                print(f'BENCH_RESULT_{tier.upper()}_SUB_COUNT={counts["sub_count"]}')
            ok = all(c['sub_count'] > 0 for c in result.values())
            if ok and not _debug():
                shutil.rmtree(outdir, ignore_errors=True)
            sys.exit(0 if ok else 1)
        else:
            print(f'Scenario {args.scenario} not yet implemented '
                  f'(Phase 3 of #18)', file=sys.stderr)
            sys.exit(2)
    finally:
        _cleanup_all()


if __name__ == '__main__':
    main()
