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

## Confirmed by the #52 session after their fix pass

Their three doc sites are corrected in their tree (not yet pushed):
`admission_control_design.md` now says the decrease "reads no goodput term at
all" and carries a dedicated section on the clamp removal; `README.md:213` goes
further than deleting the false claim and states the parameter is
**"Currently inert"** in those words. `.agents/README.md` and the test remain
ours, untouched by them.

**New scope for this branch**: they deliberately DEFERRED the
`ParameterDescriptor` half to #76 — i.e. to us. Their reasoning, which holds:
`read_only: true` is the load-bearing part and is a behaviour change belonging
with the work that decides which parameters stop being configure-time-only, and
a bare `description` on 4 of ~30 parameters is inconsistency without the
contract. So the descriptor for `link_headroom_fraction` — the thing an operator
actually reads under pressure — lands here regardless of which way the
delete-vs-retain question goes.

Refractory constants to add to the runtime validation set, final:

- `admission_refractory_period_seconds` — per-connection double, default **5.0**
- `kMaximumAdmissionRefractoryPeriodSeconds` = **60.0**; the setter clamps DOWN
  to it rather than falling back to the default, so an operator asking for a
  slower loop gets the slowest one that still leaves the loop alive. NaN and
  negatives fall back to the default.
- `kAdmissionRefractoryGrowthFactor` = 2.0, `kAdmissionRefractoryMaximumMultiple`
  = 2.0 (window doubles within an unresolved episode, capped at 2x base, resets
  on the first clean sample)

Caveat they flagged: `Connection` has no logger, so a clamped value is silently
clamped at that layer. The clamp is the fix; the "nothing logged" half cannot be
closed there — it would have to be surfaced from the parameter callback, which
is this branch's territory.

## SETTLED: link_headroom_fraction stays DECLARED, inert, with a WARN

The #52 session's operator first chose to delete the parameter outright, then
reversed after their round-2 review. **The settled outcome is: keep it
DECLARED, remove all behaviour, and WARN once at on_configure if it is set to a
non-default value.** Do not act on the deletion — it is superseded. Their
round-3 pass writes the final shape; wait for their confirmation before
rebasing.

The reasoning for the reversal is worth keeping, because it is this branch's own
argument turned around. Deleting genuinely breaks nothing — the design doc's
"removing it would break existing configs" is false by this repo's own
behaviour, since `udp_bridge.cpp:363-368` shows an override for an UNDECLARED
parameter is silently ignored rather than rejected. But that is precisely what
argues against deleting: with the parameter gone, someone setting it later from
an old config or a stale doc gets **no error and no warning** — the same
"set it, reads fine, does nothing, draw a false conclusion" shape this branch
exists to eliminate, relocated from the setter to the config loader. The repo
already solved this once, for the retired `remotes.<label>.name` key: keep it
declared so a stale key stays VISIBLE, then refuse a non-default value.

Consequences for this branch, superseding the deletion advice:

- **Keep the `kMaxLinkHeadroomFraction` hoist** if the round-3 shape still needs
  the constant; they will confirm what survives.
- **Do NOT delete `LinkHeadroomFractionAppliesToLiveConnection`.** Rename it and
  state explicitly that it proves plumbing, not behaviour — which is now exactly
  right, because plumbing is precisely what the parameter still has.
- `.agents/README.md:109` is a **correction** again, not a deletion, and should
  describe the tripwire.
- **The descriptor work lands here.** A declared-but-inert parameter carrying a
  WARN is the strongest case for `param describe` saying "currently has no
  effect on the control law": the WARN fires at configure, but `param describe`
  is what an operator reads at 3am mid-survey.

## Mirror-image issue/PR confusion on their branch

Their round 2 found five references calling **#76** the tracking issue when #76
is the PR and #75 is the issue — the mirror image of the error flagged in our
issue body. Being fixed in their round 3. Worth a grep on this branch too before
it goes up.

## Do not trust a single bench run

They overturned an earlier claim of theirs that a `lossy` bench regression was
"not noise". Re-measured at n=3, resend/msg on `lossy` gave 0.0131 / 0.0184 /
0.0705 — a 5.4x spread — and `critical` 0.2838 / 0.4735 / 0.1115. The move that
had been called significant sits inside that spread. The durable finding is
about the measurement, not the control law: single-run values from that
invariant do not support three decimals. **If this branch touches those bench
assertions, do not trust a single run either.**

## Raised, not decided

After #52, `link_headroom_fraction` is accepted, reads back, and does nothing —
the precise shape that cost roughly three minutes of link outage at Appledore,
where a value was set, read back, and a false conclusion drawn. Raised with the
#52 session for their operator checkpoint: if it stays retained-but-inert, its
`ParameterDescriptor` — not just the docs — should say it has no effect on the
control law, because `param describe` is what an operator reads under pressure.
