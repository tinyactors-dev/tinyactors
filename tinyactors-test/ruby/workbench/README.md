# The interactive workbench

A live Tinyactors system you can watch and poke at, whose charts are files on
disk. Save a chart and the running actors are upgraded to it in place: the host
process, the system, the actor addresses, and the accumulated data-model state
all survive.

```text
cd tinyactors-test
mise run build
mise run workbench            # opens ./workbench in tmux
```

Or run the program directly, on whatever directory you are standing in:

```text
cd somewhere/with/charts
/path/to/tinyactors-test/bin/workbench
```

```text
+----------------------------+----------------------------+
| actors                     | data model                 |
+----------------------------+----------------------------+
| logs                                                    |
+---------------------------------------------------------+
| workbench>                                              |
+---------------------------------------------------------+
```

## What a workbench directory holds

Every `*.rb` file in the directory is loaded. A file may define charts with the
normal scenario DSL (`ruby/tinyactors_test/chart.rb`) and declare the top-level
actors to run:

```ruby
# subscription.rb
TinyactorsTest.chart :subscription do
  data account: 'acct_1'
  data failures: 0
  initial :active

  state :active do
    transition on: 'payment.failed', target: :past_due do
      assign '.failures', '.failures + 1'
    end
  end

  state :past_due do
    transition on: 'payment.succeeded', target: :active
  end
end
```

```ruby
# actors.rb
Workbench.actor :acct1, chart: :subscription
Workbench.actor :acct2, chart: :subscription, binding: { 'account' => 'acct_2' }
```

Files are loaded in several passes, so a chart that inlines another with
`invoke chart:` may sit in any file: the pass order resolves the dependency
without the filenames having to encode it.

## The ping-pong example

`workbench/player.rb` and `workbench/actors.rb` are a rally between two named
actors:

```ruby
Workbench.actor :ping, chart: :player, binding: { 'partner' => '#_actor_player/pong' }
Workbench.actor :pong, chart: :player, binding: { 'partner' => '#_actor_player/ping' }
```

Both run the same chart; the binding is merged into the chart's declared data,
so only `partner` differs. Each hit is a `send_event` with `to_expr: '.partner'`
and a one-second delay, addressed logically as `#_actor_<family>/<name>` — the
directory resolves it, so neither player knows the other's runtime ID.

```text
deliver ping serve
run 3              # three volleys, one deadline at a time
clock start 5x     # or let the rally play itself
```

The delay is what makes it steppable. Without one the two would volley inside
a single macrostep and the system would never come to rest.

Edit `player.rb` mid-rally and the rally carries on under the new chart with
its volley count intact: the two players keep `rallying` and `server` in the
data model and resume from them. See "Writing a chart that survives its own
edits" below for why that is needed.

## How a reload works

Every top-level actor is a **named actor**, spawned at the address
(definition family, name). That is what makes an in-place upgrade possible:
`ta_actor_prepare_replacement` only accepts a published named incumbent, and
the atomic directory swap in `ta_actor_activate_replacement` is what hands the
address over without a gap.

On save, for each actor whose chart changed:

1. The new chart is compiled and registered. Its family is unchanged, so it is
   a new **revision** of the same definition family.
2. `ta_actor_prepare_replacement` builds an unpublished session of the new
   revision, bound to **the incumbent's current data-model root**, and holds
   its bootstrap effects.
3. `ta_actor_activate_replacement` swaps the directory entry and releases those
   effects in SCXML order.
4. The incumbent's invoked children are destroyed, then the incumbent.
   `ta_actor_destroy` does not cascade into children — for `invoker.scxml` the
   service handle *is* the child's actor ID, so the workbench walks
   `ta_actor_inspection.invokes` and reaps them itself.
5. The old definition is retired. Core frees it once no resident actor uses it.

So across a reload:

| | |
|---|---|
| host process, system, logical time | unchanged |
| actor address | unchanged |
| data-model root | **carried over** into the replacement |
| active configuration | **reset** to the chart's initial state |
| invoked children | destroyed; the replacement starts its own |
| pending delayed sends | dropped with the incumbent |

The configuration resets because a replacement is a fresh session. Core has no
way to map a configuration from one revision onto another — `persistence.md`
says so plainly: "No translation of definition-local state IDs is required."
Pretending otherwise would be the one thing a test workbench must not do.

### Writing a chart that survives its own edits

The consequence catches everyone once: a chart that keeps its progress only in
its configuration **stops dead on every edit**, because it comes back in its
initial state with its delayed sends gone. The reload says so in the logs pane:

```text
ping: replaced (definition 4294967297 -> 8589934592, data carried)
ping: 1 delayed send(s) dropped - a replacement starts at the chart's initial state
pong: was rallying, now idle - a replacement starts at the chart's initial state
```

The data model *is* carried, so a chart resumes by keeping what it needs there
and reading it back on entry. An eventless transition out of the initial state
is the whole trick:

```ruby
data rallying: false      # survives the upgrade

state :idle do
  # Fires as soon as the replacement initializes.
  transition cond: '.rallying', target: :working
end
```

`workbench/player.rb` does exactly this, and also keeps a `server` flag so that
only one of the two players puts a new ball in play — otherwise both would
resume and every edit would double the number of balls in flight.

### What reloading will not do

- **A chart whose family changed** (its `document_name`, or its registry name
  when it has none) cannot be a replacement, because the address names the
  family. The workbench destroys and respawns the actor instead, carries the
  data root by hand, and says so in the logs pane.
- **A chart file that fails to load** leaves its actors running untouched. The
  error goes to the logs pane; fix the file and save again.

## Commands

| Command | |
|---|---|
| `actors`, `ls` | the named actors and their configuration |
| `charts` | the charts loaded from the directory |
| `focus NAME`, `f` | point the data-model pane at an actor |
| `deliver NAME EVENT [JSON]`, `d` | deliver an external event |
| `advance 5s \| 500ms \| 30000`, `a` | advance logical time and run what falls due |
| `run [STEPS]` | hop from one pending delayed-send deadline to the next |
| `clock [start [RATE] \| stop]` | let logical time run on its own |
| `events [NAME]`, `e` | the events an actor listens for, and which apply now |
| `data [NAME]` | print a data-model root in the command pane |
| `reload`, `r` | re-read the directory now |
| `quit`, `q` | shut the host down and close the window |

Input goes through **Reline**, the line editor Ruby ships and IRB uses, so you
get line editing, history, and tab completion over command names, actor names
and chart names. After `deliver NAME `, completion switches to the events that
actor listens for. When stdin is not a terminal the loop falls back to `gets`,
which is how the self-test drives it.

## Finding out what an actor accepts

`events` answers the question a chart file otherwise makes you read for:

```text
workbench> events ping
  accepted now
    ball       when .volleys < .limit [true]          -> rallying
    ball       when .volleys >= .limit [false]        -> idle
    stop                                              -> idle
  elsewhere in the chart
    serve                                             in idle -> rallying
    ball       when .rallying | not                   in idle -> rallying
```

**accepted now** are the transitions whose state is in the actor's current
configuration; the rest of the chart follows, with the state each one sits in.
`[true]` and `[false]` are the guard evaluated against the data model as it
stands right now, so a transition that is active but will not fire is visible
rather than mysterious. A guard that needs a macrostep's variables (one
reading `$_event`) is left unmarked, because it cannot be evaluated from
outside one.

The rows come from `ta_definition_export` — the definition the actor is
*running*, not the chart file on disk. After a reload that failed those two
differ, and only the first one describes the actor in front of you.

## The clock

Logical time only moves when you move it, so a chart driven by delayed sends
sits still until `advance` or `run`. `clock start` lets it run by itself:

```text
clock start        # logical time tracks real time
clock start 10x    # ten times faster
clock stop
clock              # is it running, and how fast
```

`RATE` is logical milliseconds per wall-clock millisecond. Each tick advances
by the wall time that actually elapsed, so a slow step falls behind rather
than quietly compressing time, and the overview header shows the rate while it
runs. A step that raises stops the clock and says why in the logs pane, rather
than repeating the failure several times a second.

The clock is a thread, not a fiber. Fibers are cooperative and the command
loop blocks in Reline, which never yields, so nothing would resume a tick
fiber while you were typing. Three threads touch the system — the command
loop, the file watcher, and the clock — and because the driver is a single
JSON-RPC pipe whose requests must not interleave, every one of them goes
through one mutex. The command loop holds it only while a command runs, never
while it waits for a line, so a reload or a tick lands promptly mid-keystroke.

## Options

```text
bin/workbench [--dir DIR] [--host PATH] [--session NAME] [--no-tmux]
```

`--no-tmux` runs the command loop on its own, without the display panes, which
is how the workbench is driven from a script.

## How it is put together

| File | |
|---|---|
| `sources.rb` | loads a directory of charts and actor declarations, in several passes |
| `watcher.rb` | reports changes; Ruby has no filesystem-event API, so it polls mtimes on a thread |
| `clock.rb` | advances logical time on a thread while it runs |
| `session.rb` | the live system: named actors, the replacement protocol, reaping, retiring |
| `journal.rb` | the bounded record of reloads, commands, errors, and `<log>` |
| `panes.rb` | the three renderers |
| `render.rb` | ANSI, tty panes, and a JSON printer that tags lines with their path |
| `tmux.rb` | builds the window and finds the panes' ttys |
| `repl.rb` | the command table |
| `app.rb` | the launcher and the event loop |

The display panes run a command that produces no output, and the workbench
writes to their ttys directly. Nothing is ever written to the command pane
except by the command loop itself, so a reload or a clock tick redraws the
other three panes without disturbing a half-typed command.

`Session` builds on `TinyactorsTest::Context` (`ruby/tinyactors_test/scenario.rb`),
the same class the scenario suite uses, so the workbench drives the public C
API through exactly the generated bindings the tests do. It runs the system
through the Context's `TinyactorsTest::Scheduler`, the loop the scenarios
run to quiescence with. The debug adapter (`ruby/dap/README.md`) runs a
workbench directory headless through the same Session and Scheduler, and
steers the Scheduler to stop and step.

`ruby/workbench_selftest.rb` checks all of this against the live host,
including that nothing leaks once the system is destroyed. `mise run test`
runs it.
