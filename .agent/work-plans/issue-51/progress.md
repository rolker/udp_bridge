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
