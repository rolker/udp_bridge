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
- [ ] (must-fix) Connection ids keyed RAW in connection_rates / TunableTarget / drop records but canonicalised to 7 chars everywhere else — invisible shedding, up to 2x the configured cap, silent cross-retune. Cross-confirmed by 3 independent passes. Latent on the current fleet (vpn/wifi/cell); live in the repo's own example_params.yaml ("telemetry") — `src/udp_bridge.cpp:1758`, `include/udp_bridge/destination_selection.h:186`
- [ ] (must-fix) A per-topic cap can never be REMOVED by a re-configure: addSubscriberConnection's "0 retains" is correct for the wire paths but wrong for on_configure, where 0 is a real documented value. `param get` reads 0/unlimited while the selector still sheds at the old cap — the exact accepted-reads-back-inert defect this branch exists to remove — `src/udp_bridge.cpp:1771`
- [ ] (must-fix) Connection-level maximum_bytes_per_second narrows int64->int with no range check: a YAML 3000000000 goes negative, the `> 0` guard fails, and the link silently falls back to default_rate_limit 50000 B/s with no warning. The same PR adds full clamp+WARN for the per-topic cap 120 lines below and advertises 0-4294967295 in the README — `src/udp_bridge.cpp:569`
- [ ] (must-fix) on_shutdown is not symmetric with on_cleanup: the parameter callback stays registered into FINALIZED, so a param set still mutates live Connections; and runtime_tunables_ is declared AFTER the handle, so it is destroyed first while the callback can still read it — `src/udp_bridge.cpp:898`, `include/udp_bridge/udp_bridge.h:593`
- [ ] (must-fix) doc/relay_design.md was missed entirely: documented call signature no longer compiles (3rd arg is message_bytes, not source_node); the drop-reason contract is false on both halves now that rateLimited() sums TopicRateLimited and rate_limited_by_topic_cap exists; the "single implementation of the period rule" section omits that it is now also the byte cap — `doc/relay_design.md:71,465,494`
- [ ] (must-fix) Four behaviours untested, all regression-prone: target-gone refusal, cleanup->configure tunables rebuild, RelayDropReason::TopicRateLimited accounting, and the callback-registration ordering — documented as load-bearing in three places, enforced by nothing, and its failure mode is on_configure throwing past the socket bind where rclcpp_lifecycle swallows it — `src/udp_bridge.cpp:1877`, `:503`, `include/udp_bridge/relay_drops.h`
- [ ] (suggestion) A byte-cap shed skips periods.insert(), so capping one connection transiently perturbs an UNCAPPED sibling's send timing — `include/udp_bridge/destination_selection.h:278`
- [ ] (suggestion) Drop records carry fragment_count 0 and dilute average_fragment_count, the figure an operator uses to size maximum_packet_size — `include/udp_bridge/destination_selection.h:177`
- [ ] (suggestion) A parameter left over from a previous configure (connection removed from connections_list) is accepted and does nothing; the not-registered rejection covers removed-forwarding but not removed-from-config — `src/udp_bridge.cpp:1817`
- [ ] (suggestion) README runtime table gives the floor as "finite double >= 0" but the code also refuses > kMaxTunableFloat — `README.md:238`
- [ ] (suggestion) The config-file over-range clamp for the per-topic cap is in code but in none of the three docs, which mention only the negative->0 case — `src/udp_bridge.cpp:704`
- [ ] (suggestion) Only the runtime negative-rejection is tested; the config-side negative->warn+0 path is not, and the asymmetry between the two entry points is this branch's central validation policy — `src/udp_bridge.cpp:701`
- [ ] (suggestion) example_params.yaml's four connection tunables do not mention they are now live-tunable, while the new per-topic comment does — `config/example_params.yaml:100`
- [ ] (suggestion) addSubscriberConnection's "0 retains" rule is called out in a test comment but never exercised; a service re-subscribe silently wiping an operator's cap is what it exists to prevent — `src/udp_bridge.cpp:1763`

### Notes
- The field review's "8 must-fix" list is unrecoverable (ran with --no-progress). This review is independent and its findings are new; the count landing near 8 is coincidence, not corroboration.
- Lens A specifically CLEARED the risk the field flagged hardest — callback registration order and teardown symmetry across on_configure/on_cleanup, plus token-bucket correctness (sustained rate, oversized message, backwards clock, first use) and the single-implementation claim. Lens B independently cleared the concurrency claim (bounded arithmetic inside the existing subscribers_mutex_ scope, nothing held across sendto), lock ordering, and all-or-nothing batching. B's on_shutdown finding is not a contradiction of A: A examined configure/cleanup, B examined shutdown and destruction.
- CONCURRENT WORK HAZARD: udp_bridge#52 (branch feature/issue-52, 13 commits, unpushed, updated today) rewrites the AIMD control law these four tunables tune, and adds 78 lines to resend_constants.h — the file this branch hoists kMaxLinkHeadroomFraction into precisely so the setter clamp and the validation cannot drift. Trial merge: textual conflicts in 3 docs only; the code auto-merges, which is what makes the semantic drift dangerous. Issue #75's body wrongly states #52's fix "merged 2026-08-22". Sequence #52 first, then rebase this and re-check the ranges.
- Open #66 (on_cleanup never closes the socket; CLEANUP->CONFIGURE exit(1)s on EADDRINUSE) touches the same lifecycle path this branch modifies.
