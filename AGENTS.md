# AGENTS.md — udp_bridge

Instructions for AI agents working in this repository — including **GitHub
Copilot code review**, which reads this file when reviewing PRs. There is no
`.agents/README.md` deep guide yet; start from the top-level `README.md`.

## Workspace Rules

This repo is developed inside a
[ROS 2 Agent Workspace](https://github.com/rolker/ros2_agent_workspace).
The workspace root `AGENTS.md` carries the full shared rules (worktree
isolation, issue-first policy, commit conventions, AI signatures). This file
**references** those rules and adds repo-specific context only — it must
never restate or fork them.

## Quality Standard

This is software for autonomous robot boats operating on open water.
Robustness is not optional.

- Fix bugs completely: add the test, handle the edge case, check the
  lifecycle transition.
- Concerns about error handling, silent failures, stale data, or missing
  validation are not nits — flag them unless the failure mode genuinely
  cannot occur. "Config is under our control" and "pathological input" are
  not blanket dismissals; field configs change under pressure.
- A change includes its consequences: tests, documentation, and dependent
  references update in the same PR.

## Reviewing PRs

- If the PR carries a work plan (`.agent/work-plans/issue-<N>/plan.md` or a
  plan in the PR body), the plan is kept **in sync with the implementation
  as it evolves** — an implementation that matches the current plan text is
  not "plan drift", even if the plan changed after the PR opened.
- Verify claims against source: parameters, topics, services, and message
  types in docs must match the code.

## Review Context — udp_bridge

- **This is the boat↔operator telemetry link** over lossy field radio paths
  (WiFi, cell, Starlink/VPN). Link loss and over-horizon operation are
  **valid operating modes, not errors** — degradation must be graceful and
  disconnects should not be surfaced at ERROR severity.
- **Bandwidth/rate fields are bytes per second** (`*_bytes_per_second`),
  not bits — unit confusion is a recurring bug class in analysis and
  display code built on these stats.
- **Stale-data gating is an operator-facing safety behavior**: whether stale
  remote data is shown or suppressed changes what the operator sees during
  a live run. Changes to gating defaults need explicit operator sign-off.
- **Connection config is a single complete per-connection file**: nested
  partial parameter overrides silently break return-path routing (the boat
  replies to the wrong host and data flow drops to zero).
