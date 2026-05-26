# mininet tests — retired

The mininet-based manual integration scripts that used to live here are
**retired**. They needed root + a `mininet` install and never ran in CI.

The current bench harness replaces them with an **unprivileged
Linux-netns + `tc netem`** approach that runs without root (and skips
cleanly where unavailable):

➡ **[`../bench/`](../bench/)** — orchestrator, scenarios (smoke /
full-mix / range_degradation / subscriber_death), thresholds, and run
instructions in [`../bench/README.md`](../bench/README.md).

The old ROS-1-vintage scripts and launch files are preserved for
reference under [`.archived/`](.archived/) but have no callers.
