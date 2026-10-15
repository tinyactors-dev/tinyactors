# Brief templates

Fill in the parts in angle brackets. Run each with `model: "sonnet"`.
Paths are relative to the repository root; give agents absolute paths.

## Roast

```
Roast the headline of a project's front page. Be blunt and specific: what
is wrong with it, who it fails to reach, what a reader thinks in the first
three seconds. Then propose 5 better headlines, each with a one-line
subhead, and say which one you would ship and why. Work only from the
context below; do not read files or browse.

## The project: staticjs
<facts.md, pasted: what it is, the language, properties, the library,
performance, the alternatives. Uses only from its Uses section.>

## The current hero, verbatim
Headline: "<...>"
Lede: "<...>"
Buttons: <...>. Next to the text is a live, editable code sample.
```

## Section

```
You are rewriting ONE section of the staticjs front page. Read exactly two
files and nothing else:
1. <root>/staticjs/site/WRITING.md, the writing guide. Follow it
   strictly, especially rules <the ones this section turns on, e.g. 1, 4,
   5, 6, 7, 8, 15 and 16>.
2. <root>/staticjs/.agents/skills/site-copy/facts.md, the only facts you
   may use.

The front page follows the guide's "The front page" order. <N> writers
work one after another, one per section; you are number <k>. Match the
voice of what is already written and don't repeat it.

## Already written
<every earlier section, verbatim, as the reader sees it>

## Your section: <name> (step <k> of the order)
<what it holds, as a short list: e.g. a noun heading; two short
paragraphs; a list of properties, each a word and one defining sentence;
one code block with results as comments>

No use cases<, except ...>. No reader-job framing. No headings that are
sentences.

Return exactly, with no commentary:
HEADING: <noun phrase>
P1: <paragraph>
...
```

## Critique

```
You are a senior human copy editor who has edited developer-tool websites
(think the SQLite, Redis, Lua and Fly.io sites). The maintainer of
staticjs says the page below "<their words>". Find every line that does,
and fix it. Work only from the text and facts here; do not read files or
browse.

## How to critique
1. First, name the LLM tells you see across the page as patterns, with a
   count of how often each appears: the rule of three; "X, not Y"
   contrasts; colon-then-list sentences; the same rhythm in every block;
   vague subjects ("someone else", "your build"); "so you can"; mirrored
   sentences; headings that are sentences; the page explaining itself;
   uniform sentence length; paragraphs that end on a neat summary. Find
   others too.
2. Then go section by section. For each flagged line: quote it, say in a
   few words why it reads as generated, and give a rewrite or CUT. Write
   like a terse engineer who built the thing. Keep every fact true to the
   facts below; never add claims. <What is fixed: the headline, the
   order.>
3. Leave out lines that are fine. Table cells and measured numbers are
   data.
4. End with the full rewritten page text, section by section, in the same
   order and structure.

## Facts (all claims must stay within these)
<facts.md, pasted>

## The page, as rendered (visible text, in order)
<the page's visible text, section by section, with [SECTION] markers>
```
