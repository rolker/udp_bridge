# Rebase notes: what udp_bridge#52 does to this branch

Not an ADR-0013 progress entry — this is coordination context for whoever
rebases `feature/issue-75` onto `feature/issue-52`. Recorded here rather than
left in a chat log, because the hazard is invisible in the diff.

Confirmed directly with the session working #52, verified by them against #52
commit `8944bf4`. #52 is mid-lifecycle (fix pass, then re-review, then an
operator publish checkpoint) — agreed ordering is **#52 first, then this
branch rebases** — but it is not imminent.

## Why a clean merge is the dangerous case

The two branches share 9 files and conflict textually in only 3, all docs. The
code auto-merges. That is the problem: #52 rewrites the control law these
tunables drive, and nothing in git will say so.

## Per-tunable

| Tunable | Under #52 | Consequence here |
|---|---|---|
| `admission_floor_bytes_per_second` | **Unchanged** — still `max(floor, cap * kAdmissionDecreaseFactor)` (`connection.cpp:327`) | Validation range stays correct |
| `link_headroom_fraction` | **Inert** — #52 removes the `(1 - headroom) * goodput` clamp from the congested branch; afterwards the member is read by nothing but its own setter and getter. Deliberately retained, not deleted | This branch's central invariant goes vacuous for it: an accepted value cannot read back as something other than what is in force, because nothing is in force |
| `resend_budget_fraction`, per-topic `maximum_bytes_per_second` | Unaffected | — |

`resend_constants.h`: #52's ~78 lines are pure additions with no
modifications, so the `kMaxLinkHeadroomFraction` hoist should not conflict.

## Must fix during the rebase

Four assertions on this branch become FALSE once #52 lands. **Three sit in the
three files that conflict**, so a mechanical "keep both sides" resolution
preserves each one as a description of a law that no longer exists:

- `doc/admission_control_design.md:48` — `(1 - link_headroom_fraction) x goodput` as the congested-branch target
- `doc/admission_control_design.md:134` — the paragraph justifying the parameter
- `README.md:212` — "Applied as the target of a congested backoff"
- `.agents/README.md:109` — same claim, **and it already cites "#52"** as its
  authority (from their earlier merged round), which is exactly what will make
  it read as current after their branch lands

The #52 session is taking the first three on their side. **`.agents/README.md:109`
is ours.**

`test_runtime_tunables.cpp:249 LinkHeadroomFractionAppliesToLiveConnection`
still PASSES after #52 — it goes hollow rather than red. It proves the value
reaches the live `Connection` while the `Connection` no longer consults it:
plumbing asserted as behaviour, which is close to the failure mode this branch
exists to remove. Rename it and state the inertness in the test rather than
delete it; the parameter is deliberately retained.

## Also pick up

#52 adds a per-connection `admission_refractory_period_seconds` (default 5.0)
that is **configure-time only** — the same defect this branch exists to fix, so
this branch completes #52. Add it to the runtime validation set on rebase and
pin it against #52's clamp. (Their round-1 review found the setter accepted
`+Inf`, freezing the controller for the life of the node; being fixed on their
side, so the clamp will exist to pin against.)

## The rebase target may change shape

The #52 session is putting the retained-but-inert question to their operator at
their publish checkpoint rather than deciding it themselves, and has warned that
the answer may be **delete it** — which would remove the parameter, its setter,
its getter and its plumbing, enlarging their diff and changing what this branch
rebases onto. In that case this branch does not just correct four sentences: it
drops `link_headroom_fraction` from the runtime validation set, the
`TunableTarget` registry, `connection_tunables.h`, `README.md`'s runtime table,
`example_params.yaml`, and deletes rather than renames
`LinkHeadroomFractionAppliesToLiveConnection`. They will say which before they
push. Do not start the rebase until that answer is known — the two outcomes need
different work, and the smaller one is not a subset of the larger.

## Raised, not decided

After #52, `link_headroom_fraction` is accepted, reads back, and does nothing —
the precise shape that cost roughly three minutes of link outage at Appledore,
where a value was set, read back, and a false conclusion drawn. Raised with the
#52 session for their operator checkpoint: if it stays retained-but-inert, its
`ParameterDescriptor` — not just the docs — should say it has no effect on the
control law, because `param describe` is what an operator reads under pressure.
