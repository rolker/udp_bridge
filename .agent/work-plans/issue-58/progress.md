---
issue: 58
---

# Issue #58 — Lower the default maximum_packet_size from 65500 to 1200


## Local Review (Pre-Push)
**Status**: complete
**When**: 2026-08-22 18:02 +00:00
**By**: Claude Code Agent (Claude Opus)
**Verdict**: changes-requested

**Branch**: feature/issue-58 at `6deade2`
**Mode**: pre-push
**Depth**: Standard (reason: 89 changed lines, 50-199 band; no Deep/override triggers)
**Must-fix**: 1 | **Suggestions**: 2
**Round**: 1 | **Ship**: continue — one mechanical doc fix outstanding; re-review should converge immediately

### Findings
- [x] (must-fix) `.agents/README.md` param table still lists maximum_packet_size default 65500 — stale after lowering to 1200 — `udp_bridge/.agents/README.md:87`
- [x] (suggestion) Bench config explicitly sets maximum_packet_size: 65500 — confirm intentional (IP-fragmentation path) or lower/comment — `udp_bridge/test/bench/configs/three_path.yaml:24,48`
- [x] (suggestion) Add a one-line CHANGELOG/release note that the implicit default dropped to 1200 (behavior change; strictly safer) — `udp_bridge/README.md`

## Implementation
**Status**: complete
**When**: 2026-08-22 18:06 +00:00
**By**: Claude Code Agent (Claude Opus)

**Branch**: feature/issue-58 at `4781a64`
**Addressed**: Local Review (Pre-Push) (When 2026-08-22 18:02 +00:00, reviewed `6deade2`)
**Commits**: `0365d86`, `9174999`, `4781a64`

### Actions
- [x] (must-fix) Synced `.agents/README.md` param table default 65500 → 1200, with a one-line note on the tunnelled-link rationale — `.agents/README.md:87` (`0365d86`)
- [x] (suggestion) Confirmed the bench's explicit `65500` is intentional (not the node default) and documented it inline at both bridge stanzas — kept the value, deferred representativeness to #57 per the original commit rationale — `udp_bridge/test/bench/configs/three_path.yaml:24,48` (`9174999`)
- [x] (suggestion) Added a "Behavior change (#58)" release note to the parameter reference flagging the implicit default dropped 65500 → 1200 (strictly safer; only unconfigured deployments affected) — `udp_bridge/README.md:82` (`4781a64`)

Quick checks: `three_path.yaml` re-validated as parseable YAML; grep confirms no remaining stale "default 65500" text (only the correct 256–65500 clamp range and the new change note). Pre-commit hooks ran on every commit (no `--no-verify`). Untracked `a.out` left in place — pre-existing stray build artifact, unrelated to this work.

**Next step**: re-review — dispatch `review-code` (pre-push re-review) as a fresh-context sub-agent to confirm the findings are genuinely resolved.

## Local Review (Pre-Push)
**Status**: complete
**When**: 2026-08-22 18:13 +00:00
**By**: Claude Code Agent (Claude Opus)
**Verdict**: approved

**Branch**: feature/issue-58 at `6d1ca06`
**Mode**: pre-push
**Depth**: Standard (reason: touches .agents/ governance doc; consistent with round 1)
**Must-fix**: 0 | **Suggestions**: 0
**Round**: 2 | **Ship**: recommended — round-1 must-fix + both suggestions all addressed and verified; two adversarial lenses + static analysis found nothing actionable

### Findings
- [ ] No issues found. LGTM.
