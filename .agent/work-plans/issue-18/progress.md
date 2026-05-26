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
