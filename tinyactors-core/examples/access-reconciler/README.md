# Access reconciler — a Kubernetes-style controller for joiners, movers and leavers

Every company has this problem, and most handle it badly: people join, change
teams, get promoted and leave, and their access to a dozen SaaS apps is
supposed to follow. In practice the ex-employee still has GitHub admin three
months later, the new hire waits a week for Slack, and someone's "temporary" AWS
admin is permanent.

This example solves it the way Kubernetes solves "make the cluster look like the
manifest": **a controller per person** continuously reconciles what they
*should* have (the HR record plus a policy) with what every app *actually*
grants. It runs on one `ta_system`. The person's controller is an SCXML chart
([charts/access.scxml](charts/access.scxml)). The host
([host/sim.c](host/sim.c)) simulates the rest of the world deterministically.

```sh
mise run example:access-reconciler                       # 10,000 people, 30 days (release build)
mise run example:access-reconciler --employees 1500 --check --quiet   # invariants checked
mise run example:access-reconciler --employees 50 --days 5 --trace    # every event
```

Run these in `tinyactors-core`. The task builds the program
(`build/release/bin/access-reconciler`) and runs it; `--seed` gives another
deterministic run, and `--trace` sets `TRACE=1`. `mise run examples:test`
runs it with 1,500 people for 30 days, invariants checked, on the sanitizer
build (`build/dev/bin/access-reconciler`).

## Kubernetes concepts, mapped

| Kubernetes | Here |
|---|---|
| resource `spec` | the HR record: status (active / leaving / left), department, role, legal hold, end date |
| controller + reconcile loop | one SCXML actor per person: `reconciler` region, `pass` → `waiting` |
| level-triggered | every trigger (spec change, app reply, watch event, resync, approval, timer) re-runs the same pass over the full picture. No trigger carries an instruction. |
| desired vs observed | `desired` (policy minus unapproved privileges) vs `observed` (what the apps reported) |
| idempotent actions | `apply` with key *(person, app, level, generation, revision)*: a retry after a lost response reuses the key, so the app deduplicates it |
| requeue with backoff | `apply.failed` → exponential backoff (30 s … 1 h) or the app's `Retry-After`, plus deterministic jitter |
| watch / informer | `changed` events when someone edits an app directly; some get lost |
| resync period | daily `list` → `inventory` snapshot; it catches whatever the watch missed |
| status conditions, `observedGeneration` | `status` to HR whenever it changes: Ready, Progressing, AwaitingApproval, Degraded, Blocked, `generation` / `observedGeneration` |
| finalizer | `offboarding` → `finalized`: everything is revoked (except what a legal hold keeps) before the record may be deleted; overdue after 1 h → alert |
| admission / approval | privileged levels (GitHub admin, AWS admin) need a manager's approval. Until then the person gets the least-privilege level. Approval requests expire after 72 h. |

## The controller

```text
┌─ alive (parallel) ─────────────────────────────────────────────────────────┐
│ reconciler                 resync                  lifecycle               │
│ ┌──────┐  eventless  ┌─────────┐   ┌───────────┐   ┌──────────┐            │
│ │ pass │────────────▶│ waiting │   │  armed    │   │ employed │──┐         │
│ └──────┘◀────────────└─────────┘   │ 24 h →    │   └──────────┘  │ spec    │
│  policy, diff,   any trigger:      │ list apps │   ┌──────────┐  │         │
│  actions,        spec, apply.ok,   └───────────┘   │ leaving  │◀─┘         │
│  approvals,      apply.failed, retry,              │ end date │            │
│  status          changed, inventory,               └────┬─────┘            │
│                  approval.*, reconcile                  ▼ offboard.due     │
│                                                    ┌──────────────┐ 1 h    │
│                                                    │ offboarding  │──alert │
│                                                    └──────┬───────┘        │
│                                                in sync ▼  (finalizer)      │
│                                                    ┌───────────┐           │
│                                                    │ finalized │──▶ deleted│
│                                                    └───────────┘           │
└────────────────────────────────────────────────────────────────────────────┘
```

One pass (`pass`'s `onentry`):

1. **Policy.** Everyone gets Google and Slack. Engineering gets GitHub and AWS
   (admin for leads). Sales gets a Salesforce license, support gets Zendesk.
   Offboarding gets nothing, except a suspended Google account under legal hold.
2. **Least privilege.** A privileged level without approval becomes the safe
   level (GitHub member, AWS read-only) and an approval request goes out.
3. **Diff.** Every app where `desired` and `observed` disagree, nothing is in
   flight and no retry is pending becomes an `apply` with an idempotency key.
4. **Status.** Conditions are recomputed and sent to HR if they changed.

All state lives in `<data>`, nothing in the host. Hosts restart, and
controllers hibernate.

## The world (host)

[host/sim.c](host/sim.c) is a deterministic discrete-event simulation. It jumps
the system's clock (`ta_system_advance_time`) to the next host event or system
timer, so 30 days run in seconds and every run with the same seed is identical.

- **HR:** each day brings joiners, leavers (end dates within two weeks, 15% under
  legal hold, HR marks them "left" up to two days late), movers (promotions,
  demotions, transfers; privileges get re-approved) and rescinded resignations.
  Mid-run, a reorg moves 5% of engineering to support.
- **Apps:** 50–1500 ms latency, a 6 h Slack outage, a 12 h Google error spike
  (30%), a 3 h AWS outage timed over end-of-day leavers, GitHub always rate
  limited (120 requests/min). 0.5% of responses are lost, keys are
  deduplicated, and Salesforce has fewer licenses than salespeople eventually
  need.
- **Rogue admins:** every day some people get GitHub/AWS admin "just for this
  deploy" or lose Slack by mistake. 5% of those watch events are lost.
- **Managers:** 80% approve and 12% deny, within 1–36 h. The rest never answer,
  and the chart expires the request after 72 h.
- **Memory:** every simulated hour, idle controllers are hibernated
  (`ta_system_cold_actors` + `ta_actor_hibernate`) into a temp file. Whatever
  needs one again, whether an app reply, an HR change or its own resync timer,
  produces a `TA_EFFECT_WAKE` and the host calls `ta_actor_wake`. The library never
  touches the file.

The last three days are quiet: no HR changes, drift or outages. Then the run
checks the invariants:

| Invariant | |
|---|---|
| nobody gets privileged access without approval | checked by the app at every grant |
| no record is finalized while access remains | checked when the finalizer reports |
| leavers lose access within 1 h of their end date | unless an app was down at the time; those are reported separately, and the controller raised an overdue alert |
| every active person converges to the policy | after the quiet days; a Salesforce license may be missing only while the pool is exhausted |
| every rogue change is corrected | including those whose watch event was lost: the next resync catches them |

## Results

Release build, 4-core orb, one thread:

```
30 simulated days, 10567 people ever employed
  HR: 10567 joiners, 432 leavers (53 legal holds), 964 movers, 12 rescinded; 337 records finalized
  apps: 118227 calls, 77229 failed (outages, errors, rate limits, licenses), 225 responses lost, 225 retries deduplicated by idempotency key
  drift: 907 rogue changes (49 watch events lost), slowest fix 23.6 h
  offboarding: slowest revocation 171 min after the end date, 7 overdue alerts
  approvals asked 1438; salesforce licenses 1995/2100
  memory: 10230 controllers, 559 hibernated; 309037 hibernations, 308478 wakes; region 8.9 MB, JS heap 46.0 MB

invariants:
  privileged access granted without approval                 0
  records finalized while access remained                    0
  leavers revoked later than 1 h                             0 (+7 during outages)
  people not converged after the quiet days                  0
  rogue changes not corrected                                0
  controller faults                                          0
all invariants hold
```

34 s wall clock, 79 MB peak RSS. Between events, essentially every controller
is hibernated (the daily report shows 100%). The daily resync wakes each one
once a day, which accounts for most of the ~300k wakes. The slowest drift fix
(23.6 h) is a lost watch event waiting for the next resync. That is the
resync's job. The 7 late revocations all fall in the AWS outage, and each
raised an overdue alert. Most failed calls are GitHub's rate limit while 6,000
initial grants squeeze through 120 requests/min.

## What the simulation caught

Each of these was a real bug in the first version of the chart or the host. A
deterministic simulation with invariants found all of them in minutes:

1. **Idempotency keys must change when the world changes.** With *(person, app,
   level, generation)* as the key, a rogue admin removing Slack made the repair
   reuse the original grant's key. The app deduplicated it, reported "none", and
   the controller looped. The fix: a per-app `rev`, bumped whenever the app
   changed behind the controller's back (watch, resync, an "ok" with the wrong
   level), plus backoff on that mismatch.
2. **SCXML event names match by prefix.** A transition on `observed` also took
   `observed.list`, so resync snapshots were applied as watch events for app
   `undefined`. Resync never repaired anything. Event names are now prefix-free
   (`changed`, `inventory`).
3. **A self-transition on a parallel region's child has the whole document as
   its domain.** `resync → resync` exited and re-entered *all* regions every day.
   That reset the lifecycle, and the offboarding timer was rescheduled from a
   stale clock, a day later each time. The self-transition now lives on an inner
   state.
4. **Retry-After without jitter is a thundering herd.** Every controller limited
   in the same minute retried in the same second. The jitter is a hash of the
   person, the app and the attempt, not `Math.random`, so runs replay exactly.

And one on the host side: a stale "left" event from a rescinded resignation
started offboarding early, which is why HR events now carry the end date they
belong to.
