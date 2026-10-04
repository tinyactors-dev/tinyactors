export const meta = {
  name: 'building-block',
  description: 'Implement one ring-protocol building block in tinyactors-core, spot-check it, and stop for the maintainer',
  whenToUse: 'Phase 2 of docs/proposals/ring-proto/IMPLEMENTATION.md, one block per run: args {block: "B1"}. Leaves changes uncommitted for the maintainer to approve.',
  phases: [
    { title: 'Implement', detail: 'block-implementer writes the unit, its header docs and its test' },
    { title: 'Review', detail: 'block-reviewer spot-checks the diff; findings go back to the implementer' },
  ],
}

const block = args && args.block
if (!block) throw new Error('args.block is required, for example {block: "B1"}')
const MAX_ROUNDS = (args && args.maxRounds) || 3

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
        },
        required: ['rule', 'severity', 'problem'],
      },
    },
    test_binary_passed: { type: ['boolean', 'null'] },
    summary: { type: 'string' },
  },
  required: ['verdict', 'findings', 'summary'],
}

phase('Implement')
let impl = await run('implementer',
  `Implement building block ${block} of the ring protocol plan in tinyactors-core, with its header documentation and its own test program, as your instructions describe. Leave everything uncommitted.`,
  { schema: IMPL, label: `implement ${block}` },
)
if (!impl) throw new Error(`the implementer for ${block} returned nothing`)

const rounds = []
let review = null
for (let round = 1; round <= MAX_ROUNDS; round++) {
  phase('Review')
  review = await run('reviewer',
    `Spot-check building block ${block}. The implementer's report:\n${JSON.stringify(impl, null, 2)}`,
    { model: 'haiku', effort: 'low', schema: REVIEW, label: `review ${block} #${round}` },
  )
  if (!review) break
  rounds.push({ round, verdict: review.verdict, findings: review.findings })
  log(`${block} review ${round}: ${review.verdict} (${review.findings.filter(f => f.severity === 'blocking').length} blocking)`)
  if (review.verdict !== 'fix' || round === MAX_ROUNDS) break

  phase('Implement')
  const fixed = await run('implementer',
    `Fix these review findings for building block ${block} in the working tree, then rerun the commands and report again. Dispute a finding in disputed_findings only with a reason.\n${JSON.stringify(review.findings, null, 2)}`,
    { schema: IMPL, label: `fix ${block} #${round}` },
  )
  if (fixed) impl = fixed
}

const outcome = !review ? 'review-failed'
  : review.verdict === 'pass' ? 'ready-for-maintainer'
  : review.verdict === 'escalate' ? 'escalated'
  : 'unresolved-after-max-rounds'

return { block, outcome, implementer: impl, final_review: review, rounds }
