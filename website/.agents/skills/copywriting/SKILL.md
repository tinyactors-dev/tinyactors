---
name: copywriting
description: The process for writing and reviewing the copy of www.tinyactors.dev (website/content/, and the words inside the site's components and live examples), from choosing the page's kind, audience and message, through its voice, facts and structure, to a review as a fresh reader in a browser. Use before writing or changing any page, headline, lede, heading, paragraph, claim, list item, caption, button label, chart title or status line, before reordering a page's sections, when a page needs a kind of block the schema does not have, and to review copy someone else wrote. references/failures.md holds the failures found on this site, references/patterns.md the before-and-after pairs; the developer-voice skill holds how the site sounds.
---

# Writing copy for the website

A reader gives the site a few seconds. Every sentence must tell them
something: a fact about tinyactors, what it does for them, or what to do
next. Copy is written in this order, and each step has a check. Skip a
step and the later ones cannot fix what it would have caught.

1. The page's job: its kind and its audience.
2. The message: what the reader leaves with.
3. The voice: how the page sounds.
4. The facts: what the page may claim.
5. The structure: the order of its sections and blocks.
6. The draft and the review of every line.
7. The review as a fresh reader, in a browser.
8. Shipping.

## 1. The page's job

Every page declares `<purpose kind for>` (`website/AGENTS.md`, "Purposes
and audiences"). Decide both before writing a word.

- **Kind:** `pitch` (deciding whether to adopt), `tutorial` (learning by
  doing), `how-to` (reaching a goal), `reference` (looking up facts),
  `explanation` (understanding why). A page does one. When a second need
  pulls at it, link the page that answers it, or write that page.
- **Audience:** the main one first, from `content/site.xml`'s
  `<audiences>`. Read the audience's `knows`, `want`, `task` and
  `objection` lines: they say what the reader brings, what to explain
  and what to leave out. `build/needs.json` lists the needs no section
  answers yet; a new page should take some off it.
- Write for the role by choosing what to explain. Never name it on the
  page.

**Check:** can you say in one line who arrives here, from where (the
home page, a guide, a search), and what they want to do next?

## 2. The message

Write the one sentence the reader should leave with, before the page.
For a pitch it follows the position in `website/AGENTS.md` ("The
pitch"); for a guide it is the outcome ("an idle actor waits in your
database"); for a tutorial, what they will have built.

Then list what the reader must believe for that sentence to hold, in
the order they will doubt it. Each item becomes a section, and its
proof decides what the section shows.

**Check:** does every section serve the message? A section that serves
another message belongs on another page.

## 3. The voice

The page's kind picks its voice in `content/site.xml` (`<voices>`,
published at `/voices/<id>.xml`). Load the `developer-voice` skill
(`../developer-voice`): it holds why the voice is what it is, the
register of each kind of page, excerpts from rivet.dev, stately.ai and
effect.website, rewrites of our copy, and `scripts/measure.py`.

**Check:** read the voice's rules once before drafting; the review in
step 6 reads them again.

## 4. The facts

A number or a claim in copy comes from the repository: a transcluded
table, a measurement in an example's README, a guide, a test, or a live
chart on the page itself. Find it before writing it, and link it as the
claim's source. Where the page measures live (the anthill), the copy
names what the reader will see and states no recorded numbers. When a
measurement is redone, change the copy that quotes it. Code a page shows
is code that ran (`website/AGENTS.md`, "Content first").

Do not claim a feature that does not exist. Keep the pitch's words:
properties and generated runs, never formal verification.

**Check:** every number and every "passes all" has a source a reader
can open.

## 5. The structure

### How a page is ordered

- **A pitch** leads with the reader's problem and the claim that
  answers it, then one section per thing the reader must believe, each
  with its proof, then the next step.
- **A demo page** leads with the experience: the hero invites the reader
  to watch or try ("Watch a million ants forage and fight."), the demo
  follows at once, then how it works, piece by piece, then what it costs
  (after the mechanics the cost depends on), then how to run it
  yourself.
- **A guide** opens each section with the situation it solves, then the
  steps, each a sentence and the code that does it.
- **A tutorial** builds one thing; each step opens with the problem the
  last one left and ends with its output.
- **A measurement page** leads with its numbers: the lede states them,
  the sections show the programs that made them.
- **An explanation** opens each section with a `<problem>`, then
  passages, then the figure, table or claim that proves them.
- On every page, a section is a few sentences of prose and then the
  proof: code, output, a table, a live chart.

### Finding a structural gap

Sometimes the copy is right and the page still cannot say it, because
the schema has no block for it. Signs:

- a paragraph describes what a command prints, instead of showing the
  command and its output;
- a list fakes a table (term, value, value);
- two options are compared in prose, where a reader wants them side by
  side;
- a claim's proof is a sentence, where it should be a figure, a live
  chart or a program that ran;
- the same paragraph shape repeats on several pages (an objection and
  its answer, a "choose X for" list).

First look for an existing block. `schema/site.xsd` documents each
element; the ones for proof are `claim` (a statement with its
`source`), `example` (a `program` with its output), `code` (with
`role="output"`), the CLI's `job` and `step` (a command with its exit
status), `table`, `figure`, `diagram`, `live` and `demo`. Most gaps are
one of these used in the wrong place.

A real gap, a kind of thing the site says that no element names, is a
new block. It is rare and costs work in every layer, in this order
(`website/AGENTS.md`, "The schema is the vocabulary"):

1. the element and its documentation in `schema/site.xsd`: what it *is*,
   never how it looks;
2. a document in `test/invalid/` for each refusal the schema makes;
3. its HTML in `render/site.xsl`, its style in `style/site.css`;
4. its Markdown in `render/markdown.xsl`, so `index.md` and `llms.txt`
   say what the page says.

Propose it to the maintainer first when it changes what a page can say.

**Check:** read the section headings alone, top to bottom. They should
tell the page's argument in order, and each should be the finding of
its section.

## 6. The draft and its review

Write the draft in the voice. Then review every line.

**The two tests.** For each sentence: if I deleted it, would the reader
know less about tinyactors? If not, delete it or replace it with the
fact it was standing in for. Then: is it true where the reader is? On
their device, in the code the sentence introduces, of the thing the
sentence names as its subject.

**Headings carry no period.** A headline, a section's heading and a
claim's statement end without one; a question keeps its question mark.
The schema refuses a final period.

**Print the copy and read it without the code and tables:**

```sh
cd website
sxml xpath --context-item=content/pages/anthill.xml \
  'for $e in //*:headline | //*:lede | //*:heading | //*:p | //*:passage | //*:problem | //*:statement | //*:item/*:text | //*:caption | //*:description
   return local-name($e) || ": " || normalize-space($e)'
```

(`sxml` is pinned in `website/mise.toml`; outside a mise shell, prefix
the command with `mise x --`.) Copy also lives in the components and
live examples: chart titles, labels, buttons, status lines, progress
messages. Print it too:

```sh
cd website
rg -n "title: '|textContent = |'aria-label'|, '[A-Z][a-z]+ '|\`[A-Z][a-z].*\\$\\{" components examples/*/page.mjs
```

Before changing any part of a page, read the whole page as it stands
now, top to bottom: what the reader knows when they reach the part, and
what they will read next. A fix made to one line against a page you have
not reread contradicts the rest of it.

Then, for each line:

1. Apply both tests.
2. Go down `references/failures.md`, and compare with the patterns for
   its kind of element in `references/patterns.md`.
3. Read it against the page's voice rules (`/voices/<id>.xml`).
4. Check every number against its source, and every sentence before a
   block against the block.
5. Read the page's order against "How a page is ordered".

Measure the rhythm of the whole page once:
`../developer-voice/scripts/measure.py` on its built `index.md`. A spread
under 7 means sentences of one length; rewrite until short ones land the
points.

## 7. The review as a fresh reader

The author cannot read their own page fresh. Two reviews do.

**In the browser, in context.** Run `mise run serve` in `website/` and
arrive the way the reader does: from the home page, the navigation or
the link that points here, not by typing the address. Then:

- Read the first screen only, at desktop and at phone width. Do the
  headline and lede say what the page is for, and is the next action
  visible?
- Read top to bottom without skipping. Mark where you would stop
  reading, where a term arrived before its explanation, and where you
  scrolled past a block without knowing why it was there.
- Press every button, run every live example and open every link. A
  link's target must exist, be finished and answer what the link
  promised at that point of the page.
- Look at every diagram, table and code block at phone width: nothing
  cut off, nothing scrolls the page sideways.

Take screenshots with Playwright when the review is to be shown
(`deviceScaleFactor: 2`).

**A fresh agent as the reader.** Give a new agent, with none of your
context, only the page's URL (or its `index.md`) and the audience's
description from `site.xml` (`description`, `knows`), and ask:

1. What does this page say, in one sentence?
2. What would you do next, and why?
3. Where did you stop understanding, and which sentence lost you?
4. Which claim do you not believe, and what would convince you?

Compare its first answer with the message from step 2. A different
sentence means the page says something else; fix the page, not the
agent's answer.

## 8. Shipping

Run `mise run test` in `website/`: it validates the content against the
schema (the voice's limits included), builds the site, resolves every
link and anchor and runs the examples. Commit with the scope `website`,
push, deploy (`mise run deploy`, from a clean tree at `origin/main`),
and read the changed page on www.tinyactors.dev once more.

## References

- `references/failures.md`: the failures found on this site, each with
  how to fix it.
- `references/patterns.md`: real sentences before and after review, by
  kind of element. The pairs were collected when headings ended with a
  period and under the old 21-word limit: read their substance, not
  their punctuation or length.
- `../developer-voice`: how the site sounds, and the evidence for it.
