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
| `content/site.xml` | the site: its name, navigation, the pages in order, the footer |
| `content/pages/*.xml` | one page each: a hero, then sections of prose, lists, code, claims and actions |
| `schema/site.xsd` | what content may say, as XSD 1.1: every element is documented there |
| `render/site.xsl` | content to HTML, `sitemap.xml` and `llms.txt`; it stops at a link to a page or section that does not exist |
| `style/site.css` | the layout, on the design system's tokens |
| `examples/` | the programs pages show, each run by `mise run test` against its `output.txt`; `examples/property/` also runs in the page (`<live>`) |
| `components/` | the custom elements the pages use: `<ta-live>` runs an example's export in the browser, `<ta-trace-view>` draws a run's spans as a tree on a timeline |
| `test/` | documents the schema must refuse, and a site the build must refuse |
| `tools/serve.mjs` | the local server |
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
  meaning, then render it in `render/site.xsl`.

A new page goes in `content/pages/` and in `site.xml`'s `<pages>`.
