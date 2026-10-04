export const meta = {
  name: 'ring-proto-wave',
  description: 'One wave of docs/proposals/ring-proto/IMPLEMENTATION.md: plan, implement and review C units in parallel, integrate onto ring-proto one at a time, then stop for human approval',
  whenToUse: 'Run once per wave of the ring-proto plan. Pass args {wave, units:[{id,title,brief,full_test?}], max_rounds?}. Read the returned report and approve before running the next wave.',
  phases: [
    { title: 'Plan', detail: 'one planner per unit: lifetimes, allocation, measurements' },
    { title: 'Implement', detail: 'one implementer per unit, each in its own worktree' },
    { title: 'Review', detail: 'lifetimes and allocation (Sonnet), docs and repo rules (Haiku)' },
    { title: 'Fix', detail: 'address blocking findings, then review again' },
    { title: 'Integrate', detail: 'one at a time: rebase onto ring-proto, mise run test, push' },
    { title: 'Report', detail: 'wave summary and the proposed next wave, for human approval' },
  ],
}

// ---------------------------------------------------------------------------
// Arguments
//
//   wave        a label such as "1a"
//   units       [{ id: "1.4", title: "A2 layout builder", brief: "...", full_test: false }]
//   max_rounds  review rounds per unit before it is handed to a human (default 2)
//   integrate   false to stop after review and leave the wip branches unmerged
//
// The workflow never starts a second wave by itself. The human checkpoint is
// the gap between runs: read the report, inspect the pushed commits, then run
// the next wave with the units you approved.
// ---------------------------------------------------------------------------

const WAVE = (args && args.wave) || 'unnamed'
const UNITS = (args && args.units) || []
const MAX_ROUNDS = (args && args.max_rounds) || 2
const INTEGRATE = !(args && args.integrate === false)
const REPO = '/home/user/tinyactors'
const BASE = 'ring-proto'

if (!UNITS.length) throw new Error('args.units is empty: pass the units of this wave')

const wip = u => `wip/ring-proto/${u.id}`

const ENV = `
Environment: before any mise command run
  export PATH="$HOME/.local/bin:$PATH" LANG=C.UTF-8 LC_ALL=C.UTF-8; mise trust -a >/dev/null 2>&1
The machine has 4 CPUs shared with other agents: run the narrowest test that covers your change
(a single core test program after \`mise run build\` in tinyactors-core, or \`mise run asan\` there after
changing src/). Do not run the repository-wide \`mise run test\`; the integrator does that.`

const CONTEXT = `
You are working on the ring-proto experiment in ${REPO} (git branch ${BASE}, a deliberate parallel
experiment; the AGENTS.md "work on main" rule is suspended for it, everything else in AGENTS.md and
tinyactors-core/AGENTS.md applies). The plan is docs/proposals/ring-proto/IMPLEMENTATION.md; block
names (A2, B3, D5, E2, ...) and their contracts are in docs/proposals/ring-proto/reference/building-blocks.md,
whose "Lifetimes come first" table is binding.

What the maintainer cares about, in order:
1. Respect for lifetimes. Every block belongs to exactly one lifetime (system, definition, session,
   envelope, input, parked, macrostep/unit, record, call). Say which, and make the code honour it:
   who owns each allocation, when it is freed, what may hold a pointer or index into it and for how long.
2. Allocation rules. No malloc/free or stdlib; everything goes through ta_allocator / ta_mem_*. State
   when a function may allocate (only at the safe points the catalog names) and when it never does.
   Sized frees match their allocation. Checked arithmetic for every size computation. Failure paths
   leave no partial state.
3. Reference-level documentation in the code, Go style: every unit (a .h with its .c) opens with a big
   comment that explains the block as a package doc would: what it is for, its contract, its lifetime,
   when it allocates, its invariants, complexity, failure behaviour, and a short usage example. Every
   declared function has a comment that begins with its name and says what it does, what it needs and
   what it guarantees. Explain why, not just what.
4. A measured approach: small steps, each verified; record numbers (lines of core before/after, test
   results) rather than claims; change nothing the unit does not need; do not refactor code a later
   phase deletes (timer.c, the event blocks, io.c / io_origin.c, the kernel's containers).
${ENV}`

// ---------------------------------------------------------------------------
// Schemas
// ---------------------------------------------------------------------------

const PLAN = {
  type: 'object',
  properties: {
    summary: { type: 'string' },
    new_files: { type: 'array', items: { type: 'string' } },
    touched_files: { type: 'array', items: { type: 'string' } },
    lifetimes: { type: 'array', items: { type: 'object', properties: {
      thing: { type: 'string' }, lifetime: { type: 'string' }, allocates_when: { type: 'string' },
      freed_by: { type: 'string' } }, required: ['thing', 'lifetime', 'allocates_when', 'freed_by'] } },
    api: { type: 'string', description: 'the declarations the unit adds or changes, as C' },
    replaces: { type: 'array', items: { type: 'string' }, description: 'duplicates or code removed, with file:line' },
    callers_to_migrate: { type: 'array', items: { type: 'string' } },
    tests: { type: 'array', items: { type: 'string' } },
    steps: { type: 'array', items: { type: 'string' }, description: 'small, separately verifiable steps' },
    risks: { type: 'array', items: { type: 'string' } },
    full_test_needed: { type: 'boolean', description: 'touches allocation, pause/resume, paging, trace writing or per-actor costs' },
  },
  required: ['summary', 'new_files', 'touched_files', 'lifetimes', 'api', 'replaces', 'tests', 'steps', 'risks', 'full_test_needed'],
}

const IMPL = {
  type: 'object',
  properties: {
    branch: { type: 'string' },
    commits: { type: 'array', items: { type: 'object', properties: {
      sha: { type: 'string' }, subject: { type: 'string' } }, required: ['sha', 'subject'] } },
    tests_run: { type: 'array', items: { type: 'object', properties: {
      command: { type: 'string' }, passed: { type: 'boolean' }, note: { type: 'string' } }, required: ['command', 'passed'] } },
    core_lines_before: { type: 'integer' },
    core_lines_after: { type: 'integer' },
    deviations_from_plan: { type: 'string' },
    open_questions: { type: 'array', items: { type: 'string' } },
  },
  required: ['branch', 'commits', 'tests_run', 'core_lines_before', 'core_lines_after', 'deviations_from_plan', 'open_questions'],
}

const FINDINGS = {
  type: 'object',
  properties: {
    findings: { type: 'array', items: { type: 'object', properties: {
      severity: { type: 'string', enum: ['blocking', 'nit'] },
      file: { type: 'string' }, line: { type: 'integer' },
      issue: { type: 'string' }, fix: { type: 'string' } },
      required: ['severity', 'file', 'issue', 'fix'] } },
  },
  required: ['findings'],
}

const FIX = {
  type: 'object',
  properties: {
    commits: { type: 'array', items: { type: 'string' } },
    addressed: { type: 'array', items: { type: 'string' } },
    declined: { type: 'array', items: { type: 'object', properties: {
      issue: { type: 'string' }, why: { type: 'string' } }, required: ['issue', 'why'] } },
    tests_passed: { type: 'boolean' },
  },
  required: ['commits', 'addressed', 'declined', 'tests_passed'],
}

const INTEGRATION = {
  type: 'object',
  properties: {
    status: { type: 'string', enum: ['pushed', 'conflict', 'tests_failed', 'push_failed'] },
    head_sha: { type: 'string' },
    pushed_commits: { type: 'array', items: { type: 'string' } },
    test_summary: { type: 'string', description: 'the step-time table and anything that failed' },
    full_test: { type: 'string', description: 'result of mise run test:full, or "not run"' },
    notes: { type: 'string' },
  },
  required: ['status', 'head_sha', 'pushed_commits', 'test_summary', 'full_test', 'notes'],
}

const REPORT = {
  type: 'object',
  properties: {
    markdown: { type: 'string', description: 'the wave report for the maintainer' },
    next_wave: { type: 'array', items: { type: 'object', properties: {
      id: { type: 'string' }, title: { type: 'string' }, brief: { type: 'string' },
      full_test: { type: 'boolean' } }, required: ['id', 'title', 'brief'] } },
  },
  required: ['markdown', 'next_wave'],
}

// ---------------------------------------------------------------------------
// Review lenses. The lifetime lens needs judgement; the other two are
// checklists, which a smaller model does well.
// ---------------------------------------------------------------------------

const LENSES = [
  {
    key: 'lifetimes', model: 'sonnet',
    prompt: `Review for lifetimes and allocation. For every allocation, pointer and index the diff
introduces or moves: which lifetime from the catalog's table owns it, who frees it, with the same
size and alignment, and whether anything can outlive it (a pointer into a page that moves, an index
used after give-back, a reference kept past reset). Flag any allocation outside a safe point, any
size computation without checked arithmetic, any failure path that leaves partial state or leaks,
recursion (ADR 0060), and any behaviour change in a refactor that claims none. Read the code paths,
not just the diff hunks. A finding is blocking when it is a real defect or a broken rule.`,
  },
  {
    key: 'docs', model: 'haiku',
    prompt: `Review the documentation only. Each new or changed unit (.h and its .c) must open with a
Go-style package comment: purpose, contract, lifetime, when it allocates, invariants, complexity,
failure behaviour, a short usage example. Every declared function must have a comment that starts
with its name and says what it does, needs and guarantees. Flag comments that say what instead of why,
that are stale or wrong against the code, or that cite files and lines that no longer exist. Missing
unit docs or missing function docs are blocking; wording is a nit.`,
  },
  {
    key: 'rules', model: 'haiku',
    prompt: `Review against the repository's rules, as a checklist: no malloc/free or stdlib headers in
core (stdint, stddef, stdbool are fine); at most three parameters per function; no recursion; acronyms
all caps or all lowercase; public API only in include/tinyactors.h with TA_API; include/tinyactors.c
regenerated, not hand-edited; APIs changed in place with every caller migrated, no _v2 / _ex /
compatibility shims; nothing left unused; the duplicates the unit claims to replace are actually gone;
new test programs are listed in the core test task's sources; commit subjects scoped like
refactor(core): with a body that says what a caller does if behaviour or API changed. Broken rules are
blocking; style is a nit.`,
  },
]

// ---------------------------------------------------------------------------
// Per-unit stages
// ---------------------------------------------------------------------------

function unitHeader(u) {
  return `Unit ${u.id}: ${u.title}\nBrief: ${u.brief}\nWork branch for this unit: ${wip(u)} (based on ${BASE}).`
}

function planUnit(u) {
  return agent(`${CONTEXT}

${unitHeader(u)}

Plan this unit; do not edit files. Read the plan row, the catalog entries for its blocks, and every
source file the unit touches. Find the duplicates it replaces with git grep and cite them as file:line.
Give each thing the unit creates a lifetime from the catalog's table, and say when it allocates and who
frees it. Propose the API as C declarations that follow tinyactors-core's conventions (snake_case,
ta_ prefix, params structs when more than three parameters). Break the work into small steps that each
build and can be tested on their own. Other units of this wave run at the same time: ${UNITS.map(x => x.id + ' ' + x.title).join('; ')}.
Name any file you expect to share with them as a risk.`,
    { label: `plan:${u.id}`, phase: 'Plan', schema: PLAN, model: 'sonnet' })
}

function implementUnit(plan, u) {
  return agent(`${CONTEXT}

${unitHeader(u)}

The approved plan:
${JSON.stringify(plan, null, 2)}

Implement it in this worktree:
1. git checkout -b ${wip(u)}
2. Record core's size first: cat tinyactors-core/src/*.c tinyactors-core/src/*.h | wc -l
3. Work through the plan's steps in order. After each, build and run the narrowest test that covers it.
   Commit each working step separately; keep history linear. Commit subjects are scoped
   (refactor(core): ..., test(core): ...), and a body says what a caller does when an API changes. End
   every commit message with these two lines:
     Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
     Claude-Session: https://claude.ai/code/session_01WbAzYQ22M3bPB3TrDc1XzX
4. Write the Go-style unit comment and the per-function comments as you write the code, not after.
5. Run mise run amalgamate after changing any header or source in core, and mise run api if you
   changed include/tinyactors.h. Run mise run check-acronyms.
6. Record core's size again.
7. Leave the worktree clean, then run: git checkout --detach   (so that later stages can check out ${wip(u)}).
If the plan turns out wrong, deviate as little as needed and say so in deviations_from_plan. Do not push.`,
    { label: `impl:${u.id}`, phase: 'Implement', schema: IMPL, model: 'sonnet', isolation: 'worktree' })
}

function reviewUnit(plan, impl, u, round) {
  return parallel(LENSES.map(lens => () => agent(`${CONTEXT}

${unitHeader(u)}

You are a reviewer (round ${round}). Do not edit files. From ${REPO}, read the change with
  git log --oneline ${BASE}..${wip(u)}
  git diff ${BASE}...${wip(u)}
and read whole files at that revision with git show ${wip(u)}:<path>.

The plan it implements:
${JSON.stringify(plan, null, 2)}

${lens.prompt}

Report each finding with file and line at ${wip(u)}, what is wrong, and the fix. Report only what you
have checked against the code. An empty list is a fine answer.`,
    { label: `review:${lens.key}:${u.id}#${round}`, phase: 'Review', schema: FINDINGS, model: lens.model })
    .then(r => r && r.findings.map(f => ({ ...f, lens: lens.key })))))
    .then(rs => rs.filter(Boolean).flat())
}

function fixUnit(plan, findings, u, round) {
  return agent(`${CONTEXT}

${unitHeader(u)}

Reviewers found the following in round ${round}:
${JSON.stringify(findings, null, 2)}

In this worktree: git checkout ${wip(u)}. Verify each finding against the code first. Fix every
blocking finding that is real, and every nit that is plainly right. Decline the rest with the reason.
Build, run the narrowest covering tests, run mise run amalgamate if core changed, and commit in small
scoped commits with the same two trailer lines as before:
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01WbAzYQ22M3bPB3TrDc1XzX
Leave the worktree clean and finish with: git checkout --detach. Do not push.`,
    { label: `fix:${u.id}#${round}`, phase: 'Fix', schema: FIX, model: 'sonnet', isolation: 'worktree' })
}

// Integration is serialized: each unit lands on top of the previous one, so
// ring-proto stays linear and every pushed commit has passed mise run test.
let gate = Promise.resolve()
function integrateUnit(u, plan) {
  const fullTest = !!(u.full_test || plan.full_test_needed)
  const run = () => agent(`${CONTEXT.replace(/Do not run the repository-wide[^\n]*/, '')}

${unitHeader(u)}

You integrate this unit, in the main checkout ${REPO} (branch ${BASE}). Nobody else writes there now.
1. git status must be clean and the branch ${BASE}. git pull --rebase origin ${BASE}.
2. Rebase the unit onto it: git checkout ${wip(u)} && git rebase ${BASE}. Resolve conflicts only when
   the resolution is mechanical (both sides kept, nothing lost); otherwise git rebase --abort and
   report status "conflict" with the files and why.
3. Back on ${BASE}: git merge --ff-only ${wip(u)}. Never create a merge commit.
4. Run mise run test (it runs what changed and prints a step-time table).${fullTest ? `
   This unit touches allocation or per-actor costs: also run mise run test:full, and if it passes,
   amend nothing; say in your notes that it passed so the report can cite it.` : ''}
5. If a test fails: find whether the unit caused it. If a fix is small and clearly within the unit,
   make it as a separate scoped commit (same trailer lines as the unit's commits) and re-run. Otherwise
   git reset --hard to the commit before the unit (ORIG_HEAD of the merge) and report "tests_failed"
   with the failing output.
6. Push: git push -u origin ${BASE}. On a network failure retry up to 4 times, waiting 2, 4, 8, 16 s.
   Never force-push.
7. Delete the local branch ${wip(u)}.`,
    { label: `integrate:${u.id}`, phase: 'Integrate', schema: INTEGRATION, model: 'sonnet' })
  const p = gate.then(run, run)
  gate = p.catch(() => null)
  return p
}

// ---------------------------------------------------------------------------
// The wave: every unit runs its own pipeline; integration is the only point
// where they wait for each other.
// ---------------------------------------------------------------------------

log(`wave ${WAVE}: ${UNITS.map(u => u.id).join(', ')}; up to ${MAX_ROUNDS} review rounds each`)

const results = await pipeline(
  UNITS,
  u => planUnit(u),
  (plan, u) => implementUnit(plan, u).then(impl => ({ plan, impl })),
  async ({ plan, impl }, u) => {
    const rounds = []
    let open = []
    for (let round = 1; round <= MAX_ROUNDS; round++) {
      const findings = await reviewUnit(plan, impl, u, round)
      const blocking = findings.filter(f => f.severity === 'blocking')
      const fix = findings.length ? await fixUnit(plan, findings, u, round) : null
      rounds.push({ round, findings, fix })
      open = blocking
      if (!blocking.length) { open = []; break }
      if (round === MAX_ROUNDS) {
        // The last fix was not reviewed: one more look, blocking findings only.
        const last = (await reviewUnit(plan, impl, u, round + 1)).filter(f => f.severity === 'blocking')
        rounds.push({ round: round + 1, findings: last, fix: null })
        open = last
      }
    }
    log(`${u.id}: ${open.length ? open.length + ' blocking finding(s) left for a human' : 'review clean'}`)
    return { plan, impl, rounds, open }
  },
  async (r, u) => {
    if (!INTEGRATE) return { ...r, integration: { status: 'skipped' } }
    if (r.open.length) return { ...r, integration: { status: 'held', notes: 'blocking findings remain' } }
    const integration = await integrateUnit(u, r.plan)
    log(`${u.id}: ${integration ? integration.status : 'integration agent failed'}`)
    return { ...r, integration }
  },
)

phase('Report')
const summary = UNITS.map((u, i) => ({ unit: u, result: results[i] }))
const report = await agent(`${CONTEXT}

You write the report for wave ${WAVE} of the ring-proto plan. The maintainer reads it before approving
the next wave, so it must make inspection quick and honest. From ${REPO}, check the facts against git
(git log --oneline origin/${BASE}, git show --stat) rather than trusting the data below.

Data from the wave (null means a stage failed):
${JSON.stringify(summary, null, 2)}

Write markdown with: a table of units (status, commits pushed, core lines before/after, tests, full
test); for each unit, the lifetimes and allocation decisions in two or three lines, what it replaced,
review findings that were declined and why, anything still open; what a human should look at first
(file:line); and the risks for the next wave. Then propose the next wave from IMPLEMENTATION.md: units
that can run in parallel without editing the same files, each with a brief that names the blocks, the
files and the duplicates to replace. Prefer a smaller wave to a conflicting one. Mark full_test for
units that touch allocation, pause/resume, paging, trace writing or per-actor costs.`,
  { label: `report:${WAVE}`, phase: 'Report', schema: REPORT, model: 'sonnet' })

return { wave: WAVE, results: summary, report }
