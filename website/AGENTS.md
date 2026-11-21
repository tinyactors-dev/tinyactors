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
- **A live example's logic is tested in Node.js.** A `<live>` block runs
  an export of a module in the reader's browser on `@tinyactors/wasm`.
  Keep what it computes in a module that the example's `main.mjs` runs too
  (`examples/property/property.mjs`), so `mise run test` checks it against
  `output.txt`. `live.mjs` only adapts it for the page. After changing a
  component or a live example, open the page in a browser (`mise run
  serve`) and run each button: nothing in the test task does.
- **The voice: short and literal.** Headlines and headings are 1 to 8
  words, sentence case, ending with a period ("Build a team chat."). A
  paragraph is one or two sentences of at most 21 words each, with the
  product or the reader's code as the subject. The schema refuses a
  heading or a paragraph outside these limits (the types `title` and
  `paragraph`), and any text that says "formal verification". Each
  section is one line of prose and then the proof: code, output, a claim
  with its source. No
  metaphors, no adjectives that do not measure something, no hype words,
  no emoji. Actions are a verb and an object, 2 to 4 words.
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

## The pitch

Three points, in this order:

1. **An in-process library** (the home page's section `actors`): no
   server, small actors, millions of them with most asleep.
2. **Charts suit agents** (section `agents`): an actor's behavior is one
   chart (local reasoning), charts do no I/O (isolated effects), and a
   simulated clock runs any scenario (simulation).
3. **Properties instead of unit tests** (section `verify`): statecharts
   are a formal model with W3C semantics, so state properties over
   generated runs instead of examples. Say "properties" and "generated
   runs", not "formal verification": there is no model checker, and the
   site does not claim one.

The wording of each point is the content's (`content/site.xml`,
`content/pages/home.xml`); this brief names what each argues, not how.

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
