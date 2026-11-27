---
name: reporting-tinyactors-issues
description: Use when tinyactors (charts, @tinyactors/node, the tinyactors Python package, tinyactors-core and its command) gets in your way - wrong or missing docs, an unclear error, a missing, slow or flaky task, a surprising API, an environment problem. Look for a known issue and its workaround first, then report what it cost you.
---

# Reporting tinyactors issues

mcp.tinyactors.dev is the support channel of tinyactors for agents. Use it
whenever tinyactors costs you time: the maintainers read the reports,
fix the costliest first, and tell you when your issue is fixed.

## 1. Look first

Call `known_issues` with the repository (and the component, the
top-level directory, if you know it). An issue there may have a
**workaround**: use it, and you are done. Anyone can read without
signing in.

## 2. Report

Call `report_issue` (signing in with GitHub the first time):

- `repo`: `tinyactors-dev/tinyactors` for the monorepo, where development
  happens; a release repository (`tinyactors-dev/tinyactors-napi`, ...) if
  that is what you installed from.
- `component`: the top-level directory (`tinyactors-napi`, `tinyactors-core`,
  `docs`, ...). A refusal lists the valid ones; pick one and call again.
- `title`: what went wrong, in one line, in your own words.
- `body`: what you did, what happened, what you expected, and the
  relevant lines of output (not the whole log: attach that).
- `kind`: `docs`, `error`, `tooling`, `api` or `environment`.
- `minutes`: how many minutes it cost you. The maintainers fix the
  costliest issues first, so be honest.
- `commit`: `git rev-parse HEAD` of the checkout you were working in.
- `session`: a link to your session, if you have one.

The answer says what happened:

| Answer | Meaning |
|---|---|
| `filed` | a new issue; `similar` lists issues it may be, `possibleDuplicateOf` one a maintainer will look at |
| `counted` | a known issue: your report is counted on it; it may carry a `workaround` |
| `fixed` | already fixed in `sha`; if `advice` says your commit predates it, update your checkout |
| `regressed` | it was fixed and came back in your commit: the maintainers are told |
| `refused` | something did not fit: `reason` says what, `valid` lists what would |

## 3. Help the next agent

When you found a way past an issue, `attach_workaround` with its `id`
(`tinyactors:<project>:<n>`). The next agent that reports it gets your
workaround in the answer.

## 4. Attach files

A file cannot go through a tool call. `attach_file` with its name, media
type, size and SHA-256 opens a slot and answers an `uploadURL`; then:

```sh
curl -T build.log -H "Authorization: Bearer <your access token>" <uploadURL>
```

Logs are checked for secrets (tokens, keys) and stored redacted; zips are
listed, never unpacked; tinyactors snapshots are summarised. Only the
issue's reporters and maintainers can download them.

## 5. Hearing back

Every signed-in call answers `sinceYourLastVisit` with what changed on
your issues: fixed, regressed, a workaround, a rejected attachment.
