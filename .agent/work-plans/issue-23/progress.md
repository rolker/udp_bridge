---
issue: 23
---

# Issue #23 — Restrict resend response to the connection the request arrived on (add connection_id to ResendRequest)

## External Review
**Status**: complete
**When**: 2026-05-20 16:54
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))

**PR**: #25 — 1 review (Copilot), 5 inline comments: 2 valid, 1 false positive, 2 borderline (surface to user)
**CI**: all-pass (Prepare, Agent, Upload results, Cleanup artifacts — 4/4)

### Actions
- [x] Fix `udp_bridge/include/udp_bridge/remote_node.h:63–72` — doc comment claims `dispatchResendRequest` logs at DEBUG on lookup miss, but the impl in `remote_node.cpp:186–199` emits WARN on first-miss-per-id and DEBUG on subsequent. Update the comment to match (mirror the existing field comment at `remote_node.h:177–183`).
- [x] Rename and reword "silently drops" tests in `udp_bridge/test/test_remote_node_resend.cpp` — tests `DispatchSilentlyDropsWhenStampedIdAbsent` (line 533) and `DispatchSilentlyDropsUnknownConnectionId` (line 555), plus the comment at lines 531–532 ("Dispatch must no-op silently"), wrongly imply no diagnostic output. The invariant is "no broadcast fallback," not "no log." Rename to `DispatchDropsWithoutBroadcast…` and reword the comments.
- [x] Clamp incoming `rr.connection_id` to `maximum_connection_id_size - 1` (7-byte) at the receive side in `udp_bridge.cpp:decodeResendRequest`. User decision 2026-05-20: option A from the borderline-findings discussion. Closes the latent unbounded-growth of `dispatch_miss_warned_ids_` AND fixes a latent interop gap for older/buggy peers that didn't apply the truncation contract.
- [ ] (Optional) On PR #25, reply to or dismiss the CMake/ODR Copilot finding (false positive — the existing `UDP_BRIDGE_BUILD_TESTING` namespacing in `CMakeLists.txt:67–94` is exactly the mitigation Copilot recommended; no downstream consumers currently include `udp_bridge/connection.h`).

## Address External Review
**Status**: complete
**When**: 2026-05-20 17:25
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))

Round-3 changes addressing Copilot's PR-25 review:

- **`udp_bridge/src/udp_bridge.cpp`** — `decodeResendRequest` clamps `rr.connection_id` to `maximum_connection_id_size - 1` after the deserialize. Mirrors the sender-side truncation at line 1034-1035. Closes two latent failure modes: unbounded growth of `dispatch_miss_warned_ids_` from network-supplied strings, and silent resend-routing miss for any older/buggy peer that didn't apply the sender-side truncation. No dedicated unit test added: the clamp is a one-line `resize` on a value field, exposing the private `decodeResendRequest` would require a friend declaration or test-only refactor disproportionate to the change, and the existing `DispatchHonorsTruncatedConnectionId` test already covers the downstream truncated-form routing behavior.
- **`udp_bridge/include/udp_bridge/remote_node.h`** — `dispatchResendRequest` header comment updated to describe WARN-first/DEBUG-subsequent log behavior, matching `remote_node.cpp:186–199` and the field comment at lines 177–183.
- **`udp_bridge/test/test_remote_node_resend.cpp`** — renamed `DispatchSilentlyDrops…` tests to `DispatchDropsWithoutBroadcast…` and reworded the surrounding "must no-op silently" prose to "no broadcast fallback." Invariant under test is unchanged.
- **`.agent/work-plans/issue-23/plan.md`** — added two round-3 implementation notes (receive-side clamp; doc/test-name polish).

Build and full test suite (50 tests) green locally on the worktree.

CMake/ODR Copilot finding left as-is; existing `UDP_BRIDGE_BUILD_TESTING` namespacing in `CMakeLists.txt:67–94` already addresses it.

## External Review
**Status**: complete
**When**: 2026-05-20 17:30
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))

**PR**: #25 (after round-3 push) — 2 reviews (Copilot), 6 inline comments total. Of the 5 from review 1 (stale, against `e244b9b`): 3 addressed by round-3 (#1 doc, #5 test rename, #2/#3 partially via clamp), 1 standing-by false positive (#4 CMake/ODR), 1 still open. Review 2 (against current HEAD `f6dad12`) adds 1 new comment sharpening the unbounded-set concern: the receive-side clamp bounds per-entry size but not the count of distinct ids.
**CI**: all-pass (Prepare, Agent, Upload results, Cleanup artifacts — 4/4)

### Actions
- [x] User decision 2026-05-20: option (b). Bounded FIFO with eviction added in round-4 (see entry below).
- [ ] (Optional) On PR #25, dismiss or reply to the #4 (CMake/ODR) Copilot finding so it stops reappearing in future reviews.

## Address External Review
**Status**: complete
**When**: 2026-05-20 17:47
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))

Round-4 changes addressing Copilot's review-2 sharpening of the unbounded-set concern:

- **`udp_bridge/include/udp_bridge/remote_node.h`** — replaced `std::set<std::string> dispatch_miss_warned_ids_` with a bounded FIFO: `std::deque<std::string> dispatch_miss_warned_order_` (insertion order) plus `std::unordered_set<std::string> dispatch_miss_warned_ids_` (O(1) presence checks), capped at `static constexpr std::size_t kDispatchMissWarnedCap = 256`. Field comment rewritten to describe what's now actually enforced. Added `dispatchMissWarnedIdCountForTest()` accessor under `UDP_BRIDGE_BUILD_TESTING`.
- **`udp_bridge/src/remote_node.cpp`** — `dispatchResendRequest` updated to use the bounded FIFO: presence check via `unordered_set::count`; on insertion-past-cap, evict from the front of the deque and the corresponding entry from the set. New test accessor implemented (returns `dispatch_miss_warned_ids_.size()` under `state_mutex_`).
- **`udp_bridge/test/test_remote_node_resend.cpp`** — new `DispatchMissWarnedIdsAreBounded` test pushes 1024 distinct ids through `dispatchResendRequest` (no connections registered, so all are lookup-misses by construction) and asserts `dispatchMissWarnedIdCountForTest()` returns `<= 512` (generous upper bound on the cap), `< 1024` (proves the cap fires), and `>= 1` (proves the WARN-once path is reached). Test name and assertions are written against the observable cap behavior, not a specific cap value — cap value is an implementation choice, not a wire contract.
- **`.agent/work-plans/issue-23/plan.md`** — added round-4 implementation note describing the FIFO+set design, eviction semantics, and cap rationale.

Build and full test suite (51 tests; +1 cap test) green locally on the worktree.

CMake/ODR Copilot finding still standing as documented false positive.

## External Review
**Status**: complete
**When**: 2026-05-20 18:00
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))

**PR**: #25 (after round-4 push) — 3 reviews (Copilot). Review 1 (against stale `e244b9b`) and Review 2 (against `f6dad12`) status unchanged; Review 3 against current HEAD `5225c2e` adds 2 new findings, both valid.
**CI**: all-pass (Prepare, Agent, Upload results, Cleanup artifacts — 4/4)

Two new findings:

1. **`remote_node.h:13`** — `<set>` include removed in round-4 (set was only used for the now-replaced `dispatch_miss_warned_ids_`), but `given_up_packet_numbers_` at line 260 still uses `std::set<uint64_t>`. Build succeeds only because `udp_bridge.h:10` transitively brings `<set>` in. Fragile.
2. **`CMakeLists.txt:95`** — when `BUILD_TESTING=ON`, the library is compiled with `UDP_BRIDGE_BUILD_TESTING` (larger `Connection`/`RemoteNode` layouts), but the `udp_bridge_node` executable target links against that library without defining the same macro. Its compilation of `connection.h`/`remote_node.h` (via `udp_bridge.h:25`) sees the smaller layouts. In-package ODR mismatch. In practice the executable doesn't allocate `Connection` by value or inline layout-dependent methods (everything goes through `shared_ptr` and out-of-line library methods), so it hasn't bitten us — but it's UB per the standard.

Updates the prior classification: the original CMake/ODR finding from Review 1 was directionally correct on the principle; I covered downstream-package consumers but missed the in-package executable consumer. Reclassifying that comment from false-positive to partially-valid.

### Actions
- [x] Fix #7: re-add `#include <set>` to `udp_bridge/include/udp_bridge/remote_node.h` alongside `<deque>` and `<unordered_set>`.
- [x] Fix #8: add `target_compile_definitions(udp_bridge_node PRIVATE UDP_BRIDGE_BUILD_TESTING)` inside the existing `if(BUILD_TESTING)` block in `CMakeLists.txt`. Extend the CMake comment to cover the in-package executable case alongside the existing downstream-package wording.
- [x] Update `plan.md` round-5 note documenting both fixes and the partial-false-positive correction.

## Address External Review
**Status**: complete
**When**: 2026-05-20 18:05
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))

Round-5 changes addressing Copilot's review-3 findings (against round-4 HEAD `5225c2e`):

- **`udp_bridge/include/udp_bridge/remote_node.h`** — added `#include <set>` alongside the existing `<deque>` and `<unordered_set>`. `given_up_packet_numbers_` at line 260 still uses `std::set<uint64_t>`; without the direct include the build was relying on a transitive include via `udp_bridge.h:10`. Fragile.
- **`udp_bridge/CMakeLists.txt`** — added `target_compile_definitions(udp_bridge_node PRIVATE UDP_BRIDGE_BUILD_TESTING)` inside the existing `if(BUILD_TESTING)` block, with a new comment paragraph explaining why the in-package executable needs the same discipline as the test targets (the executable includes `udp_bridge.h` which directly includes `connection.h`, so it sees `Connection`'s class definition; without the macro it would compile against a different layout than the library was built with). Walks back my earlier "false positive" classification on the original CMake/ODR finding from review 1 — the in-package executable was the consumer I'd failed to audit.
- **`.agent/work-plans/issue-23/plan.md`** — round-5 implementation note documenting both fixes and the corrected classification.

Build and full test suite (51 tests) green locally on the worktree.

## External Review
**Status**: complete
**When**: 2026-05-20 18:30
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))

**PR**: #25 (after round-5 push) — 4 reviews (Copilot). Reviews 1–3 status unchanged (all earlier findings addressed in rounds 2–5). Review 4 against current HEAD `3f40162` adds 3 new findings, all valid polish items.
**CI**: all-pass (Prepare, Agent, Upload results, Cleanup artifacts — 4/4 on `5225c2e`; new run for `3f40162` not yet queued at triage time)

Three new findings:

1. **`udp_bridge/src/udp_bridge.cpp:716-732`** — receive-side clamp rationale comment claims `dispatch_miss_warned_ids_` "is never pruned" as part of gap #2's framing. Round-4 added FIFO eviction with cap `kDispatchMissWarnedCap`, so the table IS pruned now. The clamp itself still pays off (bounds per-entry size to 7 bytes, aligns wire contract for older/buggy peers), but the "ties the practical bound to the configured id space" wording is stale.
2. **`udp_bridge/test/test_remote_node_resend.cpp:666`** — `DispatchMissWarnedIdsAreBounded` pushes 1024 distinct ids through dispatch, each emitting a WARN-on-first-miss log line. The captured test log carries ~1024 WARN lines per run. Doesn't affect pass/fail but is CI-noisy.
3. **`udp_bridge/src/remote_node.cpp:201-206`** — `joined` string and `known_ids` join loop are built unconditionally on every lookup miss, even when the message will only log at DEBUG. Typically 2–4 known ids so the wasted work is small, but it's avoidable.

### Actions
- [x] Fix #9: tighten the clamp rationale comment in `udp_bridge.cpp:716–732` to reflect the round-4 FIFO cap (mention the cap+eviction; keep the per-entry-size and older-peer rationales).
- [x] Fix #10: reduce `DispatchMissWarnedIdsAreBounded`'s push count to 288 (cap+32) and silence the node logger inside the test (restore on exit). WARN lines from this test dropped from ~1024 to 0.
- [x] Fix #11: move both the `known_ids` snapshot (inside the lock) and the `joined` build (after the lock) into the WARN branch only; the DEBUG branch now references the prior WARN.

## Address External Review
**Status**: complete
**When**: 2026-05-20 18:45
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))

Round-6 changes addressing Copilot's review-4 polish findings:

- **`udp_bridge/src/udp_bridge.cpp`** — reworded the receive-side clamp rationale to acknowledge round-4's FIFO cap; reframed the clamp's ongoing value (bounds per-entry size; normalizes incoming ids to prevent double-tracking truncated/untruncated forms).
- **`udp_bridge/src/remote_node.cpp`** — `dispatchResendRequest` now skips the `known_ids` snapshot AND the join loop when the message will only log at DEBUG. The DEBUG message references the prior WARN line for this id ("see the prior WARN line for this id for the known-ids snapshot"). Cap-eviction-induced re-WARNs always carry a fresh `known_ids` list, so the narrative is accurate even under capacity pressure.
- **`udp_bridge/test/test_remote_node_resend.cpp`** — `DispatchMissWarnedIdsAreBounded` push count reduced from 1024 to 288; node logger temporarily silenced (level set to `Error`) for the duration of the loop, then restored. Verified: captured test log dropped from ~1024 "Dropping ResendRequest" lines to 0 from this test. The cap-enforcement assertion (`dispatchMissWarnedIdCountForTest <= 512`) is unchanged and continues to verify the cap fires.
- **`.agent/work-plans/issue-23/plan.md`** — round-6 implementation note documenting all three changes.

Build and full test suite (51 tests) green locally on the worktree.

## External Review
**Status**: complete
**When**: 2026-05-20 19:25
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))

**PR**: #25 (after round-6 push) — 5 reviews (Copilot). Reviews 1–4 status unchanged. Review 5 against current HEAD `f7d5977` adds 2 comments — same finding presented twice (one on the `.msg` file, one on the C++ decode site).
**CI**: all-pass (Prepare, Agent, Upload results, Cleanup artifacts — 4/4)

The finding: round-3's try/catch comment in `decodeResendRequest` (and the matching wording in `ResendRequest.msg`) overstates the benefit. Both comments claim that without the local try/catch a deserialization exception would propagate to the executor and kill the bridge. Verified false: `UDPBridge::decode()` at `udp_bridge.cpp:568-614` already has an outer catch-all around every `decode*` call (`decodeResendRequest` is invoked at line 596-598). The inner try/catch's actual benefit is diagnostic — it emits a specific WARN naming the source (node_name, host, port) and the failure kind, instead of falling through to the outer's generic "decoding error on packet of type N" ERROR. Useful for operators chasing a coordinated-redeploy mismatch, but not load-bearing for executor survival.

### Actions
- [x] Fix #12 + #13 (bundle): updated both the inner try/catch comment in `udp_bridge.cpp:697-712` and the matching note in `ResendRequest.msg` to describe the real benefit (site-specific WARN vs outer's generic ERROR) and explicitly note that executor survival comes from `UDPBridge::decode`'s outer catch-all, not this site-specific one. Comment-only change; build + 51/0/0 tests green.

## Address External Review
**Status**: complete
**When**: 2026-05-20 19:35
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))

Round-7 changes addressing Copilot's review-5 (same finding presented twice):

- **`udp_bridge/src/udp_bridge.cpp:697-712`** — rewrote the inner try/catch rationale to describe the actual contract: diagnostic clarity (site-specific WARN naming source bridge + failure kind) vs the outer catch-all's generic "decoding error on packet of type N" ERROR. Explicitly notes that executor survival comes from the outer catch in `UDPBridge::decode` (line 568-614), not this one. The follow-up note about other `decode*` sites was also softened — mirroring the pattern is a diagnostic-payoff judgment call, not a survival requirement.
- **`udp_bridge_interfaces/msg/ResendRequest.msg`** — matched the corrected framing on the message side: site-specific catch is about diagnosability of the coordinated-redeploy scenario, not survival; survival is provided by the outer catch in `UDPBridge::decode`.
- **`.agent/work-plans/issue-23/plan.md`** — round-7 implementation note documenting the correction.

Build and full test suite (51 tests) green locally on the worktree.

## Local Review (Post-PR)
**Status**: complete
**When**: 2026-05-20 21:37
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))
**Verdict**: changes-requested

**PR**: #25 at `3319fcd` (post-jazzy-merge)
**Mode**: post-PR
**Depth**: Deep (reason: 1197 added lines, wire-format change, concurrency code, in-package ABI hygiene, network-supplied data)
**Must-fix**: 2 | **Suggestions**: 10

### Findings
- [x] (must-fix) `DispatchHonorsTruncatedConnectionId` bypasses the receive-side clamp — test asserts a false invariant about wire-contract behavior — `udp_bridge/test/test_remote_node_resend.cpp:597-632`
- [x] (must-fix) WARN message claims "Further misses for this id will log at DEBUG" but FIFO eviction can re-WARN — comment-vs-code drift — `udp_bridge/src/remote_node.cpp:222`
- [x] (suggestion) `DispatchMissWarnedIdsAreBounded` ceiling 512 too loose vs cap 256; also no FIFO-order assertion — `udp_bridge/test/test_remote_node_resend.cpp:700`
- [ ] (suggestion) Receive-side clamp can route a malformed id to a valid connection by prefix-truncation; document this in the clamp comment — `udp_bridge/src/udp_bridge.cpp:~733`
- [ ] (suggestion) `dispatchResendRequest` is public on RemoteNode but only one production caller; either mark "public for test access only; production callers must clamp first" or make private+friend — `udp_bridge/include/udp_bridge/remote_node.h:65-86`
- [x] (suggestion) Field comment line-number drift: refers to clamp "around line 715" but the clamp is now at ~733 post-merge — `udp_bridge/include/udp_bridge/remote_node.h:~202` (fixed round-9, also dropped a sibling drift in `udp_bridge.cpp` decodeResendRequest comment that referenced "udp_bridge.cpp:568-614")
- [ ] (suggestion) Audit other in-package `add_executable` targets and document the `UDP_BRIDGE_BUILD_TESTING` contract — `udp_bridge/CMakeLists.txt:112`
- [ ] (suggestion) Per-connection serialize+send adds CPU work vs broadcast; mention the tradeoff in the resendMissingPackets comment — `udp_bridge/src/udp_bridge.cpp:~1129-1151`
- [ ] (suggestion) `resend_call_count_for_test_` increment isn't atomic; document "counter bumps before any I/O" invariant in code or use `std::atomic` — `udp_bridge/src/connection.cpp:~447-449`
- [ ] (suggestion) Test logger level restore preserves prior-test state; cheap to add a comment noting this is deliberate — `udp_bridge/test/test_remote_node_resend.cpp:668-688`
- [ ] (suggestion) WARN known_ids list uses bare comma join; would parse ambiguously if any future id contained a comma. Quote each id — `udp_bridge/src/remote_node.cpp:~226`
- [ ] (suggestion) `rr.connection_id.resize()` doesn't shrink_to_fit; bounded (rr is stack-local) but the field-comment "table-entry-size" argument depends on copies normalizing capacity. Add note or call shrink_to_fit — `udp_bridge/src/udp_bridge.cpp:~734`

## Address External Review
**Status**: complete
**When**: 2026-05-20 21:50
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))

Round-8 changes addressing the two must-fixes (and suggestion #3) from the local `/review-code` pass:

- **`udp_bridge/test/test_remote_node_resend.cpp`** — renamed `DispatchHonorsTruncatedConnectionId` to `DispatchDoesNotFuzzyMatchConnectionId` and rewrote the comment block to be honest about what the test actually verifies (dispatch strict-equality, no fuzzy-match) and what it does NOT verify (the wire contract — sender truncation + receive clamp — which lives in the private wire-decode plumbing of `UDPBridge`). The previous comment block falsely claimed this test caught wire-contract regressions; post-round-3 it didn't, because the receive-side clamp would normalize an unclamped 8-char id before reaching dispatch.
- **`udp_bridge/src/remote_node.cpp`** — WARN message at line 222 reworded to acknowledge that FIFO eviction can re-fire the WARN: "Further misses for this id will log at DEBUG (until the rate-limit table evicts this id under pressure, at which point the WARN re-fires on next sighting)." Aligns the user-facing string with the field comment's documented design.
- **`udp_bridge/test/test_remote_node_resend.cpp`** — `DispatchMissWarnedIdsAreBounded` ceiling tightened from `EXPECT_LE(count, 512u)` to `EXPECT_EQ(count, dispatchMissWarnedCapForTest())` (via a new public static accessor on `RemoteNode` that returns `kDispatchMissWarnedCap` without exposing the private constant). Added a FIFO-order assertion using a new `isDispatchMissWarnedForTest(const std::string&)` accessor: after pushing >cap distinct ids, the most-recently-pushed id MUST still be in the bookkeeping and the earliest-pushed id MUST have been evicted. Catches a regression that evicts LIFO or arbitrarily.
- **`udp_bridge/include/udp_bridge/remote_node.h`** — added `isDispatchMissWarnedForTest(const std::string&)` and `dispatchMissWarnedCapForTest()` accessors under `UDP_BRIDGE_BUILD_TESTING`. Both follow the existing test-accessor pattern.
- **`udp_bridge/src/remote_node.cpp`** — added the `isDispatchMissWarnedForTest` implementation under the same gate.
- **`.agent/work-plans/issue-23/plan.md`** — round-8 implementation note documenting the self-review must-fixes and the suggestion tightening.

Build and full test suite (82 tests; +0 vs round-7 — same count, renamed test) green locally on the worktree.

Remaining suggestions from the local review (none must-fix; defer to follow-up if needed):
- Clamp comment doesn't surface prefix-truncation misroute behavior.
- `dispatchResendRequest` is public on `RemoteNode`; consider marking "public for test access only".
- Field-comment line-number reference drift ("around line 715" → 847).
- Audit other in-package `add_executable` targets for the `UDP_BRIDGE_BUILD_TESTING` discipline.
- Per-connection serialize+send CPU-cost tradeoff isn't surfaced in the `resendMissingPackets` comment.
- `resend_call_count_for_test_` increment isn't atomic.
- Test logger level restore preserves prior-test state (low risk, by design).
- WARN known_ids list uses bare comma join (theoretical ambiguity if ids ever contain commas).
- `rr.connection_id.resize()` doesn't `shrink_to_fit` (bounded; stack-local).

## External Review
**Status**: complete
**When**: 2026-05-21 03:10
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))

**PR**: #25 — 7 review(s) (all Copilot, no human), 18 inline comments, 3 valid, 0 false positives (15 addressed by intervening commits)
**CI**: all-pass (Prepare, Agent, Upload results, Cleanup artifacts — 4/4)

### Actions
- [x] Fix `udp_bridge/test/test_remote_node_resend.cpp:461` — block comment says "Four tests below cover…" but the routing section now contains five tests (the bookkeeping-cap test is labeled "Routing case 5" at line 647). Update the count or factor the bookkeeping test into its own header section. (Raised twice — R6 line 476, R7 line 463.)
- [x] Fix `udp_bridge/include/udp_bridge/remote_node.h:220–221` — replace hard-coded `"udp_bridge.cpp around line 715"` with a function-name reference (`UDPBridge::decodeResendRequest`). Already on the local-review deferred-suggestion list above; Copilot independently flagged it.
- [x] Fix `udp_bridge/src/udp_bridge.cpp:801` — replace hard-coded `"udp_bridge.cpp:568-614"` with `UDPBridge::decode` by name. `decode()` is currently at 647–715 with the catch at 707–715, so the existing range is already wrong.
- [ ] (Optional) Dismiss the stale Copilot review threads (R1–R5, and the R6 finding that R7 superseded) — concerns were resolved by intervening commits but the threads remain marked open in the GitHub UI.

## Address External Review
**Status**: complete
**When**: 2026-05-21 03:25
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))

Round-9 changes addressing all three valid Copilot findings from the PR #25 triage above. Comment-only edits — no behavior change; build clean, full suite green (82 tests, +0 vs round-8).

- **`udp_bridge/test/test_remote_node_resend.cpp`** — rewrote the routing-section block comment to enumerate all five tests by their current names. The pre-round-8 `TruncatedConnectionIdMatches` bullet was still present even though the test had been renamed to `DispatchDoesNotFuzzyMatchConnectionId` in round-8. New bullet describes what the renamed test actually verifies (dispatch strict-equality, not the wire-contract truncation that lives in private receive plumbing).
- **`udp_bridge/include/udp_bridge/remote_node.h`** — dropped the hard-coded `"see udp_bridge.cpp around line 715"` from the `dispatch_miss_warned_ids_` field comment. `UDPBridge::decodeResendRequest` is already named there, so the line reference is redundant and was already drifted (actual clamp is at udp_bridge.cpp:847 post-merge).
- **`udp_bridge/src/udp_bridge.cpp`** — dropped the hard-coded `"udp_bridge.cpp:568-614"` line range from the `decodeResendRequest` comment block. The outer `UDPBridge::decode` catch-all is actually at lines 707–715 inside the function at 647, so the range was already wrong.

## External Review
**Status**: complete (resolved by round-10 below)
**When**: 2026-05-21 13:55
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))

**PR**: #25 — 8 review(s) (all Copilot, no human), 20 inline comments total; 2 new on R8 (against `20e67cb`), both substantive and **not addressed**
**CI**: all-pass (Prepare, Agent, Upload results, Cleanup artifacts — 4/4)

R8 raised two views of one underlying contract gap. Both **valid** and **substantive** (not comment drift):

- `udp_bridge/src/udp_bridge.cpp:1173` and `udp_bridge/src/remote_node.cpp:149`: **The truncation contract is one-sided.** Sender truncates `rr.connection_id` to 7 chars (`maximum_connection_id_size - 1`). Receiver's `connections_` map, however, is populated from BridgeInfo (`rc.connection_id = connection->id();` at `udp_bridge.cpp:1474` — **full configured id**) and from config — never canonicalized to the 7-char wire form. The duplicate "starlin" entry created by `unwrap()` on packet arrival is what `dispatchResendRequest` actually finds. That duplicate connection's `sent_packets_` is empty (the real sends went through the "starlink" connection). Resend response is silently empty for any configured id ≥8 chars.

The round-8 self-review explicitly stated a contract that turns out not to hold. `test_remote_node_resend.cpp:600-617` (the scope note above `DispatchDoesNotFuzzyMatchConnectionId`) says:

> "A regression that re-introduces the pre-fix bug (sender stamps the full untruncated id) would still route correctly today because the receive-side clamp would normalize the id before reaching dispatch."

This argument assumes the receiver's `connections_` is keyed on the truncated wire form. The BridgeInfo population path keeps the full configured id. Test setup at line 623 (`newConnection("starlin", ...)`) doesn't mirror production where the map would also contain "starlink" from BridgeInfo.

**Mitigating context (why this isn't a live-loss bug today)**: current production configs in `unh_echoboats_project11/izzyboat_project11/config/{izzyboat,operator}.yaml` use connection ids `"wifi"` (4 chars) and `"vpn"` (3 chars). No production config in this branch has an id ≥8 chars. The contract gap is latent, not actively breaking field resends. But the team has discussed Starlink-named links (see workspace memory on link calibration); a future config that names an id "starlink" or similar would silently lose resend recovery on that link.

### Actions
- [x] **Decision**: user picked (a) — fix in PR #25 by canonicalizing. Implemented in round-10 below. Three options that were on the table:
  - **(a) In scope for PR #25**: canonicalize ids to the 7-char wire form at every insertion point (param ingestion, `RemoteNode::update(Remote)`, `RemoteNode::update(BridgeInfo)`, `unwrap()`, CONNECT path); reject collisions at config time; add a wire-contract integration test (a real `WrappedPacket` round-trip — neither sender truncation nor receive-side clamp has a unit test today, by design per the round-8 scope note).
  - **(b) Follow-up issue**: file a sequel to #23 with both Copilot citations + the BridgeInfo/unwrap analysis above; close R8 threads with a link to the new issue so the signal isn't lost.
  - **(c) Documented invariant**: accept "configured ids must be ≤7 chars" as a hard rule. Add a runtime check at param ingestion that rejects/warns on ids ≥8 chars; update the field comment on `dispatch_miss_warned_ids_` to state the invariant; **fix the round-8 scope-note in `test_remote_node_resend.cpp:600-617`** — it's currently asserting a contract that doesn't hold.
- [ ] (Optional) Dismiss the stale R1–R8 Copilot threads in the GitHub UI.

## Address External Review
**Status**: complete
**When**: 2026-05-21 14:35
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))

Round-10 changes implementing option (a) for Copilot R8's truncation-contract finding: canonicalize all connection ids to the on-wire truncated form at every connections_ insertion / lookup point. After the change, `connection->id()`, `connections_` keys, and the on-wire stamp always agree on a single canonical form (first `maximum_connection_id_size - 1` bytes), so a ResendRequest stamped with the wire form always lands on the connection that holds `sent_packets_` for the link — even for configured ids ≥ 8 chars.

- **`udp_bridge/include/udp_bridge/packet.h`** — new `truncate_connection_id(const std::string&)` helper. Single source of truth for the canonical form.
- **`udp_bridge/src/connection.cpp`** — `Connection::Connection` canonicalizes `id_` in its initializer so `connection->id()` is always canonical.
- **`udp_bridge/src/remote_node.cpp`** — `newConnection` canonicalizes its input and emits a one-time WARN when truncation actually shortens (surfaces the change at startup; flags the collision risk for any two configured ids sharing the first 7 chars). `connection()` canonicalizes its lookup id so callers can pass either form.
- **`udp_bridge/src/udp_bridge.cpp`** — updated comments on the sender-side stamp (now a defensive no-op since `connection->id()` is canonical) and the receive-side clamp (now a defense against misbehaving peers rather than the primary canonicalization mechanism).
- **`udp_bridge/include/udp_bridge/remote_node.h`** — field comment on `dispatch_miss_warned_ids_` updated to reflect the new contract.
- **`udp_bridge/test/test_remote_node_resend.cpp`** — new `NewConnectionCanonicalizesLongId` test exercises the contract end-to-end. Scope note above `DispatchDoesNotFuzzyMatchConnectionId` rewritten — the wire-decode clamp is no longer the primary canonicalization mechanism, the connections_ insertion sites are. Routing-section header updated to "Six tests below…" with the new canonicalization bullet.

Build clean, full suite green (83 tests, +1 vs round-9). New test verified with direct gtest filter — both the WARN-on-truncation and the canonical-form routing assertions fire.

## Local Review (Pre-Push)
**Status**: complete
**When**: 2026-05-21 14:50
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))
**Verdict**: changes-requested

**Branch**: feature/issue-23 at `0697ee0`
**Mode**: pre-push
**Depth**: Deep (reason: 1533 lines, concurrency-sensitive wire-protocol code, new msg field, build-config change)
**Must-fix**: 2 | **Suggestions**: 7

### Findings
- [ ] (must-fix) `RemoteNode::update(const Remote&)` uses bare `operator[]` with untruncated id — orphan null entry + Connection-state clobber on repeat calls — `udp_bridge/src/remote_node.cpp:35`
- [ ] (must-fix) `RemoteNode::newConnection` unconditionally overwrites canonical-key entries — silent collision when two configured ids share first 7 chars — `udp_bridge/src/remote_node.cpp:119`
- [ ] (suggestion) Stale comment claims connections_ is keyed on "full pre-truncation id from config" — post-R10 it's canonical — `udp_bridge/src/udp_bridge.cpp:1175`
- [ ] (suggestion) `dispatchResendRequest` raw `connections_.find` — route through canonical `connection()` for defense-in-depth — `udp_bridge/src/remote_node.cpp:146`
- [ ] (suggestion) `truncate_connection_id` could be `noexcept` + take `std::string_view` to skip copies on no-truncation common path — `udp_bridge/include/udp_bridge/packet.h`
- [ ] (suggestion) `adoptConnection` keys on `connection->id()` (canonical by transitivity post-R10); add `assert(connection->id() == truncate_connection_id(connection->id()))` to pin the invariant — `udp_bridge/src/remote_node.cpp:128`
- [ ] (suggestion) Add tests for `update(Remote)` long-id path + collision case (two configured ids sharing first 7 chars) — `udp_bridge/test/test_remote_node_resend.cpp`
- [ ] (suggestion) Update plan.md "Files to Change" table to include new `packet.h` helper added in round-10 — `.agent/work-plans/issue-23/plan.md`
- [ ] (suggestion) Verify PR body carries the coordinated-redeploy notice for whoever cuts the deployment (plan flags this as open question) — PR body

## External Review
**Status**: complete
**When**: 2026-05-21 15:00
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))

**PR**: #25 — 9 review(s) (all Copilot, no human), 23 inline comments total; 3 new + 1 suppressed-low-confidence on R9 (against `0697ee0`)
**CI**: all-pass (4/4)

R9 fully confirms the local /review-code findings from the prior entry. Four-way convergence on the must-fixes (Claude fresh-context Adversarial, Governance, synchronous Copilot CLI, GitHub-side Copilot R9). All four findings target round-10:

- `remote_node.cpp:142` — `newConnection` unconditional overwrite. (Matches local must-fix #2.)
- `remote_node.cpp:35` (R9 suppressed-low-confidence — Copilot named the pattern but not the specific offending line). `update(Remote)` uses bare `operator[]` with untruncated id. (Matches local must-fix #1.)
- `remote_node.h:81` — doc/impl drift on `dispatchResendRequest`. (Matches local suggestion: either update doc or route through `connection()`.)
- `udp_bridge.cpp:1177` — stale "keyed on the full pre-truncation id from config" comment. (Matches local suggestion #1.)

### Actions
- [x] Fix must-fix #1 (`remote_node.cpp:35`) and must-fix #2 (`remote_node.cpp:119`) in one round-11 commit.
- [x] Resolve `remote_node.h:81` doc/impl drift — picked the doc-update path (rather than route dispatch through `connection()`) to preserve `DispatchDoesNotFuzzyMatchConnectionId`'s intent.
- [x] Fix stale comment at `udp_bridge.cpp:1175`.
- [x] Add tests for `update(Remote)` long-id path + collision case.
- [ ] (Optional) Dismiss R1–R8 stale Copilot threads in the GitHub UI.

## Address External Review
**Status**: complete
**When**: 2026-05-21 15:20
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))

Round-11 changes addressing all four valid R9 findings (which also matched the local /review-code from the prior entry — four-way convergence).

- **`udp_bridge/src/remote_node.cpp`** (`update(Remote)` at line 33–48): replaced bare `connections_[c.connection_id]` with `this->connection(c.connection_id)`. The canonicalizing helper does a `find()` (no insertion on miss) so a missing entry returns null cleanly without polluting the map with an orphan untruncated-key slot. `recursive_mutex` tolerates the inner re-acquire.
- **`udp_bridge/src/remote_node.cpp`** (`newConnection` at line 121–171): no-op when the canonical key already maps to a live Connection; returns the existing entry. If `(host, port)` differ between the new request and the existing Connection, logs an ERROR identifying both configured ids and the host/port pair on each (the actual canonical-id collision signal). Same `(host, port)` silently returns the existing — defense-in-depth for callers that miss a `connection()` lookup. `newConnection` is now idempotent for a given canonical key.
- **`udp_bridge/include/udp_bridge/remote_node.h`** (`dispatchResendRequest` doc comment): updated to document the actual contract — caller pre-canonicalizes; the production caller (`UDPBridge::decodeResendRequest`) does so via the receive-side clamp; tests may pass either form to assert hits or misses. (Chose the doc-update over routing dispatch through `connection()` to preserve `DispatchDoesNotFuzzyMatchConnectionId`'s strict-equality intent.)
- **`udp_bridge/src/udp_bridge.cpp`** (stale comment at the `RemoteConnectionsList` populate site): rewrote to match the post-round-10 reality (both `connection->id()` and `RemoteNode::connection()` are canonical-form).
- **`udp_bridge/test/test_remote_node_resend.cpp`** — added two regression tests:
  - `UpdateRemoteWithLongIdDoesNotOrphanOrClobber` (regression for must-fix #1): drives `update(Remote)` with a long configured id, asserts the canonical-keyed Connection exists, lookup via either form returns the same pointer, and a second `update(Remote)` call returns the same pointer (no clobber).
  - `NewConnectionRejectsCanonicalIdCollision` (regression for must-fix #2): registers under "starlink" then calls with "starlink-vp" (same canonical, different host/port). Asserts the second call returns the existing pointer, host/port are preserved, and the idempotent same-input case also returns the existing pointer.
  - Routing-section header bumped from "Six tests below…" to "Eight tests below…" with bullets for both new tests.

Build clean, full suite green (85 tests, +2 vs round-10). Direct gtest filter confirmed both new tests exercise the regression paths and the ERROR log fires on the collision case.

