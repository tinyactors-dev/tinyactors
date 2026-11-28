# tinyactors-support

The support channel of tinyactors for agents: an MCP server at
`https://mcp.tinyactors.dev/mcp`. Its sibling projects are
[staticjs](https://github.com/tinyactors-dev/staticjs), a pure, total
subset of JavaScript for programs someone else writes, and
[sxml](https://github.com/tinyactors-dev/sxml), an XML toolchain (XPath,
XQuery, XSLT, XSD) in portable C.

- **Anyone** can read the skills, through the MCP Skills extension
  (`skills/list`, `skills/get`, each file a `skill://` resource) or, for
  hosts without it, the tools `list_skills` and `get_skill` (with `path`, a
  skill's references): the skills
  `using-tinyactors` (how to build with tinyactors) and
  `reporting-tinyactors-issues`,
  `search_docs` and `get_doc` over the packages' guides, `known_issues`
  (the open issues, costliest first, with their workarounds) and
  `get_issue`.
- **Anyone signed in with GitHub** can `report_issue`, `attach_workaround`,
  `attach_file` (then `PUT` the bytes with curl) and `get_attachment`, and
  hears what changed on their issues on every call
  (`sinceYourLastVisit`).
- **A repository's maintainers** (from GitHub's permissions) can `triage`,
  `close`, `wontfix` and `delete_issue`.

Every tool answers with structured content that its output schema
describes, and a line of text. Where the host renders MCP Apps (Claude,
ChatGPT, VS Code), `known_issues` and `get_issue` open the issue list:
filter by repository, component and state, open an issue, report one,
attach a workaround, and, for a maintainer, triage, decline, close and
delete.

Reports of the same problem are one issue: an exact match by component
and title, otherwise Jev (a decision model) picks among the open issues
it could be, and the chart accepts its pick only when it is confident. A
commit with `Fixes-issue: tinyactors:<project>:<n>` fixes an issue; a
later report from a commit that contains the fix reopens it as regressed.

## Run it here

```sh
npm ci
node server.mjs          # http://localhost:8000/mcp, the stand-in GitHub and login, data in ./data
mise run test
```

Without the GitHub App's four variables (`GITHUB_APP_ID`,
`GITHUB_APP_CLIENT_ID`, `GITHUB_APP_CLIENT_SECRET`,
`GITHUB_APP_PRIVATE_KEY`) the server runs on the stand-in GitHub: three
fake repositories, and a login page where you pick who you are. Without
`TYPESAFE_API_KEY` (1Password: `console.typesafe.ai API Key`), a report that matches no issue exactly is filed as
new. `server.mjs` lists every variable.

## Connecting

```sh
claude mcp add --transport http tinyactors-support https://mcp.tinyactors.dev/mcp
```

On claude.ai: Settings → Connectors → Add custom connector, with the URL
`https://mcp.tinyactors.dev/mcp`. A tool that needs a signed-in caller
starts the login (OAuth, through the GitHub App) the first time.

The domain is a CNAME to `ta-mcp.exe.xyz` in Cloudflare, **DNS only**
(exe issues the certificate; a proxied record breaks it), added on exe
with `domain add ta-mcp mcp.tinyactors.dev`, and the VM's proxy is public.
`mise run secrets` writes the server's `PUBLIC_URL` (`mise/support.rb`).

## What is where

| Path | What it holds |
|---|---|
| `charts/` | the eight charts and their helpers |
| `lib/host.mjs` | the host: the charts, the processors, the store, snapshots and restore, `request` (an ask) |
| `lib/gateway.mjs` | what each tool does, the OAuth steps, uploads |
| `lib/tools.mjs` | the tools: input and output schemas, and the line of text for each result |
| `lib/app/` | the issue list app: its source, and `issues.html`, which `tools/build-app.mjs` builds with the design system's tokens |
| `lib/http.mjs` | MCP over HTTP, the OAuth endpoints, the stand-in login, files |
| `lib/processors/` | GitHub (the App, and the stand-in), Jev, the catalog, blobs, the scan |
| `lib/processors/catalog.mjs` | the catalog (`catalog.sqlite`): every issue in full, its reports and files, full text search, and the ledger it is a projection of |
| `lib/migrate.mjs` | the one-time move from the ledger of before to the catalog |
| `lib/skills.mjs`, `skills/` | the skills (each a `SKILL.md` and its `references/`) as one manifest: digests, sizes, frontmatter, directories |
| `lib/content.mjs` | the skills for the tools, and the documentation search |
| `tools/check-skill-samples.mjs` | runs every code sample of the skills (`--python` runs the Python ones too) |
| `server.mjs` | the entry point and its configuration |
| `test/` | the scenario, the tools' typed results, the catalog and its recovery, the HTTP test, the app in Chromium |
| `tools/check-github.mjs` | the read-only checks of the GitHub App, run on the VM |
| `mise/` | the VM, its secrets, the deploy, the smoke test |

## Where the issues are, and recovery

Every issue is in the catalog, `DATA_DIR/catalog.sqlite`: its title, body,
reports, workaround, fix and files, with the ledger of every change, both
written in one transaction before the change is answered. Readers
(`known_issues`, with `query` for full text search, `get_issue`, the app)
read it and wake no actor. The actors keep what their decisions need (an
issue its state, title, reporters, fix and files' keys; a project its
issues' states and fingerprints), in the snapshot (`host.snapshot`) and
the store of hibernated actors (`store.sqlite`).

- **The snapshot is lost or unreadable:** `mise run rebuild` stops the
  service, sets the snapshot and the store aside (renamed, never deleted)
  and starts it with `REBUILD=1`: the tracker, its projects and their
  issues are spawned again from the catalog, under their identifiers,
  numbers going on. Logins, users and grants are not in the catalog:
  people sign in again. Timers start again from the rebuild (a fixed
  issue's fortnight before it closes, a closed one's 90 days before its
  files go).
- **The catalog's rows are wrong or from an older schema:** they are a
  projection of its ledger; `Catalog#rebuild()` replays the ledger into
  them.
- **The catalog is lost:** the backups (below) have it.
