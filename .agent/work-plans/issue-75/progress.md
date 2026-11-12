---
issue: 75
---

# Issue #75 — Field import: runtime-applied connection tunables + per-topic rate caps (2026-08-25 Appledore)

## Local Review (Pre-Push)
**Status**: complete
**When**: 2026-08-25 23:44 -04:00
**By**: Claude Code Agent (Claude Opus 5 (1M context))
**Verdict**: changes-requested

**Branch**: feature/issue-75 at `9a7d54a`
**Mode**: pre-push
**Depth**: Deep (reason: user-forced; 17 files / +2101 -112, lifecycle + concurrency + new ROS parameter)
**Must-fix**: 6 | **Suggestions**: 8
**Round**: 1 | **Ship**: continue — the field's own review was lost (`--no-progress`); this is the branch's first surviving review and it found correctness defects, not polish.

Specialists: Static Analysis (pre-commit green over origin/jazzy..HEAD, incl. cmake-lint + yamllint), Governance, Claude Adversarial Lens A + Lens B. Copilot/local not run. Build + tests re-verified independently: 290 tests, 0 failures.

### Findings
- [x] (must-fix) Connection ids keyed RAW in connection_rates / TunableTarget / drop records but canonicalised to 7 chars everywhere else — invisible shedding, up to 2x the configured cap, silent cross-retune. Cross-confirmed by 3 independent passes. Latent on the current fleet (vpn/wifi/cell); live in the repo's own example_params.yaml ("telemetry") — `src/udp_bridge.cpp:1758`, `include/udp_bridge/destination_selection.h:186`
- [x] (must-fix) A per-topic cap can never be REMOVED by a re-configure: addSubscriberConnection's "0 retains" is correct for the wire paths but wrong for on_configure, where 0 is a real documented value. `param get` reads 0/unlimited while the selector still sheds at the old cap — the exact accepted-reads-back-inert defect this branch exists to remove — `src/udp_bridge.cpp:1771`
- [x] (must-fix) Connection-level maximum_bytes_per_second narrows int64->int with no range check: a YAML 3000000000 goes negative, the `> 0` guard fails, and the link silently falls back to default_rate_limit 50000 B/s with no warning. The same PR adds full clamp+WARN for the per-topic cap 120 lines below and advertises 0-4294967295 in the README — `src/udp_bridge.cpp:569`
- [x] (must-fix) on_shutdown is not symmetric with on_cleanup: the parameter callback stays registered into FINALIZED, so a param set still mutates live Connections; and runtime_tunables_ is declared AFTER the handle, so it is destroyed first while the callback can still read it — `src/udp_bridge.cpp:898`, `include/udp_bridge/udp_bridge.h:593`
- [x] (must-fix) doc/relay_design.md was missed entirely: documented call signature no longer compiles (3rd arg is message_bytes, not source_node); the drop-reason contract is false on both halves now that rateLimited() sums TopicRateLimited and rate_limited_by_topic_cap exists; the "single implementation of the period rule" section omits that it is now also the byte cap — `doc/relay_design.md:71,465,494`
- [x] (must-fix) Four behaviours untested, all regression-prone: target-gone refusal, cleanup->configure tunables rebuild, RelayDropReason::TopicRateLimited accounting, and the callback-registration ordering — documented as load-bearing in three places, enforced by nothing, and its failure mode is on_configure throwing past the socket bind where rclcpp_lifecycle swallows it — `src/udp_bridge.cpp:1877`, `:503`, `include/udp_bridge/relay_drops.h`
- [x] (suggestion) A byte-cap shed skips periods.insert(), so capping one connection transiently perturbs an UNCAPPED sibling's send timing — `include/udp_bridge/destination_selection.h:278`
- [x] (suggestion) Drop records carry fragment_count 0 and dilute average_fragment_count, the figure an operator uses to size maximum_packet_size — `include/udp_bridge/destination_selection.h:177`
- [x] (suggestion) A parameter left over from a previous configure (connection removed from connections_list) is accepted and does nothing; the not-registered rejection covers removed-forwarding but not removed-from-config — `src/udp_bridge.cpp:1817`
- [x] (suggestion) README runtime table gives the floor as "finite double >= 0" but the code also refuses > kMaxTunableFloat — `README.md:238`
- [x] (suggestion) The config-file over-range clamp for the per-topic cap is in code but in none of the three docs, which mention only the negative->0 case — `src/udp_bridge.cpp:704`
- [x] (suggestion) Only the runtime negative-rejection is tested; the config-side negative->warn+0 path is not, and the asymmetry between the two entry points is this branch's central validation policy — `src/udp_bridge.cpp:701`
- [x] (suggestion) example_params.yaml's four connection tunables do not mention they are now live-tunable, while the new per-topic comment does — `config/example_params.yaml:100`
- [x] (suggestion) addSubscriberConnection's "0 retains" rule is called out in a test comment but never exercised; a service re-subscribe silently wiping an operator's cap is what it exists to prevent — `src/udp_bridge.cpp:1763`

### Notes
- The field review's "8 must-fix" list is unrecoverable (ran with --no-progress). This review is independent and its findings are new; the count landing near 8 is coincidence, not corroboration.
- Lens A specifically CLEARED the risk the field flagged hardest — callback registration order and teardown symmetry across on_configure/on_cleanup, plus token-bucket correctness (sustained rate, oversized message, backwards clock, first use) and the single-implementation claim. Lens B independently cleared the concurrency claim (bounded arithmetic inside the existing subscribers_mutex_ scope, nothing held across sendto), lock ordering, and all-or-nothing batching. B's on_shutdown finding is not a contradiction of A: A examined configure/cleanup, B examined shutdown and destruction.
- CONCURRENT WORK HAZARD: udp_bridge#52 (branch feature/issue-52, 13 commits, unpushed, updated today) rewrites the AIMD control law these four tunables tune, and adds 78 lines to resend_constants.h — the file this branch hoists kMaxLinkHeadroomFraction into precisely so the setter clamp and the validation cannot drift. Trial merge: textual conflicts in 3 docs only; the code auto-merges, which is what makes the semantic drift dangerous. Issue #75's body wrongly states #52's fix "merged 2026-08-22". Sequence #52 first, then rebase this and re-check the ranges.
- Open #66 (on_cleanup never closes the socket; CLEANUP->CONFIGURE exit(1)s on EADDRINUSE) touches the same lifecycle path this branch modifies.

## Implementation
**Status**: complete
**By**: Claude Code Agent (Claude Opus 5 (1M context))

**What**: merged the now-merged #52 (PR #77) into this branch and resolved the
semantic drift the round-1 review predicted. Merge commit, not a rebase — the
field SHAs (tip `9a7d54a`) are the provenance link issue #75 records.

- `link_headroom_fraction` runtime tunable REMOVED. #52 round 3 retired the
  parameter *and* undeclared it, deleting `Connection::setLinkHeadroomFraction`;
  the merged tree called a setter that no longer exists. `TunableKind::
  LinkHeadroomFraction`, `validateLinkHeadroomFraction` and the hoisted
  `kMaxLinkHeadroomFraction` are gone with it. This supersedes
  `rebase-notes-issue-52.md`, which recorded the round-2 shape
  ("stays DECLARED, inert") and correctly warned round 3 would overwrite it.
- `admission_refractory_period_seconds` ADDED as the fifth runtime tunable
  (operator-approved scope addition). #52 wrote the expectation down and it is
  the parameter RCA item D names as what blocked live mitigation on 2026-08-25.
  Runtime path REFUSES a base above the 60 s ceiling where configure CLAMPS —
  the branch's central validation asymmetry.
- `AdmissionControl.FloorRaisedAtRuntimeLiftsTheCapNoFurtherThanTheLimit`
  drifted while merging clean. Fixed, and the fix exposed an operator-facing
  interaction now documented: `updateAdmissionControl` returns at the refractory
  gate BEFORE the floor clamp, so a floor raised mid-collapse is inert for up to
  60 s. Reads exactly like the defect this branch removes; pinned both ways.

**Verification**: build clean; full suite **332 tests, 0 failures, 14 skipped**.
`pre-commit run --from-ref origin/jazzy --to-ref HEAD` all Passed, **including
`cmake-lint` and `yamllint`** — the issue's "pre-commit did not run" blocker is
discharged. The issue body's "UNCOMPILED, no test ever run" is stale as of the
round-1 review.

### Notes for the fix pass
- The round-1 review's **6 must-fix findings are all still open** and unaddressed.
- Their **line numbers have shifted** — re-locate by symbol, not by line.
- Nothing in the merge addressed any of them. Finding 4 (`on_shutdown` not
  symmetric with `on_cleanup`; `runtime_tunables_` declared after the callback
  handle) and finding 6 (callback-registration ordering untested) touch the
  configure/teardown path the merge also moved through — recheck them against
  the merged code, not the reviewed code.
- Finding 5 (`doc/relay_design.md` missed entirely) is untouched by the merge.

## Implementation
**Status**: complete
**When**: 2026-09-03 13:49 -04:00
**By**: Claude Code Agent (Claude Opus)

**Branch**: feature/issue-75 at `f2a88f2`
**Addressed**: the `## Local Review (Pre-Push)` of 2026-08-25 23:44 -04:00 (round 1,
branch at `9a7d54a`) — all 6 must-fix and all 8 suggestions. None deferred.
**Commits**: `db15c94` `5e6a581` `825ede3` `7d8a1d9` `bf20894` `3007639` `b60d0eb`
`261f81a` `d3655d6` `0792564` `f2a88f2`

Re-located every finding by symbol, not by line: the #52 merge had moved
all of them. Two of the six needed re-checking against the merged code, as
the previous entry's notes required, and both survived it — finding 4's
`on_shutdown`/declaration-order defect and finding 6's untested
callback-registration ordering are still real after the merge. Nothing
touched `link_headroom_fraction`; it is retired and stays retired. The
fifth runtime tunable added by the merge,
`admission_refractory_period_seconds`, is carried through every fix and is
the probe the new ordering test uses.

### Actions
- [x] (must-fix) Connection ids keyed raw in `connection_rates` / `TunableTarget` /
  drop records — `addSubscriberConnection` and `removeSubscriberConnection` now
  canonicalize on the way in, matching `RemoteNode::connection()`, and `on_configure`
  records the canonical id in each `TunableTarget`. `topicRateInfoForTest` deliberately
  does NOT canonicalize, so a test can pin which key the table is built under —
  `src/udp_bridge.cpp`, `include/udp_bridge/connection_tunables.h` (`db15c94`)
- [x] (must-fix) A per-topic cap could never be removed by a re-configure — the cap
  argument is `std::optional<uint32_t>`: absent retains (every wire/service caller, and
  the test hook offers no way to pass anything else), present is authoritative
  including 0 (`on_configure`) — `src/udp_bridge.cpp`, `include/udp_bridge/udp_bridge.h`
  (`5e6a581`)
- [x] (must-fix) Connection-level `maximum_bytes_per_second` narrowed int64→int with no
  range check — read as int64 and clamped at both ends with a WARN, mirroring the
  per-topic cap. Pinned by a test on the exact reported value: 3000000000 must reach the
  Connection, not fall back to the 50000 default — `src/udp_bridge.cpp` (`825ede3`)
- [x] (must-fix) `on_shutdown` not symmetric with `on_cleanup` — the callback handle is
  reset and the registry cleared there too, and the handle is now declared AFTER
  `runtime_tunables_` so `~UDPBridge` destroys it first. The ordering is documented as
  load-bearing where a tidy-up would reach for it — `src/udp_bridge.cpp`,
  `include/udp_bridge/udp_bridge.h` (`7d8a1d9`)
- [x] (must-fix) `doc/relay_design.md` missed entirely — call signature corrected, the
  drop-reason contract rewritten around both rate limits (`rateLimited()` sums them,
  `topicRateLimited()` is the cap half, `rate_limited_by_topic_cap` is its row), and the
  "single implementation" section now covers the byte cap as well as `period`. Also
  records the gate ordering and that the relay probe stays may-send — `doc/relay_design.md`
  (`3007639`)
- [x] (must-fix) Four behaviours untested — target-gone refusal, cleanup→configure
  registry rebuild, `RelayDropReason::TopicRateLimited` accounting, and the
  callback-registration ordering. The ordering test supplies a config value the runtime
  validator would refuse (`admission_refractory_period_seconds` above the 60 s ceiling)
  as a NodeOptions override, so `on_configure` performs the declaration itself: the
  transition must succeed with the value clamped, and the same value must be refused once
  configure has returned. Two `UDP_BRIDGE_BUILD_TESTING` hooks support this —
  `test/test_runtime_tunables.cpp`, `test/test_relay_queue.cpp`,
  `test/test_topic_rate_caps.cpp` (`bf20894`)
- [x] (suggestion) A byte-cap shed skipped `periods.insert()`, so capping one connection
  changed when an uncapped sibling of the same period was sent to — group membership is
  now decided by the `period` rule alone — `include/udp_bridge/destination_selection.h`
  (`261f81a`)
- [x] (suggestion) Drop records diluted `average_fragment_count` — averaged over the
  records that carry a fragment count, 0 when none does — `src/statistics.cpp` (`d3655d6`)
- [x] (suggestion) A tunable left over from a previous configure was accepted and did
  nothing — `stale_tunables_` accumulates the keys each cycle drops and a set of one is
  refused with a reason. Accumulating rather than replacing is load-bearing: by the time
  `on_configure` runs, `on_cleanup` has usually already emptied the registry —
  `src/udp_bridge.cpp` (`0792564`)
- [x] (suggestion) README runtime table gave the floor's range as "finite double >= 0" —
  now `[0, 3.4028235e38]`, with the float-representability reason — `udp_bridge/README.md`
  (`b60d0eb`)
- [x] (suggestion) The per-topic cap's config-file over-range clamp was in no document —
  recorded in the README, `config/example_params.yaml` and `.agents/README.md`, along with
  the connection-level clamp added here (`b60d0eb`)
- [x] (suggestion) The config-side negative→warn+0 path was untested —
  `ConfiguredNegativeCapWarnsAndBecomesNoCap` and `ConfiguredOverRangeCapIsClampedNotWrapped`
  (`825ede3`)
- [x] (suggestion) `example_params.yaml` said "live-tunable" on two of the four connection
  tunables — now on all four (`b60d0eb`)
- [x] (suggestion) The "0 retains" rule was never exercised — now pinned as
  `AReadvertiseDoesNotWipeTheCap`, in the shape finding 2 gave it: a re-advertise carries
  no cap field and must leave the cap alone, which is what makes "an engaged value is
  authoritative" safe (`5e6a581`)

### Notes
- Out of scope, found while working and NOT fixed: two configured connection ids that
  agree in their first `maximum_connection_id_size - 1` characters are now correctly ONE
  routing entry, but nothing REJECTS such a collision at configure time. That is a
  separate hazard (the operator's two configured links silently become one) and wants its
  own issue.
- `.agents/README.md`'s live-tunable list was stale after the #52 merge — it named the
  retired `link_headroom_fraction` and omitted `admission_refractory_period_seconds`.
  Corrected in `b60d0eb`.
- **Verification**: build clean; full suite **352 tests, 0 failures, 14 skipped** (baseline
  332/0/14 — 20 new tests, no test removed or weakened).
  `pre-commit run --from-ref origin/jazzy --to-ref HEAD` all Passed, including cmake-lint
  and yamllint. Not pushed — the host pushes.

## Local Review (Pre-Push)
**Status**: complete
**When**: 2026-09-03 14:01 -04:00
**By**: Claude Code Agent (Claude Opus)
**Verdict**: changes-requested

**Branch**: feature/issue-75 at `cc4b25e`
**Mode**: pre-push
**Depth**: Deep (reason: 22 files / +3622 -150, lifecycle + concurrency + ROS parameter surface, merged with #52's control-law rewrite)
**Must-fix**: 4 | **Suggestions**: 11
**Round**: 2 | **Ship**: continue — must-fix is not rising (6 -> 4) and the round-1 fix pass is genuinely complete, but two of the four are correctness/design questions (a newly-introduced rate-limit bypass, and a control-law decision on the floor) rather than mechanical fixes, so they warrant another independent read after they land.

Specialists: Static Analysis (pre-commit green over origin/jazzy..HEAD incl. cmake-lint + yamllint), Governance, Claude Adversarial Lens A + Lens B. Copilot/local not run. Build + tests re-verified independently: clean build, **352 tests, 0 errors, 0 failures, 14 skipped**.

### Findings
- [x] (must-fix) A saturated per-topic cap defeats an uncapped same-period sibling's `period` limit: a shed connection is never stamped, so it is due on EVERY message and re-opens its period cohort every message, pulling a later-iterating sibling through `periods.count()>0` past its own period check — the sibling then sends at the full publish rate on the link the cap was meant to protect. New on this branch; `AShedConnectionStillOpensItsPeriodGroup` makes one selector call and cannot see it. Cross-confirmed by both adversarial lenses. Fix shape: a "period satisfied" stamp distinct from `last_sent_time` — `include/udp_bridge/destination_selection.h:287`
- [x] (must-fix) The floor/refractory interaction was documented rather than fixed, and the documentation is built on an incomplete model: the floor is read ONLY in the congested branch, so on the clean recovery path (which is where a collapsed, quiesced link sits) a raised floor binds on NO sample at all — not "inert for up to 60 s". `doc/admission_control_design.md:418` ("takes effect on the next admission sample") and `:432` are false there, and `connection.h:342`'s "absolute" floor is not absolute. The refractory workaround the doc offers does not help on that path. `FloorRaisedAtRuntimeLiftsTheCapNoFurtherThanTheLimit` drives only congested samples. One fix closes both holes: apply `max(effective, min(floor, data_rate_limit_))` unconditionally before the refractory `return` — a lower bound, not a new AIMD decision, and `setRateLimit`/`setAdmissionRefractoryPeriodSeconds` already touch live control state on set — `src/connection.cpp:437`, `doc/admission_control_design.md:418`
- [x] (must-fix) The new per-topic `maximum_bytes_per_second` is declared INTEGER via `declareIfMissing` with no override coercion, so a YAML `1500000.0` throws `InvalidParameterTypeException` out of `on_configure` AFTER the socket bind, where rclcpp_lifecycle swallows it: silently UNCONFIGURED holding the port, a restart loop under the shipped `respawn=True`. The hazard class this file documents at `:363-369`; #52 built the mirror `declareDoubleIfMissing` and there is no int counterpart. One `declareIntIfMissing` also covers the pre-existing `:668`, `:877`, `:902` — `src/udp_bridge.cpp:939`
- [x] (must-fix) The verified-parameter table contradicts the code and itself: it says the runtime path applies "the same 60 s clamp" to `admission_refractory_period_seconds`, but the runtime path REFUSES above 60 — the branch's central validation policy, stated correctly in three other places including the same file's own live-tunable block. Agent-facing guide — `.agents/README.md:109`
- [x] (suggestion) "A batch ... applies all of them or none" holds only via `set_parameters_atomically`; `ros2 param set` and `ros2 param load` invoke the callback per parameter. Cross-confirmed by Lens A and Governance — `udp_bridge/README.md:237`
- [x] (suggestion) `average_fragment_count` changed meaning (averaged over records that carry a count) with the rationale only in a code comment; `TopicStatistics.msg`, the README and `.agents/README.md` document the field's semantics nowhere, and `test/bench/README.md:128,218` derives `T_fragment_floor` from it — `src/statistics.cpp:96`
- [x] (suggestion) A runtime connection `maximum_bytes_per_second` of 0 logs "applied: 0" while the link drops to 50000 B/s; log `connection->rateLimit()` (the value in force) instead — `src/udp_bridge.cpp:2380`
- [x] (suggestion) Lowering a per-topic cap at runtime drains the existing overdraft at the NEW cap, so a 400 kB/s -> 20 kB/s change silences the topic ~20 s — an outage bounded by nothing the operator can see in the parameter they set — `include/udp_bridge/destination_selection.h:130`, `src/udp_bridge.cpp:2373`
- [x] (suggestion) Two connection ids agreeing in their first 7 chars register two parameter names against one `TunableTarget`: last write wins and the loser reads back a value not in force — the invariant `connection_tunables.h:44` calls load-bearing. The branch's notes flagged the routing half; the parameter half is new here. Cheapest fix is a duplicate-canonical-id check above the socket bind — `src/udp_bridge.cpp:637`, `:857`
- [x] (suggestion) A per-topic cap shedding every destination is invisible on the local-origin path: the relay path counts `TopicRateLimited` and surfaces `rate_limited_by_topic_cap`, `callback()` has only an RCLCPP_DEBUG. The #60 shape (indicators healthy, data not flowing) with a configured rather than a failed cause — `src/udp_bridge.cpp:1336`
- [x] (suggestion) Re-configure gap: the parameter callback is unregistered in `on_cleanup` and re-registered at the END of `on_configure`, after DNS resolution and DDS entity creation. A set landing in between is accepted, reads back, and changes nothing — the branch's own defect, inside its own transition — `src/udp_bridge.cpp:1036`, `:1121`
- [x] (suggestion) `ParameterDescriptor` ranges (ADR-0008) were explicitly assigned to this branch by `rebase-notes-issue-52.md:85,139` ("The descriptor work lands here") and did not land, nor is the re-deferral recorded. Four newly-live tunables with hard numeric ranges is a stronger case than the one #52 deferred — add them, or record the re-deferral with an issue number **(deferred: a descriptor range makes rclcpp REJECT an out-of-range value at DECLARE time, which inverts this node's deliberate clamp-at-configure policy and would throw out of `on_configure` into the swallow — a field-behaviour decision, not an annotation. Reason recorded at `connection_tunables.h`; no issue number, this agent has no GitHub access — the host should file one)**
- [x] (suggestion) The read-back invariant ("an accepted value reads back as the value in force") is exact for 3 of 5 parameters: the store holds `double` while `Connection` holds `float` for `resend_budget_fraction` and the floor. One exception is already carved out; this deserves the same half-sentence — `include/udp_bridge/connection_tunables.h:37`, `udp_bridge/README.md:227`
- [x] (suggestion) The live-tuning note documents refusal for a connection "no longer registered" but not the distinct, equally reachable per-topic target-gone refusal (a forwarding removed at runtime via `remove_subscribe`/`remove_advertise`) — `udp_bridge/README.md:238`, `src/udp_bridge.cpp:2246`
- [x] (suggestion) Mutating live state from an `OnSetParameters` (pre-set) callback is correct only while this node registers exactly one such callback; a later-registered one returning false would leave Connections mutated while the store holds the old value. The rclcpp-sanctioned split is validate-on-set / mutate in `add_post_set_parameters_callback`. Worth a note at the callback, not a change today — `src/udp_bridge.cpp:2118`

### Notes
- **On the adjudication the round asked for**: documenting the floor/refractory interaction rather than fixing it was NOT the right call. The doc, the design-doc section, `example_params.yaml` and the test are all well executed, but they describe a delay bounded by the refractory window, and the actual behaviour on the clean recovery path is unbounded inertness. Lens B independently CLEARED this finding on the narrower model the documentation presents — which is itself evidence that the framing misleads a careful reader, not just an operator under pressure.
- Two of the four must-fixes (1 and 3) are exactly the "#52 merged clean while the semantics drifted" hazard round 1 predicted, in its third and fourth instance: the byte-cap gate composes badly with the period-group rule the same commit reordered, and the new integer parameter reproduces the declaration hazard #52 had just built the mirror remedy for.
- Round-1 verification: all 6 must-fix and all 8 suggestions confirmed addressed. No test removed or weakened — the `test_relay_routing.cpp` edits are mechanical adaptations to the selector's new signature and return type.
- Instruction-update CANDIDATES for `.agent/knowledge/ros2_development_patterns.md` (proposals for the operator, not applied): (a) an `OnSetParameters` callback fires on `declare_parameter` too, so registering it before a lifecycle node's declarations lets a YAML override throw past the socket bind; (b) resolve runtime tunables through a map built at declaration time, never by parsing the dotted parameter name (a path segment can itself contain '.'); (c) clamp-at-configure / refuse-at-runtime as a reusable validation policy, documented at BOTH entry points or it reads as a bug; (d) "accepted, reads back, changes nothing" as a named defect class for any node that reads parameters once in `on_configure`.
- Not run this round, per the invocation: Copilot Adversarial (publish-time decision) and the local-model specialist.

## Implementation
**Status**: complete
**When**: 2026-09-03 14:35 -04:00
**By**: Claude Code Agent (Claude Opus)

**Branch**: feature/issue-75 at `75a1467`
**Addressed**: `## Local Review (Pre-Push)` (2026-09-03 14:01 -04:00, verdict changes-requested, branch at `cc4b25e`) — 4 must-fix + 11 suggestions
**Commits**: `b647fb3` `cc30b92` `c8631d0` `85915ca` `9801d20` `3c8a5bf` `86b79aa` `75a1467`

**Build + tests**: clean build; **360 tests, 0 errors, 0 failures, 14 skipped** (baseline 352 + 8 new).
`test/test_admission_field_replay.cpp` — #52's acceptance gate for the 2026-08-25
Appledore fix — is untouched and green: the floor change is a lower bound applied
where the congested branch already clamped at the same value, so no replay result
moved. `pre-commit run --from-ref origin/jazzy --to-ref HEAD`: all hooks pass.

### Actions

Must-fix:

- [x] Saturated per-topic cap re-opening its period cohort — `include/udp_bridge/destination_selection.h:287`. Added `ConnectionRateInfo::period_satisfied_time`, stamped whenever the period rule selects a connection (sent **or** shed); the due test now requires the period to have elapsed against both stamps, so `last_sent_time` keeps its meaning (actual sends only) and existing callers/tests that set it are unaffected. New `TopicRateCaps.ASaturatedCapDoesNotReopenItsPeriodGroupEveryMessage` drives the multi-message sequence the single-call `AShedConnectionStillOpensItsPeriodGroup` could not see — `b647fb3`
- [x] The admission floor, fixed rather than documented — `src/connection.cpp:437`. `max(effective, min(floor, data_rate_limit_))` is now applied at the top of `updateAdmissionControl`, ahead of the refractory gate and of both AIMD branches. A lower bound, not a decision: it never lowers the cap, never touches the refractory bookkeeping, never substitutes for the decrease or the additive recovery. This closes both holes the review found — inert inside an open window, and (the worse one) never read at all on the clean recovery path, where a collapsed quiesced link actually sits. `FloorRaisedAtRuntimeLiftsTheCapNoFurtherThanTheLimit` was rewritten (it pinned the in-window inertness as if it were the contract) and `FloorRaisedAtRuntimeBindsOnTheCleanRecoveryPath` added for the case that had no test — `c8631d0`
- [x] The three surfaces that documented the bounded-delay model are CORRECTED, not extended: `doc/admission_control_design.md` "Tuning it while it is running" (the whole "a raised floor waits out an open refractory window" bullet is replaced, and says plainly that an earlier draft described an unbounded inertness as a bounded delay), `config/example_params.yaml` (both the floor entry and the refractory entry's cross-reference), and `include/udp_bridge/connection.h`'s floor contract at the setter and the member — `c8631d0`
- [x] INTEGER-declared parameters with no override coercion — `src/udp_bridge.cpp:939`. Added `declareIntIfMissing`, the mirror of #52's `declareDoubleIfMissing`, and applied it to the new per-topic `maximum_bytes_per_second` **and** to every other integer parameter the node declares (`port`, `maximum_packet_size`, `publish_queue_max_bytes`, `relay_queue_max_bytes`, connection `port`/`return_port`/`maximum_bytes_per_second`, per-topic `queue_size`/`history_depth`) — the review named `:668`, `:877`, `:902`, and the hazard is identical for the rest, all of which declare after the socket bind. A non-integral double is rounded with a WARN; an unrepresentable one is an ERROR falling back to the default; any other type is still refused. Four tests mirroring the existing double-literal set — `cc30b92`
- [x] Verified-parameter table contradiction — `.agents/README.md:109`. The runtime path REFUSES above 60 s where the configure path clamps; the table now says so, in the branch's own words. The same edit records the two behaviour changes above — `85915ca`

Suggestions (all actioned; one deferred with its reason recorded):

- [x] Batch atomicity holds only via `set_parameters_atomically` — `udp_bridge/README.md:237` — `75a1467`
- [x] `average_fragment_count` semantics documented where the field lives (`TopicStatistics.msg`) and referenced from both READMEs; `test/bench/README.md`'s `T_fragment_floor` derivation named — `src/statistics.cpp:96` — `75a1467`
- [x] Log the value IN FORCE (`connection->rateLimit()`), so a runtime `maximum_bytes_per_second` of 0 no longer reports "applied: 0" while the link drops to 50000 B/s — `src/udp_bridge.cpp:2380` — `3c8a5bf`
- [x] Lowering a per-topic cap drains the overdraft at the NEW cap: WARN naming the expected silence when it applies, plus the documentation at the token bucket — `include/udp_bridge/destination_selection.h:130`, `src/udp_bridge.cpp:2373` — `3c8a5bf`
- [x] Two connection ids sharing a canonical id now FAIL `on_configure`, checked above the socket bind so the remedy is edit-and-reconfigure; two tests — `src/udp_bridge.cpp:637`, `:857` — `9801d20`
- [x] A per-topic cap shedding every destination on the local-origin path is now a throttled WARN naming the cap, with `period` staying at DEBUG — the #60 shape with a configured cause — `src/udp_bridge.cpp:1336` — `3c8a5bf`
- [x] Re-configure gap closed: after registering the OnSetParameters callback, `on_configure` re-applies the current store values through the same validated path, so a set that landed during the transition is picked up rather than accepted-and-inert — `src/udp_bridge.cpp:1036`, `:1121` — `86b79aa`
- [x] `ParameterDescriptor` ranges (ADR-0008) — **(deferred: a `FloatingPointRange`/`IntegerRange` makes rclcpp REJECT an out-of-range value at DECLARE time, which inverts this node's deliberate clamp-at-configure policy and would surface as an exception thrown out of `on_configure` and swallowed — a bridge left unconfigured over a typo, the exact failure `declareDoubleIfMissing`/`declareIntIfMissing` exist to prevent. Adopting descriptors means deciding whether the configure path should start refusing what it clamps: a field-behaviour change needing its own issue and decision. Reason recorded in `include/udp_bridge/connection_tunables.h` so it is not re-deferred silently a third time. NO issue number: this agent has no GitHub access — the host should file one.)** — `75a1467`
- [x] Read-back invariant is exact to `float` precision for `resend_budget_fraction` and the floor; said at `connection_tunables.h:37` and in the README table — `75a1467`
- [x] The per-topic target-gone refusal (forwarding removed at runtime by `remove_subscribe`/`remove_advertise`) documented alongside the connection-no-longer-registered one — `udp_bridge/README.md:238` — `75a1467`
- [x] Note at `applyRuntimeParameters` that mutating from a PRE-set callback is correct only while this node registers exactly one, and that the mutation half moves to `add_post_set_parameters_callback` the day a second is added — `src/udp_bridge.cpp:2118` — `86b79aa`

### Notes

- One defect was introduced and caught during this pass, worth recording because it
  segfaulted three test binaries before the first commit: `rclcpp::Parameter::as_string_array()`
  returns a reference INTO the `Parameter`, so a range-for directly over
  `get_parameter(name).as_string_array()` iterates a destroyed vector. Fixed by
  binding the array to a local; the surrounding code already did this by copy.
- The four `.agent/knowledge/ros2_development_patterns.md` candidates in the review's
  Notes were NOT applied — that file is an instruction file and Ask-First under
  AGENTS.md. They remain proposals for the operator.
- Scope note on must-fix 3: the review named three pre-existing call sites; the same
  hazard applied to five more integer declarations in the same file, and all are
  covered. That is deliberate completeness, not scope creep — every one of them
  declares after the socket bind.
