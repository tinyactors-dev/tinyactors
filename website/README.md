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
| `components/` | the custom elements and scripts the pages use: `<ta-tabs>` (alternatives, one at a time), `chart.mjs` (a chart's diagram), `highlight.mjs` (syntax highlighting), `<ta-live>` (an example's export run in the browser), `<ta-playground>` (an example's program with controls of its own: the anthill, the lab), `costs.mjs` (the anthill's live charts), `timeline.mjs` (the lab's charts of a whole run) and `<ta-trace-view>` (a run's spans as a tree on a timeline) |
| `package.json` | what the pages load in the browser, vendored by the build into `vendor/`: `@tinyactors/scxmljs` (`<scxml-view>`, with its QuickJS) and highlight.js's browser modules; one import map on every page names them |
| `test/` | documents the schema must refuse (`invalid/`), and sites the build must refuse (`broken-*/`: a link to a missing page, an unknown audience, an unknown need) |
| `tools/serve.mjs` | the local server |
| `../copywriter/` | not the site's: the copywriter's build (`dist/site`) is copied to `/copywriter/` (the extension's ZIP, a switch that loads its widget on every page of a device; not indexed) |
| `wrangler.jsonc` | the Cloudflare Worker: `build/site` as static assets on www.tinyactors.dev |
| `mise/` | the tasks (Ruby) and what they share (`website.rb`) |

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
<meta name="page-answers" content="tech-lead.release-history">
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
- `<meta name="page-answers">`, optional: needs the page answers as a
  whole, space-separated.
- A page without these declares nothing (here, the 404).

**On a section:** the `<section>` element carries
`data-answers="tech-lead.host-dies agent-platform.memory"`, the needs its
text answers, space-separated, and `data-kind="how-to"` when its kind
differs from the page's. Neither attribute appears where there is nothing
to say. A need is named `<audience id>.<need id>`, where the audience id is
that of a file the page links (its `id` attribute).

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
its meta and links in order, every section with `answers` its
`data-answers`, and every audience file validates against
`audience.xsd`.
