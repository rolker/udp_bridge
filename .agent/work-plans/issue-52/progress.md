---
issue: 52
---

# Issue #52 — Resend amplification under real link degradation

## Plan Authored
**Status**: complete
**When**: 2026-08-22 11:25 -04:00
**By**: Claude Code Agent (Claude Opus 5 (1M context))

**Plan**: `.agent/work-plans/issue-52/plan.md` at `036a716`
**Branch**: feature/issue-52 at `036a716` (stacked on `feature/issue-18` / PR #27, unmerged)
**Phases**: single PR with atomic commits by default; may split into three
(admission scaling / resend-budget basis / bench invariant)

Root cause refined while planning: it is not that AIMD is too slow. **Every
control parameter is scaled by `data_rate_limit_`** — the additive increase
step (`0.1 x cap` = +400 kB/s per clean sample on a 62.5 kB/s link), the
admission floor (`0.1 x cap`), and transitively the resend budget
(`0.25 x effective`). The additive step is the immediate defect: one clean
sample undoes three halvings, which is why the measured cap oscillated around
2291 kB/s and never even reached its own 400 kB/s floor. The delivery signal
itself is sound and needs no wire-protocol change; what changes is what the
step sizes and floors are measured against.

`maximum_bytes_per_second` is explicitly NOT removed — it stays as a hard
ceiling and as cost control on metered cell links. Only its implicit second
job, acting as the model of link capacity, goes away.

### Open questions
- [ ] Absolute floor value — what rate must always be admitted? (suggest 4–8 kB/s from the critical tier + BridgeInfo; boat's real control set should decide)
- [ ] Headroom target — aim for a fraction of measured capacity (prevents lockout by construction) or merely avoid overshoot? The former is a behaviour change beyond fixing the scaling.
- [ ] Deprecate `admission_floor_fraction` outright or keep one release?
- [ ] One PR with atomic commits, or three stacked PRs?
- [ ] If PR #27 merges first, rebase this branch onto `jazzy` before opening the PR.
