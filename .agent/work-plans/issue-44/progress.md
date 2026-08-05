---
issue: 44
---

# Issue #44 — Cap/back off resend bandwidth — resend storms self-amplify into degraded links

## Issue Review
**Status**: complete
**When**: 2026-08-05 00:00 +00:00
**By**: Claude Code Agent (Claude Sonnet)

**Issue**: #44
**Comment**: (best-effort post follows this entry; not recorded inline)
**Scope verdict**: well-scoped

### Scope Assessment

**What is proposed**: Two complementary mechanisms to prevent resend storms on
degraded links:
1. Budget resends as a bounded fraction of each connection's `maximum_bytes_per_second`
   (fresh data first; fraction configurable per connection).
2. Exponential backoff on the resend path when ack stream from a remote collapses
   (ack starvation → retrying harder cannot help).
3. Open design question: per-connection resend admission vs. fanning out to all
   connections (field data from 2026-08-04 supports per-connection scoping — storm
   resent on all three connections including the SIM-less cell path).

**Well-scoped?** Mostly yes. The rate-cap and backoff mechanisms address the same
root cause (unbounded retry load under degraded links) and can ship together. The
per-connection vs. global admission question is a design decision for plan-task to
resolve. Could optionally be split into two PRs (cap first, backoff second), but
joint delivery is defensible.

**Right repo?** Yes — behavior change lives in `rolker/udp_bridge`.

**Dependencies**:
- #36 (transport rework umbrella) — parent; no blocking dependency.
- #19 (priority scheduling) — governs what is shed once a resend budget binds;
  must be verified compatible or sequenced before this issue.
- #18 (bench harness) — regression gate ("assert resend rate stays bounded") depends
  on this harness existing; plan-task should assess whether to depend on #18 or add
  the gate inline.
- #21 (give-up counter semantics) — informational context from the same 08-04
  dataset; no blocking dependency.

### Principle Alignment

| Principle | Status | Notes |
|---|---|---|
| Human control and transparency | OK | Configurable fraction per connection; explicit behavior documented in field incidents |
| Capture decisions, not just implementations | Watch | Design choices (fraction value, per-connection vs. global, backoff parameters) should be recorded in an ADR or design note before merge |
| A change includes its consequences | Watch | New configurable parameters → README/API docs must land in same PR; #18 bench harness status needs confirming |
| Only what's needed | OK | Backed by two field incidents with specific measurements; no speculative scope |
| Improve incrementally | Watch | Two mechanisms combined; splitting (cap / backoff) is an option if PR becomes large |
| Test what breaks | OK | Issue explicitly calls out #18 bench harness and a regression gate; bench and degraded-link tests are the right target |
| Safety First (project) | OK | Resend storms degrade operator link; capping them is safety-positive |
| Modularity and Decoupling (project) | OK | Configurable per-connection limits align with community-reuse design goal |
| Iterative, Validated Evolution (project) | OK | Field validation already documented; bench harness provides pre-field gate |

### ADR Applicability

| ADR | Triggered | Notes |
|---|---|---|
| ADR-0001 — Adopt ADRs | Yes | Design decisions about resend fraction, backoff strategy, and per-connection vs. global admission should be captured in an ADR or equivalent record |
| ADR-0002 — Worktree isolation | N/A | Already in correct worktree (`feature/issue-44`) |
| ADR-0008 — Follow ROS 2 conventions | Yes (standard) | Package changes; follow ROS 2 parameter and interface conventions |

### Consequences

From the consequences map — changes to this issue will require:
- **Package parameters** (new `resend_budget_fraction`, backoff config): README/API docs
  must reflect them in the same PR; check whether
  `.agents/review-context.yaml` maps connection parameters.
- **Connection behavior change**: verify compatibility with #19 (priority scheduling)
  before or during implementation.
- **Bench harness (#18)**: if not yet merged, decide whether to land #18 first or
  embed the regression gate inline.

### Actions
- [ ] Record the resend-budgeting design decision in an ADR (fraction model, backoff strategy, per-connection vs. global admission) — before or in the same PR as implementation.
- [ ] Clarify ordering/compatibility with #19 (priority scheduling) — the issues intersect where a binding budget determines what gets shed.
- [ ] Update README/API docs for all new configurable parameters in the same PR.
- [ ] Confirm #18 bench harness status; if not available, add the regression test ("resend rate stays bounded under ack starvation") inline.
