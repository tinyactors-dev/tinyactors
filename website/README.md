# The website

https://www.tinyactors.dev, written content first. What a page says, and in
what order, is XML (`content/`), checked against a schema
(`schema/site.xsd`). How it looks is an XSLT stylesheet (`render/site.xsl`)
and a layout (`style/site.css`) over the design system. Changing the copy
never means touching either of those.

```sh
mise run build     # validate content/, then write build/site
mise run serve     # build, then http://localhost:4321 as Cloudflare serves it
mise run test      # what the schema and build refuse, links and anchors, the examples run
mise run deploy    # build and deploy the working tree to www.tinyactors.dev
mise run release -- 0.1.0 [--publish]   # a release: changelog, tag, snapshot, deploy
```

[sxml](https://sxml.tinyactors.dev) (pinned in `mise.toml`) validates the
content and runs the stylesheet. Node.js runs the examples and wrangler.

## How it fits together

| Path | What it is |
|---|---|
| `content/site.xml` | the site: its name, its audiences, navigation, the pages in order, the footer |
| `content/pages/*.xml` | one page each: a hero, then sections of prose, lists, code, claims and actions |
| `content/guides/*.xml` | the guides, one page each, served at `/guides/<name>/`; their examples run in `examples/guides` against the published packages |
| `schema/site.xsd` | what content may say, as XSD 1.1: every element is documented there |
| `schema/audience.xsd` | the published format of an audience file, `/audiences/audience.xsd` ("Audiences and page kinds") |
| `render/needs.xq` | the needs report: `build/needs.json`, which pages and sections answer each audience's needs, and which needs none does |
| `render/site.xsl` | content to HTML and `sitemap.xml`; it stops at a link to a page or section that does not exist |
| `render/markdown.xsl` | the site for agents ([llmstxt.org](https://llmstxt.org)): each page as Markdown beside its HTML (`/x/index.md`), `llms.txt`, which lists them in the navigation's groups, and `llms-full.txt`, every page but the changelog in one file |
| `style/site.css` | the layout, on the design system's tokens |
| `examples/` | the programs pages show, each run by `mise run test` against its `output.txt`; `examples/property/` also runs in the page (`<live>`), `examples/anthill/` and `examples/lab/` are playgrounds |
| `components/` | the custom elements and scripts the pages use: `<ta-tabs>` (alternatives, one at a time), `<ta-copy>` (a copy button on every code block), `chart.mjs` (a chart's diagram), `highlight.mjs` (syntax highlighting), `<ta-live>` (an example's export run in the browser), `<ta-playground>` (an example's program with controls of its own: the anthill, the lab), `costs.mjs` (the anthill's live charts), `timeline.mjs` (the lab's charts of a whole run) and `<ta-trace-view>` (a run's spans as a tree on a timeline) |
| `package.json` | what the pages load in the browser, vendored by the build into `vendor/`: `@tinyactors/scxmljs` (`<scxml-view>`, with its QuickJS), highlight.js's browser modules, and `lucide-static`, whose icons the stylesheet inlines (the copy button's); one import map on every page names them |
| `test/` | documents the schema must refuse (`invalid/`), and sites the build must refuse (`broken-*/`: a link to a missing page, an unknown audience, an unknown need) |
| `tools/serve.mjs` | the local server |
| `../copywriter/` | not the site's: the copywriter's build (`dist/site`) is copied to `/copywriter/` (the extension's ZIP, a switch that loads its widget on every page of a device; not indexed) |
| `wrangler.jsonc` | the Cloudflare Worker: `build/site` as static assets on www.tinyactors.dev |
| `mise/` | the tasks (Ruby) and what they share (`website.rb`; `cli.rb`, the CLI section's capture) |
| `examples/cli/` | the charts and inputs the `tinyactors` command's help examples name (`order.scxml`, `system.xml`, `requests.jsonl`, ...), which the build runs them on; the outputs the how-to pages show (`requests.txt`, `actor-6.txt`, ...); and the walkthroughs `tutorial/` and `ci/`, each step's input and output |

The design system is the repository's one copy,
`.agents/skills/tinyactors-design`. The build puts its tokens and
components ahead of `style/site.css` in one `assets/site.css`, and copies
its fonts and brand files.

## Writing a page

A page is a hero and sections. Each section has a heading and blocks:

```xml
<section id="simulate">
  <heading>A thousand runs, <em>no waiting</em>.</heading>
  <p>The chart talks to the world only through events, ...</p>
  <example>
    <code src="../../examples/simulate/simulate.mjs" language="javascript" file="simulate.mjs"/>
    <code src="../../examples/simulate/output.txt" language="text" role="output"/>
  </example>
  <claims>
    <claim>
      <statement>The semantics are a W3C standard.</statement>
      <p>Charts are written in SCXML, and the core passes ...</p>
      <source href="https://github.com/...">tinyactors-core, <code>docs/architecture.md</code></source>
    </claim>
  </claims>
</section>
```

- **A claim needs a source.** A `<claim>` without a `<source>` does not
  validate. Put what a reader may doubt in a claim, with where to check it.
- **Code that runs is a file.** `<code src>` names a file under
  `examples/`, and the test runs that example and compares what it prints
  with its `output.txt`. So the page shows exactly the code that ran.
- **Code is highlighted; charts can be diagrams.** Every `<code>` block
  but output and text is highlighted in the browser, in the design
  system's colours. An SCXML block read from a file (`<code src>`) also
  gets a Diagram tab: the whole chart that file holds, drawn and run by
  scxmljs's `<scxml-view>`. The choice holds for every chart on the page.
  Each I/O processor type the chart sends to gets a stand-in that lists
  what it received under the diagram; the reader answers by clicking a
  transition's label, and "Run again" restarts the run.
- **One program in every language is a `<program>`.** It shows how a
  reader's code uses tinyactors: an `<in package>` per package of
  `site.xml` (its files as `<code src file>`, the program first, then the
  chart it reads), then the one output they all print. The page shows one
  package at a time, behind the install commands' tabs (the reader's
  platform holds), and one file at a time, a chart's file followed by its
  diagram. Its programs live in one directory under `examples/`, which
  `mise run test` runs: `main.mjs`; `browser.mjs` under Node.js with
  `tools/file-fetch.mjs`, so its `fetch` of the chart beside it reads the
  file; `main.py` with uv, against its `uv.lock`; and `main.c`, built with
  the pinned clang against tinyactors-core's newest release libraries
  (downloaded once into `build/c/`, checked against the release's
  `SHA256SUMS`). Each must print `output.txt`. A `<diagram chart>` says
  how a reader runs the chart's diagram: an `<event name data>` is what
  clicking that event's label sends, and one with words is also a button
  under the diagram that sends it ("Add a pen"). The reader plays each
  I/O processor by clicking its reply's label, and the diagram lists the
  chart's variables after each step.
- **A live example runs in the browser.** `<live src call>` names an
  export of a module under `examples/`, run by a button whose `<label>` is
  the content's. The build serves the module with `@tinyactors/wasm` at
  the version its lockfile pins, through an import map. Keep the logic in
  a module that `main.mjs` also runs, so `mise run test` covers it.
- **Links name pages, not paths.** `<link to="chat"/>` names a page by
  its id (`section` for an anchor), and the build refuses an id the site
  does not have. A link elsewhere is `href`, https only.
- **No styling in content.** There is no class, colour or layout element.
  If a page needs a new kind of block, add it to the schema with its
  meaning, then render it in `render/site.xsl`, and as Markdown in
  `render/markdown.xsl` (the build stops at a block with no Markdown).

A new page goes in `content/pages/` and in `site.xml`'s `<pages>`.

## The CLI section

`/cli/` is the `tinyactors` command's section, each kind of page under a
path of its own: `/cli/` is where to start (what the command is, how to
install it, where to go), `/cli/tutorial/` is a first session,
`/cli/guides/<name>/` are how-to guides (`ci`, `scripting`, `snapshots`),
`/cli/commands/` is the reference, the commands by job and the options
every command takes, each command's page `/cli/commands/<name>/`, and
`/cli/cheatsheet/` is common command lines by task, and `/cli/why/`
explains when to use the command beside the libraries. As the docs of
gh, Cargo, mise and Deno do (`docs/research/cli-docs-survey.md`), the
menu and each page's title use a short label ("Reference", "Cheat
sheet"), and the headline says what the page does. A command's page has
every command beside it, grouped by job (on a phone, after it), its
related pages and the commands that share its jobs, and the command
before and after it. The
command pages are written by nobody: `<commands>`
(`content/pages/cli-commands.xml`) holds only the words they use (headings,
the jobs that group the commands, the sentence pointing to the shared options, the word before an option's
values). The build (`mise/cli.rb`) runs the binary `mise.toml` pins
(`"github:tinyactors-dev/tinyactors-core"`, installed by mise from the
release's archive), reads its help as XML (`help -f xml`, `help COMMAND
-f xml`), runs each example of its help on the charts of `examples/cli/`
and writes `build/cli/cli.xml`, from which `render/site.xsl` makes one
page per command: usage, description, options (a list of terms, so a
description wraps and an option stacks on a phone), examples with what
they printed and the pictures they drew, and the exit statuses and
example captions once the help carries them. A new command or option
appears when the site builds against a new release, after the pin moves.
`<artifact>` serves a file the binary writes (`run --schema` as
`/cli/commands/run.schema.json`). An address that moved redirects
(`_redirects`, `MOVED` in `mise/website.rb`).

The hand-written pages show sessions that ran. A how-to page's output
is a file of `examples/cli/` with its command line in `TRANSCRIPTS`; a
walkthrough (`WALKTHROUGHS`: the tutorial, the CI guide) is a directory
of `examples/cli/` whose steps run in order in one copy of it, each a
command line its page shows and the file it printed (or wrote: the
tutorial's `order.svg`, shown with `<drawing>`). After a deliberate
change, write the outputs again with `ruby -r ./mise/website -e
'Website::CLI.walkthrough!("tutorial", write: true)'` and read the diff.

`mise run test` checks that every command the binary lists has its page,
that no page names a command or an option it lacks, that its help's
examples ran, that every transcript and walkthrough step prints what its
file says (with the exit status it must give), and that the pin is
tinyactors-core's newest release.

For development, `TINYACTORS_CLI` names another binary, such as
tinyactors-core's `build/dev/bin/tinyactors`; the build and the tests use
it instead of the pin's. Before the pinned release is published, mise
cannot install it and refuses to run the tasks; run them as

```sh
MISE_TASK_RUN_AUTO_INSTALL=false TINYACTORS_CLI=../tinyactors-core/build/dev/bin/tinyactors mise run test
```

## Purposes and audiences in the content

`site.xml`'s `<audiences>` defines who reads the site, once: each
`<audience id>` has a `name`, a `description`, what it `knows`, and its
needs, one line each, as `<want id>`, `<task id>` and `<objection id>`.
A page says what it is for after its description:

```xml
<audience id="tech-lead">
  <name>Tech lead who approves dependencies</name>
  <description>An architect or tech lead deciding whether ...</description>
  <knows>Reasons like sqlite.org's "when to use" page ...</knows>
  <want id="failure-semantics">The failure semantics of one process: ...</want>
  <task id="decide">Decide yes or no</task>
  <objection id="host-dies">What happens to state when the host dies?</objection>
</audience>

<purpose kind="pitch" for="agent-platform tech-lead workflow-tester"/>

<section id="production" answers="tech-lead.failure-semantics">
```

`kind` is one of `pitch`, `tutorial`, `how-to`, `reference`,
`explanation`; `for` lists the page's audiences, the main one first; a
purpose's `answers` names needs the whole page answers, a section's those
its text answers, and a section's own `kind` is given only where it
differs from the page's. The schema checks the shapes; the build stops at
an audience or a need `site.xml` does not define. The build prints how many
needs of each audience some page answers, and writes `build/needs.json`
(not published):

```json
{"audiences": [{"id": "tech-lead", "name": "...", "mainAudienceOf": ["snapshots"],
                "answered": 5,
                "needs": [{"id": "tech-lead.host-dies", "kind": "objection", "text": "...",
                           "answeredBy": [{"page": "guide-browser", "path": "/guides/browser/",
                                           "section": "page-goes-away"}]}]}],
 "unanswered": ["tech-lead.when-to-use", "..."]}
```

## Audiences and page kinds: the convention

How a page tells a tool (the copywriter, an agent, a crawler) what it is
for and whom it is for, with standard HTML and one published file per
audience. It is written so that any site can adopt it: a tool reads the
links of the page it is given and knows no audience of its own.

**In the page's `<head>`:**

```html
<meta name="page-kind" content="pitch">
<link rel="audience" href="/audiences/agent-platform.xml">
<link rel="audience" href="/audiences/tech-lead.xml">
```

- `<meta name="page-kind">`, exactly one, is the page's kind: `pitch`
  (the reader is deciding whether to adopt the idea: a claim, a
  demonstration that runs, proof with its source, a next step),
  `tutorial` (learning by doing), `how-to` (reaching a goal), `reference`
  (looking up facts) or `explanation` (understanding why); the last four
  are Diátaxis's.
- `<link rel="audience">`, one or more, each names an audience file, in
  order: the first is the page's main audience. `audience` is not one of
  HTML's registered link types; HTML lets a page use other `rel` values,
  and a browser ignores one it does not know.
- A page without these declares nothing (here, the 404).

**What the page does not publish:** which needs its sections answer.
The content says so (`answers` on a purpose or a section), the build
checks it and lists the unanswered needs in `build/needs.json`, and the
repository's tools read it from `content/`; the published HTML carries
none of it. A tool reviewing a page judges from its text which needs it
answers. A need is named `<audience id>.<need id>`, where the audience id
is that of a file the page links (its `id` attribute).

**An audience file** (`/audiences/<id>.xml`), valid against
[`/audiences/audience.xsd`](schema/audience.xsd), namespace
`https://www.tinyactors.dev/ns/audience`:

```xml
<audience xmlns="https://www.tinyactors.dev/ns/audience" id="tech-lead">
  <name>Tech lead who approves dependencies</name>
  <description>An architect or tech lead deciding whether ...</description>
  <knows>Reasons like sqlite.org's "when to use" page ...</knows>
  <want id="failure-semantics">The failure semantics of one process: ...</want>
  <task id="decide">Decide yes or no</task>
  <objection id="host-dies">What happens to state when the host dies?</objection>
</audience>
```

`name`, `description` and `knows`, once each and in that order; then one
or more `want` (what the reader looks for), one or more `task` (what they
came to do) and any number of `objection` (what makes them doubt, in their
words), each one line of plain text with an `id` unique within the
audience. `/audiences/index.xml` lists a site's audiences:

```xml
<audiences xmlns="https://www.tinyactors.dev/ns/audience">
  <item id="tech-lead" href="tech-lead.xml">Tech lead who approves dependencies</item>
</audiences>
```

A page's Markdown (`index.md`) says the same in its first line under the
title, `Kind: pitch. For: [name](audience file URL), ...`, and `llms.txt`
lists the audiences once, under "Audiences". The build writes all of it
from `content/`; `mise run test` checks that every page with a purpose has
its meta and links in order, that no page publishes the content's
answers, and that every audience file validates against
`audience.xsd`.
