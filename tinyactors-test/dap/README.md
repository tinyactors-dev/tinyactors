# Shared DAP transcripts

Two debug adapters speak the Debug Adapter Protocol (DAP) for Tinyactors:
the test host's (`ruby/dap/`, which launches a workbench directory) and
napi's (`tinyactors-napi/lib/debug.js`, which attaches to one actor of a
running system). A transcript here is one behaviour both must show: a
chart, then the requests a client sends and the responses and events the
adapter must answer with. Each host plays every transcript against its
own adapter, so a change that makes one adapter drift from the other
fails a test in the host that drifted. The design is in [the debug adapter
proposal](../../docs/proposals/debug-adapter.md) ("Keeping the two in
step"); what each message does in each host is [the status
map](../../docs/debug-adapter.md).

| Directory | |
|---|---|
| `transcripts/` | one JSON file per behaviour |
| `charts/` | each chart a transcript names, as `<chart>.rb` (the test host's Ruby DSL, a workbench chart file) and `<chart>.js` (napi's `statechart()` builder) |

| Runner | Runs in |
|---|---|
| `ruby/dap_transcripts.rb` | `mise run //tinyactors-test:test` |
| `tinyactors-napi/test/dap-transcripts.js` | `mise run //tinyactors-napi:test`, under Node and Bun |

Both take transcript names to play only those, and print every message
with `DAP_TRACE=1`:

```text
ruby ruby/dap_transcripts.rb stepping cancel
DAP_TRACE=1 node test/dap-transcripts.js stepping    # in tinyactors-napi
```

The transcripts live in tinyactors-test because it is the verification
host: its test task already runs the scenario suites, the conformance
imports and the adapter's self-test, and a behaviour of the protocol is
one more thing it verifies. napi's runner reads this directory by
relative path, as its test task runs from the checkout; napi's package
does not ship its tests, so nothing outside the repository reads it.
Keeping both forms of each chart side by side here keeps them in step
too: a change to one shows in the same directory as its twin.

## Charts

Each chart has a fixture in each host's authoring form, with the same
state IDs, events, data and actions, in the same order. They were written
when napi could not import SCXML; napi imports it now (napi ADR 0019), and
names the document's spans after define()'s `name`, so one SCXML fixture
could serve both hosts, but the two builders' fixtures also check each
host's own source lines. Until a transcript needs it, keep the pairs, and
keep these the same:

- the names of states and events, and the transitions and their order;
- the actions of each block, in order: frames name an action by its kind
  and place in its block (`assign 1`, `foreach 2 iteration 1`);
- the data, declared in alphabetical order, which is the order the test
  host's jq data model lists it in.

A transcript names places by state, transition and action names, never by
line: the two fixtures' lines differ. `counter.js` exports a function that
takes napi's `statechart`, so the fixture needs no path to the package.

## A transcript

```json
{
  "description": "what the transcript shows, in a sentence",
  "chart": "counter",
  "only": "test",
  "why": "it stops at a breakpoint; napi's adapter never stops a live actor",
  "steps": [
    { "start": true },
    { "request": "stackTrace", "arguments": { "threadId": { "$var": "thread" } },
      "response": { "body": { "stackFrames": [ { "id": { "$capture": "frame" } } ] } } },
    { "request": "evaluate", "arguments": { "expression": "send bump", "context": "repl", "frameId": { "$var": "frame" } } },
    { "request": "continue", "arguments": { "threadId": { "$var": "thread" } } },
    { "event": "stopped", "body": { "reason": "function breakpoint" } }
  ]
}
```

| Field | |
|---|---|
| `chart` | the fixture in `charts/` |
| `only`, `why` | a transcript for one host (`test` or `napi`), and the reason; the other host skips it. napi's adapter never stops a live actor (napi ADR 0027), so a transcript that stops, steps or waits at a breakpoint is the test host's; any other difference between the adapters is a bug unless the status map explains it |
| `steps` | played in order |

A step is one of:

| Step | |
|---|---|
| `{ "start": true }` | the host's own session start (below); `initialize` first if no step sent it |
| `{ "request", "arguments", "response" }` | sends the request, waits for its response and matches it. `response` matches the whole response message; it must have `success: true` unless `response` says `"success": false` |
| `{ "event", "body" }` | waits for an event of that name whose body matches |
| `{ "events": [ … ] }` | waits for each of these events, in any order: for what the protocol does not order, such as a stop and the output of the unit that ran before it |

### Session start

Starting differs between the hosts, so each runner does its own, then
plays the steps. Afterwards the one actor of the chart, named `a1`, is
at rest and reported stopped, the variable `thread` is its thread ID, and
`title` the name the adapter writes for it (its thread's name without the
states):

- **test host:** a workbench directory with the chart's file and
  `Workbench.actor :a1, chart: :<chart>`, launched with `stopOnEntry`;
  the runner takes the `entry` stop and finds the thread named
  `<chart>/a1 · …`.
- **napi:** a System with a manual clock and the actor spawned as `a1`;
  `attach` to `<chart>/a1` with every permission. The session reports the
  actor stopped once, on entry, and never stops it.

### Matching

What a response or event must be is written as the JSON it must match:

- An object matches an object that has each of its keys, with matching
  values; other keys may be there too.
- An array matches an array of the same length, element by element.
- Any other value matches the equal value.

An object with one key starting with `$` is a matcher instead:

| Matcher | Matches |
|---|---|
| `{ "$any": true }` | any value that is there |
| `{ "$absent": true }` | as a key's value: the key is not there |
| `{ "$capture": "name" }` | any value, and binds it to the variable `name` |
| `{ "$var": "name" }` | the value bound to `name` |
| `{ "$contains": "text" }` | a string containing `text` |
| `{ "$prefix": "text" }` | a string starting with `text` |
| `{ "$prefix": [ … ] }` | an array whose first elements match these; the rest may be anything (a stack trace down to the macrostep, without the causal chain that follows) |
| `{ "$text": "… {name} …" }` | the string with each `{name}` replaced by the variable's value: `{title} · busy` |
| `{ "$some": … }` | an array with an element that matches |
| `{ "$expr": { "jq": …, "js": … } }` | the host's form: `jq` for the test host, `js` for napi; each form may be a matcher itself |

In a request's arguments, `{ "$var": "name" }` is replaced by the bound
value and `{ "$expr": … }` by the host's form. Write an expression both
data models read the same way without `$expr`: napi reads a leading `.`
as the data root, so `.count` needs no second form, but `.count + 1` does.

An event step takes the first pending event of its name whose body
matches, and every pending event of that name that came before it. Events
the transcript does not name (`output`, `invalidated`, `module`, …) are
never a failure, so a transcript names only what both adapters send.

## Adding a transcript

1. Write the behaviour as a transcript in `transcripts/`, with a
   `description`. Use a chart that is here, or add one in both forms.
2. Play it in both hosts, with `DAP_TRACE=1` to see what each sends:
   `ruby ruby/dap_transcripts.rb <name>` in tinyactors-test, and
   `node test/dap-transcripts.js <name>` and `bun test/dap-transcripts.js
   <name>` in tinyactors-napi.
3. Where the two answer differently, decide which is right. A difference
   listed below is matched loosely, with `$contains`, `$prefix` or
   `$expr`, or left out; match everything else exactly, with `$text` for
   the actor's name. Any other difference is a bug: fix the adapter that
   is wrong, with a test in its own suite.

## Where the adapters differ

The two adapters say the same things in the same words: console results,
macrostep summaries (one output each), `<log>` output (`busy: 1`), thread
and actor names, and the names in the scopes. What differs, and what a
transcript therefore leaves out, matches loosely, or plays in one host
only:

- **Stopping.** napi's adapter watches a live actor and never stops it
  (napi ADR 0027): it reports the actor stopped once, on entry, refreshes
  the view after each macrostep with an `invalidated` event, refuses
  `pause` and the steps, and answers `continue` by changing nothing.
  Breakpoints there are tracepoints (`would have stopped after macrostep
  2: entered busy`). The test host's adapter stops a workbench's actors,
  all of them when one stops (`allThreadsStopped: true`), and steps them.
  A transcript that stops, steps or waits at a breakpoint is `only:
  "test"`; `live-view` is `only: "napi"`.
- **The frame of an actor at rest.** The test host's says what holds it
  (`idle in idle (held; 0 messages waiting)`); napi's says only what
  waits (`idle in idle`, or `idle in idle (1 message waiting)`). A
  transcript matches it with `$prefix`.
- **A console send.** The test host adds that the message waits in the
  mailbox while the system is stopped; napi's answer is `sent bump to
  counter/a1`.
- **The data model.** jq in the test host, JavaScript in napi: an
  expression is `$expr`, and so is text that holds one, such as an
  `<assign>` location in a summary (`assign .count = 1`, `assign count =
  1`) or a fault's message (`JSON expression produced no value`, `Error:
  boom`).
- **Records.** The test host reads trace records as each resume writes
  them. napi reads spans, which end when the next microstep begins: a
  microstep in progress has no transitions in its frame name
  (`microstep 2.1`, not `microstep 2.1: idle → busy on bump`).
- **SCXML's own data.** The test host's Event scope is `_event`, with
  `origintype` and `invokeid` besides the fields napi shows, and its
  System scope has `_ioprocessors`. A transcript matches the fields both
  have.
