# Plan: Relay support: provenance-aware remote→remote forwarding (cloud-hub topology)

## Issue

https://github.com/rolker/udp_bridge/issues/51

## Context

Forwarding subscriptions set `options.ignore_local_publications = true`
(`udp_bridge/src/udp_bridge.cpp:1280`, `UDPBridge::updateLocalSubscriptions`)
so a bridge's own forwarding subscription never hears its own republish of a
topic received from a remote — remote→remote relay is structurally blocked.
Provenance is also genuinely dropped by publish time: `PublishItem`
(`udp_bridge/include/udp_bridge/publish_queue.h:27`) has no source field,
`decodeData`'s `build_item()` lambda (`udp_bridge/src/udp_bridge.cpp:902-931`)
never copies `SourceInfo` into it, and `publishItem()`
(`udp_bridge/src/udp_bridge.cpp:993+`) publishes via a plain
per-topic `GenericPublisher` with no connection awareness. Verified in the
`## Issue Review` progress.md entry (commit `ac24186`).

The cloud-hub topology needs the hub to both publish locally (for
salmon/pandy/web-renderer) and re-forward boat-origin topics onward to
other operator connections — this issue makes that first-class instead of
just removing the echo guard.

## Provenance storage granularity (decision)

**Decision: add a field directly to `PublishItem`** — `std::string
origin_remote_node`, populated from `SourceInfo::node_name` in
`decodeData`'s `build_item()` lambda — not a side-table.

Justification, from reading the two paths a `PublishItem` travels before
`publishItem()` runs:

- **`PublishQueue`** (`publish_queue.h`) moves `PublishItem` by value
  through a `std::deque` (`push`/`run`), including its drop-oldest
  overflow policy. A field on the struct survives this for free; a side
  table keyed by e.g. `(topic, packet_number)` would dangle whenever an
  item is dropped under the byte budget, since nothing clears the
  matching side-table entry.
- **`RemoteNode`'s reorder buffer** (issue #35) stores `PublishItem` by
  value in `ReorderBufferEntry` (`include/udp_bridge/remote_node.h:406`,
  `PublishItem item;`) across the hold window and releases it later from
  `admitOrBuffer`/`flushExpiredBuffer`. A field on `PublishItem` survives
  buffering and replay with **zero changes** to the reorder-buffer
  mechanics themselves — only `build_item()` (populate) and `publishItem()`
  /the new relay-forwarding path (read) need to touch it. A side-table
  keyed independently of the buffered item would need its own hold/expiry
  logic paralleling the reorder buffer's, duplicating state for no
  benefit.
- The loop-protection rule only needs provenance *at the moment the item
  is in hand* (when deciding whether to re-forward it) — never a
  standalone lookup after the item's lifecycle ends. That is exactly the
  access pattern a struct field gives for free and a side-table would
  have to reconstruct.
- Cost is small and already inside the existing `byte_size()` accounting
  pattern: one `std::string`, typically a short node name, added to
  `PublishItem::byte_size()` alongside the other string fields.

**Scope of the provenance**: `origin_remote_node` records the *immediate
sender* (last hop), matching `SourceInfo::node_name` as populated today
(including through `unwrap()`'s wrapped-packet path, which already
resolves the true origin node for sequenced packets — see
`udp_bridge.cpp` `unwrap()`). This is sufficient for the issue's stated
rule ("never forward a message out the connection it arrived on") and for
the 3-node test (hub must not forward a boat-origin message back to the
boat's own connection). It does **not** track a full multi-hop path; a
hub-of-hubs chain beyond one relay hop is out of scope for this issue and
not implied by the ask.

## Approach

1. **Provenance plumbing.**
   - Add `std::string origin_remote_node` to `PublishItem`
     (`include/udp_bridge/publish_queue.h`), default-empty (empty means
     "published from a genuinely local source, not relayed"). Include it
     in `byte_size()`.
   - Populate it in `decodeData`'s `build_item()` lambda
     (`src/udp_bridge.cpp`) from `source_info.node_name`.
   - No change needed to `PublishQueue` or `RemoteNode`'s reorder-buffer
     storage/replay code — both already move `PublishItem` by value (see
     decision above).

2. **Opt-in relay flag (default off).**
   - Add a per-topic `relay` boolean parameter, following the existing
     per-topic parameter pattern in the connection/topic parsing loop
     (`src/udp_bridge.cpp`, the `topics_list` loop that also declares
     `source`/`destination`/`reliability`/... around line 470-520):
     `remotes.<r>.connections.<c>.topics.<t>.relay`, `declareIfMissing(...,
     false)`.
   - Thread it into `SubscriberDetails`/`addSubscriberConnection` so the
     forwarding subscription for a given *source topic*
     (`include/udp_bridge/types.h:36`) knows whether relay is requested.
     Because a single shared subscription serves potentially multiple
     remote destinations for the same source topic (`subscribers_` is
     keyed by source topic; `remote_details` holds one entry per
     requesting connection), the effective flag is the **OR** across all
     requesting connections for that topic — if any remote wants a topic
     relayed, the subscription must hear locally-republished messages;
     per-destination loop protection (step 3) still applies independently
     per connection.
   - In `updateLocalSubscriptions()` (`src/udp_bridge.cpp`, ~line 1236-1280
     today), set `options.ignore_local_publications = !relay_enabled` for
     that topic's subscription instead of the current unconditional
     `true`.

3. **Loop-protection logic.**
   - At the forward-out decision point for the relay-eligible path (the
     `callback()` that `updateLocalSubscriptions()` wires to
     `create_generic_subscription`, which forwards to each remote
     connection listed for that topic): for a message whose
     `origin_remote_node` is non-empty, skip forwarding to any connection
     whose remote node name equals `origin_remote_node`. This is the
     literal "never forward out the connection it arrived on" rule.
   - Plain locally-originated messages (`origin_remote_node` empty) are
     unaffected — forwarded to every configured remote as today.
   - Symmetric two-host configs with `relay: false` (the default) are
     bit-for-bit unaffected: `ignore_local_publications` stays `true`, so
     the forwarding subscription never sees the republish in the first
     place and the loop-protection check never runs.

4. **Tests** (`test/`, following the existing `test_reorder_buffer.cpp` /
   `test_admission_control.cpp` style):
   - **Three-node relay**: node A → hub → node B, hub also has a local
     subscriber on the same topic. Assert the local subscriber receives
     the message AND node B receives it, with correct payload.
   - **Echo-loop regression**: symmetric two-host pair with `relay: true`
     on both sides. Assert host A never receives back a message it
     originated (no ping-pong), using `origin_remote_node` to detect a
     would-be echo if the test harness needs to inspect internals, or an
     iteration/hop-count ceiling at the transport level as an outer
     safety net.
   - **Provenance retention across publish-queue/reorder paths**: unit
     test at the `PublishQueue` level (already-mockable via its `Sink`)
     confirming `origin_remote_node` survives a push/pop cycle unchanged,
     and a `RemoteNode` reorder-buffer test (extending
     `test_reorder_buffer.cpp`) confirming a buffered-then-released item
     still carries its `origin_remote_node`.

5. **Documentation** (same PR, not a follow-up):
   - New `doc/relay_design.md`, matching the format of
     `doc/qos_design.md` / `doc/admission_control_design.md` /
     `doc/resend_budget_design.md`: state the default-off rationale
     (existing symmetric configs must keep today's echo protection
     unchanged), the exact loop-protection rule, the provenance-storage
     decision above (with the reasoning), and a **Security** section
     cross-referencing open issue #53 (unauthenticated transport trust
     model) — a relay hub is a bigger target than a point-to-point
     bridge because it re-forwards traffic between three or more parties
     instead of terminating it at two; anyone who can inject packets at
     the hub can now reach every downstream relay target, not just the
     hub's own local subscribers.
   - `config/example_params.yaml`: add a commented `relay: false` line to
     the topic-entry example, matching the existing per-field comment
     style (see the `period` / `queue_size` comments already there).
   - `README.md` and `doc/conceptual_overview.md`: describe the new
     remote→remote relay capability and its default-off, opt-in posture
     in the data-flow / architecture sections.
   - `.agents/README.md`'s verified-parameter table (the row ending
     `... per-topic source ... history_depth (0, clamped ≤ 10000)`):
     append `relay` (default `false`) to that same row's per-topic list,
     per the workspace convention that this table is the required edit
     target for any ROS param change.

Recommended commit sequence (atomic, per the Issue Review's
recommendation not to split into sub-issues): provenance plumbing → opt-in
relay flag → loop-protection logic → tests → docs.

## Files to Change

| File | Change |
|------|--------|
| `udp_bridge/include/udp_bridge/publish_queue.h` | Add `origin_remote_node` field to `PublishItem`; include in `byte_size()` |
| `udp_bridge/src/udp_bridge.cpp` | Populate `origin_remote_node` in `build_item()`; add `relay` param parsing in the topics-list loop; conditional `ignore_local_publications` in `updateLocalSubscriptions()`; loop-protection check in the forwarding callback |
| `udp_bridge/include/udp_bridge/types.h` | Thread `relay` (or an equivalent per-topic flag) through `SubscriberDetails`/`RemoteDetails` as needed for the OR-across-connections semantics |
| `udp_bridge/test/test_reorder_buffer.cpp` | Extend to assert `origin_remote_node` survives buffer/replay |
| `udp_bridge/test/` (new) | Three-node relay test, echo-loop regression test, publish-queue provenance-retention test |
| `udp_bridge/doc/relay_design.md` | New design doc: default-off rationale, loop-protection rule, provenance decision, #53 cross-reference |
| `udp_bridge/config/example_params.yaml` | Document new `relay` per-topic flag |
| `udp_bridge/README.md` | Describe relay capability |
| `udp_bridge/doc/conceptual_overview.md` | Describe relay capability in data-flow description |
| `.agents/README.md` | Add `relay` (default `false`) to the verified per-topic parameter list |

## Principles Self-Check

| Principle | Consideration |
|---|---|
| Capture decisions, not just implementations | `doc/relay_design.md` captures the default-off rationale, the loop-protection rule, and the provenance-storage decision (with reasoning), matching the project's existing per-feature design-doc convention. |
| A change includes its consequences | `config/example_params.yaml`, `README.md`, `doc/conceptual_overview.md`, and `.agents/README.md`'s parameter table are all updated in this PR, not deferred. |
| Test what breaks | Tests target the three concrete failure modes named in the issue: cross-relay delivery, echo loops, and provenance loss across the queue/reorder path — not generic coverage. |
| Only what's needed | Default `relay: false` preserves today's behavior for every existing symmetric config; the OR-across-connections semantics in step 2 is the minimum needed to keep the shared-subscription model working, not new topology beyond what's asked. |
| Human control and transparency | Routing stays fully explicit via the existing per-remote `topics_list` config — the hub only relays a topic to a destination that already lists it, per the issue's ask #3. |
| Improve incrementally | One PR, atomic commits in the sequence above; the Issue Review already judged the four asks too tightly coupled to split into sub-issues. |

## ADR Compliance

| ADR | Triggered | How addressed |
|---|---|---|
| (none — no workspace ADR applies) | No | Issue Review confirmed this is a project-repo domain change; `udp_bridge` uses narrative `doc/*_design.md` docs instead of ADRs, which this plan follows (`doc/relay_design.md`). |

## Consequences

| If we change... | Also update... | Included in plan? |
|---|---|---|
| `PublishItem` gains `origin_remote_node` | `PublishQueue::byte_size()` accounting, reorder-buffer tests | Yes |
| `updateLocalSubscriptions()` sets `ignore_local_publications` conditionally | `doc/relay_design.md` (behavior change from unconditional `true`) | Yes |
| New per-topic `relay` param | `config/example_params.yaml`, `README.md`, `doc/conceptual_overview.md`, `.agents/README.md` parameter table | Yes |
| Relay increases hub blast radius | `doc/relay_design.md` Security section cross-referencing #53 | Yes |
| Reorder buffer (#35) now carries a provenance field through its stored items | `test_reorder_buffer.cpp` extended, not a new file | Yes |

## Documentation & Instruction Impact

- **Stale docs** (must land in this PR): `README.md` and
  `doc/conceptual_overview.md` describe the current one-directional
  local→remote/remote→local data flow only — both become inaccurate the
  moment relay ships, so both are updated in this PR. `.agents/README.md`'s
  verified-parameter table also becomes stale the moment a new per-topic
  param exists — updated in this PR per the table's own convention.
- **Agent-instruction candidates** (proposals only — operator decides):
  None. The pattern here (thread a new per-message field through a
  by-value queue/buffer instead of a parallel side-table) is a normal
  engineering judgment call specific to this codebase's existing
  `PublishItem`/`PublishQueue`/reorder-buffer shape, not a generalizable
  workspace- or ROS-pattern worth promoting to `.agent/knowledge/`.

## Open Questions

- None — the provenance-granularity question the Issue Review left open
  is settled above (per-message field on `PublishItem`).

## Estimated Scope

Single PR, five atomic commits (provenance plumbing → opt-in flag →
loop-protection logic → tests → docs), per the Issue Review's
recommendation.
