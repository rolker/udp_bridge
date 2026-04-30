# Plan: Robustness — MultiThreadedExecutor + best-available republish QoS + per-topic config

## Issue

https://github.com/rolker/udp_bridge/issues/11

## Context

`udp_bridge` is a ROS-1 port; QoS was kicked down the road. Three concrete pain
points motivate this work:

1. `udp_bridge_node.cpp:7` uses `SingleThreadedExecutor` — every timer, every
   forwarding subscription callback, and every receive-decode-publish cycle
   share one thread. Any blocking call (#10's symptom: a dead remote
   subscriber) starves `spin_once()` and the kernel `Recv-Q` (500 KB
   `SO_RCVBUF` set at `udp_bridge.cpp:91`) backs up.
2. `udp_bridge.cpp:413` publishes received data with bare `rclcpp::QoS(1)`
   which defaults to RELIABLE — a lie for UDP, and exactly the dead-subscriber
   stall mechanism #10 hypothesizes.
3. There is no per-topic QoS knob; a topic that genuinely wants
   `transient_local` (latched config, static map) cannot get it.

The package's QoS contract has never been written down. This PR closes both
the code gaps and the documentation gap.

## Approach

**Sequencing**: this PR lands first; #9 rebases onto its mutex additions in
`Connection`. Decided during planning.

**As-you-go cleanup**: every commit that edits a file in this PR also renames
any `m_foo` member to `foo_` in that file, with full grep-and-replace across
the codebase to update every reference (header + all `.cpp` + tests).
The current package mixes both conventions — newer additions use `*_`, older
ROS-1-port code uses `m_*`. Standardize on `*_` (ROS 2 / Google style).
Never half-rename. Per-commit grep before push to verify zero remaining
hits of the old name.

Six commits on `feature/issue-11`:

1. **Plan commit** — this file.
2. **`udp_bridge/doc/qos_design.md`** — written first so the rest of the
   PR has an anchor. (Lives in the existing `doc/` sphinx setup, not a
   new top-level `docs/` directory.) Captures: mental model (UDP wire is dominant constraint), per-
   dimension policy (reliability=BEST_AVAILABLE, durability=opt-in
   transient_local, history=KEEP_LAST(N), deadline/lifespan/liveliness not
   translated), per-topic override mechanism, future-work pointer for
   source-QoS advertisement in `bridge_info`. Also covers
   `MessageInternal` schema mismatched-pair behavior: empty/zero values
   from an old sender are treated as defaults; ROS 2 message
   serialization tolerates trailing-field absence so a new sender's
   QoS fields are simply not read by an old receiver (verify the
   trailing-field-tolerance behavior during implementation; if not
   automatic, document explicitly that both endpoints must be redeployed
   together).
3. **Executor split + thread-safety primitives.** Split into two commits
   for reviewability:

   **3a — Executor split, callback groups, m_*→*_ renames, mutex
   declarations.** `udp_bridge_node.cpp` switches to
   `MultiThreadedExecutor` with the three-group invariant documented
   inline. `udp_bridge.h` gains the three callback-group members and
   the four mutexes (publishers_, subscribers_, remote_nodes_,
   pending_connections_) as declarations. `udp_bridge.cpp` creates the
   groups in `on_configure` and assigns timers, services, and the
   forwarding-subscription callback to the right groups. The five
   `m_*` members in the package (`m_socket`, `m_port`,
   `m_max_packet_size`, `m_subscribers`, `m_publishers`) are renamed
   to trailing-underscore form. Build verifies the structural changes
   compile and link.

   **3b — Mutex audit + lock guards.** Adds `std::lock_guard` /
   `std::scoped_lock` at every site that accesses shared state across
   callback groups. Lock order (to prevent deadlock):
   `remote_nodes_mutex_` → `subscribers_mutex_` → `publishers_mutex_` →
   `pending_connections_mutex_`. Helper functions (e.g. `sendBridgeInfo`)
   document that callers must hold the relevant locks; entry points
   (timers, callbacks, service handlers) acquire them. Pattern: lock
   briefly to access the map, copy out shared_ptrs, release before
   slow operations (publish, network I/O).

   Three callback groups created in `UDPBridge::on_configure`:
   - `socket_drain_group_` (MutuallyExclusive) — `spin_timer_` only.
   - `republish_group_` (MutuallyExclusive) — forwarding subscription
     callbacks (`callback`) and per-topic generic publishers' work in
     `decodeData`. Note: a generic publisher's `publish()` runs on the
     calling thread, so the group constraint applies via the path that
     reaches it (the receive-decode pipeline triggered by `spin_once`,
     and forwarding subs).
   - `periodic_group_` (MutuallyExclusive) — `bridge_info_timer_`,
     `stats_report_timer_`, `diagnostic_timer_`,
     `subscription_update_timer_`.
   Add thread-safety where the executor split lets concurrent access happen:
   - `m_publishers` map (`udp_bridge.h:229`) — guard with `std::mutex`;
     reads in `decodeData`, writes on first arrival per topic.
   - `m_subscribers` — guard with `std::mutex`; modified in
     `addSubscriberConnection` (service thread) and `updateLocalSubscriptions`
     (periodic group), read in `callback` (republish group).
   - `remote_nodes_` — guard with `std::mutex`; modified in
     `decodeBridgeInfo` and `unwrap` (socket_drain group), read by stats /
     diagnostic / subscription-update timers (periodic group).
   - `Connection::sent_packets_` — already read/written across two paths
     (send-and-cache from publish path; resend lookup from socket-drain
     path). Add `std::mutex` inside `Connection`. Cleanup runs on a
     configured cadence; serialize against both readers and writers.
   - Statistics aggregation — already touched by send (republish group)
     and stats timer (periodic group). Add `std::mutex` inside the
     statistics struct, or use the existing one if present.
   - `diagnostic_task_names_` (`udp_bridge.h:240`) — modified in
     `syncDiagnosticTasks` (periodic group). Single-writer; no contention
     today, but document the invariant.
   - `pending_connections_` (`udp_bridge.h:254`) — touched by
     `decodeConnection` (socket-drain group, on connection-management
     packet receipt) and the four service handlers (currently default
     callback group, see below). Add `std::mutex`.
   - **Service handlers** (`subscribe_service_`, `advertise_service_`,
     `add_remote_service_`, `list_remotes_service_` declared at
     `udp_bridge.h:217-220`) — under `MultiThreadedExecutor` these
     default to the node's default callback group, which races against
     the new socket-drain / republish / periodic groups. Assign all four
     services to the **periodic group** (services are infrequent admin
     calls; same group as other admin-style timers is fine and avoids
     proliferating groups). The mutexes added for `m_subscribers`,
     `remote_nodes_`, and `pending_connections_` cover the actual data
     races; the group assignment ensures we don't accidentally serialize
     hot-path work behind a service call.
   Add a brief `// callback group invariants:` comment block in
   `udp_bridge_node.cpp` explaining the three groups and what each owns.
4. **Receive-side publisher QoS = BEST_AVAILABLE + per-topic config wiring.**
   - `udp_bridge.cpp:413` — replace bare `rclcpp::QoS(1)` with a `QoS`
     constructed from per-topic config (passed via `MessageInternal`),
     defaulting to `reliability_best_available()` and `durability_volatile()`
     and `keep_last(1)`.
   - Extend `udp_bridge_interfaces/msg/MessageInternal.msg` with three
     fields: `string reliability` ("best_available"|"reliable"|"best_effort",
     default "best_available"), `string durability`
     ("volatile"|"transient_local", default "volatile"), `uint32 history_depth`
     (default 1).
   - Backward-compat note: empty/zero values from an old sender are treated
     as defaults. Document this in `qos_design.md`.
   - Per-topic config in `on_configure` (`udp_bridge.cpp:162-179` block)
     reads three new params (`reliability`, `durability`, `history_depth`)
     alongside `queue_size`, `period`, `source`, `destination`. Stash on
     `RemoteDetails` in `types.h` (new fields: `std::string reliability`,
     `std::string durability`, `uint32_t history_depth`).
   - `callback()` populates `MessageInternal` QoS fields from the
     `RemoteDetails` for the destination connection.
   - `updateLocalSubscriptions` (`udp_bridge.cpp:507-508`) — when per-topic
     config asks for `transient_local`, build the source-side subscription
     with `durability_transient_local()` so the bridge actually receives
     latched messages. Reliability stays `best_effort` on the subscriber
     (already accepts any publisher).
5. **Tests.** Three layers:
   - **Unit**: a new `test_qos_resolution.cpp` in `udp_bridge/test/` that
     constructs `MessageInternal` instances with each combination of QoS
     fields and verifies the resolved `rclcpp::QoS` matches expectations
     (default fallback, explicit reliable/best_effort, transient_local,
     varying depth). No node spin required; just the QoS-resolution helper.
   - **Integration (gtest + rclcpp)**: spin two nodes — a publisher with
     declared RELIABLE QoS and a subscriber with declared RELIABLE QoS,
     bridged through a single-process `UDPBridge` configured with
     loopback. Assert both directions match (publisher→bridge,
     bridge→subscriber) without the silent-no-match failure mode.
   - **Mininet (#10 reproduction attempt — manual run, not CI gate)**:
     the existing `udp_bridge/test/mininet/` setup
     (`multilink.py`, `operator.bash`, `robot.bash`, etc.) was added some
     time ago and is likely stale — first task is to get it running on a
     current Linux dev machine (sudo + netns) and document the steps in a
     new `udp_bridge/test/mininet/README.md`. Then add a scenario that
     kills an operator-side subscriber mid-stream. Assert the bridge's
     `Recv-Q` (via `getsockopt`/`/proc/net/udp`) stays bounded and
     bridge-info heartbeats keep flowing. Outcome documented in PR
     description regardless of whether the wedge reproduces — the test
     is exploratory. Not wired into CI (mininet needs root and a Linux
     kernel; flaky in containerized runners). The gtest integration test
     in this same commit is the CI signal; mininet is the closed-loop
     manual check on a dev machine before merge. Capture a
     `Recv-Q`-over-time trace (e.g., a CSV from periodic
     `/proc/net/udp` snapshots, or a small `python3 + getsockopt` helper)
     before-and-after the killed-subscriber event; include the trace in
     the PR description or as a dated results file under
     `udp_bridge/test/mininet/results/` so the wedge-or-no-wedge outcome
     is reviewable rather than narrated.
6. **Validation.** Build + run all tests. Run `make test` from workspace
   root. Capture output for PR description. Flag #10 reproduction outcome
   explicitly.

## Files to Change

| File | Change |
|------|--------|
| `udp_bridge/src/udp_bridge_node.cpp` | Switch to `MultiThreadedExecutor`; add design comment |
| `udp_bridge/src/udp_bridge.cpp` | Create 3 callback groups in `on_configure`; assign timers; add mutexes; per-topic QoS config reads; resolve QoS for publisher in `decodeData`; populate QoS fields in `MessageInternal` from `callback`; honor `transient_local` on source subscription in `updateLocalSubscriptions` |
| `udp_bridge/src/connection.cpp` | Add `std::mutex` for `sent_packets_` access |
| `udp_bridge/include/udp_bridge/udp_bridge.h` | Callback group members; mutexes for `m_publishers` / `m_subscribers` / `remote_nodes_` |
| `udp_bridge/include/udp_bridge/connection.h` | Mutex declaration |
| `udp_bridge/include/udp_bridge/types.h` | Extend `RemoteDetails` with `reliability`, `durability`, `history_depth` |
| `udp_bridge_interfaces/msg/MessageInternal.msg` | Add `reliability`, `durability`, `history_depth` fields |
| `udp_bridge/doc/qos_design.md` | New design doc (added to existing `doc/` sphinx setup, not a new `docs/` dir) |
| `udp_bridge/doc/index.rst` | Add `qos_design` to toctree |
| `udp_bridge/test/test_qos_resolution.cpp` | New unit test |
| `udp_bridge/test/test_executor_split_integration.cpp` | New integration test |
| `udp_bridge/test/mininet/test_subscriber_death.{py,bash}` | New mininet scenario |
| `udp_bridge/test/mininet/README.md` | New: how to run the (refreshed) mininet setup, prerequisites, expected output |
| `udp_bridge/test/mininet/*` | Refresh stale setup as needed to get it running on a current Linux dev machine |
| `udp_bridge/CMakeLists.txt` | Register new tests |
| `udp_bridge/package.xml` | Add test deps if needed |

## Principles Self-Check

| Principle | Consideration |
|---|---|
| Human control and transparency | Per-topic QoS config is visible. QoS-default change uses BEST_AVAILABLE so no subscriber is silently disconnected. PR description must call out the QoS-contract change explicitly. |
| Capture decisions, not just implementations | `qos_design.md` is the artifact; this is the whole point of commit 2. Callback-group invariants captured in inline comment in `udp_bridge_node.cpp`. |
| A change includes its consequences | Tests (unit + integration + mininet) ship with the change. `MessageInternal` schema extension documented in `qos_design.md`. CAMP impact analyzed in review-issue comment; mitigation is built-in (BEST_AVAILABLE matches RELIABLE subscribers). |
| Only what's needed | Idle publisher cleanup explicitly dropped (issue body Out of Scope). Auto-mirror deferred to Phase 2 (issue body Out of Scope). |
| Improve incrementally | 6 commits, each independently reviewable. |
| Test what breaks | Three test layers; mininet wedge reproduction is the headliner. rolker explicitly asked for "as many tests as appropriate". |

## ADR Compliance

| ADR | Triggered | How addressed |
|---|---|---|
| 0002 — Worktree isolation | Yes | Working in `feature/issue-11` worktree on the udp_bridge project repo. |
| 0008 — Follow ROS 2 conventions | Yes | `MultiThreadedExecutor` + `MutuallyExclusive` callback groups is canonical rclcpp. Per-topic param naming follows existing `udp_bridge.cpp:162-179` style. New `MessageInternal` fields use ROS 2 standard names (`reliability`, `durability`, `history_depth`). Targets Jazzy (verified `reliability_best_available()` available in `/opt/ros/jazzy/include/rclcpp/rclcpp/qos.hpp:190`). |
| 0001 — Adopt ADRs | No | Package-internal design decision; `qos_design.md` in the udp_bridge repo is the right home, not `docs/decisions/` in the workspace. |

## Consequences

| If we change... | Also update... | Included in plan? |
|---|---|---|
| `udp_bridge.cpp` executor / threading | Connection state synchronization | Yes — commit 3 mutex audit |
| `MessageInternal.msg` schema | `udp_bridge_interfaces` build, downstream readers | Yes — schema is internal to udp_bridge, only `udp_bridge.cpp` reads it; backward-compat handled by treating empty strings as defaults |
| Receive-side publisher QoS default | CAMP subscribers (`layers/main/ui_ws/src/camp/src/camp2/ros/`) | No code update needed — `BEST_AVAILABLE` matches RELIABLE subscribers automatically; that's why this option was chosen over a flat `BEST_EFFORT` flip. **No per-topic `reliability: reliable` override inventory ships in this PR**; `BEST_AVAILABLE` matching is the sole mitigation for the CAMP blast-radius concern raised in the review-issue comment. The per-topic override (item 3) is the future hatch if a specific topic ever needs guaranteed reliable receive-side semantics — but no current consumer demands it, so we don't pre-emptively populate the inventory. |
| `data_rates.msg` / `bridge_info.msg` | `rqt_udp_bridge` consumer | Not changed by this PR — confirmed |

## Open Questions

None remaining at plan time. All five questions resolved during planning
(see `## Decisions made during planning` below).

## Estimated Scope

Single PR, six commits. Working estimate: ~600-900 lines of code change plus ~300-500 lines of new tests, plus the design doc (~150 lines). Some additional churn from `m_*` → `*_` renames in any file touched.

## Updates from review-plan

PR-#12 review (https://github.com/rolker/udp_bridge/pull/12#issuecomment-4356543190)
flagged five items; absorbed inline:

1. Thread-safety audit was missing `pending_connections_` and the four
   service handlers under `MultiThreadedExecutor`. Now enumerated in
   commit 3 with a periodic-group assignment for the services.
2. CAMP override decision made explicit in the Consequences table and
   in commit 2's `qos_design.md` outline ("`BEST_AVAILABLE` matching is
   the sole mitigation; no override inventory ships").
3. `MessageInternal` mismatched-pair compatibility note added to commit
   2's `qos_design.md` outline.
4. Mininet test artifact requirement (`Recv-Q` trace) added to commit 5.
5. Reviewer's "typo" finding (`udp_bridge.cpp:7` vs `udp_bridge_node.cpp:7`)
   was a misread — both the plan and the issue body correctly say
   `udp_bridge_node.cpp:7`. No fix needed.

## Decisions made during planning

These resolved the original `Open Questions` set; recorded here so the
rationale survives.

- **Sequencing**: this PR (#11) lands first; #9 rebases. #11 is structural
  (executor split adds mutexes to `Connection::sent_packets_`, which #9
  touches via TTL alignment). The reverse rebase would be more error-prone.
- **Mininet as manual-run, not CI gate**: mininet needs root and a Linux
  kernel; CI runners are flaky for it. Wedge reproduction in mininet is a
  one-time manual exercise per PR; gtest integration test is the CI signal.
  Stale mininet setup gets a freshening pass as part of this work.
- **`MessageInternal` schema = explicit string fields** (`reliability`,
  `durability`, `history_depth`). Debuggable in `ros2 topic echo` and bag
  inspection; backward-compat trivial (empty/zero = defaults). Rejected
  alternatives: opaque QoS-bytes blob (worse debugging) and receive-side-only
  config (two configs to maintain per topic, async with discovery).
- **Naming convention**: trailing-underscore (`*_`) is the target. Rename
  `m_*` → `*_` as we go, with full grep-and-replace per variable; never
  half-rename.
