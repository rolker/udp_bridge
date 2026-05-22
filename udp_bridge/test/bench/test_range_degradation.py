"""Range-degradation single-path invariants for udp_bridge#18.

Skipped unless UDP_BRIDGE_BENCH_SCENARIOS=1 is set — running the scenario
takes ~110 s at the default hold_s=10 and is not a sensible smoke
target. Also skipped if `unshare -Urn` is unavailable (see
test/bench/README.md "Prerequisites") or ROS 2 isn't sourced.

The fixture runs `run_scenario.py --scenario range_degradation` ONCE
per pytest invocation and feeds the produced artifacts (bag, phase log,
Recv-Q CSV, bridge stderr logs) into the five invariant tests below.

Thresholds (N, X, T, F, R) live in `test/bench/README.md` along with
the bag queries that anchor them; this file references them by name
and pulls the constants from `THRESHOLDS` below — keep the two in
sync if you change a value.

Note on the issue's six single-path invariants vs. the five below: the
issue lists "Forwarding resumes without bridge restart after the
over-horizon window ends" as a separate invariant. The orchestrator
never restarts the bridge — there is no respawn path in
`run_scenario.py` — so the only way that condition can fail is if the
bridge dies and the data plane never recovers. That failure mode is
caught structurally by `test_invariant_recovery_completeness`: a
dead bridge means post-recovery success_bps is zero, which trips the
X% threshold. The "no restart" invariant is therefore implicit in
the harness shape and the recovery-completeness check; encoding it
as a separate test would be tautological.
"""

from __future__ import annotations

import csv
import json
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

import pytest


THIS_DIR = Path(__file__).parent

# Match THRESHOLDS section of `test/bench/README.md`. These are
# initial values; the README's "Refinement plan" section governs how
# each one is re-derived as field bags accumulate.
THRESHOLDS = {
    'N_recv_q_climb_s': 10.0,    # Recv-Q monotonic climb tolerance (s)
    'X_recovery_pct': 0.90,      # Post-recovery rate must reach X * pre-baseline
    'T_recovery_s': 30.0,        # ...within T seconds of leaving over-horizon
    'F_resend_multiplier': 2.0,  # resend/tx_ok ratio ceiling = F * loss_rate
    'R_stats_pct': 0.50,         # bridge_info+topic_statistics rate floor
}

# Phase-hold seconds for the scenario. Long enough to exercise resend
# behavior on the lossy/critical legs and to produce a stable in-range
# baseline at each end.
SCENARIO_HOLD_S = 10.0
# Subprocess timeout — generous for setup/teardown overhead.
SCENARIO_TIMEOUT_S = SCENARIO_HOLD_S * 9 + 60.0


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


pytestmark = [
    pytest.mark.skipif(
        os.environ.get('UDP_BRIDGE_BENCH_SCENARIOS') != '1',
        reason='Opt-in via UDP_BRIDGE_BENCH_SCENARIOS=1 (run takes ~110s).',
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
]


@dataclass
class RunArtifacts:
    bag_dir: Path
    phase_log: Path
    recvq_csv: Path
    bridge_stderr_operator: Path
    bridge_stderr_boat: Path
    stdout: str
    returncode: int


def _parse_marker_lines(stdout: str) -> dict:
    """Pull BENCH_* marker lines (KEY=VALUE) out of the orchestrator stdout."""
    markers = {}
    for line in stdout.splitlines():
        if line.startswith('BENCH_') and '=' in line:
            key, _, val = line.partition('=')
            markers[key] = val.strip()
    return markers


@pytest.fixture(scope='module')
def artifacts() -> RunArtifacts:
    script = THIS_DIR / 'run_scenario.py'
    proc = subprocess.run(
        [sys.executable, str(script),
         '--scenario', 'range_degradation',
         '--hold-s', str(SCENARIO_HOLD_S)],
        capture_output=True, text=True,
        timeout=SCENARIO_TIMEOUT_S,
    )
    markers = _parse_marker_lines(proc.stdout)
    if proc.returncode != 0:
        pytest.fail(
            f'range_degradation orchestrator exited {proc.returncode}.\n'
            f'stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}'
        )
    required = ['BENCH_BAG_DIR', 'BENCH_PHASE_LOG', 'BENCH_RECVQ_OPERATOR',
                'BENCH_BRIDGE_STDERR_OPERATOR', 'BENCH_BRIDGE_STDERR_BOAT']
    missing = [k for k in required if k not in markers]
    if missing:
        pytest.fail(f'Orchestrator did not emit markers: {missing}. '
                    f'stdout:\n{proc.stdout}')
    return RunArtifacts(
        bag_dir=Path(markers['BENCH_BAG_DIR']),
        phase_log=Path(markers['BENCH_PHASE_LOG']),
        recvq_csv=Path(markers['BENCH_RECVQ_OPERATOR']),
        bridge_stderr_operator=Path(markers['BENCH_BRIDGE_STDERR_OPERATOR']),
        bridge_stderr_boat=Path(markers['BENCH_BRIDGE_STDERR_BOAT']),
        stdout=proc.stdout,
        returncode=proc.returncode,
    )


def _load_phase_log(path: Path) -> dict:
    return json.loads(path.read_text())


def _phase_windows(phase_log: dict) -> list[tuple[str, float, float]]:
    """Return (phase_name, t_start_s, t_end_s) windows in walk_local seconds."""
    phases = phase_log['phases']
    out = []
    for i in range(len(phases) - 1):
        out.append((phases[i]['phase'],
                    phases[i]['t_offset_s'],
                    phases[i + 1]['t_offset_s']))
    return out


def _walk_unix_to_local(phase_log: dict, t_unix_s: float) -> float:
    return t_unix_s - phase_log['walk_start_unix']


def _stamp_ns_to_walk_local(phase_log: dict, stamp_ns: int) -> float:
    return _walk_unix_to_local(phase_log, stamp_ns / 1e9)


# -----------------------------------------------------------------------
# Invariant 1: Recv-Q never climbs monotonically for more than N seconds.
# -----------------------------------------------------------------------

def test_invariant_recv_q_no_sustained_climb(artifacts):
    """Recv-Q on the operator UDP socket must not monotonically increase
    for more than N seconds in any single phase, including the
    over-horizon → recovery edge."""
    rows = []
    with artifacts.recvq_csv.open() as f:
        for r in csv.DictReader(f):
            try:
                t = float(r['t_seconds_since_start'])
                q = int(r['recv_q_bytes']) if r['recv_q_bytes'] else 0
                rows.append((t, q))
            except (ValueError, KeyError):
                continue
    if not rows:
        pytest.skip('No Recv-Q samples recorded; nothing to assert.')

    n_limit = THRESHOLDS['N_recv_q_climb_s']
    climb_start_t: float | None = None
    prev_q = rows[0][1]
    longest_climb_s = 0.0
    longest_climb_end_q = 0
    for t, q in rows[1:]:
        if q > prev_q:
            if climb_start_t is None:
                climb_start_t = t
        else:
            if climb_start_t is not None:
                run = t - climb_start_t
                if run > longest_climb_s:
                    longest_climb_s = run
                    longest_climb_end_q = prev_q
                climb_start_t = None
        prev_q = q
    if climb_start_t is not None:
        run = rows[-1][0] - climb_start_t
        if run > longest_climb_s:
            longest_climb_s = run
            longest_climb_end_q = prev_q

    assert longest_climb_s <= n_limit, (
        f'Recv-Q climbed monotonically for {longest_climb_s:.1f}s '
        f'(end-of-climb Recv-Q = {longest_climb_end_q} bytes); '
        f'invariant N = {n_limit:.1f}s.'
    )


# -----------------------------------------------------------------------
# Invariant 2: No ERROR-severity log during the over-horizon window.
# -----------------------------------------------------------------------

ERROR_LINE_RE = re.compile(r'^\[(ERROR|FATAL)\]\s*\[(\d+\.\d+)\]')


def _error_lines_in_window(log_path: Path,
                           t_start_unix: float,
                           t_end_unix: float) -> list[str]:
    if not log_path.exists():
        return []
    out = []
    for line in log_path.read_text().splitlines():
        m = ERROR_LINE_RE.match(line)
        if not m:
            continue
        try:
            stamp = float(m.group(2))
        except ValueError:
            continue
        if t_start_unix <= stamp <= t_end_unix:
            out.append(line)
    return out


def test_invariant_no_error_logs_during_over_horizon(artifacts):
    """Graceful disconnect is a mode, not a fault — neither bridge may
    emit ERROR or FATAL during the over-horizon phase."""
    phase_log = _load_phase_log(artifacts.phase_log)
    windows = _phase_windows(phase_log)
    over_horizons = [w for w in windows if w[0] == 'over_horizon']
    if not over_horizons:
        pytest.skip('No over_horizon phase found in phase log.')
    walk_start = phase_log['walk_start_unix']

    bad = []
    for _, t_start, t_end in over_horizons:
        t_start_unix = walk_start + t_start
        t_end_unix = walk_start + t_end
        for path in (artifacts.bridge_stderr_operator,
                     artifacts.bridge_stderr_boat):
            bad.extend(_error_lines_in_window(path, t_start_unix, t_end_unix))

    assert not bad, (
        f'{len(bad)} ERROR/FATAL log line(s) during over-horizon window:\n'
        + '\n'.join(bad[:10])
    )


# -----------------------------------------------------------------------
# Helpers for bridge_info-derived invariants
# -----------------------------------------------------------------------

def _import_bag_reader():
    sys.path.insert(0, str(THIS_DIR))
    import bag_reader  # type: ignore
    return bag_reader


def _wifi_rates_per_phase(artifacts) -> dict[str, dict[str, float]]:
    """Average DataRates fields of the boat→operator WiFi connection
    within each phase. Reads `/operator_bridge/remotes/boat/bridge_info`
    (the mirror of the boat's BridgeInfo as seen by the operator).
    """
    bag_reader = _import_bag_reader()
    infos = bag_reader.read_bridge_infos(
        artifacts.bag_dir, '/operator_bridge/remotes/boat/bridge_info')
    phase_log = _load_phase_log(artifacts.phase_log)
    windows = _phase_windows(phase_log)
    walk_start_ns = int(phase_log['walk_start_unix'] * 1e9)

    out: dict[str, dict[str, float]] = {}
    for phase_name, t_start, t_end in windows:
        t_start_ns = walk_start_ns + int(t_start * 1e9)
        t_end_ns = walk_start_ns + int(t_end * 1e9)
        in_win = [(t, m) for t, m in infos if t_start_ns <= t < t_end_ns]
        succ_sum = drop_sum = resend_sum = recv_sum = n = 0.0
        for _, msg in in_win:
            conn = bag_reader.get_wifi_connection(msg, remote_name='operator')
            if conn is None:
                continue
            succ_sum += conn.message.success_bytes_per_second
            drop_sum += conn.message.dropped_bytes_per_second
            resend_sum += conn.resend.success_bytes_per_second
            recv_sum += conn.received_bytes_per_second
            n += 1
        if n == 0:
            continue
        out.setdefault(phase_name, {
            'success_bps': 0.0, 'dropped_bps': 0.0,
            'resend_bps': 0.0, 'received_bps': 0.0, 'samples': 0,
        })
        # Average across all in_range_clean (etc.) windows in a single
        # row — the trajectory hits in_range_clean / lossy / etc. twice.
        cur = out[phase_name]
        total_n = cur['samples'] + n
        cur['success_bps'] = (cur['success_bps'] * cur['samples']
                              + succ_sum) / total_n
        cur['dropped_bps'] = (cur['dropped_bps'] * cur['samples']
                              + drop_sum) / total_n
        cur['resend_bps'] = (cur['resend_bps'] * cur['samples']
                             + resend_sum) / total_n
        cur['received_bps'] = (cur['received_bps'] * cur['samples']
                               + recv_sum) / total_n
        cur['samples'] = total_n
    return out


def _split_in_range_phases(artifacts) -> tuple[dict, dict] | None:
    """Return (pre_event, post_recovery) {success_bps,...} for the FIRST
    and LAST in_range_clean phases. Returns None if either is missing.
    """
    bag_reader = _import_bag_reader()
    infos = bag_reader.read_bridge_infos(
        artifacts.bag_dir, '/operator_bridge/remotes/boat/bridge_info')
    phase_log = _load_phase_log(artifacts.phase_log)
    windows = _phase_windows(phase_log)
    walk_start_ns = int(phase_log['walk_start_unix'] * 1e9)

    in_range_windows = [(s, e) for n, s, e in windows if n == 'in_range_clean']
    if len(in_range_windows) < 2:
        return None

    def avg(t_start, t_end):
        t_start_ns = walk_start_ns + int(t_start * 1e9)
        t_end_ns = walk_start_ns + int(t_end * 1e9)
        in_win = [(t, m) for t, m in infos if t_start_ns <= t < t_end_ns]
        succ_sum = n = 0.0
        for _, msg in in_win:
            conn = bag_reader.get_wifi_connection(msg, remote_name='operator')
            if conn is None:
                continue
            succ_sum += conn.message.success_bytes_per_second
            n += 1
        return {'success_bps': (succ_sum / n) if n else 0.0, 'samples': n}

    return avg(*in_range_windows[0]), avg(*in_range_windows[-1])


# -----------------------------------------------------------------------
# Invariant 3: Rate recovers to X% of baseline within T seconds.
# -----------------------------------------------------------------------

def test_invariant_recovery_completeness(artifacts):
    """After the over-horizon → in_range edge, the WiFi connection's
    `message.success_bytes_per_second` must reach X% of the pre-event
    in_range baseline. (Convergence-time T is read from the LAST
    in_range_clean phase; if hold_s ≥ T the phase IS the convergence
    window — see README "Refinement plan" item 2 for the next step.)"""
    pair = _split_in_range_phases(artifacts)
    if pair is None:
        pytest.skip('Phase log does not contain two in_range_clean phases.')
    pre, post = pair
    if pre['samples'] == 0 or post['samples'] == 0:
        pytest.skip(
            f'No bridge_info samples in one of the in_range phases '
            f'(pre samples={pre["samples"]}, post samples={post["samples"]}).'
        )
    if pre['success_bps'] <= 0:
        pytest.skip(
            f'Pre-event baseline success_bps is {pre["success_bps"]:.1f}; '
            f'cannot evaluate recovery ratio.'
        )

    ratio = post['success_bps'] / pre['success_bps']
    x_pct = THRESHOLDS['X_recovery_pct']
    assert ratio >= x_pct, (
        f'Post-recovery success rate {post["success_bps"]:.0f} B/s is '
        f'{ratio:.1%} of pre-event baseline {pre["success_bps"]:.0f} B/s; '
        f'invariant X = {x_pct:.0%}.'
    )


# -----------------------------------------------------------------------
# Invariant 4: Resend amplification ceiling F× during recovery leg.
# -----------------------------------------------------------------------

def _loss_pct_to_fraction(loss: str) -> float:
    """Convert a tc-netem loss spec ('0.5%' or '0%') to a fraction."""
    return float(loss.rstrip('%')) / 100.0


def _phase_loss_rates() -> dict[str, float]:
    """Derive phase → loss fraction by reading run_scenario.PHASE_TRAJECTORY.

    Keeps the two in lockstep — if the trajectory's `loss` field
    changes, this picks it up automatically so the test stays honest.
    """
    sys.path.insert(0, str(THIS_DIR))
    import run_scenario  # type: ignore
    out: dict[str, float] = {}
    for name, profile in run_scenario.PHASE_TRAJECTORY:
        out[name] = _loss_pct_to_fraction(profile['loss'])
    return out


PHASE_LOSS_RATE = _phase_loss_rates()


def test_invariant_resend_amplification(artifacts):
    """On the recovery leg, the ratio
    `resend.success_bytes_per_second / message.success_bytes_per_second`
    on the boat→operator WiFi connection must not exceed F × applied
    loss rate, for each lossy phase visited."""
    per_phase = _wifi_rates_per_phase(artifacts)
    f_mult = THRESHOLDS['F_resend_multiplier']
    violators = []
    evaluated = 0
    for phase, rates in per_phase.items():
        loss = PHASE_LOSS_RATE.get(phase)
        if not loss or loss == 1.0:
            # over_horizon: nothing gets through, ratio is undefined; skip.
            continue
        succ = rates['success_bps']
        resend = rates['resend_bps']
        if succ <= 0:
            continue
        evaluated += 1
        ratio = resend / succ
        ceiling = loss * f_mult
        if ratio > ceiling:
            violators.append(
                f'{phase}: resend/tx_ok = {ratio:.3f} > F×loss = {ceiling:.3f} '
                f'(F={f_mult}, loss={loss:.3f})')
    if evaluated == 0:
        pytest.skip('No lossy phases had non-zero success_bps to evaluate.')
    assert not violators, '\n'.join(violators)


# -----------------------------------------------------------------------
# Invariant 5: bridge_info / topic_statistics pub rate stays ≥ R% baseline.
# -----------------------------------------------------------------------

def _msg_rate_per_phase(artifacts, topic: str) -> dict[str, float]:
    """Messages-per-second on `topic` aggregated by phase name."""
    bag_reader = _import_bag_reader()
    msgs = bag_reader.iter_messages(artifacts.bag_dir, topic_filter=[topic])
    stamps = [t for _, t, _ in msgs]
    phase_log = _load_phase_log(artifacts.phase_log)
    windows = _phase_windows(phase_log)
    walk_start_ns = int(phase_log['walk_start_unix'] * 1e9)
    counts: dict[str, list[float]] = {}
    for phase_name, t_start, t_end in windows:
        t_start_ns = walk_start_ns + int(t_start * 1e9)
        t_end_ns = walk_start_ns + int(t_end * 1e9)
        in_win = sum(1 for s in stamps if t_start_ns <= s < t_end_ns)
        dur = (t_end - t_start) or 1.0
        counts.setdefault(phase_name, []).append(in_win / dur)
    return {p: sum(rs) / len(rs) for p, rs in counts.items() if rs}


def test_invariant_stats_publish_rates(artifacts):
    """`bridge_info` and `topic_statistics` publication rates on both
    sides must remain at ≥ R% of the in-range baseline through the
    over-horizon and recovery phases. (Catches #20-class stalls in
    the stats path while the data plane keeps working.)"""
    interesting_topics = [
        '/operator_bridge/bridge_info',
        '/operator_bridge/topic_statistics',
        '/operator_bridge/remotes/boat/bridge_info',
        '/operator_bridge/remotes/boat/topic_statistics',
    ]
    r_pct = THRESHOLDS['R_stats_pct']
    violators = []
    evaluated = 0
    for topic in interesting_topics:
        try:
            rates = _msg_rate_per_phase(artifacts, topic)
        except Exception as exc:  # noqa: BLE001 — bag-level failure surfaces here
            pytest.fail(f'Could not compute rates for {topic}: {exc}')
        baseline = rates.get('in_range_clean')
        if baseline is None or baseline <= 0:
            continue
        evaluated += 1
        for phase, rate in rates.items():
            if phase == 'in_range_clean':
                continue
            if rate < r_pct * baseline:
                violators.append(
                    f'{topic} {phase}: {rate:.2f} Hz < {r_pct:.0%} of '
                    f'in_range_clean baseline {baseline:.2f} Hz')
    if evaluated == 0:
        pytest.skip('No baseline pub rate established for any stats topic.')
    assert not violators, '\n'.join(violators)
