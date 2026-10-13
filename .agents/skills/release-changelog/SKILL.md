---
name: release-changelog
description: Write the changelog section for a release of tinyactors-core, tinyactors-napi, tinyactors-scxml, staticjs or tinyxml, and stop for the maintainer's review before the release. Use when asked to cut or prepare a release, write or update a CHANGELOG.md, summarize what changed since the last release, or when a release task fails on "is not fit to release". Releases only; do not use it to draft a changelog between releases.
---

# Release changelog

A release of a component needs a section in its `CHANGELOG.md`
(`docs/adr/0002-changelogs.md`). You write the **Summary** and the
**Upgrading** text; the tooling writes everything else (the commit lists and
the API changes) and checks your prose against the repository. The release
task refuses to run without a section that passes `changelog:check`, and
takes its release notes from that section.

Draft at release time only. Do not write or refresh a summary between
releases.

## Steps

Work in the order below. Components: `core`, `host`, `napi`, `scxml`,
`staticjs`, `tinyxml`.

1. **Scope.** Ask which component and, if not said, whether the maintainer
   has a version in mind. The working tree of the repository must be on
   `main` and clean for the component's directory, or the release task will
   refuse later.

2. **Read the evidence, and nothing else by default.**

   ```
   mise run changelog:draft <component>
   ```

   It prints the last release, the proposed version and why, every commit
   since (subject, body, files), the change in the public API (removed,
   added, changed, with the changed lines), the implementation notes of the
   ADRs the commits name, the commits whose subject says little, and the
   previous summary. Write from this. Read a diff or a file only for a commit
   the packet flags as saying little, or to settle a doubt the packet leaves;
   say in your report what you read beyond it.

   If the component has no earlier release, the packet says so and lists no
   commits: write a summary of what the component is and offers, nothing
   about its history.

3. **Choose the version.** The packet proposes the lowest version the release
   may have: below 1.0 a minor version changes the API or ABI and a patch
   does not. State the version and the reasons, and confirm it with the
   maintainer. `changelog:check` fails a version lower than the proposal.

4. **Write the Summary and the Upgrading text** into two files in the
   scratchpad (plain Markdown, no headings: the tool adds them).

   *Summary*: one to three short paragraphs for someone deciding whether to
   upgrade and what to expect. Say what the release is **about**: the two or
   three themes, in the order they matter to a user, and why each one
   happened. Do not list commits; the generated lists do that. Name the
   consequences a reader can act on: new capabilities, changed behavior,
   cost (memory, speed) only where the packet gives figures. Keep to about
   150 words, and never above 400.

   *Upgrading*: required when the release has a breaking commit or a removed
   or changed public symbol, otherwise leave it out. A short list, one item
   per thing a caller must change: what was removed or changed (by the name
   in the API delta), what replaces it, what to do. Every removed or changed
   symbol in the packet's API delta must be named here, even if only as "removed,
   no replacement". Say what a stored artifact does (a checkpoint, a log, a
   URI) when the change makes old ones unreadable.

   Rules the checks enforce, and the reasons:
   - Cite the commit, by its seven-character hash in parentheses, for every
     breaking commit. A hash you cite must be a commit of the range.
   - Put an identifier in backticks only if it appears in the packet
     (a commit, the API delta, an ADR note). A function, option or type the
     packet does not show is how a summary goes wrong. If you need a name the
     packet lacks, find it in the repository, and then cite the commit that
     has it, or leave the backticks off.
   - No `TODO`, `FIXME` or `TBD`.

   Rules the checks cannot enforce, and you must:
   - Claim only what the packet supports. If the evidence is thin, say the
     summary is thin; do not fill the gap.
   - Explain in a user's words, not the ADR's. ADR numbers are references,
     not explanations: one sentence of what the decision does beats "see ADR
     0094".
   - A benchmark or size figure comes from a commit or an ADR note, with its
     meaning (what was measured), or it stays out.
   - A declaration marked `// status: unimplemented` in `index.d.ts` is a
     design target, not something a user can call: do not present it as
     released.

5. **Write the section.**

   ```
   mise run changelog:write <component> <version> --summary FILE [--upgrading FILE]
   ```

   It writes `<component dir>/CHANGELOG.md`: your prose, then the lists it
   generates, and then runs `changelog:check`. On a failure it names what to
   fix; edit the files and run it again. Never edit below the
   `<!-- Generated ... -->` line; `changelog:check` renders it again and
   fails when it differs.

6. **Independent review.** Start a fresh agent (the Agent tool, type
   `general-purpose`) that has not seen your reasoning. Give it the output of
   `mise run changelog:draft <component>` and the two prose files, and this
   instruction:

   > You review a changelog summary against the evidence it was written
   > from. List every claim in the Summary and the Upgrading text that the
   > evidence does not support or contradicts: a wrong name, a wrong
   > direction (added vs removed), a figure that is not in the evidence, a
   > theme the commits do not show, a breaking change the text omits or
   > understates. Quote the claim and say what the evidence shows instead.
   > Do not rewrite the text. If every claim is supported, say so.

   Apply each supported finding. If you disagree with one, tell the
   maintainer and why, instead of dropping it. Run `changelog:write` again
   after the edits.

7. **Stop for the maintainer.** Show, in this order: the proposed version
   and its reasons; the Summary and Upgrading text as written; the API
   delta's removed and changed lines; the reviewer's findings and what you
   did about each; the output of `changelog:check`. Then wait. Do not commit,
   tag or publish before the maintainer approves.

8. **After approval.** Commit only the component's `CHANGELOG.md`, as
   `docs(<component>): changelog for <version>` on `main`. A commit that
   touches nothing but changelogs is left out of the generated lists, so this
   commit does not make the section stale. Then the release task, which runs
   `changelog:check` first and puts the section in the GitHub release:

   - core, scxml, staticjs, tinyxml: `mise run //<directory>:release -- <version>`
   - napi: `mise run //tinyactors-napi:release -- <version>`

   Without `--publish` these are dry runs; show what they report, and let the
   maintainer decide on `--publish`. The host has no release task yet.

## When a check fails

| Message | What it means | What to do |
|---|---|---|
| `the generated part of the section is stale or was edited` | A commit for the component landed since `changelog:write`, or the lists were edited | Run `changelog:write` again; read the new commits in the packet and revise the summary |
| `the breaking commit … is not cited` | A `!` commit's hash is in neither text | Name what changed and cite the hash |
| `Upgrading does not mention … ` | A removed or changed symbol is unnamed | Add it to Upgrading |
| `… is not in the evidence` | A backticked identifier the packet lacks | Verify it; cite a commit that has it, or drop the backticks |
| `is lower than the proposed` | The version understates the change | Raise it, or ask the maintainer why not |

## Notes

- The packet lists commits by path and by scope: a commit naming a component
  in its scope belongs to it, and one that names none belongs to the
  component whose directory it touches. A `feat(core,napi)` commit is in both
  changelogs. Wrong scopes in commit subjects give a wrong packet; say so in
  your report when you see one.
- The packet's API delta for core, scxml, staticjs and tinyxml comes from the
  checked-in `include/*.api.json` at the last release and at `HEAD`; for napi
  from the declarations `index.d.ts` exports; for the host from its headers.
  Struct layout changes (size, offset) count as changes: they break the ABI.
- This skill does not change the release tasks' other checks, builds or
  publishing.
