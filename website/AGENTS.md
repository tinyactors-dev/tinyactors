# AGENTS.md: website

https://www.tinyactors.dev. The repository root's `AGENTS.md` applies too.
`README.md` says how the directory fits together; this file holds the
rules.

## Content first

- **Copy and structure live in `content/`, nowhere else.** The stylesheet
  writes no words of its own beyond what the HTML needs (the "Source" label
  of a claim, "output", "Skip to content").
- **The schema is the vocabulary.** An element says what something *is*
  (a claim, an example, an install command), never how it looks. A new kind
  of block goes into `schema/site.xsd` with its documentation first, then
  into `render/site.xsl`, then into `style/site.css`.
- **Every claim has its source**, a URL a reader can open: a file in a
  public repository, a decision record, a measurement. Before writing a
  number or a "passes all", find it in the repository and link it. When
  nothing backs a claim, leave it out.
- **Code on a page is code that ran.** A program a page shows lives under
  `examples/<name>/`, with a `package.json` whose `main` is the program, a
  `package-lock.json`, and `output.txt`. `mise run test` runs it.
- **The voice is the design system's** (`.agents/skills/tinyactors-design`,
  "Content fundamentals"): precise and calm, "you" for the reader, sentence
  case, the product name lowercase, exact numbers, no hype words, no emoji.
  Name SCXML and actor terms exactly (state, event, transition, actor,
  host); the root `GLOSSARY.md` defines them.

## The pitch

Statecharts are control flow formal enough to simulate, test and fuzz.
tinyactors makes them easy to use, scalable and available everywhere. Lead
with what a reader can do with a chart (simulate it, assert on its states,
model the world around it), and bring in the runtime's capacity as
evidence. granary (tinyactors-dev/granary) is the reference application and
the case study. Its decision records are the sources for what the site says
about it.

Do not claim a library feature that does not exist. Property testing and
fuzzing are a pattern applications write (granary's load generator, the
elevator example's scenario runner), not an API of tinyactors.

## Tasks and tests

- `mise run test` before every commit that changes this directory. It
  validates the content, checks that the schema refuses every document in
  `test/invalid/` by the rule its comment names, checks that the build
  refuses `test/broken-link/`, builds the site, resolves every internal
  link and anchor, and runs the examples.
- A new refusal the schema makes gets a document in `test/invalid/` with
  `expect: <rule>` in its first comment.
- Deploying: `mise run deploy` deploys the working tree (CLOUDFLARE_API_TOKEN
  through fnox). A release (`mise run release -- <version> --publish`)
  needs its section of `CHANGELOG.md` from the release-changelog skill, and
  the maintainer's approval, as every component's does.
- Commits are scoped `website` (`feat(website): ...`, `fix(website): ...`).
