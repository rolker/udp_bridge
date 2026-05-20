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
- [x] Fix (udp_bridge.cpp OnSetParameters): refactored to single-pass validator. Added `validateGiveupThresholds` to `giveup_diagnostic.h` — rejects NaN/inf, negatives, and `warn > error` with operator-actionable reason strings. Callback computes proposed_warn/proposed_error from batch + current under `remote_nodes_mutex_`, then validates once.
- [x] Fix (udp_bridge.cpp on_configure): same validator called after `get_parameter` so launch-line overrides (`-p resend_giveup_warn_rate_per_s:=…`) can't slip a bad pair past startup. Fails the lifecycle transition with `RCLCPP_ERROR` + `CallbackReturn::FAILURE` — scope expansion beyond Copilot's flagged surface, approved by user.
- [x] Fix (udp_bridge.cpp:1716 → now 1740): `stat.add("window_s", std::max(0.0, elapsed_s))` so a backward clock jump (NTP correction, sim-time rewind) doesn't surface a negative published window.
- [x] Fix (udp_bridge.h diagnoseRemoteGiveups docstring): dropped stale `last_giveup_*_` reference, points at `giveup_rate_state_` / `GiveupRateState` / `stepGiveupDiagnostic`.
- [x] Fix (README.md diagnostic surface paragraph): DiagnosticStatus called out as the default surface; per-event DEBUG log capture now documented as requiring `--ros-args --log-level udp_bridge:=DEBUG` or `RCUTILS_LOGGING_SEVERITY_THRESHOLD=DEBUG`.
- [x] Test coverage: +9 gtests in `test_giveup_diagnostic.cpp` for the validator — defaults pass, equal warn=error passes, 0/0 passes, negative warn/error rejected per side, NaN warn/error rejected per side, +inf rejected (finite check), inverted pair rejected with both values in the reason string. 65 → 74 tests, all passing.
- [ ] (Optional) Reply or dismiss Copilot's `as_double()` type-check thread — false positive (strict typing rejects mismatched `param set` server-side; `dynamic_typing` not enabled). Deferred to user.

### Notes
- One scope expansion beyond Copilot's flagged surface: launch-line overrides bypass the OnSetParameters callback. Same bug class (NaN/negative/inverted pair → silently broken diagnostic), closed at the launch entry point too. Authorized via AskUserQuestion before committing.

## External Review (round 2)
**Status**: complete
**When**: 2026-05-20 22:00 -04:00
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))

**PR**: #24 at `87e8b6e` — Copilot re-reviewed after the round-1 push. 3 new inline comments; round-1's 5 comments are now stale (concerns addressed by the round-1 push).
**CI**: all-pass (Agent, Prepare, Upload results, Cleanup artifacts)

### Actions
- [ ] Fix (udp_bridge.cpp:1703): drop the `:1271` line-number citation in the `diagnoseRemoteGiveups` lock comment — line 1271 is unrelated (TopicStatisticsArray code); the actual lock-order rationale lives at lines 1371–1374. Either describe the invariant in words or point at `remote_node.h`'s `resendGiveupCount()` declaration.
- [ ] Decide scope (udp_bridge.cpp:152): Copilot flagged that `declare_parameter` inside `on_configure` throws on a second cleanup→configure cycle. The same pattern applies to every existing `declare_parameter` in this node (e.g., `maximum_packet_size` at line 115). Reconfigure was broken pre-PR. Options: (a) add `has_parameter()` guards to just my new params (cosmetic; doesn't fix reconfigure since line 115 throws first), (b) file a separate issue for repo-wide cleanup, (c) leave it (matches existing pattern). Surfacing to user.
- [ ] (Optional) Resolve README `||` thread on GitHub — false positive (table at lines 30–33 uses single pipes; Copilot's claim does not match the source).
- [ ] (Optional) Resolve 5 stale round-1 threads on GitHub — all addressed by the round-1 push.
