---
issue: 22
---

# Issue #22 — 'Giving up on resend' WARN log too loud — rate-limit or surface as DiagnosticStatus

## Local Review
**Status**: complete
**When**: 2026-05-20 14:10 -04:00
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))
**Verdict**: changes-requested

**PR**: #24 at `cfd57d3`
**Mode**: post-PR
**Depth**: Standard (reason: new C++ surface + new ROS 2 params + new threaded state on `UDPBridge`; 8 files +506/-4)
**Must-fix**: 1 | **Suggestions**: 2

### Findings
- [ ] (must-fix, Claude Adv. + Copilot Adv. cross-source) Thresholds latched at `on_configure`, README implies runtime-tunable. Add `OnSetParametersCallback` OR tighten README wording — `udp_bridge.cpp:144-147`, `README.md:31`
- [ ] (suggestion, Claude Adv.) `std::to_string(double)` in summary text emits 6 fractional digits — noisy in aggregator UIs. Use `std::fixed << std::setprecision(2)` — `udp_bridge.cpp:1667-1671`
- [ ] (suggestion, Copilot Adv.) No wrapper-side integration test for `diagnoseRemoteGiveups` (mutex discipline, map-update ordering, STALE path). Plan §4 deferred this as "verified at integration level"; flagging the coverage cost — `test/test_giveup_diagnostic.cpp` (gap)

### Notes
- Cross-source convergence on must-fix #1: both Claude Adv. (in-context sub-agent) and Copilot Adv. (CLI) independently flagged the parameter live-update gap. Strongest signal class per ADR-0013.
- Process miss: this review was run **post-push**, not pre-push. AGENTS.md Post-Task Verification §5 requires pre-push. Captured in the broader pattern tracked at workspace umbrella issue #480.
- One Copilot false positive dismissed: claim that `.agent/work-plans/issue-22/plan.md` is "review noise" — it's the canonical plan-first artifact per ADR-0013, ships with the PR by design.
- Static analysis skipped: `cpplint` / `cppcheck` not installed on this host. CI will run them if configured upstream.
- Plan §4's deferred "annunciator allowlist check" (Open Question 1) is still open — required during deployment integration, not blocking this PR.
