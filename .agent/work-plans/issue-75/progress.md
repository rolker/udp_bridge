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
