# Plan: Adaptive per-connection rate control — back off when a link's delivered capacity drops

## Issue

https://github.com/rolker/udp_bridge/issues/43

## Context

Post-#44, `Connection` has `resend_budget_fraction_` (per-connection param under
`config_mutex_`), `bytes_in_window(category, time)` for strict 1-second window
accounting, ack-starvation backoff via `last_receive_time()` (0.0 sentinel for new
connections), and `doc/resend_budget_design.md` as the design-doc precedent.

#44 bounds the *resend category*. This PR adds the *total-admission* counterpart:
AIMD scaling of an `effective_rate_limit_` per connection. Congestion signal:
compare the remote's echoed `BridgeInfo.RemoteConnection.received_bytes_per_second`
(what the remote actually received from us on this connection) against our own
smoothed sent rate from `data_sent_rate()`. Fallback: when `last_receive_time()`
is stale, treat as max congestion — folds in #44's ack-starvation signal.

Operator checkpoint decisions (2026-08-05) resolved the design fork: signal is
delivered-vs-sent, no wire-protocol change needed (the field already exists in
`RemoteConnection.msg`).

## Approach

1. **Add AIMD constants to `resend_constants.h`** — `kAdmissionDecreaseFactor = 0.5f`,
   `kAdmissionAdditiveStepFraction = 0.1f` (fraction of rate_limit per feedback
   interval), `kAdmissionLossThreshold = 0.1f` (trigger decrease when delivery < 90%
   of sent), `kDefaultAdmissionFloorFraction = 0.1f` (min effective cap as fraction of
   static limit).

2. **Extend `Connection`** (`connection.h` + `connection.cpp`):
   - Add `float effective_rate_limit_` (init to `default_rate_limit`, guarded by
     `config_mutex_`). Updated by AIMD; `setRateLimit()` **follows the new
     limit when no backoff has accumulated** (effective at the old limit,
     incl. fresh construction — so configuring a larger cap binds
     immediately) **and otherwise clamps to `min(effective, new_limit)`**
     rather than resetting (review-plan S2: a CONNECT/adopt or addRemote
     re-applying an unchanged limit must not wipe accumulated backoff and
     burst a congested link at full rate; a backed-off connection reaches a
     raised cap via additive recovery). Implementation note: the resend
     budget (#44) now bases on `effective_rate_limit_` instead of
     `data_rate_limit_` so resends shrink proportionally with admission —
     documented in both design notes; #44 tests unchanged (caps equal
     without AIMD activity).
   - Add `float admission_floor_fraction_` (default `kDefaultAdmissionFloorFraction`,
     guarded by `config_mutex_`).
   - New methods: `setAdmissionFloorFraction(float)` / `admissionFloorFraction()` /
     `effectiveRateLimit()` returning `uint32_t`.
   - New method: `updateAdmissionControl(float remote_received_bps, rclcpp::Time now)`:
     - Reads own sent rate via `data_sent_rate(now)` (smoothed total success bps,
       no lock ordering hazard — two separate lock acquisitions).
     - Reads `last_receive_time()` to detect feedback-stale fallback
       (`lrt > 0.0 && now.seconds() - lrt >= kAckStarvationThreshold.count()`).
     - AIMD: if congested (fallback OR `sent_bps > 0` AND `delivered < (1 - kAdmissionLossThreshold) * sent_bps`):
       `effective_rate_limit_ = max(floor, effective_rate_limit_ * kAdmissionDecreaseFactor)`
     - Else: `effective_rate_limit_ = min(data_rate_limit_, effective_rate_limit_ + kAdmissionAdditiveStepFraction * data_rate_limit_)`
     - When `sent_bps == 0` (link idle): additive increase (no congestion possible).
   - Change both `Connection::send()` overloads to read `effective_rate_limit_`
     (cast to `uint32_t`) instead of `data_rate_limit_` for the `can_send` check.

3. **Update `RemoteNode::update(BridgeInfo, source_info)`** (`remote_node.cpp`):
   After `c->setSourceIPAndPort(...)`, add
   `c->updateAdmissionControl(connection_info.received_bytes_per_second, clock_->now())`.

4. **Update `RemoteConnection.msg`** — Add `uint32 effective_rate_limit` field (the
   current AIMD-adjusted cap; 0 when no feedback has been received yet, or equals
   `maximum_bytes_per_second` at startup before any BridgeInfo arrives).

5. **Update `udp_bridge.cpp`**:
   - `on_configure`: declare and read
     `remotes.<remote>.connections.<conn>.admission_floor_fraction` (double,
     default `kDefaultAdmissionFloorFraction`), call
     `live_connection->setAdmissionFloorFraction(...)` after `setResendBudgetFraction`.
   - `sendBridgeInfo()`: populate
     `rc.effective_rate_limit = connection->effectiveRateLimit()` alongside
     `rc.maximum_bytes_per_second`.

6. **Write `test/test_admission_control.cpp`** — 7 pinned test cases:
   1. `DecreaseOnCongestion` — `remote_received_bps` < 90% of sent → effective cap halves.
   2. `AdditiveRecovery` — clean delivery → effective cap increases by 10% of rate limit.
   3. `FloorClamp` — repeated decreases never drop below `floor_fraction * rate_limit`.
   4. `CeilingClamp` — repeated recovery does not exceed `data_rate_limit_`.
   5. `FeedbackStaleBackoff` — stale `last_receive_time` → decrease to floor even with zero remote_received_bps.
   6. `NeverReceivedSentinel` — `last_receive_time() == 0.0` is not stale; no fallback.
   7. `EffectiveLimitAffectsCanSend` — after AIMD reduces effective cap, `send()` sheds at the reduced rate.
   Register in `CMakeLists.txt` via `add_udp_bridge_gtest(test_admission_control ...)`.

7. **Write `doc/admission_control_design.md`** — Design note mirroring
   `doc/resend_budget_design.md`: problem (2026-08-04 Broadkill WiFi congestion
   collapse), mechanism (delivered-vs-sent AIMD), decisions (signal choice rationale,
   constant values, fallback, scope honesty vs #19/#36, `data_rate_limit_` vs
   `effective_rate_limit_` for resend budget interaction), verification.
   Per review-plan, the doc must also state honestly:
   - **S1 scope boundary**: the stale-feedback fallback fires per connection on
     BridgeInfo receipt, so it can only mark connections *other than* the one
     that delivered the BridgeInfo; a single connection in total inbound
     blackout receives no updateAdmissionControl calls at all and gets no AIMD
     decrease from either path (that regime is #45/#44 territory — the resend
     budget's own starvation backoff still bites there). Congestion-with-
     feedback (the primary 08-04 field mode) is fully covered because
     BridgeInfo keeps flowing under congestion.
   - **S3 signal caveats**: the delivered/sent ratio is a trend signal, not an
     exact loss measure — remote `received_bytes_per_second` includes
     duplicates, our sent rate includes resend traffic, the two rates use
     different smoothing windows, and BridgeInfo adds propagation delay.
   - **Lock-ordering note**: `updateAdmissionControl` reads `data_sent_rate()`
     and `last_receive_time()` (each under its own mutex) BEFORE acquiring
     `config_mutex_` — never nested.

8b. **Update `.agents/README.md`** (review-plan S4) — add
   `admission_floor_fraction` to the "Key Parameters (verified in
   on_configure)" table AND backfill the missing `resend_budget_fraction`
   row (stale since #44).

8. **Update `udp_bridge/README.md`** — Add `admission_floor_fraction` parameter entry
   alongside `resend_budget_fraction`.

9. **Update `config/example_params.yaml`** — Add `admission_floor_fraction` with comment.

## Files to Change

| File | Change |
|------|--------|
| `udp_bridge/include/udp_bridge/resend_constants.h` | Add `kAdmissionDecreaseFactor`, `kAdmissionAdditiveStepFraction`, `kAdmissionLossThreshold`, `kDefaultAdmissionFloorFraction` |
| `udp_bridge/include/udp_bridge/connection.h` | Add `effective_rate_limit_`, `admission_floor_fraction_`, new accessor/update methods |
| `udp_bridge/src/connection.cpp` | Implement `updateAdmissionControl()`; change both `send()` overloads to use `effective_rate_limit_`; update `setRateLimit()` to reset `effective_rate_limit_` |
| `udp_bridge/src/remote_node.cpp` | Call `updateAdmissionControl` in `update(const BridgeInfo&, const SourceInfo&)` |
| `udp_bridge/src/udp_bridge.cpp` | Wire `admission_floor_fraction` param in `on_configure`; populate `rc.effective_rate_limit` in `sendBridgeInfo()` |
| `udp_bridge_interfaces/msg/RemoteConnection.msg` | Add `uint32 effective_rate_limit` field |
| `udp_bridge/test/test_admission_control.cpp` | New: 7 gtest cases for AIMD invariants |
| `udp_bridge/CMakeLists.txt` | Register `test_admission_control` gtest target |
| `udp_bridge/doc/admission_control_design.md` | New: design note (mirrors `resend_budget_design.md`) |
| `udp_bridge/README.md` | Document `admission_floor_fraction` parameter |
| `udp_bridge/config/example_params.yaml` | Add `admission_floor_fraction` with comment |

## Principles Self-Check

| Principle | Consideration |
|---|---|
| Human control and transparency | `effective_rate_limit` in `RemoteConnection` telemetry lets operators observe backoff events; `admission_floor_fraction` param gives control over the minimum effective cap |
| Capture decisions, not just implementations | `doc/admission_control_design.md` records the signal choice (delivered-vs-sent over resend-request-rate or ack-latency), AIMD constants, and scope boundaries |
| Only what's needed | One new float field per Connection; reuses `last_receive_time()`, `data_sent_rate()`, `can_send()`, and `config_mutex_` infrastructure from #44 |
| Test what breaks | 7 unit tests cover all AIMD contract points without waiting for bench harness #18 |
| Improve incrementally | Total admission as a layer on top of #44's resend cap; #19 per-topic priority stays deferred |
| A change includes its consequences | README, param YAML, design doc, and telemetry field all land in the same PR |

## ADR Compliance

| ADR | Triggered | How addressed |
|---|---|---|
| ADR-0001 (Adopt ADRs) | Yes | Design captured in `doc/admission_control_design.md`; not workspace-wide so no `docs/decisions/` ADR needed |
| ADR-0002 (Worktree isolation) | Yes | Worktree `issue-udp_bridge-43` already active |
| ADR-0008 (ROS 2 conventions) | Yes | `admission_floor_fraction` follows `resend_budget_fraction` naming; `setAdmissionFloorFraction`/`admissionFloorFraction()` follow existing accessor style |
| ADR-0013 (progress.md vocabulary) | Yes | This plan triggers a `## Plan Authored` entry in progress.md |

## Consequences

| If we change... | Also update... | Included in plan? |
|---|---|---|
| `RemoteConnection.msg` adds `effective_rate_limit` | `sendBridgeInfo()` must populate it | Yes — step 5 |
| Both `Connection::send()` overloads use `effective_rate_limit_` | `test_connection_rate_limit.cpp` may need review if it tests at `data_rate_limit_` exactly | Yes — existing tests exercise the `can_send` path; verify they still pass |
| `setRateLimit()` follows (no backoff) / clamps (backoff) `effective_rate_limit_` — never resets | addRemote / CONNECT flaps preserve accumulated backoff; larger configured caps bind immediately on un-backed-off connections | Yes — shipped + pinned by `SetRateLimitClampPreservesBackoff` |
| Resend budget (#44) bases on `effective_rate_limit_` | `resend_packets()` + both design notes updated; #44 tests unchanged (caps equal without AIMD activity) | Yes — shipped + documented in both design notes |
| Resend budget (`resend_packets`) still uses `data_rate_limit_` | Not `effective_rate_limit_` — resend budget stays relative to the static cap, so it stays meaningful even during AIMD reduction | Documented in design doc (scope honesty) |

## Documentation & Instruction Impact

- **Stale docs** (must land in this PR): `udp_bridge/README.md` (missing `admission_floor_fraction`); `config/example_params.yaml` (missing param); `RemoteConnection.msg` gains new field; `doc/admission_control_design.md` (new file).
- **Agent-instruction candidates** (proposals only — operator decides): The delivered-vs-echoed-BridgeInfo congestion pattern may be worth a note in `.agent/knowledge/` for future transport-layer work in this or other projects. The AIMD constants-in-`resend_constants.h` / param-in-`on_configure` pattern established by #44 and extended here could be the basis of a general transport-tuning guide.

## Open Questions

- [ ] No open questions — plan is review-plan-ready. Operator checkpoint (2026-08-05) resolved all five open actions from the issue review.
