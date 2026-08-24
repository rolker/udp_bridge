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

## Local Review (Pre-Push)
**Status**: complete
**When**: 2026-08-23 17:58 -04:00
**By**: Claude Code Agent (Claude Opus)
**Verdict**: changes-requested

**Branch**: feature/issue-51 at `0e87d79`
**Mode**: pre-push
**Depth**: Deep (reason: new worker thread on an unauthenticated transport + restructure of the receive/admit path + three new headers unreviewed before this round)
**Must-fix**: 3 | **Suggestions**: 11
**Round**: 2 | **Ship**: recommended — all three must-fixes are sentence-level documentation/example corrections with no code change required; the code itself is sound and the round-1 must-fix count fell 4 → 0 on code.

**Specialists**: Static Analysis (cppcheck: no findings on any new or changed file; yamllint under the repo's own pre-commit config: exit 0, one `comments-indentation` warning — see the decline assessment); Governance + Plan Drift (lead); two Claude Adversarial passes (Lens A logic/correctness, Lens B systemic/safety) both returned and contributed. Copilot off (default). Local Adversarial skipped: Ollama unavailable on this host (OOM).

**Verification re-run independently**: `./build.sh udp_bridge` exit 0; `./test.sh udp_bridge` exit 0; `colcon test-result --verbose` exit 0 — **209 tests, 0 errors, 0 failures, 14 skipped**, confirmed from the per-suite XML (`test_relay_routing` 13, `test_relay_queue` 8, `test_relay_send` 4, `test_reorder_buffer` 17, `test_publish_queue` 8, all 0 failures). Commit identity correct on all 29 commits; no issue-closing keywords anywhere in the range.

### Round-1 must-fixes: all four verified real, not reworded

- **1. `source_topic` rewrite** — real. `applyRelayDestination` (`include/udp_bridge/relay_item.h:76`) sets `source_topic = hub_topic` before `destination_topic`/QoS. Traced the receiver side: `decodeData` resolves `destination_topic` else `source_topic` (`src/udp_bridge.cpp:950-952`), and `decodeSubscribeRequest`/`remoteAdvertise` pass an empty `destination_topic` through with no fallback of their own — so the fallback now lands on the hub's name. Provenance claim also holds: `source_node` is a wrapped-packet header field (`packet.h:102`, set in `unwrap`), untouched by the rewrite.
- **2. Per-destination failure isolation** — real. `ConnectionException` genuinely has no base class (`connection.h:22`) and is what `Connection::send()` throws (`connection.cpp:653,658,672,674`); `UDPBridge::send()` has no try/catch, so it propagates. `sendToEachDestination` (`relay_send.h:38`) wraps each destination separately with `ConnectionException` / `std::exception` / `...` arms and reports via `on_error`, which logs at ERROR with remote, topic and sender (`udp_bridge.cpp:1171-1176`). `OneFailingDestinationDoesNotCancelTheOthers` binds the isolation.
- **3. Empty `source_node`** — real, and no path bypasses it. `hasRelayDestination` returns false for an empty sender (`destination_selection.h:66-67`), which is the only thing that sets `PublishItem::relay` (`udp_bridge.cpp:992`), and `relayToOtherRemotes` restates the guard at the sink (`udp_bridge.cpp:1094`). No other construction site of `RelayItem` exists outside tests.
- **4. Single admit-to-publish point** — **verified exhaustively.** `grep` over `src/` + `include/` shows `publish_queue_.push` occurs at exactly one site, inside `enqueuePublish` (`udp_bridge.cpp:1211`), and `relay_queue_.push` at exactly one site, immediately above it. `enqueuePublish` has three callers and they are the three admit paths: `decodeData`'s immediate path (`1057`), `admitOrBuffer`'s `to_publish` (`1036`), and `flushExpiredBuffer` in `spin_once` (`769`). Read `remote_node.cpp:390-505`: every `AdmitDecision::Drop` return leaves `to_publish` empty, so a gated packet cannot be relayed by any route; `clearReorderBuffer` (`529`) erases without returning items. A buffered item is stored by move with its `relay` pointer intact and released in wire order. On the ordinary path `relay` is null, so nothing is allocated. Byte accounting: `PublishItem::byte_size()` adds the relay form, `enqueuePublish` moves the `RelayItem` out and `reset()`s the pointer *before* `publish_queue_.push`, so there is no double count and no leak — though the consequence is that the added term is unreachable in production (see suggestion 4).

### Also verified sound

- **`callback()` rate-limiting is still behaviour-identical.** `selectRateLimitedConnections` (`destination_selection.h:139-153`) is a verbatim transcription of the removed inline block, including the order-dependent `periods` grouping; the `period < 0` change this round landed only in the *probe* (`hasRelayDestination`), not the selector. `destination_config_by_remote` is still populated for every remote, unconditionally, as before. Lens A concurred independently.
- **Lock discipline.** No lock is held across I/O on the relay path; `subscribers_mutex_` is scoped to selection and re-taken briefly for statistics; `send()` takes only `remote_nodes_mutex_` / `pending_connections_mutex_`, never `subscribers_mutex_` — no nesting, no ordering hazard against the drain thread's probe. `relay_queue_` is declared last, so its stop+join precedes destruction of the maps its sink touches.
- **Lens B's deadlock hypothesis is disproved.** It flagged `get_current_state()` on the relay worker as deadlocking `on_deactivate`'s join. Disassembled `librclcpp_lifecycle.so` on this jazzy install: `LifecycleNodeInterfaceImpl::get_current_state()` is `lea 0x120(%rdi),%rax; ret` — lock-free. No deadlock. The residual concern is only the unsynchronized read (suggestion 1).
- Documentation consequences are carried: `README.md`, `doc/conceptual_overview.md`, `doc/relay_design.md` (310 lines), `.agents/README.md` (test-target list, architecture note, **and the verified-parameter table row for `relay_queue_max_bytes`**). The parameter is declared, clamped 1 MiB–2 GiB identically to its sibling, logged, and surfaced in the `relay queue` diagnostic. `publish_queue_max_bytes` is likewise absent from `example_params.yaml`, so the omission there is consistent, not a gap.

### Assessment of the two declines

- **`topic_statistics` relayed-vs-local conflation — decline is reasonable.** Separating source really does need a per-source dimension in `MessageStatistics`, i.e. a `TopicStatistics` wire-format change, correctly out of scope. But the documented claim is not quite right: see suggestion 3.
- **yamllint `comments-indentation` — decline is reasonable.** Reproduced under the repo's own pre-commit args (`line-length: 120`, `document-start: disable`): exit 0, warning only, and the hook passes. The alternatives (indenting a copy-pasteable example below its real level, or hoisting a 50-line aside above the live config) are worse than the warning. No suppression comment was added, which is the right call.

### Findings

- [x] (must-fix) The loop rule compares the wire `source_node` against `remote_details` keys, which for statically-configured remotes are the `remotes_list` **labels** (`udp_bridge.cpp:436,559`) — `remotes.<label>.name` is declared in the example but never read anywhere. If a remote's own `name` parameter differs from the hub's label for it, the loop rule matches nothing and the hub echoes the sender's own traffic back to it, which is exactly what `relay_design.md` promises cannot happen ("the same key `remote_nodes_` is keyed by, so the comparison is a plain string compare in a single namespace" — there are two namespaces). `config/example_params.yaml:34` ships the mismatch as the example (`robot_a` label, `name: "robot_a_bridge"`). Document the label == remote-`name` invariant in `doc/relay_design.md` and drop or correct the unread `name:` key — `doc/relay_design.md:113-118`, `config/example_params.yaml:34`
- [x] (must-fix) The Security section's "The topic lists are the access-control list, such as it is" is not accurate: `decodeSubscribeRequest` feeds wire-supplied `source_topic` / `destination_topic` / `period` straight into `addSubscriberConnection` (`udp_bridge.cpp:1529`), which does `subscribers_[source_topic].remote_details[remote_node]` (`1406-1409`) and creates topic keys that were never configured — so any reachable host self-enrolls as a relay destination, and a spoofed `source_node` can overwrite a legitimate remote's entry (a negative `period` silently cuts that operator's relayed feed). The rest of that section is careful and honest; this one sentence overstates the hub operator's control — `doc/relay_design.md:275-278`
- [x] (must-fix) The operator-facing relay rule in the example config is wrong: "a relay hub for any topic **both remotes carry**" is false for this very file — `robot_a`'s `topics_list` keys `subscribers_` on its `source:` names (`/odometry`, `/battery_state`), never on `/shoreside/robot_a/odometry`, so the two remotes share no routing-table key. Relay needs only the *receiving* remote to list the hub-local topic as its `source:`; the sender need not appear in that topic's table at all — `config/example_params.yaml:33-36`
- [x] (suggestion) `relayToOtherRemotes` reads the lifecycle `current_state_` from the relay worker while the executor thread can be writing it — unsynchronized, and redundant since `on_deactivate`'s `stop()` joins the worker; `publishItem` pointedly makes no such call, so the pattern is new and asymmetric — `src/udp_bridge.cpp:1086`
- [x] (suggestion) Four early returns discard an already-dequeued relay item without counting it (not ACTIVE, empty sender, topic torn down, nothing due), so `relay_queue_.dropped_count()` and the "relay queue dropping" WARN under-report real relay loss — `src/udp_bridge.cpp:1086,1094,1103,1129`
- [x] (suggestion) The relay path omits `callback()`'s once-per-message `send_results[""][""]` seed (`udp_bridge.cpp:818`), so the aggregate `destination_node == ""` row counts only locally-originated messages; the adjacent comment and `relay_design.md` § Statistics say relayed and local traffic are "summed", which holds only for the per-destination rows — `src/udp_bridge.cpp:1157-1169`, `doc/relay_design.md:229-243`
- [x] (suggestion, cross-pass confirmed by both adversarial lenses) The rationale says reconstructing the `MessageInternal` at release "would mean keeping the payload twice over" — the chosen design keeps it twice too (`PublishItem::message` plus `relay->message.data`) for as long as the item sits in the reorder buffer, which is bounded per topic but not by bytes; a relaying hub's held-packet memory doubles with no accounting and no mention where the hold window is sized — `include/udp_bridge/publish_queue.h:40-54`
- [x] (suggestion) The relay probe takes `subscribers_mutex_` before the stale gate, so every packet the reorder-disabled fast path drops now pays for it, weakening the "gate before the payload copy so a dropped packet costs nothing" property that path's own comment advertises; moving the probe below that gate would restore it (the reorder path must keep it where it is) — `src/udp_bridge.cpp:962`
- [x] (suggestion) Nothing binds that `applyRelayDestination` is called *inside* the per-destination lambda: it is tested standalone and `sendToEachDestination` is tested with stub sinks, so hoisting the rewrite out of the loop — every destination getting one remote's topic and QoS, a classic fan-out bug — would fail no test — `test/test_relay_routing.cpp`, `src/udp_bridge.cpp:1146-1150`
- [x] (suggestion) Failure isolation is per *remote*, not per connection: `UDPBridge::send()` has no try/catch around its `connection->send()` loop, so a throw on a remote's first connection skips its remaining redundant connections and its statistics record — `src/udp_bridge.cpp:1815`
- [x] (suggestion) The two `last_sent_time` assertions in `UnnamedSenderIsNeverRelayed` are vacuous — the test never calls the selector, so they pass for any implementation; the `EXPECT_FALSE` above them is what binds the guard — `test/test_relay_routing.cpp:151-152`
- [x] (suggestion) Comment names the sink `UDPBridge::relayItem`; it is `relayToOtherRemotes` — `include/udp_bridge/relay_queue.h:150`
- [x] (suggestion) Two `PublishQueue`-inherited hardening gaps are now duplicated in a second queue: concurrent `stop()` callers can both reach `worker_.join()` (`running_` is cleared only after the join), and `configure()` writes `sink_`/`max_bytes_` unlocked against `run()`'s unlocked read — safe only by the "call once before start()" comment; an `assert(!running_)` would enforce it. Also `on_shutdown` stops neither worker — `include/udp_bridge/relay_queue.h:48-52,74-89`, `src/udp_bridge.cpp:686`
- [x] (suggestion) Plan drift: `plan.md` still carries "No new parameter: … `.agents/README.md`'s verified-parameter table needs no row", contradicting the revision recorded above it and the row actually shipped; `relay_item.h` and `relay_send.h` are absent from the Files-to-Change table — `.agent/work-plans/issue-51/plan.md:193-194,180-193`

### Plan adherence

Plan was updated in-branch (`7b9f708`) for both reversals — gate ordering and the relay-queue parameter — and both revisions match what is built. The stale "No new parameter" sentence in Files-to-Change is the only residue. Two new headers arrived that the plan does not list; both are extractions the review asked for, not scope creep. Commits remain atomic and in a defensible order.

## Implementation
**Status**: complete
**When**: 2026-08-23 18:37 -04:00
**By**: Claude Code Agent (Claude Opus)

**Branch**: feature/issue-51 at `a3a6917`
**Addressed**: `## Local Review (Pre-Push)` of 2026-08-23 17:58 -04:00 (branch at `0e87d79`), round 2 — 3 must-fixes, 11 suggestions
**Commits**: e317ba5 07539ed 0d60fda 4984d51 ac4290d d9b059d e89a8ec e2615ce 6312636 f296f76 5d1cd53 a3a6917

**Verification**: `./build.sh udp_bridge` exit 0; `./test.sh udp_bridge` exit 0; `colcon test-result --verbose` exit 0 — **231 tests, 0 errors, 0 failures, 14 skipped** (was 209 before this pass). `pre-commit run --from-ref origin/jazzy --to-ref HEAD`: all hooks Passed. Not pushed; no PR.

### Must-fix 1 was re-classified as a code defect, not a doc correction

The review filed it as "document the label == remote-`name` invariant and drop
the unread `name:` key". The host disagreed and verified why, and the
verification holds against source: `subscribers_[topic].remote_details` is
keyed by the `remotes_list` **label** (`udp_bridge.cpp:436,559`), the arriving
packet carries the sender's **self-declared** name
(`WrappedPacket::source_node`), and `remotes.<label>.name` was declared and
never read. Whenever the two differ the loop rule's string compare matches
nothing and the hub relays the message straight back to its sender — with a
**single** remote configured, which falsifies `relay_design.md`'s claim that a
symmetric pair is bit-for-bit unchanged, the claim used to justify shipping
without a relay opt-in flag.

Of the host's two options, (b) — single namespace, fail loud — was taken, in
the form that makes the namespace genuinely single rather than mapping wire
names back to labels at use time: resolve the identity once at configure time
(`remotes.<label>.name`, else the label; `remote_identity.h`), so the label
survives only as a parameter-path spelling and every downstream key is the
wire name. (a) was rejected because a wire-name→label map leaves the two
namespaces in place and only patches the one comparison, while `remote_nodes_`
would still grow a second entry for the same remote.

Fail-loud, in two places: two labels resolving to one wire name fail the
`on_configure` transition (they would otherwise collide in `remote_nodes_` and
silently overwrite a remote's connections); and a sequenced packet from a
sender matching no configured remote logs a throttled WARN naming the
parameter to set. The WARN is not a refusal because genuinely dynamic remotes
(CONNECT / `add_remote` / an unconfigured host's subscribe request) legitimately
arrive the same way — refusing would break them. It is the only signal an
operator gets that a mismatch is live.

### Actions

- [x] (must-fix) Loop rule compares wire `source_node` against config labels — fixed as a code defect, see above — `src/udp_bridge.cpp:431-464`, `include/udp_bridge/remote_identity.h`, `doc/relay_design.md`, `config/example_params.yaml` (`e317ba5`, `07539ed`, `0d60fda`)
- [x] (must-fix) Security section overstates operator control — `decodeSubscribeRequest` lets any reachable host self-enrol as a relay destination and a spoofed `source_node` can overwrite a legitimate remote's entry; stated plainly and tied to #53 — `doc/relay_design.md` (`07539ed`)
- [x] (must-fix) "a relay hub for any topic both remotes carry" is wrong; relay needs only the *receiving* remote to list the hub-local topic as its `source:` — corrected in three places in the example, including the operator_1 block — `config/example_params.yaml` (`0d60fda`)
- [x] (suggestion) Unsynchronized lifecycle read on the relay worker — **removed** rather than synchronized: the worker runs only between `start()` and the `stop()` that joins it, and during the transition the state reads DEACTIVATING, which would abandon an in-flight item the join is willing to wait for — `src/udp_bridge.cpp` (`e2615ce`)
- [x] (suggestion) Four uncounted relay drops — `RelayDropCounters` (`relay_drops.h`) records a reason at each early return; `diagnoseRelayQueue` sums queue drops and sink drops for the total and the WARN and reports them separately. The "nothing due" return is counted apart as `rate_limited` and excluded from loss, so the diagnostic does not sit permanently in WARN on a rate-limited topic. `stop()`'s discarded backlog is now counted too (both queues) — `include/udp_bridge/relay_drops.h`, `src/udp_bridge.cpp` (`4984d51`, `e2615ce`)
- [x] (suggestion) Missing `send_results[""][""]` statistics seed on the relay path — added, under the same lock and before the rate-limit check, as `callback()` does — `src/udp_bridge.cpp`, new `test/test_message_statistics.cpp` (`ac4290d`)
- [x] (suggestion, cross-pass confirmed) Doubled payload residency in the reorder buffer — the rationale claim is now true rather than softened: while a relay form is attached it is the sole holder of the payload, and `enqueuePublish` materializes the publish form from it just before handing the relay form off. The copy is deferred, not added, and a gate-dropped packet now pays for no publish copy at all — `include/udp_bridge/publish_queue.h`, `src/udp_bridge.cpp`, `doc/relay_design.md` (`e89a8ec`)
- [x] (suggestion) Relay probe above the stale gate — moved below it on the reorder-disabled fast path (the default), restoring "a dropped packet costs nothing"; the reorder path must keep it above, since a Buffer decision stores the built item — `src/udp_bridge.cpp`, `doc/relay_design.md` (`6312636`)
- [x] (suggestion) Nothing pinned `applyRelayDestination` *inside* the fan-out loop — extracted `relayToEachDestination` and tested it; **verified failing** against a deliberately hoisted rewrite (3 of 7 relay-send cases fail), passing as written — `include/udp_bridge/relay_send.h`, `test/test_relay_send.cpp` (`d9b059d`)
- [x] (suggestion) Failure isolation per remote, not per connection — extracted the three-arm try/catch as `callIsolated` (`send_isolation.h`) and applied it per connection in `UDPBridge::send()`: the failure is recorded as `SendResult::failed` and logged, and the remote's remaining redundant connections are still attempted — `src/udp_bridge.cpp`, `test/test_relay_send.cpp` (`f296f76`)
- [x] (suggestion) Vacuous `last_sent_time` assertions in `UnnamedSenderIsNeverRelayed` — replaced with the corollary that binds: with an empty `exclude_remote` the selector reaches every remote, sender included, which is what the probe's refusal prevents — `test/test_relay_routing.cpp` (`5d1cd53`)
- [x] (suggestion) Comment named the sink `UDPBridge::relayItem` — `include/udp_bridge/relay_queue.h` (`5d1cd53`)
- [x] (suggestion) Two `PublishQueue`-inherited hardening gaps + `on_shutdown` — `stop_mutex_` held for the duration of `stop()` in both queues so concurrent callers cannot both reach `join()`; `assert(!running_)` in both `configure()`s; `on_shutdown` now stops both workers, since shutdown is reachable from ACTIVE without passing through `on_deactivate` — `include/udp_bridge/relay_queue.h`, `include/udp_bridge/publish_queue.h`, `src/udp_bridge.cpp` (`e2615ce`)
- [x] (suggestion) Plan drift — the superseded "No new parameter" sentence is marked as such with what is true instead, and the five new headers are listed in Files-to-Change — `.agent/work-plans/issue-51/plan.md` (`a3a6917`)

**Deferred**: none. Every finding was actioned.

### New tests (231 total, up from 209)

- `test_relay_routing` +3: `ConfiguredNameIsTheIdentityNotTheLabel`, `DuplicateRemoteIdentityIsRejected`, `LabelDifferingFromWireNameStillNeverEchoes` (the must-fix-1 regression: a one-remote config whose label differs from the sender's wire name relays nothing, with the label-keyed table asserted alongside it as the pre-fix behaviour).
- `test_relay_queue` +4: sink-drop counting, rate-limited-is-not-loss, `StopCountsTheDiscardedBacklog`, `ConcurrentStopsAreSerialized`.
- `test_relay_send` +6: per-destination topic/QoS inside the fan-out loop, unconfigured-destination defaults, rewrite after a failed destination, and the three `callIsolated` cases.
- `test_publish_queue` +5: single payload residency, faithful materialization, no-relay no-op, empty payload, and the two stop-path cases.
- `test_message_statistics` (new file, 3): no aggregate row without the seed, aggregate counts messages not destinations, relayed messages reach the aggregate row.

### Note for the re-review

Two of the new tests bind an *extraction* rather than the call site, which is
this package's established pattern (`destination_selection`, `relay_send`,
`giveup_diagnostic`) but is worth naming: `resolveRemoteIdentity` /
`resolveRemoteIdentities` and `materializePublishPayload` are unit-tested, and
`on_configure` / `enqueuePublish` calling them is not separately pinned — there
is no node-level test harness in this package (no test constructs a
`UDPBridge`). Against the pre-fix tree both fail to compile rather than fail an
assertion. The `applyRelayDestination` and `callIsolated` cases, by contrast,
were verified failing against a deliberately reverted implementation.

## Local Review (Pre-Push)
**Status**: complete
**When**: 2026-08-23 18:51 -04:00
**By**: Claude Code Agent (Claude Opus)
**Verdict**: changes-requested

**Branch**: feature/issue-51 at `928b962`
**Mode**: pre-push
**Depth**: Deep (reason: the round-2 fix pass re-keyed `remote_nodes_` and every `subscribers_[topic].remote_details` entry by the remote's wire name, added an `on_configure` failure path, and restructured payload ownership — none of it seen by a review before this round)
**Must-fix**: 4 | **Suggestions**: 12
**Round**: 3 | **Ship**: recommended — the one code defect is a block move with an obvious correction and the other three are documentation corrections with precise locations; nothing outstanding is a design question needing another independent read.

**Specialists**: Static Analysis (cppcheck on all changed/new files: no findings on `remote_identity.h`, `relay_drops.h`, `send_isolation.h`; the `destination_selection.h` and `udp_bridge.cpp` hits are pre-existing style items already assessed in round 2. Repo pre-commit: all hooks pass except one trailing-whitespace fix inside this file, applied here); Governance + Plan Drift (lead); two Claude Adversarial passes (Lens A logic/correctness, Lens B security/concurrency/lifecycle) — both returned and both independently found must-fix 1. Copilot off (default). Local Adversarial skipped: Ollama unavailable on this host (OOM).

**Verification re-run independently**: `./build.sh udp_bridge` exit 0; `./test.sh udp_bridge` exit 0; `colcon test-result --verbose` exit 0 — **231 tests, 0 errors, 0 failures, 14 skipped**, matching the implementer's report. Commit identity correct on all 43 commits in the range; no issue-closing keywords anywhere.

### Round-2 fixes: verified

All three round-2 must-fixes and all eleven suggestions were actioned, and the actions hold against source rather than merely being reworded.

- **Must-fix 2 (Security section)** — accurate. `decodeSubscribeRequest` does pass wire `source_topic` / `destination_topic` / `period` straight into `addSubscriberConnection`, which writes `subscribers_[source_topic].remote_details[source_info.node_name]`. Self-enrolment and spoof-overwrite are both real as described. (It is still incomplete in two other ways — must-fix 4.)
- **Must-fix 3 (example config)** — accurate. Relay matches on `subscribers_[topic]` keyed by each remote's resolved `source:`, so only the *receiving* remote must carry the hub-local topic; the corrected text says exactly that.
- **Doubled-payload elimination** — correct, and I could not break it. `enqueuePublish` is the sole `publish_queue_.push` / `relay_queue_.push` site; its three callers are the three admit paths. No path publishes without materializing, no double materialization (`materializePublishPayload` early-returns on a null relay form and the pointer is reset on the next line), and no use-after-move (the statistics seed reads `item.message.data.size()` before the `std::move`). `byte_size()` matches reality throughout: while buffered, `message` is empty and the payload is counted once through the relay form; `remote_node.cpp` never touches `item.message`, and the reorder buffer is occupancy-bounded, not byte-bounded, so nothing there is misled by the deferred copy.
- **Removed lifecycle read** — the argument holds. `relay_queue_.stop()` is the first thing `on_deactivate` does and joins before anything else in that function runs, so `subscribers_`, `remote_nodes_`, the clock, the logger and the connections are all intact when the worker last ran. Member declaration order puts both queues last, so on a non-lifecycle teardown they destruct first and their joins precede destruction of the maps their sinks touch. The `on_shutdown` addition is genuinely needed — ACTIVE→shutdown skips `on_deactivate`.
- **`stop()`'s discarded backlog** — accounting is right and cannot double-count or underflow: `push()` is excluded before the join, the in-flight item was already popped, the second `stop()` returns at `!running_`, and `dropped_count_` is monotonic with nothing resetting it. (It does produce a misleading diagnostic on the ordinary deactivate — suggestion 1.)
- **Regression risk of the re-keying** — for `label == name`, `resolveRemoteIdentity` returns the label and every downstream key is byte-identical; behaviour is genuinely unchanged. I checked every real config in this workspace: none sets `remotes.<label>.name`, and every label matches the peer bridge's own `name:` (`bizzy`/`operator`, `operator`/`bizzy`, `dev`/`operator`). The fleet is unaffected. For `label != name` the renames are real and undocumented — must-fix 3.
- **Fail-loud paths** — the duplicate-identity check is correct in what it detects but not in *where* it returns (must-fix 1). The unknown-sender WARN cannot flood: `RCUTILS_LOG_CONDITION_THROTTLE_BEFORE` keeps its state in a function-local `static` at the expansion site, so throttling is per-call-site and independent of message content; varying `source_node` cannot defeat it, and the WARN sits inside the `!remote` branch so it fires only for a name not yet in `remote_nodes_`.
- **Dynamic remotes** — cannot resurrect the two-namespace bug. `decodeSubscribeRequest`, the CONNECT path and `unwrap` all key on `wrapped_packet->source_node`; `addSubscriberConnection` keys `remote_details` with the same string; `configured_remote_names_` is correctly not extended by the dynamic paths, is read under `remote_nodes_mutex_`, and is cleared in `on_cleanup` under that lock. All four paths agree on one namespace.

### Findings

- [x] (must-fix, cross-pass confirmed by both adversarial lenses and the lead) The new duplicate-identity `return CallbackReturn::FAILURE` sits after `socket()`/`bind()`, the parameter callback and all six services. A lifecycle FAILURE leaves the node UNCONFIGURED and does **not** call `on_cleanup`; nothing in the package ever closes `socket_`. So the operator's natural remedy — fix `remotes.<label>.name`, re-`configure` — re-enters `on_configure`, binds the same port with no `SO_REUSEADDR`, hits `EADDRINUSE` and calls `exit(1)`. A config typo becomes a killed process on the retry. The pre-existing FAILURE at `:213` returns before the socket, so this regresses that property. The block needs only parameters, so hoisting it above the socket block makes the failure as clean as `:213`'s — `src/udp_bridge.cpp:441-455` (socket `:338`, `:351`, `exit(1)` `:353`)
- [x] (must-fix) Two documentation sites now contradict the code. `.agents/README.md:182-184` still says "**`remotes.<label>.name` is not a real parameter** — the remote's name is the `remotes_list` label itself … claiming otherwise are stale", which this branch inverts; it is the pitfall list an agent reads first, and the verified-parameter table at `:108` has no entry for the parameter either. Separately, `src/udp_bridge.cpp:2680` and `doc/relay_design.md:272` both name "the node left ACTIVE" as a sink drop reason — that reason was removed in `e2615ce` and `RelayDropReason` has only `UnnamedSender`, `TopicGone`, `NoDestinationDue` — `.agents/README.md:108,182-184`, `src/udp_bridge.cpp:2680`, `doc/relay_design.md:272`
- [x] (must-fix, cross-pass confirmed by Lens A and the lead) The re-keying renames user-visible surfaces for any `label != name` config and no migration note says so: the ROS topics `~/remotes/<remote>/bridge_info` and `~/remotes/<remote>/topic_statistics`, `BridgeInfo.remotes[].name` and `BridgeInfo.topics[].remotes[].remote`, `TopicStatistics.destination_node`, the per-remote diagnostic task names, and the `remote` / `name` arguments of `remote_subscribe`, `remote_advertise`, `remove_subscribe`, `remove_advertise` and `add_remote`. Sharpest point: on `jazzy` the parameter was never declared, so a YAML `name:` key was silently inert — and the shipped `config/example_params.yaml` set `label: robot_a` with `name: "robot_a_bridge"`. Anyone who copied the example gets all of the above renamed on upgrade. The example was fixed to comment the key out; the note telling an upgrader to check for a stale `name:` is still missing — `doc/relay_design.md`, `udp_bridge/README.md`
- [x] (must-fix) The rewritten Security section is now the authoritative blast-radius statement, and it omits two consequences relay introduces or sharpens. (a) `uncompress()` allocates `std::vector<uint8_t> ret(decomp_size)` from the attacker-controlled `uncompressed_size` header field (`uint32_t`, unbounded, no ratio check) — pre-#51 a zip-bomb packet cost the hub one local allocation; post-#51 the expanded payload is relayed **outbound** to every other remote listing the topic, and this repo's own bench data (480 kB of zeros → 488 B on the wire) puts the achievable ratio near 1000×, which is a bandwidth amplifier aimed at the boats' scarce links. (b) `addSubscriberConnection` assigns `rd.destination_topic` unconditionally, so a spoofed subscribe request can *redirect* a victim remote's feed onto an arbitrary topic in the victim's ROS graph, not merely cut it as the section currently says — `doc/relay_design.md` Security, `src/packet.cpp:42-48`, `src/udp_bridge.cpp:1511`
- [x] (suggestion) `diagnostic_timer_` is not stopped in `on_deactivate` (only reset in `on_cleanup`), so the first tick after a deactivate reads the discarded backlog as fresh drops and reports "relay dropping (N since last tick) — outgoing link stalled?" when the cause was the operator. Snapshot `last_reported_*_drops_` in `on_deactivate`, or report the discard separately from overflow — `src/udp_bridge.cpp:671-694`, `include/udp_bridge/relay_queue.h:102`, `include/udp_bridge/publish_queue.h:193`
- [x] (suggestion, cross-pass confirmed) `breakdown()` includes `rate_limited`, which `relay_drops.h` explicitly excludes from loss, yet it is published as `sink_drop_reasons` and appended to a WARN whose trigger excludes it — a hub reads "relay dropping (1 since last tick; topic_gone=1, rate_limited=40321)". Give `rate_limited` its own `stat.add` and keep the WARN string to loss reasons — `src/udp_bridge.cpp:2696-2710`
- [x] (suggestion) `assert(!running_)` is compiled out under `NDEBUG` — the builds that run on the boats — and `sink_` / `max_bytes_` are assigned *outside* the locked block, so even in a debug build the check-then-assign is not atomic. Nothing reaches it today; either assign under `mutex_` or downgrade the comment, which currently claims a guarantee it does not provide — `include/udp_bridge/relay_queue.h:52-62`, `include/udp_bridge/publish_queue.h:135-147`
- [x] (suggestion) The header comment justifies the unlocked write to `configured_remote_names_` with "before any timer exists, so no reader can race the write". That is false on a re-configure: `on_cleanup` resets only `diagnostic_timer_`, so `spin_timer_`, `bridge_info_timer_`, `stats_report_timer_` and `subscription_update_timer_` survive and keep firing during the second `on_configure`. It is benign only because `spin_once` gates on ACTIVE — a different reason than the one given. Taking `remote_nodes_mutex_` costs nothing and makes the comment true — `include/udp_bridge/udp_bridge.h:461-469`, `src/udp_bridge.cpp:457-459`
- [x] (suggestion) The unknown-sender WARN sits inside `if(!remote)`, so it fires exactly once per unknown name for the process lifetime; the throttle can only suppress a *second* unknown sender within 10 s. `relay_design.md` calls it "a throttled WARN", implying a recurring signal an operator can find later. For a misconfiguration the doc describes as silently defeating the loop rule, a `DiagnosticStatus` row would be the findable surface — `src/udp_bridge.cpp:1671-1684`, `doc/relay_design.md:138`
- [x] (suggestion) `add_remote` keys `remote_nodes_[request->name]` from an operator-supplied string with no equivalent warning, so an operator who types the *label* creates a phantom remote matching nothing — the same misconfiguration the new WARN exists to catch, at the one entry point that lacks it — `src/udp_bridge.cpp:2269`
- [x] (suggestion) A repeated label in `remotes_list` (`["a","a"]`) is now a hard `on_configure` failure with a self-contradictory message: "remotes 'a' and 'a' both resolve to remote name 'a'". Previously it was processed twice, idempotently. Detect duplicate *labels* separately, or skip an already-seen label — `include/udp_bridge/remote_identity.h:63-77`
- [x] (suggestion) `subscribers_` keys are resolved topic names (`resolve_topic_or_service_name` at `:552`) but `decodeData`'s lookup key is the raw wire `destination_topic`, unresolved. A sender whose `destination:` is relative therefore publishes locally under the resolved name while the relay probe's key stays unresolved, and relay silently never matches. Pre-existing for `publishers_`, newly load-bearing for routing — `src/udp_bridge.cpp:988-991`
- [x] (suggestion) Empty-payload asymmetry: `materializePublishPayload` guards `reserve(0)` (which throws from the rcl uint8_array layer) but `build_item`'s non-relay branch still calls `reserve()` unconditionally. A wire `MessageInternal` with empty `data` — an attacker can send one — throws and is dropped when no relay destination exists, and publishes normally when one does. Same packet, different outcome depending on unrelated remote config; reuse the guard — `src/udp_bridge.cpp:1026-1030`
- [x] (suggestion) `relay_design.md`'s no-loopback argument ("Relay does not touch `ignore_local_publications`, so a bridge still never hears its own republish") is load-bearing for single delivery and is asserted, not verified: on a hub the relay match condition guarantees the hub also holds a forwarding subscription on the topic it is about to publish to, and only that rmw-dependent option separates the two paths. The empirical evidence that it currently holds is #51's own symptom — a hub does not relay today — but it deserves the sentence, and a hub single-delivery assertion in the #18 bench harness — `doc/relay_design.md:93-95`, `src/udp_bridge.cpp:1604`
- [x] (suggestion) `diagnoseRelayQueue` reads `dropped_count()` and `lost()` twice each and `breakdown()` a third time without a snapshot, so the worker can increment between reads and `dropped_by_queue + dropped_by_sink` can exceed `dropped_total` in the same status. Snapshot once into locals — `src/udp_bridge.cpp:2686-2696`
- [x] (suggestion) `RelayDropCounters::reset()` is dead code. Leave it uncalled or delete it — if it is ever wired into `on_cleanup`, `last_reported_relay_drops_` must be reset in the same place or `recent = dropped - last_reported_relay_drops_` underflows to ~2^64 and pins the diagnostic at WARN — `include/udp_bridge/relay_drops.h:87-92`, `src/udp_bridge.cpp:2688`

### On the test-quality question the implementer raised

**Acceptable — not a must-fix.** `resolveRemoteIdentity` / `resolveRemoteIdentities` and `materializePublishPayload` are pure functions and are properly pinned; what is unpinned is two call-site lines, and both adversarial passes traced them independently and confirmed them correct. A node-level harness in this package would have to stand up a lifecycle node that binds a UDP socket and creates six services, which is disproportionate for pinning two lines and still would not exercise a real two-remote relay.

The proportionate home is the **#18 bench harness**: a three-bridge hub scenario with `label != name` on the hub, asserting that the sender receives nothing back and each other remote receives exactly one copy. That single scenario would pin the `on_configure` call site, the `enqueuePublish` call site, and the `ignore_local_publications` single-delivery property above, all at once. Recommend filing it as a follow-up against #18 rather than blocking this branch.

### Plan adherence

Plan is in sync. The five new headers are listed in Files-to-Change, and the superseded "No new parameter" sentence is marked as such with what is true instead. No scope creep: every file in the diff traces to the issue or to a review finding.

### Governance

Consequences carried: `README.md`, `doc/conceptual_overview.md`, `doc/relay_design.md`, `.agents/README.md` (test-target list, architecture note, `relay_queue_max_bytes` row) all updated. The two gaps are must-fix 2 (the inverted `remotes.<label>.name` pitfall and its missing table row) and must-fix 3 (no migration note for the re-keying). Atomic commits throughout; agent identity correct on all 43; no issue-closing keywords.

## Implementation
**Status**: complete
**When**: 2026-08-23 19:03 -04:00
**By**: Claude Code Agent (Claude Opus)

**Branch**: feature/issue-51 at `5b81bad`
**Addressed**: `## Local Review (Pre-Push)` (round 3, 2026-08-23 18:51 -04:00, branch at `928b962`) — 4 must-fixes, 12 suggestions
**Commits**: `6841e97`, `f3298ef`, `0fc9e03`, `0ff79fd`, `00d8edf`, `789695c`, `2a3ba8e`, `6d2d3d2`, `b5aa98a`, `344303b`, `5eaf9f3`, `0eb2150`, `2b30ea6`, `50fc387`, `98b6f0f`, `5b81bad`

### Actions

Must-fixes:

- [x] Duplicate-identity FAILURE returned after `socket()`/`bind()` and the six
  services — hoisted the whole parameter-only validation block (declare
  `remotes_list`, declare each `remotes.<label>.name`, `resolveRemoteIdentities`,
  the FAILURE return) above the socket block, with a comment stating the rule so
  the next parameter check lands in the right place. `configured_remote_names_`
  population and the `remote_nodes_` loop stay where they were — they need
  `name_` and the node, not just parameters — `src/udp_bridge.cpp:336-368`
  (`6841e97`).
  **Scope check requested by the review**: `on_configure` has exactly two
  `CallbackReturn::FAILURE` returns — the give-up-threshold check at `:213` and
  this one. Both are now above every resource acquisition. Nothing else in
  `on_configure` returns FAILURE after acquiring anything, so there was nothing
  to fix silently beyond scope.
  **No test added, and why**: the property is "this early return precedes the
  socket", which is only observable by driving a real lifecycle transition — the
  node would have to bind a UDP port and create six services, then be
  re-configured after a deliberate config error to prove the port is still free.
  That is the node harness the round-2 discussion already judged
  disproportionate for this package. `resolveRemoteIdentities` itself (the thing
  that decides *whether* to fail) is unit-tested; what is unpinned is one call
  site's position in the function.
- [x] `.agents/README.md` inverted pitfall + missing table row — the pitfall
  bullet now says the parameter IS real as of #51, what it changes, and that a
  pre-#51 `name:` key was inert; added a `remotes.<label>.name` row to the
  verified-parameter table (default empty, duplicate names fail `on_configure`
  before the socket opens). Stale "node left ACTIVE" sink drop reason removed
  from both `src/udp_bridge.cpp` (the `diagnoseRelayQueue` comment) and
  `doc/relay_design.md`, which now names the three real `RelayDropReason`
  values and keeps `NoDestinationDue` out of the loss list — `.agents/README.md:108,183`,
  `src/udp_bridge.cpp:2701`, `doc/relay_design.md:294` (`f3298ef`).
- [x] Migration note for the rename — written as a blockquote callout under
  `remotes.<remote_label>.name` in `udp_bridge/README.md` (an upgrader reading
  the parameter reference is who needs it), listing all six renamed surfaces
  (per-remote `bridge_info` / `topic_statistics` topics, the two `BridgeInfo`
  fields, `TopicStatistics.destination_node`, diagnostic task names, the five
  services' `remote`/`name` arguments), the "the parameter was inert on jazzy
  and the shipped example set `name: robot_a_bridge`" trap, and both remedies
  (delete the key to keep the old names, or accept the rename). A short
  **Upgrading:** paragraph in `doc/relay_design.md`'s identity section points at
  it, since that is where a reader arrives from the loop-rule explanation —
  `README.md:85-115`, `doc/relay_design.md:169-176` (`0fc9e03`).
- [x] Security section completed with both consequences, each tied to
  [#53](https://github.com/rolker/udp_bridge/issues/53) — the unconditional
  `rd.destination_topic` assignment as a *redirect* (a victim keeps receiving,
  on an attacker-chosen topic, which is quieter than a cut feed), and
  `uncompress()`'s unbounded `uncompressed_size` allocation, stated explicitly
  as pre-existing and **not** introduced here, with relay changing its severity
  (the expanded payload now fans outbound to the boats' links; the bench's
  480 kB → 488 B figure gives the ~1000x) — `doc/relay_design.md:340-361`
  (`0ff79fd`).
  **Recommendation, not implemented here (as instructed)**: the ratio/absolute
  cap in `uncompress()` is worth implementing, not just documenting. It is a
  handful of lines (reject when `uncompressed_size` exceeds an absolute ceiling
  or a fixed multiple of the compressed size) and it is the only bound on an
  attacker's allocation on the drain thread. It belongs to #53: it changes what
  the bridge accepts on the wire, so a too-tight cap silently drops legitimate
  large images, and picking the number needs the real payload-size distribution
  — its own blast radius, separate from relay.

Suggestions:

- [x] Diagnostic reports the deactivate discard as fresh loss — `on_deactivate`
  now re-baselines `last_reported_publish_drops_` / `last_reported_relay_drops_`
  after both `stop()` calls, with a comment on the (benign) interleave with
  `periodic_group_`; both diagnose functions' "touched only here" comments
  corrected — `src/udp_bridge.cpp:680-692` (`00d8edf`).
- [x] `dropped_by_queue + dropped_by_sink` could exceed `dropped_total` —
  every counter is read once into a local in `diagnoseRelayQueue` —
  `src/udp_bridge.cpp:2704-2718` (`789695c`).
- [x] `rate_limited` mixed into the loss breakdown and the WARN — `breakdown()`
  became `lossBreakdown()` (loss reasons only) plus a `rateLimited()` accessor;
  the diagnostic publishes `rate_limited` as its own field and the WARN string
  carries only loss reasons. Tests updated to assert the separation —
  `include/udp_bridge/relay_drops.h:66-90`, `src/udp_bridge.cpp:2721-2726`,
  `test/test_relay_queue.cpp:404-441` (`2a3ba8e`).
- [x] `assert(!running_)` compiled out under NDEBUG and the assignment outside
  the lock — both queues' `configure()` now check and assign under `mutex_` in
  one critical section, so the ordering rests on the lock (which also
  happens-before the worker `start()` launches) rather than on the assert; the
  comments say so instead of claiming a guarantee they did not provide —
  `include/udp_bridge/relay_queue.h:46-62`, `include/udp_bridge/publish_queue.h:134-149`
  (`6d2d3d2`).
- [x] False justification for the unlocked `configured_remote_names_` write —
  the write is now under `remote_nodes_mutex_` like every reader, and the header
  comment states the real hazard (a re-configure leaves the other timers
  running) instead of the untrue "before any timer exists" —
  `src/udp_bridge.cpp:466-474`, `include/udp_bridge/udp_bridge.h:460-466`
  (`b5aa98a`).
- [x] "A throttled WARN" overstates the unknown-sender signal — `relay_design.md`
  now says what it is: once per unknown name for the process lifetime, a
  startup-time signal, with the throttle only collapsing a burst of *different*
  unknown senders — `doc/relay_design.md:137-147` (`344303b`).
  (partially deferred: the suggested `DiagnosticStatus` row is **not** added —
  it is a new continuously-published surface, needing a tracked set of unknown
  senders and a new task in `syncDiagnosticTasks`, which is feature work beyond
  a review fix. The doc now names it as the follow-up rather than implying the
  WARN already covers it.)
- [x] `add_remote` keys `remote_nodes_[request->name]` with no namespace
  warning — logs an INFO on creation saying the argument must be the name that
  bridge calls itself, not a local label, and a comment explains why this entry
  point cannot detect the mistake (any string is a legitimate new remote) —
  `src/udp_bridge.cpp:2296-2310` (`5eaf9f3`).
- [x] A repeated label in `remotes_list` failed `on_configure` with "remotes 'a'
  and 'a' both resolve to 'a'" — `resolveRemoteIdentities` now skips an
  already-seen label (one parameter block, one identity, configured twice
  idempotently as before), while a genuine two-label collision still fails.
  New test `RelayRouting.RepeatedLabelIsIdempotentNotACollision` covers both —
  `include/udp_bridge/remote_identity.h:65-70`, `test/test_relay_routing.cpp:184-207`
  (`0eb2150`).
- [x] `subscribers_` keyed by resolved topic names vs `decodeData`'s raw wire
  key — documented in `doc/relay_design.md`'s Mechanism section: write
  hub-carried topic names absolutely on both sides, why a relative
  `destination:` publishes but never relays, and that `publishers_` has always
  been keyed this way — `doc/relay_design.md:41-54` (`2b30ea6`).
  (partially deferred: resolving the key per packet is **not** done —
  `resolve_topic_or_service_name` on the socket-drain thread runs on every data
  packet and would be handed an attacker-supplied name that can throw from the
  rcl layer, turning a malformed remote topic name into a caught-and-logged
  decode error at a new place. Changing what the receive path accepts is not a
  review-fix-sized change; the constraint is now stated instead.)
- [x] Empty-payload asymmetry — `build_item`'s non-relay branch now uses the
  same `!data.empty()` guard as `materializePublishPayload`, so a wire
  `MessageInternal` with an empty payload behaves identically whether or not a
  relay destination exists — `src/udp_bridge.cpp:1054-1069` (`50fc387`).
- [x] The no-loopback argument was asserted, not verified — `relay_design.md`
  now spells out that on a hub the relay match condition implies the hub also
  holds a forwarding subscription on the topic it republishes, making
  `ignore_local_publications` (`src/udp_bridge.cpp:1641`) the only separator;
  states that the evidence is empirical; and recommends the three-bridge
  single-delivery assertion in the #18 bench harness as the place to pin it —
  `doc/relay_design.md:107-122` (`98b6f0f`).
- [x] `RelayDropCounters::reset()` is dead code — deleted, with a comment in its
  place recording why the counters are monotonic and what zeroing them without
  zeroing `last_reported_relay_drops_` would do (underflow to ~2^64, diagnostic
  pinned at WARN). Its only caller was a test assertion, removed with it —
  `include/udp_bridge/relay_drops.h:92-98`, `test/test_relay_queue.cpp` (`5b81bad`).

### Follow-ups recommended (not filed, not implemented)

- A `uncompress()` ratio/size cap under [#53](https://github.com/rolker/udp_bridge/issues/53) — see must-fix 4 above.
- A three-bridge hub scenario in the #18 bench harness, with `label != name` on
  the hub: pins the `on_configure` call site, the `enqueuePublish` call site and
  the single-delivery property in one test. This is the review's own
  recommendation from the test-quality section, restated here so it is not lost.
- A `DiagnosticStatus` row for unknown senders — see the suggestion above.

### Verification (verbatim)

```
$ ./build.sh udp_bridge
BUILD EXIT=0
Finished <<< udp_bridge [24.1s]
Summary: 1 package finished [24.5s]
  1 package had stderr output: udp_bridge
```

(stderr is the pre-existing `-Wpedantic` flexible-array-member warnings from
`packet.h`, unchanged by this pass.)

```
$ ./test.sh udp_bridge
TEST EXIT=0
$ colcon test-result --verbose
RESULT EXIT=0
Summary: 232 tests, 0 errors, 0 failures, 14 skipped
```

231 was the floor; 232 is 231 plus
`RelayRouting.RepeatedLabelIsIdempotentNotACollision`. No test was weakened —
`test_relay_queue.cpp`'s edits track the `breakdown()` → `lossBreakdown()` +
`rateLimited()` split and the removal of `reset()`, and add an assertion that
the rate limit never appears in the loss breakdown.

```
$ pre-commit run --from-ref origin/jazzy --to-ref HEAD
trim trailing whitespace.................................................Passed
fix end of files.........................................................Passed
check yaml...............................................................Passed
check xml............................................(no files to check)Skipped
check for merge conflicts................................................Passed
check that executables have shebangs.................(no files to check)Skipped
mixed line ending........................................................Passed
check for added large files..............................................Passed
cmake-lint...............................................................Passed
yamllint.................................................................Passed
don't commit to branch...................................................Passed
```

Not pushed, no PR — per the dispatch contract.

## Integrated Review
**Status**: complete
**When**: 2026-08-23 09:42 -04:00
**By**: Claude Code Agent (Claude Opus 5 (1M context))

**PR**: #63 at `b054298`
**Sources**: 2 (Copilot @ `b054298`; Local Review (Pre-Push) x3 @ `cb3f738`/`0e87d79`/`928b962`)
**Cross-source confirmations**: 1
**CI**: all-pass (build-and-test success; copilot-pull-request-reviewer success)

Copilot verdict: changes recommended. 7 inline comments, all accurate against the code; no false positives.

### Findings
- [ ] (cross-confirmed: Copilot + Local Review R2 + R3) No end-to-end `decodeData()` -> `enqueuePublish()` -> `RelayQueue` -> `send()` test; the suite can pass while the production wiring is broken. Already filed as #64 -- three independent reviewers now agree. No action in this PR beyond tracking -- `test/test_relay_routing.cpp:127`
- [ ] (must-fix, Copilot) The label-vs-name echo fix is incomplete: `source_node[maximum_node_name_size]` is 24 and `setName()` truncates the local name to 23, but `resolveRemoteIdentity` returns the configured string UNTRUNCATED. A remote whose name exceeds 23 chars therefore sends a truncated `source_node` that never equals the hub's configured identity -- loop rule inert, hub echoes. Second hole in the same place: collision detection compares untruncated strings, so two identities differing only after char 23 pass validation and then collide on the wire. Canonicalize to the wire limit in ONE place, applied to both collision detection and the loop comparison. Trigger is an ordinary naming choice, not an attack (`bizzyboat_operator_bridge` is 25 chars) -- `include/udp_bridge/remote_identity.h:41`
- [ ] (must-fix, Copilot) Identity validation rejects duplicates among remotes but not an identity equal to `name_`. The self entry is installed in `remote_nodes_`, and `unwrap()`'s self-packet rejection only runs in the not-found branch, so a successful lookup bypasses it; `RemoteNode`'s own guard is an `assert`, compiled out in release. Reject at configure, before constructing the `RemoteNode` -- `src/udp_bridge.cpp:365`
- [ ] (must-fix, Copilot) `remote_nodes_[...] = make_shared<RemoteNode>(...)` and the following `update()` run outside `remote_nodes_mutex_`, while `spin_once()`, `decode()`, `sendBridgeInfo()` and diagnostics read the same map under it. Benign on first configure; a re-configure with timers still alive is a data race. Resolve/create under the lock, keep the pointer for the slow `update()` outside -- `src/udp_bridge.cpp:483`
- [ ] (must-fix, Copilot) `on_deactivate`'s drop-delta re-baselining races `diagnoseRelayQueue()` on a plain `uint64_t`: diagnose reads 100, deactivate stores 120, diagnose subtracts 120 from 100 -> unsigned underflow, backlog re-reported as link loss. Introduced by a round-3 suggestion (deactivate re-baselining). Serialize the pair or make it atomic -- `src/udp_bridge.cpp:698`
- [ ] (suggestion, Copilot) Relay reuses `UDPBridge::send()`, whose per-connection failure handler logs `Connection::send()` timeouts at ERROR (`src/udp_bridge.cpp:1997-2000`). An over-horizon spoke is a normal field condition and a hub hits it for every forwarded topic, so this emits error-level noise. Use a non-error severity for expected relay link failures, or give `send()` a relay-specific reporting policy -- `src/udp_bridge.cpp:1291`
- [ ] (suggestion, Copilot) `RelayItem::byte_size()` omits `message.reliability` and `message.durability`, so the relay queue's byte bound understates the memory actually held -- `include/udp_bridge/relay_item.h:51`

### Notes
- Copilot framed the identity-length and byte_size findings as attacker-driven ("a sender can fill a stalled relay queue"). Per operator context (2026-08-23) udp_bridge targets a trusted network and malicious-packet defence was never a design goal, so that framing does not apply here -- but neither finding needs it. The identity-length trigger is a node name longer than 23 characters, an ordinary naming choice.
- Governance: no new ROS parameter in this round, so `.agents/README.md`'s verified-parameter table needs no row (`relay_queue_max_bytes` was added there earlier in this PR).
- Finding 4 is a regression introduced by addressing a round-3 suggestion -- worth noting that the fix pass created it, which argues for the re-review that caught it.

### False positives
- None. All seven Copilot comments were checked against the code and hold.
