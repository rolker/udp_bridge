---
issue: 9
---

# Issue #9 — Resend loop amplifies traffic rather than tracking loss

## Plan Review
**Status**: complete
**When**: 2026-05-18 13:00
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))

**Plan file**: `.agent/work-plans/PLAN_ISSUE-9.md` (legacy path; migration to `issue-9/plan.md` recommended during rebase)
**PR**: #13 (DRAFT)
**Verdict**: mostly ready; 3 substantive findings + 1 structural rebase step before implementation starts.

### Findings

1. **[Consequences] `Remote.msg` schema append — bag-replay implication missing.**
   Plan claims "appending is backward-compatible" but `BridgeInfo` is recorded
   in `bizzyboat.yaml` / `izzyboat.yaml` bag specs and ROS 2 Jazzy uses
   strict type-hash discovery. Old bags played against new code may emit
   type-hash warnings; mixed-version bridges may fail to discover each
   other on the `bridge_info` topic. Plan should add a consequences-table
   row documenting bag-replay behavior and the coordinated bridge-update
   expectation.

2. **[Consequences] Internal inconsistency: `BridgeInfo.resend_giveup_count` aggregate vs per-`Remote`.**
   Plan body and "Decisions made during planning" both place the field on
   `Remote.msg` (per-remote). But the Files-to-Change table says
   `udp_bridge.cpp` will "populate `BridgeInfo.resend_giveup_count` aggregate
   before publishing". `BridgeInfo` has `Remote[]` but no top-level
   aggregate field. Reword the table row to "populate each
   `Remote.resend_giveup_count` from the corresponding RemoteNode counter".

3. **[File targeting] `static_assert(receiver_window >= kSentPacketTTL)` risks tautology.**
   If both sites literally reference `kSentPacketTTL`, the assert collapses
   to `kSentPacketTTL >= kSentPacketTTL`. Either (a) introduce a separate
   `kReceiveHistoryWindow` constant defaulted to `kSentPacketTTL` and
   assert the relationship, or (b) drop the static_assert and rely on
   the single-constant structural enforcement (which the prose already
   argues is sufficient). Decide and document.

4. **[Scope, structural] Branch is 20 commits behind `origin/jazzy`.**
   Plan's "Approach" assumes only #11 lands first. Since the plan was
   written, #11, #12, #14, #15, and #16 have all merged. Rebase will need
   to absorb: executor split + Copilot fixes (#12), executor thread
   pinning (#14), test infra (#15), and the atomic rate-limit fix in
   `connection.cpp` (#16 / commit `1489cfb`). Expand the rebase note to
   "rebase onto current jazzy"; grep for residual `m_*` in touched files
   post-merge.

### Stale line-number references (informational, not blocking)

Plan file:line references drifted with #11 + #12. Implementer should
re-locate before editing:

| Plan reference | Current (jazzy) |
|---|---|
| `remote_node.cpp:170` (`getMissingPackets`) | `remote_node.cpp:200` |
| `remote_node.cpp:175` (receiver window 5.0s) | `remote_node.cpp:207` |
| `remote_node.cpp:177` (0.2s cooldown) | `remote_node.cpp:209` |
| `udp_bridge.cpp:742` (`cleanupSentPackets`) | `udp_bridge.cpp:1077-1082` |

### Recommended Actions (in order)

- [ ] Rebase `feature/issue-9` onto current `origin/jazzy`. Verify `m_*` rename completeness in touched files via `git grep "m_[a-z]"` afterwards. Migrate plan file from `PLAN_ISSUE-9.md` to `issue-9/plan.md` as part of this commit.
- [ ] Update the plan: add bag-replay consequences row (finding 1), fix the `BridgeInfo` aggregate wording (finding 2), and pick a static_assert resolution (finding 3).
- [ ] Begin implementation per the 5-commit plan (TTL constants → `Remote.msg` schema → debounce → backoff + give-up → tests).
