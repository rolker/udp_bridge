"""Smoke test for the bench harness.

Verifies one Critical-tier message flows pub → boat_bridge → operator_bridge
→ sub through the netns + tc topology, on a clean baseline.

Skips when:
  - unshare -Urn is unavailable (host doesn't have the AppArmor
    prerequisite applied; see test/bench/README.md "Prerequisites").
  - ROS 2 environment is not sourced (no ROS_DISTRO set).
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest


THIS_DIR = Path(__file__).parent


def _userns_available() -> bool:
    if not shutil.which('unshare'):
        return False
    try:
        result = subprocess.run(
            ['unshare', '-Urn', 'true'],
            capture_output=True,
            text=True,
            timeout=5,
        )
        return result.returncode == 0
    except (subprocess.TimeoutExpired, OSError):
        return False


def _ros2_available() -> bool:
    return shutil.which('ros2') is not None and bool(os.environ.get('ROS_DISTRO'))


pytestmark = [
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


def test_smoke_pub_reaches_sub():
    """Run run_scenario.py --scenario smoke; expect sub_count > 0."""
    script = THIS_DIR / 'run_scenario.py'
    proc = subprocess.run(
        [sys.executable, str(script), '--scenario', 'smoke',
         '--duration-s', '10'],
        capture_output=True,
        text=True,
        timeout=120,
    )

    counts = {}
    for line in proc.stdout.splitlines():
        if line.startswith('BENCH_RESULT_'):
            key, _, val = line.partition('=')
            try:
                counts[key] = int(val)
            except ValueError:
                continue

    assert proc.returncode == 0, (
        f'Orchestrator exited {proc.returncode}.\n'
        f'stdout:\n{proc.stdout}\n'
        f'stderr:\n{proc.stderr}'
    )
    sub_count = counts.get('BENCH_RESULT_SUB_COUNT', 0)
    assert sub_count > 0, (
        f'Subscriber received no messages. counts={counts}\n'
        f'stdout:\n{proc.stdout}\n'
        f'stderr:\n{proc.stderr}'
    )
