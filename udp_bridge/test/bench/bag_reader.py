"""In-tree ros2 bag reader for the bench harness.

rosbag2_py-backed iterators (`iter_messages`, `read_bridge_infos`,
`read_topic_statistics_arrays`) used by `test_range_degradation.py` for
the Phase 3 single-path invariants. These work with whichever storage
backend the bag uses (mcap by default in Jazzy, sqlite3 if the recorder
was told to use it).

An earlier revision also carried direct-SQLite shims (`list_topics`,
`count_messages`, `find_bag_db`) documented as serving the Phase 1
smoke. Nothing ever called them, and being sqlite3-only they were wrong
against Jazzy's mcap default anyway, so they were removed rather than
left as a maintenance trap. Use the rosbag2_py readers below, which are
storage-agnostic.

The Phase 3 readers deserialize via `rclpy.serialization.deserialize_message`
+ `rosidl_runtime_py.utilities.get_message`, so the message types come
from the bag's own type catalog rather than a hardcoded import — which
means a bag captured under a future schema-evolved BridgeInfo still
reads here.
"""

from __future__ import annotations

from pathlib import Path
from typing import Iterator, Tuple


def _detect_storage_id(bag_dir: Path) -> str:
    """Return 'mcap' or 'sqlite3' based on what files are in `bag_dir`."""
    if any(bag_dir.glob('*.mcap')):
        return 'mcap'
    if any(bag_dir.glob('*.db3')):
        return 'sqlite3'
    raise FileNotFoundError(f'No mcap/db3 storage file in {bag_dir}')


def iter_messages(bag_dir: Path, topic_filter: list[str] | None = None
                  ) -> Iterator[Tuple[str, int, object]]:
    """Yield (topic_name, stamp_ns, deserialized_message) tuples.

    `stamp_ns` is the bag's receive timestamp (nanoseconds since epoch),
    NOT the message's own header stamp. For Phase 3 invariants we care
    about wall-clock ordering relative to the phase log, so the bag
    stamp is what we want.

    `topic_filter`, if given, restricts iteration to those topics — the
    underlying rosbag2 SequentialReader supports a storage_filter so this
    is more efficient than client-side filtering.
    """
    # rosbag2_py and rclpy imports are deferred so this module stays
    # importable without a ROS 2 env sourced -- pytest collects the bench
    # tests before any scenario runs, and `_detect_storage_id` is pure
    # pathlib. Only the readers below actually need rosbag2.
    import rosbag2_py
    from rclpy.serialization import deserialize_message
    from rosidl_runtime_py.utilities import get_message

    storage_options = rosbag2_py.StorageOptions(
        uri=str(bag_dir),
        storage_id=_detect_storage_id(bag_dir),
    )
    converter_options = rosbag2_py.ConverterOptions(
        input_serialization_format='cdr',
        output_serialization_format='cdr',
    )
    reader = rosbag2_py.SequentialReader()
    reader.open(storage_options, converter_options)

    if topic_filter:
        reader.set_filter(rosbag2_py.StorageFilter(topics=topic_filter))

    type_by_topic = {t.name: t.type for t in reader.get_all_topics_and_types()}
    type_cache: dict[str, object] = {}

    while reader.has_next():
        topic_name, raw_data, stamp_ns = reader.read_next()
        type_str = type_by_topic.get(topic_name)
        if type_str is None:
            continue
        msg_type = type_cache.get(type_str)
        if msg_type is None:
            msg_type = get_message(type_str)
            type_cache[type_str] = msg_type
        yield topic_name, int(stamp_ns), deserialize_message(raw_data, msg_type)


def read_bridge_infos(bag_dir: Path, topic: str
                      ) -> list[Tuple[int, object]]:
    """Return all BridgeInfo messages on `topic` as (stamp_ns, msg) tuples."""
    return [
        (stamp, msg)
        for _, stamp, msg in iter_messages(bag_dir, topic_filter=[topic])
    ]


def read_topic_statistics_arrays(bag_dir: Path, topic: str
                                 ) -> list[Tuple[int, object]]:
    """Return all TopicStatisticsArray messages on `topic`."""
    return [
        (stamp, msg)
        for _, stamp, msg in iter_messages(bag_dir, topic_filter=[topic])
    ]


def messages_in_window(messages: list[Tuple[int, object]],
                       t_start_ns: int, t_end_ns: int
                       ) -> list[Tuple[int, object]]:
    """Filter (stamp_ns, msg) tuples to those with t_start_ns <= stamp < t_end_ns."""
    return [(t, m) for t, m in messages if t_start_ns <= t < t_end_ns]


def get_connection(bridge_info, connection_id: str, remote_name: str = 'boat'):
    """Pick a named connection out of a BridgeInfo's remote-list.

    Returns the `RemoteConnection` with `connection_id` on the named
    remote, or None if not found. Lets multi-link tests reach the
    DataRates of cell / starlink / wifi without hardcoding indices.
    """
    for remote in bridge_info.remotes:
        if remote.name != remote_name:
            continue
        for conn in remote.connections:
            if conn.connection_id == connection_id:
                return conn
    return None


def get_wifi_connection(bridge_info, remote_name: str = 'boat'):
    """Back-compat shim for the Phase 3 invariants: the WiFi connection."""
    return get_connection(bridge_info, 'wifi', remote_name)


def topic_connection_ids(bridge_info, topic: str, remote_name: str = 'boat'
                         ) -> set:
    """Return the set of connection_ids carrying `topic` to `remote_name`,
    as advertised in the BridgeInfo `topics` section. Empty set if the
    topic isn't advertised. Used by the topic-list confinement invariant.
    """
    out = set()
    for ti in bridge_info.topics:
        if ti.topic != topic:
            continue
        for trd in ti.remotes:
            if trd.remote != remote_name:
                continue
            for trc in trd.connections:
                out.add(trc.connection_id)
    return out
