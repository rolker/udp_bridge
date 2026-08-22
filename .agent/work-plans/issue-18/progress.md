---
issue: 18
---

# Issue #18 — Bench-test harness for field failure modes

## Implementation
**Status**: complete (Phases 0–6 + added subscriber_death scenario)
**When**: 2026-05-26 00:14 -0400
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))

**Branch**: feature/issue-18 (PR #27)
Phases 0–3 landed earlier (`191e5e1`). This session added:
- **Phase 4** multi-link invariants (topic-list confinement, cross-path non-poisoning Y, critical-gap over-horizon G, drop-by-tier measure-and-report #19) — all 9 range_degradation invariants pass (~112 s, FastDDS).
- **Phase 5** retire mininet → `test/mininet/.archived/` + pointer README + `mainpage.dox` repoint.
- **Phase 6** docs/CI: no CI in repo; skip-guards + opt-in gating documented as the protection.
- **subscriber_death** scenario (issue #10 wedge) — see finding below.

## Local Review (Pre-Push)
**Status**: complete
**When**: 2026-05-26 00:14 -0400
**By**: Claude Code Agent (Claude Opus 4.7 (1M context)) — fresh-context adversarial + flake8
**Verdict**: approved (3 suggestions applied)

**Branch**: feature/issue-18 at `ca66f76`
**Mode**: pre-push
**Depth**: Standard (bench Python; opt-in/skip-guarded tests)
**Must-fix**: 0 | **Suggestions**: 3 (applied) + 1 noted

### Findings
- [x] (suggestion) Wedge test correlated wall-clock `stall_unix` against the tracer's monotonic axis — added a `unix_time` column to `recv_q_trace.py`, test now slices on it.
- [x] (suggestion) `test_no_wedge_recvq_bounded` could skip vacuously if the tracer never bound the socket — now fails on rows-but-no-socket (harness failure).
- [x] (suggestion) SIGKILLed Bulk victim not reaped — added `wait()`.
- [ ] (noted, pre-existing, not fixed) `_parse_count` reads child stderr only after `wait()`; a very chatty RMW could fill the 64 KB pipe. Bounded by the per-`wait` timeout (degrades to a missed `sub_count`, not a hang) and does not affect the new test's assertions, which read the flushed count-trace files, not the pipe.
- flake8: no ament lint enforced for this package; remaining hits are pre-existing style (E241 aligned `PHASE_TRAJECTORY` table, W503 default-ignored).

### Finding — wedge not reproducible at bench scale
The `subscriber_death` scenario could NOT reproduce the #10 wedge under FastDDS, CycloneDDS, or Zenoh, even at ~58 MB/s Bulk into a frozen consumer — operator Recv-Q/Send-Q stayed flat at 0. `publish()` doesn't block the drain at bench scale (Zenoh async hand-off). So it is a **no-wedge regression guard**, not a red→green reproduction; #28's fix remains component-proven by the `PublishQueue` unit tests. Captured in the test docstring, `test/bench/README.md`, and the plan's Implementation Notes. Per user decision (2026-05-25), not posted to GitHub issues.

## Post-PR Review — PR #27 @ 1e3278432 — 2026-06-14 (coordinator-dispatched reviewer; Copilot quota out)

**Verdict: CHANGES NEEDED (close to APPROVE).** Architecture is correct and well-reasoned: namespace/qdisc leak-safety is excellent (orchestrator re-execs under `unshare -Urn` so all veths/qdiscs live in an ephemeral userns the kernel tears down — no host state to leak); privilege handling is CI-safe (no root, CAP_NET_ADMIN inside the userns, tests skip-not-fail without userns, scenarios double-gated on `UDP_BRIDGE_BENCH_SCENARIOS=1`); child cleanup solid (killpg TERM→KILL, atexit/signal handlers). Issue coverage honest (#20 asserted; #10 framed as a no-wedge regression guard with candid docstring; #23 tested elsewhere via gtest). Governance clean.

Two unaddressed prior-round (Copilot) findings block:
- **#1 (must-fix): vacuous-pass guard missing in `test_range_degradation.py`** (`test_range.py:199,203`). If `recv_q_trace.py` never binds the operator socket, all rows have empty `recv_q_bytes` → every sample q=0 → `test_invariant_recv_q_no_sustained_climb` passes vacuously (harness failure masquerading as healthy queue). `test_subscriber_death.py:163-186,218` already does this right (tracks total vs usable rows, fails when `total>0 and usable==0`). Port that guard. For a harness whose job is catching silent failures, this is the exact failure mode the Quality Standard forbids.
- **#2 (must-fix): missing rosbag2 `<test_depend>`s in package.xml** — the range scenario shells `ros2 bag record` and `bag_reader.py` imports `rosbag2_py`/`rclpy.serialization`/`rosidl_runtime_py`; none declared. Opt-in path fails on a clean install. Add `rosbag2_py`, `rosbag2_transport`, `rosidl_runtime_py` test_depends.

Nice-to-have (round-2 one-liners, unaddressed): tee docstring lie (run_scenario.py:322), leaked log_fp handle (:326), per-tick 480KB alloc in Bulk pub (pub.py:66 — determinism), dead SQLite shims in bag_reader.py, run_scenario docstring omits subscriber_death scenario. Fix #1+#2 and optionally sweep these → APPROVE.

## Implementation
**Status**: complete — PR #27 rehabbed (rebase + all review findings addressed)
**When**: 2026-08-22 10:05 -04:00
**By**: Claude Code Agent (Claude Opus 5 (1M context))

**Branch**: feature/issue-18 (PR #27), rebased onto `origin/jazzy`

### Rebase
PR had gone 58 commits behind `jazzy` and GitHub reported it DIRTY. Single
conflict: `udp_bridge/CMakeLists.txt` — jazzy added gtest registrations
(`test_subscriber_registry`, `test_stale_packet_gate`, `test_reorder_buffer`)
where this branch adds the `ament_add_pytest_test` bench block. Resolved by
keeping both; no semantic overlap. All 18 commits replayed clean.

### Findings addressed (both must-fix from the 2026-06-14 review + all five nice-to-haves)
- [x] **(must-fix #1) Vacuous-pass guard** — `test_invariant_recv_q_no_sustained_climb`
      folded empty `recv_q_bytes` rows in as q=0, so a tracer that never bound
      the operator socket made the invariant pass vacuously. Now counts total vs
      usable rows: skips only when nothing was recorded, and **fails** with an
      explicit harness-failure message when there are rows but none usable.
      Synthetic zeros also broke genuine climb runs in two, so dropping them
      strengthens the assertion in the healthy case too.
- [x] **(must-fix #2) rosbag2 test_depends** — added `ros2bag`, `rosbag2_py`,
      `rosbag2_transport`, `rosidl_runtime_py`, plus `rosbag2_storage_mcap`
      (Jazzy records mcap by default, so the plugin is needed to read back what
      the scenario just recorded — not in the original finding).
- [x] Dead SQLite shims in `bag_reader.py` — `list_topics`/`count_messages`/
      `find_bag_db` had no callers anywhere and were sqlite3-only against a
      mcap default. **Removed** rather than re-documented (workspace preference:
      remove obsolete outright). Deferred-import comment corrected.
- [x] `pub.py` per-tick 480 KB allocation — hoisted to one reused immutable
      buffer. ~4.6 MB/s of allocator churn inside the publisher used to measure
      the link; the harness was perturbing its own measurement.
- [x] `launch_bridge_logged` docstring claimed a `tee` that does not exist.
- [x] Same function leaked the parent's `log_fp` handle — closed after spawn.
- [x] `run_scenario.py` Usage block omitted the `subscriber_death` scenario.

### Verification
- `colcon build` clean (only pre-existing `-Wpedantic` flexible-array warnings).
- Default suite: **162 tests, 0 failures, 11 skipped**.
- Opt-in `UDP_BRIDGE_BENCH_SCENARIOS=1 ... -k test_bench_range_degradation`:
  **all 9 invariants pass** (3 min 4 s), including the modified recv_q invariant
  running against real (non-empty) tracer data.
- Guard logic verified against synthetic traces: rows-but-no-values → FAIL
  (harness failure), healthy trace → proceed, zero rows → skip.
- `pre-commit run --from-ref origin/jazzy --to-ref HEAD` clean. Note the repo
  has **no pre-commit git hook installed** and CI excludes linter tests pending
  #40, so nothing automated would have caught the four end-of-file misses fixed
  here; hooks were run by hand.

### Notes / follow-ups
- The PR body's "no CI workflow in this repo" is stale — `.github/workflows/ci.yml`
  landed in `973ead7` after this branch was last touched. This push is the
  first time CI will run against #27. PR body needs that line corrected.
- Multi-agent `/review-code` was **not** run on this delta (sub-agent dispatch
  disabled for this session); the delta was self-reviewed against the diff
  instead. Worth a pass before merge if the session constraint lifts.
