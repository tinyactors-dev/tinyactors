# ADR 0002: Changelogs, written by an agent from the commits and the API

**Status:** Accepted (2026-10-01). Extends ADR 0001: a release's notes are its changelog section, not a list of commit subjects.
**Date:** 2026-10-01

## Context

Each project is released on its own (ADR 0001, decision 3). The four 0.1.0
releases went out on 27 and 28 September. Since then `main` has moved a
long way: core, the host kernel and napi each have dozens of commits, and
five of them break callers of core or the kernel.

`Release.notes` built the release notes from the Conventional Commit
subjects since the last tag that touch a directory, keeping `feat`, `fix`
and `perf` and pasting each subject as it was. That loses what a reader of
a release needs:

- a `refactor!` commit, which can be the most dangerous change in the range,
  was dropped, and a `!` was never marked as breaking;
- subjects are written for the repository's history, in ADR vocabulary
  ("ADR 0089 §§4, 5, 8, 9"), and do not say what a change means to someone
  who upgrades;
- a commit scoped `core,host` belonged to whichever directory it touched;
- nothing said what to do to upgrade.

A person reads a changelog to decide whether to upgrade and what to change.
No one on this project writes prose for that by hand: an agent drafts it,
in a session, with the maintainer's attention at release time. The prose has
to be checkable, because an agent's summary can be fluent and wrong.

## Decisions

1. **Every component has a `CHANGELOG.md`** in its directory: core, host
   and napi first, and scxml and tinyxml by the same tooling. One section
   per release, newest first, `## [<version>] - <date>`, in the format
   below. A project's release notes (the GitHub release body) are its
   section for that version.

2. **A section has a written part and a generated part.**
   - **Summary** (written): one to three short paragraphs for someone
     deciding whether to upgrade: what the release is about, and why.
   - **Upgrading** (written): required when the release has a breaking commit
     or a removed or changed public symbol: what a caller changes.
   - **Generated**, below a marker line: *Breaking changes* (every commit
     with `!` or a `BREAKING CHANGE:` footer, whatever its type), *API
     changes* (public symbols removed, added, changed), *Changes* (`feat`
     as Added, `fix` as Fixed, `perf`, and `refactor` or `revert` as
     Changed), and *Internal* (every other commit, collapsed).
   Nothing below the marker is edited by hand: the check renders it again
   from git and fails on any difference.

3. **The evidence is computed, not remembered.** `mise run
   changelog:draft <component>` prints everything a release is written
   from and writes nothing:
   - the commits since the last release of the component, with subject,
     body, files and breaking marks;
   - the change in the public API between the last tag and `HEAD`;
   - the implementation notes of the ADRs the commits name;
   - the commits whose subject says too little to summarize;
   - the proposed version and why;
   - the previous summary, for voice.

4. **Commits belong to a component by scope, then by path.** A commit whose
   scopes name a component (`core`, `host` and `kernel`, `napi`, `scxml`,
   `tinyxml` and `txml`) belongs to exactly those. One that names none
   (`feat(test-host)`, `docs`, no scope) belongs to the components whose
   directory it touches. `feat(core,host)` is in both changelogs. A commit
   that changes nothing but changelogs is left out, so committing the
   changelog does not change the list; `release(...)` commits are left out.

5. **The public API comes from what the project already describes.** Core,
   scxml and tinyxml keep a clang-generated `include/*.api.json` in step
   with their header (`mise run api --check`): the delta is read from that file at the
   last tag and at `HEAD`. Every leaf except the docs counts, so a struct's
   size, alignment or field offset changing is a change to the ABI. Napi's
   is what `index.d.ts` exports, and the host's what its headers declare,
   both read with ast-grep. ast-grep's C parser is not used where an
   `api.json` exists: the core header's macro-annotated enums defeat it, and
   it then silently drops most declarations. A symbol that a
   `*_VERSION*` macro names is ignored. A removed or changed symbol is
   breaking for this purpose, whatever the commit said.

6. **The version is at least what the evidence says.** Below 1.0, a minor
   version changes the API or ABI and a patch does not (the header's rule):
   any breaking commit or any API delta gives a minor version, otherwise a
   patch. From 1.0, a breaking change is a major version, a feature or an
   added symbol a minor one. The proposal is a lower bound: the check fails
   a lower version, and a higher one is the maintainer's choice.

7. **An agent writes the prose, in a session, at release time, and the
   prose is checked.** The `release-changelog` skill
   (`.agents/skills/release-changelog/`) is the procedure. `mise run
   changelog:write <component> <version> --summary FILE [--upgrading FILE]`
   adds the generated part and runs `mise run changelog:check <component>
   [version]`, which fails unless:
   - the section exists, its version is higher than the last release's and
     not lower than the proposal;
   - the Summary is not empty, is at most 400 words, and holds no `TODO`,
     `FIXME` or `TBD`;
   - a release that breaks something has an *Upgrading* section that names
     every removed or changed public symbol;
   - the Summary or Upgrading cites, by hash, every breaking commit;
   - every hash the prose cites is a commit of the release range;
   - every identifier the prose puts in backticks appears in the evidence
     (the packet the agent was given, and the names of the API at both
     revisions);
   - the generated part equals what the repository renders now.
   A first release of a component has no range: it needs a summary and
   nothing else.

8. **A second agent reviews; the maintainer approves.** The skill has a fresh
   agent check every claim of the prose against the packet, and apply or
   dispute the findings. It then shows the maintainer the version, the prose,
   the API delta, the findings and the check, and stops. Nothing is
   committed, tagged or published before the maintainer says so.

9. **The release tasks require a passing section.** `Release.notes(prefix:,
   version:, title:)` and `Release.check_changelog!` fail a release unless
   `changelog:check` passes, in the check stage before anything is built, and
   `notes` returns the section without its heading and marker. The
   `summary:` of the first release is gone from `CRelease.release` and from
   the napi task. Development builds (`--dev`) have no changelog.

10. **The changelog is committed before the release**, as
    `docs(<component>): changelog for <version>`, because the release task
    needs a clean tree and releases from `main`. The next release's range
    starts at the release's tag.

## Considered and rejected

- **A fragment file per change**, written in the commit and compiled at
  release. It needs a person to remember to write one per commit, and a
  commit that forgets is invisible in the changelog. The commit log already
  records every change; the prose is what is missing, and it is written once,
  by the agent, from all of them.
- **Generated lists only**, as `Release.notes` did. It keeps the subjects'
  vocabulary and drops what matters.
- **A rolling summary, updated when a breaking commit lands.** Every such
  commit would start an agent, and the summary would churn. The release-time
  check that every `!` commit is cited gives the same protection.
- **Checking the prose by a second model only.** A model's judgement of a
  model's text misses what a hash lookup does not. The deterministic checks
  run first; the reviewer covers the claims no pattern can check.

## Consequences

- Commit subjects and scopes are input: a wrong scope puts a commit in the
  wrong changelog. A subject that says nothing is listed in the packet, and
  the agent reads its diff.
- The skill's use is the only way to get a release through: a release of a
  component with no section, or a stale one, fails in its check stage.
- Adding a component means adding it to `Changelog::COMPONENTS` with its
  scopes and where its public API is.
- `mise run changelog:test` (part of `mise run test`) builds a repository
  and runs the ledger, the API delta, the evidence, the document and every
  check's pass and failure.
