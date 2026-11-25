# The command on the website: a reference from its own help, and its voice

**Status:** Sketch (2026-11-25). The voice definition below was written by
Gemini (gemini-4-hydrogen, thinking high) from the site's voice, the
copywriting skill, the command's help and a survey of eleven CLI reference
sites, in two passes: a draft, then a revision against eight review findings
(contradictory period rules, two rewrites that bent facts, a wrong
placeholder). Nothing here is applied yet; the maintainer decides the
points under "Decisions for the maintainer".
**Touches:** `website/` (content, schema, render, build), `copywriter/`
(a second voice), `tinyactors-core/tools/tinyactors/program.c` (the help
text, examples with captions), `tinyactors-core/docs/cli.md`.

## Whom it serves, and who pays

- **First:** the workflow developer who tests and the agent-platform
  engineer, who reach for `tinyactors lint`, `diff --breaking --exit-code`
  and `run` in CI. Today the site does not mention the command at all.
- **Already answers them:** `tinyactors help <command>` and
  `tinyactors-core/docs/cli.md`, on GitHub only.
- **Pays:** the website's build (one more input: the released command's
  help as XML), the help text in `program.c` (rewritten once to the voice),
  and the copywriter (a voice per page kind). Nothing per event or per actor.
- **Out of scope:** a man page, shell completions, translating the guides.

## What the website lacks today (review, 2026-11-25)

1. **No page names the command.** `content/` has no install line for it
   (`mise use github:tinyactors-dev/tinyactors-core`), no reference and no
   navigation entry. 0.4.0 makes it the release's headline.
2. **The C install on the home page** is the source tarball; 0.4.0 also
   ships the command as `tinyactors-core-<version>-<target>.tar.gz` per
   target, which a reader without a C toolchain can use.
3. **Guides that would show it:** testing (`lint`, `path`, `diff` in CI),
   snapshots and inspection (`inspect`, `actors`), migration (`diff`'s
   "migration" answer), charts (`fmt`, `graph -f svg`).
4. **The schema already has the page shape:** `<reference>` (no hero, a
   headline, then blocks) and the page kind `reference`.
5. **The help's Markdown wraps a description holding `<` in one code span**
   (`tinyactors help compile` prints its whole description in backticks).

## The reference comes from the command

`tinyactors help -f xml` lists the commands with their summaries, and
`tinyactors help <command> -f xml` gives a command's name, summary, usage,
description, options (name, short form, placeholder, description,
choices), program options and examples. The website's build runs the
command of the release it documents (pinned like the npm packages, never
the working tree) and turns each answer into one page under
`/cli/<command>/` with a grouped index at `/cli/`: usage, description,
options as a definition list, examples, exit status. The words come only
from the command, so the terminal and the site cannot disagree, and the
copywriter's export for those pages is a change to `program.c`.

Two things the help lacks: a caption per example (today an example is a
bare command line) and a per-command exit status (today in `docs/cli.md`
only). Both would become fields of the command table in `program.c`.

## Voice definition

You write and review copy for the `tinyactors` CLI across three surfaces: help text inside the program, generated `reference` pages and hand-written `how-to` guides.

The voice: short, literal and consistent across terminal and web.

### Summaries
- Write every command summary as an imperative verb phrase in sentence case with no trailing period (`Compile charts or a system manifest into an image`).
- Keep the summary on one line: 3 to 12 words and under 72 characters.
- State what the command does or returns, never what it is.

### Descriptions
- Open in third-person present with what the command reads and produces (`Compiles...`, `Reads...`, `Compares...`).
- State experimental status or destructive effects in the first sentence when they apply.
- Follow in short sentences (at most 21 words each) for resolution rules, side effects and output destinations.
- Keep `reference` descriptions free of tutorials; move workflows to `how-to` guides.

### Option descriptions
- Write each option description as an imperative sentence ending with a period (`Search DIR...`).
- Add a second sentence only to state a side effect or hazard, ending it with a period.
- Put constraints after the description in fixed clauses, each ending with a period, in this order when present: `Values: a, b or c.` `Default: x.` `Env: VAR.` `Repeatable.`
- Keep `Env: VAR.` only as a future convention; `tinyactors` reads no environment variables in current CLI help.
- When an option has a `Values` column in CLI help, the generated reference page renders those choices in the `Values:` clause so the description does not repeat them. Gloss non-obvious choices inside `Values:` in under six words each.

### Arguments and placeholders
- Write required arguments in angle brackets (`<command>`), optional ones in square brackets (`[INPUT...]`) and repeatable ones with `...`.
- Name placeholders in uppercase (`FORMAT`, `DIR`, `NAME`, `THEME`, `N`, `REALM`) and use the exact same placeholder in usage, option headings and option text.

### Examples
- In program help and `reference` pages, write a one-line imperative caption ending with a colon (`Fail if a change breaks running actors:`) followed by the bare command without a `$` prompt and without output.
- In `how-to` guides, add `#` comments only when a step needs explanation, and show command output only when captured verbatim from that command.

### Exit statuses
- Document exit codes in an `Exit status` section whenever a command returns non-zero codes beyond invalid arguments (`1: changes break running actors.`).
- Write each code condition in present tense as a sentence ending with a period.

### Error and diagnostic messages
- Name the command, file, state, event or rule that failed and state why in one plain present-tense sentence.
- Write with no apologies, no exclamation marks and no guesses at intent.

### Terminology, person and tense
- Use these terms with their exact meaning and no synonyms: `chart`, `image`, `system manifest`, `snapshot`, `blob`, `host`, `actor`, `state`, `transition`, `event`, `segment`, `migration`.
- Write `tinyactors` in lowercase.
- Use present tense everywhere; keep help and `reference` impersonal (never "you"), and use "you" only in `how-to` guides.

### Never write
- Never write "formal verification", "will" for command behaviour, metaphors, unmeasured adjectives, hype words, emoji or serial commas.
- Never invent an option, exit code, environment variable, number or output line not verified in the program.

---

## Before and after

| Surface | Before | After | Rule applied |
| --- | --- | --- | --- |
| Summary (`inspect`) | what an image, a snapshot or a blob holds | Show what an image, a snapshot or a blob holds | Imperative verb phrase, sentence case, no period |
| Summary (`states`) | each chart's states, as a tree in document order | List each chart's states as a tree in document order | Imperative verb phrase, sentence case, no comma |
| Summary (`diff`) | what changed between two versions, and what it does to running actors | Compare two versions and show what changes for running actors | Imperative verb phrase, sentence case, no period |
| Description (`compile`) | Compiles SCXML charts (the first is the image's default boot chart) or one \<system\> manifest into a .tai image, reading each static src relative to the file that names it, then in each -I directory. The answer lists the diagnostics and what the image holds; when the image goes to standard output (-o -), the answer goes to standard error. | Compiles SCXML charts or one \<system\> manifest into a .tai image; the first chart is the default boot chart. Reads each static src relative to the file that names it, then in each -I directory. The answer lists diagnostics and what the image holds. When -o - writes the image to standard output, the answer goes to standard error. | Third-person present, sentences under 21 words, all facts kept |
| Option (`program --format FORMAT`) | write the answer as markdown (the default), json or xml; graph also as svg | Write the answer in FORMAT; only graph supports svg. Default: markdown. | Imperative sentence with period; generated page renders `Values` column separately |
| Option (`compile --include DIR`) | look for a src here when it is not beside the file naming it; repeat it for more, searched in order | Search DIR for a src when it is not beside the file naming it, in the order given. Repeatable. | Imperative sentence with period, exact lookup rule, `Repeatable.` |
| Option (`compile --symbol NAME`) | the C array's name with --emit c (default: tinyactors_image) | Set NAME as the C array's name with --emit c. Default: tinyactors_image. | Imperative sentence with period, `NAME` placeholder, `Default:` clause |
| Option (`graph --theme THEME`) | -f svg: the colours, light, dark, or auto (the default): the reader's preference | Set the SVG colour THEME with -f svg. Values: auto (reader preference), light or dark. Default: auto. | Imperative sentence with period, glossed `Values:`, `Default:` clause |
| Option (`run --seed N`) | Math.random's seed (default: 1; 0: the engine's own, different on every run) | Set Math.random's seed to N; 0 picks a new engine seed each run. Default: 1. | Imperative sentence with period, `N` placeholder, `Default:` clause |
| Option (`run --realm REALM`) | charts share one realm unless they declare globals (auto, the default), or each session has its own (isolated) | Set realm sharing across charts to REALM. Values: auto (shared unless globals are declared) or isolated (one per session). Default: auto. | Imperative sentence with period, glossed `Values:`, `Default:` clause |
| Example (`compile`) | - tinyactors compile order.scxml -o - \| tinyactors inspect - | Compile to standard output and inspect the image:<br>`tinyactors compile order.scxml -o - \| tinyactors inspect -` | One-line caption with colon, bare command, no bullet |
| Example (`diff`) | - tinyactors diff --breaking --exit-code deployed.tai order.scxml | Fail if a change breaks running actors:<br>`tinyactors diff --breaking --exit-code deployed.tai order.scxml` | One-line caption with colon, bare command, no output |

---

## Decisions for the maintainer

- **Backtick wrapping on `<` in Markdown help:** in `cli-help.md`, the program's Markdown help wraps `compile` and `graph` descriptions in backticks because they contain `<system>` or `<scxml>`, turning the whole paragraph into one code span. Decide whether the voice should avoid literal markup in descriptions (writing `system manifest` and `SCXML charts`) or the renderer should escape or inline-code only the tag.
- **Imperative summaries everywhere:** convert all 18 command summaries to imperative verb phrases ("Show...", "List...", "Compare...") instead of mixing verbs with noun phrases. Alternative: keep noun phrases for read-only commands.
- **Sentence case in terminal help:** capitalize the first word of every summary and option sentence in CLI help. Alternative: keep lowercase fragments in terminal output and capitalize only on the web.
- **Structured trailing option clauses:** move inline defaults, glossed choices and repetition notes into `Values:`, `Default:` and `Repeatable.` clauses, and render unglossed `Values` columns on the web without repeating them in the description. Alternative: keep inline parentheticals inside option text.
- **Captions on reference examples:** add a one-line caption above every example in `tinyactors help` and generated pages. Alternative: write captions only on web pages.
- **One page per command and `Exit status` section:** generate 18 command pages from a task-grouped index, and extract non-zero exit behaviour (`diff --exit-code`, `lint --deny`) into an `Exit status` section. Alternative: keep one long page or leave exit codes inside option text.

---

## Where it departs from other CLIs

- **Follows Cargo and mise on imperative summaries and captions, rejects Biome's third-person summaries.** Every summary opens with an imperative verb while descriptions use third-person present, and every reference example carries a one-line caption without output.
- **Follows Cargo on an explicit `Exit status` section, rejects Cargo's and git's ALL-CAPS headings.** Generated pages use plain sentence-case headings (`Usage`, `Options`, `Exit status`, `Examples`) matching `site-voice.md`.
- **Follows mise on one page per command and definition-list options, rejects uv's single page and Docker's tables.** Separate pages and definition lists stay scannable on narrow screens without cramping option text.
- **Rejects gh's `$` prompts and reference-page output.** Reference examples stay bare copyable commands; real output appears only in `how-to` guides when reproduced verbatim.
- **Follows Deno and ripgrep by separating guides from reference, rejecting git's inline tutorials.** Generated reference stays strictly factual while workflows live in `how-to` guides.
