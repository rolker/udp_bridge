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

## Plan Authored
**Status**: complete
**When**: 2026-08-23 00:05 -04:00
**By**: Claude Code Agent (Claude Opus 5 (1M context))

**Plan**: `.agent/work-plans/issue-51/plan.md` at `9244e9b`
**Branch**: feature/issue-51 at `9244e9b`
**Phases**: single PR, four atomic commits

Host-inline amendment to the round-2 plan (`361d55e`), on operator direction.
The relay opt-in flag is removed: the per-remote `topics_list` is already the
routing table (ROS 1 precedent), and the flag guarded an echo the loop rule
prevents on its own.

Evidence the flag protected nothing: this design never touches
`ignore_local_publications`, so a bridge still never hears its own republish
and a symmetric pair is bit-for-bit unchanged; and relay only alters behaviour
where a SECOND remote lists the same topic, while every config in the repo
declares exactly one remote per bridge (`config/example_params.yaml:33`,
`test/bench/configs/three_path.yaml:30,56`).

Round-2 findings 1 and 2 were both flag-plumbing costs and are resolved by
deletion: no `ConnectionRateInfo` field for a dynamic subscribe to reset, and
no relay filter to mis-order in the rate-limiting helper. Finding 3 (relay
precedes the stale/reorder gate) is carried as a `doc/relay_design.md` line.

Retained as load-bearing: the `relay_queue_` worker (`Connection::send()`'s
`sendto()` has a ~200 ms worst-case retry loop, so forwarding must stay off
the socket drain thread, per #10) and the shared `selectRateLimitedConnections`
extraction so forwarded traffic is rate-limited identically to local traffic.

### Open questions
- [ ] No open questions — plan is implementation-ready.

## Implementation
**Status**: complete
**When**: 2026-08-23 00:58 -04:00
**By**: Claude Code Agent (Claude Opus)

**Plan**: `.agent/work-plans/issue-51/plan.md` at `9244e9b`, kept in sync in-place
**Branch**: feature/issue-51
**Commits** (four atomic, in plan order):

| SHA | Commit |
|---|---|
| `1b711b4` | relay: add the relay_queue_ scaffolding (#51) |
| `374b8dc` | relay: forward received messages to the other remotes that ask for the topic (#51) |
| `0c69bb6` | test: relay routing and relay-queue regression tests (#51) |
| `47fb5db` | docs: document relay, its loop rule and its blast radius (#51) |

Not pushed; no PR opened (host publishes).

### What landed

- `include/udp_bridge/relay_queue.h` — `RelayItem` / `RelayQueue`, the same
  bounded drop-oldest single-worker component as `PublishQueue`, so
  `Connection::send()`'s ~200 ms `sendto()` retry loop never runs on the
  socket-drain thread (#10 on the send side). Separate from `publish_queue_`:
  a stalled local subscriber and a stalled remote link are independent
  failure domains.
- `include/udp_bridge/destination_selection.h` — `selectRateLimitedConnections`
  (callback()'s period/`last_sent_time` block moved verbatim, plus an
  `exclude_remote` applied *before* any stamp), `hasRelayDestination` (the
  read-only form of the same loop rule) and the shared `DestinationConfig`.
- `src/udp_bridge.cpp` — `decodeData()` probes `hasRelayDestinations()` and,
  only when some other remote lists the topic, pushes a `RelayItem` before
  the stale/reorder gate; `relayToOtherRemotes()` (the queue sink) selects
  destinations with the sender excluded and forwards through the existing
  `send()` path; `relay_queue_` lifecycle mirrors `publish_queue_`;
  `diagnoseRelayQueue()` registered in `on_configure`.
- Tests: `test/test_relay_routing.cpp` (7 cases) and
  `test/test_relay_queue.cpp` (4 cases), registered via `add_udp_bridge_gtest`.
- Docs: new `doc/relay_design.md`; `README.md`, `doc/conceptual_overview.md`,
  a commented second-remote block in `config/example_params.yaml`, and
  `.agents/README.md` (relay worker in the threading overview, the two new
  test targets, the design doc, and a no-parameter/no-cycle-protection
  pitfall).

Constraints held: no new config parameter (`types.h` untouched,
`addSubscriberConnection` signature unchanged, `ignore_local_publications`
unchanged); forwarding never runs on the drain thread; rate limiting is one
shared implementation, not a parallel path.

### Divergences from the plan (all recorded inline in plan.md, `374b8dc`)

1. `selectRateLimitedConnections` is a **free function in a new header**
   (`destination_selection.h`), not a private member of `UDPBridge`. It needs
   no member state, and as a free function the routing decision — which is
   all relay is — is unit-testable without a node or socket, matching the
   `qos_resolution.h` / `giveup_diagnostic.h` / `subscriber_registry.h`
   precedent in this repo. `DestinationConfig` (was local to `callback()`)
   moved with it.
2. `relayToOtherRemotes(RelayItem&& item)` takes the queue item (carrying
   exactly the planned topic / outer_message / source_node) so it serves
   directly as the queue sink, mirroring `publishItem`.
3. Added `hasRelayDestinations` / `hasRelayDestination`: a cheap probe so the
   drain thread skips the payload copy entirely when nothing can be relayed —
   which is every configuration in this repo today.
4. Added, beyond the plan: the relay queue's byte budget is the compile-time
   `kRelayQueueMaxBytes` (64 MiB) rather than a ROS parameter (relay adds no
   configuration surface), and a `relay queue` diagnostic task mirroring
   `diagnosePublishQueue` — a relay drop is unrecoverable (the message never
   reached a `Connection`, so the resend layer has nothing to retransmit).

### Verification

Build (`./build.sh udp_bridge`, exit code checked directly, not through a pipe):

```
BUILD_EXIT=0
Summary: 1 package finished [39.9s]
```

Tests (`./test.sh udp_bridge` then `colcon test-result --verbose`):

```
TEST_EXIT=0
Summary: 191 tests, 0 errors, 0 failures, 14 skipped
```

Relay suites specifically: `test_relay_routing.gtest.xml` — 7 tests, 0
failures; `test_relay_queue.gtest.xml` — 4 tests, 0 failures.

**Mutation check of the loop rule** (the deliverable required the echo test to
fail if the loop rule is removed): with the `exclude_remote` filter deleted
from `selectRateLimitedConnections` and the `source_node` comparison deleted
from `hasRelayDestination`, the suite went to `191 tests, 0 errors, 5
failures`, including `RelayRouting.EchoIsNeverSentBackToTheSender`. The
mutation was reverted and the suite is green again.

`pre-commit run --from-ref origin/jazzy --to-ref HEAD` (workspace `.venv`
binary; `pre-commit` is not on PATH): all hooks Passed or Skipped
(no-files-to-check).

### Notes for review

- The loop rule compares `SourceInfo::node_name` with the `remote_details`
  key. Re-confirmed the same-namespace premise beyond the plan's citation:
  `unwrap()` sets `node_name` from `wrapped_packet->source_node`
  (`src/udp_bridge.cpp:1462`), `remote_nodes_` is keyed by it
  (`:1474`/`:1486`), the config path keys remotes by their `remotes_list`
  label (`:405`), and the dynamic subscribe path calls
  `addSubscriberConnection(..., source_info.node_name, ...)` (`:1452`) — one
  namespace, plain string compare.
- Relayed traffic is added to the topic's `MessageStatistics` per destination,
  as `callback()` does, so relayed bytes are visible in
  `~/topic_statistics` / `BridgeInfo` rather than invisible.
- The three-node deliverable's "hub also locally subscribed" half is covered
  structurally rather than by an end-to-end assertion: relay is an additional
  `relay_queue_` push in `decodeData()`, not a branch around the
  `publish_queue_` push, and the test asserts the probe says "relay" for
  exactly that configuration. A live three-node hub run belongs to the #18
  bench harness (`test/bench/`), which is opt-in and needs `unshare -Urn`; not
  attempted here.

### Deferred / not done

- No bench-harness scenario for a live three-node hub (see above).
- Cycle tolerance (hop counter or origin stamp in `MessageInternal`) is a
  wire-format change and stays out of scope; the star-only constraint is
  documented instead.
- Not pushed, no PR — per the handoff contract.

## Local Review (Pre-Push)
**Status**: complete
**When**: 2026-08-23 01:14 -04:00
**By**: Claude Code Agent (Claude Opus)
**Verdict**: changes-requested

**Branch**: feature/issue-51 at `cb3f738`
**Mode**: pre-push
**Depth**: Deep (reason: new cross-thread worker + shared rate-limit state mutated from two threads + refactor of a working field send path)
**Must-fix**: 4 | **Suggestions**: 9
**Round**: 1 | **Ship**: continue — three precise fixes plus one genuine design question (relay ordering vs the stale gate) that warrants an operator decision, not another blind round.

**Specialists**: Static Analysis (cppcheck + yamllint; ament_cpplint unavailable — no new findings); Copilot off (default); Local Adversarial skipped (Ollama llama-server OOM-killed on both attempts, and the diff exceeds the default 32k num_ctx). Two Claude Adversarial passes (Lens A / Lens B) were dispatched but **neither returned before this entry was written** — no specialist report contributed to it. Every finding below is the lead reviewer's own, and each was verified directly against source in this session (file and line cited per finding); none is carried on an unreturned subagent's authority. A later adversarial report, if it lands, should be reconciled as a separate entry.

**Verification re-run independently**: `./build.sh udp_bridge` exit 0; `./test.sh udp_bridge` exit 0; `colcon test-result` 191 tests, 0 errors, 0 failures, 14 skipped. Relay suites confirmed in the XML: test_relay_routing 7/7, test_relay_queue 4/4. Commit identity correct on all 14 commits; no issue-closing keywords anywhere.

### Verified sound (the high-risk items from the brief)

- **callback() extraction is behaviour-identical.** `selectRateLimitedConnections` diffed token by token against the pre-#51 inline block: the `period >= 0` skip, the `period == 0 || last_sent_time.nanoseconds() == 0 || now - last_sent_time > Duration::from_seconds(period) || periods.count(period) > 0` chain, the stamp and the conditional `periods.insert` are character-identical; `periods` remains per-remote in scope; `destination_config_by_remote` is still populated for all remotes; `SelectedConnections` and `RemoteConnectionsList` are the same type. Lens A concurred independently.
- **Concurrency and lock discipline are correct.** Both writers of `last_sent_time` mutate under `subscribers_mutex_`; no reference or iterator outlives the lock (value copies out, re-`find()` under the lock for post-send statistics). `subscribers_mutex_` is `mutable`, so the `const` probe is well-formed. No lock nesting between the queue mutex, `subscribers_mutex_` and `send()`'s `scoped_lock` — no deadlock path found.
- **Drain thread stays clear** of `send()`; the probe is a `find()` plus a one-entry walk.
- **RelayQueue is a faithful PublishQueue mirror** (diffs to comments only): bounded, drop-oldest, non-blocking push, balanced byte accounting, declared last for destruction order, lifecycle wired configure/activate/deactivate/cleanup. 64 MiB cannot wedge the bridge.
- **The loop rule is genuinely bound by the tests** for wrapped traffic; the implementer's mutation check reproduces.
- **Doc claims spot-checked against source**: `ignore_local_publications` untouched and still true; the param path defaults `destination` to `source`; `remotes.<label>.name` really is never read; relay really precedes the gate.

### Findings

- [x] (must-fix) `relayToOtherRemotes` never rewrites `source_topic`, so with an empty per-destination `destination_topic` the downstream bridge resolves to the upstream publisher's topic instead of the hub-local topic — reachable via `remoteAdvertise` / `decodeSubscribeRequest`, which pass `destination_topic` through with no source fallback — `src/udp_bridge.cpp:1093`
- [x] (must-fix) `catch(const std::exception&)` cannot catch `ConnectionException` (no base class, `connection.h:22`), which `Connection::send()` throws on the ordinary Timeout path, so relay send failures are swallowed silently by RelayQueue's `catch(...)`; the single try also wraps the whole loop, so one stalled remote aborts the relay to all the others — `src/udp_bridge.cpp:1097`
- [x] (must-fix) The relay probe keys on `source_info.node_name`, empty for any packet that did not pass through `unwrap()`, so an injected bare Data packet is relayed to every remote listing the topic with the loop rule inert — including in single-remote configs the doc calls "bit-for-bit unchanged" — `src/udp_bridge.cpp:937`
- [x] (must-fix, needs operator decision) Relay before the stale gate plus fresh hub sequence numbers makes a superseded upstream resend undetectable downstream, so older data republishes as newest; `relay_design.md`'s first bullet states the mechanism but draws the opposite conclusion — `src/udp_bridge.cpp:937`
- [x] (suggestion) The two `///` blocks run together, so Doxygen attaches `selectRateLimitedConnections`'s `@param` text to `hasRelayDestination` — `include/udp_bridge/destination_selection.h:33`
- [x] (suggestion) `relay_item.message = outer_message;` is a full payload copy on the socket-drain thread, contradicting the comment and `relay_design.md` §Mechanism — `src/udp_bridge.cpp:942`
- [x] (suggestion) The same-period grouping doc claim is false: grouping is map-order dependent, and `SamePeriodConnectionsSendAsAGroup` only exercises the due-first ordering — `include/udp_bridge/destination_selection.h:53`
- [x] (suggestion) Probe and selector disagree: `hasRelayDestination` ignores `connection_rates`, so a `period < 0` remote still costs a copy, a queue slot and a wakeup — `include/udp_bridge/destination_selection.h:75`
- [x] (suggestion) Relayed bytes go into the topic's statistics with no received-size seed, conflating relayed and local-origin traffic in `topic_statistics` — `src/udp_bridge.cpp:1113`
- [x] (suggestion) Security section omits that `source_node` is an attacker-controllable wire field, and that relayed packets occupy each Connection's `sent_packets_` resend buffer — `doc/relay_design.md`
- [x] (suggestion) `kRelayQueueMaxBytes` is fixed while the sibling `publish_queue_max_bytes` is a tunable parameter, though the relay queue carries the hub's whole downstream fan-out — `include/udp_bridge/udp_bridge.h:533`
- [x] (suggestion) Missing the `test_publish_queue` parity cases most relevant here, notably `ThrowingSinkDoesNotKillWorker` — `test/test_relay_queue.cpp`
- [x] (suggestion) yamllint comment-spacing/indentation warnings in the new commented block — `config/example_params.yaml:97`

### Plan adherence

All eight planned files changed, four atomic commits in planned order, four recorded divergences accurate and independently verified. The three-node test's "hub also locally subscribed" half is structural rather than end-to-end; accepted — relay is an added `if` with no early return, so the local path is unchanged by construction and there is no unit-testable seam without a live node. It should be a filed follow-up issue against the #18 bench harness rather than only a progress.md note. Separately, nothing tests above the pure helper: `relayToOtherRemotes`'s field rewriting and the topic round trip are untested, which is exactly where must-fix 1 sits.

## Implementation
**Status**: complete
**When**: 2026-08-23 17:42 -04:00
**By**: Claude Code Agent (Claude Opus)

**Branch**: feature/issue-51 at `7b9f708`
**Addressed**: `## Local Review (Pre-Push)` of 2026-08-23 01:14 -04:00 (branch at `cb3f738`), verdict changes-requested — all 4 must-fix and all 9 suggestions
**Commits** (twelve atomic, in the order below): `0be0a66`, `3932e85`, `a9d1447`, `b6dc656`, `f7b67c5`, `c7e7462`, `7cbef5f`, `b6b0a92`, `2a7a8a0`, `0bd1982`, `6cbee45`, `7b9f708`

Not pushed; no PR opened (host publishes).

### Actions

Must-fix:

- [x] `relayToOtherRemotes` never rewrites `source_topic` — `src/udp_bridge.cpp:1093` — **fixed** (`a9d1447`). Resolved *against* the old code comment: `source_topic` is now the hub-local topic. The receiver resolves `destination_topic`, falling back to `source_topic`, and an empty `destination_topic` is ordinary (`remoteAdvertise` / `decodeSubscribeRequest` pass it through), so the old behaviour sent that fallback to the upstream publisher's topic — two boats publishing `/nav`, remapped by the hub, collide back onto `/nav`. Rewriting also makes a relayed message identical in shape to a locally-originated one (`callback()` sets `source_topic` to the sending bridge's own topic), and provenance is unaffected: the immediate sender travels in the wrapped packet's `source_node`. Field rewriting extracted to `applyRelayDestination` (new `include/udp_bridge/relay_item.h`, which also now owns `RelayItem`); comment and `doc/relay_design.md` rewritten to match. Tests: `RelayRouting.RelayRewritesSourceTopicToTheHubTopic`, `RelayRouting.RelayAppliesPerDestinationTopicAndQos`.
- [x] `catch(const std::exception&)` cannot catch `ConnectionException`; one stalled remote aborts relay to all — `src/udp_bridge.cpp:1097` — **fixed** (`3932e85`). New `include/udp_bridge/relay_send.h::sendToEachDestination` wraps each destination individually with `ConnectionException` / `std::exception` / `...` arms and reports through an error callback; the outer handler gained a `ConnectionException` arm for the selection phase. Tests: new `test/test_relay_send.cpp` (4 cases) — `ConnectionExceptionIsCaught`, `StdExceptionAndUnknownThrowsAreAlsoReported`, `OneFailingDestinationDoesNotCancelTheOthers`, `EveryDestinationIsSentToWhenNothingThrows`.
- [x] Loop rule inert when `source_info.node_name` is empty — `src/udp_bridge.cpp:937` — **fixed** (`0be0a66`). `hasRelayDestination` returns false for an empty sender (a rule that excludes by name excludes nobody when there is no name), and `relayToOtherRemotes` restates the guard at the sink, which is reachable independently of the probe. The packet is still published locally, exactly as before #51. Test: `RelayRouting.UnnamedSenderIsNeverRelayed`; **mutation-checked** — deleting the guard fails it.
- [x] Relay before the stale gate (needs operator decision) — `src/udp_bridge.cpp:937` — **fixed** (`b6dc656`) per the operator's decision: **relay only what the gate admits**. `decodeData()` now only probes; the relay form rides inside the `PublishItem` as an optional `RelayItem` (null whenever relay is unreachable — every configuration in this repo today, so the ordinary path allocates nothing), which is what lets the reorder buffer carry it: a held packet keeps its relay form and is forwarded when — and only when — the buffer releases it, in wire order, and never at buffer time (it can still be dropped). `UDPBridge::enqueuePublish()` is the single admit-to-publish point and the only place a relay item reaches `relay_queue_`; every admit path goes through it (`decodeData`'s immediate path, a gap-filler's release, `flushExpiredBuffer`'s window expiry). `clearReorderBuffer` discards held packets without publishing, so those are not relayed either. Docs corrected to the new behaviour — `relay_design.md`'s ordering section rewritten (it had stated the fresh-sequence-number mechanism and drawn the opposite conclusion from it), plus `conceptual_overview.md` and `.agents/README.md`. Tests: `ReorderBufferFixture.StalePacketReleasesNoRelay`, `BufferedPacketKeepsItsRelayFormUntilRelease`, `ExpiredBufferReleaseKeepsItsRelayForm`; **mutation-checked** — resetting `item.relay` when the buffer stores the item fails two of them.

Suggestions:

- [x] Doxygen blocks run together — `include/udp_bridge/destination_selection.h:33` — **fixed** (`f7b67c5`): `hasRelayDestination`'s doc and definition moved above `selectRateLimitedConnections`' doc block, so each block precedes its own function. Comment reordering only.
- [x] `relay_item.message = outer_message;` is a full payload copy on the drain thread — `src/udp_bridge.cpp:942` — **fixed** (`b6dc656`, folded into the gate-ordering restructure, which is what made it possible): `outer_message` is moved into the relay form after the serialized payload copy has been taken, so the code now does what the comment and `relay_design.md` §Mechanism claimed. The lambda is called at most once per `decodeData` invocation and nothing reads `outer_message` afterwards; noted in the comment.
- [x] Same-period grouping doc claim is false — `include/udp_bridge/destination_selection.h:53` — **fixed** (`c7e7462`): the comment now states the grouping is iteration-order dependent (a not-yet-due connection visited before the first due one with that period is skipped). The behaviour is pre-#51 and preserved verbatim; new `RelayRouting.SamePeriodGroupingFollowsIterationOrder` pins it, since the existing case only exercised the due-first ordering.
- [x] Probe and selector disagree on `period < 0` — `include/udp_bridge/destination_selection.h:75` — **fixed** (`7cbef5f`): the probe now applies both of the selector's *structural* filters (loop rule, `period >= 0`), so a never-send remote (or one with no connections) no longer costs a copy, a queue slot and a wakeup. It deliberately does not apply the *temporal* part of the rate limit, which owns the clock and mutates `last_sent_time`: the probe is may-send, never must-send — stated in the doc block. Tests: `ProbeAgreesWithSelectorOnNeverSendRemotes`, `ProbeIgnoresRemotesWithNoConnections`, each asserting the selector's answer as the precondition.
- [x] Relayed bytes conflated with local-origin in `topic_statistics` — `src/udp_bridge.cpp:1113` — **documented, not restructured** (`2a7a8a0`). New `## Statistics` section in `relay_design.md` plus a code comment: the two are summed per topic, the per-destination `send_results` breakdown still separates *where* traffic went, and separating *where it came from* needs a per-source dimension in `MessageStatistics` — a `TopicStatistics` wire-format change, out of scope for #51. Keeping relayed bytes visible-but-summed beats making them invisible.
- [x] Security section omits the attacker-controllable `source_node` and the `sent_packets_` exposure — `doc/relay_design.md` — **fixed** (`2a7a8a0`): two bullets added — `source_node` is a sender-chosen wire field (an injected packet can claim any sender, suppressing relay to that one remote and reaching the rest; an unwrapped one claims none, which is why must-fix 3 refuses those outright), and relayed packets occupy every outgoing `Connection`'s `sent_packets_` resend buffer for `kSentPacketTTL`, so injected traffic consumes downstream resend state, not just bandwidth. Both tied to open #53.
- [x] `kRelayQueueMaxBytes` fixed while the sibling is tunable — `include/udp_bridge/udp_bridge.h:533` — **fixed** (`6cbee45`): now the `relay_queue_max_bytes` parameter, same default (64 MiB) and same 1 MiB–2 GiB clamps as `publish_queue_max_bytes`, declared and clamped identically in `on_configure`, and reported by the relay-queue diagnostic. A fixed value was a capability limit on the one queue that carries a hub's whole downstream fan-out. Separate knob rather than a reuse of `publish_queue_max_bytes` because the two queues are independent failure domains. `.agents/README.md` parameter table and `doc/relay_design.md` updated. (This reverses a plan divergence — "relay adds no configuration surface" — recorded inline in `plan.md`.)
- [x] `test_relay_queue` missing the `test_publish_queue` parity cases — `test/test_relay_queue.cpp` — **fixed** (`b6b0a92`): added `OversizeItemEnqueuedThenEvicted`, `StopWhileSinkBlockedJoinsAfterSinkReturns`, `ThrowingSinkDoesNotKillWorker`, plus a relay-specific `ConnectionExceptionInSinkDoesNotKillWorker` — the barrier exercised against the exception the relay sink actually throws (no base class, catchable only by `catch(...)`). 4 → 8 cases.
- [x] yamllint warnings in the new commented block — `config/example_params.yaml:97` — **partly fixed, partly declined** (`0bd1982`). Fixed: both `too few spaces before comment` warnings (lines 40 and 97). Declined: the `comments-indentation` warning on the commented second-remote block — the block is a sibling of `robot_a:` at that indentation and is the last thing in the file, so yamllint compares it against the *preceding* content (indent 16). The only ways to silence it are to indent a copy-pasteable example deeper than the level it belongs at, or to move a ~50-line commented aside ahead of the real configuration; neither is worth it for a style warning (yamllint exits 0). Not suppressed with a disable comment.

### Verification

Build (`./build.sh udp_bridge`, exit code read directly):

```
BUILD_EXIT=0
Summary: 1 package finished
```

Tests (`./test.sh udp_bridge` then `colcon test-result --verbose`):

```
TEST_EXIT=0
Summary: 209 tests, 0 errors, 0 failures, 14 skipped
```

(191 before this pass → 209; the four relay suites in the XML: `test_relay_routing` 13/13, `test_relay_queue` 8/8, `test_relay_send` 4/4, `test_reorder_buffer` 17/17, all 0 failures.)

`pre-commit run --from-ref origin/jazzy --to-ref HEAD` (workspace `.venv` binary; `pre-commit` is not on PATH): all hooks Passed or Skipped (no-files-to-check) — including yamllint and check-yaml.

Mutation checks run and reverted (each new behavioural guard has a test that fails without it):

- Empty-sender guard deleted from `hasRelayDestination` → `RelayRouting.UnnamedSenderIsNeverRelayed` FAILED.
- `item.relay.reset()` inserted where the reorder buffer stores the item → `BufferedPacketKeepsItsRelayFormUntilRelease` and `ExpiredBufferReleaseKeepsItsRelayForm` FAILED.

### Notes for the re-review

- New headers: `include/udp_bridge/relay_item.h` (`RelayItem`, moved out of `relay_queue.h`, plus `applyRelayDestination`) and `include/udp_bridge/relay_send.h` (`sendToEachDestination`). `publish_queue.h` now includes `relay_item.h` for `PublishItem::relay`.
- `PublishItem::byte_size()` counts an attached relay form, so the publish queue's byte budget accounts for memory it actually holds. Zero effect where relay is unreachable (the pointer is null), and `test_publish_queue`'s expectations are unchanged.
- One review note was *not* a checkbox and is left for the host: the review suggested filing a follow-up issue against the #18 bench harness for the live three-node hub scenario, rather than leaving it as a progress.md note. No issue was filed from here (no push / no GitHub writes per the handoff contract).
