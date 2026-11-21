# Patterns

Sentences from www.tinyactors.dev before and after review, by kind of
element. Each pair says which failure of `../SKILL.md` it fixes. When you
write an element of one of these kinds, read its section first; when a
new review finds a failure, add its pair here.

## Headlines

A headline states what the reader gets, or on a demo page invites them to
watch or try. It claims what the page shows by default.

| Before | After | Why |
|---|---|---|
| Small actors, flat costs. | Small enough to use everywhere. | 1: two labels shaped like a slogan; the after says what small actors let you do |
| Four million *ants*. | Watch a million ants forage and fight. | 10: four million needs a long rest and a big machine; the page starts at a million |
| Four million actors in a *browser tab*. | Watch a million ants forage and fight. | 16: italics copied from the last headline; 10 again |
| A million actors in this tab. | Watch a million ants forage and fight. | a demo page invites; the reader learns what they will see |

## Ledes

A lede is the page's strongest facts, or on a demo page one sentence
that sets the scene. It does not repeat the headline.

| Before | After | Why |
|---|---|---|
| What an actor costs in memory, how to size a host, and how each operation's cost holds as the population doubles. Every number comes from a program in the repository that you can run. | A started actor takes about 530 bytes and a hibernated one about 120. Four million fit in one process, and each operation costs about the same at four million actors as at two. | 2: a table of contents; the after states the findings |
| Every ant and queen is a statechart, running on tinyactors in your browser. An ant at rest takes memory and waits for its next event. | Two colonies share this field, and every ant and queen is an actor running in your browser. | 7: "takes memory and waits" replaced a negation with nothing; the cost moved to the section that shows it |
| Two colonies, and every ant and queen is an actor. Drop food, squash ants, and raise the population to four million, all in this tab. | (as above) | 5: three actions; 10: four million |

## Section headings

A heading states the section's finding, or the instruction the reader
follows there, in the reader's words.

| Before | After | Why |
|---|---|---|
| What an actor costs. | An actor takes about 530 bytes. | 1: topic to finding |
| Costs stay flat. | Twice the actors, the same cost. | 1: the after says flat against what |
| Watch them. | Drop food and squash ants. | 1: a label; the after is what the reader does with the field |
| Messages set the limit. | Messages decide how many ants keep up. | 4: the limit of what? the after names it |
| The world is an I/O processor. | Charts send messages to the world. | 12: SCXML's term before the page explains it |
| Two hosts, two cores. | (cut to one sentence in another section) | 14: how our example is wired |
| Each colony gets a CPU core. | …so the browser can use two CPU cores. | 10: two Workers let the browser use two cores; nothing guarantees them |

## Paragraphs

A sentence says one fact with the right subject, true of the block below
it and of the reader's machine.

| Before | After | Why |
|---|---|---|
| A resting ant costs memory and no work. | An ant uses the CPU only while it handles a message. | 6: said by what it lacks |
| An ant runs when an event reaches it, such as arriving at food. | The world tells an ant when it reaches food or home, and that message is the ant's next event. | 8: arriving is no event; the world's message is |
| Shorter rests send more messages a second. | Shorter rests mean more messages a second. | 8: ants send, rests do not |
| An egg becomes a larva, then a worker that forages for 6 to 14 trips. | An egg becomes a larva and then a worker, which forages until an event ends it. The world sends `die` when the ant grows old. | 9: the chart below counts no trips; the world does |
| Two fights kill it, and so do old age, after 6 to 14 trips, and your tap. | (as above) | 5: three causes |
| An ant decides only when something happens: it leaves, finds food, comes home, meets an enemy. | (as above) | 5: a catalogue where one example does |
| The world is an object in your JavaScript that picks the destination. | The world is a JavaScript object on this page that picks each destination. | 11: the reader wrote none of it |
| Your GPU draws it walking in between. | While it walks or rests, it waits in memory and your GPU draws it. | 6, hidden: the before only existed to say tinyactors does no work |
| The charts under the field measure this tab. | (deleted) | 3: points at the charts, says nothing about them |
| Each colony is a host in its own Web Worker, so the two colonies run in parallel. | (deleted; one sentence elsewhere) | 17: repeated its heading |
| This is a pattern, not a library. It grows with your system. | There is no library to install: the world, the properties, the generator, the search and the shrinker are 76 lines of your own code. | 4: the after gives the number |
| Three rules that must hold whatever the service does. | Whatever the service does, the payment charges at most three times, gives up only after the third charge, and is paid only when the service answered. | 3: the after states the rules a reader who skips the code still learns |
| Read what a host uses at any time: | `hostStats` reports each pool's capacity, what it uses and its high water mark: | 3 |

A run of sentences opening the same way reads as filler, whatever each
says: "Every ant and queen is…", "Each ant asks…", "Each colony is…",
"Every 50 simulated milliseconds…" (17).

## Guides

A guide is steps: each heading an instruction in the reader's words, the
thing they want to do, never the API's term for it; each step a sentence
or two and the code that does it. Reference detail (every option, every
case) goes in lists and tables under the step that needs it.

| Before | After | Why |
|---|---|---|
| Pick a datamodel per host. | Choose the engine for each host. | 12: "datamodel" before the page says what it is |
| A runaway evaluation ends in an error. | Stop a chart that loops forever. | a fact about the engine; the reader came to stop a chart |
| Bound each evaluation's work and memory. | Limit how long one evaluation runs. / Limit memory for the whole engine. | 5: two limits in one heading; one step each |
| Shared builtins, or a realm each. | Give each actor its own builtins. | 1: a label of two options; the after is the instruction |
| Three edges differ from native. | Expect hibernation and values to differ. | 5 and 1: a count instead of what differs |
| `hostStats` reports each pool's use and high water. | (an instruction:) See how full each pool is. | 12: code in a heading names the API, not the task |
| Follow these habits in every test. | (one heading per habit that matters, or cut) | 4: says nothing until the list |
| Or return it at once and await inside. | (its own instruction, standing alone) | a heading that leans on the one before it |
| The host handles three cases. | (what the reader decides:) Decide what a send to a missing actor does. | 5: a count |
| The lede: "The `quickjs` datamodel runs chart code in a separate engine, with no I/O and limits on time and memory." | "An uploaded chart runs in QuickJS, a separate engine inside the package, where a runaway loop ends in an error." | 12 and 5: a term first, then three features; the after is the reader's case and the one fact that settles it |
| A `vm` context is not a security boundary. | A `vm` context looks like a sandbox but is not one. | 6: a negative kept, because it corrects a belief, and the belief is named |

In a guide, a sentence that only joins two blocks ("This one reads the
spans back.") still says what the block shows, and a section's last
sentence says what the reader now has, not what comes next.

## Problems before solutions

A feature is worth reading about only after its problem (failure 20).
State the problem as the failure that happens, not as what "must" hold,
and keep both halves literal: the product or the reader's code as the
answer's subject.

| Before | After | Why |
|---|---|---|
| Who is in which room: each room is an actor that keeps its members and handles one join or post at a time. | Who is in which room: two joins at the same moment can overwrite each other in a shared member list. A room actor handles one event at a time, so its member list keeps every join. | 20: a topic in place of the problem |
| Who is online: a timer turns an active user away after 300 seconds. | Who is online: a user who stops being active sends no event, so nothing marks them as away. The user's chart restarts a timer on each activity and marks the user away after 300 seconds. | 20: why a timer is needed is the problem |
| People who go idle: a user at rest is saved to a store and woken by its next event, so a thousand users run with fifty in memory. | People who go idle: most users are idle, and each one's data takes memory as long as it stays loaded. An idle user's actor is saved to a store and loaded again on its next event… | 20: the mechanism and the result, without the problem they solve |
| (a first fix) "…the member list must stay right", "…their state must survive until they return", "A user who goes quiet must turn away, though no event says so." | (the afters above) | the voice: a "must" states a wish, not the failure; "survive" and an event that "says" are figures; "turn away" reads as "reject" |

## Sentences before code

The sentence says what the block shows, so a reader who skips the code
still knows it; the block shows what the reader would write or run.

| Before | After | Why |
|---|---|---|
| A frame moves the clock 50 milliseconds, tells the ants that arrived, and answers a message for the other colony. (then 50 lines of `step()`) | The same world runs in Node.js. With a simulated clock and a fixed seed, every run prints the same five minutes of both colonies. (then `main.mjs` and its output) | 14: our plumbing out; what a reader would run in |

## Demo pages

Order: invite, demo, how it works, what it costs, run it yourself.

| Before | After | Why |
|---|---|---|
| Hero, then a cost section with the demo inside it, then the charts and the world, then how two Workers split the field | Hero ("Watch a million ants forage and fight."), the field ("Drop food and squash ants."), the ant's chart, the queen's, the world, the costs, the Node.js run | the reader sees the ants before reading what they cost, and reads the cost after the messages it is made of |
| A section "How would you build it?" between the demo and the explanation | (deleted) | 15: the order should make the reader ask; the page does not ask for them |

## Component copy

Chart titles, status lines and labels are copy too, and live in
`examples/*/page.mjs` and `components/`.

| Before | After | Why |
|---|---|---|
| Simulation speed 1.00× | Keeping up with real time 99%, a line at 100% labelled "real time" | 18: what is 1×? |
| Pace, % of real time | Keeping up with real time | 18: a formula as a title |
| 2,071 bytes of WebAssembly memory an actor | 336 bytes of WebAssembly memory an ant (what the ants added to memory, over the ants) | 19: the module's fixed size divided among 10,000 ants |
| Decisions a second, next to Messages a second | Time per message, next to Messages a second | 13: two charts moving 1:2 said one thing; the new one proves the page's claim |
| limit 1,000,000 (the dashed line of every chart) | limit 1,000,000 on Actors, real time 100% on the pace | a reference line names what it is |
