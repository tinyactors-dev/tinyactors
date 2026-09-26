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

napi builds charts in JavaScript and does not import SCXML, so each chart
has a fixture in each host's authoring form, with the same state IDs,
events, data and actions, in the same order. Keep these the same:

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
  "napi": { "mode": "live" },
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
| `napi.mode` | the mode napi attaches in: `fork` (the default) or `live` |
| `only`, `why` | a transcript for one host (`test` or `napi`), and the reason; the other host skips it. Keep these rare: a difference between the adapters is a bug unless the status map explains it |
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
stopped at rest, and the variable `thread` is its thread ID:

- **test host:** a workbench directory with the chart's file and
  `Workbench.actor :a1, chart: :<chart>`, launched with `stopOnEntry`;
  the runner takes the `entry` stop and finds the thread named
  `<chart>/a1 · …`.
- **napi:** a System with a manual clock and the actor spawned as `a1`;
  `attach` to `<chart>/a1` in the transcript's mode. A fork's copy stops
  on entry; a live actor is paused.

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
| `{ "$some": … }` | an array with an element that matches |
| `{ "$expr": { "jq": …, "js": … } }` | the host's form of an expression: `jq` for the test host, `js` for napi |

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
   the status map explains (the test host stops all actors at once; napi
   has a mailbox, stops a live actor only between macrosteps, and reads
   records only as spans end) is matched loosely, with `$contains` or
   `$prefix`, or left out. Any other difference is a bug: fix the adapter
   that is wrong, with a test in its own suite.

## Where the adapters differ

What a transcript leaves out or matches loosely, and why:

- **Presentation.** The text of console results (`sent bump to a1; it
  runs when you continue` and `sent bump to counter/a1 (fork); it waits
  in the mailbox`), of macrostep summaries and of `<log>` output (`<log>
  busy 1` and `busy: 1`), and thread labels (`a1`, `counter/a1 (fork)`).
- **Delivering.** The test host delivers a console `send` at once, so the
  actor holds the event (`about to take bump`); napi posts it to the
  mailbox, and the actor takes it when it runs. A transcript syncs on a
  stop both make: an `on:` breakpoint, then `continue`.
- **All-stop.** The test host stops every actor when one stops
  (`allThreadsStopped: true`) and says `continued` when a step lets them
  run; napi's session is one actor.
- **A fork.** napi's session switches to the copy, which keeps thread 1;
  the test host adds the copy as a thread of its own. A transcript finds
  the fork by name with `$some`.
- **Records.** The test host reads trace records as each resume writes
  them. napi reads spans, which end when the next microstep begins: a
  microstep in progress has no transitions in its frame name
  (`microstep 2.1`, not `microstep 2.1: idle → busy on bump`), and on a
  fork an exception or a data breakpoint stops after the macrostep, not
  at the next unit.
- **Operators.** A `goto` while the actor is inside a macrostep is
  refused in the test host, which points at `abandon`; napi queues it
  behind the mailbox. After `abandon`, napi sends a `stopped` event with
  the new frames; the test host sends none, and its frames are new too.
- **Scopes.** Event and System hold each host's own names: SCXML's
  `_event` fields in the test host, napi's `context.event`; napi has no
  `_ioprocessors`. A transcript matches the fields both have.
- **Resting frame.** napi adds what holds an actor at rest: `idle in idle
  (held; 0 messages waiting)`.
