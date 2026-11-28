# stately.ai

Read 27 and 28 Nov 2026: the homepage, the XState v6 docs (why state
machines, choose XState, the tutorial, durable execution, the FAQ), the
older concept pages (state machines and statecharts, the actor model),
the LangGraph comparison and the announcement of the Stately skill
(23 Nov 2026). Median sentence 13 words; 13% over 25 words; almost no
contractions in the docs.

Stately's v6 docs are the closest of the three to our current voice:
short declarative sentences, the product as subject, no hype. What they
have that we lack is the problem in code before the answer, and a
plain "when not to".

## The problem in the reader's code, first

> Application logic is often described with values that can contradict
> each other.
>
> ```
> const request = { isLoading: false, hasData: true, hasError: true };
> ```
>
> Is this request successful or failed? The object allows both at the
> same time. Every consumer has to decide what the combination means.

Then the answer, with the same request as a machine, and:

> The request is in one state at a time. A retry event only has meaning
> in the failure state. The model documents the allowed behavior and
> rejects behavior you did not define.

**Take:** show the tangle in code the reader writes today. Ask the one
question it cannot answer. Three short sentences, then the chart.

## Saying what it does not do

> A state machine does not reduce the complexity of the behavior. It
> describes that complexity as states and transitions.

> ## When not to use a state machine
> Do not add a state machine only to replace a value. A counter, an open
> menu and a single text field may be clearer as ordinary state. Use a
> machine when the rules between states are the important part of the
> problem.

"Choose XState" gives three options and recommends the simplest when in
doubt:

> Start with events when you are unsure. If the logic becomes difficult
> to change, list its possible states and transitions. That list will
> show whether a state machine would make the behavior clearer.

**Take:** a limit stated by the product itself reads as confidence.
Give the reader a test to decide, not only a verdict.

## A tutorial that says what you will see

> In this tutorial you build one machine, part by part. Each part adds a
> single concept, ends with code you can run, and lists the expected
> output.

> What you should see: stopped, playing, paused, stopped.
>
> Sending pause while stopped does nothing. A state only responds to the
> events it declares, so the machine cannot take a transition you did
> not define.

> Run player.trigger.next() four times and the index runs off the end of
> the playlist. A guard prevents that.

**Take:** each part opens with the problem the previous part left
("runs off the end"), ends with the output to look for, and explains
the output's one surprise.

## An FAQ that answers first

> ## Is XState v6 stable?
> Not yet. v6 is in beta and is published under the beta dist-tag, so
> npm install xstate@beta gets the current release.

> ## Where are and, or, not and stateIn?
> Guards are plain JavaScript in v6, so the combinators are gone.

**Take:** the heading is the reader's question in their words. The
first two words answer it.

## The homepage: a claim the reader can picture

> ## Agents write code faster than anyone can review it.
> A diff shows which lines moved. It doesn't show that refund can no
> longer be reached, or that RETRY now loops back into payment.

**Take:** a heading that states the reader's problem as a fact, and a
second sentence with a concrete failure in the product's own terms.

## The announcement: a person with a history

> Today, we're releasing a new version of the Stately skill for Claude
> Code, Codex, Cursor, and other coding agents.

> I wrote about this back in 2019: when the states of a feature are
> tracked with separate flags, nothing stops those flags from getting
> into combinations that make no sense.

> Each line looks fine. But if the server responds without a URL, the
> early return skips setIsUploading(false), and the spinner spins
> forever.

**Take:** a post has an author who has thought about this for years.
"Each line looks fine. But …" is the move: the reader agrees, then sees
the bug.

## Avoid

The older concept page repeats one empty word:

> Simple to understand. … Simple to test. … Simple to implement. …
> Simple to maintain. State machines are simple to maintain because
> they're simple to understand, test and implement.

Its examples are a video player and a dog walk. Ours are things people
build (root `AGENTS.md`).
