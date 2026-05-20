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
- [ ] Decide between (a) comment-only honesty fix on `remote_node.h:184–189`'s `dispatch_miss_warned_ids_` field comment — drop "bounded by the configuration space ... which is small" and replace with the actual trust-model assumption — or (b) hard cap / LRU eviction on the set (~8 LOC + test) so the code enforces what the comment promises. Recommend (b) this round — bot has flagged this concern twice; an LRU cap is small and stops the re-review treadmill.
- [ ] (Optional) On PR #25, dismiss or reply to the #4 (CMake/ODR) Copilot finding so it stops reappearing in future reviews.
