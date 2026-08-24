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
     `remotes.hub` (`name: "hub"`, port 5402) lists exactly **one** topic:
     outbound `/a/heartbeat` → dest `/hub/from_a/heartbeat` (the message A
     sends). A's own config carries no inbound entry — per
     `udp_bridge.cpp`'s push model (`source`/`destination` params,
     ~583-592: a bridge locally subscribes to `source` and forwards to
     `destination` on that remote), a config entry under A's own block
     would subscribe A to a topic A never locally publishes, so it can
     never fire. It would be a decorative, dead entry, not a working echo
     guard.
   - `bridge_hub` (`name: "hub"`, port 5402): `remotes_list: ["a", "b"]`.
     `remotes.a` (`name: "a"` — matches label) carries **one** topic: the
     echo-guard entry, source `/hub/from_a/heartbeat` → dest
     `/a/echo_from_hub`. This is the **only** place the echo-guard topic is
     configured — the rule stated once, unambiguously: hub locally
     subscribes to `/hub/from_a/heartbeat` (published locally, unconditionally,
     the moment hub receives A's relayed data — `decodeData` →
     `enqueuePublish` fires regardless of any further forwarding config) and
     is configured to also forward it back to remote `"a"`. Normally
     `relayToOtherRemotes()`'s loop rule (`udp_bridge.cpp`:1213,
     `selectRateLimitedConnections(..., item.source_node)`) excludes the
     sender from `subscribers_[topic].remote_details` before sending, so
     this entry should never actually deliver to A even though it's
     configured — that's what makes `a_echo == 0` a real assertion of the
     exclusion logic, not a vacuous one: a regression that fails to exclude
     `item.source_node` is the only realistic way traffic reaches this
     destination. Hub's own local copy needs no separate config entry — the
     `hub_local` subscriber (step 2) listens directly on
     `/hub/from_a/heartbeat` on hub's domain, the unconditional local
     publish described above.
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
     hub's own local copy, `/b/from_a/heartbeat` = B, `/a/echo_from_hub` =
     A's echo-guard), per the host decision — no shared topic + stderr
     disambiguation.

2. **New scenario function** `run_relay_hub(duration_s)` in
   `udp_bridge/test/bench/run_scenario.py`, placed after `run_full_mix()`
   (~line 767). Domains `A_DOMAIN=51`, `HUB_DOMAIN=52`, `B_DOMAIN=53`
   (distinct from `OPERATOR_DOMAIN`/`BOAT_DOMAIN`, avoids any cross-scenario
   ambiguity in logs). No `setup_topology()`/`netns=` args — `launch_bridge`
   (`:336`), `launch_pub`/`launch_sub` (`:366`/`:380`) already accept
   `netns=None` and run in the orchestrator's own (already-unshared)
   namespace.

   **Publish volume**: `pub.py`'s critical tier defaults to 1 Hz
   (`TIER_DEFAULTS['critical']`); at `duration_s=10` that's ~10 messages,
   where one anomalous message is 10% of the sample — too coarse to tell
   clean delivery from a single drop or duplicate. `launch_pub` gains an
   optional `rate_hz: float | None = None` param that, when set, passes
   `--rate-hz` through to `pub.py` (already supports the override; see
   `TIER_DEFAULTS`/`--rate-hz` in `pub.py`). `run_relay_hub` calls it with
   `rate_hz=10.0` at the same `duration_s=10` default, giving ~100
   messages — one anomalous message is now ~1%, and total scenario runtime
   is unchanged (still ~10s, since this scenario runs on every build).

   **Readiness gate, not fixed sleeps**: `run_smoke`'s pattern (2s
   bridge-handshake sleep + 0.5s discovery sleep before publishing) is
   exactly what `range_degradation`/`subscriber_death` replaced with
   `wait_ready.py` after #57 — a fixed sleep can under-wait and silently
   measure a broken startup as if it were clean data. A three-hop hub
   topology has one more discovery leg than the two-party pattern, so
   reusing the fixed-sleep numbers unchanged is exactly the assumption
   `wait_ready.py` exists to avoid. `wait_ready.py`'s `ReadinessGate`
   already reads any `(topic, connection_id, remote_name)` triple's
   `DataRates` off `BridgeInfo` — it only reads `message.success_bytes_per_second`,
   which stays at 0 until the counted publisher starts (circular for a
   pre-publish gate). Give it a `--field {message,overhead}` arg (default
   `message`, so `range_degradation` is untouched) so `relay_hub` can gate
   on `overhead.success_bytes_per_second` instead — the CONNECT/subscribe-
   handshake traffic each bridge exchanges with its peers at activation,
   independent of any topic data (`connection.cpp`'s `is_overhead` send
   path, `udp_bridge.cpp`:2331/2528 `PacketSendCategory::overhead`). Launch
   the three bridges, then two `wait_ready.py` calls with a short timeout
   (e.g. 5s — this scenario has no netem and should be near-instant once
   up) and a low `--min-bps` tuned during implementation to sit clearly
   above idle noise: one for bridge_a's connection to `"hub"`, one for
   bridge_hub's connection to `"boat_b"`. Both must report ready — timeout
   raises `RuntimeError` (per `wait_ready()`'s existing contract), aborting
   loudly rather than launching the publisher into an unconfirmed graph.
   Note `run_scenario.py`'s existing `wait_ready()` wrapper forwards only
   `--timeout-s`; it needs the topic / connection-id / remote-name /
   `--min-bps` / `--field` overrides plumbed through for these two calls.
   Only then launch the three `sub.py` subscribers (hub-local on
   `/hub/from_a/heartbeat`, B on `/b/from_a/heartbeat`, A's echo-guard on
   `/a/echo_from_hub`) and `pub.py --tier critical --rate-hz 10.0` on A's
   domain targeting `/a/heartbeat`. Parse counts via `_parse_count` (`:687`).

3. **Assertions** (in `main()`'s new `elif args.scenario == 'relay_hub':`
   branch, alongside the other scenarios, ~line 934). `pub_count` is
   `_parse_count(pub)` — the publisher's own tally, never rate×duration
   arithmetic, so a publisher that underruns (e.g. scheduling jitter costs
   it a cycle) tightens the bar on itself instead of silently loosening it:
   - `hub_local` and `b` sub-counts: `sub_count == pub_count`, exactly.
     **Fails on**: any drop, any duplicate. An earlier revision allowed
     `abs(sub_count - pub_count) <= 1` "for the message in flight at
     shutdown" — but one message of slack is exactly the size of the bug
     this scenario exists to catch, so a single drop or a single duplicate
     passed it. The slack is also unnecessary: the subscriber outlives the
     publisher by 2 s (step 3) on loopback with sub-millisecond delivery,
     so there is no legitimate source of even a one-message gap. If a run
     ever fails by exactly one, that is a finding to investigate, not
     tolerance to widen — record the observation before touching this
     line.
   - `a_echo` sub-count: `== 0` exactly. **Fails on**: any regression in the
     sender-exclusion (loop) rule in `relayToOtherRemotes()` that relays a
     message back to its own sender — the only realistic way this line
     could receive traffic given the config in step 1.
   - `pub_count > 0` (A). **Fails on**: A's publisher never starting —
     without this, a total stand-up failure could read as "everything
     downstream is correctly zero."
   - Print `BENCH_RESULT_{HUB_LOCAL,B,A_ECHO}_SUB_COUNT=` and
     `BENCH_RESULT_A_PUB_COUNT=`, matching the `BENCH_RESULT_*` convention.

   **Correcting the stand-up rationale**: `launch_bridge`'s `check=True`
   (`_run`, raised if any lifecycle `configure`/`activate` transition
   fails) only proves a bridge reached ACTIVE on its own — it says nothing
   about whether it ever exchanged a packet with a peer. The actual guard
   against a silently-degraded run (e.g. two bridges up, the third
   isolated) is the delivery-count assertions above: an unreachable B or a
   dead hub relay drives `b`'s or `hub_local`'s count to 0, which the
   equality check rejects. `check=True` and the count assertions are
   complementary, not redundant — a future edit must not drop the count
   assertions on the assumption `check=True` already covers three-bridge
   connectivity.

4. **New pytest** `udp_bridge/test/bench/test_relay_hub.py`, modeled on
   `test_smoke.py` (same `_userns_available()`/`_ros2_available()` skip
   markers — **no** `UDP_BRIDGE_BENCH_SCENARIOS=1` gate, matching the
   `smoke` case: this scenario has no netem and completes in seconds, so it
   runs by default and is skipped only when `unshare -Urn` itself is
   unavailable). Runs `run_scenario.py --scenario relay_hub`, parses the
   four `BENCH_RESULT_*` lines, re-asserts the four conditions from step 3
   (defense in depth — the orchestrator's own exit code already encodes
   them) with the same `== pub_count` / `== 0` / `> 0` logic.

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
| `udp_bridge/test/bench/run_scenario.py` | Add `run_relay_hub()`, `'relay_hub'` CLI choice, `main()` branch; `launch_pub()` gains optional `rate_hz` param |
| `udp_bridge/test/bench/wait_ready.py` | Add `--field {message,overhead}` arg (default `message`, `range_degradation` unaffected) |
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
