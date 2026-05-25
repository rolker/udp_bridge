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

`publish()` is not the *only* rmw call on the drain path. `decodeData` also
runs, on the same thread: `create_generic_publisher()` (first-arrival, line
759 — the existing comment at 733-737 calls this "exactly the wedge mechanism")
and `sendBridgeInfo()` (first-arrival, line 773 — a graph query + two-mutex
snapshot + UDP send). Both can block on rmw discovery. So the decoupling must
move *all* of first-arrival create + bridge-info + publish off the drain
thread, not just the `publish()` call — otherwise the first message on any new
topic can still wedge the reader. The drain thread's only remaining
`decodeData` work is the (CPU-bound) `MessageInternal` deserialize + building
the inner `SerializedMessage`.

## Approach

Decouple the whole rmw-touching tail of `decodeData` from the socket-drain
thread via a dedicated **publish-worker thread** fed by a bounded queue. The
drain thread deserializes the `MessageInternal`, builds the inner
`SerializedMessage`, and enqueues a self-contained item; the worker does
lookup-or-create publisher + first-arrival bridge-info + `publish()`. A stalled
publish (or a blocking first-arrival create) then blocks only the worker —
`spin_once` keeps draining the socket. This realizes the package's documented
"best-effort with loss reduction" model on the local hop: when the local
publish path stalls, we drop republished messages rather than backing up the
wire reader.

1. **`PublishQueue` component** (`include/udp_bridge/publish_queue.h`,
   header-only). A bounded MPSC queue of `PublishItem`
   `{topic, datatype, reliability, durability, history_depth, SerializedMessage}`
   plus one worker thread. The worker is parameterized by an injected **sink**
   `std::function<void(PublishItem&&)>` — keeps all rmw/`UDPBridge` coupling out
   of the component so it unit-tests with a fake sink (a deliberately-blocking
   sink proves the non-stall property; a recording sink proves FIFO). `push()`
   never blocks; the bound is in **bytes** (`PublishItem::byte_size()`, since a
   reassembled image is large) and on overflow it drops the **oldest** item(s)
   and increments an atomic dropped counter. `start()` launches the worker,
   `stop()` signals + joins; `push()` after `stop()` is a no-op (drop), so it is
   safe to call from any thread at any lifecycle phase.
2. **Wire into `UDPBridge`** — own a `PublishQueue` member, **constructed in
   `on_configure`** (matching the package's "all resources in `on_configure`"
   discipline; `spin_timer_` et al. are created there). The injected sink is a
   member lambda that runs the existing three-phase lookup-or-create
   (`udp_bridge.cpp:738-769`), the first-arrival `sendBridgeInfo()`
   (773-774), and `publisher->publish()` (776) — i.e. that block moves verbatim
   from `decodeData` into the sink. `decodeData` keeps only the deserialize +
   inner-`SerializedMessage` build, then `publish_queue_.push(std::move(item))`.
   Start the worker in `on_configure` (idle until messages arrive, which only
   happens when ACTIVE per `spin_once`'s state guard); `stop()`+join in
   `on_cleanup` (and the destructor as a backstop). The join is what gates
   post-deactivation publishes — destination publishers are **non-lifecycle**
   `GenericPublisher`s (not state-gated), so the queue must be fully joined
   before the node/`publishers_` are torn down. Lifecycle re-entrancy: a
   cleanup→configure cycle re-creates a fresh, re-startable worker.
3. **Bound + observability** — byte budget as a ROS parameter
   (`publish_queue_max_bytes`, declared via `declareIfMissing`); register a
   single global drop diagnostic via the existing `syncDiagnosticTasks()` /
   `diagnostic_task_names_` pattern so a stalled local subscriber surfaces as a
   WARN instead of silently. (Drop count is a global tally, not per-remote.)
4. **Unit test** (`test/test_publish_queue.cpp`) — the non-stall property at the
   component level, which is also the bug: with a sink that blocks on a latch
   (simulating a stuck `publish()`), assert `push()` still returns promptly and
   the producer is never blocked; FIFO order via a recording sink; drop-oldest
   + counter when over the byte budget; clean `stop()`/join (including stop while
   the sink is blocked). Register via `add_udp_bridge_gtest` in `CMakeLists.txt`.
5. **End-to-end reproduction** — the kill-a-subscriber-mid-forward repro
   (acceptance #1) belongs in the issue #18 bench harness (PR #27 draft, which
   already targets the "wedge" failure mode). Coordinate rather than duplicate:
   this PR lands the component + the blocking-sink non-stall unit test;
   cross-link #18 for the live-DDS integration repro. (Residual: no test in
   *this* PR exercises a real dying DDS subscriber — the blocking-sink test is
   the proxy; see Open Questions.)
6. **Docs** — add a "Receive-path publish decoupling" subsection to
   `qos_design.md`: why the wedge is fixed at the transport layer (not QoS) and
   holds regardless of the RELIABLE/BEST_AVAILABLE default, and that queue drops
   are **unrecoverable** loss *after* successful wire delivery — distinct from,
   and not recoverable by, the resend layer (which operates pre-`decodeData`).
   Also update the `socket_drain_group_` invariant comment in
   `udp_bridge_node.cpp` (which currently predicts this exact "move publish work
   to a separate group via an internal queue" change).

## Files to Change

| File | Change |
|------|--------|
| `udp_bridge/include/udp_bridge/publish_queue.h` | New header-only bounded publish-worker queue (`PublishItem`, byte-bounded, injected sink, atomic drop counter) |
| `udp_bridge/src/udp_bridge.cpp` | Move first-arrival create + `sendBridgeInfo` + `publish` (738-776) into the queue sink lambda; `decodeData` deserializes + enqueues; construct/start queue in `on_configure`, `stop()`+join in `on_cleanup`; `declareIfMissing("publish_queue_max_bytes")`; drop diagnostic via `syncDiagnosticTasks` |
| `udp_bridge/include/udp_bridge/udp_bridge.h` | `PublishQueue` member + `publish_queue_max_bytes_`; the sink helper decl |
| `udp_bridge/src/udp_bridge_node.cpp` | Update the `socket_drain_group_` invariant comment (it predicts this change) |
| `udp_bridge/test/test_publish_queue.cpp` | New unit test (blocking-sink non-stall, FIFO, byte-budget drop-oldest, join-while-blocked) |
| `udp_bridge/CMakeLists.txt` | Register `test_publish_queue` via `add_udp_bridge_gtest` |
| `udp_bridge/doc/qos_design.md` | Document transport-level decoupling as the #10 fix + unrecoverable-drop note |

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
