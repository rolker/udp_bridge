"""Stalled-subscriber (wedge) regression guard for udp_bridge#10 / #18.

Runs the `subscriber_death` scenario: a Bulk operator-side subscriber is
frozen (SIGSTOP) mid-stream while the boat keeps forwarding Bulk, and the
operator bridge's UDP Recv-Q (port 4200) is traced throughout. The intent
is to reproduce the issue #10 wedge — a stalled consumer back-pressuring a
RELIABLE destination publisher until the socket-drain thread blocks and the
kernel Recv-Q backs up.

IMPORTANT — what this test does and does NOT establish (measured 2026-05-25):
The wedge could **not** be reproduced at bench scale on this harness, across
FastDDS, CycloneDDS, AND rmw_zenoh_cpp, with either a clean-killed or a frozen
consumer, with Bulk streaming into the republish path. In every case the
operator Recv-Q (and Send-Q) stayed flat at 0 and the surviving tiers were
unaffected. The mechanism: at bench scale `publish()` does not block the drain
thread — under Zenoh in particular the publish is an async hand-off to the
session. The field wedge (4× on 2026-04-27, under rmw_zenoh_cpp) evidently
needs conditions this harness can't hit at bench scale (far higher sustained
volume to exhaust Zenoh's internal buffers, the real fragmented costmap topic,
over-horizon/router dynamics) — or a root cause other than the back-pressure
hypothesis the issue records.

Correction (#57): that 2026-05-25 run used the pre-#57 all-zeros Bulk payload,
which compressed to a single ~488 B packet, so the harness pushed only ~5 kB/s
of single-packet traffic — NOT the ~58 MB/s this note originally claimed. The
no-wedge finding was therefore reached in an unrepresentative,
zero-fragmentation regime. #57 makes this scenario stream real incompressible,
fragmented Bulk (~4.8 MB/s, ~480+ fragments per image), so the finding must be
re-validated on the host bench run before it can be trusted.

So this is a **no-wedge regression guard**, not a red→green demo: it asserts
the healthy behavior (Recv-Q stays bounded, surviving tiers keep delivering)
and would fail if a future change reintroduced drain-blocking under an RMW
that blocks. The component-level proof of the #10 fix is the `PublishQueue`
unit tests in `test_publish_queue.cpp`; this is the system-level guard.

Skipped unless UDP_BRIDGE_BENCH_SCENARIOS=1. Also skipped without
`unshare -Urn`, ROS 2, or rmw_zenoh_cpp (the scenario forces Zenoh, since
DDS does not exhibit the back-pressure path at all).
"""

from __future__ import annotations

import csv
import json
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

import pytest


THIS_DIR = Path(__file__).parent

THRESHOLDS = {
    # Post-stall operator Recv-Q ceiling. Healthy is ~0 (drain keeps up);
    # the field wedge backed up ~361 KB toward the ~426 KB SO_RCVBUF. 100 KB
    # cleanly separates a healthy drain from a wedging one, above any normal
    # transient.
    # TODO(#57 part 2): re-derive from the host bench run. This ceiling was set
    # against single-packet Bulk (~5 kB/s); #57 now streams real fragmented
    # Bulk (~4.8 MB/s), so a healthy drain edge may sit higher and the ceiling
    # may need to move. Do NOT re-derive from container data -- range/death
    # scenarios need unshare -Urn, unavailable here.
    'recvq_wedge_ceiling_bytes': 100_000,
    # Surviving tiers must keep delivering after the stall at >= this fraction
    # of their pre-stall rate. Observed ~1.0 (no head-of-line impact); 0.5
    # tolerates jitter but trips on a cross-topic collapse (the #29 concern).
    'survivor_min_post_rate_frac': 0.5,
}

SCENARIO_HOLD_S = 8.0
SCENARIO_TIMEOUT_S = SCENARIO_HOLD_S * 2 + 90.0


def _userns_available() -> bool:
    if not shutil.which('unshare'):
        return False
    try:
        return subprocess.run(['unshare', '-Urn', 'true'],
                              capture_output=True, timeout=5).returncode == 0
    except (subprocess.TimeoutExpired, OSError):
        return False


def _ros2_available() -> bool:
    return shutil.which('ros2') is not None and bool(os.environ.get('ROS_DISTRO'))


def _zenoh_available() -> bool:
    try:
        out = subprocess.run(['ros2', 'pkg', 'executables', 'rmw_zenoh_cpp'],
                             capture_output=True, text=True, timeout=10)
        return 'rmw_zenohd' in out.stdout
    except (subprocess.TimeoutExpired, OSError):
        return False


pytestmark = [
    pytest.mark.skipif(
        os.environ.get('UDP_BRIDGE_BENCH_SCENARIOS') != '1',
        reason='Opt-in via UDP_BRIDGE_BENCH_SCENARIOS=1 (run takes ~30s).',
    ),
    pytest.mark.skipif(
        not _userns_available(),
        reason='Unprivileged user namespaces unavailable. '
               'See test/bench/README.md "Prerequisites".',
    ),
    pytest.mark.skipif(
        not _ros2_available(),
        reason='ROS 2 environment not sourced.',
    ),
    pytest.mark.skipif(
        not _zenoh_available(),
        reason='rmw_zenoh_cpp not installed; subscriber_death forces Zenoh '
               '(the wedge path does not exist under DDS).',
    ),
]


@dataclass
class DeathArtifacts:
    recvq_csv: Path
    death_meta: Path
    count_trace_critical: Path
    count_trace_telemetry: Path
    stdout: str


def _parse_marker_lines(stdout: str) -> dict:
    markers = {}
    for line in stdout.splitlines():
        if line.startswith('BENCH_') and '=' in line:
            key, _, val = line.partition('=')
            markers[key] = val.strip()
    return markers


@pytest.fixture(scope='module')
def artifacts() -> DeathArtifacts:
    script = THIS_DIR / 'run_scenario.py'
    proc = subprocess.run(
        [sys.executable, str(script),
         '--scenario', 'subscriber_death',
         '--hold-s', str(SCENARIO_HOLD_S)],
        capture_output=True, text=True, timeout=SCENARIO_TIMEOUT_S,
    )
    markers = _parse_marker_lines(proc.stdout)
    if proc.returncode != 0:
        pytest.fail(
            f'subscriber_death orchestrator exited {proc.returncode}.\n'
            f'stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}'
        )
    required = ['BENCH_RECVQ_OPERATOR', 'BENCH_DEATH_META',
                'BENCH_COUNT_TRACE_CRITICAL', 'BENCH_COUNT_TRACE_TELEMETRY']
    missing = [k for k in required if k not in markers]
    if missing:
        pytest.fail(f'Orchestrator did not emit markers: {missing}. '
                    f'stdout:\n{proc.stdout}')
    return DeathArtifacts(
        recvq_csv=Path(markers['BENCH_RECVQ_OPERATOR']),
        death_meta=Path(markers['BENCH_DEATH_META']),
        count_trace_critical=Path(markers['BENCH_COUNT_TRACE_CRITICAL']),
        count_trace_telemetry=Path(markers['BENCH_COUNT_TRACE_TELEMETRY']),
        stdout=proc.stdout,
    )


def _load_meta(path: Path) -> dict:
    return json.loads(path.read_text())


def _recvq_post_stall(recvq_csv: Path, meta: dict) -> tuple[list[int], int, int]:
    """Operator Recv-Q samples (bytes) at/after the stall instant.

    Returns (post_stall_values, total_rows, usable_rows). `usable_rows` counts
    samples where the tracer actually found the socket — distinguishing a
    healthy quiet queue from a tracer that never bound to the port (a harness
    failure the caller should fail on, not skip). Correlates on the tracer's
    wall-clock `unix_time` column against the stall instant — no cross-process
    monotonic subtraction.
    """
    stall_unix = meta['stall_unix']
    post: list[int] = []
    total = usable = 0
    with recvq_csv.open() as f:
        for r in csv.DictReader(f):
            total += 1
            ut = r.get('unix_time')
            q = r.get('recv_q_bytes')
            if not q:
                continue
            usable += 1
            if ut and float(ut) >= stall_unix:
                post.append(int(q))
    return post, total, usable


def _rate_around(count_csv: Path, split_unix: float) -> tuple[float, float]:
    """(pre_rate, post_rate) msg/s from a (unix_time,count) trace, split at
    split_unix. Slope of cumulative count over each segment."""
    pts = []
    with count_csv.open() as f:
        for r in csv.DictReader(f):
            pts.append((float(r['unix_time']), int(r['count'])))

    def slope(seg):
        if len(seg) < 2:
            return 0.0
        dt = seg[-1][0] - seg[0][0]
        return (seg[-1][1] - seg[0][1]) / dt if dt > 0 else 0.0

    pre = [p for p in pts if p[0] < split_unix]
    post = [p for p in pts if p[0] >= split_unix]
    return slope(pre), slope(post)


def test_no_wedge_recvq_bounded(artifacts):
    """The operator drain must keep up after the consumer stalls: Recv-Q
    stays bounded (no wedge). This is the system-level guard for issue #10 —
    if a change reintroduced a blocking publish on the drain thread under a
    blocking RMW, Recv-Q would climb toward SO_RCVBUF here."""
    meta = _load_meta(artifacts.death_meta)
    post, total, usable = _recvq_post_stall(artifacts.recvq_csv, meta)
    # A trace with rows but zero usable samples means the tracer never bound
    # to the operator UDP socket — a harness failure, not a quiet queue. Fail
    # rather than skip so it can't pass vacuously.
    assert not (total > 0 and usable == 0), (
        f'Recv-Q trace has {total} rows but found the operator socket in none '
        f'of them — the tracer never bound to port 4200 (harness failure).')
    # Zero post-stall samples is also a harness failure, not a quiet result:
    # the tracer is started before the stall and the scenario runs a full
    # observation window afterwards, so an empty post-stall slice means the
    # tracer died or exited early and the guard never observed the event it
    # exists to watch. Skipping here would report that as a pass.
    assert post, (
        f'Recv-Q trace has {usable} usable samples but none at or after the '
        f'stall instant -- the tracer did not survive the observation window '
        f'(harness failure).')
    ceiling = THRESHOLDS['recvq_wedge_ceiling_bytes']
    worst = max(post)
    assert worst < ceiling, (
        f'Operator Recv-Q reached {worst} bytes after the consumer stalled '
        f'(ceiling {ceiling}). The drain thread is backing up — the issue #10 '
        f'wedge. {len(post)} post-stall samples; series={post}'
    )


def test_surviving_tiers_keep_delivering(artifacts):
    """Critical and Telemetry (whose subscribers stay alive) must keep
    delivering after the Bulk consumer stalls — they must not be dragged down
    with it. A collapse here would be the head-of-line failure issue #29 asks
    about; the pre/post rates are also reported for that measurement."""
    meta = _load_meta(artifacts.death_meta)
    split = meta['stall_unix']
    frac = THRESHOLDS['survivor_min_post_rate_frac']
    report = []
    for name, csv_path in (('critical', artifacts.count_trace_critical),
                           ('telemetry', artifacts.count_trace_telemetry)):
        pre, post = _rate_around(csv_path, split)
        report.append(f'{name}: pre={pre:.2f}/s post={post:.2f}/s')
        # post must be a real positive rate, and not collapse vs pre.
        assert post > 0.0, (
            f'{name} delivery stopped after the Bulk consumer stalled '
            f'(pre={pre:.2f}/s, post={post:.2f}/s) — cross-topic stall.'
        )
        if pre > 0.0:
            assert post >= frac * pre, (
                f'{name} delivery collapsed after the Bulk consumer stalled: '
                f'post={post:.2f}/s < {frac} * pre({pre:.2f}/s). Head-of-line '
                f'blocking (issue #29).'
            )
    print('Head-of-line measurement (issue #29): ' + '; '.join(report))


if __name__ == '__main__':
    sys.exit(pytest.main([__file__, '-v', '-s']))
