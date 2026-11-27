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

## MCP tools and the app

The maintainer's rules (2026-11-27), and what they rest on:

- **JSON goes only into structured output.** A result is its
  `structuredContent` and a line or two of text for people
  (`lib/tools.mjs`, `summarize`): what happened, the identifier, what to
  do next; never JSON. A document (`get_doc`, `get_skill`) is its own
  text. The MCP specification says a tool that returns structured content
  SHOULD also return its serialized JSON in a text block "for backwards
  compatibility" ([2025-11-25, server/tools, "Structured
  Content"](https://modelcontextprotocol.io/specification/2025-11-25/server/tools#structured-content));
  the rule takes the SHOULD's exception, so a client that reads only
  `content` sees the line, not the data.
- **Every tool has an `outputSchema`**, an object schema (the SDK's
  `ToolSchema` requires `type: "object"`, `@modelcontextprotocol/sdk`
  1.32.2 `types.js:1252`), and every answer kind conforms: filed, counted,
  fixed, regressed, refused, failed, and an `error` for a call that never
  reached the tracker. The SDK's client validates `structuredContent`
  against it (`client/index.js:485-500`); `test/stack.mjs` checks every
  result with the same validator (`@modelcontextprotocol/sdk/validation/ajv`).
- **Data that reads better as a view gets an MCP App**
  ([the extension's specification, 2026-01-26](https://github.com/modelcontextprotocol/ext-apps/blob/main/specification/2026-01-26/apps.mdx);
  `@modelcontextprotocol/ext-apps` 2.0.4, whose wire protocol is that of
  1.x). The issue list is `ui://tinyactors-support/issues`
  (`text/html;profile=mcp-app`), linked from `known_issues` and
  `get_issue` by `_meta.ui.resourceUri`; a host without apps ignores it
  and shows the text. The server marks the tools for every client: it is
  stateless and does not see an `initialize`'s
  `io.modelcontextprotocol/ui` capability when it answers `tools/list`
  (the specification's SHOULD check, which the graceful degradation
  covers).
- **The app** (`lib/app/source/`, built into one file,
  `lib/app/issues.html`, by `tools/build-app.mjs`; `mise run test` fails
  when it is out of date) speaks JSON-RPC over `postMessage` itself:
  `ui/initialize`, then `ui/notifications/initialized`, the host's
  `tool-input` and `tool-result`, `host-context-changed`, and
  `size-changed` from a ResizeObserver. Every operation is a `tools/call`
  of the server's own tools through the host, as the signed-in user; it
  shows a maintainer's actions only to a maintainer of the issue's
  repository (`whoami`, `list_projects`), and the charts refuse them to
  anyone else anyway. It loads nothing (an empty CSP: the host's
  restrictive default, `connect-src 'none'`, no `font-src`), so no font
  is embedded: the tokens' fallback stacks render. Its colours are the
  tinyactors design system's tokens, light or dark from the host's
  `theme`, `prefers-color-scheme` when the host gives none; the host's
  style variables are not used.
- **Hosts:** Claude (claude.ai and the desktop app) renders MCP Apps for a
  custom connector backed by a remote server
  ([Get started with MCP Apps](https://claude.com/docs/connectors/building/mcp-apps/getting-started)),
  as do ChatGPT, VS Code and Goose (the extension's README). Claude Code's
  terminal shows the text (unverified: no page says it renders apps).

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
  through the gateway, a SimulatedClock, the stand-ins),
  `test/tools.test.mjs` (every tool's answer kinds, the maintainer's
  actions in every state, deleting), `test/http.test.mjs` (the server
  over HTTP) and `test/app.test.mjs` (the issue list in Chromium under a
  fake host). Every tool result a test sees is checked against its output
  schema and its text (`test/stack.mjs`, `checked`). A new behaviour gets
  a step in one of them.
- **Deploying:** `mise run vm` (once), `mise run secrets`, `mise run
  deploy`, `mise run smoke`. The proxy stays private and no domain is
  added until the maintainer says so.
