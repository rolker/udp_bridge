---
issue: 57
---

# Issue #57 — Bench harness never exercises fragmentation / reassembly / resend-under-fragmentation

## Issue Review
**Status**: complete
**When**: 2026-08-22 21:27 +00:00
**By**: Claude Code Agent (Claude Sonnet)

**Issue**: #57
**Comment**: (best-effort post follows this entry; not recorded inline)
**Scope verdict**: well-scoped

### Summary

Two compounding bugs render the bench harness (PR #27, issue #18) unrepresentative of
field traffic:

1. **Zero-byte Bulk payload** — `pub.py` builds the Image payload as `bytes(480000)`,
   which zlib-compresses to ~488 bytes. On the bench, a nominally 480 kB message
   occupies a single UDP packet (65500-byte packets, 488-byte payload — no fragmentation).
   Source: `pub.py` line 35 (`self._payload_buf = bytes(payload_bytes)`).

2. **Maximum packet size 50–65× field value** — `configs/three_path.yaml` sets
   `maximum_packet_size: 65500` for both sides; field deployments use 1000–1400.
   Source: `three_path.yaml` lines 28–29 and 52–53 (both `operator_bridge` and
   `boat_bridge`).

The compound effect: the bench delivers single-packet Bulk messages at ~0.1% of the
claimed "~120% of WiFi rate budget" rate, while the field sends ~490 fragments per
480 kB message. The resend machinery — whose entire job is fragment loss recovery — is
never exercised.

### Scope Assessment

**Well-scoped?** Yes — five tightly coupled items, all on the harness representativeness
axis: (1) incompressible payload in `pub.py`, (2) field-representative `maximum_packet_size`
in config, (3) re-run tests + re-derive thresholds with stated reasoning,
(4) revisit #54 numbers, (5) fix the "~120%" comment. All belong in one PR.

**Right repo?** Yes — `udp_bridge` project repo. The bench lives under
`udp_bridge/test/bench/`.

**Dependencies**:
- **#54** (residual resend amplification at low loss) — measurements were taken in the
  zero-fragmentation regime. Revisiting is explicitly in scope.
- **#58** (shipped default `maximum_packet_size`) — related but separate; the issue
  correctly calls it out and defers it. No blocking dependency.
- **PR #56 / #52** — results are before/after on the same unrepresentative conditions;
  the before/after comparison is still valid. No rework needed on merged code.

### Principle Alignment

| Principle | Status | Notes |
|---|---|---|
| Test what breaks | Action needed | The harness must exercise fragmentation/reassembly paths — those are what break in the field. The fix is exactly this; the implementation must assert it (Acceptance criterion 2). |
| A change includes its consequences | Action needed | Thresholds in `test_range_degradation.py` and `test_subscriber_death.py` must be re-derived with stated reasoning. Do not just widen to fit — the issue explicitly forbids it. Derivations should appear as inline comments or be referenced in the bench README. |
| Capture decisions, not just implementations | Watch | New threshold values are design decisions. Record derivations (sample values, loss-rate arithmetic, field-analogue rationale) as comments in the test file or in the bench README's "Refinement plan" section — not just in commit messages. |
| Only what's needed | OK | Scope is tightly bounded: two files changed (pub.py + three_path.yaml), test re-runs, threshold updates. No new infrastructure. |
| Improve incrementally | OK | Single PR, all items already identified. |
| Human control and transparency | OK | The fix is explicit and documented. The "~120% comment" fix (scope item 5) directly corrects a misleading claim. |
| Enforcement over documentation | Watch | The Acceptance criterion "Bulk traffic within a stated tolerance of its nominal rate, asserted by the harness" should land as a real assertion, not just a comment update. The rate-checking assertion should be visible in the test rather than inferred from config. |

### ADR Applicability

| ADR | Triggered | Notes |
|---|---|---|
| ADR-0008 — Follow ROS 2 conventions | Yes | Test code follows ROS 2 package conventions. `os.urandom()` or `secrets.token_bytes()` are both fine; prefer `os.urandom()` for consistency with Python stdlib usage. |
| ADR-0013 — progress.md entry-type vocabulary | Yes | This entry; `plan-task` next. |

### Consequences

- **`pub.py`**: change `bytes(payload_bytes)` to `os.urandom(payload_bytes)`. The
  buffer-reuse pattern (allocated once, reused per tick, PR #56 note) is still correct
  and should be preserved — `bytes` from `os.urandom` is immutable and safe to share.
- **`configs/three_path.yaml`**: set `maximum_packet_size` to a field-representative
  value (1000 is the most common field value; 1200 is used on `lr30`). Update the
  "~120% of WiFi rate budget" comment to reflect actual measured rate after the fix.
- **`test_range_degradation.py` / `test_subscriber_death.py`**: thresholds derived
  against the zero-fragmentation regime will change. Re-derive each changed threshold
  with arithmetic (e.g., `survival = (1 - loss)^n_fragments`) and record it inline.
- **bench README** (if it exists / `test/bench/README.md`): update "Refinement plan"
  / THRESHOLDS section to reflect the new regime and new baseline values.

### Recommendations

- For the incompressible payload, `os.urandom(payload_bytes)` allocated once at
  `__init__` time (not per-tick) matches the PR #56 buffer-reuse pattern exactly and
  requires no other change to `pub.py`.
- When setting `maximum_packet_size` in the bench config, pick one value and document
  why (e.g., "1000 — matches `bizzyboat`/`izzyboat` field config, the most constrained
  deployed value"). Using the minimum field value is conservative and makes the bench
  the hardest reasonable scenario.
- Acceptance criterion 2 ("a large message fragments into a field-representative number
  of fragments, and the run exercises reassembly and fragment resends") could be asserted
  by checking bridge `topic_statistics` resend counts or fragment counts in at least one
  test phase — make this a real assertion, not just an expected side effect.
- The "revisit #54" item (scope item 4) may produce a separate follow-up issue rather
  than a fix in this PR if the numbers change substantially. That is fine; the PR can
  note the updated #54 numbers and defer the analysis.

### Actions
- [ ] Replace `bytes(payload_bytes)` with `os.urandom(payload_bytes)` in `pub.py` (keep buffer-reuse pattern)
- [ ] Set field-representative `maximum_packet_size` in `three_path.yaml`; document the chosen value's field analogue
- [ ] Re-derive all changed thresholds in `test_range_degradation.py` and `test_subscriber_death.py` with stated arithmetic — do not widen without reasoning
- [ ] Assert Acceptance criterion 2 (fragmentation + resend exercised) as a real harness assertion, not just a comment
- [ ] Update the "~120% of WiFi rate budget" comment in `three_path.yaml` to reflect actual post-fix rate
- [ ] Revisit #54 numbers once harness is representative; note updated values in PR description (separate issue if analysis warrants it)

## Plan Authored
**Status**: complete
**When**: 2026-08-22 00:00 +00:00
**By**: Claude Code Agent (Claude Sonnet)

**Plan**: `.agent/work-plans/issue-57/plan.md` at `a55692d`
**Branch**: feature/issue-57 at `a55692d`
**Phases**: single

### Open questions
- [ ] What is the observed `average_fragment_count` on the first post-fix run? Needed to confirm `T_fragment_floor = 400` is not too tight or too loose.
- [ ] Does F (resend multiplier = 2×) survive the new fragmentation regime? At 0.5% loss across ~505 fragments, ~92% of messages need ≥ 1 resend; may exceed 2× and require re-derivation.

## Plan Review
**Status**: complete
**When**: 2026-08-22 21:40 +00:00
**By**: Claude Code Agent (Claude Opus)

**Plan**: `.agent/work-plans/issue-57/plan.md` at `a55692d`
**PR**: PR-less (`--issue 57`, local worktree)
**Verdict**: approve-with-suggestions

Plan is well-targeted and technically grounded — verified every dependency
exists: `average_fragment_count` is a real field (`TopicStatistics.msg:11`),
`_wifi_rates_per_phase`/`read_topic_statistics_arrays` supply the reads, and
`recvq_wedge_ceiling_bytes` exists. Deferring threshold finalization to a
required experimental run is correct. One must-fix gap (the strict-xfail resend
test) must be folded in before implementation; the rest are completeness
refinements.

### Findings
- [ ] (must-fix) Plan is silent on `test_invariant_resend_amplification` being `@pytest.mark.xfail(strict=True)` — new fragmentation regime may flip it to XPASS (→ CI failure), and re-deriving F upward would mask the #52 residual the marker forbids loosening — `test_range_degradation.py:480-495`, `plan.md:46-52`
- [ ] (must-fix) Stale-string cleanup undercounts locations: `~58 MB/s` is in README.md:141 AND test_subscriber_death.py:13; `~120%` is in README.md:60 AND run_scenario.py:670 — enumerate all in Files-to-Change — `plan.md:57-59,71,94`
- [ ] (suggestion) `three_path.yaml:24-27,51-53` comment says the high packet size "is #57's concern, not this change" — becomes wrong once #57 changes it; replace it, don't just append — `plan.md:29-30`
- [ ] (suggestion) Real saturating Bulk newly exercises X/R/Y/K/N invariants (only ever validated at ~0.5 kB/s) — add them to the explicit post-run re-examination set — `plan.md:46-52`
- [ ] (suggestion) `test_invariant_bulk_wire_rate` reads connection-aggregate (`_wifi_rates_per_phase`), not per-topic Bulk; rename or source per-topic from topic_statistics — `plan.md:38-41`
- [ ] (suggestion) `T_fragment_floor` 0.85 rationale ("header compression") is wrong for incompressible payload — fragments trend ≥480, not fewer; floor of 400 is safe regardless — `plan.md:33-36`

## Implementation
**Status**: complete (part 1 of 2)
**When**: 2026-08-22 21:57 +00:00
**By**: Claude Opus

**Branch**: feature/issue-57
**Scope**: code/config/doc changes only. **Part 2 (host bench run + threshold
re-derivation) is owed** — see below. No `colcon build` was run (host owns the
build tree for this worktree; ros2_agent_workspace#602). No scenario was run:
`range_degradation`/`subscriber_death` need `unshare -Urn`, which this container
cannot grant. **No existing THRESHOLDS value was changed.**

### What changed (each atomic commit)
- [x] **Incompressible Bulk payload** — `pub.py` `bytes()` → `os.urandom()`, buffer-reuse preserved (`8b8aaa2`)
- [x] **maximum_packet_size 1000** — both stanzas in `three_path.yaml`, with the bizzyboat/izzyboat + worst-case-fragment-count rationale inline; old "#57's concern" comment **replaced** (`027522c`)
- [x] **Fragmentation acceptance assertion** — `test_invariant_fragmentation_exercised` reads boat send-side `average_fragment_count` for `/boat/bulk/image`, asserts `>= T_fragment_floor` (400) in `in_range_clean` (`4d6a373`)
- [x] **Wire-rate acceptance assertion** — `test_invariant_bulk_wire_rate` sourced **per-topic** from `topic_statistics` (`send.success_bytes_per_second`), not the WiFi connection aggregate; asserts `>= W_wire_pct × 4 MB/s` (`4d6a373`)

### Plan Review findings folded in (all operator-approved)
- [x] (must-fix) resend xfail `strict=True` XPASS-under-fragmentation risk documented in a comment on the marker + in plan.md; marker NOT removed, `F_resend_multiplier` NOT raised — a #54 decision after the run (`68175bc`, plan `bf963ca`)
- [x] (must-fix) all **four** stale-string locations fixed: `~58 MB/s` in README.md + test_subscriber_death.py (`e06ed13`); `~120%` in README.md + run_scenario.py (`ced1353`)
- [x] (suggestion) `three_path.yaml` "#57's concern" comment replaced, not appended (`027522c`)
- [x] (suggestion) X/R/Y/K/N added to the post-run re-examination set (plan `bf963ca`, README `946d543`)
- [x] (suggestion) `T_fragment_floor` rationale corrected — incompressible payload trends **≥ 480** fragments, not fewer; the "× 0.85 header compression" reasoning dropped; floor 400 stays (threshold comment `4d6a373`, plan `bf963ca`, README `946d543`)
- [x] (suggestion) wire-rate test named/sourced for what it measures (per-topic Bulk) (`4d6a373`)
- [x] README documents invariants 10 & 11 and adds `T_fragment_floor`/`W_wire_pct` rows + refinement item (`946d543`)

### Verification actually performed
- `python3 -m py_compile` on all four changed `.py` files — pass.
- `yaml.safe_load` on `three_path.yaml`; both `maximum_packet_size` now `1000`, no `65500` remains.
- `_bulk_stats_per_phase` bucketing/averaging logic exercised in isolation with a mock (repeated `in_range_clean` windows merge; other phases bucket separately; non-Bulk topics ignored) — pass.
- **Not** verified: the invariants against a real run (needs `unshare -Urn` + build) — that is part 2.

### Part 2 owed — thresholds flagged for re-derivation from the host bench run
- `T_fragment_floor` (currently 400) — re-derive from observed `average_fragment_count`
- `W_wire_pct` (currently 0.5) — re-derive from observed Bulk `send.success_bytes_per_second`
- `F_resend_multiplier` (currently 2.0) — re-examine; do **not** raise; watch for the strict-xfail XPASS → CI-failure flip (a #54 decision)
- `recvq_wedge_ceiling_bytes` (currently 100_000, test_subscriber_death.py) — re-derive; single-packet-era value, real fragmented Bulk may raise the healthy drain edge
- Re-check (new regime touches them for the first time): **X** (recovery), **R** (stats rate), **Y** (cross-path), **K** (co-tenant), **N** (Recv-Q climb)

All changed-threshold TODOs carry `TODO(#57 part 2)` markers in-tree.

## Implementation
**Status**: complete (part 2 of 2) — host bench run + threshold disposition
**When**: 2026-08-22 19:20 -04:00
**By**: Claude Code Agent (Claude Opus 5 (1M context))

**Branch**: feature/issue-57 at `c05d426`
**Commits**: `d3bf110` (metric fixes), `c05d426` (defect recording + threshold annotations)

Part 1 stopped at the measurement boundary. This is the host side: the run
part 1 could not do, and the threshold work that depends on it.

### The first honest run produced three failures — all different in kind
- [x] **fragmentation 254 vs floor 400 — measurement artifact, fixed.**
      `MessageStatistics::get()` buckets by (remote, connection) and
      `UDPBridge::callback` records a pre-send point keyed `("","")` with
      `fragment_count` 0 (`udp_bridge.cpp:773-777`); averaging it with the real
      `("operator","wifi")` bucket halved the metric. Filtered on non-empty
      `connection_id`. Observed after: **508** (predicted ~505).
- [x] **Bulk wire rate 467 kB/s vs 2 MB/s — wrong metric, fixed.** Asserted
      SUCCESS where the limiter legitimately sheds an over-budget tier; that
      would have been asserting admission control fails. Now asserts OFFERED
      (success + dropped) against `BULK_NOMINAL_BPS`. Observed **5.08 MB/s =
      1.06x nominal**; the 6% is fragment-header overhead and cross-checks the
      fragment count.
- [x] **recovery to 0.1% / 0.3% of baseline — real defect.** Filed
      [#60](https://github.com/rolker/udp_bridge/issues/60); `xfail(strict=True)`
      with the ramp arithmetic (~54 s vs `T_recovery_s` 30) and an explicit
      prohibition on lowering `X_recovery_pct` / widening `T_recovery_s`.

### A fourth surfaced on the next run
- [x] **co-tenant p95 15.768 s at `critical#3` — real defect, different cause.**
      Investigated at operator request rather than deferred. Latency spikes at
      the downshift and decays monotonically (13.2 -> 4.7 -> 1.9 -> 0.12 s):
      a standing queue draining, not starvation. netem inherits the kernel
      default `limit 1000`; 1000 x 1000 B / 62,500 B/s = **16.0 s** against
      15.768 s observed — 1.5%. Rate headroom cannot recall bytes already
      committed to a FIFO, so `maximum_bytes_per_second` and #52's
      `link_headroom_fraction` were never going to prevent it. Filed
      [#61](https://github.com/rolker/udp_bridge/issues/61).
      Invariant refined to carry two bounds for two phenomena: 5 s steady
      state, 20 s for windows whose link rate dropped (derived from the drain
      arithmetic). **Intermittent** — a later run measured 0.132 s in the same
      window — so the bound stays asserted, and #61 records that a single
      clean run does not retire it.

### Thresholds — re-derived or reason stated (plan step 6)
| threshold | observed | disposition |
|---|---|---|
| `T_fragment_floor` 400 | 508 frags | unchanged; ~21% margin, guards collapse-to-1 not exact count |
| `W_wire_pct` 0.5 | 1.06x nominal | unchanged; the failure it guards is 4 orders below the floor |
| `K_cotenant_delivery_pct` 0.50 | worst 0.89 | unchanged; margin is real headroom against jitter |
| `K_cotenant_p95_latency_s` 5.0 | steady max 0.054 s | unchanged; two orders of margin |
| `K_cotenant_transient_p95_latency_s` | new, 20.0 | derived from the 16.0 s worst-case drain |
| `X_recovery_pct` / `T_recovery_s` | 0.3% of baseline | untouched — #60 xfail forbids adjusting them |
| `F_resend_multiplier` | — | untouched — #54 xfail forbids raising it |

### Verification (host)
- Clean rebuild; **178 tests, 0 errors, 0 failures** (default suite).
- `range_degradation`: **10 pass, 2 xfail, 0 fail**.
- `subscriber_death`: **both invariants pass** — and for the first time this
  means something, since the frozen consumer is now fed 4.8 MB/s of
  incompressible fragmented Bulk rather than ~5 kB/s of single packets. The
  "no wedge at bench scale" finding has narrowed three times; this is the
  first run that actually tests it.
- flake8 + `pre-commit run --from-ref origin/jazzy --to-ref HEAD` clean.

### Next step
Lifecycle: **Implementation → review-code** (pre-push). Nothing pushed; no PR.

## Local Review (Pre-Push)
**Status**: complete
**When**: 2026-08-22 23:28 +00:00
**By**: Claude Code Agent (Claude Opus)
**Verdict**: changes-requested

**Branch**: feature/issue-57 at `db937b2`
**Mode**: pre-push
**Depth**: Deep (reason: test_range_degradation.py +277/-6 ≥200 lines; project plan.md override-trigger)
**Must-fix**: 1 | **Suggestions**: 1
**Round**: 1 | **Ship**: continue — one mechanical README metric/denominator fix, then shippable; no correctness/design concern

### Findings
- [x] (must-fix) README invariant #11 / W_wire_pct row / refinement item 6 describe `send.success_bytes_per_second` × the 4 MB/s WiFi budget, but code asserts offered (success+dropped) × 4.8 MB/s BULK_NOMINAL_BPS — `udp_bridge/test/bench/README.md:129,191,224` (code at `test_range_degradation.py:947`)
- [x] (suggestion) Dead constant `WIFI_BUDGET_BPS = 4_000_000.0`, never referenced — `udp_bridge/test/bench/test_range_degradation.py:61`
