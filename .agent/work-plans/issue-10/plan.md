# Plan: Bridge wedges (reader thread blocked, Recv-Q backup) when remote subscriber dies

## Issue

https://github.com/rolker/udp_bridge/issues/10

## Context

The receive path runs entirely in `socket_drain_group_` (MutuallyExclusive):
`spin_once` (10 ms timer) → `recvfrom` loop → `decode` → `decodeData` →
`publisher->publish()` at `src/udp_bridge.cpp:776`. The destination publisher
is created **RELIABLE** + `KEEP_LAST(1)` (`qos_resolution.h` — a deliberate
`rmw_zenoh_cpp` 0.2.9 graph-visibility workaround; design intent is
`BEST_AVAILABLE`). When a local subscriber (e.g. CAMP) dies *uncleanly*, DDS
keeps it matched until liveliness times out, and a RELIABLE `publish()` blocks
waiting for a history slot. Because that publish is on the **MutuallyExclusive**
drain group, the blocked call stops `spin_once` from re-running → `recvfrom`
stalls → kernel `Recv-Q` backs up (361 KB observed) → forwarded topics go
silent. Four occurrences on 2026-04-27; one masked an RTK alert by freezing
`/diagnostics` stale on the operator side.

`qos_design.md` (lines 36-39, 89-96, 305-309) already documents this mechanism
and rules out the QoS knobs as the fix: `BEST_EFFORT` would break CAMP's
RELIABLE subscribers (operator goes dark), `BEST_AVAILABLE` is blocked by the
rmw bug, and even BEST_AVAILABLE behaves RELIABLE for the local hop when a
RELIABLE subscriber matches — so it can still block. The fix must be
**transport-level, independent of QoS and rmw**: the drain thread must never
make a call that can block on a single subscriber.

## Approach

Decouple the destination publish from the socket-drain thread via a dedicated
**publish-worker thread** fed by a bounded queue. `decodeData` enqueues
`(publisher, serialized_message)` and returns immediately; the worker pops and
calls `publish()`. A stalled publish then blocks only the worker — `spin_once`
keeps draining the socket. This realizes the package's documented
"best-effort with loss reduction" model on the local hop: when the local
publish path stalls, we drop republished messages rather than backing up the
wire reader.

1. **`PublishQueue` component** (`include/udp_bridge/publish_queue.h`,
   header-mostly so it unit-tests without a node) — bounded MPSC queue of
   `{GenericPublisher::SharedPtr, SerializedMessage}`. `push()` never blocks;
   on overflow it drops the **oldest** item and increments a dropped counter.
   One worker thread loops pop→`publish()`. `stop()` joins cleanly.
2. **Wire into `UDPBridge`** — own a `PublishQueue` member; start the worker in
   `on_activate`, drain+`stop()` in `on_deactivate`/`on_cleanup` (lifecycle-safe,
   matches existing timer/handle reset discipline). Replace the inline
   `publisher->publish(serialized_message)` at `udp_bridge.cpp:776` with
   `publish_queue_.push(publisher, std::move(serialized_message))`.
3. **Bound + observability** — queue capacity as a ROS parameter
   (`publish_queue_max`, default sized for worst-case fragment bursts); expose
   the dropped-message count through the existing `diagnostic_updater_` so a
   stalled local subscriber surfaces as a WARN instead of silently.
4. **Unit test** (`test/test_publish_queue.cpp`) — the reader-thread
   non-stall property at the component level: `push()` returns without blocking
   even when no consumer drains; FIFO order; drop-oldest + counter on overflow;
   clean `stop()` join. Register in `CMakeLists.txt` via `add_udp_bridge_gtest`.
5. **End-to-end reproduction** — the kill-a-subscriber-mid-forward repro
   (acceptance #1) belongs in the issue #18 bench harness (PR #27 draft, which
   already targets the "wedge" failure mode). Coordinate rather than duplicate:
   this PR lands the component + unit test; cross-link #18 for the integration
   repro. (Open question below.)
6. **Docs** — add a "Receive-path publish decoupling" subsection to
   `qos_design.md` explaining why the wedge is fixed at the transport layer
   (not QoS) and that it holds regardless of the RELIABLE/BEST_AVAILABLE default.

## Files to Change

| File | Change |
|------|--------|
| `udp_bridge/include/udp_bridge/publish_queue.h` | New bounded publish-worker queue component |
| `udp_bridge/src/udp_bridge.cpp` | Enqueue instead of inline publish (line 776); start/stop worker in lifecycle hooks; param + diagnostic |
| `udp_bridge/include/udp_bridge/udp_bridge.h` | `PublishQueue` member + `publish_queue_max_` |
| `udp_bridge/test/test_publish_queue.cpp` | New unit test (non-stall, FIFO, drop-oldest, join) |
| `udp_bridge/CMakeLists.txt` | Register `test_publish_queue` |
| `udp_bridge/doc/qos_design.md` | Document transport-level decoupling as the #10 fix |

## Principles Self-Check

| Principle | Consideration |
|---|---|
| Test what breaks | Unit-test the non-stall/drop invariants — the exact failure seen 4× in the field — not framework glue |
| A change includes its consequences | `qos_design.md` updated; diagnostic surfaces drops; #18 coordination noted, not deferred silently |
| Only what's needed | One worker + bounded queue; watchdog (issue fix-shape #2) deferred — decoupling removes the blocking site, making Recv-Q polling redundant |
| Capture decisions | Rationale (why transport-level, not QoS) recorded in `qos_design.md`, not just the diff |
| Human control & transparency | Drop count is observable via diagnostics; queue bound is a tunable parameter |

## ADR Compliance

| ADR | Triggered | How addressed |
|---|---|---|
| ADR-0008 (ROS 2 conventions) | Yes | Worker thread is internal; lifecycle start/stop follows LifecycleNode activate/deactivate contract; QoS contract unchanged |
| ADR-0013 (progress.md vocabulary) | Yes | `progress.md` gets a `## Plan Authored` entry |

## Consequences

| If we change... | Also update... | Included? |
|---|---|---|
| publish path moves off drain thread | `qos_design.md` receive-path description | Yes |
| add `publish_queue_max` param | parameter docs / README if it lists params | Yes (verify during impl) |
| reader-thread non-stall property | issue #18 bench harness adds the e2e wedge repro | No — cross-linked follow-up in #18 |

## Decisions (resolved)

- **Cross-topic isolation scope → single worker** (user, 2026-05-25). One worker
  thread + bounded queue this PR; it fixes the reported reader-thread wedge (all
  four field occurrences). Per-publisher isolation (so a stalled subscriber on
  one topic can't head-of-line-block others) is deferred to a follow-up issue,
  to be filed when this PR lands. The bounded-queue drop counter + diagnostic
  make the head-of-line residual observable in the meantime.

## Open Questions

- **E2E repro home.** Default: the kill-subscriber integration repro lands in
  the #18 bench harness (PR #27), which already targets the "wedge" failure
  mode — not a standalone launch_test here. Flag if you'd rather it live in
  this repo's tests.

## Estimated Scope

Single PR for the component + unit test + wiring + docs. The end-to-end
reproduction (acceptance #1) is coordinated into issue #18's harness.
