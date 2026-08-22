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

## Implementation
**Status**: netns defect fixed; **one invariant now fails legitimately — open decision**
**When**: 2026-08-22 10:35 -04:00
**By**: Claude Code Agent (Claude Opus 5 (1M context))

**Branch**: feature/issue-18 (PR #27)

### Round-2 review findings — all seven addressed
Copilot's re-review after the rebase raised seven items. The headline one was
real and load-bearing.

- [x] **`run_scenario.py:186` — tc netem was a total no-op.** Both veth ends
      were addressed in one network namespace, so the kernel installed a
      `local` route per address and delivered over `lo`; nothing egressed the
      veth and the qdisc was never traversed. **Confirmed by experiment, not
      inspection**: same-namespace pair with `netem loss 100%` → ping 0% loss,
      device TX counter 0 packets; after splitting → ping 100% loss.
      Fix: orchestrator's own namespace is the operator side; boat side is a
      nested namespace held open by a keeper process, addressed by PID via
      `nsenter -t <pid> -n` (PID rather than `ip netns add`, which would need
      a writable /run/netns and so a private mount namespace). Boat bridges,
      publishers and their lifecycle clients run under nsenter; Zenoh gets a
      router per namespace; both `lo`s come up at setup.
      Also fixed while in there: impairment was applied to the operator end
      only, i.e. to the *return* direction — the boat→operator bulk flow was
      the unshaped one. Now applied to both ends, so `delay_ms` is a one-way
      delay and RTT is 2× it.
- [x] `run_scenario.py:757` — `any(c > 0 ...)` let a documented three-tier run
      pass with one tier delivering. Now `all`, and names the empty tiers.
- [x] `test_subscriber_death.py:222` — skipped on an empty post-stall slice;
      the tracer starts before the stall and runs a full observation window
      afterwards, so empty means it died mid-run. Now fails.
- [x] README: stderr described as tee'd (it is captured to a file); CI section
      said the repo has no CI workflow (it does — `ci.yml`, added after this
      branch was last touched); added a Topology section documenting the
      namespace split and why collapsing it re-breaks every impairment.
- [x] `.archived/README.md` pointed at a `recv_q_trace.py` that moved to
      `test/bench/`.
- [x] `plan.md:95` documented `UDP_BRIDGE_BENCH_OUTDIR`, never implemented.

### Verification after the fix
- Smoke, run directly: 8 published / 8 delivered across the real veth path.
- Default suite: **162 tests, 0 errors, 0 failures, 11 skipped**.
- `subscriber_death`: **both invariants pass** (39 s). Worth noting this
  *strengthens* the PR's "no wedge at bench scale" finding rather than
  flipping it — that scenario runs clean profiles by design, but "clean" still
  means 30 mbit / 5 ms, which was previously not applied at all. Bulk offers
  ~4.8 MB/s into a now-real 3.75 MB/s cap and the drain still does not wedge.
- `range_degradation`: **8 of 9 invariants pass. `test_invariant_resend_amplification`
  now FAILS** — see below.

### OPEN — resend amplification fails under real impairment
```
fringe:   resend/tx_ok = 0.045 > F×loss = 0.010  (loss 0.5%)
lossy:    resend/tx_ok = 1.643 > F×loss = 0.060  (loss 3%)
critical: resend/tx_ok = 3.108 > F×loss = 0.200  (loss 10%)
```
This is the failure mode issue #9 exists for, and the invariant was written to
catch it — it only ever passed because the link was never impaired.

Two candidate explanations, not yet separated, and **not** to be resolved by
loosening `F`:

1. **A real amplification path in `Connection::send`.** The aggregate
   `can_send` pre-check drops a whole batch *before* `WrappedPacket` assigns a
   packet number — those cost nothing. But the per-packet inner `send()` runs
   `can_send` again *after* the packet has been numbered and stored in
   `sent_packets_` (`connection.cpp` ~line 375-390 vs ~479). A rate-limiter
   drop on that path burns a sequence number without transmitting, the
   receiver sees a gap, NAKs, and the sender resends a packet it deliberately
   chose not to send — under load, repeatedly. Matches the field picture in
   #9 (~22% resend overhead, 126% rx_duplicate) and the 2026-08-03 saturation
   episode.
2. **The invariant's threshold model is wrong under oversubscription.** The
   ceiling is `F × netem_loss`, but in the `critical` phase Bulk offers
   ~4.8 MB/s into a 500 kbit/s shaped path — ~77× oversubscribed. Most loss is
   queue/limiter drop, not netem's 10%, so the denominator does not describe
   the packet loss the resend machinery actually sees.

Both may be true. Needs a decision before this PR merges: recommended path is
`xfail(strict=True)` on that one invariant with a pointer to a new udp_bridge
issue, so the signal is recorded rather than suppressed and flips to a failure
when fixed. **Not done unilaterally — awaiting Roland.**

## Implementation
**Status**: complete — #52 filed, invariant xfailed, opt-in suite clean
**When**: 2026-08-22 10:54 -04:00
**By**: Claude Code Agent (Claude Opus 5 (1M context))

**Branch**: feature/issue-18 (PR #27)

Closes out the open decision from the previous entry.

### Root cause established (was left as two competing hypotheses)
Pulled the per-phase `BridgeInfo` out of the preserved artifacts. **The
earlier sequence-burn hypothesis is wrong** — `message.dropped_bytes_per_second`
and `message.failed_bytes_per_second` are 0.0 in every phase, so the rate
limiter never sheds and never burns a sequence number. Actual mechanism:

| phase | link | AIMD cap | msg | resend | dup | resend/msg |
|---|---|---|---|---|---|---|
| in_range_clean | 3750 | 3346 | 7.7 | 0.3 | 0.0 | 0.04 |
| fringe | 1250 | 2600 | 8.5 | 0.4 | 0.0 | 0.05 |
| lossy | 375 | 2480 | 8.5 | 13.9 | 0.3 | 1.64 |
| critical | 62.5 | 2291 | 8.5 | 26.4 | 3.5 | 3.11 |
| over_horizon | (100% loss) | 860 | 8.5 | 3.2 | 0.2 | 0.38 |

(kB/s; `link` = applied netem rate, `AIMD cap` = reported `effective_rate_limit`)

- Admission never converges on the real link — 2291 kB/s reported against a
  62.5 kB/s path, and `kDefaultAdmissionFloorFraction = 0.1` floors it at 10%
  of the **configured** cap (400 kB/s), still 6× the real link.
- The #44 resend budget is 25% of that, authorizing 573 kB/s of resends on a
  62.5 kB/s path — 9× the link. It behaves as specified and is irrelevant.
- Real amplification: 3× more bytes retransmitted than originally sent, ~31×
  the loss-implied expectation; duplicates 41% of the message rate, matching
  #9's 126% rx_duplicate signature.
- The bridge's own telemetry reports zero dropped and zero failed throughout —
  a diagnostic blind spot matching the 2026-08-03 "all red, no obvious cause".

Confound named in the issue: symmetric shaping makes `delay_ms` one-way, so
`critical` RTT is 200–260 ms against `kResendBackoffBase` of 0.200 s. But
`lossy` at 80–110 ms RTT still shows 1.64, so it is not the driver.

### Actions
- Filed **[udp_bridge#52](https://github.com/rolker/udp_bridge/issues/52)**
  "Resend amplification under real link degradation: admission floor and
  resend budget scale off the configured cap, not observed throughput"
  (label `bug`) with the table, mechanism, repro command, and four
  not-yet-decided directions. Cross-referenced #9, #43, #44, #45.
- `test_invariant_resend_amplification` marked `xfail(strict=True)` against
  #52, with an explicit "do NOT loosen F_resend_multiplier" note in the
  reason string. strict so it becomes a failure again when #52 lands.

### Verification
- Opt-in `range_degradation`: **8 pass, 1 xfail, 0 failures** (3 min 5 s).
- `colcon test-result`: 162 tests, 0 errors, **0 failures**, 1 skipped.
- flake8 + `pre-commit run --from-ref origin/jazzy --to-ref HEAD` clean.

### PR #27 status
Rebase clean, CI green, all 11 round-1 and all 7 round-2 review threads
addressed, opt-in suite green-with-one-recorded-xfail. Ready for human
content review. Not merged — awaiting Roland.
