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
- [ ] (suggestion) Bench config explicitly sets maximum_packet_size: 65500 — confirm intentional (IP-fragmentation path) or lower/comment — `udp_bridge/test/bench/configs/three_path.yaml:24,48`
- [ ] (suggestion) Add a one-line CHANGELOG/release note that the implicit default dropped to 1200 (behavior change; strictly safer) — `udp_bridge/README.md`
