#!/usr/bin/env python3
"""Bench orchestrator: stand up a Linux user+net namespace, create veth
pairs, apply `tc qdisc netem` impairment, launch the bridges + pub/sub,
drive a scenario.

Re-execs self under `unshare -Urn` on first invocation, then performs
setup and teardown inside that ephemeral namespace.

Usage:
    run_scenario.py --scenario smoke      [--duration-s 10] [--outdir DIR]
    run_scenario.py --scenario full-mix   [--duration-s 10] [--outdir DIR]
    run_scenario.py --scenario range_degradation [--hold-s 10] [--outdir DIR]
    run_scenario.py --scenario subscriber_death  [--outdir DIR]

Smoke runs the bench in a clean-link configuration and verifies one
Critical-tier topic flows boat → bridge → operator → subscriber. The
`full-mix` scenario actively generates all three tiers (Critical /
Telemetry / Bulk) on a clean link and reports per-tier delivery
counts. The `range_degradation` scenario walks the WiFi path through
a sail-out-and-return trajectory, captures a `bridge_info` /
`topic_statistics` bag plus a Recv-Q CSV trace, and emits the marker
lines (`BENCH_BAG_DIR=`, `BENCH_PHASE_LOG=`, `BENCH_RECVQ_OPERATOR=`,
`BENCH_BRIDGE_STDERR_OPERATOR=`, …) that
`test_range_degradation.py` consumes to evaluate the single-path
invariants. The `subscriber_death` scenario freezes (SIGSTOP) the Bulk
operator subscriber mid-stream and traces the operator's Recv-Q plus
survivor-topic delivery; it is a no-wedge regression guard (see
`test_subscriber_death.py` and README.md for why it does not reproduce
the issue #10 wedge at bench scale).

Environment:
    UDP_BRIDGE_BENCH_DEBUG=1  — keep child stderr visible.
"""

from __future__ import annotations

import argparse
import atexit
import json
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

# Sail-out-and-return trajectory applied to the WiFi path only (cell
# and Starlink hold their CLEAN_PROFILE baselines). The phase names
# are written into the phase log so `test_range_degradation.py` can
# slice bag data by phase; the over_horizon phase is special-cased
# because invariant 2 forbids ERROR-severity logs during that window.
PHASE_TRAJECTORY = [
    ('in_range_clean', dict(rate='30mbit',  loss='0%',   delay_ms=5,   jitter_ms=1)),
    ('fringe',         dict(rate='10mbit',  loss='0.5%', delay_ms=15,  jitter_ms=5)),
    ('lossy',          dict(rate='3mbit',   loss='3%',   delay_ms=40,  jitter_ms=15)),
    ('critical',       dict(rate='500kbit', loss='10%',  delay_ms=100, jitter_ms=30)),
    ('over_horizon',   dict(rate='30mbit',  loss='100%', delay_ms=5,   jitter_ms=1)),
    ('critical',       dict(rate='500kbit', loss='10%',  delay_ms=100, jitter_ms=30)),
    ('lossy',          dict(rate='3mbit',   loss='3%',   delay_ms=40,  jitter_ms=15)),
    ('fringe',         dict(rate='10mbit',  loss='0.5%', delay_ms=15,  jitter_ms=5)),
    ('in_range_clean', dict(rate='30mbit',  loss='0%',   delay_ms=5,   jitter_ms=1)),
]

# Topics captured by ros2 bag record on the operator domain. The
# operator bridge mirrors the boat's bridge_info / topic_statistics
# under `/remotes/boat/...`, so a single recorder on one domain is
# sufficient for the single-path invariants.
BAG_TOPICS = [
    '/operator_bridge/bridge_info',
    '/operator_bridge/topic_statistics',
    '/operator_bridge/remotes/boat/bridge_info',
    '/operator_bridge/remotes/boat/topic_statistics',
    # The republished Critical heartbeat — its arrival stamps let the
    # multi-link "critical gap during over-horizon" invariant verify the
    # always-on (cell/starlink) paths keep it flowing when WiFi is dark.
    '/operator/boat/critical/heartbeat',
]


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


# PID of the keeper process holding the boat-side network namespace open.
# The orchestrator's own netns (from the `unshare -Urn` re-exec) is the
# OPERATOR side; the boat side is a nested namespace created at setup.
#
# Why two namespaces and not one: with both ends of a veth pair addressed in
# a single namespace, the kernel installs a `local` route for each address and
# delivers between them over `lo` -- the packets never egress the veth, so the
# `tc netem` qdisc attached to it is never traversed and every impairment is
# silently a no-op. Verified directly: a 100%-loss qdisc on a same-namespace
# pair drops nothing and the device's TX counter stays at zero. Splitting the
# ends across namespaces is what makes the shaping real. Do NOT "simplify"
# this back into one namespace.
_BOAT_NS_PID: int | None = None

# Namespace selectors accepted by the launch/_run helpers. None (the default)
# means "this process's namespace", i.e. the operator side.
BOAT_NS = 'boat'


def _ns_prefix(netns: str | None) -> list[str]:
    """Command prefix that runs `cmd` inside `netns`."""
    if netns is None:
        return []
    if netns != BOAT_NS:
        raise ValueError(f'unknown namespace selector: {netns!r}')
    if _BOAT_NS_PID is None:
        raise RuntimeError('boat namespace not created; call setup_topology() first')
    return ['nsenter', '-t', str(_BOAT_NS_PID), '-n']


def _run(cmd: list[str], check: bool = True, netns: str | None = None,
         **kwargs) -> subprocess.CompletedProcess:
    cmd = _ns_prefix(netns) + cmd
    if _debug():
        print(f'[bench] $ {" ".join(cmd)}', file=sys.stderr)
    return subprocess.run(cmd, check=check, **kwargs)


def setup_boat_namespace() -> None:
    """Create the boat-side network namespace and hold it open.

    `unshare -Urn` already gave this process CAP_NET_ADMIN/CAP_SYS_ADMIN in
    its own user namespace, so a nested `unshare -n` needs no extra privilege.
    A keeper process is the namespace handle -- we address it by PID via
    `nsenter -t <pid> -n` rather than `ip netns add`, which would need a
    writable /run/netns and therefore a private mount namespace too.
    """
    global _BOAT_NS_PID
    if _BOAT_NS_PID is not None:
        return
    keeper = _spawn(
        ['unshare', '-n', 'sleep', 'infinity'],
        env=os.environ.copy(),
        name='boat-netns-keeper',
        stderr=subprocess.DEVNULL if not _debug() else None,
    )
    # Wait for the namespace to exist before anyone tries to enter it.
    ns_link = Path('/proc') / str(keeper.proc.pid) / 'ns' / 'net'
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        if ns_link.exists() and keeper.proc.poll() is None:
            break
        time.sleep(0.05)
    else:
        raise RuntimeError('boat namespace keeper did not come up')
    if keeper.proc.poll() is not None:
        raise RuntimeError('boat namespace keeper exited immediately')
    _BOAT_NS_PID = keeper.proc.pid


def setup_namespace() -> None:
    """Bring `lo` up in both namespaces — required for DDS discovery."""
    _run(['ip', 'link', 'set', 'lo', 'up'])
    _run(['ip', 'link', 'set', 'lo', 'up'], netns=BOAT_NS)


def setup_veth_pair(name: str, op_ip: str, bb_ip: str) -> None:
    """Create a veth pair with one end in each namespace, address both ends."""
    op_dev = f'{name}_o'
    bb_dev = f'{name}_b'
    _run(['ip', 'link', 'add', op_dev, 'type', 'veth', 'peer', 'name', bb_dev])
    # Move the boat end across before addressing it: an address configured
    # here would be dropped by the namespace move.
    _run(['ip', 'link', 'set', bb_dev, 'netns', str(_BOAT_NS_PID)])
    _run(['ip', 'addr', 'add', f'{op_ip}/24', 'dev', op_dev])
    _run(['ip', 'link', 'set', op_dev, 'up'])
    _run(['ip', 'addr', 'add', f'{bb_ip}/24', 'dev', bb_dev], netns=BOAT_NS)
    _run(['ip', 'link', 'set', bb_dev, 'up'], netns=BOAT_NS)


def apply_impairment(name: str, rate: str, loss: str,
                     delay_ms: int = 0, jitter_ms: int = 0) -> None:
    """Set or replace the `netem` qdisc on BOTH ends of a path.

    netem shapes egress only, so one qdisc impairs one direction. The
    dominant bench flow is boat -> operator (the bulk/telemetry traffic),
    while resend requests and subscribe requests run operator -> boat; a
    radio link degrades both. Shaping each end's egress with the same
    profile models that symmetrically.

    Consequence worth remembering when reading thresholds: `delay_ms` is a
    ONE-WAY delay, so a round trip now costs 2 x delay_ms.
    """
    for dev, netns in ((f'{name}_o', None), (f'{name}_b', BOAT_NS)):
        cmd = ['tc', 'qdisc', 'replace', 'dev', dev, 'root', 'netem']
        if delay_ms > 0:
            cmd.extend(['delay', f'{delay_ms}ms'])
            if jitter_ms > 0:
                cmd.append(f'{jitter_ms}ms')
        if loss and loss not in ('0', '0%'):
            cmd.extend(['loss', loss])
        if rate:
            cmd.extend(['rate', rate])
        _run(cmd, netns=netns)


def setup_topology() -> None:
    setup_boat_namespace()
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
           stderr=None, netns: str | None = None) -> _Child:
    """Popen helper that detaches the child into its own session/pgid
    so `os.killpg` can take down the whole subtree on cleanup.

    `netns` selects the network namespace to run in; the `nsenter` wrapper
    stays inside the new session, so the pgid still covers the whole subtree.
    """
    cmd = _ns_prefix(netns) + cmd
    proc = subprocess.Popen(
        cmd, env=env, stderr=stderr, start_new_session=True, text=True,
    )
    pgid = os.getpgid(proc.pid)
    child = _Child(proc, name, pgid)
    _children.append(child)
    return child


def launch_bridge(node_name: str, params_file: Path, domain: int,
                  netns: str | None = None) -> _Child:
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
        netns=netns,
    )
    time.sleep(2)  # let the node register with DDS
    for transition in ('configure', 'activate'):
        # The lifecycle client must speak to the node over the same
        # namespace's loopback, so it runs in `netns` too.
        _run(
            ['ros2', 'lifecycle', 'set', f'/{node_name}', transition],
            env=env,
            stdout=subprocess.DEVNULL,
            netns=netns,
        )
    return child


def launch_pub(domain: int, tier: str, duration_s: float,
               netns: str | None = None) -> _Child:
    env = _env_for(domain)
    script = Path(__file__).parent / 'pub.py'
    return _spawn(
        [sys.executable, str(script), '--tier', tier,
         '--duration-s', str(duration_s)],
        env=env,
        name=f'pub-{tier}',
        stderr=subprocess.PIPE,
        netns=netns,
    )


def launch_sub(domain: int, topic: str, msg_type: str, duration_s: float,
               count_trace: Path | None = None) -> _Child:
    env = _env_for(domain)
    script = Path(__file__).parent / 'sub.py'
    cmd = [sys.executable, str(script), '--topic', topic, '--msg-type', msg_type,
           '--duration-s', str(duration_s)]
    if count_trace is not None:
        cmd.extend(['--count-trace', str(count_trace)])
    return _spawn(
        cmd,
        env=env,
        name=f'sub-{topic}',
        stderr=subprocess.PIPE,
    )


def launch_recv_q_trace(port: int, output_csv: Path,
                        interval_s: float = 0.5) -> _Child:
    """Run recv_q_trace.py against a UDP port; writes CSV until SIGTERM."""
    script = Path(__file__).parent / 'recv_q_trace.py'
    return _spawn(
        [sys.executable, str(script),
         '--port', str(port),
         '--interval', str(interval_s),
         '--output', str(output_csv)],
        env=os.environ.copy(),
        name=f'recv_q-{port}',
        stderr=subprocess.DEVNULL if not _debug() else None,
    )


def launch_bag_record(domain: int, topics: list[str], bag_dir: Path) -> _Child:
    """Run `ros2 bag record` on the operator domain into bag_dir."""
    env = _env_for(domain)
    return _spawn(
        ['ros2', 'bag', 'record', '-o', str(bag_dir)] + topics,
        env=env,
        name='bag_record',
        stderr=subprocess.DEVNULL if not _debug() else None,
    )


def launch_bridge_logged(node_name: str, params_file: Path, domain: int,
                         stderr_log: Path, netns: str | None = None) -> _Child:
    """Like `launch_bridge` but captures the bridge's stderr to `stderr_log`.

    Phase 3 needs per-bridge stderr captured so we can check invariant
    2 (no ERROR-severity log during the over-horizon window). We can't
    redirect to a file inside the Popen because we still need the
    lifecycle transitions to be issued; the cleanest approach is to
    open the log file in append mode and pass it as `stderr=`.

    Note this always redirects: unlike `launch_bridge`, there is no
    UDP_BRIDGE_BENCH_DEBUG passthrough to the terminal here, because the
    invariant checks need the stderr in a file. Read the log if you are
    debugging a scenario that uses this launcher.
    """
    env = _env_for(domain)
    # Opened here and closed as soon as the child is spawned: Popen dups the
    # fd into the child, so the parent's handle is dead weight afterwards.
    # Scenarios launch several bridges per run, and the process is long-lived.
    log_fp = open(stderr_log, 'ab')
    child = _spawn(
        [
            'ros2', 'run', 'udp_bridge', 'udp_bridge_node',
            '--ros-args',
            '-r', f'__node:={node_name}',
            '--params-file', str(params_file),
        ],
        env=env,
        name=node_name,
        stderr=log_fp,
        netns=netns,
    )
    log_fp.close()
    time.sleep(2)  # let the node register with DDS
    for transition in ('configure', 'activate'):
        _run(
            ['ros2', 'lifecycle', 'set', f'/{node_name}', transition],
            env=env,
            stdout=subprocess.DEVNULL,
            netns=netns,
        )
    return child


# The #10 wedge is rmw_zenoh_cpp-specific: Zenoh's reliability handshake
# blocks the republish to a dead-but-still-matched reader far longer than
# DDS's bounded max_blocking_time, so the operator drain backs up. FastDDS
# and CycloneDDS do not reproduce it (verified — Recv-Q stays flat). The
# subscriber_death scenario therefore forces Zenoh + its router daemon.
ZENOH_RMW = 'rmw_zenoh_cpp'


def zenoh_available() -> bool:
    """True if rmw_zenoh_cpp and its router daemon are installed."""
    try:
        out = subprocess.run(
            ['ros2', 'pkg', 'executables', 'rmw_zenoh_cpp'],
            capture_output=True, text=True, check=False,
        )
        return 'rmw_zenohd' in out.stdout
    except (OSError, subprocess.SubprocessError):
        return False


def launch_zenoh_router(netns: str | None = None) -> _Child:
    """Start the Zenoh router daemon. rmw_zenoh_cpp sessions need it for
    discovery; sessions reach it on their own namespace's localhost:7447.

    Each network namespace needs its own router: the boat and operator
    sides no longer share a loopback, so one router cannot serve both.
    """
    return _spawn(
        ['ros2', 'run', 'rmw_zenoh_cpp', 'rmw_zenohd'],
        env=os.environ.copy(),
        name=f'zenohd-{netns or "operator"}',
        stderr=subprocess.DEVNULL if not _debug() else None,
        netns=netns,
    )


def walk_wifi(hold_s: float, phase_log: list) -> None:
    """Step the WiFi path through PHASE_TRAJECTORY, holding each entry
    for `hold_s` seconds. Appends (t_offset_s, phase_name) tuples to
    `phase_log` at every transition.
    """
    t0 = time.monotonic()
    for phase_name, profile in PHASE_TRAJECTORY:
        t_offset = time.monotonic() - t0
        phase_log.append((t_offset, phase_name))
        apply_impairment('wifi', **profile)
        time.sleep(hold_s)
    # Record the end-of-walk marker so the test knows when the
    # recovery leg's in-range phase ended.
    phase_log.append((time.monotonic() - t0, 'walk_end'))


def run_range_degradation(hold_s: float, outdir: Path) -> dict:
    """Walk the WiFi path through the trajectory while the full
    three-tier mix runs; capture bag + Recv-Q + bridge stderr for
    test_range_degradation.py to evaluate.

    Phase-hold seconds default to 10. Total walk time ≈ 9 × hold_s
    (5 forward phases + 4 recovery phases). Add ~10s for warmup and
    ~10s for shutdown/drain. `outdir` collects:
      - bag/        ros2 bag of operator-side bridge_info / topic_statistics
      - recv_q_operator.csv
      - bridge_stderr_operator.log, bridge_stderr_boat.log
      - phase_log.json   list of {t_offset_s, phase} dicts
    """
    config = Path(__file__).parent / 'configs' / 'three_path.yaml'
    setup_topology()

    operator_stderr = outdir / 'bridge_stderr_operator.log'
    boat_stderr = outdir / 'bridge_stderr_boat.log'
    launch_bridge_logged('boat_bridge', config, BOAT_DOMAIN, boat_stderr,
                         netns=BOAT_NS)
    launch_bridge_logged('operator_bridge', config, OPERATOR_DOMAIN, operator_stderr)
    time.sleep(2)  # let the bridges handshake

    bag_dir = outdir / 'bag'
    bag = launch_bag_record(OPERATOR_DOMAIN, BAG_TOPICS, bag_dir)
    recvq_csv = outdir / 'recv_q_operator.csv'
    recvq = launch_recv_q_trace(4200, recvq_csv, interval_s=0.5)
    time.sleep(2)  # let recorder + tracer settle

    # We need pub/sub to outlive the walk by a couple of seconds so the
    # final in_range_clean recovery phase has steady-state data.
    pub_duration = hold_s * len(PHASE_TRAJECTORY) + 4
    subs = {
        tier: launch_sub(OPERATOR_DOMAIN, topic, msg_type, pub_duration + 2)
        for tier, (msg_type, topic) in TIER_DESTINATIONS.items()
    }
    time.sleep(0.5)
    pubs = {
        tier: launch_pub(BOAT_DOMAIN, tier, pub_duration, netns=BOAT_NS)
        for tier in TIER_DESTINATIONS
    }

    walk_start = time.time()
    phase_log: list = []
    walk_wifi(hold_s, phase_log)
    walk_end = time.time()

    for tier in TIER_DESTINATIONS:
        pubs[tier].proc.wait(timeout=pub_duration + 10)
        subs[tier].proc.wait(timeout=pub_duration + 15)

    # Stop the tracers gracefully so files flush before cleanup.
    recvq.stop(timeout=3.0)
    # `ros2 bag record` only flushes the metadata on SIGINT; SIGTERM
    # to the wrapper truncates the bag. Send SIGINT to the whole
    # process group instead of the SIGTERM path that _Child.stop uses.
    if bag.proc.poll() is None:
        try:
            os.killpg(bag.pgid, signal.SIGINT)
            bag.proc.wait(timeout=10.0)
        except (ProcessLookupError, subprocess.TimeoutExpired):
            bag.stop(timeout=3.0)

    phase_log_path = outdir / 'phase_log.json'
    phase_log_path.write_text(json.dumps({
        'walk_start_unix': walk_start,
        'walk_end_unix': walk_end,
        'hold_s': hold_s,
        'phases': [{'t_offset_s': t, 'phase': name} for t, name in phase_log],
    }, indent=2))

    return {
        'bag_dir': bag_dir,
        'phase_log': phase_log_path,
        'recvq_operator': recvq_csv,
        'bridge_stderr_operator': operator_stderr,
        'bridge_stderr_boat': boat_stderr,
        'pub_counts': {tier: _parse_count(pubs[tier]) for tier in TIER_DESTINATIONS},
        'sub_counts': {tier: _parse_count(subs[tier]) for tier in TIER_DESTINATIONS},
    }


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

    launch_bridge('boat_bridge', config, BOAT_DOMAIN, netns=BOAT_NS)
    launch_bridge('operator_bridge', config, OPERATOR_DOMAIN)
    time.sleep(2)  # let the bridges handshake

    sub = launch_sub(
        OPERATOR_DOMAIN, '/operator/boat/critical/heartbeat',
        'std_msgs/String', duration_s + 2,
    )
    time.sleep(0.5)  # let the subscriber's discovery catch up
    pub = launch_pub(BOAT_DOMAIN, 'critical', duration_s, netns=BOAT_NS)

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

    launch_bridge('boat_bridge', config, BOAT_DOMAIN, netns=BOAT_NS)
    launch_bridge('operator_bridge', config, OPERATOR_DOMAIN)
    time.sleep(2)  # let the bridges handshake

    subs = {
        tier: launch_sub(OPERATOR_DOMAIN, topic, msg_type, duration_s + 2)
        for tier, (msg_type, topic) in TIER_DESTINATIONS.items()
    }
    time.sleep(0.5)  # let subscriber discovery catch up
    pubs = {
        tier: launch_pub(BOAT_DOMAIN, tier, duration_s, netns=BOAT_NS)
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


def run_subscriber_death(hold_s: float, outdir: Path) -> dict:
    """Reproduce the issue #10 wedge: STALL the Bulk operator-side subscriber
    mid-run (SIGSTOP — a frozen-but-matched reader that stops draining is the
    back-pressure trigger; a cleanly-killed one just unmatches) while the boat
    keeps forwarding Bulk at the saturating rate. Trace the operator bridge's
    UDP Recv-Q (port 4200) throughout; count-trace the surviving Critical /
    Telemetry subscribers so the test can compare delivery rate before vs.
    after the stall (the head-of-line question, issue #29).

    Clean link on all paths — this isolates the stalled-subscriber failure
    mode from link degradation. Timeline (hold_s each window):
      [0, hold_s)          baseline: all three tiers flowing
      stall at t≈hold_s    SIGSTOP the Bulk operator subscriber
      [hold_s, 2*hold_s)   observe: boat keeps publishing; watch Recv-Q

    Pre-#10-fix the operator drain thread blocks in the stalled republish and
    Recv-Q on 4200 climbs; with the fix the publish moves to the worker, the
    drain keeps reading, and Recv-Q stays bounded.
    """
    # Force Zenoh — the wedge does not reproduce under DDS (see ZENOH_RMW).
    os.environ['RMW_IMPLEMENTATION'] = ZENOH_RMW
    os.environ.setdefault('ZENOH_ROUTER_CHECK_ATTEMPTS', '10')

    config = Path(__file__).parent / 'configs' / 'three_path.yaml'
    setup_topology()

    # Routers first, before any session, so the bridges + pub/sub discover.
    # One per namespace: the boat and operator sides no longer share a
    # loopback, so a single router cannot serve both.
    launch_zenoh_router()
    launch_zenoh_router(netns=BOAT_NS)
    time.sleep(3)  # let the routers bind their namespace's localhost:7447

    operator_stderr = outdir / 'bridge_stderr_operator.log'
    boat_stderr = outdir / 'bridge_stderr_boat.log'
    launch_bridge_logged('boat_bridge', config, BOAT_DOMAIN, boat_stderr,
                         netns=BOAT_NS)
    launch_bridge_logged('operator_bridge', config, OPERATOR_DOMAIN, operator_stderr)
    time.sleep(2)  # let the bridges handshake

    recvq_csv = outdir / 'recv_q_operator.csv'
    recvq_start_unix = time.time()
    recvq = launch_recv_q_trace(4200, recvq_csv, interval_s=0.5)
    time.sleep(1)  # let the tracer find the socket

    # Run long enough for baseline + observe + a drain margin. Subscribers
    # outlive the publishers by 2s so their final counts/traces are complete.
    run_s = 2 * hold_s + 4
    count_traces = {
        'critical': outdir / 'count_critical.csv',
        'telemetry': outdir / 'count_telemetry.csv',
        # Trace the victim too: its count climbs in baseline then flatlines at
        # the stall — confirms Bulk was actually flowing into the republish
        # path before we froze its consumer.
        'bulk': outdir / 'count_bulk.csv',
    }
    subs = {
        tier: launch_sub(OPERATOR_DOMAIN, topic, msg_type, run_s + 2,
                         count_trace=count_traces.get(tier))
        for tier, (msg_type, topic) in TIER_DESTINATIONS.items()
    }
    time.sleep(0.5)  # let subscriber discovery catch up
    pubs = {
        tier: launch_pub(BOAT_DOMAIN, tier, run_s, netns=BOAT_NS)
        for tier in TIER_DESTINATIONS
    }

    # Baseline window, then STALL the Bulk operator subscriber with SIGSTOP.
    # A frozen-but-alive reader is the back-pressure trigger that actually
    # wedges the bridge: the RMW session stays matched, the reader stops
    # draining, its buffers fill, it stops ACKing, and a RELIABLE destination
    # publisher blocks. A cleanly-killed (SIGKILL) reader instead closes its
    # session → the publisher unmatches → no block → no wedge (verified).
    time.sleep(hold_s)
    victim = subs['bulk']
    stall_unix = time.time()
    try:
        os.killpg(victim.pgid, signal.SIGSTOP)
    except ProcessLookupError:
        pass

    # Observe window: boat keeps forwarding Bulk to the frozen subscriber.
    time.sleep(hold_s)

    # Teardown the frozen victim: SIGTERM is queued (undeliverable) until a
    # process is continued, so SIGCONT then SIGKILL, then reap it so it doesn't
    # linger as a zombie until the atexit cleanup.
    try:
        os.killpg(victim.pgid, signal.SIGCONT)
        os.killpg(victim.pgid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    try:
        victim.proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        pass

    # Let survivors + publishers finish and flush their counts/traces.
    for tier in ('critical', 'telemetry'):
        try:
            subs[tier].proc.wait(timeout=run_s + 10)
        except subprocess.TimeoutExpired:
            pass
    for tier in TIER_DESTINATIONS:
        try:
            pubs[tier].proc.wait(timeout=run_s + 10)
        except subprocess.TimeoutExpired:
            pass
    recvq.stop(timeout=3.0)

    meta_path = outdir / 'death_meta.json'
    meta_path.write_text(json.dumps({
        'recvq_start_unix': recvq_start_unix,
        'stall_unix': stall_unix,
        'hold_s': hold_s,
        'victim_tier': 'bulk',
    }, indent=2))

    return {
        'recvq_operator': recvq_csv,
        'death_meta': meta_path,
        'count_trace_critical': count_traces['critical'],
        'count_trace_telemetry': count_traces['telemetry'],
        'bridge_stderr_operator': operator_stderr,
        'bridge_stderr_boat': boat_stderr,
        'sub_counts': {t: _parse_count(subs[t]) for t in ('critical', 'telemetry')},
    }


def _reexec_under_unshare(argv: list[str]) -> None:
    os.environ[_INSIDE_ENV] = '1'
    os.execvp('unshare', ['unshare', '-Urn', sys.executable, sys.argv[0]] + argv)


def main(argv: list[str] | None = None) -> None:
    argv = list(argv) if argv is not None else sys.argv[1:]
    parser = argparse.ArgumentParser(description='udp_bridge bench orchestrator')
    parser.add_argument('--scenario',
                        choices=['smoke', 'full-mix', 'range_degradation',
                                 'subscriber_death'],
                        required=True)
    parser.add_argument('--duration-s', type=float, default=10.0,
                        help='Run duration for smoke and full-mix scenarios.')
    parser.add_argument('--hold-s', type=float, default=10.0,
                        help='Per-phase hold time for range_degradation.')
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
        elif args.scenario == 'range_degradation':
            result = run_range_degradation(args.hold_s, outdir)
            print(f'BENCH_BAG_DIR={result["bag_dir"]}')
            print(f'BENCH_PHASE_LOG={result["phase_log"]}')
            print(f'BENCH_RECVQ_OPERATOR={result["recvq_operator"]}')
            print(f'BENCH_BRIDGE_STDERR_OPERATOR={result["bridge_stderr_operator"]}')
            print(f'BENCH_BRIDGE_STDERR_BOAT={result["bridge_stderr_boat"]}')
            for tier, count in result['pub_counts'].items():
                print(f'BENCH_RESULT_{tier.upper()}_PUB_COUNT={count}')
            for tier, count in result['sub_counts'].items():
                print(f'BENCH_RESULT_{tier.upper()}_SUB_COUNT={count}')
            # Pass condition for the orchestrator alone is "the run
            # completed and produced artifacts". Invariants are
            # evaluated by test_range_degradation.py.
            ok = (
                result['bag_dir'].exists()
                and result['phase_log'].exists()
                and any(c > 0 for c in result['sub_counts'].values())
            )
            # Preserve artifacts regardless of pass/fail — the
            # downstream test needs them.
            sys.exit(0 if ok else 1)
        elif args.scenario == 'subscriber_death':
            result = run_subscriber_death(args.hold_s, outdir)
            print(f'BENCH_RECVQ_OPERATOR={result["recvq_operator"]}')
            print(f'BENCH_DEATH_META={result["death_meta"]}')
            print(f'BENCH_COUNT_TRACE_CRITICAL={result["count_trace_critical"]}')
            print(f'BENCH_COUNT_TRACE_TELEMETRY={result["count_trace_telemetry"]}')
            print(f'BENCH_BRIDGE_STDERR_OPERATOR={result["bridge_stderr_operator"]}')
            print(f'BENCH_BRIDGE_STDERR_BOAT={result["bridge_stderr_boat"]}')
            for tier, count in result['sub_counts'].items():
                print(f'BENCH_RESULT_{tier.upper()}_SUB_COUNT={count}')
            # Orchestrator success = "the run produced the artifacts the test
            # needs". The wedge/head-of-line invariants are evaluated by
            # test_subscriber_death.py. Preserve artifacts regardless.
            ok = (
                result['recvq_operator'].exists()
                and result['death_meta'].exists()
                and result['count_trace_critical'].exists()
            )
            sys.exit(0 if ok else 1)
        else:
            print(f'Scenario {args.scenario} not recognized',
                  file=sys.stderr)
            sys.exit(2)
    finally:
        _cleanup_all()


if __name__ == '__main__':
    main()
