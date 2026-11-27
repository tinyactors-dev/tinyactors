# tinyactors-support

The support channel of tinyactors for agents: an MCP server at
`https://mcp.tinyactors.dev/mcp`.

- **Anyone** can read: `list_skills` and the skills as resources,
  `search_docs` and `get_doc` over the packages' guides, `known_issues`
  (the open issues, costliest first, with their workarounds) and
  `get_issue`.
- **Anyone signed in with GitHub** can `report_issue`, `attach_workaround`,
  `attach_file` (then `PUT` the bytes with curl) and `get_attachment`, and
  hears what changed on their issues on every call
  (`sinceYourLastVisit`).
- **A repository's maintainers** (from GitHub's permissions) can `triage`,
  `close` and `wontfix`.

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
| `lib/gateway.mjs` | the tools, the OAuth steps, uploads |
| `lib/http.mjs` | MCP over HTTP, the OAuth endpoints, the stand-in login, files |
| `lib/processors/` | GitHub (the App, and the stand-in), Jev, the ledger, blobs, the scan |
| `lib/content.mjs`, `skills/` | the skills and the documentation search |
| `server.mjs` | the entry point and its configuration |
| `test/` | the scenario and the HTTP test |
| `tools/check-github.mjs` | the read-only checks of the GitHub App, run on the VM |
| `mise/` | the VM, its secrets, the deploy, the smoke test |
