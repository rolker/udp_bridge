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
