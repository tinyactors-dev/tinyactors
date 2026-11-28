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
- **A chart lives in its own `.scxml` file** (the maintainer, 2026-11-28),
  beside the program that reads it, never in a string inside the
  program: the reader copies it whole, the page draws its diagram and an
  editor highlights it. A program that needs a chart is a `<program>` over
  `examples/<name>/`, one `in` per package with the program and the
  chart, so the reader's language picks the tab. The guides' older
  `<example>` programs with a chart in a string move to files when they
  are next changed.
- **A live example's logic is tested in Node.js.** A `<live>` block runs
  an export of a module in the reader's browser on `@tinyactors/wasm`.
  Keep what it computes in a module that the example's `main.mjs` runs too
  (`examples/property/property.mjs`), so `mise run test` checks it against
  `output.txt`. `live.mjs` only adapts it for the page. After changing a
  component or a live example, open the page in a browser (`mise run
  serve`) and run each button: nothing in the test task does.
- **The voices are content.** `content/site.xml`'s `<voices>` holds how
  the site writes, one voice for each kind of page, each rule one line
  with a stable id; the build publishes each whole as `/voices/<id>.xml`
  and every page's head links its own (README.md, "Audiences and page
  kinds: the convention"), so the copywriter and agents read the rules
  from the page. Change a rule there, not here. The developer voice (the
  maintainer, 2026-11-28, replacing "short and literal", "deep dive" and
  the first tutorial voice): one voice per kind of page, measured against
  rivet.dev, stately.ai and effect.website in the `developer-voice` skill
  (`.agents/skills/developer-voice`), which holds the evidence, the
  registers and rewrites of our own copy.
  - **Reference** (`reference`) holds the rules every voice keeps:
    headings of 1 to 8 words with no period, sentences of at most 30
    words that vary in length, a number with its conditions, a negative
    only against the reader's current tool or belief, no rhetorical
    question. The schema refuses a heading or a paragraph outside its
    limits (the types `title` and `paragraph`: one to three sentences of
    at most 30 words), and any text that says "formal verification".
  - **Pitch** (`pitch`): a problem the reader has had before the answer,
    the enemy named (logic spread across services with no owner), proof
    under every claim, comparisons that give both sides real entries.
  - **How-to** (`how-to`): the reader's situation first, one line under
    the code for each part that matters, one example carried through.
  - **Tutorial** (`tutorial`): `<passage>` blocks of two or three
    sentences; each step opens with the problem the last one left, and
    after its output says what to look for.
  - **Explanation** (`explanation`; the maintainer, 2026-11-25: the
    Architecture page "should read like a book or a technical deep
    dive"): `<passage>` blocks (the schema's `passage` type), each
    section opened by a `<problem>`, never a rhetorical question; "we"
    for what the maintainers decided or measured.
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
- **Every sentence says something.** Follow the `copywriting` skill
  (`.agents/skills/copywriting`) before writing or changing copy: it is
  the process, from the audience and the message to a review as a fresh
  reader in a browser, and it loads `developer-voice` for how the page
  sounds.
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

The position (the maintainer, 2026-11-27, replacing the three points of
2026-11-21): **the enemy is complexity**, logic spread across services,
queues, workers and `async`/`await` chains with no clear owner of any
piece of state. Durable Objects, Temporal, Rivet and distributed systems
in general answer it with infrastructure that makes decisions for you.
tinyactors answers it by removing the spread: actors you can verify,
without the infrastructure, at in-process speed. XState has the
verifiability, Rivet the infrastructure and distribution; tinyactors has
actors and verifiability without the infrastructure.

Five reasons, the home page's sections in this order, each a claim the
site proves:

1. **One process, so you can reason about it** (section `process`): no
   server, one place to look, one order of events, one C core behind
   every language's package.
2. **Small actors, so you can inspect it** (about half a kilobyte; the
   sections `process` and `production`): everything fits in memory, so
   your code reads any actor's state, snapshots the whole process and
   restores it elsewhere; small enough to make a request, a connection
   or an order an actor.
3. **Statecharts the computer can check** (sections `actors`, `agents`
   and `verify`): an actor's behavior is one SCXML chart with every state
   and transition written down, where an Erlang or Akka actor runs any
   code. The same chart runs in simulation and in your process; a
   simulated clock runs its timeouts; properties are checked over
   generated runs; the CLI's `check`, `lint` and `diff` run in CI; and an
   agent reasons locally, writing an actor from the events entering and
   leaving it (the maintainer, 2026-11-25). Say "properties" and
   "generated runs", not "formal verification": there is no model
   checker, and the site does not claim one. "Easy to verify" is fine
   (the maintainer, 2026-11-21): it says what a formal model allows, not
   that tinyactors verifies a chart.
4. **No opinion about I/O, so no lock-in**: effects leave the chart as
   events your code performs, and idle actors hibernate into the store
   your program already uses. Your program made its infrastructure
   decisions; tinyactors uses them.
5. **Behavior changes while it runs** (the maintainer, 2026-11-27):
   charts are data the host loads, so a new version is added to the
   running process, new actors start on it, and a migration moves
   running actors to it with the state they reached. No redeploy.

**Say when infrastructure is the right choice.** When an application
needs to be globally distributed and work everywhere at once, the
infrastructure is the product, and Cloudflare or Rivet fits better.
tinyactors is for a program on a machine you understand. Every
comparison says so, and When to use already does.

The wording of each reason is the content's (`content/site.xml`,
`content/pages/home.xml`); this brief names what each argues, not how.
The home page follows the old order until it is rewritten.

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
  command (the explanation voice), the tutorial teaches by doing, the
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
- **Published URLs keep working.** `published-urls.txt` holds every path
  the sitemap has ever held, and the old paths of what moved; append
  only. `mise run test` fails on a line that is neither a file of the
  build nor a redirect of `_redirects` (a `MOVED` entry in
  `mise/website.rb`), naming the line, and on a path of the sitemap the
  file lacks: add a new page's path to it in the same commit, and give a
  page you move a `MOVED` entry.
- **What else the test checks of the build's plumbing:** each page "/x/"
  has a 301 from "/x" in `_redirects`; every namespace of the site's
  origin that a document names has its page (`content/site.xml`,
  `<namespaces>`) and links its schema; the CLI's JSON Schema is served at
  the path of its `$id`; `sitemap.xml` gives each page its `<lastmod>`
  (`build/dates.xml`, from git); the changelog's Atom feed
  (`/changelog/feed.xml`) has an entry per release and the changelog page
  links it; `/.well-known/security.txt` has its Contact, an Expires
  within the year and its Canonical URL.
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
