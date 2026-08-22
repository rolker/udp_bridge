---
issue: 55
---

# Issue #55 — BridgeInfo DataRates undercount fragmented messages (~350x)

## Issue Review
**Status**: complete
**When**: 2026-08-22 16:53 +00:00
**By**: Claude Code Agent (Claude Sonnet)

**Issue**: #55
**Comment**: (best-effort post follows this entry; not recorded inline)
**Scope verdict**: well-scoped

### Scope Assessment

The issue is sharply defined: measurement evidence shows 2.84 MB/s delivered while
`BridgeInfo` DataRates report ~8 kB/s; the 350x gap matches the large-fragmented-message
class. The issue names the relevant code path (`UDPBridge::send<MessageType>` →
`fragment()` → `Connection::send(vector<WrappedPacket>&)`), lists plausible root-cause
candidates (different accounting branch for fragmented vs. single-packet, per-connection
attribution, window collapse, wrapper-size vs. payload recording), and proposes a
focused gtest as the first experiment. Acceptance criteria are specific and testable.
Single-PR scope is realistic: investigate root cause, fix accounting, add regression
test, and answer the `can_send` metering question explicitly.

The issue is in the `rolker/udp_bridge` project repo (core_ws layer), which is the
correct repo. All code in scope (`udp_bridge.cpp`, `connection.cpp`, stats internals)
lives there.

Dependencies: the issue references #52 (resend traffic fix, likely already merged),
#45 (saturation RCA, unstarted), #17/#20 (stats gathering, stats-timer stalls).
Investigation of #55 is independent — it neither blocks nor is blocked by any open
issue. The `can_send` question the acceptance criteria require answering does bear
on #52's goodput signal, but #52 appears complete and this is informational, not
a blocker.

### Principle Alignment

| Principle | Status | Notes |
|---|---|---|
| Human control and transparency | OK | BridgeInfo is operator-facing; fixing the undercount restores honest telemetry. Acceptance criteria allow explicit documentation as exclusion if intentional, which is a valid transparency outcome. |
| Enforcement over documentation | Watch | Acceptance criteria require a regression test — implementation must deliver it, not defer it. Existing test `test_connection_rate_limit.cpp` does not exercise the fragment boundary; the new test fills that gap. |
| Capture decisions | Watch | If the root cause turns out to require a non-obvious accounting choice (e.g., recording compressed vs. uncompressed size, or outer WrappedPacket size vs. payload), that decision warrants a comment or brief design note. |
| A change includes its consequences | Action needed | If `can_send` is found to NOT meter fragmented traffic, fixing it may cause the rate limiter to start dropping large messages that previously passed unmetered — a behavior change that needs verification in the bench and a bench-test update. Also: if DataRates change, the `.agents/README.md` Topics section and any operator-facing docs built on these numbers should be reviewed. |
| Only what's needed | OK | Proposed scope (gtest first, then fix) is appropriately narrow. The receive-side blind spot mentioned in the issue is correctly deferred to a separate concern. |
| Improve incrementally | OK | Single focused PR with a specific first experiment. |
| Test what breaks | OK | Acceptance criteria mandate a regression test that pins byte accounting across the fragmentation boundary — exactly the right shape for a stats-regression test. |
| Workspace vs. project separation | OK | Entirely within `rolker/udp_bridge` project repo. |

### ADR Applicability

| ADR | Triggered | Notes |
|---|---|---|
| ADR-0001 — Adopt ADRs | Watch | If the fix surfaces a design decision (e.g., what "bytes" means in DataRates — wire bytes, payload bytes, compressed bytes), record the decision with rationale. A one-paragraph inline comment may suffice if the scope is local. |
| ADR-0008 — ROS 2 conventions | Yes | Modifying a ROS 2 package; follow ROS 2 conventions. New test must be registered via `add_udp_bridge_gtest()` (not raw `ament_add_gtest` — ODR discipline, see `.agents/README.md` Common Pitfalls). |
| ADR-0013 — progress.md vocabulary | Yes | This entry satisfies the requirement for an `## Issue Review` entry before plan-task. |

### Consequences

- If `sent_packet_statistics_` accounting is changed, all callers of `can_send` and `data_sent_rate()` are affected: review `test_connection_rate_limit.cpp`, `test_resend_budget.cpp`, `test_admission_control.cpp` for test validity.
- If `can_send` does not meter fragmented traffic (found to be a bug), fixing it is a separate severity and the issue already calls this out — the implementer must explicitly answer this and file a follow-up issue if it's out of scope for this PR.
- Operator-facing: if the fix makes DataRates report orders-of-magnitude higher values than before, any configured `maximum_bytes_per_second` thresholds in `bizzyboat.yaml` and similar configs that were tuned against the old (incorrect) numbers will need re-evaluation. Flag this in the PR description.
- `.agents/README.md` DataRates documentation may need updating to reflect what the fixed counters actually measure.

### Recommendations

- [ ] Implement the suggested gtest first (before touching production code): push one message large enough to fragment through a `Connection` and compare bytes-in vs. `data_sent_rate()` bytes-out. This distinguishes "not recorded at all" from "recorded somewhere else" without touching the bench.
- [ ] Explicitly answer the `can_send` metering question in the PR description — even if the answer is "yes, it does meter correctly" — so the #52 goodput signal question is closed.
- [ ] Before merging, verify bench behavior under the fix: if the rate limiter starts seeing fragmented traffic for the first time, large-message delivery rates may change. Document in the PR description.
- [ ] Run `ci_local.sh` for full-scope attestation (ADR-0018) before opening the PR, given the stats-path changes touch concurrent send paths.

## Plan Authored
**Status**: complete
**When**: 2026-08-22 00:00 +00:00
**By**: Claude Code Agent (Claude Sonnet)

**Plan**: `.agent/work-plans/issue-55/plan.md` at `a89b594`
**Branch**: feature/issue-55 at `a89b594`
**Phases**: single

### Open questions
- [ ] Diagnostic test will identify exact fix location in connection.cpp or statistics.cpp (TBD after test).
- [ ] Audit whether receive-side `ReceivedSize::size` (uint16_t) has a symmetric gap; fix here or file follow-up.
- [ ] After fix, bench run to observe whether rate limiter engages for Bulk traffic for the first time.
