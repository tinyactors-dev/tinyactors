# Our pages, before and after

Each "before" is copy from www.tinyactors.dev on 28 Nov 2026, written
in the old voice. Each "after" is in the developer voice and uses only
facts that page, or a page it links, already states. The moves named
are SKILL.md's.

Read the copywriting skill's `references/patterns.md` for the
substance of each element. These pairs are about sound.

## Home: the lede

Before:

> tinyactors runs millions of actors inside your Node.js, Python,
> browser or C process, with no separate server. Each actor runs a
> composable statechart of states, events and transitions.

Two sentences of 20 and 12 words, both about the product. The reader's
problem is absent, and "composable statechart" comes before anything
explains it.

After (moves 1, 2, 3):

> An order's logic ends up in a queue, a worker, a cron job and three
> status columns, and nobody owns it. Put it in one chart instead, run
> by one actor in your own process. You can read every state and
> transition it has. You can test its timeouts in milliseconds.

The first sentence is the reader's world (a scenario, not a claim about
tinyactors). The rest are facts the home page states: in your own
process, every state and transition written down, a simulated clock.
Sentences of 21, 13, 10 and 7 words.

## Architecture: the first problem

Before:

> An application handling order workflows needs explicit state machines
> without running a separate actor cluster or letting workflow logic
> open network sockets directly. A library that parsed XML or opened its
> own sockets behind the application's back would take that I/O out of
> the team's hands.

The problem is stated in the abstract ("an application handling order
workflows"), in two long sentences of equal weight.

After (moves 2, 3, 4; the negatives name what the reader's engine does,
which `reference/negatives` allows):

> Most workflow engines want your I/O. They open their own connections,
> run their own timers and keep your state on their own servers. The
> engine inside a tinyactors host does none of that: it never opens a
> socket, writes a file or sleeps. When an actor sends a message, the
> host hands your code an effect, and your code decides what happens.

The negative names what the reader's current engine does. That is the
enemy, stated as the reader knows it.

## Hibernation guide: the lede (keep it)

Before:

> An actor that waits days for its next event can wait as bytes in your
> database instead. Two million actors ran with at most 1000 in memory,
> about 150 bytes of memory each.

This is already the voice: the reader's situation, then a measured
number. One change would sharpen it (move 3):

> Most actors are waiting. An order waits for its payment, a session for
> its user. Hibernation keeps a waiting actor in your database instead
> of in memory: two million actors ran with at most 1000 in memory,
> about 150 bytes each.

The first sentence moves up from the guide's second paragraph. Four
words land the point before the number.

## Chat tutorial: a step (keep the problem, add what to look for)

Before:

> A user who stops typing sends no event, so nothing marks them away.
> The user's chart restarts a 300-second timer on each activity, and
> when it fires, the chart moves to away.

Problem first, cause joined to effect: keep it. What the step lacks is
Stately's closing line (move 5): one sentence after the output that
names what to look for in it, and the one surprise it shows. Write it
from the step's own `output.txt`, never from memory.

## When to use: a comparison

Before (a heading and two sentences):

> ### Temporal runs a server; tinyactors runs in-process
> Self-hosted Temporal runs Temporal Server and persistence stores on
> Docker, Kubernetes or by hand.
> Your program creates a tinyactors host object directly for tests and
> runtime, handling journaling and cross-host transport.

The reader gets no reason to choose either. "Handling journaling and
cross-host transport" sounds like a feature and is a cost.

After (move 7, Rivet's shape):

> ### Temporal runs a server; tinyactors runs in your process
>
> **Choose tinyactors for**
> - Workflows you test in milliseconds. A simulated clock runs an hour's
>   timeout in no time.
> - No server to operate. Your program creates the host, in tests and in
>   production.
>
> **Choose Temporal for**
> - Workflows that must survive a machine. Temporal Server and its
>   persistence store keep them; a tinyactors host keeps only what your
>   program saved.
> - Placement and failover across machines. With tinyactors, your
>   program decides where each host runs.

Each fact comes from the same page ("A crash stops memory and keeps
saved state", "Managed clustering and failover", the simulated clock).
The other side gets the reasons the page already admits.

## A myth (a page we do not have yet)

Effect's shape (move 10), answered with facts from When to use:

> ## "One process is a single point of failure."
> True: a crash stops every actor in it. What survives is what your
> program saved: the blobs of hibernated actors in your store, and the
> last snapshot. Host.restore runs that snapshot in another process,
> with each actor's variables, queued events and pending timers. Your
> program replays what came after it from its own journal.

The objection is quoted as a reader would say it. The first word
concedes. The rest is the mechanism, with no adjective.

## A release post (a page we do not have yet)

Effect's register (moves 8, 9, 12) for a post's opening, as a pattern.
The facts come from the release's changelog, never from memory:

> We released @tinyactors/node 0.4.0 on 19 November. [What changed,
> in one sentence, from the changelog's summary.]
>
> ## What this means for you
> [The changelog's "Upgrading" text, as one or two sentences.]
