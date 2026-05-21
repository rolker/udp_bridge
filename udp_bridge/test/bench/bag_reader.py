"""Minimal sqlite reader for ros2 bag captures produced by the bench.

Phase 1 (smoke): just counts rows in a per-topic table — enough to
prove pub→bridge→sub flowed end-to-end.

Phase 3 (scenarios): full deserialization of `BridgeInfo` /
`TopicStatistics` for the trajectory invariants. The Phase 3 work will
extend this module rather than introducing a separate reader.
"""

from __future__ import annotations

import sqlite3
from pathlib import Path


def list_topics(db_path: Path) -> list[str]:
    """Return the topic names present in a ros2 bag SQLite store."""
    with sqlite3.connect(str(db_path)) as conn:
        rows = conn.execute('SELECT name FROM topics').fetchall()
    return [r[0] for r in rows]


def count_messages(db_path: Path, topic: str) -> int:
    """Count messages on `topic` in a ros2 bag SQLite store."""
    with sqlite3.connect(str(db_path)) as conn:
        row = conn.execute(
            'SELECT COUNT(*) FROM messages m JOIN topics t ON m.topic_id = t.id WHERE t.name = ?',
            (topic,),
        ).fetchone()
    return int(row[0]) if row else 0


def find_bag_db(bag_dir: Path) -> Path:
    """Return the .db3 file inside a ros2 bag directory."""
    candidates = list(bag_dir.glob('*.db3'))
    if not candidates:
        raise FileNotFoundError(f'No .db3 in {bag_dir}')
    if len(candidates) > 1:
        # Multi-file bag — return the first; callers that need all should
        # iterate themselves.
        return sorted(candidates)[0]
    return candidates[0]
