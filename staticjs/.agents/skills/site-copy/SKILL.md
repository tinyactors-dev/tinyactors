---
name: site-copy
description: Write, rewrite or review the copy of the staticjs documentation website (staticjs/site), the front page above all - its headline, sections, positioning against QuickJS, CEL and Starlark, and the prose of the guide pages. Use when asked to improve the site's copywriting, rewrite or reorder a section, roast a headline, critique prose that reads like an LLM wrote it, or position staticjs. Not for the C API's reference comments or docs/internals.
---

# Copy for the staticjs site

The site's copy is made in a fixed process. You write the briefs and keep
the facts; Sonnet subagents write and critique; the maintainer decides.
Each step exists because skipping it went wrong before.

The rules for the prose itself are `staticjs/site/WRITING.md`. Read it
first, whole. The facts a writer may use are [facts.md](facts.md). The
brief templates are [briefs.md](briefs.md).

## Why the process looks like this

Earlier rounds produced copy the maintainer called terrible, then "reads
like an LLM", then "still talking about rules and guards". The cause each
time was the brief, not the writer:

- **A brief that framed staticjs by the reader's job** ("code someone else
  writes", "guards, filters, pricing rules") came back as copy about that
  job. Fix: a neutral fact sheet that describes the engine, with the uses
  in a section of their own.
- **Many writers in parallel, each given the whole pitch,** repeated the
  same facts in every section. Fix: one writer per section, one after
  another, each shown what is already written.
- **Rules without examples** were not followed. Fix: `WRITING.md` quotes
  how QuickJS, CEL and Starlark write about themselves, and lists our own
  past mistakes with examples.

## Steps

1. **Fix the frame.** What is being written (a section, the front page, a
   guide page), and what the maintainer has fixed: today the headline ("A
   small, embeddable and fast JavaScript engine") and the front page's
   section order (`WRITING.md`, "The front page"). Do not reopen either.

2. **Check the facts.** Read `facts.md` against the code before handing
   it out: the header, the guide, `bench/engines/REPORT.md`, the race's
   cases. Change what is stale. A claim not in `facts.md` is not made.
   For positioning, read the neighbors' own pages (QuickJS, quickjs-wasi,
   CEL, Starlark) and quote them; `docs/research/staticjs-market.md` has
   the market research.

3. **Roast, when the question is positioning or a headline.** One Sonnet
   agent, given only the project's context pasted into the prompt (the
   facts and the current text, verbatim), told not to read files or
   browse. It says what is wrong, who the text fails to reach, what a
   reader thinks in three seconds, and proposes five alternatives with
   the one it would ship. Template: briefs.md, "Roast". Show the roast to
   the maintainer; they choose.

4. **Write, one section at a time.** One Sonnet agent per section, run
   sequentially, never in parallel. Each reads exactly two files,
   `WRITING.md` and `facts.md`, and gets:
   - its place in the page's order, and which rules apply most;
   - every section already written, verbatim, to match its voice and not
     repeat it;
   - what its section holds, as a short list, and what it must not do;
   - an exact output format with no commentary.

   You write the brief, never the copy. Template: briefs.md, "Section".

5. **Critique.** One Sonnet agent as a senior human copy editor, given the
   rendered page's visible text and the facts, not the files. It names
   the LLM tells across the page with a count of each, then quotes each
   bad line with a rewrite or CUT, then returns the whole page rewritten.
   Template: briefs.md, "Critique". Apply what keeps to the facts and to
   `WRITING.md`; a critic that adds a claim is wrong there.

6. **Edit the whole page.** Read it top to bottom yourself:
   - each fact in one section only;
   - the sections in the fixed order;
   - no headings that are sentences, no eyebrows, no lede that restates
     its section;
   - the checklist in `WRITING.md`, "What we kept getting wrong".

7. **Wire it in.** The front page is `HomePage.vue` (its arrays:
   `properties`, `faults`, `ways`, `compare`, `others`, `limits`, `docs`)
   and its markdown mirror `index.md`, in the same order; guide pages are
   `site/guide/*.md`. A user-visible change to the language or the API
   also goes in `docs/guide/` (staticjs's `AGENTS.md`, Documentation).
   Diagrams come from the design system (`WRITING.md`, "Diagrams").

8. **Check and show.**
   - Run every snippet you added with `build/examples/eval` (`mise run
     examples` builds it), and diff its result against `node`.
   - Build the site: `mise exec -- npx vitepress build` in `site/`.
   - Run the route tests: `node --test test/routes.test.mjs` in `site/`.
   - Run `mise run check-acronyms` from the repository root.
   - Take screenshots with agent-browser, with
     `AGENT_BROWSER_EXECUTABLE_PATH` set to the pre-installed Chromium:
     light mode, dark mode (`set media dark`) and a phone
     (`set viewport 390 844`), served by `node server.mjs --port 4174`.
   - Send the screenshots to the maintainer before you call it done.

9. **Commit** with `docs(staticjs)` (or `feat(staticjs)` for a new
   component). The body names what changed in the page and which agents
   wrote or critiqued it, and it carries no model identifiers.

## What not to do

- Do not name statecharts, guards, SCXML or `In()` in user-facing copy.
  The uses are plugins and code mode (`facts.md`, Uses).
- Do not claim staticjs beats Node's JIT, or is smaller than QuickJS in
  bytes.
- Do not let a subagent see a draft framed around a use case. It will
  copy the framing.
- Do not write copy yourself and then ask for a critique of your own
  framing. Brief a writer from the facts.
