# Measurements

How the numbers in `SKILL.md` were made, and how to measure a page of
ours against them.

## The pages

Rendered in Chromium on 28 Nov 2026. Each page's main content was dumped
as Markdown (headings, paragraphs, list items; code fenced). Pages whose
content is not in `<p>` elements (Rivet's and Effect's homepages, our
When to use, Architecture and CLI and libraries) were dumped as lines of
text instead, which counts some interface labels as short sentences.

- **Rivet (10):** the homepage, /actors/, the crash course, the
  chat-room guide, "A Radically Simpler Architecture", design patterns,
  compare vs Cloudflare Durable Objects, compare vs Temporal, the blog
  posts "SQLite-on-the-server is misunderstood" and "Writing docs for
  AI".
- **Stately (10):** the homepage; XState v6 "Why state machines?",
  "Choose XState", the tutorial, durable execution, the FAQ; the older
  "What are state machines?" and "The actor model"; "Coming from
  LangGraph"; the post "Find the state machines hiding in your code".
- **Effect (8):** the homepage, the v4 onboarding, "Why Effect?",
  "Myths about Effect", "Effect vs Promise", "Coming from ZIO", the post
  "Effect 4.0: the first week", the Module of the Week post on Cluster.
- **tinyactors (7):** the home page, When to use, the chat tutorial,
  Architecture, the hibernation guide, Property testing, CLI and
  libraries.

## The table

| | Rivet | Stately | Effect | tinyactors |
|---|---|---|---|---|
| sentences | 479 | 601 | 375 | 523 |
| words per sentence, median | 11 | 13 | 12 | 15 |
| spread (standard deviation) | 7.1 | 9.4 | 10.2 | 6.6 |
| % under 8 words | 28.4 | 17.8 | 31.2 | 20.5 |
| % over 25 words | 4.6 | 12.8 | 10.9 | 2.1 |
| % with you | 18.0 | 11.3 | 19.2 | 15.1 |
| % with we, our, us | 2.3 | 1.8 | 13.3 | 0.0 |
| % with a contraction | 7.7 | 3.8 | 16.8 | 0.2 |
| % questions | 3.1 | 0.2 | 4.5 | 0.2 |
| % with a number | 10.6 | 7.0 | 5.6 | 18.4 |
| % with a hype word | 2.3 | 0.5 | 5.6 | 0.0 |
| headings | 116 | 110 | 59 | 44 |
| heading words, mean | 3.9 | 5.8 | 3.2 | 4.6 |

"Hype word" is a short list (blazing, seamless, powerful, effortless,
magical, simply, just, easy, simple, instant, radically, incredible,
robust, production-ready). "Contraction" leaves out the possessive 's.

## What the numbers say, and what they do not

- **Spread is the clearest difference.** Ours is the lowest (6.6). A
  flat spread is what a hard cap of 21 words produces: few short
  sentences, almost no long ones.
- **"We" and contractions are near zero on our pages**, by rule. The
  three sites use them where a person speaks (posts, essays, myths), not
  in reference.
- **Numbers are our strength.** 18% of our sentences carry one.
- These are counts, not judgments. A page can score like Effect and
  still say nothing. The copywriting skill's test (would the reader know
  less without this sentence?) comes first.

## Measuring a page of ours

The site publishes each page's Markdown at `<page>/index.md`. Fetch the
pages to compare into a directory, one directory per label, and run:

```sh
mkdir -p /tmp/voice/home
curl -sS https://www.tinyactors.dev/index.md -o /tmp/voice/home/index.md
python3 website/.agents/skills/developer-voice/scripts/measure.py home=/tmp/voice/home
```

Or measure the whole built site after `mise run build` in `website/`:

```sh
python3 website/.agents/skills/developer-voice/scripts/measure.py site=website/build/site
```

Compare a page before and after a rewrite: the spread should rise above
7, and the share of sentences under 8 words toward a quarter.
