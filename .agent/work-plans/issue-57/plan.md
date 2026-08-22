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
   `operator_bridge` and `boat_bridge` `maximum_packet_size: 65500` to `1000`. Add
   inline comment: matches `bizzyboat`/`izzyboat` field config, the most constrained
   deployed value.

3. **Add fragmentation acceptance assertion** (`test_range_degradation.py`) — new test
   `test_invariant_fragmentation_exercised`: reads `average_fragment_count` from
   `topic_statistics` for `/boat/bulk/image` in `in_range_clean` phase; asserts
   `>= T_fragment_floor`. Initial derivation: `floor(480000 / 1000) × 0.85 ≈ 408`;
   use `T_fragment_floor = 400`. Addresses Acceptance criterion 2.

4. **Add wire-rate acceptance assertion** (`test_range_degradation.py`) — new test
   `test_invariant_bulk_wire_rate`: reads `_wifi_rates_per_phase` in `in_range_clean`
   and asserts `success_bps >= W_wire_pct × 4_000_000`. Initial `W_wire_pct = 0.5`
   (WiFi budget is shared with Telemetry; generous start, re-derive from run data).
   Addresses Acceptance criterion 1.

5. **Re-run both scenarios** with `UDP_BRIDGE_BENCH_SCENARIOS=1`. Record raw per-phase
   outputs (fragment counts, resend ratios, success_bps). This run is required before
   THRESHOLDS can be finalized.

6. **Re-derive changed thresholds** — update `THRESHOLDS` dicts in both test files with
   inline arithmetic derivations. Candidates: `F` (resend amplification; fragment resends
   dominate now — at 0.5% loss across ~505 fragments, ~92% of messages need a resend),
   `W_wire_pct` and `T_fragment_floor` from observed run values,
   `recvq_wedge_ceiling_bytes` in `test_subscriber_death.py` (real fragmented traffic in
   flight, not single packets). For any threshold that does not change, state why inline.

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
| `udp_bridge/test/bench/pub.py` | `bytes(payload_bytes)` → `os.urandom(payload_bytes)` (line 35) |
| `udp_bridge/test/bench/configs/three_path.yaml` | Both `maximum_packet_size: 65500` → `1000`; add field-source comment |
| `udp_bridge/test/bench/test_range_degradation.py` | Add `test_invariant_fragmentation_exercised`, `test_invariant_bulk_wire_rate`; add `T_fragment_floor`, `W_wire_pct` to THRESHOLDS; re-derive F and others after run |
| `udp_bridge/test/bench/test_subscriber_death.py` | Re-derive `recvq_wedge_ceiling_bytes` from post-fix run data |
| `udp_bridge/test/bench/README.md` | Correct "~120%" claim, threshold table, subscriber_death "~58 MB/s" note |

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
  `T_fragment_floor = 400` is not too tight or too loose. (If compression of ROS message
  headers reduces effective payload density, fragment count may be 10–15% lower than 480.)
- Does F (resend multiplier = 2×) survive the new regime? At 0.5% loss across ~505 fragments,
  ~92% of messages need ≥ 1 resend. If multiple rounds are common, the ratio may exceed 2×
  and F will need re-derivation. Cannot finalize without running the scenario.

## Estimated Scope

Single PR. Steps 1–4 are code changes (can land before the run); step 5 is a required
experimental run; steps 6–8 depend on run data. The run must complete before THRESHOLDS
are finalized — the PR lands after the run.
