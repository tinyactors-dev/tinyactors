# ADR 0016: A release repository per component

**Status:** Accepted (2026-10-15). Amends
[ADR 0001](0001-packaging-and-releases.md), decision 3 (where a project's
release is published) and decision 1 (GitHub hosts more than one
repository's releases).
**Date:** 2026-10-15

## Whom it serves first, and who pays

Following [docs/roles.md](../roles.md):

- **First: whoever installs one component**: the C embedder who downloads
  tinyxml or core, the application developer who runs tinyparse's
  `install.sh`, and the staticjs site that serves the latest staticjs.
  Today one releases page mixes six components, GitHub's single "Latest"
  is whichever released last, and nothing outside the repository can ask
  for "the latest tinyxml": `install.sh` asks for `releases/latest` and
  gets cpython's, and the site's release task lists every release and
  filters by tag prefix.
- **Then: the maintainer who releases**, who wants each component's
  releases on a page of their own.
- **Out of scope:** splitting development. Changing an API in place and
  migrating every caller in the same change (`AGENTS.md`) needs one
  repository, and so does every change across layers. Issues and pull
  requests stay in the monorepo too.
- **Who pays:** the release code in `mise/lib` (a snapshot commit and a
  push per release, a task to copy or finish a release) and one GitHub
  repository per component. Nothing per event, per actor, or in any
  binding's runtime.

## Context

The repository moved to the `tinyactors-dev` organization on 2026-10-15.
Each component is released on its own, tagged `<directory>/v<version>`
(ADR 0001, decision 3), and its GitHub release is made in the monorepo.
Eight releases sit on its page: tinyactors-core 0.1.0 and 0.2.0,
tinyactors-napi 0.1.0 and 0.2.0, tinyxml 0.1.0, tinyactors-scxml 0.1.0
(whose directory is gone), staticjs 0.9.0 and tinyactors-cpython 0.1.0.

The changelogs (`changelog:draft`, `changelog:check`; ADR 0002), the next
version (`Release.released_versions`) and the development versions
(`Release.dev_version`) read the prefixed tags in the monorepo.

## Decision

1. **Development stays in one repository.** The monorepo keeps its
   prefixed tags (`tinyxml/v0.2.0`) and remains the source of truth.
2. **Each component's GitHub releases are in a repository of its own**,
   named after its directory in the same organization:
   `tinyactors-dev/tinyxml`, `tinyactors-dev/tinyactors-core`,
   `tinyactors-dev/staticjs`, `tinyactors-dev/tinyparse`,
   `tinyactors-dev/tinyactors-cpython`, `tinyactors-dev/tinyactors-napi`.
   The name follows from the directory, so no table maps one to the other
   (`Release.release_repository`). The bindings get one too: a binding's
   GitHub release carries its wheels or addons and their `SHA256SUMS`
   beside the registry, and one rule for every component is simpler than
   an exception.
3. **A release repository holds one commit per release.** Its tree is the
   component's directory at the monorepo tag, its parent the previous
   release's commit, and its author, committer and dates are those of the
   tagged monorepo commit. It is pushed as the repository's `main` and
   tagged `v<version>`. `git commit-tree` makes it from objects the
   monorepo has, so no subtree-split tool is needed and no history is
   rewritten. The commit exists because `gh release create --verify-tag`
   needs the tag in the target repository; it also lets a reader browse
   exactly what was released.
4. **The release repository is created on the first publish**, private
   like the monorepo (ADR 0001, decision 13), without issues or a wiki.
5. **Publishing is `Release.push_and_release!`**, for every release task:
   push `main` and the monorepo tag, push the snapshot, create the GitHub
   release in the release repository with the same notes and assets as
   before. Each step can run again after a stop: a push of what the remote
   has changes nothing, a release repository that has the tag with the
   same tree is left alone (another tree fails the publish), and a release
   that exists is left as it is. A version not above the release
   repository's latest is refused, so its `main` only moves forward.
6. **A pushed tag is never undone.** A C release that stops after its push
   keeps its commit and tag and says how to finish:
   `mise run release:mirror -- <tag> --assets <dir>` publishes a pushed
   tag's snapshot and release, with the tag's message as its notes. The
   Python release's `--resume` finishes through `push_and_release!` as
   before.
7. **Releases from before are copied**, oldest first per component, with
   `mise run release:mirror -- <tag>`: it takes the title, notes and assets
   of the monorepo's GitHub release and checks the assets against their
   `SHA256SUMS`. All but tinyactors-scxml's, whose directory and component
   are gone; its release stays on the monorepo's page as the record. Once
   a copy is checked, the maintainer deletes the monorepo's GitHub release
   of it (its tag stays), so there is one place to download each.
8. **Consumers move to the release repositories:** a C release downloads
   its dependency's release from the dependency's release repository
   (`CRelease.fetch_dependency!`), the staticjs site takes
   `tinyactors-dev/staticjs`'s latest release, and tinyparse's `install.sh`
   defaults to `tinyactors-dev/tinyparse` and tags `v<version>`, so its
   `releases/latest` is tinyparse's.

Package metadata (`repository` in `package.json`, `pyproject.toml`'s
URLs) keeps naming the monorepo: the source, the issues and the pull
requests are there.

## Consequences

- Each component has its own releases page, a working "Latest" and a
  stable download URL (`/releases/latest/download/<asset>` where the
  asset's name has no version).
- A release session needs push access to the release repositories as
  well as the monorepo: the Claude GitHub App is installed for the
  organization or for each repository.
- The monorepo's releases page holds only tinyactors-scxml 0.1.0 once
  the copies are made.
- Unverified: the copies of the eight releases and the first release
  through a release repository; both are the maintainer's to run, with
  `gh` authenticated for the organization.
