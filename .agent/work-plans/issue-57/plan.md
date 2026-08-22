# Plan: Bench harness never exercises fragmentation / reassembly / resend-under-fragmentation

## Issue

https://github.com/rolker/udp_bridge/issues/57

## Context

The bench harness (`test/bench/`) builds Bulk (`sensor_msgs/Image`) payloads as
`bytes(480000)` (all zeros), which compresses to ~488 bytes and travels in a single
UDP packet because `maximum_packet_size: 65500`. Field deployments use 1000-byte packets,
producing ~490–510 fragments per 480 kB message. Two causes compound:

1. **`pub.py:35`** — `self._payload_buf = bytes(payload_bytes)` (all zeros → compresses to 488 B)
2. **`three_path.yaml`** — `maximum_packet_size: 65500` on both sides; field uses 1000

Result: the harness runs single-packet Bulk delivery at ~0.5 kB/s (wire), while the boat
sends ~490-fragment messages at ~4 MB/s. Resend machinery is never exercised.

## Approach

1. **Make Bulk payload incompressible** (`pub.py`) — replace `bytes(payload_bytes)` with
   `os.urandom(payload_bytes)` on line 35. `bytes` from `os.urandom` is immutable; the
   single-allocation buffer-reuse pattern from PR #56 is preserved unchanged.

2. **Set field-representative packet size** (`three_path.yaml`) — change both
   `operator_bridge` and `boat_bridge` `maximum_packet_size: 65500` to `1000`
   (chosen over 1200, the shipped node default as of #58): `1000` matches the
   `bizzyboat`/`izzyboat` field config — the boats this work is for — and is the
   smallest deployed value, hence the worst case for fragment count, so the
   invariants derive against the hardest real configuration. **Replace** (do not
   append to) the existing `three_path.yaml:24-27,51-53` comment saying the high
   value is "#57's concern, not this change" — that claim becomes wrong the moment
   #57 changes it.

3. **Add fragmentation acceptance assertion** (`test_range_degradation.py`) — new test
   `test_invariant_fragmentation_exercised`: reads `average_fragment_count`
   (`TopicStatistics.msg:11`) from the boat send-side `topic_statistics` for
   `/boat/bulk/image` in `in_range_clean` phase; asserts `>= T_fragment_floor`.
   Derivation: the payload is **incompressible**, so a 480 KB message over a
   1000-byte packet fragments into `floor(480000 / 1000) = 480` pieces at minimum;
   per-fragment bridge headers reduce effective payload per packet, so the real
   count trends **≥ 480**, not fewer (the earlier "× 0.85 header compression"
   rationale was wrong — an incompressible payload gets no compression discount).
   `T_fragment_floor = 400` sits safely below with margin for partial-window
   sampling. Addresses Acceptance criterion 2. TODO(#57 part 2): re-derive from
   the host run's observed count.

4. **Add wire-rate acceptance assertion** (`test_range_degradation.py`) — new test
   `test_invariant_bulk_wire_rate`: sources the **per-topic** Bulk send rate
   (`send.success_bytes_per_second` for `/boat/bulk/image`) from the boat
   `topic_statistics`, NOT the WiFi connection aggregate (`_wifi_rates_per_phase`
   folds in Critical+Telemetry, so a "bulk" name over that aggregate would
   overstate what it measures). Asserts `send_success_bps >= W_wire_pct × 4_000_000`
   in `in_range_clean`. Initial `W_wire_pct = 0.5` (WiFi budget is shared and
   fragment-header overhead consumes it; generous start). Addresses Acceptance
   criterion 1. TODO(#57 part 2): re-derive from run data.

5. **Re-run both scenarios** with `UDP_BRIDGE_BENCH_SCENARIOS=1`. Record raw per-phase
   outputs (fragment counts, resend ratios, success_bps). This run is required before
   THRESHOLDS can be finalized.

6. **Re-derive changed thresholds** — update `THRESHOLDS` dicts in both test files with
   inline arithmetic derivations. Candidates: `F` (resend amplification; fragment resends
   dominate now — at 0.5% loss across ~505 fragments, ~92% of messages need a resend),
   `W_wire_pct` and `T_fragment_floor` from observed run values,
   `recvq_wedge_ceiling_bytes` in `test_subscriber_death.py` (real fragmented traffic in
   flight, not single packets). For any threshold that does not change, state why inline.

   **Post-run re-examination set.** Real saturating Bulk now exercises invariants that
   were only ever validated at ~0.5 kB/s single-packet traffic, so the host run must
   re-examine — beyond the changed thresholds above — the full set
   **F, W_wire_pct, T_fragment_floor, recvq_wedge_ceiling_bytes** PLUS
   **X, R, Y, K, N** (recovery completeness, stats-rate floor, cross-path
   non-poisoning, co-tenant delivery/latency, Recv-Q climb), which the new regime
   touches for the first time. State inline why any that don't change stay put.

   **`test_invariant_resend_amplification` is `@pytest.mark.xfail(strict=True)`.**
   Under the old zero-fragmentation regime resend traffic was near-zero and the xfail
   held trivially. With real fragmentation (~480 pieces at ~0.5% loss, nearly every
   message needing a resend) a lossy phase's resend/tx_ok ratio may move the other way
   and flip the marker to **XPASS**, which with `strict=True` is a **CI failure**. Do
   NOT pre-emptively remove the marker and do NOT raise `F_resend_multiplier` (raising F
   would mask the residual the marker records). Whether the marker comes off is a **#54
   question**, decided from the host run's numbers. The interaction is documented in a
   comment on the marker in `test_range_degradation.py`.

7. **Update README** — correct "~120% of WiFi rate budget" (now accurate with
   incompressible payload: 10 Hz × 480 KB = 4.8 MB/s = 120% of 4 MB/s WiFi budget).
   Update threshold table with new values and derivation sources. Correct the
   subscriber_death note that cited "~58 MB/s of Bulk" (was actually ~5 kB/s
   single-packet traffic).

8. **Note #54 impact** — record updated resend amplification numbers in PR description.
   Open a follow-up issue if they differ substantially from #54's findings.

## Files to Change

| File | Change |
|------|--------|
| `udp_bridge/test/bench/pub.py` | `bytes(payload_bytes)` → `os.urandom(payload_bytes)` (line 35); add `import os` |
| `udp_bridge/test/bench/configs/three_path.yaml` | Both `maximum_packet_size: 65500` → `1000`; **replace** the "#57's concern" comment with the 1000/field-source rationale |
| `udp_bridge/test/bench/test_range_degradation.py` | Add `test_invariant_fragmentation_exercised`, `test_invariant_bulk_wire_rate` + shared helper; add `T_fragment_floor`, `W_wire_pct` to THRESHOLDS; add #57/XPASS note on the resend xfail; re-derive F + X/R/Y/K/N after run |
| `udp_bridge/test/bench/test_subscriber_death.py` | Correct the stale `~58 MB/s` claim (line 13 — it was ~5 kB/s single-packet); TODO to re-derive `recvq_wedge_ceiling_bytes` from the post-fix run |
| `udp_bridge/test/bench/run_scenario.py` | `~120%` claim (line 670) — now realized on the wire with the incompressible payload |
| `udp_bridge/test/bench/README.md` | Stale `~120%` (line 60) and `~58 MB/s` (line 141); add the two #57 acceptance invariants and `T_fragment_floor`/`W_wire_pct` threshold rows |

**Stale-string enumeration (must-fix):** `~58 MB/s` appears in **both**
`README.md:141` and `test_subscriber_death.py:13`; `~120%` appears in **both**
`README.md:60` and `run_scenario.py:670`. All four are fixed.

## Principles Self-Check

| Principle | Consideration |
|---|---|
| Test what breaks | Fixes put the harness in the regime where resend machinery does real work (~490 fragments at 0.5% loss; nearly every message needs a resend round) |
| A change includes its consequences | All threshold changes carry inline arithmetic derivations; #54 impact noted in PR |
| Enforcement over documentation | Both acceptance criteria land as real assertions, not comments |
| Only what's needed | Two code-line changes + test additions + threshold updates; no new infrastructure |
| Capture decisions, not just implementations | Derivations inline in THRESHOLDS dicts and README threshold table |

## ADR Compliance

| ADR | Triggered | How addressed |
|---|---|---|
| ADR-0008 — Follow ROS 2 conventions | Yes | `os.urandom()` is stdlib; test structure follows ROS 2 package layout |
| ADR-0013 — progress.md entry-type vocabulary | Yes | Issue Review exists; Plan Authored entry follows |

## Consequences

| If we change... | Also update... | Included in plan? |
|---|---|---|
| `pub.py` payload | `test_subscriber_death.py` "~58 MB/s" README claim | Yes — step 7 |
| `three_path.yaml` packet size | Both `operator_bridge` and `boat_bridge` sides | Yes — both sides, step 2 |
| THRESHOLDS values | README threshold table | Yes — step 7 |
| New assertions added | README invariant list (invariants 10, 11) | Yes — step 7 |

## Documentation & Instruction Impact

- **Stale docs** (must land in this PR): `udp_bridge/test/bench/README.md` — "~120% of WiFi
  rate budget" and the threshold table reflect the old unrepresentative regime; update with
  post-fix values. Subscriber_death "~58 MB/s Bulk" claim must be corrected.
- **Agent-instruction candidates** (proposals only): A bench-harness representativeness check
  could be added to `.agent/knowledge/` — "before recording threshold values from the bench,
  verify payload is incompressible and `maximum_packet_size` matches field config." This issue
  is the third time the bench's basis has narrowed a finding.

## Open Questions

- What is the observed `average_fragment_count` on the first post-fix run? Needed to confirm
  `T_fragment_floor = 400` is not too tight or too loose. (The payload is incompressible, so
  per-fragment bridge headers push the count **above** `floor(480000/1000)=480`, not below —
  expect ≥ 480; the floor of 400 leaves margin.)
- Does F (resend multiplier = 2×) survive the new regime? At 0.5% loss across ~505 fragments,
  ~92% of messages need ≥ 1 resend. If multiple rounds are common, the ratio may exceed 2×
  and F will need re-derivation. Cannot finalize without running the scenario.

## Estimated Scope

Single PR. Steps 1–4 are code changes (can land before the run); step 5 is a required
experimental run; steps 6–8 depend on run data. The run must complete before THRESHOLDS
are finalized — the PR lands after the run.
