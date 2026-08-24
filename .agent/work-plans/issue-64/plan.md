# Plan: Bench: three-bridge hub scenario for relay

## Issue

https://github.com/rolker/udp_bridge/issues/64

## Context

`relayToOtherRemotes()` (#51/#63) is unit-tested but has no live multi-node
bench coverage of the boat→hub→{op1,op2} shape it exists for. Per the Issue
Review (`.agent/work-plans/issue-64/progress.md`), the new scenario must
avoid generalizing the two-party veth/netns machinery in
`udp_bridge/test/bench/run_scenario.py` (`setup_topology()`:303,
`_ns_prefix()`:206, `_BOAT_NS_PID`:199 are hard-coded to one boat namespace)
and instead run three plain bridges over loopback in the single namespace
`main()`'s `unshare -Urn` re-exec already provides — no `tc netem`, no veth.

## Approach

1. **New config** `udp_bridge/test/bench/configs/relay_hub.yaml` — three
   bridge blocks, all on `127.0.0.1`:
   - `bridge_a` (`name: "a"`, port 5401): `remotes_list: ["hub"]`;
     `remotes.hub` (`name: "hub"`, port 5402) lists two topics on one
     connection: outbound `/a/heartbeat` → dest `/hub/from_a/heartbeat`
     (the message A sends), and inbound `/hub/from_a/heartbeat` → dest
     `/a/echo_from_hub` — A also *asks for* the topic it publishes, so a
     broken sender-exclusion in `relayToOtherRemotes()` (the loop rule) has
     somewhere to deliver to and the echo-guard assertion (below) can
     actually fail.
   - `bridge_hub` (`name: "hub"`, port 5402): `remotes_list: ["a", "b"]`.
     `remotes.a` (`name: "a"` — matches label) carries A's two topics above.
     `remotes.b` (`name: "boat_b"` — **deliberately differs from the label
     `"b"`**, port 5403) lists `/hub/from_a/heartbeat` → dest
     `/b/from_a/heartbeat`. This is #63's exact shape: the mismatch lives on
     the hub's entry for the remote whose wire identity differs from the
     hub's own label for it, so a regression of `on_configure` keying
     `remote_nodes_` by label instead of `remotes.<label>.name` (source
     `udp_bridge/src/udp_bridge.cpp`:504) breaks B's delivery specifically,
     not the scenario's stand-up.
   - `bridge_b` (`name: "boat_b"`, port 5403): `remotes_list: ["hub"]`;
     `remotes.hub` (`name: "hub"`, port 5402) lists `/hub/from_a/heartbeat`
     as an inbound topic → dest `/b/from_a/heartbeat`.
   - Three distinct destination topics total (`/hub/from_a/heartbeat` =
     hub-local, `/b/from_a/heartbeat` = B, `/a/echo_from_hub` = A's
     echo-guard), per the host decision — no shared topic + stderr
     disambiguation.

2. **New scenario function** `run_relay_hub(duration_s)` in
   `udp_bridge/test/bench/run_scenario.py`, placed after `run_full_mix()`
   (~line 767). Domains `A_DOMAIN=51`, `HUB_DOMAIN=52`, `B_DOMAIN=53`
   (distinct from `OPERATOR_DOMAIN`/`BOAT_DOMAIN`, avoids any cross-scenario
   ambiguity in logs). No `setup_topology()`/`netns=` args — `launch_bridge`
   (`:336`), `launch_pub`/`launch_sub` (`:366`/`:380`) already accept
   `netns=None` and run in the orchestrator's own (already-unshared)
   namespace. Sequence: launch all three bridges (`launch_bridge` raises via
   `check=True` in `_run` if any lifecycle `configure`/`activate` transition
   fails — this is the "did three bridges really come up" guard: a silently
   degraded 2-bridge run aborts loudly here, before any counting), launch
   three `sub.py` subscribers (hub-local, B, A's echo-guard) on their
   respective domains/topics, then one `pub.py --tier critical` on A's
   domain targeting `/a/heartbeat`. Wait/parse counts via `_parse_count`
   (`:687`), matching `run_smoke`'s wait pattern.

3. **Assertions** (in `main()`'s new `elif args.scenario == 'relay_hub':`
   branch, alongside the other scenarios, ~line 934):
   - `hub_local` and `b` sub-counts: `0.8 * pub_count <= sub_count <= 1.2 *
     pub_count`. **Fails on**: 0 delivery (relay/local-publish broken), or
     ~2x delivery (a duplicate-delivery regression — the exact vacuous-pass
     shape #57 warned about; a bare `> 0` check cannot distinguish 1x
     from 2x).
   - `a_echo` sub-count: `== 0` exactly. **Fails on**: any regression in the
     sender-exclusion (loop) rule in `relayToOtherRemotes()` that relays a
     message back to its own sender — the only realistic way this line
     could receive traffic given the config in step 1.
   - `pub_count > 0` (A). **Fails on**: A's publisher never starting —
     without this, a total stand-up failure could read as "everything
     downstream is correctly zero."
   - Print `BENCH_RESULT_{HUB_LOCAL,B,A_ECHO}_SUB_COUNT=` and
     `BENCH_RESULT_A_PUB_COUNT=`, matching the `BENCH_RESULT_*` convention.

4. **New pytest** `udp_bridge/test/bench/test_relay_hub.py`, modeled on
   `test_smoke.py` (same `_userns_available()`/`_ros2_available()` skip
   markers — **no** `UDP_BRIDGE_BENCH_SCENARIOS=1` gate, matching the
   `smoke` case: this scenario has no netem and completes in seconds, so it
   runs by default and is skipped only when `unshare -Urn` itself is
   unavailable). Runs `run_scenario.py --scenario relay_hub`, parses the
   four `BENCH_RESULT_*` lines, re-asserts the four conditions from step 3
   (defense in depth — the orchestrator's own exit code already encodes
   them) with the same tolerance-band logic.

5. **CI capability check (must state, not assume):**
   `.github/workflows/ci.yml` runs `container: image: ros:jazzy-ros-core`
   on a `ubuntu-24.04` GitHub-hosted runner with no `--privileged` and no
   sysctl override step. `test/bench/README.md`'s own Prerequisites section
   states Ubuntu 24.04+ defaults `apparmor_restrict_unprivileged_userns=1`,
   which blocks `unshare -Urn` for unprivileged users, and that the existing
   `smoke` test already relies on `_userns_available()` to skip cleanly for
   exactly this reason. Nothing in the CI workflow resets that sysctl or
   runs the container privileged, so **`relay_hub` will almost certainly
   skip in CI, the same as `smoke` already does** — this is a known,
   pre-existing gap (not introduced by this change), not something this PR
   can close without adding `--privileged`/host sysctl control to CI, which
   is out of scope here.

## Files to Change

| File | Change |
|---|---|
| `udp_bridge/test/bench/configs/relay_hub.yaml` | New — three-bridge hub config (step 1) |
| `udp_bridge/test/bench/run_scenario.py` | Add `run_relay_hub()`, `'relay_hub'` CLI choice, `main()` branch |
| `udp_bridge/test/bench/test_relay_hub.py` | New — pytest wrapper, no opt-in gate |
| `udp_bridge/test/bench/README.md` | New scenario-table row; CI section note that `relay_hub` skips under the same `unshare` restriction as `smoke` |

## Principles Self-Check

| Principle | Consideration |
|---|---|
| Test what breaks | Every assertion above states its failure mode; none is a bare presence check. |
| Only what's needed | Topology reuses zero shared veth/netns code — additive only. |
| A change includes its consequences | README table + CI section updated in this PR. |

## ADR Compliance

| ADR | Triggered | How addressed |
|---|---|---|
| 0008 — ROS 2 conventions | Mild | New pytest matches `test_<scenario>.py` naming and existing skip-marker structure. |

## Consequences

| If we change... | Also update... | Included? |
|---|---|---|
| Add a bench scenario | `test/bench/README.md` scenario table + CI section | Yes |
| Add new bridge names/domains | Nothing else references `A_DOMAIN`/`HUB_DOMAIN`/`B_DOMAIN` | N/A |

## Documentation & Instruction Impact

- **Stale docs**: `test/bench/README.md`'s scenario table and CI section —
  lands in this PR (step 5 above).
- **Agent-instruction candidates**: None — the CI unshare-restriction gap is
  already documented in README's Prerequisites; no new pattern to capture.

## Open Questions

- None — both Issue Review open questions (destination-topic scheme, which
  side carries the label/name mismatch) are resolved in Approach steps 1–2.

## Estimated Scope

Single PR.
