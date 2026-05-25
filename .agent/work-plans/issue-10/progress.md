---
issue: 10
---

# Issue #10 — Bridge wedges (reader thread blocked, Recv-Q backup) when remote subscriber dies

## Plan Authored
**Status**: complete
**When**: 2026-05-25 18:30 -0400
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))

**Plan**: `.agent/work-plans/issue-10/plan.md` at `44a81ad`
**PR**: https://github.com/rolker/udp_bridge/pull/28 (`[PLAN]` prefix)
**Phases**: single

### Open questions
- [x] Cross-topic isolation scope — **decided: single publish-worker this PR; per-publisher isolation deferred to a follow-up issue** (user, 2026-05-25).
- [ ] Confirm the kill-subscriber end-to-end reproduction (acceptance #1) lands in the issue #18 bench harness (PR #27), not a standalone launch_test here.

## Plan Review
**Status**: complete — verdict **revise**, revisions applied
**When**: 2026-05-25 18:30 -0400
**By**: Claude Code Agent (Claude Opus 4.7 (1M context)) — fresh-context internal review (pre-Copilot)

**Plan**: `.agent/work-plans/issue-10/plan.md` at `078f794`

Findings, all incorporated into the revised plan:
- [x] **B1** (blocking): `create_generic_publisher()` (first-arrival) is also on the drain thread and can block on rmw → move the whole first-arrival create + bridge-info + publish into the worker sink, not just `publish()`.
- [x] **B2**: first-arrival `sendBridgeInfo()` also inline on drain → folded into B1's sink move.
- [x] **B3/B4** (lifecycle): construct/start queue in `on_configure`, `stop()`+join in `on_cleanup`; `push()` no-op after stop; join gates post-deactivation publishes (destination publishers are non-lifecycle); cleanup→configure re-creates a fresh worker.
- [x] **S2**: bound the queue by **bytes**, not message count (reassembled images are large).
- [x] **N1/N2**: injected `std::function` sink seam → unit-test non-stall with a blocking fake sink (the proxy for a dying DDS subscriber).
- [x] **S1/S3/S4/N3**: doc unrecoverable drop; diagnostic via `syncDiagnosticTasks`; `declareIfMissing`; update `udp_bridge_node.cpp` invariant comment.

## Implementation
**Status**: complete
**When**: 2026-05-25 19:48 -0400
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))

**Branch**: feature/issue-10 at `558bebf`
Implemented per plan: `PublishQueue` component + `decodeData`/`publishItem` rewiring + lifecycle wiring + diagnostic + `qos_design.md` + `udp_bridge_node.cpp` comment. Build clean; full suite **94 tests, 0 failures** (8 new `PublishQueue` tests).

## Local Review (Pre-Push)
**Status**: complete
**When**: 2026-05-25 19:48 -0400
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))
**Verdict**: changes-requested (all resolved)

**Branch**: feature/issue-10 at `558bebf`
**Mode**: pre-push
**Depth**: Deep (reason: concurrency + lifecycle in a core comms package)
**Must-fix**: 1 | **Suggestions**: 2

### Findings
- [x] (must-fix) Worker `run()` had no exception barrier — a throwing sink (`create_generic_publisher`/`publish`/`sendBridgeInfo`) → `std::terminate`, a regression vs `decode()`'s old catch-all. Fixed: `publishItem` try/catch+log + `run()` `catch(...)` backstop + `ThrowingSinkDoesNotKillWorker` test — `publish_queue.h`, `udp_bridge.cpp` `publishItem`
- [x] (suggestion) Worker published while INACTIVE — moved `start`→`on_activate`, `stop`→`on_deactivate` — `udp_bridge.cpp` `on_activate`/`on_deactivate`
- [x] (suggestion) New concurrent outbound-send caller + shutdown-join bound documented — `udp_bridge_node.cpp` callback-group comment
- [x] (static) cppcheck: all findings on pre-existing/context lines (not changed lines) — dropped per silence filter
