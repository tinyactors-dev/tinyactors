// Three starting audiences: who reads www.tinyactors.dev. Drawn from how
// three neighbours write for their readers (2026-11-23): rivet.dev (actors
// inside your backend, density numbers with methodology, a FAQ that names
// competitors, code first), sqlite.org (a "when to use" checklist that
// states its limits) and temporal.io (durable workflows, "replace your
// brittle state machines", maturity and failure guarantees).
//
// They are put in the store once, on the first start that has none of
// them, and are the reviewer's from then on: edited, chosen or deleted like
// any other. None is active until the reviewer chooses it.

export const STARTER_AUDIENCES = Object.freeze([
  {
    id: 'starter-agent-platform',
    name: 'Agent-platform engineer',
    description: 'A senior backend engineer building a multi-tenant backend for AI agents, chats or realtime sessions, in Node.js or Bun, evaluating a runtime for one long-lived stateful unit per session or tenant.',
    knows: 'Actor systems and Cloudflare Durable Objects; Rivet; XState; containers and serverless limits. Little SCXML.',
    wants: 'Whether it runs inside the backend they already have; memory per actor and how many fit in one process, with the benchmark behind the number; hibernation and snapshots for idle sessions; a code example before any concept.',
    tasks: 'Evaluate in minutes, npm install, run the first chart, compare with Rivet and Durable Objects.',
    objections: '"Another runtime or platform to operate?" "Why SCXML and not XState?" "Is anyone running this in production?" "Will an agent really write good charts?"',
  },
  {
    id: 'starter-workflow-tester',
    name: 'Workflow developer who tests',
    description: 'A backend developer, often in Python, whose long-running business logic (retries, timeouts, payments, multi-day waits) is hard to test and has been hand-rolled as state machines or considered for Temporal.',
    knows: 'Temporal or Celery and their replay or retry models; pytest; the pain of sleeping in tests and of flaky timing.',
    wants: 'A simulated clock that runs a week of timeouts in milliseconds; the whole flow written down as states and transitions; effects as events their code performs, so tests need no mocks of the world; no server to run.',
    tasks: 'Write one workflow as a chart, test its timeouts in CI, show the team the speedup.',
    objections: '"Does it need Docker or a cluster like Temporal?" "What survives a crash of the process?" "Will the simulated run match production?"',
  },
  {
    id: 'starter-tech-lead',
    name: 'Tech lead who approves dependencies',
    description: 'An architect or tech lead deciding whether the team may depend on tinyactors across services, clients and devices (Node.js, Python, the browser, C), and accountable when it fails.',
    knows: 'Reasons like sqlite.org\'s "when to use" page: what it is for, what it is not for. Knows SQLite, embedded libraries, and the cost of a new server.',
    wants: 'An honest "use it when / do not use it when"; the failure semantics of one process (what is lost, what a snapshot or migration keeps); one chart running the same everywhere; the license, the release history, the test suite that backs "passes all".',
    tasks: 'Decide yes or no, write the case for the team, check the license and portability.',
    objections: '"One process is one point of failure." "What happens to state when the host dies?" "Are we locked in to this runtime or its chart format?" "Who maintains it?"',
  },
].map(Object.freeze));
