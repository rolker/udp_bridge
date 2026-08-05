# Plan: Cap/back off resend bandwidth — resend storms self-amplify into degraded links

## Issue

https://github.com/rolker/udp_bridge/issues/44

## Context

`Connection::resend_packets()` sends all requested missing packets at full
rate, with no ceiling on the fraction of a connection's budget that resends
may consume.  Under ack starvation (inbound link collapse starves the sender
of acknowledgements), the sender treats all in-flight packets as lost and
retries them at multiples of link capacity, sustaining the storm until restart.

Observed twice: 2026-05-01 (13 MB/s resend vs 1.0 MB/s VPN link) and
2026-08-04 (1.5 MB/s dropped + 220 kB/s sent resends vs 1.2 MB/s cap on
VPN; similar on WiFi and Starlink).

The rate limit in `PacketSendStatistics::can_send()` already applies to all
packet categories including resends, but it does not prevent resends from
crowding out fresh data — it just prevents the *total* from exceeding the
cap.  A resend storm can consume 100% of the budget.

## Approach

1. **Add `resend_budget_fraction` field to `Connection`** — a configurable
   float [0, 1] (default 0.25) representing the maximum fraction of
   `data_rate_limit_` that resend traffic may consume per second.  Store in
   `resend_budget_fraction_`, guarded by `config_mutex_`.  Add
   `setResendBudgetFraction(float)` / `resendBudgetFraction() const`.

2. **Add constants to `resend_constants.h`** — `kDefaultResendBudgetFraction`
   (0.25f), `kAckStarvationThreshold` (1.0 s: interval before backoff begins),
   `kMaxAckStarvationBackoffShift` (4: max 16× reduction → floor of ~1.6%).

3. **Modify `Connection::resend_packets()`** — before sending each resend
   packet, enforce the effective budget:
   - Effective fraction = `resend_budget_fraction_` scaled down by
     `2^min(starvation_steps, kMaxAckStarvationBackoffShift)`, where
     `starvation_steps = floor((now − last_receive_time) / kAckStarvationThreshold)`.
     Zero starvation (recent ack activity) → full fraction.
   - Budget bytes = `effective_fraction * data_rate_limit_`.
   - Query recent resend sent bytes from `sent_packet_statistics_.get(PacketSendCategory::resend).success_bytes_per_second`.
   - Iterate `packets_to_resend`; stop (drop remainder) once
     `already_sent_resend_bps + pending_packet_size > budget_bytes`.
   - Fresh data is never affected — only `PacketSendCategory::resend` packets.
   - FIFO order within the resend list (as today); priority scheduling (#19)
     is a follow-up that decides *what* gets shed once a budget binds.

4. **Wire parameter in `udp_bridge.cpp`** — alongside the existing
   `maximum_bytes_per_second` read (line ~400), declare and read
   `remotes.<label>.connections.<id>.resend_budget_fraction` (double, default
   `kDefaultResendBudgetFraction`), then call
   `connection->setResendBudgetFraction(static_cast<float>(fraction))`.
   Also call `setResendBudgetFraction` in the `addRemote` service handler
   (line ~1805) if a per-connection fraction is carried in the service
   request — otherwise set the default there.

5. **Update `README.md`** — add `resend_budget_fraction` (float, 0.0–1.0,
   default 0.25) under the per-connection parameter table.

6. **Write `doc/resend_budget_design.md`** — design note covering: problem
   statement with field measurements, the two mechanisms (budget fraction +
   ack-starvation backoff), algorithm, tuning rationale, ordering vs #19, and
   interaction with existing `kSentPacketTTL` / per-packet backoff constants.
   Format and location mirrors `doc/qos_design.md`.

7. **Add `test/test_resend_budget.cpp`** and register it in `CMakeLists.txt`
   — regression test with two cases:
   - **Bounded under load**: configure a `Connection` with a small rate limit
     and `resend_budget_fraction=0.25`; seed `sent_packets_` with many
     entries; call `resend_packets()` with the full missing list on a loopback
     socket; assert
     `data_sent_rate(PacketSendCategory::resend).success_bytes_per_second <=
     rate_limit * 0.25 * tolerance` (tolerance 1.1× for quantisation).
   - **Ack-starvation reduction**: same setup but inject a stale
     `last_receive_time` (2 × `kAckStarvationThreshold` seconds ago via a
     `update_last_receive_time` call with an old timestamp); assert resend
     budget is at most 0.5× the baseline budget (one backoff step).

## Files to Change

| File | Change |
|------|--------|
| `udp_bridge/include/udp_bridge/resend_constants.h` | Add `kDefaultResendBudgetFraction`, `kAckStarvationThreshold`, `kMaxAckStarvationBackoffShift` |
| `udp_bridge/include/udp_bridge/connection.h` | Add `resend_budget_fraction_`, `setResendBudgetFraction()`, `resendBudgetFraction()` |
| `udp_bridge/src/connection.cpp` | Implement setter/getter; modify `resend_packets()` to enforce budget + starvation backoff |
| `udp_bridge/src/udp_bridge.cpp` | Read `resend_budget_fraction` param (~line 400) and in `addRemote` handler (~line 1805); call `setResendBudgetFraction` |
| `udp_bridge/README.md` | Add `resend_budget_fraction` to per-connection parameter table |
| `udp_bridge/doc/resend_budget_design.md` | New design note (problem, algorithm, tuning, ordering) |
| `udp_bridge/test/test_resend_budget.cpp` | New regression test (bounded under load; ack-starvation reduction) |
| `udp_bridge/CMakeLists.txt` | Register `test_resend_budget.cpp` under `ament_add_gtest` |

## Principles Self-Check

| Principle | Consideration |
|---|---|
| Human control and transparency | Fraction is per-connection and configurable; default 0.25 exposes safe behavior without requiring operator tuning; field-incident measurements inform the default |
| Capture decisions, not just implementations | `doc/resend_budget_design.md` records fraction model, ack-starvation backoff algorithm, and ordering decision relative to #19 |
| A change includes its consequences | README updated for new parameter; regression test included in same PR; design note in same PR |
| Only what's needed | Two mechanisms backed by two field incidents with measurements; no priority interface (deferred to #19) |
| Improve incrementally | Minimal bound first (FIFO shedding); priority scheduling in #19 decides what to shed within the budget |
| Test what breaks | Inline regression test asserts bounded resend rate under ack starvation without netns harness |
| Safety First | Resend storms degrade the operator link; capping them is safety-positive |
| Modularity and Decoupling | Fraction is per-connection; logic lives in `Connection`, which already owns rate accounting |

## ADR Compliance

| ADR | Triggered | How addressed |
|---|---|---|
| ADR-0001 — Adopt ADRs | Yes | Design decisions recorded in `doc/resend_budget_design.md` (no ADR directory in this repo; operator confirmed design-note format) |
| ADR-0008 — Follow ROS 2 conventions | Yes | New parameter follows existing `remotes.<label>.connections.<id>.<param>` naming; `setResendBudgetFraction` follows existing setter convention |

## Consequences

| If we change... | Also update... | Included in plan? |
|---|---|---|
| `Connection` adds `resend_budget_fraction_` | `remote_node.cpp` `adoptConnection` path (no change needed — fraction set via `setResendBudgetFraction` after adoption) | Yes — wiring in `udp_bridge.cpp` |
| New ROS 2 parameter | `README.md` parameter table | Yes — step 5 |
| New algorithm in `resend_packets()` | Interaction with `kSentPacketTTL` / per-packet backoff (receiver-side); noted in design note | Yes — step 6 |
| Resend admission changes | `#19` priority scheduling (WHAT gets shed once budget binds) | No — deliberate follow-up; design note records the interface boundary |

## Documentation & Instruction Impact

- **Stale docs** (must land in this PR): `README.md` — new `resend_budget_fraction` parameter entry. `doc/resend_budget_design.md` — new file (not stale, but required by operator checkpoint decision 1).
- **Agent-instruction candidates** (proposals only): The pattern of checking `last_receive_time()` as an ack-starvation proxy may be useful for future adaptive rate-control work (#36 umbrella); could note in `.agent/knowledge/` that this field is now used for sender-side decisions, not just diagnostics.

## Open Questions

- [ ] No open questions — all operator checkpoint decisions resolved; plan is review-plan-ready.

## Estimated Scope

Single PR — two mechanisms (budget fraction + ack-starvation backoff) are tightly coupled in `resend_packets()` and ship together as the minimal bound before #19.
