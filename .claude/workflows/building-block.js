export const meta = {
  name: 'building-block',
  description: 'Implement one ring-protocol plan step in tinyactors-core, or deep-review one landed commit',
  whenToUse: 'One plan step per run: args {block: "4.2 commit 4", notes}. Leaves the change uncommitted. Review a landed commit with {reviewCommit: "<sha>", block}; the review runs read-only and reports findings for a follow-up fix commit, so the next step can be implemented at the same time.',
  phases: [
    { title: 'Brief', detail: 'a read-only pass maps call sites, tests and generated files' },
    { title: 'Implement', detail: 'block-implementer writes the change, its header docs and its tests' },
    { title: 'Review', detail: 'block-reviewer reviews the change in depth; findings go back to the implementer' },
  ],
}

const block = args && args.block
if (!block) throw new Error('args.block is required, for example {block: "4.2 commit 4"}')
const MAX_ROUNDS = (args && args.maxRounds) || 2

// The agent types are defined in .claude/agents/. A session loads them when
// it starts; a session that began before they existed passes
// {agentsFromFiles: true}, and the script runs general-purpose agents told to
// read their definition first.
const FROM_FILES = !!(args && args.agentsFromFiles)
function as(role, prompt) {
  return FROM_FILES
    ? {
        prompt: `Before anything else, read .claude/agents/block-${role}.md at the repository root (/Users/dhamidi/project/tinyactors-scratch). Everything below its front matter is your standing instructions; follow them exactly.\n\nYour task:\n${prompt}`,
        opts: { agentType: 'general-purpose' },
      }
    : { prompt, opts: { agentType: `block-${role}` } }
}
function run(role, prompt, opts) {
  const a = as(role, prompt)
  return agent(a.prompt, Object.assign({}, opts, a.opts))
}

const IMPL = {
  type: 'object',
  properties: {
    status: { type: 'string', enum: ['done', 'blocked'] },
    unit: { type: 'string' },
    files: { type: 'array', items: { type: 'string' } },
    design_decisions: { type: 'array', items: { type: 'string' } },
    deviations: { type: 'array', items: { type: 'string' } },
    test_program: { type: 'string' },
    test_claims: { type: 'array', items: { type: 'string' } },
    commands: {
      type: 'array',
      items: {
        type: 'object',
        properties: { command: { type: 'string' }, passed: { type: 'boolean' }, note: { type: 'string' } },
        required: ['command', 'passed'],
      },
    },
    disputed_findings: { type: 'array', items: { type: 'string' } },
    open_questions: { type: 'array', items: { type: 'string' } },
  },
  required: ['status', 'unit', 'files', 'design_decisions', 'test_program', 'commands', 'open_questions'],
}

const REVIEW = {
  type: 'object',
  properties: {
    verdict: { type: 'string', enum: ['pass', 'fix', 'escalate'] },
    findings: {
      type: 'array',
      items: {
        type: 'object',
        properties: {
          rule: { type: 'string' },
          severity: { type: 'string', enum: ['blocking', 'minor'] },
          file: { type: 'string' },
          line: { type: 'integer' },
          problem: { type: 'string' },
          fix: { type: 'string' },
        },
        required: ['rule', 'severity', 'problem'],
      },
    },
    summary: { type: 'string' },
  },
  required: ['verdict', 'findings', 'summary'],
}

const NOTES = args && args.notes ? `\n\nThe maintainer's notes for this run, part of your task:\n${args.notes}` : ''

// args.reviewCommit: review a commit already on main, read-only, while the
// next step is implemented in the working tree. Findings become a follow-up
// commit; nothing is sent back to an implementer here.
if (args && args.reviewCommit) {
  phase('Review')
  const review = await run('reviewer',
    `Review commit ${args.reviewCommit} (${block}), already on main. The working tree belongs to another implementer: do not build or run anything in it. To build or run tests, make a worktree of that commit under your scratchpad (git worktree add --detach <dir> ${args.reviewCommit}) and remove it when done.${NOTES}`,
    { schema: REVIEW, label: `review ${block}` },
  )
  return { block, commit: args.reviewCommit, outcome: review ? review.verdict : 'review-failed', review }
}

// args.fix: the maintainer's corrections to a step already in the working
// tree; the run starts with a fix round instead of implementing anew.
const FIX = args && args.fix

// A cheap read-only pass maps the change before the implementer starts, so
// the implementer does not rediscover call sites across four projects.
// args.brief === false skips it.
let BRIEF = ''
if (!FIX && !(args && args.brief === false)) {
  phase('Brief')
  const brief = await agent(
    `Map the code a change will touch, for the implementer who makes it. Do not design or edit anything. The change: ${block} of docs/proposals/ring-proto/IMPLEMENTATION.md (read its entry under "Step 4.2 in commits" or the step's row).${NOTES}\n\nReturn a compact brief, file:line throughout:\n1. The functions, types and fields the change replaces or extends, where each is defined.\n2. Every call site of each, grouped by project (tinyactors-core, tinyactors-host, tinyactors-otlp, tinyactors-test C and Ruby, tinyactors-napi src/lib/test), from git grep.\n3. The tests that cover them (core test programs, kernel tests, test host scenario files and names, napi suites), and which are likely to need changes.\n4. Generated files to regenerate (amalgamation, napi bindings) and the commands that do it.\n5. Anything surprising: a second code path doing the same thing, a test that pins the old behaviour.\nNo prose beyond what the implementer needs.`,
    { agentType: 'Explore', label: `brief ${block}`, phase: 'Brief' },
  )
  if (brief) BRIEF = `\n\nA read-only pass mapped the code for you. Verify what you rely on, but do not rediscover it:\n${brief}`
}

phase('Implement')
let impl = await run('implementer',
  FIX
    ? `${block} is already implemented in the working tree, uncommitted. Apply the maintainer's corrections below, rerun the commands and report as for an implementation. Change nothing else.\n${FIX}`
    : `Carry out ${block} of the ring protocol plan, with its header documentation and its tests, as your instructions describe. Leave everything uncommitted.${NOTES}${BRIEF}`,
  { schema: IMPL, label: FIX ? `correct ${block}` : `implement ${block}` },
)
if (!impl) throw new Error(`the implementer for ${block} returned nothing`)

// args.review === false: stop after the implementer; the maintainer commits
// and reviews the commit with {reviewCommit}.
if (args && args.review === false) return { block, outcome: 'implemented', implementer: impl }

const rounds = []
let review = null
for (let round = 1; round <= MAX_ROUNDS; round++) {
  phase('Review')
  review = await run('reviewer',
    `Review ${block}: the uncommitted change in the working tree.${NOTES}\n\nThe implementer's report:\n${JSON.stringify(impl, null, 2)}`,
    { schema: REVIEW, label: `review ${block} #${round}` },
  )
  if (!review) break
  rounds.push({ round, verdict: review.verdict, findings: review.findings })
  log(`${block} review ${round}: ${review.verdict} (${review.findings.filter(f => f.severity === 'blocking').length} blocking)`)
  if (review.verdict !== 'fix' || round === MAX_ROUNDS) break

  phase('Implement')
  const fixed = await run('implementer',
    `Fix these review findings for ${block} in the working tree, then rerun the commands and report again. Dispute a finding in disputed_findings only with a reason.${NOTES}\n${JSON.stringify(review.findings, null, 2)}`,
    { schema: IMPL, label: `fix ${block} #${round}` },
  )
  if (fixed) impl = fixed
}

const outcome = !review ? 'review-failed'
  : review.verdict === 'pass' ? 'ready-for-maintainer'
  : review.verdict === 'escalate' ? 'escalated'
  : 'unresolved-after-max-rounds'

return { block, outcome, implementer: impl, final_review: review, rounds }
