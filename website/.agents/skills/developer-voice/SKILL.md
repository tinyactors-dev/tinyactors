---
name: developer-voice
description: How www.tinyactors.dev should sound, extracted from rivet.dev, stately.ai and effect.website and measured against our own pages. Use when writing or revising any page's prose, when a page reads flat, dense or like a specification, when choosing how a pitch, guide, tutorial, concept essay, comparison, FAQ or release post should sound, and before changing the voices in content/site.xml. The copywriting skill decides what a sentence says; this skill decides how it sounds. references/ holds annotated excerpts from each site, the measurements and rewrites of our own pages.
---

# The developer voice

The voice of a senior engineer explaining a design to a peer at a
whiteboard. They start from a problem the peer has had. They show the
code before the concept. They say plainly what the thing costs and when
to use something else. They have an opinion about what makes software
hard, and they say it.

This skill is about how a page sounds. What a sentence must say, and
whether it is true, is the `copywriting` skill's job (`../copywriting`).
Run both.

## Status: a proposal until the maintainer adopts it

`content/site.xml`'s `<voices>` are the site's rules, and
`website/AGENTS.md` says to change a rule there. Several moves below
contradict those rules (listed in "Conflicts with the current voices").
Until the maintainer changes `site.xml`, **site.xml wins** where the two
disagree, and this skill is evidence for the change. Use the moves that
do not conflict now.

## What we measured

Prose from 35 pages, about 47,000 words, measured on 28 Nov 2026. Code,
headings and list items are counted apart from sentences.
`references/measurements.md` has the full table, the pages and the
script (`scripts/measure.py`), which runs on our built pages too.

| | Rivet | Stately | Effect | tinyactors |
|---|---|---|---|---|
| words per sentence, median | 11 | 13 | 12 | **15** |
| spread (standard deviation) | 7.1 | 9.4 | 10.2 | **6.6** |
| sentences under 8 words | 28% | 18% | 31% | 21% |
| sentences over 25 words | 5% | 13% | 11% | **2%** |
| sentences with "you" | 18% | 11% | 19% | 15% |
| sentences with "we", "our" | 2% | 2% | 13% | **0%** |
| sentences with a contraction | 8% | 4% | 17% | **0%** |
| questions | 3.1% | 0.2% | 4.5% | 0.2% |
| sentences with a number | 11% | 7% | 6% | **18%** |

Our sentences are not too long. They are **all the same length**. The
21-word cap pushes every sentence to 13 to 20 words, so the prose has no
rhythm: no short sentence that lands a point, no long one that carries
a cause to its effect. With no contractions, no "we" and no questions,
the result reads like a specification. Our numbers are our strength:
nearly one sentence in five carries one, more than any of the three.

## The moves

Each move names where it comes from. `references/<site>.md` has the
excerpts.

1. **Open with a problem the reader has had, in their own code.**
   Stately opens "Why state machines?" with
   `{ isLoading: false, hasData: true, hasError: true }` and asks which it
   is. Effect's homepage lists pains in the reader's words ("That
   Promise.all call? One failure and everything crashes."). Rivet's chat
   guide starts with what a chat room needs. Show the tangle first, then
   the chart that removes it.

2. **Name the enemy, then concede what it is good for.** Rivet's
   architecture essay names the stack nobody chose ("Nobody got fired
   for adding Kafka, Redis, and RabbitMQ to the stack."). Stately says
   what a machine does not do: "A state machine does not reduce the
   complexity of the behavior. It describes that complexity as states
   and transitions." Our enemy is complexity. Say what it costs the
   reader, and say when distribution is worth it.

3. **Vary the length on purpose.** A short sentence lands the point. A
   longer one carries the cause to its effect. Stately: "Is this request
   successful or failed? The object allows both at the same time. Every
   consumer has to decide what the combination means." Aim for a third
   of the sentences under 8 words and a few over 25, as the three sites
   have.

4. **Join cause and effect in one sentence.** Rivet's guides explain
   each line of code by its consequence: "Every sendMessage for a room
   runs in the same Actor, the only writer to that room's database. The
   AUTOINCREMENT id therefore gives the room one order without any
   locks." Use "so", "because", "therefore", "which is why".

5. **Say what you will see.** Stately's tutorial ends each part with
   "What you should see: stopped, playing, paused, stopped." Effect's
   module posts open with "By the end of this post, you'll know".
   Our tutorials print output already. Say in one sentence what to look
   for in it.

6. **Number with its conditions, and the conditions with their bias.**
   Rivet sources every hero number and states its method under it ("The
   sandbox figure is a provider's documented default allocation … not
   measured memory use."). It says which way a benchmark leans:
   "Durable Objects are timed inside Cloudflare, which favors them."
   Keep our numbers. Add the condition and the bias.

7. **Compare with "Choose us for / Choose them for".** Rivet's compare
   pages give both lists. Each entry is a one-line verdict followed by
   one line of reason: "No: 128 MB of memory per isolate. Large
   in-memory state has to be split up." The other side gets real
   entries ("Apps already on Cloudflare Workers").

8. **Say "we" when the team decided, erred or measured.** Effect: "We
   missed it in review. That's on us." and "What this means for you:
   Nothing in your code changes." A release post, a decision and a
   benchmark have authors. The product stays the subject of how-to and
   reference prose.

9. **Contractions in prose that talks; none in reference.** Effect's
   posts and Rivet's essays use "don't", "it's", "you'll"; Stately's
   reference does not. Our pitch, tutorials and posts should; our
   reference and guides' option lists need not.

10. **Answer objections in the reader's words.** Effect's "Myths about
    Effect" quotes each objection as the reader says it ("Effect will
    make your code 500x slower!"), concedes what is true ("Effect does
    perform 500x slower if you are comparing…") and then reframes it.
    Stately's FAQ answers "Is XState v6 stable?" with "Not yet." first.

11. **One concept, one example that grows.** Stately's tutorial grows
    one media player across eight parts. Effect's cluster series builds
    one bot across posts. Our chat tutorial already does this. Do it in
    the guides as well: one order, carried through every section.

12. **Put a person in posts.** Effect's module posts are signed and
    have humor and a running joke; Stately's announcement says "I wrote
    about this back in 2019". Keep personality for posts and essays,
    never for reference or how-to pages.

## Registers by page kind

| Page kind | Sounds like | Sentences | "you" / "we" | Borrow from |
|---|---|---|---|---|
| Pitch (home, product) | a confident peer with an opinion | short, with a few long ones | you; no we | Effect's pain list, Rivet's sourced numbers |
| Concept essay (why, architecture) | a chapter of a book that argues | longer, varied, with the problem first | you and we | Rivet's "radically simpler architecture", Stately's "Why state machines?" |
| Tutorial | someone at your side | 2 or 3 per step; "what you should see" | you | Stately's tutorial |
| How-to guide | a colleague's runbook | short; cause joined to effect | you | Rivet's guide bullets |
| Reference | a table | fragments allowed | neither | Stately's v6 reference |
| Compare | a fair judge | verdict, then one line of reason | you | Rivet's compare pages |
| FAQ, myths | a straight answer | the answer first, then why | you, we | Stately's FAQ, Effect's myths |
| Release post, essay | a named author | conversational, contractions | we, I | Effect's release posts and module series |

## What not to borrow

Each site has weaker, older pages. The newer ones are tighter.

- Rivet's crash course: "Blazing-Fast Reads & Writes", "Infinitely
  Scalable", "no cold starts". Its own homepage measures a 4.99 ms cold
  start. Adjectives that a number contradicts are worse than none.
- Stately's older concept page: "Simple to understand. … Simple to test.
  … Simple to implement. … Simple to maintain." Four sentences, one word.
- Effect's v3 "vs Promise": "In this guide, we will explore…", a lede
  that lists what is coming (copywriting failure 2). Its myths page runs
  sentences together with commas.
- Stately's examples of a video player and a dog walk, and any door or
  light switch. Our examples are things people build (root `AGENTS.md`).
- Personality in reference or how-to pages.

## Conflicts with the current voices

The repository's rules ask that a conflict be named, not resolved
quietly. Each row is a rule in `content/site.xml` (or the copywriting
skill), what the evidence says, and the recommended change. The
maintainer decides.

| Current rule | What the three sites do | Recommendation |
|---|---|---|
| `literal/paragraphs`: at most 21 words a sentence, one or two sentences | a third under 8 words, 5 to 13% over 25; no fixed cap | Replace the cap with a range and a spread: most sentences under 20, a third under 8, a few up to 30 |
| copywriting failure 6, "What it is not": avoid negatives | contrast carries their positioning ("without the lock-in", "does not reduce the complexity", "not architectures you rebuild") | Allow a negative that names the reader's current tool or belief; that is how an enemy is named |
| `literal/plain`: no metaphors | used sparingly and concretely ("They're like mini-servers", "Like AWS Lambda, but with memory") | Allow one concrete analogy per page, to a thing the reader runs; keep the ban on decorative metaphor |
| `tutorial/reader` and `deep-dive/paragraphs`: no questions | rare but present (3 to 4.5% on Rivet and Effect), as the reader's own question | Allow a question that quotes the reader (an objection, a FAQ heading); keep the ban on rhetorical questions |
| no rule, and in practice no "we", no contractions | "we" for decisions, mistakes and measurements; contractions in prose that talks | Add a rule per register (table above) |

The rules that stay: headings without a period, actions as a verb and an
object, exact SCXML terms, lowercase tinyactors, no serial comma, never
"formal verification", every claim with its source, never naming the
reader's role, never explaining how the site is made.

## Checklist for a page

1. Does the page open with a problem the reader has had, in code or in
   their words?
2. Read the sentences aloud. Is there a short one that lands each point?
   Run `scripts/measure.py` on the page: a spread under 7 means the
   rhythm is flat.
3. Does every comparison give the other side real entries?
4. Does every number say under what conditions, and which way it leans?
5. Does a tutorial step say what you will see?
6. Is the register the page kind's (table above)?
7. Then run the copywriting skill's review.

## References

- `references/rivet.md`: compare pages, guides, the architecture essay, sourced numbers.
- `references/stately.md`: problem-first docs, the tutorial, FAQ, the announcement post.
- `references/effect.md`: the pain list, onboarding, myths, release posts, the module series.
- `references/measurements.md`: the method, the full table, the pages read.
- `references/rewrites.md`: our own copy before and after, using only facts our pages state.
