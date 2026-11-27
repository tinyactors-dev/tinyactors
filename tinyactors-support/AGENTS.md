# AGENTS.md: tinyactors-support

The support channel of tinyactors for agents, served at
`mcp.tinyactors.dev` (a CNAME to `ta-mcp.exe.xyz`, DNS only, exe's
certificate; the VM's proxy is public) from the exe VM `ta-mcp`. Anyone reads skills, docs and known issues with their
workarounds; anyone signed in with GitHub reports issues, attaches
workarounds and files, and hears back when an issue is fixed. The root
`AGENTS.md` applies too.

## Layers

- **The charts decide** (`charts/`): which repositories and components are
  valid (pulled from GitHub), which reports are the same issue, when an
  issue is fixed or came back, who may triage, attach or read an
  attachment, what a login and a grant are. They make no I/O and read no
  clock: time is delayed sends, and an event's `at` comes from the gateway.
- **The I/O processors only do I/O** (`lib/processors/`): `github` (the
  App's REST client, or `FakeGitHub`), `ledger` (SQLite), `blobs` (the VM's
  disk), `scan`, `decide` (Jev, through TypeSafe's API, or unavailable), `spawn`
  (the package's `Spawner`). Every processor has a stand-in the tests use.
- **The gateway says who the caller is** (`lib/gateway.mjs`): it makes the
  secrets and hands actors only their hashes, asks the grant and the user
  who a token stands for, and passes the caller to the charts, which trust
  it.
- **The server is transport** (`lib/http.mjs`): MCP over Streamable HTTP,
  stateless; the OAuth authorization server; uploads and signed downloads.

## Rules

- **Actors:** `tracker` → `project.<repoID>` → `issue.<repoID>.<n>` →
  `upload.<repoID>.<n>.<k>`; `auth` → `login.<state>`, `user.<githubID>` →
  `grant.<id>`. A collection actor alone spawns its children's names,
  through the Spawner, and records its spawns in flight. Long-lived actors
  never end; `login` and `upload` do.
- **Charts on the native datamodel**, compiled with `warningsAsErrors`
  (`lib/charts.mjs`): a chart reads at most one helper file, and the
  compile puts `lib.js` in front of it (a helper sees only its own
  script's helpers). No top-level `var` in a helper file (a function
  returning the table instead), no helper named like a `<data>`, no
  `Date` (the native datamodel's is the process's clock, not the host's).
- **Identifiers:** an issue is `tinyactors:<project>:<n>`, `<project>`
  the slug the tracker gave the repository when it first saw it, for good.
  A commit fixes one with the trailer `Fixes-issue: <identifier>`.
- **Durability:** the snapshot (`DATA_DIR/host.snapshot`, a few seconds
  after each change and at SIGTERM) with the store of hibernated actors
  (`store.sqlite`), whose deletes wait for the next snapshot. The ledger is
  the journal, not the truth.
- **Tests:** `mise run test`: `test/scenario.test.mjs` (every behaviour
  through the gateway, a SimulatedClock, the stand-ins) and
  `test/http.test.mjs` (the server over HTTP). A new behaviour gets a step
  in one of them.
- **Deploying:** `mise run vm` (once), `mise run secrets`, `mise run
  deploy`, `mise run smoke`. The proxy stays private and no domain is
  added until the maintainer says so.
