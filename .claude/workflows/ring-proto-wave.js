export const meta = {
  name: 'ring-proto-wave',
  description: 'One wave of docs/proposals/ring-proto/IMPLEMENTATION.md: plan, implement and review C units in parallel, integrate onto ring-proto one at a time, then stop for human approval',
  whenToUse: 'Run once per wave of the ring-proto plan. Pass args {wave, prewave, units:[{id,title,brief,full_test?}], max_rounds?}. Read the returned report and approve before running the next wave.',
  phases: [
    { title: 'Plan', detail: 'one planner per unit that needs design (units[].plan !== false)' },
    { title: 'Implement', detail: 'one implementer per unit, each in its own worktree' },
    { title: 'Review', detail: 'lifetimes and allocation, conventions (both Sonnet); blocking findings verified before any fix' },
    { title: 'Fix', detail: 'confirmed findings only; a second look at the fix diff only for confirmed lifetime defects' },
    { title: 'Integrate', detail: 'one at a time: rebase, mise run test (test:full for full_test units) as the gate, then push' },
  ],
}

// ---------------------------------------------------------------------------
// Arguments
//
//   wave        a label such as "1a"
//   units       [{ id: "1.4", title: "A2 layout builder", brief: "...", full_test: false }]
//   max_rounds  review rounds per unit before it is handed to a human (default 2;
//               a second round runs only when the first confirmed a lifetime defect)
//   units[].plan false to skip the planner when the ADRs already fix the design
//   integrate   false to stop after review and leave the wip branches unmerged
//   prewave     the commit ring-proto was at before the wave, for timing baselines
//   units[].existing  true when the unit's wip branch already holds its implementation
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
const PREWAVE = (args && args.prewave) || 'the commit before the wave (ask git: the parent of its first unit commit)'

if (!UNITS.length) throw new Error('args.units is empty: pass the units of this wave')

const wip = u => `wip/ring-proto/${u.id}`

const ENV = `
Environment: before any mise command run
  export PATH="$HOME/.local/bin:$PATH" LANG=C.UTF-8 LC_ALL=C.UTF-8; mise trust -a >/dev/null 2>&1
The machine has 4 CPUs shared with other agents: run the narrowest test that covers your change
(a single core test program after \`mise run build\` in tinyactors-core, or \`mise run asan\` there after
changing src/). Run the repository-wide \`mise run test\` (incremental: it runs only the projects
whose sources changed) once, before your last commit. Never run \`mise run test:full\`: the integrator
runs it once per unit, as the gate before pushing.`

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
Core's test programs are registered by a first-line marker, not by lists: a new tests/x_test.c starts
with /* test: runtime asan */ (or standalone, with or without asan; mise/lib/test_programs.rb in
tinyactors-core explains them). Never add test names to the build, test or asan tasks.

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
    key: 'conventions', model: 'sonnet',
    prompt: `Review documentation and the repository's rules together.
Blocking: a new or substantially changed unit (.h with its .c) without its Go-style package comment
(purpose, contract, lifetime, when it allocates, invariants, complexity, failure behaviour, a short
example); a comment that is wrong against the code; malloc/free or stdlib headers in core; more than
three parameters; recursion; the public API outside include/tinyactors.h or without TA_API; a hand
edit of include/tinyactors.c; a second function, shim or alias beside one that should have changed in
place; code the unit left unused; a duplicate the unit claims to replace that is still there; a breaking
change whose commit body does not say what a caller does.
Not findings at all: a definition in a .c whose comment is "/* name: see file.h. */" (a function is
documented once, at its declaration); several related declarations sharing one comment that names
them; acronyms, recursion, freestanding builds and portability, which mise run test checks mechanically.
Everything else (wording, a thin function comment, a missing example) is a nit.`,
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

Implement it in this worktree (your current directory). Never cd into ${REPO} itself and never run git
-C ${REPO}: the main checkout belongs to the integrator, and changing its branch breaks its test run.
1. git checkout -B ${wip(u)} ${BASE}   (start from the current tip of ${BASE}, whatever this worktree was created at)
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

function reviewUnit(plan, impl, u, round, lifetimesOnly) {
  const lenses = lifetimesOnly ? LENSES.filter(l => l.key === 'lifetimes') : LENSES
  return parallel(lenses.map(lens => () => agent(`${CONTEXT}

${unitHeader(u)}

You are a reviewer (round ${round}${round > 1 ? ': the earlier rounds were fixed; review the fix commits
made after the first review, and the code paths they touch' : ''}). Do not edit files. From ${REPO}, read the change with
  git log --oneline ${BASE}..${wip(u)}
  git diff ${BASE}...${wip(u)}
and read whole files at that revision with git show ${wip(u)}:<path>.

The plan it implements:
${JSON.stringify(plan, null, 2)}

${lens.prompt}

Report each finding with file and line at ${wip(u)}, what is wrong, and the fix. Report only what you
have checked against the code. An empty list is a fine answer. Commit trailers (Co-Authored-By,
Claude-Session) are set by the maintainer: they are never a finding.`,
    { label: `review:${lens.key}:${u.id}#${round}`, phase: 'Review', schema: FINDINGS, model: lens.model })
    .then(r => r && r.findings.map(f => ({ ...f, lens: lens.key })))))
    .then(rs => rs.filter(Boolean).flat())
}

function fixUnit(plan, findings, u, round) {
  return agent(`${CONTEXT}

${unitHeader(u)}

Reviewers found the following in round ${round}. Blocking ones were confirmed against the code by a
second reader; nits were not checked:
${JSON.stringify(findings, null, 2)}

In this worktree (your current directory; never cd into ${REPO} or run git -C ${REPO}, which the
integrator owns): git checkout ${wip(u)}, and before every commit confirm that
git rev-parse --abbrev-ref HEAD prints ${wip(u)}: a commit on a detached HEAD is lost when the branch
is integrated (this happened in wave 1a-fix). Verify each finding against the code first. Fix every
blocking finding (they are confirmed), and every nit that is plainly right. In \`declined\` list only
blocking findings you could not fix, with their issue text copied verbatim: each holds the unit. Nits
you skip go in \`addressed\` as "skipped nit: <issue>: <why>".
Build, run the narrowest covering tests, run mise run amalgamate if core changed, and commit in small
scoped commits with the same two trailer lines as before:
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01WbAzYQ22M3bPB3TrDc1XzX
Before detaching, check that git log -1 --format=%h ${wip(u)} is your last commit, and list your
commits' hashes in the answer. Leave the worktree clean and finish with: git checkout --detach. Do not push.`,
    { label: `fix:${u.id}#${round}`, phase: 'Fix', schema: FIX, model: 'sonnet', isolation: 'worktree' })
}

const VERDICTS = {
  type: 'object',
  properties: {
    verdicts: { type: 'array', items: { type: 'object', properties: {
      index: { type: 'integer' }, real: { type: 'boolean' }, why: { type: 'string' } },
      required: ['index', 'real', 'why'] } },
  },
  required: ['verdicts'],
}

function verifyFindings(findings, u) {
  return agent(`${CONTEXT}

${unitHeader(u)}

A reviewer marked these findings blocking. Only confirmed ones go to the fixer, and a confirmed one
still open after fixing holds the unit from integration. Do not edit files. For each one, check it against the code at ${wip(u)}
(git show ${wip(u)}:<path>, and how the file is built: which headers a test can see, what the build
links). A finding is real only if it is a defect or a broken repository rule AND fixing it as described
is possible. Say why in one or two sentences either way.

${findings.map((f, i) => `${i}. [${f.lens}] ${f.file}:${f.line || ''} ${f.issue}\n   fix proposed: ${f.fix}`).join('\n')}`,
    { label: `verify:${u.id}`, phase: 'Review', schema: VERDICTS, model: 'sonnet' })
    .then(r => {
      if (!r) return findings
      const real = new Set(r.verdicts.filter(v => v.real).map(v => v.index))
      r.verdicts.filter(v => !v.real).forEach(v => log(`${u.id}: finding ${v.index} refuted: ${v.why}`))
      return findings.filter((_, i) => real.has(i))
    })
}

// Integration is serialized: each unit lands on top of the previous one, so
// ring-proto stays linear and every pushed commit has passed mise run test.
let gate = Promise.resolve()
function integrateUnit(u) {
  const run = () => agent(`${CONTEXT.replace(/Do not run the repository-wide[^\n]*/, '')}

${unitHeader(u)}

You integrate this unit, in the main checkout ${REPO} (branch ${BASE}). Nobody else writes there now.
1. git status must be clean and the branch ${BASE}. git pull --rebase origin ${BASE}.
2. Rebase the unit onto it: git checkout ${wip(u)} && git rebase ${BASE}. Resolve conflicts only when
   the resolution is mechanical (both sides kept, nothing lost); otherwise git rebase --abort and
   report status "conflict" with the files and why.
3. Back on ${BASE}: git merge --ff-only ${wip(u)}. Never create a merge commit.
4. The gate: ${u.full_test ? `run mise run test:full (it includes everything mise run test runs; it can
   exceed 30 minutes). In tinyactors-napi run bun install first if node_modules is missing.` : 'run mise run test (it runs what changed and prints a step-time table).'}
   Run it in the foreground with a long timeout, redirect its output to a log file and read its exit
   status (never pipe it through tail or grep), and do not answer before it has exited. If the only
   failure is a timing-ratio assertion in tinyactors-napi/test/scale.js, rerun that one script once;
   a second failure is real.
5. If a test fails: find whether the unit caused it. If a fix is small and clearly within the unit,
   make it as a separate scoped commit (same trailer lines as the unit's commits) and re-run. Otherwise
   git reset --hard to the commit before the unit (ORIG_HEAD of the merge) and report "tests_failed"
   with the failing output.
6. Push: git push -u origin ${BASE}. On a network failure retry up to 4 times, waiting 2, 4, 8, 16 s.
   Never force-push.
7. Delete the local branch ${wip(u)}, and remove the worktrees under .claude/worktrees/ whose HEAD is
   a commit of this unit (git worktree remove --force, one command per Bash call).`,
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
  // A unit marked `existing` already has its implementation on its wip branch
  // (a wave interrupted by a container restart): it skips planning and
  // implementation and resumes at review, against the branch as it stands.
  u => u.existing ? { summary: `Existing implementation on ${wip(u)}. Brief: ${u.brief}`, full_test_needed: !!u.full_test }
    : u.plan === false ? { summary: `No separate plan: the brief and the ADRs it names fix the design. Plan as you go, in small verified steps. Brief: ${u.brief}`, full_test_needed: !!u.full_test }
    : planUnit(u),
  (plan, u) => (u.existing
    ? Promise.resolve({ branch: wip(u), commits: [], tests_run: [], deviations_from_plan: 'resumed from the existing branch', open_questions: [] })
    : implementUnit(plan, u)).then(impl => ({ plan, impl })),
  async ({ plan, impl }, u) => {
    // Findings are verified before anyone fixes them: in wave 4e a whole fix
    // round went to findings a checklist reviewer got wrong. A second round
    // runs only for a confirmed lifetime defect, and looks at the fix diff.
    const rounds = []
    let open = []
    for (let round = 1; round <= MAX_ROUNDS; round++) {
      const findings = await reviewUnit(plan, impl, u, round)
      const raw = findings.filter(f => f.severity === 'blocking')
      const blocking = raw.length ? await verifyFindings(raw, u) : []
      const nits = findings.filter(f => f.severity !== 'blocking')
      const toFix = [...blocking, ...nits]
      const fix = toFix.length ? await fixUnit(plan, toFix, u, round) : null
      rounds.push({ round, findings, confirmed: blocking, fix })
      open = !blocking.length ? []
        : (!fix || !fix.tests_passed) ? blocking
        : fix.declined.map(d => ({ severity: 'blocking', lens: 'fix', file: '', issue: d.issue, fix: d.why }))
      if (!blocking.some(f => f.lens === 'lifetimes')) break
      if (round === MAX_ROUNDS) {
        const last = (await reviewUnit(plan, impl, u, round + 1, true)).filter(f => f.severity === 'blocking')
        rounds.push({ round: round + 1, findings: last, fix: null })
        open = last.length ? await verifyFindings(last, u) : []
      }
    }
    log(`${u.id}: ${open.length ? open.length + ' blocking finding(s) left for a human' : 'review clean'}`)
    return { plan, impl, rounds, open }
  },
  async (r, u) => {
    if (!INTEGRATE) return { ...r, integration: { status: 'skipped' } }
    if (r.open.length) return { ...r, integration: { status: 'held', notes: 'blocking findings remain' } }
    const integration = await integrateUnit(u)
    log(`${u.id}: ${integration ? integration.status : 'integration agent failed'}`)
    return { ...r, integration }
  },
)

// No separate full pass and no report agent: the integrator's gate is the
// full pass for full_test units, and the maintainer reads the journal.
const summary = { units: UNITS.map((u, i) => ({ unit: u.id, result: results[i] })) }
return { wave: WAVE, results: summary }
