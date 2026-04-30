# udp_bridge — manual-run integration tests

This directory holds **manual-run** integration tests for `udp_bridge`. They
are deliberately not wired into CI:

- The mininet-based scripts need root (sudo + linux netns) and a Linux
  kernel with `mininet` installed; flaky in containerized CI runners.
- The wedge-reproduction scenario is exploratory — outcomes are documented
  on the corresponding PR / deployment debrief, not gated by a pass/fail
  check.

## Status

The original `multilink.py` + `*.bash` setup in this directory was written
for **ROS 1** (`roscore`, `rosmon`, `ROS_MASTER_URI`, `.launch` v1 format).
It does **not** run as-is under ROS 2 Jazzy. The package itself was ported
to ROS 2 in earlier work; the mininet scaffold was not.

Concretely broken:

- `robot_setup.bash` / `operator_setup.bash` set `ROS_MASTER_URI`, which has
  no equivalent in ROS 2.
- `robot_launch.bash` / `operator_launch.bash` invoke `roscore` and
  `rosrun rosmon rosmon`, neither of which exists in ROS 2.
- `test_mininet_multilink_*.launch` reference the ROS 1 launch XML schema
  with `<node type="...">` (ROS 2 uses `executable=`).

A ROS-2 port is feasible (drop `ROS_MASTER_URI`, replace `roscore` with
`ros2 daemon start`, replace `rosmon` with `ros2 launch`, port `.launch`
files to Python) but is its own piece of work; not in scope for the
issue #11 PR. Tracking a port if/when needed should happen in a follow-up
issue rather than blocking #11 on it.

## What works today (scripts in this directory)

### `recv_q_trace.py` — Recv-Q telemetry helper

Polls `/proc/net/udp` for the bridge's UDP socket and writes a CSV trace of
`recv_q` (kernel receive-queue depth, bytes) over time. Useful for
documenting the wedge symptom from
[issue #10](https://github.com/rolker/udp_bridge/issues/10) (`Recv-Q` backs
up while the bridge process appears alive).

```bash
# In another terminal: ros2 launch udp_bridge udp_bridge_launch.py
./recv_q_trace.py --port 4200 --interval 0.5 --output /tmp/recv_q.csv
```

CSV columns: `t_seconds_since_start`, `recv_q_bytes`, `send_q_bytes`,
`local_addr`. One line per poll interval. Press `Ctrl-C` to finish.

This is independently useful for field debugging (no mininet required) and
documents whatever recv-queue behavior the deployed bridge actually shows.

### `subscriber_death_scenario.bash` — local two-terminal repro of #10

Runs two `udp_bridge` instances on `127.0.0.1` with different ports, has
one forward a high-rate topic to the other, then kills the operator-side
subscriber mid-stream. Asserts:

- The bridge's `Recv-Q` stays bounded (via `recv_q_trace.py`).
- The bridge's `bridge_info` heartbeat keeps publishing on the *other*
  topics it serves.
- The bridge process does not need a SIGTERM + respawn to recover when a
  new subscriber on the same topic comes back online.

This is the local equivalent of the mininet wedge reproduction — runs in
two terminals, no netns required. It does NOT exercise lossy-link
behavior; for that, the original mininet path is the right tool once the
ROS-2 port is done.

## Mininet (legacy / not currently working)

The remaining files in this directory (`multilink.py`, `*.bash`,
`*.launch`) are kept as a starting point for a future ROS-2 port. They
will not run as-is under ROS 2 Jazzy. If you start that port, please open
an issue and link it from here.
