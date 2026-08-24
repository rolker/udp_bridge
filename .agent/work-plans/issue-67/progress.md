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

## Plan Authored
**Status**: complete
**When**: 2026-08-24 00:37 -04:00
**By**: Claude Code Agent (Claude Sonnet)

**Plan**: `.agent/work-plans/issue-67/plan.md` at `da17901`
**Branch**: feature/issue-67 at `da17901`
**Phases**: single

### Open questions
- [ ] No open questions — plan is review-plan-ready.

---
**Authored-By**: `Claude Code Agent`
**Model**: `Claude Sonnet`

## Plan Review
**Status**: complete
**When**: 2026-08-24 00:42 -04:00
**By**: Claude Code Agent (Claude Sonnet) (in-context — author self-review)

**Plan**: `.agent/work-plans/issue-67/plan.md` at `da17901`
**PR**: PR-less
**Verdict**: changes-requested

### Findings
- [ ] (must-fix) Plan tells the implementer to model the new `StaleRemoteNameParameterWarns` test on "this repo's existing WARN-assertion pattern" in `test/test_stale_packet_gate.cpp` — that file has no WARN content whatsoever (it's about `admitForPublish`'s stale-packet drop gate). Grepped every test file in the repo: nothing captures log content today (`rcutils_logging_set/get_logger_level` is used only to suppress noisy WARN spam in `test_remote_node_resend.cpp`, not to assert on it; other "WARN" hits are a `DiagnosticStatus::WARN` enum, unrelated). No existing precedent exists to follow. The plan needs to name a concrete mechanism instead — e.g. `testing::internal::CaptureStderr()`/`GetCapturedStderr()` around `configure()`, asserting the captured text names the label and `remotes.<label>.name` (RCLCPP_WARN_STREAM writes through rcutils to stderr by default, confirmed no custom logging sink is installed anywhere in this package). — `plan.md:74` (step 6, `StaleRemoteNameParameterWarns` bullet)
- [ ] (must-fix) `README.md`'s node-name-length upgrade note (~122–129) is not "unaffected" as the plan claims. It explicitly documents `remotes.<label>.name` as one of the paths whose over-long value "fail[s] the configure transition with a message naming the parameter, the value, its length and the limit." After this change that's false: the key is no longer read for identity or length at all, only WARNed on if non-empty — a stale `name:` of any length, even 1000 characters, no longer fails configure or is even mentioned by the length-limit machinery. The plan's step 7 needs to add a small edit here (drop the `remotes.<label>.name` clause from that path list), not leave it as-is. — `plan.md:66` (step 7, README.md bullet)

### Notes (not findings — verified, plan is accurate)
- The dead-code claim for the `label_by_identity` collision branch (`remote_identity.h:140-148`) checks out: `resolveRemoteIdentities` has exactly one call site (`on_configure`); `add_remote`'s dynamic-remote path (`udp_bridge.cpp:2374-2436`) has its own independent self-name/length checks and never calls it. Once identity is always exactly the label, two distinct `remotes_list` entries cannot converge; only an exact repeat can, and that's the separate, already-correct idempotent-skip path (`identity_by_label.count(entry.first)` at line 104). Deletion is sound, not merely believed.
- The declare-but-don't-read mechanism holds: `udp_bridge.cpp` does not set `automatically_declare_parameters_from_overrides` (confirmed — only `NodeOptions().enable_logger_service(true)`), and the current code (`udp_bridge.cpp:360-362`) already proves `declare_parameter`/`get_parameter` surfaces a YAML override on a per-label-declared key independent of that node option. Keeping the per-label `declareIfMissing("remotes.<label>.name", "")` call for warning purposes only is correct and necessary, as the plan says.
- The 23/24 boundary is right (`packet.h:20,61`: `maximum_node_name_size = 24`, `maximum_node_name_length = 23`), and the `MaximumLengthNamesConfigure` boundary fix (move the at-limit string from the `.name` argument to the label argument) is necessary — verified the pre-fix test would silently stop testing the boundary once `.name` goes unread.
- Every test deletion/rename claim was checked against actual file content and line numbers (`test_relay_routing.cpp`, `test_node_name_limits.cpp`) and matches exactly — line ranges, test bodies, and the "which surviving test covers it" claims all hold.
- The unknown-sender WARN (`udp_bridge.cpp:1789-1796`) fires on the same condition before and after (`configured_remote_names_` is mathematically the same set of strings, just built from labels instead of resolved names) — only the advice text needs to change, as the plan says.
- Plan length (156 lines) earns its place: this is a removal, and the file:line specificity is what let this review verify claims against source rather than trust them — padding would not have supported that.

### Summary
The plan is unusually well-verified against source — every deletion's replacement-coverage claim, every line-number reference, and the resolver-mechanism claim held up under independent checking, including cross-referencing the one place I expected a gap (services/dynamic remotes feeding `resolveRemoteIdentities`) and finding there genuinely isn't one. Two concrete gaps remain before implementation: the new WARN test needs a real mechanism (there is no existing repo pattern to point at), and one README passage the plan calls "unaffected" is not. Both are small, textual fixes to the plan, not design problems.

### Recommended Actions
- [ ] Replace the `test_stale_packet_gate.cpp` pointer in step 6 with a concrete WARN-capture mechanism (e.g. `CaptureStderr`/`GetCapturedStderr`).
- [ ] Add the README.md node-name-length upgrade note (~122–129) to step 7's edit list — drop its `remotes.<label>.name` clause.

## Plan Authored
**Status**: complete
**When**: 2026-08-24 10:34 -04:00
**By**: Claude Code Agent (Claude Opus 5 (1M context))

**Plan**: `.agent/work-plans/issue-67/plan.md` at `b1d4773`
**Branch**: feature/issue-67 at `b1d4773`
**Phases**: single PR

Host-inline edit closing both must-fixes from the `## Plan Review` (`a1b8a90`).
Textual only; no design change, and the review raised no design objection.

1. The new `StaleRemoteNameParameterWarns` test pointed at
   `test_stale_packet_gate.cpp` for "this repo's existing WARN-assertion
   pattern". No such pattern exists — nothing in the package captures log
   content. The one logging-adjacent test does the opposite: it silences the
   logger and asserts through an accessor, stating so outright
   (`test_remote_node_resend.cpp:838-852`, "the cap-enforcement assertion uses
   `dispatchMissWarnedIdCountForTest`, not log inspection"). The plan now
   follows that convention rather than the review's suggested stderr capture —
   expose a test accessor incremented where the WARN fires. Stable against
   wording changes and consistent with the package's own idiom.
2. `README.md`'s node-name-length upgrade note was recorded as unaffected. It
   is not: it states an over-long `remotes.<label>.name` fails the configure
   transition, which stops being true once that key is read only to warn
   about. Now rewritten so rejection is described where it still applies.

The review verified independently, against source, that: the collision-branch
deletion is genuinely dead code (`resolveRemoteIdentities` has one call site;
`add_remote` never feeds it); the declare-but-don't-read mechanism works (no
`automatically_declare_parameters_from_overrides`); the 23/24 boundary and its
`MaximumLengthNamesConfigure` fix are correct; every test deletion's
coverage-survives claim holds; and the unknown-sender WARN fires identically
before and after. Plan length (156 lines) judged as earning its place.

### Open questions
- [ ] No open questions — plan is implementation-ready.

## Implementation
**Status**: complete
**When**: 2026-08-24 09:04 -04:00
**By**: Claude Opus

**Plan**: `.agent/work-plans/issue-67/plan.md` at `8427ae8`
**Branch**: feature/issue-67 (not pushed; no PR opened, as instructed)
**Commits**:
- `96f7648` — identity: a remotes_list entry is the remote's wire name again (#67)
- `f454f5b` — doc: a remotes_list entry is the peer's wire name (#67)
- `8427ae8` — plan: record implementation divergences for #67

### What landed

`resolveRemoteIdentity()` is gone and `resolveRemoteIdentities()` now takes
`const std::vector<std::string>& labels` and returns `std::vector<std::string>`
(first-seen order, repeats collapsed). A `remotes_list` entry is the remote's
wire identity again, as in ROS 1. `on_configure` still declares
`remotes.<label>.name` per entry — rclcpp surfaces a YAML override only for a
declared parameter, and this node sets no
`automatically_declare_parameters_from_overrides` — but reads it solely to
WARN that it is inert, naming the entry in use and what to rename.

Validation did not regress. Over-long (>23), self-name, and empty entries all
still return `CallbackReturn::FAILURE` from `on_configure`, ahead of the socket
bind as before; a repeated entry is still idempotent. Verified by
`NodeNameLimits.OverLongRemoteLabelFailsToConfigure`,
`NodeNameLimits.RemoteLabelMatchingOurOwnNameFailsToConfigure`,
`RelayRouting.EmptyRemoteIdentityIsRejected`,
`RelayRouting.OverLongRemoteIdentityIsRejected`,
`RelayRouting.RemoteResolvingToOurOwnNameIsRejected`, and
`RelayRouting.RepeatedLabelIsIdempotentNotACollision` — all passing.

The unknown-sender WARN (`src/udp_bridge.cpp`) fires on the identical
condition: `configured_remote_names_` is the same set of strings, now built
directly from the entries instead of from resolved names. Only the advice text
changed (rename the `remotes_list` entry, not set `remotes.<label>.name`).

New tests, both asserting through `staleRemoteNameKeyCountForTest()` rather
than log capture, per the package's own convention
(`test_remote_node_resend.cpp:838-852` / `dispatchMissWarnedIdCountForTest`):
`NodeNameLimits.StaleRemoteNameParameterWarns` and
`NodeNameLimits.AbsentRemoteNameParameterIsSilent`. The negative case was
added beyond the plan because asserting the counter is 1 proves nothing
unless something pins that it is 0 when the key is absent.

Docs rewritten in `doc/relay_design.md`, `.agents/README.md` (pitfalls AND the
verified-parameter table — both the `remotes.<label>.name` row and the
`remotes_list` row), `README.md` (the `remotes.<remote_label>.name`
description, the #51 upgrade note rewritten as the #67 one, the node-name-length
upgrade note corrected per the plan's own must-fix, and a fourth
empty/self-name passage the plan did not name), and
`config/example_params.yaml`. Both required statements are in all four: **your
label for a peer must be the name that peer calls itself**, and the retired
capability (an entry is now both a 23-byte wire identity and a ROS 2
parameter-path segment, so a peer whose wire name contains a `.` is
unconfigurable; no current config hits this).

### Deletions and where the protection now lives

| Deleted | Coverage now |
|---|---|
| `resolveRemoteIdentity()` | Its unset-name behaviour (label is the identity) is now the only behaviour; pinned by `RelayRouting.LabelIsTheIdentity` |
| `label_by_identity` map + collision branch in `resolveRemoteIdentities()` | Provably dead, re-verified against the changed code: with the entry as the identity, two distinct entries are two distinct identities by construction; the only repeat possible is an exact one, handled by the idempotent-skip path. `RelayRouting.RepeatedLabelIsIdempotentNotACollision` covers it and now also asserts the collapse. Reasoning recorded at the deletion site in `remote_identity.h` so it is not "restored" later |
| `RelayRouting.DuplicateRemoteIdentityIsRejected` | Tests exactly that dead branch; deleted with it |
| `RelayRouting.LabelDifferingFromWireNameStillNeverEchoes` | Premise no longer expressible. Echo prevention: `RelayRouting.EchoIsNeverSentBackToTheSender` and `RelayRouting.MaximumLengthIdentityStillClosesTheLoopRule`, neither of which depends on a label/name split |
| `RelayRouting.EmptyRemoteIdentityIsRejected`'s "empty label, real name" half | Not expressible; the empty-entry rejection it shared the test with is retained and still asserted |
| `NodeNameLimits.OverLongRemoteNameFailsToConfigure` | `NodeNameLimits.OverLongRemoteLabelFailsToConfigure` drives the same rejection through the only remaining path |
| `NodeNameLimits.RemoteWithOurOwnNameFailsToConfigure` | `NodeNameLimits.RemoteLabelMatchingOurOwnNameFailsToConfigure`, which now also carries the deleted test's assertion message |
| The "known on the wire as" INFO in `on_configure` | Unreachable — `remote_info.name` always equals the entry. No behaviour to cover |

Nothing was deleted whose protection could not be named.

`NodeNameLimits.MaximumLengthNamesConfigure` moved its at-limit string from the
`.name` argument to the entry. Left where it was, the test would have compiled
and passed while silently no longer exercising the 23-char boundary — the
sharpest silent-coverage-loss risk in this change.

### Build and test (verbatim)

    $ ./build.sh udp_bridge_interfaces udp_bridge ; echo "BUILD_EXIT=$?"
    BUILD_EXIT=0
    Summary: 2 packages finished [53.9s]
      1 package had stderr output: udp_bridge

    $ ./test.sh udp_bridge ; echo "TEST_EXIT=$?"
    TEST_EXIT=0
    Summary: 1 package finished [32.4s]

    $ colcon test-result --verbose | tail -3
    Summary: 251 tests, 0 errors, 0 failures, 14 skipped
    RESULT_EXIT=0

Exit codes read directly, not grepped from output. The udp_bridge stderr is
the pre-existing `-Wpedantic` flexible-array-member warning stream from
`packet.h`, unchanged by this work.

    $ .venv/bin/pre-commit run --from-ref origin/jazzy --to-ref HEAD
    ... 11 hooks: 8 Passed, 3 Skipped (no files to check), 0 Failed
    PRECOMMIT_EXIT=0

Confirmed the new tests actually executed by reading the gtest XML:
`StaleRemoteNameParameterWarns`, `AbsentRemoteNameParameterIsSilent`,
`LabelIsTheIdentity`, and the renamed/trimmed survivors are all present.

Note for the record: the host root filesystem is at 100% with ~7.1 GB free.
Nothing failed for disk space, but the margin is thin.

### Deferred / not done

- **Not pushed, no PR opened** — per the task instructions.
- **Issue #64 (bench: three-bridge hub scenario)** builds a deliberate
  label/name mismatch into one of its scenarios, which this change makes
  inexpressible. Out of scope per the issue's own ask item 6; #64's plan needs
  simplifying when it resumes. No action taken here.
- **No project ADR** recorded. `udp_bridge` has no `docs/decisions/` log; per
  the Issue Review the decision is captured in `remote_identity.h`'s header
  comment and `doc/relay_design.md` instead.

---
**Authored-By**: `Claude Code Agent`
**Model**: `Claude Opus`

## Local Review (Pre-Push)
**Status**: complete
**When**: 2026-08-24 09:15 -04:00
**By**: Claude Code Agent (Claude Opus)
**Verdict**: changes-requested

**Branch**: feature/issue-67 at `11bc263`
**Mode**: pre-push
**Depth**: Deep (reason: 12 files / ~1485 lines, six test deletions, and the change re-keys the wire identity every routing decision compares)
**Must-fix**: 3 | **Suggestions**: 8
**Round**: 1 | **Ship**: continue — three must-fixes at round 1; all mechanical, one address-findings pass should close them

Specialists: Static analysis (pre-commit, exit 0) · Governance + Plan Drift ·
Docs Accuracy · Claude Adversarial Lens A · Claude Adversarial Lens B.
Copilot off (not requested). Local Adversarial skipped: Ollama unavailable on
this host (OOM).

Build/test verified independently on a clean rebuild (`rm -rf build/udp_bridge`):
BUILD_EXIT=0, TEST_EXIT=0, `colcon test-result --verbose` = 251 tests,
0 errors, 0 failures, 14 skipped. Only the pre-existing `-Wpedantic`
flexible-array-member warnings from `packet.h`. The 253 → 251 drop is fully
accounted for: `test_relay_routing` 22 → 20, `test_node_name_limits` 7 → 7
(2 deleted, 2 added).

### Deletion audit (each deletion read against its claimed replacement)
Every deletion's coverage genuinely survives; nothing was deleted whose
protection could not be named in the current tree.
- `label_by_identity` collision branch + `DuplicateRemoteIdentityIsRejected` —
  branch is provably unreachable (two distinct entries are two distinct
  identities); exact repeat still covered by
  `RepeatedLabelIsIdempotentNotACollision`, which now also asserts the collapse.
- `LabelDifferingFromWireNameStillNeverEchoes` — premise unconfigurable; its
  three residual assertions survive in `EchoIsNeverSentBackToTheSender`,
  `ExcludedRemoteRateStateUntouched` and `MaximumLengthIdentityStillClosesTheLoopRule`.
- `OverLongRemoteNameFailsToConfigure` / `RemoteWithOurOwnNameFailsToConfigure` —
  label-path twins remain and now carry the deleted tests' assertion messages.
- `EmptyRemoteIdentityIsRejected`'s "empty label, real name" half — not
  expressible; the empty-entry rejection it shared the test with is retained.
- The "known on the wire as" INFO — unreachable; `remote_info.name` always
  equals the entry.

### Verified against source (not from the Implementation entry)
- No validation regression. Over-long (>23), self-name and empty entries all
  still return `CallbackReturn::FAILURE`, at `udp_bridge.cpp:390` — ahead of
  `socket()` (`:395`) and `bind()` (`:408`). The #63 round-3 socket-leak
  property holds. Lens A drove the empty-entry case end-to-end through the real
  node and confirmed clean `FAILURE`, no exception from the
  `"remotes..name"` declare that precedes validation.
- `MaximumLengthNamesConfigure` is now stronger, not weaker: the at-limit
  string moved to the `remotes_list` entry, the only path reaching
  `node_name_fits()`, and is simultaneously exercised as a 23-char parameter-path
  segment. This was the sharpest silent-coverage-loss risk and it was handled.
- Declare-but-don't-read: `declareIfMissing` runs for every label before the
  `stale_name.empty()` continue; counter reset at `:363`, incremented at `:371`
  exactly at the WARN site; `AbsentRemoteNameParameterIsSilent` is genuinely
  discriminating (an unconditional increment yields 1 vs the asserted 0).
- `configured_remote_names_` is the identical set for every configuration
  expressible both before and after (label == resolved identity whenever
  `.name` is unset). Mutex discipline preserved: clear+insert stay one critical
  section.
- `packet.h` is comment-only — it replaces a reference to the deleted collision
  check with the still-true truncation hazard. No wire-format, constant or
  predicate change. Justified.
- Docs: both required statements present and correct in all four targets
  (label-must-equal-peer's-own-name; a `.` in a wire name is now unconfigurable).
  `example_params.yaml` is internally consistent and loads.

### Findings
- [x] (must-fix) Roadmap comment names three tests deleted by this commit and states the retired `remotes.<label>.name`-else-label rule as current — the exact misconception #67 exists to remove, ~140 lines above the test that says the opposite. Cross-confirmed by Governance and Lens A — `udp_bridge/test/test_relay_routing.cpp:33-43`
- [x] (must-fix) The repo's own bench config still sets the retired key (`name: "boat"`, `name: "operator"`), contradicting this PR's own "delete it from your config" instruction and putting a permanent spurious WARN in the #18 bench harness. Cross-confirmed by Docs and Lens B — `udp_bridge/test/bench/configs/three_path.yaml:33,59`
- [x] (must-fix) The newly-prescribed upgrade remedy ("rename the `remotes_list` entry") does not work on a live re-configure and the docs do not say so: `remote_nodes_` and `subscribers_` are never cleared or erased anywhere (verified), so deactivate→cleanup→configure after a rename leaves the old `RemoteNode` resident with the same host/port and every send goes out twice to the same peer — doubled uplink on the rate-limited links this package exists for. Add "restart the node" to the three places that give this advice; code-level pruning is a follow-up, not this PR — `udp_bridge/README.md` (#67 upgrade note), `udp_bridge/doc/relay_design.md:211-219`, `udp_bridge/src/udp_bridge.cpp:372-378`
- [x] (suggestion) The WARN is self-contradictory in the most likely case — when the stale value equals the label it prints "…is set to 'boat' … rename the remotes_list entry … to 'boat'". Special-case equality to "delete the redundant key; it already matches the entry" — `udp_bridge/src/udp_bridge.cpp:372-378`
- [x] (suggestion) The thread-safety rationale is false on a re-configure, and the neighbouring `configured_remote_names_` comment says so explicitly. The member is safe for a different reason (nothing but the test accessor ever reads it). Cross-confirmed Lens A + Lens B — `udp_bridge/include/udp_bridge/udp_bridge.h:509-513`
- [x] (suggestion) The counter counts occurrences, not keys: the loop iterates `remotes_list` un-deduplicated, so a repeated entry double-WARNs and reports 2 from an accessor documented as counting keys — for a config `RepeatedLabelIsIdempotentNotACollision` blesses as legal — `udp_bridge/src/udp_bridge.cpp:363-379`
- [x] (suggestion) The counter is not reset in `on_cleanup` nor on the `on_configure` paths that fail before `:363`, so "seen by the most recent on_configure" is only approximately true — `udp_bridge/include/udp_bridge/udp_bridge.h:514`
- [x] (suggestion) No lifecycle-level test for an empty `remotes_list` entry, while both its siblings (over-long, self-name) have one. The `on_configure` path has an untested step for this input — it declares `"remotes..name"` before validating — and the `Bridge` helper makes the guard a two-liner — `udp_bridge/test/test_node_name_limits.cpp`
- [x] (suggestion) Docs omit one condition on the unknown-sender WARN: it is skipped entirely when no remote is statically configured (`!configured_remote_names_.empty()`, `udp_bridge.cpp:1804`). `relay_design.md` already documents the throttle and once-per-name nuances and is the natural home — `udp_bridge/README.md:131`, `udp_bridge/doc/relay_design.md:166`
- [x] (suggestion) Pre-existing, but this PR edits the file: "0 = default (500000)" is wrong — `Connection::default_rate_limit = 50000`. `.agents/README.md:179-182` carries a pitfall entry whose only purpose is to warn agents about this comment; fixing it lets that entry retire — `udp_bridge/config/example_params.yaml:83`
- [x] (suggestion) Pre-existing, but in a table labelled verified and this PR edits it: "20 gtest targets" — `CMakeLists.txt` has 22; missing are `test_drop_baseline` and `test_node_name_limits`, this change's own suite — `.agents/README.md:28`

### Considered and not raised as findings
- **Warn-only on a stale key that *differs* from the label** silently reproduces
  the #51 echo bug on upgrade, and is inconsistent with the FAILURE the same
  function returns for every other identity error (Lens B raised it as
  must-fix). Not a defect: issue #67 ask item 3 specifies exactly this —
  "Warn if the now-unread key is present, so a config carrying a stale `name:`
  is told rather than silently ignored." Recorded here so the operator can
  revisit it deliberately if wanted (fail when non-empty and `!= label`; warn
  only when equal). No config in the workspace sets the key, so nothing is
  currently exposed.
- Plan's *Files to Change* table omits `include/udp_bridge/udp_bridge.h`
  (accessor + member); the Divergences section covers `packet.h` but not this.
  Below the suggestion threshold.
- Ask item 6 (#64 bench-plan simplification) is correctly deferred and the
  deferral is stated in the Implementation entry — worth a one-line note on #64
  when the PR opens.
- `.agents/review-context.yaml` does not exist in this repo (pre-existing gap).

---
**Authored-By**: `Claude Code Agent`
**Model**: `Claude Opus`

## Implementation
**Status**: complete
**When**: 2026-08-24 09:34 -04:00
**By**: Claude Code Agent (Claude Opus)

**Branch**: feature/issue-67 at `9c4402b` (not pushed; no PR opened, as instructed)
**Addressed**: `## Local Review (Pre-Push)` (2026-08-24 09:15, branch at `11bc263`) — 3 must-fixes, 8 suggestions, plus the downgraded finding the operator then ruled on
**Commits**:
- `b2dc1b9` — identity: reject a config that still sets the removed remotes.<label>.name (#67)
- `4b1bcaf` — test: correct the relay-routing roadmap comment (#67)
- `3ae0cbd` — doc: a config that still sets remotes.<label>.name is rejected, not warned (#67)
- `99e5e90` — doc: the unknown-sender WARN is skipped when no remote is configured (#67)
- `d106b91` — doc: maximum_bytes_per_second's 0-default is 50000 B/s, not 500000 (#67)
- `5615c9b` — doc: the package inventory lists 22 gtest targets, not 20 (#67)
- `9c4402b` — plan: record the operator decision to reject the retired key for #67

### The operator decision (2026-08-24) — design change, done first

The review's downgraded finding recorded that warn-only left exactly one
config shape — a `remotes.<label>.name` value *differing* from its label —
announced and then ignored, and that shape is the #51 echo bug. The
operator ruled: *"fail if any name is present since it's removed and not
supported."*

Implemented in `b2dc1b9`. A non-empty `remotes.<label>.name` now returns
`CallbackReturn::FAILURE` from `on_configure`, with **no carve-out** for a
value equal to its label — presence is the error, not effect. The parameter
is still declared, unchanged, because that is the only way rclcpp surfaces a
YAML override so the key can be detected at all. The FAILURE returns at
`udp_bridge.cpp:378-398`, ahead of `socket()` and `bind()`, alongside the
other identity failures — verified by reading the surrounding code, so #63
round 3's no-socket-leak/no-EADDRINUSE-`exit(1)` property is preserved. The
ERROR names the parameter, the label, the value, tells the operator to
delete the key and make the `remotes_list` entry the peer's wire name, and
carries the restart-the-node caveat from must-fix 3. An explicitly empty
value is indistinguishable from an absent key (the declared default is
`""`) and configures normally; every doc target says so.

**Test accessor: removed, per the instruction to reconsider it.**
`staleRemoteNameKeyCountForTest()` and `stale_remote_name_key_count_`
existed only because a WARN is not observable in a package that captures no
log content anywhere. A FAILURE is directly observable in the state the node
lands in, so the accessor was unnecessary complexity; the tests assert the
transition result instead. This also obviated three of the review's
suggestions outright (see below). The negative test is kept, as
`AbsentRemoteNameParameterConfiguresCleanly`.

**Tests that fail without the change** (all four in
`test/test_node_name_limits.cpp`, driving the real lifecycle transition):
`RetiredRemoteNameParameterFailsToConfigure` (differing value) and
`RetiredRemoteNameMatchingItsLabelStillFailsToConfigure` (equal value — the
no-carve-out case) both expect `UNCONFIGURED` and returned `INACTIVE` under
the previous code; `AbsentRemoteNameParameterConfiguresCleanly` pins the
negative; `EmptyRemoteLabelFailsToConfigure` is suggestion 5's missing
lifecycle sibling.

**Must-fix 2 was load-bearing, and proved it.** With the hard failure in
place and `test/bench/configs/three_path.yaml` still setting the key on both
bridges, `test/bench/test_smoke.py::test_smoke_pub_reaches_sub` failed with
"Transitioning failed" — the #18 bench harness stops running entirely. Both
keys removed in the same commit, since the config change is what makes the
behaviour change viable rather than a separate tidy-up. Verified by
`grep -rn '^\s*name:' --include=*.yaml` across the repo that no other config
sets it (the remaining hits are the bridges' own `name` parameter, which is
current, and `rosdoc2.yaml`, unrelated).

### Actions

- [x] (must-fix) Roadmap comment names three deleted tests and states the retired rule as current — `test/test_relay_routing.cpp:33-43` — **fixed** (`4b1bcaf`). Rewritten to name the tests that are actually there (`LabelIsTheIdentity`, `EmptyRemoteIdentityIsRejected`, `RemoteResolvingToOurOwnNameIsRejected`, `RepeatedLabelIsIdempotentNotACollision`) and to state the single-namespace rule, with the removed key's rejection pointed at `test_node_name_limits.cpp` where it is pinned. Comment-only.
- [x] (must-fix) Bench config still sets the retired key — `test/bench/configs/three_path.yaml:33,59` — **fixed** (`b2dc1b9`). Now required for the bench to configure at all, not cosmetic; see above.
- [x] (must-fix) The rename remedy does not survive a live re-configure — `README.md`, `doc/relay_design.md:211-219`, `src/udp_bridge.cpp:372-378` — **fixed** (`b2dc1b9` for the failure text, `3ae0cbd` for the docs). Re-verified against source: `on_cleanup` clears `configured_remote_names_` (`:807`) but neither `remote_nodes_` nor `subscribers_`, so a deactivate→cleanup→configure after a rename leaves the old `RemoteNode` on the same host/port and every send goes out twice. "Restart the node" now appears in the README #67 upgrade note, `relay_design.md`'s Upgrading section, `.agents/README.md`'s pitfalls (a fourth place, added beyond the finding), and the ERROR text itself. **Code-level pruning is out of scope for this PR — see Follow-up candidates below.**
- [x] (suggestion) Self-contradictory WARN when the stale value equals the label — `src/udp_bridge.cpp:372-378` — **resolved by the operator decision** (`b2dc1b9`). There is no longer a WARN to contradict itself: the equal case fails identically to the differing case, deliberately and with no carve-out.
- [x] (suggestion) The counter's thread-safety rationale is false on a re-configure — `include/udp_bridge/udp_bridge.h:509-513` — **obviated** (`b2dc1b9`): the member and its comment no longer exist.
- [x] (suggestion) The counter counts occurrences, not keys, on a repeated entry — `src/udp_bridge.cpp:363-379` — **obviated** (`b2dc1b9`): counter removed. (Also moot on its own terms — the loop now returns on the first non-empty key.)
- [x] (suggestion) The counter is not reset in `on_cleanup` nor on early-failure paths — `include/udp_bridge/udp_bridge.h:514` — **obviated** (`b2dc1b9`): counter removed.
- [x] (suggestion) No lifecycle-level test for an empty `remotes_list` entry — `test/test_node_name_limits.cpp` — **fixed** (`b2dc1b9`). `EmptyRemoteLabelFailsToConfigure` drives the real transition, covering the one input that makes `on_configure` declare `"remotes..name"` before validating.
- [x] (suggestion) Docs omit that the unknown-sender WARN is skipped when no remote is statically configured — `README.md:131`, `doc/relay_design.md:166` — **fixed** (`99e5e90`). Verified the condition at `udp_bridge.cpp:1823`. Both docs now say the absence of the warning is not evidence that identities agree — on a purely dynamic bridge it can never fire.
- [x] (suggestion) `example_params.yaml` "0 = default (500000)" is wrong — `config/example_params.yaml:83` — **fixed** (`d106b91`). `Connection::default_rate_limit` is 50000 (`connection.h:243`); 500000 is the `SO_RCVBUF`/`SO_SNDBUF` size (`udp_bridge.cpp:446,450`) and, confusingly, also the value the example explicitly sets. `.agents/README.md`'s pitfall entry now records the fact rather than the erratum, as the suggestion anticipated.
- [x] (suggestion) `.agents/README.md:28` says 20 gtest targets; `CMakeLists.txt` has 22 — **fixed** (`5615c9b`). `test_drop_baseline` and `test_node_name_limits` were missing; the list is now in CMakeLists declaration order so the next drift shows up as a diff.

### Docs flipped from "warned" to "rejected"

Every target this branch had already touched described the wrong behaviour
after the operator decision. Each was re-read in full and rewritten
(`3ae0cbd`):

| File | What changed |
|---|---|
| `udp_bridge/README.md` | The `remotes.<remote_label>.name` parameter description; the #67 upgrade note (refused on presence, no carve-out, *why* — a warning would leave the differing-value shape running); the #51 node-name-length note's parenthetical, which said an over-long stale key is only warned about; plus the restart caveat |
| `udp_bridge/doc/relay_design.md` | The Upgrading section — rejection, the reasoning, the pre-socket ordering, and the restart caveat |
| `.agents/README.md` | The verified-parameter table row for `remotes.<label>.name` **and** the #67 pitfall bullet, plus a new pitfall for the rename/restart hazard |
| `udp_bridge/config/example_params.yaml` | The `remotes_list` comment block |
| `udp_bridge/include/udp_bridge/remote_identity.h` | The design-history header comment (a fifth target the review did not name — it said the key is "read ONLY to warn that it is inert") |

Swept for stragglers with `grep -rn "inert\|retired\|Retired"` across
`*.md`, `*.h`, `*.cpp`, `*.yaml`; the remaining hits are the unrelated
mininet README, the "relay is supposed to be inert" phrasing about the loop
rule, and this work plan.

### Plan sync

`plan.md` specified WARN throughout. Steps 2, 6 and 7, the *Files to
Change* table and the principles table are edited inline to match, and two
new divergence entries record the decision (6) and the accessor removal (7),
with divergences 3 and 4 marked superseded (`9c4402b`).

### Build and test (verbatim)

    $ ./build.sh udp_bridge ; echo "BUILD_EXIT=$?"
    BUILD_EXIT=0
    Summary: 1 package finished [48.5s]
      1 package had stderr output: udp_bridge

    $ ./test.sh udp_bridge ; echo "TEST_EXIT=$?"
    TEST_EXIT=0

    $ colcon test-result --verbose ; echo "RESULT_EXIT=$?"
    Summary: 253 tests, 0 errors, 0 failures, 14 skipped
    RESULT_EXIT=0

    $ /home/roland/project11/.venv/bin/pre-commit run --from-ref origin/jazzy --to-ref HEAD
    ... 11 hooks: 8 Passed, 3 Skipped (no files to check), 0 Failed
    PRECOMMIT_EXIT=0

Exit codes read directly, never grepped from output. 253 tests, up from the
251 floor: four tests added and two removed in
`test_node_name_limits` (7 → 9). The `udp_bridge` stderr is the pre-existing
`-Wpedantic` flexible-array-member stream from `packet.h`, plus two
pre-existing warnings at `udp_bridge.cpp:2154` (`-Wunused-parameter`) and
`:2254` (`-Wsign-compare`) — both confirmed by `git log -L` to predate this
branch (`cbaeef3`, `3d34a50`) and untouched by it.

New tests confirmed present in the gtest XML:
`RetiredRemoteNameParameterFailsToConfigure`,
`RetiredRemoteNameMatchingItsLabelStillFailsToConfigure`,
`AbsentRemoteNameParameterConfiguresCleanly`,
`EmptyRemoteLabelFailsToConfigure`.

Disk note for the record: the host root filesystem remains at 100% with
~7 GB free. Nothing failed for disk space.

### Follow-up candidates (host to file)

1. **Prune `remote_nodes_` and `subscribers_` on `on_cleanup`** (or erase
   entries whose identity is no longer in `remotes_list` on
   `on_configure`). Today a rename applied by deactivate→cleanup→configure
   leaves the old `RemoteNode` resident on the same host/port, so every
   send goes out twice to the same peer — doubled uplink on rate-limited
   links. Documented as "restart the node" in four places by this PR;
   code-level pruning was explicitly out of scope here.
2. **Issue #64 (bench: three-bridge hub scenario)** builds a deliberate
   label/name mismatch into one scenario. It was inexpressible after the
   original #67 change and is now a hard configure failure, so #64's plan
   needs simplifying when it resumes. Unchanged from the previous entry.

### Not done

- **Not pushed, no PR opened** — per the task instructions.

---
**Authored-By**: `Claude Code Agent`
**Model**: `Claude Opus`

## Local Review (Pre-Push)
**Status**: complete
**When**: 2026-08-24 10:04 -04:00
**By**: Claude Code Agent (Claude Opus)
**Verdict**: changes-requested

**Branch**: feature/issue-67 at `3f1ad40`
**Mode**: pre-push
**Depth**: Deep (reason: the fix pass changed the design — a WARN became a lifecycle FAILURE — and re-touched five doc targets plus the bench config)
**Must-fix**: 1 | **Suggestions**: 5
**Round**: 2 | **Ship**: recommended — one must-fix, down from three; it is a precise doc/ERROR-text correction in four named places with an obvious replacement, not a design question. Fix it and push; no third review round is warranted.

Specialists: Static analysis (pre-commit over `origin/jazzy..HEAD`, exit 0) ·
Governance + Plan Drift · Docs Accuracy · Claude Adversarial Lens A · Claude
Adversarial Lens B. Copilot off (not requested). Local Adversarial skipped:
Ollama unavailable on this host (OOM).

Build/test re-run independently in this worktree: BUILD_EXIT=0, TEST_EXIT=0,
`colcon test-result --verbose` = **253 tests, 0 errors, 0 failures, 14
skipped**, RESULT_EXIT=0. Exit codes read directly. `test_bench_smoke`
ran (1 test, 0 skipped, 0 failures) — so the bench genuinely configures with
the retired keys removed, which is the fix pass's own must-fix-2 claim
confirmed by execution rather than narrative. Disk: root at 99%, ~22 GB
free; nothing failed for space.

### The round-1 must-fixes, verified against source
- **Roadmap comment** — `test_relay_routing.cpp:33-51` now names
  `LabelIsTheIdentity`, `EmptyRemoteIdentityIsRejected`,
  `RemoteResolvingToOurOwnNameIsRejected`,
  `RepeatedLabelIsIdempotentNotACollision`; all four exist verbatim in the
  file, as do every other test the roadmap names. It states the
  single-namespace rule and points the removed key's rejection at
  `test_node_name_limits.cpp`. Fixed.
- **Bench config** — both retired keys gone from `three_path.yaml`. Swept
  the whole repo and the whole workspace: no other config sets
  `remotes.<label>.name`. The removed values (`boat`, `operator`) were
  identical to their labels, so every bench topic path and `wait_ready.py`
  identity is unchanged. Fixed and load-bearing.
- **Restart caveat** — present in `README.md:139`, `relay_design.md:238`,
  `.agents/README.md:208`, `example_params.yaml:56` and the ERROR text
  (`udp_bridge.cpp:394`). Present — but see must-fix 1: the *reason* those
  places give is wrong.

### The operator decision, verified against source
- **FAILURE ordering holds.** `on_configure` spans `:82-717`. Every
  `return CallbackReturn::FAILURE` is at `:95` (over-long local `name`),
  `:217` (give-up thresholds), `:397` (**new** — retired key) and `:409`
  (identity error). `socket()` is at `:414`, `bind()` at `:427`. There is
  no other early return. #63 round 3's no-socket-leak / no-EADDRINUSE-
  `exit(1)` property is preserved by the new path. Cross-confirmed by both
  adversarial lenses.
- **Accessor removal is clean.** `staleRemoteNameKeyCountForTest` and
  `stale_remote_name_key_count_` appear nowhere in the worktree outside the
  work-plan narrative, and nowhere under `layers/main/`. The two deleted
  tests only ever asserted 0 and 1; no test exercised a multi-key tally, so
  the counter's removal dropped no covered case (the *behaviour* it implied
  survives as suggestion 2).
- **The two rejection tests are discriminating**: revert the `return` to
  the old WARN and both flip `kUnconfigured` → `kInactive` and fail.
  `AbsentRemoteNameParameterConfiguresCleanly` guards the inverse
  over-rejection and is what pins the empty-value edge (`Bridge::remote`
  declares the key with an empty value, so a presence-of-key rule would
  fail it). `EmptyRemoteLabelFailsToConfigure` is new coverage of
  pre-existing behaviour, not a discriminating test for this change — the
  Implementation entry says so accurately.
- **Empty-value edge** is implemented as documented: only a non-empty value
  returns FAILURE, and `"remotes.<label>.name"` at `:382` is the parameter's
  only occurrence in `src/` or `include/` — `addRemote` and the CONNECT
  path never read it.
- **Blast radius is zero.** Grepped every udp_bridge config under
  `layers/main` (bizzyboat, izzyboat, ben, lr30, echo, dev_bridge, sim,
  operator variants): none sets `remotes.<label>.name`.
- **Docs flipped warn→rejected everywhere.** Swept `*.md`, `*.h`, `*.cpp`,
  `*.yaml` for "warn"/"inert"/"WARNING" about this key: no straggler. The
  remaining hits are the unrelated local-name truncation history, the "relay
  is supposed to be inert" phrasing about the loop rule, and
  `DiagnosticStatus::WARN`.
- **The three collateral doc-fact commits are each right**: 22
  `add_udp_bridge_gtest` targets, names *and* order matching
  `.agents/README.md:28` exactly (`99e5e90`… `5615c9b`);
  `Connection::default_rate_limit = 50000` at `connection.h:243`, with
  500000 confirmed as the `SO_RCVBUF`/`SO_SNDBUF` size at
  `udp_bridge.cpp:446,450`; the unknown-sender WARN is gated on
  `!configured_remote_names_.empty()` at `udp_bridge.cpp:1823`.
- **Plan sync** is thorough: divergences 6 (the decision) and 7 (the
  accessor removal) added, 3 amended and 4 marked superseded, and the
  WARN language replaced throughout steps 2, 6, 7 and both tables. No plan
  drift finding.

### Findings
- [x] (must-fix) The restart caveat's stated *mechanism* is false on every fixed-port deployment, which is all of them. `socket_` is never closed anywhere (no `close()`, no destructor, `on_cleanup` does not touch it) and no `SO_REUSEADDR` is ever set, so a deactivate→cleanup→configure re-enters `socket()`/`bind()` at `:414`/`:427` and the second bind on the same fixed port returns `EADDRINUSE` → `exit(1)` at `:430` — *before* the duplicate `RemoteNode` at `:517` can exist. The doubled-uplink story is only reachable with `port: 0`. The advice ("restart the node") stays correct and should stay; the reason must be replaced with what actually happens — the re-bind fails and the process exits, which under the shipped `launch/udp_bridge_launch.py` (`respawn=True, respawn_delay=2`) is self-healing. Reproduced the `EADDRINUSE` directly. Four places carry the wrong mechanism (`example_params.yaml:56` states only the advice and is fine). Raised by Lens B, verified independently — `udp_bridge/src/udp_bridge.cpp:393-396`, `udp_bridge/README.md:139-145`, `udp_bridge/doc/relay_design.md:238-244`, `.agents/README.md:203-208`
- [x] (suggestion) `socket_` is leaked and the port stays bound for the process lifetime, which is the root cause of must-fix 1 and silently defeats the `declareIfMissing` reentrancy discipline documented at `udp_bridge.h:299-310`. A `close(socket_)` + `socket_ = -1` in `on_cleanup` would make cleanup→configure actually work. Pre-existing and out of scope here — belongs with the existing `remote_nodes_`/`subscribers_` pruning follow-up, which it is the other half of — `udp_bridge/src/udp_bridge.cpp:414`, `on_cleanup` `:780-810` (deferred: out of scope for a config-parameter removal; its home is rolker/udp_bridge#66, which asks for the close plus the on_shutdown half and the cleanup→configure round-trip test)
- [x] (suggestion) First-offender-only: the `return` is inside the loop, so a hub with three legacy `name:` keys reports one, and recovery needs a process restart per key (see must-fix 1). Accumulating the offending labels and returning FAILURE after the loop is a three-line change. Counter-argument, which is why it is not a must-fix: `resolveRemoteIdentities` is first-error too, so this is the house style. Cross-confirmed Lens A + Lens B — `udp_bridge/src/udp_bridge.cpp:380-398`
- [x] (suggestion) The implemented rule is narrower than the documented one: the loop iterates `remotes_list`, so a `remotes.<label>.name` under a block *not* listed there is never declared, never read and never rejected. Defensible (the orphaned block is wholly inert), but the docs say flatly that setting it at all is a configuration error. A half-sentence qualifier ("for a remote listed in `remotes_list`") makes them match — `udp_bridge/README.md:95`, `.agents/README.md:191`
- [x] (suggestion) "the node will tell the operator so by refusing to start" overstates the observable symptom: `main()` spins regardless of the transition outcome, and `diagnostic_updater_` is created only *after* the failure point, so there is no `/diagnostics` row, no `bridge_info` and no heartbeat — the sole signal is one ERROR line on stderr, on the node that *is* the telemetry link. Same class as the pre-existing over-long-name failure, so not a regression; worth one clause since this pass is about docs matching behaviour — `.agents/README.md:203`
- [x] (suggestion) The comment at `udp_bridge.cpp:340-346` claims identity validation runs "ahead of every resource this callback acquires"; `add_on_set_parameters_callback` is acquired at `:288`, and a mistyped YAML scalar can still throw out of the connections/topics loops *after* the bind (`:544-640`), which `rclcpp_lifecycle` swallows. Pre-existing, but the new FAILURE path is the second to lean on the claim — narrow it to "explicit failure paths". Cross-confirmed Lens A + Lens B — `udp_bridge/src/udp_bridge.cpp:340`, `:508`, `:523`

### Considered and not raised as findings
- Issue #67's ask item 3 says "**Warn** if the now-unread key is present";
  the branch fails instead. That is the operator's 2026-08-24 ruling and is
  recorded in `plan.md` divergence 6 and the Implementation entry, so it is
  a documented deviation, not drift. Worth one line in the PR body when it
  opens so a reader of #67 does not score ask item 3 as unmet.
- `plan.md`'s *Files to Change* table still omits
  `include/udp_bridge/udp_bridge.h`, which this pass edits (accessor
  deletion). Below the suggestion threshold; divergence 7 covers the change
  in prose.
- `README.md:176-178` has ragged line wrapping left by the edit. Cosmetic.
- `.agents/review-context.yaml` does not exist in this repo (pre-existing gap).

---
**Authored-By**: `Claude Code Agent`
**Model**: `Claude Opus`

## Implementation
**Status**: complete
**When**: 2026-08-24 10:12 -04:00
**By**: Claude Code Agent (Claude Opus)

**Branch**: feature/issue-67 at `7e94b36`
**Addressed**: `## Local Review (Pre-Push)` (2026-08-24 10:04 -04:00, round 2, branch at `3f1ad40`, verdict changes-requested) — 1 must-fix, 5 suggestions
**Commits**: `5e9a09a`, `eda47bd`, `9835ed5`, `5442b05`, `7e94b36`

### Actions
- [x] (must-fix) The restart caveat's mechanism — `5e9a09a`. Re-verified against source before writing, since taking a mechanism on trust is what produced the error: `grep` over `src/` + `include/` finds `socket_` assigned once at `udp_bridge.cpp:414` and never closed — no `close()` anywhere in the package, no `~UDPBridge` in `udp_bridge.h`, and `on_cleanup` (`:780-809`) touches the queues, timers, updater, parameter handle and the two maps but not `socket_`. The only `setsockopt` calls are `SO_RCVTIMEO`/`SO_RCVBUF`/`SO_SNDBUF`; no `SO_REUSEADDR`. So the re-configure re-binds the same fixed port and `exit(1)`s at `:430`, before the duplicate `RemoteNode` at `:517` exists; the doubled-uplink path needs `port: 0`, which no config uses. `launch/udp_bridge_launch.py:18-19` confirms `respawn=True, respawn_delay=2`. All four sites now give that mechanism and say the exit is self-healing, and point at rolker/udp_bridge#66 — `src/udp_bridge.cpp:393-407`, `README.md:139-150`, `doc/relay_design.md:238-250`, `.agents/README.md:207-219`. `config/example_params.yaml` states only the advice and was left alone.
- [x] (suggestion) Report every offending key, not just the first — `7e94b36`. Applied rather than deferred to house style: must-fix 1 establishes that recovery from this failure costs a process restart, so first-offender-only converts N bad lines into N restarts, which `resolveRemoteIdentities` (edit-and-retry, no restart) does not. Loop accumulates `key = 'value'` pairs; one ERROR after it. The FAILURE itself is unchanged and already covered by `RetiredRemoteNameParameterFailsToConfigure`; added `SeveralRetiredRemoteNameParametersFailToConfigure` to pin the accumulate-then-fail shape (only lifecycle state is observable here — nothing in this package captures log content — so it pins the refusal, not the message text). Test count 253 → 254 — `src/udp_bridge.cpp:387-418`, `test/test_node_name_limits.cpp:187-203`, `README.md:96`
- [x] (suggestion) "setting it at all" narrowed to labels present in `remotes_list` — `eda47bd`. Both sites now say which labels are checked and that a block `remotes_list` does not name is inert anyway. Also rewrapped the paragraph the edit disturbed — `README.md:94-100,120-127`, `.agents/README.md:191-195`
- [x] (suggestion) "refusing to start" corrected — `9835ed5`. Verified: `main()` (`src/udp_bridge_node.cpp:81-97`) spins the executor regardless of transition outcome, and `diagnostic_updater_` is created at `udp_bridge.cpp:682`, after the `:397` failure — so no `/diagnostics` row, no `bridge_info`, no heartbeat; one stderr ERROR is the whole signal, recorded as the pre-existing symptom shared with the over-long-name failure — `.agents/README.md:203-210`
- [x] (suggestion) Stale "ahead of every resource this callback acquires" comment — `5442b05`. Narrowed to the explicit failure paths staying above the socket block, and named both exceptions: `on_set_parameters_handle_` at `:288`, and a mistyped YAML scalar throwing out of the connections/topics loops after the bind, which `rclcpp_lifecycle` swallows — `src/udp_bridge.cpp:340-352`

### Deferred
- [x] (suggestion) Close `socket_` in `on_cleanup` — **deferred to rolker/udp_bridge#66** ("Lifecycle: on_cleanup never closes the socket, so CLEANUP→CONFIGURE exit(1)s on EADDRINUSE", open). The review itself scoped it as pre-existing and the other half of the `remote_nodes_`/`subscribers_` pruning follow-up. Doing it here would widen a config-parameter removal into a lifecycle change: the close needs the `on_shutdown` half, an `socket_ = -1` invariant across every reader (`spin_once`, `dispatchResendRequest`, `Connection::send`), and a cleanup→configure round-trip test — and it would still leave the pruning half undone, which is the half-fix the instruction warns against. #66 asks for exactly this and is its home. The docs changed in `5e9a09a` now name #66 at each site, so the reader of the caveat is pointed at the fix.

### Verification
Run from `core_ws` after `source /opt/ros/jazzy/setup.bash`, exit codes read directly (no grep gating):

    ./build.sh udp_bridge   → BUILD_EXIT=0
    ./test.sh udp_bridge    → TEST_EXIT=0
    colcon test-result --verbose → RESULT_EXIT=0
    Summary: 254 tests, 0 errors, 0 failures, 14 skipped

254 = the 253 floor plus `SeveralRetiredRemoteNameParametersFailToConfigure`. No test weakened or skipped. Build warnings are the pre-existing `-Wpedantic` flexible-array-member set in `packet.h` plus the two long-standing ones at `udp_bridge.cpp:2175`/`:2275`; none in the changed region.

`pre-commit run --from-ref origin/jazzy --to-ref HEAD` → exit 0 (all hooks Passed or Skipped-no-files).

Not pushed, no PR — per the sub-agent handoff contract.

---
**Authored-By**: `Claude Code Agent`
**Model**: `Claude Opus`

## Integrated Review
**Status**: complete
**When**: 2026-08-24 14:52 -04:00
**By**: Claude Code Agent (Claude Opus 5 (1M context))

**PR**: #68 at `8208d84`
**Sources**: 2 (Copilot @ `8208d84`; Local Review (Pre-Push) x2 @ `11bc263`/`3f1ad40`)
**Cross-source confirmations**: 1
**CI**: all-pass (build-and-test success; copilot-pull-request-reviewer success)

Copilot verdict: "Needs a closer look" — a request for human review of lifecycle
and identity semantics, not an objection. 1 inline comment, accurate.

### Findings
- [ ] (cross-confirmed by root cause: Copilot @ `8208d84` + Local Review @ `11bc263`) The configure loop iterates the raw `remotes_list` rather than the deduplicated `remote_identities` returned by `resolveRemoteIdentities`, so a repeated entry constructs and `update()`s the same `RemoteNode` twice — two DDS publishers created then discarded, plus duplicate `getaddrinfo` — before the second assignment collapses the state. Final state is correct; the cost is redundant DDS entity churn during `on_configure` and an idempotence claim that holds for state but not for work. Fix: iterate `remote_identities`. Consider also strengthening `RepeatedLabelIsIdempotentNotACollision` to assert the work is not repeated, not only the resulting state — `src/udp_bridge.cpp:538`

### Notes
- Not a same-head-SHA confirmation (the two sightings are at different heads), but the same root cause found twice by independent sources. Worth recording how it survived: round 1 raised it as "the counter counts occurrences, not keys", and the fix pass resolved that by deleting the counter entirely — removing the symptom while the un-deduplicated loop remained. Copilot then found the loop's other consequence. A reminder that resolving a finding by deleting its symptom can leave the cause in place.
- Severity low: no incorrect end state, no field-visible behaviour change. A repeated `remotes_list` entry is itself a config error the package tolerates deliberately.
- Governance: no new ROS parameter this round, so `.agents/README.md`'s verified-parameter table needs no row.

### False positives
- None. The single Copilot comment was checked against source and holds.
