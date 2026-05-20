# Plan: 'Giving up on resend' WARN log too loud — rate-limit or surface as DiagnosticStatus

## Issue

https://github.com/rolker/udp_bridge/issues/22

## Context

After [#13](https://github.com/rolker/udp_bridge/pull/13) added resend give-up tracking, the per-event WARN at `remote_node.cpp:329` fires at **~233/min average, hundreds-to-thousands/s burst** on any lossy link (verified against the 2026-05-19 BizzyBoat deployment: 27,992 WARN entries in ~2h). This pollutes `/rosout`, bloats bags by ~5.6 MB per deployment, and desensitizes operators to genuine WARNs. The give-up *behavior* is correct; the *severity* is the problem.

Issue owner's comment lays out the convergent design: demote the per-event log WARN → DEBUG, and publish a per-remote `DiagnosticStatus` with rate metric and threshold-based severity. Code exploration adds a refinement: udp_bridge already runs a `diagnostic_updater::Updater` (added at `udp_bridge.cpp:341`) and registers per-(remote, connection) diagnostic tasks. The new resend-give-up status should integrate with that existing infrastructure rather than introduce a parallel raw publisher.

This is also a worked instance of the workspace pattern documented in memory `feedback_wifi_disconnect_not_an_error` — link-conditions events belong in DiagnosticStatus, not WARN logs.

## Approach

1. **Demote per-event WARN to DEBUG** at `remote_node.cpp:329-334`. One-line change (`RCLCPP_WARN_STREAM` → `RCLCPP_DEBUG_STREAM`). Per-event detail stays available in bags for forensic analysis; live operator log panels stay clean. Independently testable — restores quiet logs even if step 2 spills.

2. **Add per-remote diagnostic task** that publishes give-up rate + cumulative count + threshold-based level. Hook into the existing `diagnostic_updater_` at the per-`RemoteNode` level (parallel to existing per-connection tasks in `udp_bridge.cpp:1495-1507`). New helper `UDPBridge::diagnoseRemoteGiveups(remote_name, stat)` mirrors `diagnoseConnection`'s shape. Diagnostic task name follows the existing convention: `"udp_bridge " + name_ + ": " + remote_name + ": resend give-ups"` (mirrors the per-connection format at `udp_bridge.cpp:1498` — this is a deliberate divergence from the owner's comment wording, which proposed `"udp_bridge: resend give-ups for '<remote>'"`; the convention-conforming form is preferred for consistency with the existing aggregator output).

   **State**: `last_giveup_count_` and `last_giveup_publish_time_` per-remote maps live on `UDPBridge` and are guarded by the **existing `remote_nodes_mutex_`** (extended scope — short reads inside the diagnostic callback, no new lock introduced). Reset-safe: if `current_count < last_count`, publish rate=0 (handles counter reset per [#21](https://github.com/rolker/udp_bridge/issues/21) observation).

   **Lifecycle**: cleared in `on_cleanup` alongside the existing `diagnostic_task_names_` clear at `udp_bridge.cpp:364`. `diagnostic_updater::Updater` has no per-task `remove()` API, so re-registration of the same task name after activate→deactivate→activate must be guarded the same way `diagnostic_task_names_` already gates re-adds (line 1499). The diagnostic callback's "remote not registered" branch mirrors `diagnoseConnection`'s `STALE` return (line 1520).

3. **Declare thresholds as ROS 2 parameters**, not `const`. Defaults: `resend_giveup_warn_rate_per_s=5.0`, `resend_giveup_error_rate_per_s=50.0` — confirmed with owner. Calibrated against the 2026-05-19 storm (~700/s peak / ~3.9/s average steady): the average correctly fires WARN, the burst correctly fires ERROR. Lower-WARN-threshold question (Open Question 2 below) deferred — start with 5/50 and revisit if operators report missing mild-loss signal. Parameters allow field tuning via YAML without rebuild.

4. **Tests** — extend `test/test_remote_node_resend.cpp` is *not* the right home for the publisher tests (RemoteNode doesn't know about the diagnostic infrastructure — only exposes `resendGiveupCount()`).

   **Test strategy**: extract the rate/threshold logic into a **static free function** so it can be tested without bringing up a `UDPBridge` instance. Proposed signature:

   ```cpp
   struct GiveupDiagnostic {
     uint8_t level;        // diagnostic_msgs::DiagnosticStatus OK/WARN/ERROR
     double rate_per_s;
     uint32_t total;
   };

   GiveupDiagnostic computeGiveupDiagnostic(
       uint32_t prev_count,
       uint32_t current_count,
       double elapsed_s,
       double warn_thresh,
       double error_thresh);
   ```

   The free function lives in a small new header (`udp_bridge/include/udp_bridge/giveup_diagnostic.h`) and is consumed by `UDPBridge::diagnoseRemoteGiveups()` as a thin wrapper. Test file `test/test_giveup_diagnostic.cpp` exercises:

   - Steady-state rate calculation
   - Counter reset (`current < prev` → rate=0, level=OK)
   - Threshold transitions OK↔WARN↔ERROR at the configured cutoffs
   - **First-call behavior** (no prior call → caller passes `prev_count=0` and the function must return rate=0 without dividing by `elapsed_s=0`)
   - **Zero-elapsed-time edge case** (`elapsed_s == 0` → must not divide by zero; return rate=0)
   - Multi-remote independence is covered by the wrapper-side state isolation (separate map entries), not by this helper — verified at integration level via a follow-up sanity-check rather than a dedicated unit test.

   Determinism: the free function takes `elapsed_s` as a parameter, so the test doesn't need a clock at all — the time-dependence is moved out of the test fixture and into the caller. The wrapper `diagnoseRemoteGiveups()` uses `this->now()` (controllable via `use_sim_time` if a future integration test wants it).

## Files to Change

| File | Change |
|------|--------|
| `udp_bridge/src/remote_node.cpp:329-334` | `RCLCPP_WARN_STREAM` → `RCLCPP_DEBUG_STREAM` (1-line severity change; message text unchanged) |
| `udp_bridge/src/udp_bridge.cpp` | Declare 2 new parameters (`resend_giveup_warn_rate_per_s`, `resend_giveup_error_rate_per_s`); register per-remote diagnostic task in the same loop as existing per-connection tasks (~1500); implement `diagnoseRemoteGiveups()` (~30 LOC — most logic moves to the free function); clear new maps in `on_cleanup` alongside existing `diagnostic_task_names_` reset (~364) |
| `udp_bridge/include/udp_bridge/udp_bridge.h` | Declare `diagnoseRemoteGiveups()`; add `last_giveup_count_` / `last_giveup_publish_time_` per-remote maps guarded by `remote_nodes_mutex_`; declare parameter members |
| `udp_bridge/include/udp_bridge/giveup_diagnostic.h` | **NEW** — header for `computeGiveupDiagnostic()` free function (~25 LOC including the `GiveupDiagnostic` struct) |
| `udp_bridge/test/test_giveup_diagnostic.cpp` | **NEW** — gtest covering steady-state rate, counter reset, threshold transitions, first-call (`prev_count=0`), zero-elapsed-time edge case |
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
| Enforcement over documentation | The "link-conditions events go through DiagnosticStatus, not WARN" rule that this plan applies is a workspace convention (memory `feedback_wifi_disconnect_not_an_error`), not enforcement. Applying the convention here is the right step; broader enforcement (a grep / lint that flags `RCLCPP_WARN` near `resend_giveup_count_` and similar patterns) is acknowledged as out of scope for this PR. |
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

1. **Annunciator filter behavior**: does the operator-side annunciator allowlist diagnostic task names, or does it accept any `udp_bridge: *` pattern? If allowlisted, this PR needs a paired follow-up commit on `unh_echoboats_project11`. Owner deferred — spot-check during implementation by reading the annunciator's diagnostic-subscription config; if allowlisted, file a paired follow-up issue rather than block this PR.

2. **Threshold lower-WARN question** (deferred): owner accepted 5/50 starting defaults. The deferred question is whether the WARN threshold should be lowered (e.g., to 1/s) in a future tuning pass to flag mild loss earlier. Not blocking this PR — surface as a follow-up if operators report missing-signal in the field.

## Decided (during planning, after owner input 2026-05-20)

- **Thresholds**: 5/s WARN, 50/s ERROR (starting defaults; parameter-tunable per Approach step 3).
- **Diagnostic task naming**: use the existing convention from `udp_bridge.cpp:1498` — `"udp_bridge " + name_ + ": " + remote_name + ": resend give-ups"`.
- **Mutex strategy**: extend `remote_nodes_mutex_` scope to cover the new per-remote diag maps (no new mutex introduced).
- **Test fixture approach**: extract rate calc into a `computeGiveupDiagnostic()` static free function in `udp_bridge/include/udp_bridge/giveup_diagnostic.h`; test the helper directly (no `UDPBridge` instance, no clock fixture needed).

## Estimated Scope

Single PR, two atomic commits. Estimated diff: ~80 LOC implementation (split: ~30 LOC `UDPBridge::diagnoseRemoteGiveups()` wrapper + ~25 LOC `giveup_diagnostic.h` free function + ~25 LOC parameter / map / cleanup wiring) + ~150 LOC tests + ~20 LOC README. No cross-repo coordination required other than the annunciator allowlist check (Open Question 1).
