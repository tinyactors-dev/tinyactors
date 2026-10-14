# staticjs's documentation site

The user documentation as a website: VitePress, in the Tinyactors design
system, with live samples and a pipeline explorer that run staticjs in the
page through WebAssembly. Every page is also markdown, and `/llms.txt` is
the entry point for agents. The pages started as copies of
`../docs/guide/` and evolve on their own.

```sh
mise run site:dev     # the dev server, with markdown routes
mise run site:build   # .vitepress/dist: HTML, every page's .md, llms.txt, llms-full.txt
mise run site:serve   # serve the build as production would
mise run site:test    # build, then test the routes
```

## How it fits together

| Path | What it is |
|---|---|
| `wasm/site.c` | the site's C layer over staticjs: numbers in and out, a recording sink, the inspection calls. `mise run site:wasm` builds it with the amalgamation into `public/staticjs-site.wasm` and writes the constants the page reads (`.vitepress/theme/staticjs/constants.json`). Both are build outputs, not in git. |
| `.vitepress/theme/staticjs/runtime.js` | staticjs for the page and for the build: compile, events, functions, run, frames, write, and `pipeline()`, which runs every stage of a program. |
| `.vitepress/config.mts` | the site, and the `js live` fence: the build computes each sample's result (the page shows it before any JavaScript runs) and fails when a sample's outcome is not the one its fence states. |
| `.vitepress/agents.js` | markdown for agents: a page's source without Vue, its live samples as code and their results, links pointing at `.md`; `Accept` negotiation; `llms.txt`. |
| `server.mjs` | the production server: static files, clean URLs, markdown by `.md` or `Accept: text/markdown`, `Vary` and `Link` headers. No dependencies. |
| `.vitepress/theme/` | the theme: the Tinyactors tokens (`tokens/`, vendored from tinyactors-dev/scxmljs), their mapping onto VitePress (`style.css`), and the components (`LiveSample`, `PipelineExplorer`, `HomePage`). |

## Writing a page

- A live sample is a fence with `js live`. Add `module` for a module, and
  `fault` for a sample that must fault or be refused. The page's clock is
  2024-01-31T10:00Z and its seed 1.
- Something only the browser can show goes between `<!-- html -->` and
  `<!-- /html -->`; text only the markdown needs, in `<!-- markdown ... -->`.
- A new page goes in `.vitepress/config.mts`'s `guide` or `explore` list,
  with a description: that list is the sidebar and `llms.txt`.
