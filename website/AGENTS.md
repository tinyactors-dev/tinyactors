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
  into `render/site.xsl`, then into `style/site.css`, and as Markdown into
  `render/markdown.xsl`: what an agent reads (`llms.txt`, each page's
  `index.md`) says what the page says.
- **Every claim has its source**, a URL a reader can open: a file in a
  public repository, a decision record, a measurement. Before writing a
  number or a "passes all", find it in the repository and link it. When
  nothing backs a claim, leave it out.
- **Code on a page is code that ran.** A program a page shows lives under
  `examples/<name>/`, with a `package.json` whose `main` is the program, a
  `package-lock.json`, and `output.txt`. `mise run test` runs it.
- **A live example's logic is tested in Node.js.** A `<live>` block runs
  an export of a module in the reader's browser on `@tinyactors/wasm`.
  Keep what it computes in a module that the example's `main.mjs` runs too
  (`examples/property/property.mjs`), so `mise run test` checks it against
  `output.txt`. `live.mjs` only adapts it for the page. After changing a
  component or a live example, open the page in a browser (`mise run
  serve`) and run each button: nothing in the test task does.
- **The voice: short and literal.** Headlines, headings and claims'
  statements are 1 to 8 words, sentence case, with no period at the end
  ("Build a team chat"; the maintainer, 2026-11-21, replacing "ending
  with a period"). A paragraph is one or two sentences of at most 21
  words each, with the product or the reader's code as the subject. The
  schema refuses a heading or a paragraph outside these limits (the
  types `title` and `paragraph`), and any text that says "formal
  verification". Each section is one line of prose and then the proof:
  code, output, a claim with its source. No metaphors, no adjectives that
  do not measure something, no hype words, no emoji. Actions are a verb
  and an object, 2 to 4 words.
- **The deep-dive voice** (the maintainer, 2026-11-25: the Architecture
  page "should read like a book or a technical deep dive"). An
  explanation page a reader follows from start to end, written for the
  tech lead who must understand the design before depending on it, is
  prose in `<passage>` blocks: up to five sentences each, at most 32 words
  a sentence (the schema's `passage` type; only explanation pages have
  them). Its hero lede sets the context a newcomer lacks: what the thing
  is, what problem the page explains, and what the reader will be able to
  judge after it. Each section opens with a `<problem>`, the concrete situation it
  solves with its stakes, as the reader meets it, never a rhetorical
  question (the maintainer, 2026-11-25); then it builds the idea in
  passages, which hold no questions either, then shows the proof (a figure, a table, a claim
  with its source). Terms are introduced before they are used, and each
  section ends where the next one's question starts. The rest of the
  voice holds: literal, measured, no metaphors or hype; headings stay
  findings of at most 8 words.
- **Copy never names its audience** (the maintainer, 2026-11-25). The
  roles a page is written for (the tech lead, the operator, the workflow
  developer, the agent-platform engineer) are the writer's target and the
  `<purpose for>` bookkeeping, never words on the page: no "as a tech
  lead", "for operators" or "if you are an engineer". Write for the role
  by choosing what to explain; address the reader as "you" where a voice
  allows it.
- **Copy never explains how the site is made.** A reader does not care
  that a page is generated, captured or checked: no "this page is made
  from the command's help", "examples run at build time" or "from the
  help of version X". State what the thing does for them.
- **Every sentence says something.** Use the `copywriting` skill
  (`.agents/skills/copywriting`) before writing or changing copy: a
  headline states a benefit, a heading its finding, a lede the page's
  strongest facts (never a list of what is coming), and a sentence before
  a block what the block shows.
- **No eyebrows.** Nothing sits above a heading: the schema has no
  element for it, and `test/invalid/eyebrow.xml` keeps it that way.
- **Same thing, different words: tabs.** Content that says one thing for
  several platforms (an install command per package manager or language)
  is one element whose alternatives the page shows behind tabs, never a
  list of all of them. The reader's choice holds across the page.
- **Every piece of source code is highlighted, every chart can be seen.**
  Highlighting and diagrams are the stylesheet's and the components' work,
  never the content's: a `<code>` block names its language, and an SCXML
  block read from a file gets its diagram. Browser dependencies are
  vendored (`package.json`, `mise/website.rb` VENDOR), never loaded from a
  CDN.
- Name SCXML and actor terms exactly (state, event, transition, actor,
  host); the root `GLOSSARY.md` defines them. The product name is
  lowercase.

## Purposes and audiences

- **Every page declares its purpose** (`<purpose kind for>` after its
  description; only the 404 has none): its kind and its audiences, the
  main audience first. The five kinds: `pitch` (deciding whether to adopt
  the idea: a claim, a demonstration that runs, proof with its source, a
  next step), `tutorial` (learning by doing), `how-to` (reaching a goal),
  `reference` (looking up facts), `explanation` (understanding why).
  The guides are `how-to`, the changelog `reference`.
- **Audiences live in `site.xml` only**, each need one line with a short,
  stable id: change the text freely, keep the id. A new need is a new id.
- **Mark only the answers a section's text gives** (`answers`), and a
  section's `kind` only where it differs from the page's. An unanswered
  need is work, listed in `build/needs.json`; never mark it answered to
  empty the list.
- **Conflicting needs are resolved by placement.** The page's purpose
  wins: a tutorial does not stop for a pitch's proof, a pitch does not
  turn into reference. Link the other need to the page that answers it,
  or write that page; never squeeze it in.
- The published convention (the `page-kind` meta, the `rel="audience"`
  links, the audience files and `schema/audience.xsd`) is README.md's
  "Audiences and page kinds": tools on other sites rely on it, so change
  it there first. Which needs a section answers is the content's
  bookkeeping (`answers`): it stays in `content/` and is never published
  (the maintainer, 2026-11-23).

## The guides

- **The guides are the site's**, one page each in `content/guides/`
  (`id="guide-<name>"`, served at `/guides/<name>/`), listed in
  `site.xml` and on the index page `content/pages/guides.xml`. The
  Markdown in `tinyactors-napi/docs/guide/` they were edited from is
  notes for agents, not their source: change a guide here.
- **A section's id is its anchor**, kept when the heading changes, so
  links into a guide (`<link to="guide-x" section="y"/>`) stay valid.
- **A program a guide shows with its output runs.** An `<example>` whose
  javascript `<code>` and `<code role="output">` are written in the
  content runs in `examples/guides` against the published packages, and
  must print its output (`mise run test`). A fragment, or a program that
  needs a browser, is a plain `<code>` outside an `<example>`: shown, not
  run.
- **Measurements keep one home.** A table of measured numbers that a
  program in the repository writes (the capacity guide's) is transcluded
  from the Markdown it writes, not copied.

## The pitch

Three points, in this order (the maintainer, 2026-11-21, moving the
agents ahead of the in-process library):

1. **Actors that run charts, written by agents** (the home page's
   sections `actors` and `agents`): an actor's behavior is one statechart,
   every state and transition written down, where an Erlang or Akka actor
   runs any code. The same chart runs in simulation and in your process,
   and a simulated clock runs its timeouts (deterministic simulation).
   An agent reasons locally: the chart holds every state and transition,
   and its effects go out as events for your code, so an agent writes an
   actor from the events entering and leaving it (the maintainer,
   2026-11-25, replacing "a coding agent changes the trade").
2. **An in-process library** (section `process`): no server, one C core
   behind every language's package, small enough to make a request or a
   connection an actor, and idle actors kept in the application's own
   store.
3. **Properties instead of unit tests** (section `verify`): statecharts
   are a formal model with W3C semantics, so state properties over
   generated runs instead of examples. Say "properties" and "generated
   runs", not "formal verification": there is no model checker, and the
   site does not claim one. "Easy to verify" is fine (the maintainer,
   2026-11-21): it says what a formal model allows, not that tinyactors
   verifies a chart.

The wording of each point is the content's (`content/site.xml`,
`content/pages/home.xml`); this brief names what each argues, not how.

- **Examples are entities readers build** (an order, a payment, a
  subscription, a chat session), never a door, a light switch or a traffic
  light: the root `AGENTS.md`, "Examples are things people build".
- **Show how to build, not who built.** The site teaches with systems a
  reader recognises (a team chat, step by step) and does not feature
  specific applications, ours or anyone's.
- **A tutorial step is a program that runs.** The chat tutorial's steps
  are copies of `tinyactors-napi/examples/chat-tutorial` under
  `examples/chat/`, run against the published package; a page shows
  excerpts of them with `<code src lines>`. When the tutorial changes,
  copy it again and check each excerpt's lines.
- **Do not claim a library feature that does not exist.** Property testing
  and fuzzing are a pattern an application writes (the elevator example's
  scenario runner in `tinyactors-napi/examples`), not an API of
  tinyactors.

## The CLI section

- **It documents the latest release of the command, from the command.**
  The pages under `/cli/` are made from the pinned binary's own help and
  its examples run on `examples/cli/` (README.md, "The CLI section"):
  never write a command's usage, options or examples in `content/`. The
  hand-written pages follow Diátaxis: `/cli/` explains when to use the
  command (the deep-dive voice), the tutorial teaches by doing, the
  how-to pages reach a goal. A command line they show with its output is
  a transcript or a walkthrough step that `mise run test` runs. Move
  the pin (`mise.toml`) when tinyactors-core releases, and rerun `mise run
  test`. `TINYACTORS_CLI` is for development only.

## Tasks and tests

- `mise run test` before every commit that changes this directory. It
  validates the content, checks that the schema refuses every document in
  `test/invalid/` by the rule its comment names, checks that the build
  refuses each site in `test/broken-*/` with its message, builds the site, resolves every internal
  link and anchor, and runs the examples.
- A new refusal the schema makes gets a document in `test/invalid/` with
  `expect: <rule>` in its first comment.
- **Deploy every change you push** (the maintainer: "don't sit on
  changes, always deploy"). After the push, deploy from a clean tree at
  `origin/main` (rebase first, so another session's deployed work is not
  rolled back; `npm ci` when `package-lock.json` moved), and check the
  changed pages on www.tinyactors.dev.
- Deploying: `mise run deploy` deploys the working tree (CLOUDFLARE_API_TOKEN
  through fnox). A release (`mise run release -- <version> --publish`)
  needs its section of `CHANGELOG.md` from the release-changelog skill, and
  the maintainer's approval, as every component's does.
- Commits are scoped `website` (`feat(website): ...`, `fix(website): ...`).
