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
- [x] (must-fix, Claude Adv. + Copilot Adv. cross-source) Thresholds latched at `on_configure`, README implies runtime-tunable. Resolved by adding `OnSetParametersCallback` — `udp_bridge.cpp` `on_configure` registers the handle, validates non-negative thresholds, applies under `remote_nodes_mutex_` (matches reader's lock). `on_cleanup` resets the handle. Reader now samples thresholds inside the lock too.
- [x] (suggestion, Claude Adv.) `std::to_string(double)` in summary text replaced with `std::ostringstream` + `std::fixed << std::setprecision(2)`. Structured KeyValues (raw doubles) unaffected.
- [x] (suggestion, Copilot Adv.) Wrapper-side test coverage gap closed by refactor: consolidated two parallel maps into `std::map<std::string, GiveupRateState>` + extracted `stepGiveupDiagnostic` helper into `giveup_diagnostic.h`. Six new tests cover first-call init, two-call sequence (map-update ordering), counter reset across calls, zero-elapsed between steps, multi-state independence, and threshold-change-takes-effect-on-next-step.

### Notes
- Cross-source convergence on must-fix #1: both Claude Adv. (in-context sub-agent) and Copilot Adv. (CLI) independently flagged the parameter live-update gap. Strongest signal class per ADR-0013.
- Process miss: this review was run **post-push**, not pre-push. AGENTS.md Post-Task Verification §5 requires pre-push. Captured in the broader pattern tracked at workspace umbrella issue #480.
- One Copilot false positive dismissed: claim that `.agent/work-plans/issue-22/plan.md` is "review noise" — it's the canonical plan-first artifact per ADR-0013, ships with the PR by design.
- Static analysis skipped: `cpplint` / `cppcheck` not installed on this host. CI will run them if configured upstream.
- Plan §4's deferred "annunciator allowlist check" (Open Question 1) is still open — required during deployment integration, not blocking this PR.

## External Review
**Status**: complete
**When**: 2026-05-20 18:50 -04:00
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))

**PR**: #24 — 1 review (Copilot), 5 inline comments, 4 valid, 1 false positive
**CI**: all-pass (Agent, Prepare, Upload results, Cleanup artifacts)

### Actions
- [ ] Fix (udp_bridge.cpp:165–178): add `std::isfinite` per value and `warn_thresh <= error_thresh` cross-check in OnSetParameters validation; reject batch with actionable `result.reason`.
- [ ] Fix (udp_bridge.cpp:1716): clamp `elapsed_s` to `>= 0` for `window_s` KeyValue (`stat.add("window_s", std::max(0.0, elapsed_s))`).
- [ ] Fix (udp_bridge.h:180–185): update `diagnoseRemoteGiveups` docstring — drop the "last_giveup_*_ maps" reference, point to `giveup_rate_state_` / `GiveupRateState` from `giveup_diagnostic.h`.
- [ ] Fix (README.md:19–21): clarify DiagnosticStatus is the default operator surface; per-event DEBUG log requires enabling DEBUG severity to land in `/rosout` / bags.
- [ ] (Optional) Reply or dismiss Copilot's `as_double()` type-check thread — false positive (strict typing rejects mismatched `param set` server-side; `dynamic_typing` not enabled).
