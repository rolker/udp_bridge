---
issue: 10
---

# Issue #10 — Bridge wedges (reader thread blocked, Recv-Q backup) when remote subscriber dies

## Plan Authored
**Status**: complete
**When**: 2026-05-25 18:30 -0400
**By**: Claude Code Agent (Claude Opus 4.7 (1M context))

**Plan**: `.agent/work-plans/issue-10/plan.md` at `44a81ad`
**PR**: https://github.com/rolker/udp_bridge/pull/28 (`[PLAN]` prefix)
**Phases**: single

### Open questions
- [x] Cross-topic isolation scope — **decided: single publish-worker this PR; per-publisher isolation deferred to a follow-up issue** (user, 2026-05-25).
- [ ] Confirm the kill-subscriber end-to-end reproduction (acceptance #1) lands in the issue #18 bench harness (PR #27), not a standalone launch_test here.
