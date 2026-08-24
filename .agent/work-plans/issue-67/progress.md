---
issue: 67
---

# Issue #67 — Remove the per-remote name parameter: a label should be the peer's wire identity, not a second namespace

## Issue Review
**Status**: complete
**When**: 2026-08-24 00:29 -04:00
**By**: Claude Code Agent (Claude Sonnet)

**Issue**: #67
**Comment**: (best-effort post follows this entry; not recorded inline)
**Scope verdict**: well-scoped

### Actions
- [ ] Confirm with the planner that `resolveRemoteIdentities`' duplicate-identity check (two *different* labels resolving to the same identity) is now provably unreachable and should be **deleted**, not retargeted — see finding 1 below.
- [ ] Decide, in the plan, how "warn if a stale `name:` key is present" is detected given rclcpp declare-to-see-override semantics — see finding 3 below.
- [ ] Add explicit documentation that a label is now both a wire identity (max 23 bytes) *and* a ROS 2 parameter-path segment (so it can't contain `.`), per finding 4 — fold into ask item 4/5's doc pass.
- [ ] Fold the ROS 1 lineage (nested-dict `remotes` → ROS 2 flattening manufactured the label → deliberate retirement) from the issue's correction comment into `remote_identity.h`'s header comment or `doc/relay_design.md`, not just the GitHub issue thread — see finding 2.
- [ ] Update the unknown-sender WARN in `src/udp_bridge.cpp` (~1793) whose advice text says `set remotes.<label>.name` — that key will no longer be read (ask item 4, already in scope, restated here so it isn't missed among the smaller test edits).

## Findings

### Scope assessment

**Well-scoped?** Yes. The six ask items are one coherent change — reversing a
single design decision (#63's `resolveRemoteIdentity()` indirection) while
preserving the validation hardening #63 added for an unrelated reason (the
truncation bug). Splitting removal from re-validation would leave an
inconsistent intermediate state (declared-but-unread `name:` with no warning,
or removed reading with unretargeted validation gaps), so this belongs in one
PR. It touches ~7 source files plus 4 docs and several tests, which is real
size, but it's a mechanical collapse-two-namespaces-into-one change, not
feature work — "Improve incrementally" is satisfied by doing it atomically
rather than by fragmenting it.

**Right repo?** Yes — `rolker/udp_bridge`, a package-level parameter and
behavior change, not workspace infrastructure.

**Dependencies:** None blocking. Issue #64 (bench-test plan for the relay hub
scenario) is built around configuring a deliberate label/name mismatch, which
this issue makes impossible to express; the issue's own ask item 6 already
flags simplifying #64's scenario as follow-up, not in-scope here. No other
open issue depends on `remotes.<label>.name` remaining readable — a repo-wide
grep found no configuration in this workspace or its docs using the feature
outside the example file and generated docs.

### Principle alignment

| Principle | Status | Notes |
|---|---|---|
| Capture decisions, not just implementations | Watch | The issue's correction comment establishes real ROS 1 history (verified below) and explicitly asks that this be understood as a *deliberate retirement*, not a fix of an accident. That framing currently exists only in a GitHub issue comment. Recommend it land in `remote_identity.h`'s header comment or `doc/relay_design.md`'s identity section (both already carry #51/#63 history) so a future reader who finds `name:` in an old config sees the "this worked once, was retired on purpose" story without needing GitHub history. |
| Enforcement over documentation | OK | Ask item 3 (warn on stale `name:`) plus keeping #63's hard `on_configure` FAILUREs for length/self-name/empty is the right enforcement mix — a stale-but-inert key is lower severity than the routing-table hazards #63 promoted to FAILURE, so WARN (not FAILURE) is proportionate. |
| A change includes its consequences | Action needed (already scoped) | Ask item 5 already lists `doc/relay_design.md`, `.agents/README.md`'s parameter table, `README.md`'s #51 upgrade note, and `config/example_params.yaml`. Verified all four currently describe the two-namespace behavior in detail (see Consequences below) — the ask list is complete, not aspirational. |
| Test what breaks | Action needed | Several of #63's tests are bound to the label≠name scenario this issue makes unconfigurable. See Test impact below — some become meaningless and should be deleted with justification (coverage preserved elsewhere), one needs semantic inversion, none should just be left in place unretargeted. |
| Only what's needed | Action needed | `resolveRemoteIdentities`' duplicate-identity detection (`label_by_identity` map) exists to catch two *different* labels resolving to the same wire identity via divergent `name:` overrides. Once identity is always exactly the label, two distinct `remotes_list` entries can never collide (only an exact repeat can, and that's already handled as the separate idempotent-repeat case). This whole branch becomes dead code once the override is gone — see finding 1. |

### ADR applicability

| ADR | Triggered | Notes |
|---|---|---|
| 0008 — Follow ROS 2 conventions | Yes | Removing a declared parameter is itself a normal, convention-compliant change; no deviation, no new ADR needed. |
| 0001 — Adopt ADRs | Watch, not required | This is a project-level design reversal with real history, which is exactly what ADRs are for — but `udp_bridge` has no `docs/decisions/`-style project ADR log today (checked: none found), and workspace ADR-0001 governs the workspace repo. Recommend recording the decision in-code/in-doc (see Capture-decisions finding above) rather than opening a new ADR mechanism for one issue. |

### Consequences map hits

Confirmed by reading each file — all four are accurately in scope, and the
extent of two-namespace language in them is larger than a skim suggests:

- `doc/relay_design.md` — the entire "identity must be one namespace" section
  (~lines 147–245) narrates `remotes.<label>.name` as the resolved identity,
  including the upgrade note for the #63 rename and the truncation-rejection
  table row for `remotes.<label>.name`. All of it needs rewriting to "label is
  the identity," not just the one line the issue calls out.
- `.agents/README.md` — the parameter table (line 108) documents
  `remotes.<label>.name` as a real parameter; the pitfalls section (~183–208)
  has a multi-paragraph explanation of the label/name split as a "gotcha for
  future agents," which becomes actively wrong (it currently instructs readers
  that `remotes.<label>.name` "IS a real parameter as of #51").
- `README.md` — narrower: only the #51 upgrade-note context needs a check;
  did not find a name-specific upgrade paragraph there beyond what's in
  `relay_design.md` (worth the planner double-checking, since the issue names
  it explicitly).
- `config/example_params.yaml` — carries both a commented-out `name:
  "robot_a"` example and prose ("The remote's identity on the wire is
  remotes.<label>.name") that must be removed/rewritten, plus the existing
  in-line note "(No `name:` key here: operator_1 calls itself \"operator_1\"
  on the wire)" which is closest to the post-issue mental model and could
  become the norm rather than the exception.

### Findings requiring planner attention

**1. `resolveRemoteIdentities`' duplicate-identity check becomes dead code.**
Today it guards against two *different* `remotes_list` labels resolving to
the same wire name via divergent `name:` overrides
(`DuplicateRemoteIdentityIsRejected` in `test_relay_routing.cpp`). Once
identity is always exactly the label, `remotes_list` is a flat list of
strings — two distinct entries are, by construction, two distinct identities.
The only way two entries can share a string is an *exact* repeat, which is
already handled separately and correctly as the idempotent-repeat case
(`RepeatedLabelIsIdempotentNotACollision`). So the `label_by_identity` map and
its collision branch have no reachable input once the override is gone. This
should be **deleted**, not retargeted — recommend the plan spell this out
explicitly (with a comment at the deletion site referencing why), rather than
mechanically s/name/label/ across the function and leaving unreachable
validation code as a false signal of remaining risk.

**2. The ROS 1 lineage is verified and should be durable, not just in the
issue thread.** Read `/home/roland/archived_project11/noetic/catkin_ws/src/udp_bridge/src/udp_bridge.cpp`
(~lines 90–102): confirmed the comment's claim exactly — `remotes` was an
`XmlRpcValue` struct iterated directly (`for(auto remote : remotes_dict)`),
`remote_info.name = remote.first` (the dict key), with `if
(remote.second.hasMember("name")) remote_info.name =
std::string(remote.second["name"])` as an optional override. ROS 1's nested
parameter dictionaries made this free; ROS 2's flat parameter model forced
`remotes_list` + `remotes.<label>.*`, manufacturing the label as an artefact
of the port. This is exactly what the issue's correction comment says, and it
changes the character of the removal from "delete an accident" to
"deliberately retire a ROS 1 feature with the history known" — worth carrying
into the code/doc comments per the Capture-decisions finding above, since a
future reader hitting `name:` in an old ROS 1 config or the January example
won't see the GitHub issue thread.

**3. Detecting the stale `name:` key needs a concrete mechanism, not just
"warn."** rclcpp only surfaces a YAML-provided parameter override once the
node calls `declare_parameter` for that exact name (this node does not set
`automatically_declare_parameters_from_overrides`, confirmed by grep — no
such call in `udp_bridge.cpp`/`udp_bridge_node.cpp`). So implementing ask
item 3 still requires calling something equivalent to today's
`declareIfMissing("remotes." + label + ".name", std::string())` per label —
the difference from today is that the returned value must be used **only**
for the warning message, never for identity resolution. This is a small
detail but worth the plan stating explicitly, since "stop reading
`remotes.<label>.name`" (ask item 1) could otherwise be read as "stop
declaring it too," which would make the key invisible to the node and the
warning unimplementable.

**4. New constraint: a label is now both the wire identity (≤23 bytes) *and*
a ROS 2 parameter-path segment.** Before this issue, an operator could give a
peer's true wire name (via `remotes.<label>.name`) independently of the label
used to spell `remotes.<label>.connections_list` — so a wire name containing
characters that don't work as a parameter-path segment (a literal `.`, most
notably, since ROS 2 parameter paths use `.` as the nesting separator) was
still expressible. After this issue, the label *is* the wire identity, so
such a name can no longer be configured at all — not truncated, not warned
about, simply inexpressible as a `remotes_list` entry. No configuration in
this workspace hits this (confirmed: `izzy`, `operator`, `lr30`, `boat`,
`ben` are all plain identifiers), so this isn't a blocking objection, but ask
item 4 ("document the constraint that was always implicit") should say so
explicitly — a label containing `.` (or other parameter-path-reserved
characters) is not just discouraged, it's a new hard ceiling on what a wire
identity can be.

**5. Every #63 validation maps onto labels with no gap, except finding 1.**
Enumerated against `remote_identity.h`'s four rejections:
- over-long identity (23-char wire limit) — still needed, now checked
  against the label directly (no more "name vs. label" branch in the error
  message, which simplifies `node_name_too_long_error`'s call site).
- self-name (label == bridge's own `name`) — still needed, unchanged shape.
- empty identity (empty `remotes_list` entry) — still needed, unchanged
  shape (a label is either empty or it isn't; there's no separate "empty
  name but non-empty label" case anymore since there's only one string).
- duplicate identity via divergent overrides — dead, per finding 1.
- repeated-label idempotency — still needed, unchanged.

None of #63's *reachable* protections are lost by this issue; the reduction
in surface is real, not a regression.

### Test impact

Reviewed `test/test_node_name_limits.cpp` and `test/test_relay_routing.cpp`.

- **Becomes meaningless as written, delete with justification (coverage
  preserved elsewhere):**
  - `test_relay_routing.cpp::LabelDifferingFromWireNameStillNeverEchoes` —
    its entire premise (label ≠ configured wire name, and the bridge must
    still never echo) becomes unconfigurable once `name:` isn't read. General
    echo-prevention coverage survives via
    `RelayRouting.EchoIsNeverSentBackToTheSender`, which doesn't depend on a
    label/name split.
  - `test_relay_routing.cpp::DuplicateRemoteIdentityIsRejected` — tests the
    now-dead branch (finding 1). Delete alongside the code it tests.
  - `test_node_name_limits.cpp::OverLongRemoteNameFailsToConfigure` — uses
    the `.remote(label, wire_name)` helper's *name* argument to trigger the
    length rejection distinctly from the label path.
    `OverLongRemoteLabelFailsToConfigure` already covers the same mechanism
    via the label, which is the only path once `name:` is gone.
  - `test_node_name_limits.cpp::RemoteWithOurOwnNameFailsToConfigure` — same
    pattern; `RemoteLabelMatchingOurOwnNameFailsToConfigure` covers the
    surviving mechanism.
- **Needs semantic inversion, not deletion:**
  - `test_relay_routing.cpp::ConfiguredNameIsTheIdentityNotTheLabel` — its
    name and body assert the opposite of the post-issue behavior. Needs to
    become something like `LabelIsTheIdentity` asserting `resolveRemoteIdentity`
    (or its replacement) returns the label unchanged, with no `name:`
    involved at all.
- **Needs a new test:**
  - The stale-`name:`-key WARN (ask item 3) has no coverage today (the key
    never triggered a warning before this issue). Needs at least one new
    test asserting the WARN fires when `remotes.<label>.name` is
    non-empty and does not otherwise affect configuration or the resolved
    identity.
- **Helper repurposing:** `test_node_name_limits.cpp`'s `Bridge::remote(label,
  wire_name)` helper (which declares `remotes.<label>.name`) stays useful —
  it's exactly the shape needed to drive the new stale-key-WARN test — but
  its surviving callers should be re-audited so none of them are silently
  relying on the *name* argument still affecting identity.
- `test/test_remote_node_resend.cpp:736` sets `remote_msg.name` directly on a
  message struct (unrelated to the `remotes.<label>.name` parameter) — not in
  scope, no action needed.

### Knock-on (not in scope here)

Issue #64 (`Bench: three-bridge hub scenario for relay`) is open and paused;
its plan is built around configuring a deliberate label/name mismatch as one
of the scenarios it pins. Per the issue's own ask item 6, that dimension of
the bench plan should be simplified when #64 resumes — noted here only so
the connection isn't lost, no action taken.

### Recommendation

Proceed. This is a well-reasoned, well-scoped removal, verified against the
actual ROS 1 source rather than taken on the operator's word — the correction
comment's claim checks out exactly. No principle or ADR blocks it. The five
findings above are refinements a plan should account for (one of them —
finding 1 — actually *shrinks* the diff relative to a naive "s/name/label/"
reading of the issue), not objections to doing the work.

---
**Authored-By**: `Claude Code Agent`
**Model**: `Claude Sonnet`
