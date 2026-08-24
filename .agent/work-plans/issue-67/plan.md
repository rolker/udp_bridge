# Plan: Remove the per-remote name parameter

## Issue

https://github.com/rolker/udp_bridge/issues/67

## Context

#63 added `resolveRemoteIdentity()`: a remote's identity (the key for
`remote_nodes_` and every topic's `remote_details`) is `remotes.<label>.name`
when set, else the label. Per the issue and its ROS-1-verified correction
comment, the operator has decided the split was never wanted — a label is
always the wire name. This reverses the indirection while keeping #63's
validation (23-char limit, self-name, empty), retargeted at labels. Full
scope analysis already committed in `.agent/work-plans/issue-67/progress.md`
(`## Issue Review`, `32e9e39`) — this plan implements its five findings,
not re-derives them.

## Approach

1. **`include/udp_bridge/remote_identity.h`** — delete `resolveRemoteIdentity()`
   (14–44). Change `resolveRemoteIdentities()` (46–153) to take
   `const std::vector<std::string>& labels` and return
   `std::vector<std::string>` (valid labels — no map, since label *is* now
   the identity). Keep the empty/over-long/self-name checks and the
   idempotent-repeat skip verbatim, testing `label` in place of `identity`.
   **Delete** the `label_by_identity` map and its collision branch (140–148)
   — Issue Review finding 1: two distinct `remotes_list` entries can no
   longer share an identity, so this is unreachable, not retargetable.
   Rewrite the header comment (14–39) with the ROS 1 lineage from the
   issue's correction comment (nested `remotes` dict, `name:` as an
   optional override, ROS 2's flat model manufacturing the label) and the
   new constraint that a label is now both the wire identity (≤23 bytes)
   and a parameter-path segment (no `.`) — findings 2 and 4.
2. **`src/udp_bridge.cpp` on_configure (347–373)** — keep
   `declareIfMissing("remotes." + label + ".name", std::string())` per
   label (finding 2 — needed only so rclcpp surfaces an override at all);
   if non-empty, `RCLCPP_ERROR_STREAM` naming the parameter, the entry and
   the value, and return `CallbackReturn::FAILURE` ahead of the socket
   bind. (Revised per the operator decision of 2026-08-24 — see divergence
   6; the plan as written specified a WARN and a successful configure.)
   Call
   `resolveRemoteIdentities(remotes_list, name_, &identity_error)`; same
   `FAILURE`-on-error handling.
3. **~476–486** — `configured_remote_names_` becomes
   `std::set<std::string>(labels.begin(), labels.end())` (no translation
   needed). In the remote-construction loop, `remote_info.name = remote_name`
   directly; delete the now-always-false `if(remote_info.name !=
   remote_name)` INFO log (484–486).
4. **Unknown-sender WARN, ~1789–1796** — advice changes from "set
   `remotes.<label>.name` to `'<source_node>'`" to "rename the `remotes_list`
   label to `'<source_node>'`".
5. **`test/test_relay_routing.cpp`**:
   - `ConfiguredNameIsTheIdentityNotTheLabel` (176–190) → `LabelIsTheIdentity`:
     assert `resolveRemoteIdentities({"robot_a","operator_1"}, "", &error)`
     returns both labels unchanged.
   - Delete `DuplicateRemoteIdentityIsRejected` (195–203) — tests the dead
     branch from step 1; no replacement needed (a `vector<string>` can't
     collide except via exact repeat, already covered by
     `RepeatedLabelIsIdempotentNotACollision`).
   - `EmptyRemoteIdentityIsRejected` (218–235): drop the "empty label, real
     name" half (228–235) — no longer expressible.
   - `RemoteResolvingToOurOwnNameIsRejected` (237–262),
     `RepeatedLabelIsIdempotentNotACollision` (269–287),
     `OverLongRemoteIdentityIsRejected` / `MaximumLengthIdentityStillClosesTheLoopRule`
     (330–393): drop the `.name` half of each pair, pass plain label vectors.
   - Delete `LabelDifferingFromWireNameStillNeverEchoes` (294–~325) —
     unconfigurable now; echo coverage survives via
     `EchoIsNeverSentBackToTheSender` (148).
6. **`test/test_node_name_limits.cpp`**:
   - Keep `Bridge::remote(label, wire_name)` (74–79) — still declares
     `remotes.<label>.name`, needed for the new stale-key test.
   - Delete `OverLongRemoteNameFailsToConfigure` (112–117, covered by
     `OverLongRemoteLabelFailsToConfigure` 121–125) and
     `RemoteWithOurOwnNameFailsToConfigure` (143–149, covered by
     `RemoteLabelMatchingOurOwnNameFailsToConfigure` 152–156).
   - Fix `MaximumLengthNamesConfigure` (129–135): boundary must move to the
     label — `.remote(kOtherAtLimit, "")` instead of
     `.remote("robot_a", kOtherAtLimit)`.
   - Add `RetiredRemoteNameParameterFailsToConfigure`: `.remote("robot_a",
     "some_other_name").configure()` returns `kUnconfigured`, and
     `RetiredRemoteNameMatchingItsLabelStillFailsToConfigure` for the
     no-carve-out case. (Revised per the operator decision of 2026-08-24 —
     see divergence 6. The plan specified `StaleRemoteNameParameterWarns`
     asserting `kInactive`.)

     **Do not assert on log text.** No test in this package captures log
     content, and the one logging-adjacent test does the opposite — it
     *silences* the logger and asserts through an accessor instead
     (`test_remote_node_resend.cpp:838-852`). Under the plan's WARN that
     forced a `staleRemoteNameKeyCountForTest()` accessor, since a warning
     is not otherwise observable. A FAILURE **is** directly observable in
     the state the node lands in, so no accessor is needed and none is
     added — see divergence 7.
7. **Docs** (Issue Review confirmed all four are in-scope, deeper than a
   one-line fix):
   - `doc/relay_design.md` — rewrite the identity-namespace section
     (~158–200: drop the `remotes.<label>.name` fallback description, fold
     in ROS 1 lineage) and its upgrade note (~188–192: flips to "a live key
     is REJECTED — `on_configure` fails"); fix the over-long-names table row
     (~216) to drop the `.name` alternative.
   - `.agents/README.md` — rewrite the pitfalls section (~183–208, "IS a
     real parameter as of #51" → declared-but-unread, FAILURE on non-empty)
     and the parameter table row (108).
   - `README.md` — rewrite the `remotes.<remote_label>.name` description
     (~88) and its #51 upgrade note (~97–120). The node-name-length upgrade
     note (~122–129) is **also affected**, contrary to an earlier draft of
     this plan: it states that an over-long `remotes.<label>.name` fails the
     configure transition, which stops being true once that key is no longer
     read for identity. Rewrite it so the length rejection is described where
     it still applies — the node's own `name`, the `remotes_list` label, and
     the `add_remote` service — and so a `.name` key is described as refused
     on *presence*, whatever its length.
   - `config/example_params.yaml` — remove the commented `# name: "robot_a"`
     example (~63) and the "identity on the wire is `remotes.<label>.name`"
     prose (~34); generalize the existing operator_1 note (~158) into the
     only pattern shown; add one explicit line: *your label for a peer must
     be the name that peer calls itself.*

## Files to Change

| File | Change |
|------|--------|
| `include/udp_bridge/remote_identity.h` | Delete resolver fn; label-only signature; drop dead collision map; rewrite header comment |
| `src/udp_bridge.cpp` | Keep declaring `.name` for detection only; stop using it for identity; FAIL `on_configure` when non-empty; drop dead INFO branch; fix unknown-sender WARN advice |
| `test/test_relay_routing.cpp` | 1 renamed+inverted test, 2 deleted, 3 trimmed to label-only args |
| `test/test_node_name_limits.cpp` | 2 deleted, 1 fixed boundary, 4 new (retired-key rejection ×2, absent-key negative, empty-entry lifecycle) |
| `doc/relay_design.md`, `.agents/README.md`, `README.md`, `config/example_params.yaml` | Rewrite two-namespace language per step 7 |

## Principles Self-Check

| Principle | Consideration |
|---|---|
| Only what's needed | Dead collision-map deletion (step 1) shrinks the diff below a naive rename |
| Test what breaks | Every deletion's replacement coverage stated inline (steps 5–6); new tests for the rejection this issue adds, both the differing and the equal-to-label case |
| Capture decisions | ROS 1 lineage moves from the GitHub thread into `remote_identity.h`'s durable header comment |
| A change includes its consequences | All four Issue-Review-confirmed doc targets get concrete edits (step 7) |

## ADR Compliance

| ADR | Triggered | How addressed |
|---|---|---|
| 0008 — ROS 2 conventions | Yes | Normal declare/read parameter removal, no deviation |
| 0001 — Adopt ADRs | Watch, not required | No project ADR log in `udp_bridge`; decision recorded in-code (step 1) per Issue Review |

## Consequences

| If we change... | Also update... | Included? |
|---|---|---|
| `resolveRemoteIdentities()` signature | Both call sites + all test call sites | Yes — steps 2, 5, 6 |
| Unknown-sender WARN text | `doc/relay_design.md`'s reference to it (~168) | Yes — step 7 |
| `identity_by_label` map → label vector | `configured_remote_names_` + remote-construction loop | Yes — step 3 |
| #64's bench plan (mismatch scenario) | Simplify when #64 resumes | No — out of scope per issue ask item 6, noted only |

## Documentation & Instruction Impact

- **Stale docs** (this PR): `doc/relay_design.md`, `.agents/README.md`,
  `README.md`, `config/example_params.yaml` — all four scoped in step 7.
- **Agent-instruction candidates**: None — the durable rationale belongs in
  `remote_identity.h`'s header comment (file-local design history), not a
  cross-cutting instruction file.

## Open Questions

- None — the Issue Review's four action items are resolved: dead-code
  deletion (step 1), retired-key rejection mechanism (step 2), label-as-parameter-
  path-segment constraint documented (steps 1, 7), unknown-sender WARN fixed
  (step 4).

## Implementation Divergences (kept in sync during implementation)

The plan was followed as written except for the operator decision recorded
in divergence 6, which changed the behaviour it specified. Seven things it
did not name were needed and are in the commits:

1. **`resolveRemoteIdentities` collapses repeats in its return value.** The
   plan said "return `std::vector<std::string>` (valid labels)" without
   saying what a repeated entry yields. It returns first-seen order with
   repeats collapsed — the same shape the old `identity_by_label` map had —
   so `configured_remote_names_` and the remote-construction loop are
   unchanged in behaviour. `RepeatedLabelIsIdempotentNotACollision` now
   asserts the collapse explicitly.
2. **Two stale cross-references to the deleted collision check** had to be
   fixed outside the plan's file list: `include/udp_bridge/packet.h` (~56)
   and `test/test_node_name_limits.cpp`'s header comment both said
   truncation would reopen "the collision `resolveRemoteIdentities` exists
   to reject". Both now state the hazard without naming a check that is
   gone. `.agents/README.md` and `doc/relay_design.md` carried the same
   sentence and are fixed in the doc commit.
3. **A second new test, `AbsentRemoteNameParameterIsSilent`.** The plan
   asked only for `StaleRemoteNameParameterWarns`. Asserting the counter is
   1 when the key is set proves nothing unless something also pins that it
   is 0 when the key is absent — otherwise an unconditional increment
   passes. The negative case is what makes the accessor a real assertion.
   *Kept under divergence 6, renamed
   `AbsentRemoteNameParameterConfiguresCleanly` and now asserting the
   transition instead of the (removed) counter — the same argument holds: a
   rejection test proves nothing unless something pins that an absent key
   still configures.*
4. **The stale-key WARN counter is reset at the top of `on_configure`**,
   not merely incremented, so a re-configure reports the current config
   rather than an accumulating total. *Superseded by divergence 7 — the
   counter no longer exists.*
5. **`README.md`'s empty/self-name paragraph (~164)** also described the
   rejection in terms of "an empty `remotes_list` entry with no `name`
   set". The plan named the `remotes.<remote_label>.name` description, the
   #51 upgrade note and the node-name-length note; this fourth passage
   needed the same retargeting.
6. **The retired key is REJECTED, not warned about — operator decision,
   2026-08-24.** The plan (and the issue's ask item 3) specified a WARN and
   a successful configure. The pre-push review recorded the consequence as
   a downgraded finding: warn-only leaves exactly one config shape — a
   stale value *differing* from its label — announced and then ignored, and
   that shape is the #51 echo bug, while every other identity error fails
   the transition. The operator's ruling: *"fail if any name is present
   since it's removed and not supported."* So a non-empty
   `remotes.<label>.name` returns `CallbackReturn::FAILURE`, with **no
   carve-out** for a value equal to its label — the parameter is removed, so
   presence is the error. The parameter is still declared (unchanged: the
   only way rclcpp surfaces the override), and the FAILURE returns ahead of
   the socket bind alongside the other identity failures, preserving #63
   round 3's no-socket-leak property. An explicitly empty value is
   indistinguishable from an absent key and is accepted; the docs say so.
   This also made `test/bench/configs/three_path.yaml`'s retired key
   load-bearing rather than cosmetic — under hard failure the #18 bench
   harness stops configuring, and `test_smoke.py` fails until it is removed.
7. **`staleRemoteNameKeyCountForTest()` and its counter were removed, not
   updated.** They existed only because a WARN is not observable in a
   package that captures no log content anywhere. A FAILURE is directly
   observable in the state the node lands in, so the accessor was
   unnecessary complexity and the tests assert on the transition result
   instead. Removing it also obviated three review suggestions about the
   counter (its "written before any worker thread exists" rationale being
   false on a re-configure; counting occurrences rather than keys on a
   repeated entry; not being reset in `on_cleanup`). The negative test —
   an absent key configures cleanly — is kept, as
   `AbsentRemoteNameParameterConfiguresCleanly`.

## Estimated Scope

Single PR — one coherent reversal (2 source files, 2 test files, 4 docs);
splitting would leave an inconsistent intermediate state per the Issue
Review's scope verdict.
