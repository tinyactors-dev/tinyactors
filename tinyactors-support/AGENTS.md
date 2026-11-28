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
  App's REST client, or `FakeGitHub`), `catalog` (SQLite: the issues and
  their ledger), `blobs` (the VM's
  disk), `scan`, `decide` (Jev, through TypeSafe's API, or unavailable), `spawn`
  (the package's `Spawner`). Every processor has a stand-in the tests use.
- **The gateway says who the caller is** (`lib/gateway.mjs`): it makes the
  secrets and hands actors only their hashes, asks the grant and the user
  who a token stands for, and passes the caller to the charts, which trust
  it.
- **The server is transport** (`lib/http.mjs`): MCP over Streamable HTTP,
  stateless; the OAuth authorization server; uploads and signed downloads.

## Skills over MCP

Decided by the maintainer (2026-11-27): implement the MCP Skills extension,
and keep the tools for hosts without it.

- **The extension** is `io.modelcontextprotocol/skills`, Beta
  ([specification](https://github.com/modelcontextprotocol/ext-skills/blob/main/specification/beta/skills.mdx),
  SEP-2640, written against base revision 2026-07-28). The server declares
  it in `initialize`'s capabilities (`extensions:
  {"io.modelcontextprotocol/skills": {"directoryRead": true}}`, beside
  `resources`): the pinned SDK (1.32.2) negotiates revisions up to
  2025-11-25 and has no `server/discover`, so `initialize` is where a
  client sees it. The SDK has no types for the extension, so `skills/list`,
  `skills/get` and `resources/directory/read` are custom request handlers
  (`lib/http.mjs`), as the extension's implementations list says servers
  on older SDKs register them; its TypeScript support is in SDK 2.4.0
  (`ext/skills`), a major version up. A request may carry the 2026-07-28
  revision's `params._meta` or none.
- **One manifest** (`lib/skills.mjs`), read once at startup: every file
  of each skill with its `sha256:` digest and size, SKILL.md's
  frontmatter, and the directory tree. The methods, `resources/list` and
  `resources/read`, and the tools `list_skills` and `get_skill` all serve
  from it, so a digest a host checks is the digest of the bytes every path
  gives. A skill is `skill://<name>/SKILL.md`; its files
  `skill://<name>/<path>`; its root and subdirectories are
  `inode/directory` resources. The frontmatter reader takes flat `key:
  value` lines and refuses anything else at startup, rather than serve a
  frontmatter that differs from the file's. Listings and entries carry
  `ttlMs` (an hour: skills change with a deploy) and `cacheScope:
  "public"` (no skill depends on who asks).
- **Hosts:** the [client matrix](https://modelcontextprotocol.io/extensions/client-matrix)
  (read 2026-11-27) lists the extension in Claude (web), Claude Code,
  VS Code GitHub Copilot and others, not in Claude Desktop; the tools
  serve the rest.
- **Tests:** `test/skills.test.mjs` checks every result against the
  extension's own JSON Schema (`test/fixtures/ext-skills-beta-schema.json`,
  `schema/beta/schema.json` at ext-skills `ba23984a`) and the rules a
  schema cannot: digests and sizes against the bytes `resources/read`
  gives, frontmatter against the file, names against URIs, atomic pages,
  `-32602` for what is not a skill or a directory, and the tools agreeing
  with the methods.

## MCP tools and the app

The maintainer's rules (2026-11-27), and what they rest on:

- **JSON goes only into structured output.** A result is its
  `structuredContent` and a line or two of text for people
  (`lib/tools.mjs`, `summarize`): what happened, the identifier, what to
  do next; never JSON. A document (`get_doc`, `get_skill`) is its own
  text, whole, so a client that reads only the text can read it (the
  maintainer, 2026-11-27). The MCP specification says a tool that returns structured content
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
  is embedded: the tokens' fallback stacks render.
  Its interaction follows an issue tracker's habits (Linear's, not its
  look; the maintainer, 2026-11-27): every issue read once
  (`known_issues`, state `all`) and filtered, searched, grouped (state,
  component, kind, none) and ordered (cost, reports, newest, recently
  updated, title) in the view; filters in a menu, shown as removable
  chips; ids without the leading `tinyactors:`; a side peek beside the
  list when it is wide, over it when narrow; the keyboard (j/k, Enter,
  Esc, x, ⇧-click, /, f, c, ?); multi-select with bulk triage, close
  and decline (on a phone, a select mode from More, where a tap selects);
  optimistic changes, and on a refusal the issue read again
  (`get_issue`) so the view shows the server's state, not its own stale
  copy; one message line under the header, about the last thing done:
  the next action takes a success or a note away, or 4 s do, and an
  error stays until its × or a new action, except while the form it
  came from is open; a kind told from a component (a sans pill with
  its glyph, against a monospace folder tag); on a phone, the filter
  menu in two levels (the properties, then one's values, searchable
  past eight), while a desktop keeps the one panel (the maintainer's
  review of the recorded frames, 2026-11-28); full screen only on the expand control
  (`ui/request-display-mode`). The default view is the open issues,
  grouped by state, ordered by cost; the filter menu's components are
  those of the repositories shown (a repository filter's, else those with
  issues in the list), not every repository's (the maintainer,
  2026-11-27). Its colours are the
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
- **Durability:** the catalog (`DATA_DIR/catalog.sqlite`) holds every
  issue in full and the ledger of every change; the snapshot
  (`DATA_DIR/host.snapshot`, a few seconds after each change and at
  SIGTERM) with the store of hibernated actors (`store.sqlite`, whose
  deletes wait for the next snapshot) holds the actors. A lost snapshot
  is rebuilt from the catalog (`mise run rebuild`); see "The catalog".
- **Tests:** `test/skills.test.mjs` checks every result against the
  extension's own JSON Schema (`test/fixtures/ext-skills-beta-schema.json`,
  `schema/beta/schema.json` at ext-skills `ba23984a`) and the rules a
  schema cannot: digests and sizes against the bytes `resources/read`
  gives, frontmatter against the file, names against URIs, atomic pages,
  `-32602` for what is not a skill or a directory, and the tools agreeing
  with the methods.

## MCP tools and the app

The maintainer's rules (2026-11-27), and what they rest on:

- **JSON goes only into structured output.** A result is its
  `structuredContent` and a line or two of text for people
  (`lib/tools.mjs`, `summarize`): what happened, the identifier, what to
  do next; never JSON. A document (`get_doc`, `get_skill`) is its own
  text, whole, so a client that reads only the text can read it (the
  maintainer, 2026-11-27). The MCP specification says a tool that returns structured content
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
  is embedded: the tokens' fallback stacks render.
  Its interaction follows an issue tracker's habits (Linear's, not its
  look; the maintainer, 2026-11-27): every issue read once
  (`known_issues`, state `all`) and filtered, searched, grouped (state,
  component, kind, none) and ordered (cost, reports, newest, recently
  updated, title) in the view; filters in a menu, shown as removable
  chips; ids without the leading `tinyactors:`; a side peek beside the
  list when it is wide, over it when narrow; the keyboard (j/k, Enter,
  Esc, x, ⇧-click, /, f, c, ?); multi-select with bulk triage, close
  and decline (on a phone, a select mode from More, where a tap selects);
  optimistic changes, and on a refusal the issue read again
  (`get_issue`) so the view shows the server's state, not its own stale
  copy; one message line under the header, about the last thing done:
  the next action takes a success or a note away, or 4 s do, and an
  error stays until its × or a new action, except while the form it
  came from is open; a kind told from a component (a sans pill with
  its glyph, against a monospace folder tag); on a phone, the filter
  menu in two levels (the properties, then one's values, searchable
  past eight), while a desktop keeps the one panel (the maintainer's
  review of the recorded frames, 2026-11-28); full screen only on the expand control
  (`ui/request-display-mode`). The default view is the open issues,
  grouped by state, ordered by cost; the filter menu's components are
  those of the repositories shown (a repository filter's, else those with
  issues in the list), not every repository's (the maintainer,
  2026-11-27). Its colours are the
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

## The catalog

Decided by the maintainer (2026-11-28: "make the issue data live in a db
too, the actor holds a subset it needs for decision making, and the full
issue catalog is in one DB so we can use sqlite3 full text search").

- **One database, one transaction.** `catalog.sqlite` holds the ledger
  (every change, with the wall clock's time) and the catalog's rows
  (`issues`, `reports`, `attachments`, `projects`, and `issues_fts`, FTS5
  over title, body and workaround with porter stemming). The `catalog`
  processor appends a change and applies it in one `BEGIN IMMEDIATE`
  transaction, and the issue answers only at `ledger.written`, so a
  reader never sees an answer the catalog does not hold, and the rows are
  a projection of the ledger (`rebuild()` replays it). Considered: the
  ledger and the catalog as two files (two writes that can disagree).
- **What an actor keeps** is what it decides with. An issue: its state,
  title (for its reporters' news), count (for answers), reporters (whom to
  tell, who may attach and download), reason, fix (the compare), files'
  keys (signing, the purge) and `purged`. A project: its next number, its
  issues' states and fingerprints, the deleted numbers, and the last ten
  issues it filed (`fresh`: candidates the catalog may not have yet, as
  the issue writes its filing a moment after the spawn). The tracker:
  its inventory, which it also writes to the catalog after each pull.
- **Reads wake no actor:** `known_issues` (with `query`, full text,
  best match first by bm25: an additive input, 2026-11-28), `get_issue`
  (who may see what is the gateway's: anyone the issue, a signed-in
  caller its occurrences, its reporters and maintainers its files'
  descriptions), the repositories' names, a counted report's workaround
  and an attachment's description. The app keeps filtering the list it
  loads in the view (its search is the list's text, as before; the server
  search is the tool's).
- **The decision model's candidates** come from the catalog
  (`catalog.candidates`: the report's words, quoted, OR-ed, against the
  repository's open issues, ordered by bm25 with the report's component
  counting twice, twenty at most), merged with `fresh`; the chart still
  decides what the model's answer means.
- **Recovery:** without a snapshot (`REBUILD=1`, `mise run rebuild`) the
  tracker loads its inventory from the catalog and spawns its projects
  under their slugs, and each project loads its issues and spawns them
  with `restored`, each in the state the catalog gives, writing nothing.
  Users, grants and logins are not in it: people sign in again. Rebuilt
  timers (autoclose, purge) start from the rebuild.
- **The migration** (`lib/migrate.mjs`), on the first start with a
  catalog: the host is restored on the old charts, the old issue actors'
  variables are read (`actor.context`), the old `ledger.sqlite` is
  replayed with its times, each issue gets one `migrated` record of what
  its actor held (what the old ledger lacked: a report's minutes and
  reporter, the files), an issue the old ledger filed and no actor held is
  recorded as deleted, and only then do the actors move to the new charts
  (`support.upgrade()`). Once: the catalog's `meta.migrated` mark; the old
  file is renamed `ledger.sqlite.imported`. The `ledger` processor type
  stays an alias of `catalog` for any actor left on an older chart.
