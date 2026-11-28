# rivet.dev

Read 27 and 28 Nov 2026: the homepage, /actors/, the crash course, the
chat-room guide, "A Radically Simpler Architecture", the design patterns
page, the compare pages against Durable Objects and Temporal, and two
blog posts. Median sentence 11 words; 28% of sentences under 8 words.

Rivet's strongest writing is in its newest pages: the compare pages and
the homepage. Its crash course is older and reads like a feature list.

## Numbers with their conditions and their bias

The hero has three numbers, each with its method directly under it:

> 4.99 ms p99 cold start. Round trip from sending a request to the first
> reply from a newly created Rivet Actor: 4.99 ms p99, on a three-zone
> cluster. The sandbox figure is a p99 from a third-party sandbox
> benchmark run on October 30, 2026. See the benchmark.

> Storage at rest using AWS list prices in us-east-1: S3 Standard at
> $0.023 per GB-month, compared with three Amazon EBS gp3 copies at
> $0.08 per GB-month each. Excludes request and transfer costs.

The compare page says which way its own benchmark leans:

> From our open benchmark in one region. Durable Objects are timed
> inside Cloudflare, which favors them.

**Take:** after a number, say what was measured, where, and what was
left out. Saying which way the setup leans earns the reader's trust in
the rest of the page.

## Choose us for, choose them for

> # Durable Objects, without the lock-in.
> Rivet runs the same Actor model in your own Node.js or Bun process, on
> any cloud, with realtime, cron and lifecycle hooks built in.
>
> ## Choose Rivet for
> - Yes: Any cloud, your VPC or on-prem
> - Yes: Actors that need over 128 MB
>
> ## Choose Durable Objects for
> - Yes: Apps already on Cloudflare Workers
> - Yes: Cloudflare's edge network
> - Yes: Low-level primitives

Each detailed row is a verdict, then one line of reason:

> No: 128 MB of memory per isolate. Large in-memory state has to be
> split up. — Yes: No fixed memory cap. Sized by the machines you run.

> A task queue suits slow, occasional jobs. Agents run thousands of fast
> steps, and Temporal adds a round trip and a bill to each one.

**Take:** the headline names the rival and the one thing the reader
fears about it. The rival gets real reasons to be chosen. Every "no" has
a consequence the reader can picture.

## A guide that explains each line by its consequence

> A chat room needs three things: one place that orders every message in
> the room, history that survives restarts, and instant delivery to
> everyone who is connected. A single Rivet Actor per room gives you all
> three.

> Every sendMessage for a room runs in the same Actor, the only writer
> to that room's database. The AUTOINCREMENT id therefore gives the room
> one order without any locks.

> The insert uses ? placeholders, so user text never ends up inside the
> SQL string. Keep every query parameterized.

Then, after the example, what it leaves out, as plain instructions:

> The example has no authentication. Any client can join any room by
> knowing its name, and sender is a plain action argument, so anyone can
> post as anyone. In production, check a token in onBeforeConnect, …

**Take:** state what the problem needs before the code. Under the code,
one bullet per line that matters, each joining the line to its effect.
Then say what a production version adds.

## An essay with an enemy

"A Radically Simpler Architecture" is the closest thing on any of the
three sites to our position. It argues against accumulated
infrastructure:

> The typical backend architecture follows a familiar pattern. A web
> server connects to a database. Traffic grows, so you add Redis for
> caching. You need async processing, so you add Kafka. You need
> coordination, so you add distributed locks.

> Worse, these are bugs you can't unit test for. They're emergent
> behaviors that only appear under load when it matters most.

> Yet it's accepted as the way things must be. Nobody got fired for
> adding Kafka, Redis, and RabbitMQ to the stack.

It concedes what remains hard:

> Actor-owned state gives each Actor a clear ownership boundary. Network
> failures, retries, and concurrent application work still need to be
> handled.

**Take:** tell the reader's history as a sequence of reasonable steps
that ended badly, in short sentences with "so". Name the familiar tools.
Then concede what the alternative does not fix.

## Design patterns with an anti-pattern

> The core pattern is creating one actor per entity in your system.

> DocumentWordCount: Too granular, should be part of Document actor

**Take:** a pattern page names what not to do, with a concrete wrong
example.

## Avoid

The crash course, an older page, contradicts the homepage's measured
numbers with adjectives:

> Blazing-Fast Reads & Writes … Infinitely Scalable: Automatically scale
> from zero to millions of concurrent actors. Pay only for what you use
> with instant scaling and no cold starts.

The homepage measures a 4.99 ms cold start, so "no cold starts" is false
on Rivet's own site. A number beats an adjective; an adjective that a
number contradicts costs trust.
