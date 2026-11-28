# effect.website

Read 27 and 28 Nov 2026: the homepage, the v4 onboarding, "Why Effect?",
"Myths about Effect", "Effect vs Promise", "Coming from ZIO", the post
on Effect 4.0's first week and the Module of the Week post on Cluster.
Median sentence 12 words, the widest spread of the four sites (31% under
8 words, 11% over 25), "we" in 13% of sentences, contractions in 17%.

Effect is the model for us in two ways: it sells a library with no
infrastructure, and it fights the same enemy (async code nobody owns).
It writes in three registers: a confident pitch, plain docs and
personal posts.

## The pain list

The homepage pairs each pain with its fix. The pains are in the reader's
voice, short and a little exasperated:

> Try-catch blocks everywhere but you still do not know how things can
> fail.
> Another decorator. Another magic string. Another runtime error.
> That Promise.all call? One failure and everything crashes.
> Network failed? Try again later. But when, and how many times?
> No observability. Production is on fire and you do not know why.

Each is followed by what Effect gives ("Structured concurrency with
fibers. Parallel execution with limits. Automatic resource cleanup.").

**Take:** write the enemy as the reader experiences it, in fragments
and questions they would say themselves. One line each.

## The thesis in two short paragraphs

> TypeScript is excellent at describing your data, but it says almost
> nothing about your programs: a function's signature doesn't tell you
> what can fail, what dependencies it needs, or whether it can be safely
> retried, timed out, or interrupted.

> Because programs are values, they compose: retries, timeouts,
> concurrency, resource handling, and tracing are operators you apply,
> not architectures you rebuild.

> When coding agents write a growing share of your software, the
> bottleneck moves from writing code to trusting it.

**Take:** concede what the reader's tool does well ("excellent at
describing your data"), then name the gap. The contrast "operators you
apply, not architectures you rebuild" names the enemy in four words.
The agent sentence is a shift the reader recognises, not a buzzword.

## Myths: the objection in the reader's words

> ## Effect will make your code 500x slower!
> Effect does perform 500x slower if you are comparing: `1 + 1` to
> `Effect.runSync(Effect.zipWith(…))`. The reason is one operation is
> optimized by the JIT compiler to be a direct CPU instruction and the
> other isn't. In reality you'd never use Effect in such cases.

> ## Effect is impossible to learn, there are so many functions and
> modules!
> True, the full Effect ecosystem is quite large … you can safely start
> using Effect knowing just 10-20 functions.

**Take:** quote the objection as a reader would post it. Concede what is
true ("True,", "does perform 500x slower if"), then give the number or
the reframe. Our candidates: "One process is a single point of
failure", "SCXML is from 2015", "XML is unreadable", "An agent can't
write good charts".

## A release post that owns a mistake

> A week ago we released Effect 4.0. Since then, almost 30 people have
> landed 150 commits on main, and we've shipped two patch releases.

> ## What we got wrong
> That implicit default bit us. … We missed it in review. That's on us.

> ### What this means for you
> Nothing in your code changes.

**Take:** "we" is for what the team did. Admitting a mistake in two
short sentences, then "what this means for you" in one, is the most
trusted paragraph a project can write.

## A series with a person in it

> Last week, in a moment of unbridled confidence that I can no longer
> explain, I offered to write a series on Effect Cluster.

> DadBot makes one promise. He never tells the same joke twice in the
> same chat. … Keeping that promise turns out to be a distributed
> systems problem, which is convenient, because that's exactly what
> Effect Cluster was built for.

> Both handlers read DadBot's joke list before either one saved, so both
> picked the same joke. We've already broken the sacred DadBot promise.

**Take:** in a post, a running example with one promise to keep turns a
concurrency bug into a story. The bug is still explained exactly.
Personality belongs here and in essays, never in reference.

## Avoid

> In this guide, we will explore the differences between Promise and
> Effect … We'll discuss their type safety, creation, chaining, and
> concurrency.

A lede that lists what is coming (copywriting failure 2).

> Depends what you mean by performance, many times performance
> bottlenecks in JS are due to bad management of concurrency.

Comma splices: the myths page runs sentences together. The tone is right
and the punctuation is not.
