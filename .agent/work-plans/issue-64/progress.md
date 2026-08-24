---
issue: 64
---

# Issue #64 — Bench: three-bridge hub scenario for relay (pins the untested call sites and single delivery)

## Issue Review
**Status**: complete
**When**: 2026-08-23 22:53 -04:00
**By**: Claude Code Agent (Claude Sonnet)

**Issue**: #64
**Comment**: (best-effort post follows this entry; not recorded inline)
**Scope verdict**: well-scoped

### Actions
- [ ] Recommend the new scenario skip the veth/netem topology entirely (no `setup_topology()`/`PATHS`/`BOAT_NS` reuse) — use three plain DDS domains over loopback with distinct ports instead, since correctness of relay routing needs no link impairment. This sidesteps generalizing `_ns_prefix`/`_BOAT_NS_PID`, which are two-party by construction (single hardcoded boat namespace), and keeps the change additive rather than a shared-infrastructure rewrite that risks the other four scenarios.
- [ ] The "exactly one copy" assertion must include an upper bound, not just presence (`sub_count > 0`, the pattern `smoke`/`full-mix` use today). A duplicate-delivery bug would only be caught by comparing sub_count against pub_count with a tight tolerance (e.g. within ~10-20%) — a floor-only check would pass identically whether each remote got 1x or 2x delivery, reproducing exactly the kind of vacuous pass #57 warned against.
- [ ] "Sender receives nothing back" needs its own dedicated subscriber process on A's domain/topic (not inferred from absence of a log line), asserting a zero count over the run duration on the topic name a buggy echo would actually use.
- [ ] Harness validity guard: assert the scenario actually stood up three distinct bridges/domains (e.g. check each bridge's `bridge_info`/bag artifact exists and pub counts on all three sides are nonzero) before evaluating the relay assertions — otherwise a silently-degraded 2-bridge run (or a mid-run crash of the third node) could pass the "no echo" check vacuously.
- [ ] Configure the mismatched-label case exactly as the issue specifies (`remotes.<label>.name` differing from the label on at least one remote) — this is the precise shape of the #63 echo bug and a matching-label config cannot exercise it.
- [ ] Update `test/bench/README.md`'s scenario table and CI section with the new scenario once implemented (consequence of adding a bench scenario, per the existing table's pattern for `smoke`/`full-mix`/`range_degradation`/`subscriber_death`).

## Findings

### Scope Assessment

**Well-scoped?** Yes. The issue is narrowly targeted (one new `test/bench` scenario), the design doc (`doc/relay_design.md`) already anticipated and recommended exactly this follow-up when #51/#63 removed the `relay` opt-in flag, and the ask (three assertions: no echo, exactly-one-copy, hub-local-delivery) is concrete and boundable in one PR.

**Right repo?** Yes — `rolker/udp_bridge`, the same repo owning the relay feature (#51/#63) and the bench harness (#18).

**Dependencies**: Builds on #63 (relay label/name fix, merged) and the #18 bench harness. Not blocking on other open issues; #54 (resend residual) and #57's `xfail` invariants are unrelated to this scenario and should not be touched incidentally.

### Scope question — does the existing two-party topology generalize?

No, and it does not need to. `run_scenario.py`'s `setup_topology()` /
`PATHS` / `setup_veth_pair()` build exactly one veth-joined namespace pair:
`_BOAT_NS_PID` is a single global, and `_ns_prefix()` raises `ValueError` for
any `netns` argument other than the literal `BOAT_NS` sentinel. Extending
this cleanly to three parties would mean turning `_BOAT_NS_PID` into a
name-keyed dict, generalizing `_ns_prefix`, and turning `PATHS` into a
hub-and-spoke veth topology — a change to shared code that all four existing
scenarios depend on, which is exactly the kind of blast radius that caused
the 2026-08-22 netem-no-op bug (a bench that silently measured nothing for
years, per #57's lesson) to go unnoticed as long as it did.

That generalization is avoidable here: the ask is a *correctness* test
(does relay route exactly right), not a link-degradation test — no `tc
netem` impairment is needed on any of the three legs. `udp_bridge`'s
transport is plain UDP with configurable host:port per connection, and
domain separation between the three ROS graphs is achieved the same way
`OPERATOR_DOMAIN`/`BOAT_DOMAIN` already achieve it — distinct
`ROS_DOMAIN_ID` values, not physical network separation. So a three-bridge
hub scenario can be built as an independent function that launches three
bridges (A, hub, B) all in the *same* network namespace (still inside the
`unshare -Urn` wrapper every scenario already re-execs under, per `main()`),
each with its own domain and loopback port, with zero changes to
`setup_topology`/`PATHS`/`_ns_prefix`/`_BOAT_NS_PID`. This is a materially
smaller and lower-risk change than the "does it generalize" framing implied,
and should be called out explicitly in the plan so the implementer doesn't
default to extending the veth machinery just because the other three
scenarios use it.

### Would the proposed assertions actually catch a broken relay?

Partially, as specified — two gaps worth flagging before planning:

1. **"Exactly one copy" needs a ceiling, not just a floor.** Every existing
   scenario's pass condition is `sub_count > 0` (see `run_smoke`,
   `run_full_mix`). Applied unchanged to a hub scenario, a bug that
   delivers each message to a remote *twice* (e.g. from a stale
   label-vs-name mismatch reopening, or a future regression in the loop
   rule / rate-limiter exclusion) would still show `sub_count > 0` and pass.
   The assertion needs to compare `sub_count` against the publisher's own
   `pub_count` with a tolerance band tight enough to fail on 2x delivery
   (e.g. `0.8 * pub_count <= sub_count <= 1.2 * pub_count`), the same way
   `run_full_mix`'s docstring already reasons about tier ratios elsewhere in
   this harness.
2. **"Sender receives nothing back" needs an explicit subscriber**, not an
   absence of evidence. The cleanest implementation: A's domain also runs a
   `sub.py` instance listening on the topic name the hub would use if it
   echoed back to A (this follows directly from the mismatched-label
   config the issue already asks for — that's precisely the topic-routing
   path #63 fixed), and the assertion is `count == 0` at teardown, not
   "the process didn't crash" or similar.

Neither of these is a given from the issue text alone — the ask states the
assertions but not the implementation shape that keeps them non-vacuous.
Flagged as actions above for plan-task to make explicit.

### Principle Alignment

| Principle | Status | Notes |
|---|---|---|
| Test what breaks | OK | Directly on point — closes a real gap left open by #51/#63 (two unbound call sites, an untested single-delivery invariant), explicitly recommended by the relay design doc itself. |
| Only what's needed | Watch | The issue frames this as needing the existing veth/netns topology ("needs unshare -Urn like the other scenarios"); that's true only for the opt-in/reexec wrapper, not for veth+netem — see scope section above. Recommend the simpler loopback/three-domain approach over extending shared topology code. |
| A change includes its consequences | Watch | `test/bench/README.md`'s scenario table, CI section, and possibly the top-level `test/bench/` docstring inventory should be updated in the same PR once the scenario exists. |
| Improve incrementally | OK | Single new scenario function + one new test file, additive to existing bench infrastructure, doesn't touch #54/#57's still-open xfails. |
| Capture decisions, not just implementations | OK | The recommendation to do this work already lives in `doc/relay_design.md`'s "There is no relay parameter" section — the issue is traceable to that recorded rationale. |
| Enforcement over documentation | OK | This *is* the enforcement — a live multi-node test closing a gap three review rounds flagged as unit-test-only. |

### ADR Applicability

| ADR | Triggered | Notes |
|---|---||
| 0008 — Follow ROS 2 conventions | Mild | New pytest file under `test/bench/` should match the existing `test_range_degradation.py`/`test_subscriber_death.py` structure (opt-in gating, marker-line parsing, xfail conventions where relevant). |
| Others (0001-0007, 0009-0018) | No | Workspace-process ADRs; this is project-repo test code with no workspace-infra, git-bug, or deployment-mode surface. |

### Consequences

- `test/bench/README.md` scenario table + CI section need a new row once implemented.
- No parameter/topic/service changes, so no `.agents/review-context.yaml` or package README updates are implicated.
- No new principle or ADR is being introduced — no consequences map update needed there.

### Recommendations

- Keep the new scenario's topology independent of `setup_topology()`/`PATHS` (see Scope section) — this is the single highest-leverage recommendation for keeping the change proportionate and low-risk.
- Build the "exactly one copy" and "no echo" checks as count comparisons with explicit ceilings/zero-checks, not presence checks, per the gaps above.
- Consider naming the new pytest file `test_relay_hub.py` to match the existing `test_<scenario>.py` convention (`test_range_degradation.py`, `test_subscriber_death.py`).
- Since no impairment is needed, this scenario should run fast (seconds, not the tens-of-seconds-to-minutes budget of `range_degradation`/`subscriber_death`) — worth deciding whether it truly needs the `UDP_BRIDGE_BENCH_SCENARIOS=1` opt-in gate or can run as an unconditional `colcon test` case like `smoke` (gated only by the existing `unshare -Urn` availability skip). This is a plan-task-level call, not blocking review-issue.

### Open Questions (for plan-task)

- Should the third bridge's local subscriber (the "hub is also locally subscribed" check) use a distinct destination topic from what A and B each subscribe to, to keep the three counts independently distinguishable in one bag/log, or is per-process stderr counting (as today's `_parse_count`) sufficient?
- Does the mismatched-label remote need to be A, B, or the hub's view of one of them? (The #63 bug was in `on_configure`'s routing-table key, which is populated from the hub's own remote config — so the mismatch should be configured on the **hub's** entry for the spoke whose name differs from its label, not on the spoke's own self-configuration.) Worth confirming explicitly in the plan so the config isn't accidentally inert.

---
**Authored-By**: `Claude Code Agent`
**Model**: `Claude Sonnet`

## Plan Authored
**Status**: complete
**When**: 2026-08-23 23:02 -04:00
**By**: Claude Code Agent (Claude Sonnet)

**Plan**: `.agent/work-plans/issue-64/plan.md` at `45010c8`
**Branch**: feature/issue-64 at `45010c8`
**Phases**: single

### Open questions
- [ ] No open questions — plan is review-plan-ready.

## Plan Review
**Status**: complete
**When**: 2026-08-23 23:08 -04:00
**By**: Claude Code Agent (Claude Sonnet)

**Plan**: `.agent/work-plans/issue-64/plan.md` at `45010c8`
**PR**: PR-less
**Verdict**: changes-requested

### Findings
- [ ] (must-fix) Tolerance band `0.8x-1.2x` is too loose given the low sample count the default critical-tier config produces (~10 messages at 1Hz/10s): the lower bound tolerates 2 dropped messages on an unimpaired loopback link where loss should be near zero, and a single duplicated message (partial-duplication bug) stays inside the band on either side — neither honours the issue's "exactly one copy" ask. Tighten the floor (e.g. pub_count-1 or 0.95x) and/or raise the message count (longer duration or higher rate) so the band is statistically meaningful, and treat any loopback loss as a real signal rather than expected noise — `plan.md` Approach step 3.
- [ ] (must-fix) The echo-guard config's home is ambiguous/self-contradictory between the two Approach-step-1 bullets: bullet 1 describes the "inbound `/hub/from_a/heartbeat` → dest `/a/echo_from_hub`" topic as part of bridge_a's own `remotes.hub` block, but per `udp_bridge.cpp`'s push-config model (`source_param`/`destination_param` around udp_bridge/src/udp_bridge.cpp:583-592 — a bridge locally subscribes to `source` and forwards to `destination` on that remote) that topic has no local publisher named `/hub/from_a/heartbeat` on A's own domain, so a config entry under bridge_a's `remotes.hub` would create a dead subscription (`updateLocalSubscriptions()`'s `get_publishers_info_by_topic()` returns empty, never fires) rather than deliver anything. Bullet 2 correctly places it under the **hub's** `remotes.a` block ("carries A's two topics above"), which is the only placement that actually populates `subscribers_["hub/from_a/heartbeat"].remote_details["a"]` and gives a broken loop rule somewhere to deliver to. Fix the plan text so bullet 1 doesn't imply the inbound topic lives on bridge_a's own config — an implementer following bullet 1 literally would ship a decorative echo guard, exactly the failure mode scrutiny point 3 asked to rule out.
- [ ] (suggestion) The 3-bridge, multi-hop topology (A→hub relay to both hub-local and B) is structurally more startup-race-prone than the 2-party `run_smoke` pattern the plan says it will reuse (`run_scenario.py:701-724` — fixed 2s bridge-handshake sleep + 0.5s discovery sleep). `range_degradation`/`subscriber_death` instead use the more robust `wait_ready.py` gate specifically because a fixed sleep was how #57's "silently measuring nothing" bug went unnoticed. Consider extending the settle delays for the extra discovery hop (hub's `updateLocalSubscriptions()` 1Hz timer must independently pick up two remote-forwarding registrations) or adding a lightweight readiness probe before starting the counted publish window, rather than assuming the 2-party timing transfers unchanged.
- [ ] (suggestion) Step 2's framing that `launch_bridge`'s `check=True` lifecycle-transition calls are what closes the Issue Review's "did three bridges really launch" gap slightly overstates that mechanism's coverage: `check=True` (confirmed at `run_scenario.py:217-222`, used unconditionally by `launch_bridge`'s `ros2 lifecycle set` calls) only detects a bridge process failing to reach ACTIVE — it says nothing about whether two activated bridges actually exchanged packets. The real guard against a "silently degraded 2-bridge run" is the combination of the `b` and `hub_local` tolerance-band assertions (a dead/unreachable B or hub inevitably drives its sub_count to 0, well outside any reasonable band) plus the `pub_count > 0` check on A. Worth correcting the plan's rationale so a future reader doesn't assume `check=True` alone is sufficient and skip the delivery-count checks in some future edit.

### Verified sound
- Label/name mismatch (hub's `remotes.b` → `name: "boat_b"`) is exactly the #63 bug shape and is **not** rejected by `resolveRemoteIdentities` (`remote_identity.h`): not empty, fits `maximum_node_name_length`, not equal to the hub's own name, and does not collide with another resolved identity — confirmed by reading the validation function in full.
- `remote_nodes_` and `subscribers_[topic].remote_details` are keyed by the **resolved** identity (`identity_by_label.at(remote_name)`), not the label, per `udp_bridge.cpp:480-504` and `addSubscriberConnection` — matches the plan's premise.
- CI will indeed skip this scenario the same way `smoke` does: `.github/workflows/ci.yml` runs `ubuntu-24.04` + `container: image: ros:jazzy-ros-core` with no `--privileged` and no sysctl override, and `test_smoke.py`'s `_userns_available()` skip pattern (which the plan says `test_relay_hub.py` will reuse unmodified) is exactly the mechanism that will apply here too.
- Local publish on receipt (`decodeData` → `enqueuePublish`) is unconditional regardless of relay eligibility, so the hub-local delivery count and the relay-to-B path are genuinely independent signals, not the same check duplicated.

### Summary
Sound on the two verifiable structural claims (identity keying, CI skip behavior) and the overall scope/approach (loopback + distinct domains instead of generalizing the veth topology remains the right call). But the plan's crux mechanism — the tolerance band — is too permissive at the message volumes this scenario will actually produce, and Approach step 1's own text is internally inconsistent about where the echo-guard topic config lives, risking an inert guard if implemented as literally written. Both should be resolved before implementation.
