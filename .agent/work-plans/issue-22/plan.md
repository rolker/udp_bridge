# Plan: 'Giving up on resend' WARN log too loud — rate-limit or surface as DiagnosticStatus

## Issue

https://github.com/rolker/udp_bridge/issues/22

## Context

After [#13](https://github.com/rolker/udp_bridge/pull/13) added resend give-up tracking, the per-event WARN at `remote_node.cpp:329` fires at **~233/min average, hundreds-to-thousands/s burst** on any lossy link (verified against the 2026-05-19 BizzyBoat deployment: 27,992 WARN entries in ~2h). This pollutes `/rosout`, bloats bags by ~5.6 MB per deployment, and desensitizes operators to genuine WARNs. The give-up *behavior* is correct; the *severity* is the problem.

Issue owner's comment lays out the convergent design: demote the per-event log WARN → DEBUG, and publish a per-remote `DiagnosticStatus` with rate metric and threshold-based severity. Code exploration adds a refinement: udp_bridge already runs a `diagnostic_updater::Updater` (added at `udp_bridge.cpp:341`) and registers per-(remote, connection) diagnostic tasks. The new resend-give-up status should integrate with that existing infrastructure rather than introduce a parallel raw publisher.

This is also a worked instance of the workspace pattern documented in memory `feedback_wifi_disconnect_not_an_error` — link-conditions events belong in DiagnosticStatus, not WARN logs.

## Approach

1. **Demote per-event WARN to DEBUG** at `remote_node.cpp:329-334`. One-line change (`RCLCPP_WARN_STREAM` → `RCLCPP_DEBUG_STREAM`). Per-event detail stays available in bags for forensic analysis; live operator log panels stay clean. Independently testable — restores quiet logs even if step 2 spills.

2. **Add per-remote diagnostic task** that publishes give-up rate + cumulative count + threshold-based level. Hook into the existing `diagnostic_updater_` at the per-`RemoteNode` level (parallel to existing per-connection tasks in `udp_bridge.cpp:1495-1507`). New helper `UDPBridge::diagnoseRemoteGiveups(remote_name, stat)` mirrors `diagnoseConnection`'s shape. State needed: a `last_published_count_` and `last_publish_time_` per remote, reset-safe (if `current < last`, publish rate=0 — handles counter reset per observation in [#21](https://github.com/rolker/udp_bridge/issues/21)).

3. **Declare thresholds as ROS 2 parameters**, not `const`. Defaults: `resend_giveup_warn_rate_per_s=5.0`, `resend_giveup_error_rate_per_s=50.0` (calibrated against the 2026-05-19 storm at ~700/s peak / ~3.9/s average — both would have correctly fired ERROR at 50/s). Allows field tuning via YAML without rebuild.

4. **Tests** — extend `test/test_remote_node_resend.cpp` is *not* the right home for the publisher tests (RemoteNode doesn't know about the diagnostic infrastructure — only exposes `resendGiveupCount()`). Add a new `test/test_remote_giveup_diagnostic.cpp` exercising the rate-calculation logic in isolation (counter-reset case, threshold transitions OK↔WARN↔ERROR, zero-rate steady state, multi-remote independence).

## Files to Change

| File | Change |
|------|--------|
| `udp_bridge/src/remote_node.cpp:329-334` | `RCLCPP_WARN_STREAM` → `RCLCPP_DEBUG_STREAM` (1-line severity change; message text unchanged) |
| `udp_bridge/src/udp_bridge.cpp` | Declare 2 new parameters (`resend_giveup_warn_rate_per_s`, `resend_giveup_error_rate_per_s`); register per-remote diagnostic task in the same loop as existing per-connection tasks (~1500); implement `diagnoseRemoteGiveups()` (~50 LOC) |
| `udp_bridge/include/udp_bridge/udp_bridge.h` | Declare `diagnoseRemoteGiveups()`; add `last_giveup_publish_time_` / `last_giveup_count_` per-remote maps; declare parameter members |
| `udp_bridge/test/test_remote_giveup_diagnostic.cpp` | **NEW** — gtest covering rate calculation, counter reset, threshold transitions, multi-remote independence |
| `udp_bridge/CMakeLists.txt` | Register the new test executable |
| `udp_bridge/README.md` | Add a "Diagnostic surface" section (or extend existing) documenting the new `resend give-ups for '<remote>'` diagnostic name + threshold parameters |

## Principles Self-Check

| Principle | Consideration |
|---|---|
| Human control and transparency | DiagnosticStatus is *more* informative than per-event WARN spam — a single aggregator indicator with rate metric beats 200/min of noise. Operators get a tunable surface (params), not a hard-coded behavior. |
| A change includes its consequences | Test coverage added in step 4; README updated; existing `test_remote_node_resend.cpp` continues to verify counter correctness unchanged. |
| Only what's needed | Reuses existing `diagnostic_updater_` infrastructure + existing `resendGiveupCount()` accessor + existing `Remote.msg.resend_giveup_count` field. No new accounting state introduced. ~50 LOC plus tests. |
| Test what breaks | Test scope matches the field-observed failure modes: counter reset across `udp_bridge` restart (per #21), threshold boundaries calibrated against the 2026-05-19 storm, multi-remote independence (storm hit both directions). |
| Improve incrementally | Two atomic commits: step 1 (one-line WARN→DEBUG) is independently mergeable; step 2 (diagnostic publisher + parameters + tests + README) is its own commit. |
| Workspace vs. project separation | Pure project change in `udp_bridge`. No workspace coupling. |

## ADR Compliance

| ADR | Triggered | How addressed |
|---|---|---|
| [0008 — Follow ROS 2 Official Conventions](https://github.com/rolker/ros2_agent_workspace/blob/main/docs/decisions/0008-follow-ros2-official-conventions.md) | Yes | Use `diagnostic_updater::Updater` (already present in this package), not a raw `DiagnosticStatus` publisher — the higher-level API is the standard ROS 2 convention and integrates cleanly with `diagnostic_aggregator`. New parameters declared via `declare_parameter()` per ROS 2 convention. |
| [0001 — Adopt ADRs](https://github.com/rolker/ros2_agent_workspace/blob/main/docs/decisions/0001-adopt-architecture-decision-records.md) | Watch | Single-package severity/diagnostic change — no ADR needed in this repo. The broader workspace pattern ("link-conditions events go through DiagnosticStatus") may deserve an ADR if it cascades to other packages; flag as a follow-up question rather than block. |

## Consequences

| If we change... | Also update... | Included in plan? |
|---|---|---|
| Demote a public-facing WARN | Bag-replay workflows that grep for "Giving up on resend" in `/rosout` | Yes — README will note the demotion and the new diagnostic surface |
| Add new diagnostic task name | Operator-side annunciator (`unh_echoboats_project11`'s `bizzyboat_operator_annunciator`) — if it uses an allowlist, the new name has to be added | **Open question** — annunciator's filter behavior should be checked before merge |
| Add new ROS 2 parameters | Deployment YAMLs that override udp_bridge parameters (BizzyBoat / IzzyBoat launch configs) | No, defaults are sensible; per-deployment override is optional |
| Add new test file | `CMakeLists.txt` test registration | Yes — explicit in Files to Change |

## Open Questions

1. **Annunciator filter behavior**: does the operator-side annunciator allowlist diagnostic task names, or does it accept any `udp_bridge: *` pattern? If allowlisted, this PR needs a paired follow-up commit on `unh_echoboats_project11`. Spot-check before opening this for review.

2. **Threshold defaults**: starting suggestion is 5/s WARN, 50/s ERROR. Field data point (2026-05-19): ~3.9/s average steady storm, ~700/s burst. The averages would have shown WARN, not ERROR — is that the operator-desired indication, or should the WARN threshold be lower (e.g., 1/s) to flag even mild loss? Surfacing for owner input.

3. **Per-remote vs. aggregate diagnostic name**: owner's comment proposed `"udp_bridge: resend give-ups for '<remote_name>'"`. Confirming this matches the existing naming convention in `udp_bridge.cpp:1498` (`"udp_bridge " + name_ + ": " + remote_name + ": " + connection_id`). Slight format inconsistency — should the new task use `"udp_bridge " + name_ + ": " + remote_name + ": resend give-ups"` for uniformity? Recommend yes; will confirm before coding.

## Estimated Scope

Single PR, two atomic commits. Estimated diff: ~80 LOC implementation + ~150 LOC tests + ~20 LOC README. No cross-repo coordination required other than the annunciator allowlist check (Open Question 1).
