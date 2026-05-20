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
- [ ] Fix `udp_bridge/include/udp_bridge/remote_node.h:63–72` — doc comment claims `dispatchResendRequest` logs at DEBUG on lookup miss, but the impl in `remote_node.cpp:186–199` emits WARN on first-miss-per-id and DEBUG on subsequent. Update the comment to match (mirror the existing field comment at `remote_node.h:177–183`).
- [ ] Rename and reword "silently drops" tests in `udp_bridge/test/test_remote_node_resend.cpp` — tests `DispatchSilentlyDropsWhenStampedIdAbsent` (line 533) and `DispatchSilentlyDropsUnknownConnectionId` (line 555), plus the comment at lines 531–532 ("Dispatch must no-op silently"), wrongly imply no diagnostic output. The invariant is "no broadcast fallback," not "no log." Rename to `DispatchDropsWithoutBroadcast…` and reword the comments.
- [ ] Decide (surface to user): clamp incoming `rr.connection_id` to `maximum_connection_id_size - 1` (8-byte) at the receive side in `udp_bridge.cpp:decodeResendRequest`. Closes the latent unbounded-growth of `dispatch_miss_warned_ids_` from a misbehaving peer AND aligns the receive side with the existing sender-side truncation contract. Existing comment at `remote_node.h:181–183` documents the current "trust the peer config space" tradeoff — user picks.
- [ ] (Optional) On PR #25, reply to or dismiss the CMake/ODR Copilot finding (false positive — the existing `UDP_BRIDGE_BUILD_TESTING` namespacing in `CMakeLists.txt:67–94` is exactly the mitigation Copilot recommended; no downstream consumers currently include `udp_bridge/connection.h`).
