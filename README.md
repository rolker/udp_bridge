# Bridging Robots over unreliable networks with udp_bridge

The udp_bridge package was developed for use with surface marine robots communicating with operators over unreliable networks.
It allows the selection of topics to forward and to rate limit them. Data is compressed and bandwidth limits are used to keep from saturating connections.

Multiple data paths can be used for redundancy allowing seamless operation using wifi, mesh radios, cellular links, or satellite internet.

## Diagnostic surface

The bridge publishes [`diagnostic_msgs/DiagnosticStatus`](https://docs.ros2.org/latest/api/diagnostic_msgs/msg/DiagnosticStatus.html)
via [`diagnostic_updater`](https://github.com/ros/diagnostics) at ~1 Hz. Two task families are emitted:

- **Per-connection** — `udp_bridge <name>: <remote>: <connection_id>` — surfaces tx/rx byte rates,
  rx staleness, and tx failure/drop conditions for one outbound or inbound socket.
- **Per-remote resend give-ups** — `udp_bridge <name>: <remote>: resend give-ups` (issue #22) — surfaces
  the rate at which the sender has abandoned resending packets (give-up events). High give-up rates
  indicate sustained packet loss on the link beyond what the resend protocol can compensate.

  The aggregated DiagnosticStatus is the default operator surface — it is published whether or not
  any per-event logs are enabled. The per-event log (the `Giving up on resend of packet …` message)
  is emitted at DEBUG severity, which is below the default ROS 2 log threshold (INFO). To capture
  per-event detail in `/rosout` (and any `/rosout` bag recording), lower the node's log level — for
  example, launch with `--ros-args --log-level udp_bridge:=DEBUG` or set
  `RCUTILS_LOGGING_SEVERITY_THRESHOLD=DEBUG`.

### Resend give-up thresholds

Two ROS 2 parameters tune when the per-remote give-up diagnostic transitions between OK / WARN / ERROR levels:

| Parameter | Default | Effect |
|---|---|---|
| `resend_giveup_warn_rate_per_s` | `5.0` | give-up rate ≥ this fires WARN |
| `resend_giveup_error_rate_per_s` | `50.0` | give-up rate ≥ this fires ERROR (takes priority over WARN) |

Defaults calibrated against the 2026-05-19 BizzyBoat deployment (~3.9/s steady storm fires WARN,
~700/s burst fires ERROR). Adjust per deployment via the standard ROS 2 parameter mechanism.

KeyValue pairs published on each task:

- `remote` — remote name
- `give_ups_total` — cumulative give-up count since bridge start (also available on `Remote.msg.resend_giveup_count`)
- `give_up_rate_per_s` — rate over the window since the previous publish
- `window_s` — length of that window (typically ~1 s)
- `warn_threshold_per_s`, `error_threshold_per_s` — the active thresholds (for operator-side dashboards)

The rate is computed as `(current - previous) / elapsed`. On counter reset (sender restart, per the
behavior observed in [#21](https://github.com/rolker/udp_bridge/issues/21)), the published rate is `0`.
