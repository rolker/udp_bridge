# Plan: Reorder/jitter buffer for out-of-order packets (instead of hard-drop)

## Issue

https://github.com/rolker/udp_bridge/issues/35

## Context

The stale-packet gate (`drop_stale_packets`, #33) hard-drops any decoded packet
whose `packet_number` is strictly below the per-topic high-water mark in
`RemoteNode::highest_published_packet_number_`. For topics where strict
latest-wins semantics are acceptable that is fine, but for near-simultaneous
reorders (two packets that arrive only a few ms apart, out of order) the gate
discards a packet that arrived only marginally late.

The existing call site is in `UDPBridge::decodeData` (`src/udp_bridge.cpp` ~L855):
the gate is checked synchronously on the socket-drain thread, before the payload
copy, before the packet reaches `PublishQueue`. The socket-drain spin_once cycle
is 10 ms, so a hold window measured in single-digit ms sits within a single drain
tick — an inline age-check on each tick is sufficient; no separate timer thread is
needed.

The high-water mark **must not advance** when a packet is buffered — it only
advances on actual publish. This lets the gap-filling packet (if it arrives in
time) be admitted by the existing `admitForPublish` logic unchanged.

## Approach

1. **Extend `RemoteNode`** — add per-topic reorder-buffer state and two new
   public methods. Keep `admitForPublish` signature unchanged (existing callers,
   tests, and the disabled path still use it directly).

2. **Add `admitOrBuffer`** — a new `RemoteNode` method that replaces
   `admitForPublish` on the hot path when the reorder window is enabled. Returns
   a 3-way `AdmitDecision` (`Admit` / `Drop` / `Buffer`). When a gap is detected
   (packet_number > high_water + 1) it stores the `PublishItem` in the
   per-topic buffer and returns `Buffer` **without advancing the high-water
   mark**. At-most-one buffered packet per topic: if a new packet arrives while
   one is already buffered for that topic, flush the buffered entry immediately
   (admit it as-is) before buffering the new one.

3. **Add `flushExpiredBuffer`** — called from `UDPBridge::decodeData` on every
   socket-drain tick. Returns any `PublishItem`s whose hold time has exceeded
   `hold_window_ms_`; the caller pushes them to `PublishQueue` in order.
   Also called internally by `admitOrBuffer` on a per-topic basis when a new
   packet fills the gap (publishes the filled-in packet first, then the held one)
   or overflows the at-most-one slot.

4. **Add `reorder_hold_window_ms` parameter** — global node parameter
   (like `drop_stale_packets`; per-topic tuning deferred to the #34 opt-in
   infrastructure). Type `double`, default `0.0` (disabled — reorder buffer off,
   behaviour identical to today). Declared in `on_configure` via `declareIfMissing`.
   When 0 the hot path stays on `admitForPublish` with no overhead.

5. **Flush buffer on remote restart** — `update(BridgeInfo)` already clears
   `highest_published_packet_number_` on restart detection; extend it to also
   clear the reorder buffer, consistent with the existing restart-handling contract.

6. **Integrate in `UDPBridge::decodeData`** — three-way branch:
   - `reorder_hold_window_ms_ == 0` (or gate disabled): existing path unchanged.
   - `AdmitDecision::Admit`: proceed to PublishQueue as today.
   - `AdmitDecision::Drop`: existing drop path (log + counter).
   - `AdmitDecision::Buffer`: packet is held in RemoteNode; return from
     `decodeData` without pushing to PublishQueue.
   Add a `flushExpiredBuffer` call at the top of the drain callback (before or
   after the `recvfrom` loop) to age out held packets on each 10 ms tick.

7. **Write tests** — new `test_reorder_buffer.cpp` gtest suite:
   - Gap detected → packet buffered, high-water not advanced.
   - Gap filled within window → both published in order (lower first, then held).
   - Window expires before gap filled → held packet released, late-arriving
     lower packet then dropped by gate.
   - At-most-one overflow: second buffered packet flushes the first.
   - Restart clears buffer (extend `triggerRemoteRestart` fixture from
     `test_stale_packet_gate.cpp`).
   - `hold_window_ms = 0` → no buffering; behaviour identical to today.
   Use fake `rclcpp::Time` injection (same pattern as `test_remote_node_resend.cpp`)
   to avoid real-time sensitivity.

8. **Update documentation** — `.agents/README.md` Key Parameters table and
   `config/example_params.yaml` in the same PR.

## Files to Change

| File | Change |
|------|--------|
| `udp_bridge/include/udp_bridge/remote_node.h` | Add `AdmitDecision` enum, `ReorderBufferEntry` struct, `reorder_buffer_` map, `admitOrBuffer()`, `flushExpiredBuffer()` declarations; extend restart-clearing contract comment |
| `udp_bridge/src/remote_node.cpp` | Implement `admitOrBuffer`, `flushExpiredBuffer`; extend `update(BridgeInfo)` to clear `reorder_buffer_` |
| `udp_bridge/src/udp_bridge.cpp` | Declare `reorder_hold_window_ms_` parameter in `on_configure`; extend `decodeData` with 3-way branch; add `flushExpiredBuffer` call on each drain tick |
| `udp_bridge/include/udp_bridge/udp_bridge.h` | Add `reorder_hold_window_ms_` member |
| `udp_bridge/test/test_reorder_buffer.cpp` | New gtest suite (step 7) |
| `udp_bridge/CMakeLists.txt` | Register `test_reorder_buffer` gtest target |
| `udp_bridge/.agents/README.md` | Add `reorder_hold_window_ms` row to Key Parameters table |
| `udp_bridge/config/example_params.yaml` | Add `reorder_hold_window_ms` with comment |

## Principles Self-Check

| Principle | Consideration |
|---|---|
| Human control and transparency | New parameter is opt-in (default 0 = disabled); the 3-way admit decision is logged; no hidden state changes |
| Only what's needed | Global hold window only (per-topic deferred to #34 infrastructure); at-most-one buffer slot per topic keeps state minimal |
| A change includes its consequences | Docs (`.agents/README.md`, `example_params.yaml`), tests, and restart-clear are all in this PR |
| Test what breaks | Tests use fake time injection to avoid real-time sensitivity; boundary cases (exact window expiry, at-most-one overflow) are covered |
| Improve incrementally | Single-PR scope; per-topic tuning and opt-in gate (#34) are separate follow-ups |
| Capture decisions, not just implementations | Design rationale (global-vs-per-topic deferral, at-most-one slot, high-water-freeze-on-buffer) documented in plan and PR description |

## ADR Compliance

| ADR | Triggered | How addressed |
|---|---|---|
| ADR-0001 — Adopt ADRs | No new design decision rises to ADR level (variant of an existing mechanism); the global-vs-per-topic deferral is documented in the plan and PR description | — |
| ADR-0002 — Worktree isolation | Yes — already in `feature/issue-35` worktree | Compliant |
| ADR-0008 — ROS 2 conventions | Yes — new parameter follows `declareIfMissing` pattern and `on_configure` lifecycle | Compliant |
| ADR-0013 — progress.md vocabulary | Yes — `## Plan Authored` entry written after commit | Compliant |
| ADR-0018 — Local-first CI | Yes — project repo PR may use `ci_local.sh` full-scope attestation | Compliant |

## Consequences

| If we change... | Also update... | Included in plan? |
|---|---|---|
| `RemoteNode::admitForPublish` hot path | Existing `test_stale_packet_gate.cpp` (no change expected — existing tests still pass on the unchanged method) | Yes — verified by running existing suite |
| `RemoteNode::update(BridgeInfo)` restart clear | `test_stale_packet_gate.cpp` `RemoteRestartResetsHighWater` test pattern | Yes — new test extends same fixture |
| New `reorder_hold_window_ms` parameter | `.agents/README.md` Key Parameters table; `config/example_params.yaml` | Yes |
| New gtest target `test_reorder_buffer` | `CMakeLists.txt` gtest registration; `.agents/README.md` Package Inventory test count | Yes |

## Documentation & Instruction Impact

- **Stale docs** (must land in this PR): `.agents/README.md` Key Parameters table (add `reorder_hold_window_ms`); Package Inventory gtest count (12 → 13); `config/example_params.yaml` (add parameter with comment).
- **Agent-instruction candidates** (proposals only — operator decides): The "freeze high-water on buffer, only advance on actual publish" invariant is a non-obvious contract worth noting in `.agent/knowledge/` or the udp_bridge `.agents/README.md` pitfalls section. Propose after implementation confirms the pattern holds.

## Open Questions

- Should `reorder_hold_window_ms` be validated/clamped in `on_configure`
  (e.g., to a max of 500 ms) to prevent large accidental values from causing
  noticeable latency regressions on all topics? Recommend yes: clamp 0–500 ms
  with a WARN on out-of-range, matching how `maximum_packet_size` is handled.
- The at-most-one-slot-per-topic design covers the primary use case (two
  near-simultaneous packets swapped). If multi-packet reorders are observed
  in the field, the buffer depth can be increased — but this is a follow-up,
  not a blocker.

## Estimated Scope

Single PR. All changes are confined to `udp_bridge` package, touching ~4 source
files plus a new test file and CMakeLists update. No interface changes to
`udp_bridge_interfaces`.
