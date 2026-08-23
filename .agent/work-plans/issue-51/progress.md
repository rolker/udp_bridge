---
issue: 51
---

# Issue #51 — Relay support: provenance-aware remote→remote forwarding (cloud-hub topology)

## Issue Review
**Status**: complete
**When**: 2026-08-22 23:35 -04:00
**By**: Claude Code Agent (Claude Sonnet)

**Issue**: #51
**Comment**: (best-effort post follows this entry; not recorded inline)
**Scope verdict**: well-scoped

### Source verification

Verified both technical claims in the issue body against the worktree source
(`/home/roland/project11/layers/worktrees/issue-udp_bridge-51/core_ws/src/udp_bridge/udp_bridge`):

- **`ignore_local_publications` claim**: TRUE. `src/udp_bridge.cpp:1280` sets
  `options.ignore_local_publications = true` on every forwarding subscription
  (line number drifted from the issue's `:1274` — commit `b1e4736` "Mutex
  audit + lock guards" and later `#52` commits (`6dda032`, `7b45a8b`) shifted
  lines above it; the flag and its effect are unchanged). Since the bridge is
  the local publisher of every message it republishes from a remote, this
  flag means a forwarding subscription for topic X never hears the bridge's
  own republish of X — remote→remote forwarding is structurally blocked, as
  claimed.
- **Provenance-drop claim**: TRUE. `PublishItem` (`include/udp_bridge/publish_queue.h:27`)
  carries `topic`, `datatype`, `reliability`, `durability`, `history_depth`,
  and the serialized `message` — no source node, connection id, or remote
  identifier. `UDPBridge::decodeData`'s `build_item()` lambda
  (`src/udp_bridge.cpp:902-931`) populates `PublishItem` from `outer_message`
  fields only; `source_info` (which does carry `node_name`/`host`/`port`) is
  used solely for the stale-packet/reorder gate, never copied into the item.
  `publishItem()` (`src/udp_bridge.cpp:993+`) publishes via a plain
  `rclcpp::GenericPublisher` keyed only by topic — no per-connection
  awareness at all. Provenance is genuinely gone by the time a message could
  be forwarded onward.

Both claims hold. The issue's technical framing is accurate — no
contradictions found.

### Scope Assessment

**Well-scoped?** Yes, but substantial. The four numbered asks are tightly
coupled and don't decompose cleanly into separate issues: opt-in relay is
meaningless without loop protection, and loop protection requires the
provenance plumbing through `PublishItem` → `PublishQueue` →
`RemoteNode`'s reorder buffer (issue #35, closed/merged, but its buffer
stores `PublishItem` by value — a provenance field added there needs to
survive the buffer/replay path too, per the issue's own item 4). This is
legitimately one cohesive PR; recommend the implementer split it into
atomic commits (provenance plumbing → opt-in relay flag → loop-protection
logic → tests) rather than filing sub-issues.

**Right repo?** Yes — this is core message-relay behavior in `udp_bridge`
itself, not workspace infrastructure. Correctly filed against
`rolker/udp_bridge`.

**Dependencies**:
- #34 (per-topic tuning, open) — not blocking, but the issue itself notes a
  relay hub concentrates flows, making #34 more relevant post-#51. No action
  needed now.
- #35 (reorder buffer, closed) — real technical coupling, not just a
  reference: any provenance field added to `PublishItem` must be threaded
  through `RemoteNode::admitOrBuffer`/`flushExpiredBuffer` since those paths
  buffer `PublishItem` by value across the reorder window. The issue's own
  test-plan item 4 anticipates this. No action needed at review time beyond
  flagging it for the implementer.
- #45 (saturation RCA, closed) — informational only, no coupling found.
- **#53** (open, not mentioned in the issue body): "Document the trust model:
  transport is unauthenticated and must not be exposed outside a tunnel."
  Relay support increases the blast radius of an already-unauthenticated
  transport — a hub now re-forwards traffic between three parties instead of
  terminating it at two. Worth cross-referencing in whichever design doc
  covers #51 so the two aren't reviewed in isolation. Not blocking.

### Principle Alignment

| Principle | Status | Notes |
|---|---|---|
| Capture decisions, not just implementations | Action needed | This is an architecture-significant change (new routing/loop-protection model). The project already has a `doc/*_design.md` convention for exactly this kind of decision (`qos_design.md`, `admission_control_design.md`, `resend_budget_design.md`). Recommend a `doc/relay_design.md` capturing: default-off rationale, where provenance is tracked (per-message on `PublishItem` vs. per-topic-source table — issue leaves this open), and the exact loop-protection rule ("never forward out the connection it arrived on"). |
| A change includes its consequences | Watch | If `PublishItem`/wire-adjacent structs gain a provenance field, `config/example_params.yaml`'s per-topic `topics:` block needs the new `relay` flag documented inline (matching its existing per-field comment style), and `README.md`/`doc/conceptual_overview.md` should describe the new topology capability. Not called out explicitly in the issue's ask list — flagging so it's not dropped. |
| Test what breaks | OK | The issue's own test list (three-node relay, echo-loop regression, provenance retention across the queue/reorder path) is specific and targets the actual failure modes, not generic coverage. Matches this principle well already. |
| Only what's needed | OK | Default-off, opt-in per-topic-or-per-bridge — correctly scoped to not change behavior for existing symmetric configs. |
| Human control and transparency | OK | Routing stays explicit via the existing per-remote topic-list config; no implicit/automatic forwarding is introduced. |
| Improve incrementally | Watch | Large surface area for one PR (see Scope Assessment) but the coupling is real, not scope creep — recommend atomic commits, not a split. |

### ADR Applicability

No workspace ADR (`docs/decisions/*.md`) is triggered — this is a project-repo
domain change, not workspace infrastructure, CI, or agent-instruction change.
`udp_bridge` has no local ADR directory; it uses `doc/*_design.md` narrative
design docs instead (see Principle Alignment above) — that's the right
mechanism here, not a new ADR.

### Consequences

- `config/example_params.yaml` — add and document the new per-topic (or
  per-bridge) `relay` opt-in flag once implemented.
- `README.md` / `doc/conceptual_overview.md` — describe the new
  remote→remote relay capability and its default-off posture.
- A new `doc/relay_design.md` (or equivalent) capturing the provenance
  storage choice and loop-protection rule, consistent with the project's
  existing per-feature design-doc convention.

### Recommendations

- Cross-reference #53 (unauthenticated transport) in the design doc — a
  relay hub is a bigger target than a point-to-point bridge.
- Confirm before implementation whether provenance is tracked per-message
  (preferred, per the issue) or per-topic-source, since that choice
  determines whether `PublishItem` itself grows a field or whether a
  separate side-table is threaded through the reorder buffer.

### Actions
- [ ] Write a `doc/relay_design.md` (or extend `conceptual_overview.md`) capturing the opt-in default-off decision and the exact loop-protection rule, per the project's design-doc convention.
- [ ] Update `config/example_params.yaml` and `README.md`/`doc/conceptual_overview.md` for the new relay flag and topology capability as part of this PR, not a follow-up.
- [ ] Cross-reference #53 (unauthenticated transport trust model) in the design doc.
- [ ] Confirm provenance storage granularity (per-message vs. per-topic-source) before implementation, since it drives whether `PublishItem` gains a field or a side-table is threaded through the reorder buffer.

## Plan Authored
**Status**: complete
**When**: 2026-08-22 23:41 -04:00
**By**: Claude Code Agent (Claude Sonnet)

**Plan**: `.agent/work-plans/issue-51/plan.md` at `1c91f0d`
**Branch**: feature/issue-51 at `1c91f0d`
**Phases**: single

### Open questions
- [ ] No open questions — plan is review-plan-ready.

## Plan Review
**Status**: complete
**When**: 2026-08-22 23:45 -04:00
**By**: Claude Code Agent (Claude Sonnet)

**Plan**: `.agent/work-plans/issue-51/plan.md` at `1c91f0d`
**PR**: PR-less
**Verdict**: changes-requested

### Findings
- [ ] (must-fix) Loop-protection design gap: `origin_remote_node` is populated on `PublishItem` and verified to survive `PublishQueue` (by-value deque, `include/udp_bridge/publish_queue.h`) and `RemoteNode`'s reorder buffer (`ReorderBufferEntry.item`, `include/udp_bridge/remote_node.h:406`) — but the plan never explains how it reaches the actual forward/skip decision. `publishItem()` (`src/udp_bridge.cpp:993-1058`) discards `item` and does a plain `publisher->publish(item.message)`; the relay-triggering local subscription's `callback()` (`src/udp_bridge.cpp:722`) receives only `(topic_name, topic_type, shared_ptr<SerializedMessage>)` with zero provenance. Between these two points the message crosses a real rmw/DDS pub→sub round trip that carries no side-channel for `origin_remote_node`. As written, step 3 ("at the forward-out decision point... in the callback()... skip forwarding to any connection whose remote node name equals origin_remote_node") cannot literally be implemented — the field the plan spent its whole "decision" section justifying is unreachable exactly where the rule needs to run. This needs an explicit design fix before implementation: either (a) a correlation mechanism carrying provenance from `publishItem()`'s locally-triggered republish into that specific `callback()` invocation, or (b) invoke the relay-forwarding logic directly from `decodeData`/`publishItem` (where `origin_remote_node` is already in hand) instead of relying on the ROS-level local-subscription loopback at all. — `plan.md` "Loop-protection logic" step 3 / "Provenance storage granularity" decision section
- [ ] (must-fix) Opt-in flag leaks across connections sharing a subscription: `callback()`'s destination loop (`src/udp_bridge.cpp:748-762`) iterates every connection in `sub.remote_details` gated only by per-connection `period`, with no per-destination check of that connection's own `relay` flag. The plan's OR-across-connections semantics only gates whether the *shared subscription* hears local republishes at all (`ignore_local_publications`); once it fires (because some other connection on the same source topic set `relay: true`), every configured destination for that topic receives the forwarded traffic, filtered only by the new "not the connection it arrived on" loop check — not by whether that destination itself opted in. A connection left at the `relay: false` default therefore still receives remote-origin relayed traffic whenever a sibling connection on the same source topic enables `relay`. The plan needs an explicit per-destination relay gate in the forward loop (skip a destination whose own `relay` is false when the message's `origin_remote_node` is non-empty), not just the loop-protection check. — `plan.md` step 2 ("Opt-in relay flag") / step 3
- [ ] (suggestion) 3-node cycle: the loop-protection rule only compares against the *immediate* sender (`origin_remote_node` is rebuilt fresh at each hop from that hop's `SourceInfo::node_name`, per `unwrap()`/`decodeData`), so it provably prevents only a direct 2-hop echo. In a triangle of relay-enabled bridges (A-B, B-C, C-A all connected) a message can bounce indefinitely — a copy forwarded B→C is re-published locally at C with a fresh `origin_remote_node="B"`, so C's loop check only blocks forwarding it back to B, not onward to A, and the same argument applies at every node — producing sustained duplication/amplification, not a bounded relay. The plan's own text ("does NOT track a full multi-hop path... out of scope") gestures at this but doesn't call out the concrete mesh/cycle failure mode. Given the issue's actual topology is a star (boat→hub→operators), this is a documentation gap rather than a blocker: `doc/relay_design.md` should explicitly state that only direct-neighbor echo is prevented and that relay-enabled cycles beyond one hub are unsupported/unsafe. — `plan.md` "Scope of the provenance" paragraph
- [ ] (suggestion) Test plan's provenance-retention test (step 4, third bullet) only exercises `PublishQueue`/reorder-buffer push-pop, which is the least risky part of the by-value claim (trivially true, confirmed by inspection). It does not cover the actual gap above — a test exercising the real local-publish→forwarding-`callback()` round trip (the mechanism the loop-protection check depends on) is missing and should be added once the must-fix findings are resolved. — `plan.md` step 4

### Verified claims (no issue found)
- `PublishItem` moves by value through `PublishQueue`'s `std::deque` (`push`/`run`) and through `RemoteNode::admitOrBuffer`/`flushExpiredBuffer`'s `ReorderBufferEntry` — confirmed correct, no copies or references that would drop a new field.
- `SourceInfo::node_name` is reliably populated for real traffic: every outgoing Data message is sent via the templated `send()` which always wraps it in a `WrappedPacket`/`SequencedPacket` (`src/udp_bridge.cpp` ~1536-1553), and `unwrap()` always sets `node_name` from `wrapped_packet->source_node` before the recursive `decode()`/`decodeData()` call — so `node_name` is not silently empty on the receive side for the traffic this feature targets.
- `.agents/README.md`'s verified-parameter table row and file locations (`publish_queue.h:27`, `remote_node.h:406`, the `topics_list` loop at `src/udp_bridge.cpp` ~470-527, `updateLocalSubscriptions()`, `callback()`) all check out against current source.
- Issue #53 (unauthenticated transport trust model) is open and matches the plan's cross-reference intent.
- Existing `doc/*_design.md` / `test_*.cpp` naming conventions match the plan's proposed new files.

### Summary
The plan's central by-value-storage argument for `PublishQueue`/reorder-buffer survival is correct and well-verified. However, the plan has two must-fix design gaps that block implementation as written: (1) it never establishes how `origin_remote_node` reaches the actual forward/skip decision in `callback()`, since that decision point sits on the far side of a real ROS publish→subscribe round trip that the current design does not thread provenance through; (2) the per-topic opt-in is not actually enforced per-destination, so a `relay: false` connection can still receive relayed traffic from a sibling connection's `relay: true`. Both need a design revision (not just an implementation detail) before work starts.

### Recommended Actions
- [ ] Revise "Loop-protection logic" (plan step 3) to specify the actual mechanism carrying `origin_remote_node` into the callback()'s forwarding decision, or restructure the relay path to bypass the local-subscription loopback entirely and forward directly from `decodeData`/`publishItem`.
- [ ] Add an explicit per-destination `relay` check to the forward loop so a `relay: false` connection cannot receive relayed traffic merely because a sibling connection with `relay: true` shares the same source-topic subscription.
- [ ] Add a doc note (relay_design.md) on the 2-hop-only loop-protection scope and the unsupported cycle/mesh case.
- [ ] Extend the test plan to cover the real local-publish → forwarding-callback path, not just PublishQueue/reorder-buffer storage.

## Plan Authored
**Status**: complete
**When**: 2026-08-23 00:02 -04:00
**By**: Claude Code Agent (Claude Sonnet)

**Re-plan**: yes — rewrote the plan in place after Plan Review (`91393ec`,
verdict changes-requested) rejected the prior design's loopback-based relay
and unreachable `PublishItem` provenance field.

**What changed vs. the rejected design**:
- Dropped the ROS-loopback relay path entirely (`ignore_local_publications`
  toggling in `updateLocalSubscriptions()` is no longer touched at all).
- Dropped the `PublishItem.origin_remote_node` field and everything that
  depended on threading it through `PublishQueue`/the reorder buffer — the
  "provenance storage granularity" decision from the old plan is now moot.
- New design forwards entirely inside `decodeData()` at receive time, using
  the already-existing `subscribers_[topic].remote_details` routing table and
  `source_info.node_name` (a plain string compare against the routing table's
  key) for the loop rule — no provenance storage needed anywhere.
- Fixed must-fix #2 (opt-in leaking across sibling connections) by putting
  the new `relay` flag on `ConnectionRateInfo` itself (the same per-connection
  map `callback()` already reads), so a `relay: false` connection can never
  receive forwarded traffic regardless of a sibling's setting.
- Added a new required design element the old plan never addressed: a
  dedicated `relay_queue_` worker (mirroring `PublishQueue`/issue #10)
  because `Connection::send()`'s UDP `sendto()` has a bounded-but-nonzero
  (~200ms) worst case per packet, documented in `connection.cpp:545-560`,
  and calling it inline from `decodeData()` (on `socket_drain_group_`) would
  reintroduce the #10 drain-thread-blocking hazard.
- Added a `selectRateLimitedConnections` refactor so forwarded traffic goes
  through the identical period-gating and `Connection::send()` AIMD admission
  control (#43/#52) as locally-originated traffic, rather than a parallel
  code path that could drift.
- Test plan swapped the old "provenance retention across queue/reorder"
  test (now moot) for a "per-destination opt-in isolation" test targeting
  must-fix #2 directly, plus a new rate-limit-parity test.

**Plan**: `.agent/work-plans/issue-51/plan.md` at `361d55e`
**Branch**: feature/issue-51 at `361d55e`
**Phases**: single

### Open questions
- [ ] No open questions — plan is review-plan-ready.

## Plan Review
**Status**: complete
**When**: 2026-08-23 00:11 -04:00
**By**: Claude Code Agent (Claude Sonnet)

**Plan**: `.agent/work-plans/issue-51/plan.md` at `361d55e`
**PR**: PR-less
**Verdict**: approve-with-suggestions

### Round-1 must-fix re-verification

Both must-fix findings from the prior review (`91393ec`) are genuinely
resolved by the redesign, not just reworded — verified independently
against current source, not by trusting the plan's prose:

- **Must-fix #1 (provenance unreachable at the loop-protection decision)**:
  resolved by construction. The new design forwards from inside
  `decodeData()` (`src/udp_bridge.cpp:899`), which already holds
  `source_info.node_name` directly — there is no ROS pub→sub round trip to
  lose it across. Confirmed the two sides of the loop comparison share one
  identity namespace: `unwrap()` sets `source_info.node_name` from
  `wrapped_packet->source_node` (`src/udp_bridge.cpp:1312`), the same value
  used to key `remote_nodes_` (`:1330`) and `decodeTopicStatistics`
  (`:1162`); `subscribers_[topic].remote_details` is keyed by `remote_node`
  (`include/udp_bridge/types.h:40`, populated at `addSubscriberConnection`,
  `src/udp_bridge.cpp:1187`), and every call site threads through
  `remote_info.name` (the config-declared remote name, `:528`) or
  `source_info.node_name` (`:1308`) — the same config-name/wire-source_node
  correspondence the codebase already relies on elsewhere (e.g.
  `decodeTopicStatistics`). The plain string compare is comparing like for
  like.
- **Must-fix #2 (opt-in leaked across sibling connections)**: resolved by
  construction. `relay` lives on `ConnectionRateInfo`
  (`include/udp_bridge/types.h:10`), keyed per `(remote, connection_id)` —
  the exact same map `callback()`'s destination loop already iterates
  per-connection (`src/udp_bridge.cpp:754-762`, each `connection_rate`
  pushed individually into `destinations`). A `relay: false` connection is
  structurally unreachable from a sibling's `relay: true`.

### Findings

- [ ] (suggestion) `addSubscriberConnection` (`src/udp_bridge.cpp:1172`)
  unconditionally overwrites `rd.connection_rates[connection_id].period`
  on every call (no "if changed" guard, unlike the `reliability`/
  `durability` fields which use `if(!x.empty())`). If the plan's new
  `relay` param follows the same unconditional-overwrite pattern (as its
  own text implies — "threaded through a new trailing `bool relay = false`
  ... into `rd.connection_rates[connection_id].relay`"), then any call to
  `addSubscriberConnection` for the same `(source_topic, remote_node,
  connection_id)` tuple that doesn't carry the relay bit — `decodeSubscribeRequest`
  (`:1308`, wire-triggered `RemoteSubscribeInternal`, confirmed to have no
  `relay` field) or `remoteAdvertise` (`:1665`, the `remote_subscribe`
  ROS service) — will silently reset a statically-configured `relay: true`
  back to `false`. The plan explicitly says relay is "static-config only
  for now" and correctly notes `decodeSubscribeRequest` "keeps the
  default," but doesn't flag that this means those two dynamic paths are a
  live foot-gun if ever exercised against the same tuple a hub configured
  via `topics_list` (e.g. an operator using the `subscribe` service for
  ad hoc reconfiguration on a connection that also has static relay
  config). Low probability in the star-hub topology the issue targets, but
  worth either a one-line guard (only touch `.relay` from the static-config
  path) or an explicit doc callout. — `plan.md` step 1 ("Per-destination
  opt-in")
- [ ] (suggestion) The proposed shared helper signature,
  `selectRateLimitedConnections(remote_details, now, exclude_remote)`, has
  no parameter for the additional `relay == true` per-connection predicate
  that `relayToOtherRemotes` needs (`callback()`'s call site should NOT
  apply this predicate — local-origin forwarding is unconditional). The
  plan's prose describes filtering by `relay` first and then applying
  period-gating on the surviving subset, which is the correct order — but
  if an implementer instead calls the shared helper first and filters the
  result afterward, `last_sent_time` gets bumped on `relay: false`
  connections that were never actually sent to, corrupting future
  period-gating for that destination the next time a local publish fires.
  Worth tightening the signature (e.g. an optional predicate/require-relay
  flag) so the ordering constraint isn't just implicit in prose. —
  `plan.md` step 2 ("Forwarding, called from decodeData()")
- [ ] (suggestion, doc-only) Relay runs before the stale/reorder gate in
  `decodeData()`, so a duplicate/out-of-order resend that gets dropped
  locally as stale is still relayed onward — the plan's own rationale
  ("downstream bridge assigns its own fresh sequence on forward") is
  correct and this is self-limiting (the outbound `Connection::send()`'s
  AIMD admission control, `connection.cpp:514+`, still caps the relay
  link's byte rate), but the amplification-under-resend consequence isn't
  named explicitly and would be a natural addition to `doc/relay_design.md`.
  — `plan.md` step 2

### Verified claims (no issue found)

- `Connection::send()`'s poll/retry loop (`src/connection.cpp:615-673`,
  comment block `:545-560`) is exactly as described: `poll(..., 10)` up to
  20 tries → ~200ms worst case, running outside `config_mutex_` (only
  `sent_packet_statistics_mutex_` guards the reserve/record bookkeeping).
  Today that cost is paid on `republish_group_` (Reentrant,
  `src/udp_bridge.cpp:359`, `callback()`'s subscriptions run there per
  `:1283`), never `socket_drain_group_` (MutuallyExclusive, `:358`, the
  group `decodeData()`/`spin_once()` run on via the 10ms timer, `:541`).
  Calling `send()` inline from `decodeData()` would genuinely reintroduce
  the #10 drain-thread-stall hazard; the new dedicated `relay_queue_`
  worker thread (mirroring `PublishQueue`, not a callback group) correctly
  keeps this off both `socket_drain_group_` and `republish_group_`.
- `PublishQueue` (`include/udp_bridge/publish_queue.h`) is bounded
  (byte-budget), drop-oldest, single-worker, injected-`Sink` — the pattern
  the plan proposes mirroring for `relay_queue_` genuinely avoids
  unbounded growth under sustained relay load, contingent on the
  implementer actually replicating this shape (byte bound + drop-oldest),
  which the plan states explicitly.
- Reusing `send(message_internal, connections, false)`
  (`src/udp_bridge.cpp:1506`) genuinely routes relayed traffic through the
  identical `Connection::send()` AIMD admission-control path (#43/#52) as
  local-origin traffic — confirmed by reading the template chain down to
  the per-packet `can_send()` check in `connection.cpp`. A relay hub does
  not silently bypass admission control.
- `MessageInternal.msg` genuinely carries `destination_topic`,
  `reliability`, `durability`, `history_depth` fields that can be
  swapped per destination without re-serializing the payload.
- `callback()`'s period/`last_sent_time` gating (`src/udp_bridge.cpp:748-765`)
  matches the plan's description exactly; extracting it under
  `subscribers_mutex_` (same lock both call sites already need) is
  race-free by construction — sequential exclusion via the existing
  mutex, no new lock ordering introduced.
- `.agents/README.md:92`'s per-topic parameter list is the correct,
  current insertion point for a `relay` row. Issue #53 is open and matches
  the plan's cross-reference intent. Test-file naming
  (`test_reorder_buffer.cpp`, `test_admission_control.cpp` style) matches
  the existing `test/` convention.

### Summary

The redesign genuinely resolves both round-1 must-fix findings by
construction, not by reformulation — verified independently against
current source rather than trusting the plan's citations. The threading
analysis (retry-loop cost, callback-group boundaries, new dedicated worker)
and the admission-control reuse claim both check out exactly as described.
Three suggestion-level gaps remain, none blocking: a latent overwrite-reset
risk if `relay` follows `period`'s unconditional-overwrite pattern and a
dynamic subscribe path ever touches a statically-relay-configured tuple; an
underspecified filter/gate ordering in the proposed shared rate-limiting
helper that an implementer could get backwards; and a documentation gap
around resend amplification under the before-the-stale-gate relay point
(self-limiting via existing admission control, so cosmetic). The plan's
167 lines (vs. the 30-80 guideline) are justified by the depth of
verification required after a rejected round-1 design — the excess is
concrete file:line rationale, not padding.

### Recommended Actions

- [ ] Note in `doc/relay_design.md` (or a plan addendum) whether `relay`
  should be immune from the dynamic-subscribe overwrite paths
  (`decodeSubscribeRequest`/`remoteAdvertise`), or accept the risk
  explicitly.
- [ ] Tighten `selectRateLimitedConnections`'s contract (signature or
  docstring) so the relay-flag-filter-then-gate ordering isn't only
  implicit in the plan's prose.
- [ ] Add a one-line doc note on resend-amplification-before-relay to
  `doc/relay_design.md`.
