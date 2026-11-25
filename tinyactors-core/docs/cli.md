# The tinyactors command

`tinyactors` compiles charts and system manifests into images, and looks
into charts, manifests and images: what they hold, how their executable
content runs, how two versions differ. Every command answers in Markdown
for people, or in JSON or XML for programs, with the same members in the
same order.

It serves the application developer first (why does this chart do that,
what changes if I deploy this version), then the operator and the C
embedder, who get an image they did not compile and want to know what it
is, and the maintainers, who read the compiler's output instruction by
instruction. It costs nothing at run time: it is a program beside the
libraries, built from them, and in none of them.

## Install

Each release of tinyactors-core carries the command for Linux and macOS
(x86_64 and arm64), installable with [mise](https://mise.jdx.dev):

```sh
mise use github:tinyactors-dev/tinyactors-core
```

or from a checkout, `mise run build` in `tinyactors-core`
(`build/dev/bin/tinyactors`).

## Synopsis

```text
tinyactors [options] <command> [options] [arguments]
tinyactors help [<command>]
```

Commands take their options anywhere after the command, and the program's
options (below) anywhere on the line. Options follow GNU's conventions:
`--name VALUE` or `--name=VALUE`, `-n VALUE` or `-nVALUE`, flags clustered
(`-Wf json`), `--` before arguments that start with `-`, and `-` for standard
input. A long name is matched whole, never by a prefix. A mistyped command
or option is refused with the nearest one ("did you mean 'inspect'?").

## Inputs

A command that looks into an image takes, as its arguments:

- one compiled image (`.tai`), read as it is and verified;
- one system manifest (a `<system>` root): its charts and boot actors,
  compiled;
- or SCXML charts (`<scxml>` roots), compiled together into one image, the
  first the default boot chart.

The kind is read from the bytes, not the file's name: an image starts with
`TAI1`, and XML is told by its root element. A chart's static `src` is read
relative to the file that names it. `-` reads standard input. Compiled
inputs give their diagnostics with the answer.

## Output

`-f` / `--format` chooses `markdown` (the default), `json` or `xml`; `graph`
also answers in `svg`, the chart drawn. The
answer goes to standard output; messages about the command line or a file
that cannot be read go to standard error as `tinyactors: <message>`.

Every answer is one document with the command's name as its title:

- **Markdown**: the title as a heading; a value with a `<` outside a code
  span written as code (`` `attempts < 3` ``, or each such word when the
  value has code of its own), so it reads the same in a terminal and where
  Markdown is rendered; an object's values as
  `- **Name**: value` lines; each list of flat records as a table; nested
  objects under headings.
- **JSON**: one object, members in order, two spaces of indent. Numbers
  are integers, text is UTF-8.
- **XML**: the title as the root element; an object's values as its
  attributes, its lists and objects as child elements; a list's items as
  elements named for the item (`<chart>`, `<state>`, ...).

Member names are camelCase (`bootActors`, `doneEvent`) and are the same in
all three formats: a script reads `charts[0].states` in JSON and
`/states/charts/chart/states/state` in XML. A state is named by its id;
the root (the `<scxml>` element itself) by its XPath, `/scxml`, which no
id can be. A delay is written as SCXML writes one, in the largest unit that
holds it whole (`30s`, `1500ms`). Diagnostics come by file, then line and
column.

## Commands

| Command | Arguments | What it answers |
|---|---|---|
| `compile` | INPUT... | Compiles charts or one manifest into an image written to `-o FILE` (required; `-` for standard output, the answer then going to standard error), as the image's bytes or, with `--emit c`, an 8-aligned C array named by `--symbol` for ROM or flash. The answer: whether it compiled, the image's size, hash, charts and boot actors, and the diagnostics. A file is written whole or not at all. |
| `check` | INPUT... | Compiles the inputs or verifies the image, writing nothing: valid or not, and the diagnostics. |
| `inspect` | INPUT... | The image: its size, format version, hash, sections (offset, bytes, records), charts (source, datamodel, binding, counts of states, transitions, data, expressions, sends, invokes and code), boot actors. |
| `states` | INPUT... | Each chart's states in document order: id, kind, parent, depth, initial targets, counts of transitions, invokes and data, whether it has onentry and onexit, its done event. |
| `transitions` | INPUT... | Each chart's transitions: kind (transition, initial, history default), source, event descriptors, condition, targets, type, whether it has actions. |
| `events` | INPUT... | Every event name the selected charts use, and how many transitions handle it and raises and sends make it by name. |
| `data` | INPUT... | Each chart's `<data>`: id, the state declaring it, its expression and line. |
| `expressions` | INPUT... | Each chart's expressions and scripts: kind, line, column, text; `--kind` keeps some (`condition`, `value`, `location`, `script`, `string`, `content`, `item`, `global-script`). |
| `disassemble` | INPUT... | Each chart's executable content as instructions: their place, the entry points starting there (onentry and onexit of a state, a transition's actions, an invoke's finalize, the chart's script), the instruction and its operands. |
| `graph` | INPUT... | Each chart drawn: nested boxes for its states, orthogonal routes for its transitions, labels on their routes. `-f svg` answers the picture (every selected chart, one above the other, under its name; light or dark by the reader's preference); the other formats answer the drawing as data: each box (its state, kind, how it is drawn and its rectangle), each edge (source, target, events, condition and its route as points) and the transitions a box lists inside it (those without a target, and those crossing three or more container borders). See [Graph](#graph). |
| `diff` | OLD NEW | Two versions, each an image, a manifest or a chart, compared chart by chart (matched by name) as statecharts: states, transitions, data, executable content, invokes and the events each state handles, each change marked breaking for actors running the old version or not, and what a migration does to them. `--breaking` lists only the breaking changes, `--all` the unchanged charts too, and `--exit-code` exits with 1 when they differ (with `--breaking`, when a change breaks running actors). See [Diff](#diff). |
| `lint` | INPUT... | What cannot work as written, found without running the charts: each finding's rule, severity (warning or note), chart, state, transition, event, line (0 when the image has none) and a sentence saying what it means. `--deny RULE` (repeatable, `all` for every rule) exits with 1 when that rule finds something; `--allow RULE` (repeatable, `all`) leaves its findings out. See [Lint](#lint). |
| `path` | INPUT... | The shortest sequence of transitions from the chart's initial configuration (or `--from STATE`) to a configuration holding `--to STATE`: each step's events (or eventless), condition, source, targets and the atomic states active after it; or that no path exists, or that the search stopped at its limit. `--unreachable` asks the opposite: success when no path exists. `--chart` names the chart (default: the first). See [Path](#path). |
| `help` | [COMMAND] | The program's commands, or a command's usage, options and examples. |
| `version` | | The program's version (a development build's names its commit: `0.4.0-dev.<date>.<n>.g<sha>`), its library's version and ABI, and the image format version it reads. |

The commands that take inputs also take `-W` / `--warnings-as-errors` (a
compiled input with a warning is refused), and all but `compile` and
`check` take `-c` / `--chart NAME`, repeatable, to look at those charts
only.

Program options: `-f` / `--format FORMAT`, `-h` / `--help` (any command:
describe it), `--version`.

## Exit status

| Status | When |
|---|---|
| 0 | the command did what was asked |
| 1 | an input was refused: a chart did not compile, an image did not verify, a file is none of the three; or `diff --exit-code` found differences (breaking ones, with `--breaking`); or `lint` found something a `--deny` rule names; or `path` found no path (with `--unreachable`: found one), or stopped at its limit |
| 2 | the command line was not understood (and `tinyactors` alone, after printing the commands) |
| 3 | a file could not be read or written, or memory ran out |

## Examples

```sh
tinyactors compile order.scxml payment.scxml -o order.tai
tinyactors check -W charts/*.scxml
tinyactors inspect --format json order.tai | jq '.charts[].name'
tinyactors transitions --chart order order.scxml
tinyactors graph -f svg order.scxml > order.svg # the chart, drawn
tinyactors diff --breaking --exit-code deployed.tai order.scxml   # in CI
tinyactors lint --deny all --allow unsent-event system.xml        # in CI
tinyactors path order.scxml --to shipped
tinyactors compile order.scxml -o - | tinyactors disassemble -
```

## Diff

`tinyactors diff OLD NEW` answers three questions: a developer about to
deploy a new version of a chart asks whether the actors running the old
version will migrate correctly and what they will do differently; a
reviewer asks what changed in behaviour, as a statechart rather than as
XML text (paste the Markdown into a pull request); and CI fails a build
that would break running actors:

```sh
tinyactors diff --breaking --exit-code deployed.tai order.scxml
```

### What is compared

Charts are matched by name. Within a chart:

- **States** by id (the root as `/scxml`): added, removed, or changed in
  kind, parent (moved), initial targets, onentry, onexit or invokes.
  `before` and `after` describe the state: `compound in /scxml, initial
  open`.
- **Transitions** by their source state's id and their event descriptors,
  ties going by order: added, removed, or changed in condition, targets,
  type (internal or external) or actions: ``cond `total > 100` → paid
  (external)``.
- **Data** by id: added, removed, or changed in expression or declaring
  state.
- **Actions**: the executable content of what both versions have (the
  chart's script, onentry and onexit of kept states, a matched transition's
  actions, a matched invoke's finalize), decoded into instructions as
  `disassemble` does, with expressions and event names as text and jumps
  relative to their instruction, so code that only moved elsewhere in the
  chart is the same. A changed one shows its text before and after, cut at
  about 200 bytes.
- **Invokes** of kept states, by id or by position among those without
  one: added, removed, or changed in type, src or autoforward.
- **Events**: for each kept state an actor can rest in (atomic or final),
  the events it handles before and after, counting the transitions of its
  ancestors and the prefix matching of descriptors (`order` handles
  `order.paid`). Image-wide, the event names added and removed.
- The chart's **datamodel** and **binding**: `ecmascript`, or
  `ecmascript → python` when it changed.

Every list is present, empty when nothing is in it, so a script reading
JSON sees one shape. Only the charts that differ are listed; `--all` adds
the others with the change `unchanged`. The summary counts the charts and
the differences over all of them, `--breaking` or not.

### Breaking or behaviour

A migration (`ta_host_migrate`) moves an actor's configuration by state
id, completes it to a legal configuration of the new version without
running onentry, keeps the variables the new chart declares and
initializes the ones it adds. So a change is **breaking**, for the actors
running the old version, when it changes what they hold:

| Change | What a running actor gets |
|---|---|
| a state removed | an actor in it is completed to a legal configuration: the nearest kept ancestor enters its initial state |
| a state's kind changed | its configuration is completed for the new kind (a compound state enters its initial state, an atomic one drops its children) |
| a state moved to another parent | it enters its new ancestors; a compound ancestor that then has two active children keeps the first |
| data removed | the value is dropped; the old bindings are only in the data of `error.platform.upgrade`, the first event it gets |
| the datamodel changed | the new datamodel is given the old one's state, which it may refuse |
| binding from late to early | the data of states it never entered stays unset |
| a kept state no longer handles an event | an actor waiting there ignores the event |

Everything else is **behaviour**: added states, transitions and data, new
conditions, targets and actions, changed invokes. These change what an
actor does on its next event, not what it holds. An invoke removed from a
kept state does not break running actors: a migration never cancels a
running invocation (an actor whose invocations run moves once they
finished). An invoke added to a kept state does not start for the actors
already in it; only states entered later invoke.

`migration` sums up the breaking changes in sentences; it is present only
when there are some.

### Example

```text
$ tinyactors diff --breaking order-v1.scxml order-v2.scxml
```

answers, among the rest:

```markdown
#### States

| State | Change | Breaking | Before | After |
| --- | --- | --- | --- | --- |
| paying | removed | yes | atomic in /scxml |  |

#### Events

| State | Event | Change |
| --- | --- | --- |
| open | cancel | no longer handled |
```

## Lint

`tinyactors lint INPUT...` reads the charts' states, transitions, raises
and sends, runs nothing, and reports what cannot do what it is written
for. It answers the developer who asks "is anything in this chart dead or
stuck?" before an actor finds out, and CI, which fails a build on the
rules it denies:

```sh
tinyactors lint order.scxml
tinyactors lint --deny all --allow unsent-event system.xml
```

`check` answers whether charts compile, and refuses what does not; `lint`
answers what compiles but cannot work, and fails only on the rules a
build denies, so the two stay apart.

| Rule | Severity | What it finds |
|---|---|---|
| `unreachable` | warning | a state no path from the initial configuration enters (only the topmost of such a subtree; a history state no transition targets is one) |
| `dead-end` | warning | a reachable atomic state, not final, where an actor does nothing more: no transition of it or of an ancestor handles any event, and none of a parallel region beside it exits it |
| `never-ends` | warning | in a chart with a final state at its top level: a state from whose every configuration no such final state is reachable (the topmost; one only a dead end keeps from ending is that dead end's) |
| `shadowed` | warning | a transition an earlier transition of its state, without a condition, always takes first: its descriptors cover this one's (`order` covers `order.paid`, `*` everything), or it is eventless and taken before any event |
| `eventless-loop` | warning | an eventless transition without a condition, the first eventless one an actor resting in a state must take, that leads back to it: the actor never rests |
| `unhandled-event` | warning | an event raised, or sent by name to the chart itself, to its parent or to an invoked chart the image holds, that no transition of the receiving chart handles |
| `unsent-event` | note | with several charts (a manifest, or charts compiled together): a descriptor a chart handles that nothing in the image raises or sends by name to it, so only the application can; `done.*`, `error.*` and `*` are not reported |
| `unknown-events` | note | with several charts: a send whose event is computed (`eventexpr`) may arrive at the chart, so which events arrive is unknown and `unsent-event` is not checked for it |
| `send-target` | warning | a send to the SCXML processor whose static target no send can reach: `#_id` naming no invocation of the chart (`error.communication`), or text that is neither `#_...` nor an absolute URI (`error.execution`) |
| `search-limit` | note | the search of the chart's configurations stopped at its limit (20,000), so `never-ends` was not checked |

What is unknown is never reported as unused: a send whose target is
computed (`targetexpr`), names an address or a session, or goes to the
parent of a chart nothing in the image invokes may reach any chart, and
counts as sent to each; a send with a `type` goes to the application's
I/O processors and is not followed.

States and transitions carry no line in an image: a transition's finding
has its condition's line, a send's the line of its first expression, and
the rest 0.

### What the analysis models

A configuration is a set of active states as SCXML defines one. From a
configuration, any transition whose source is active and which has
targets may be taken, one at a time, unless the `shadowed` rule finds it;
its exit and entry are SCXML's (the transition domain, a compound state
entered by its initial transition, a parallel state with every region, a
history state by its default transition). Entering a final state at the
chart's top level ends the actor. Events are free: any event may come at
any time, `done.*` and `error.*` included; every condition may hold; an
eventless transition with a condition does not run first; parallel
regions take their transitions one after another; history holds nothing
but its default; invoked children are not followed. So a state found
unreachable is unreachable whatever happens at run time, while one found
reachable may still not be. The analysis is a layer of its own
(`src/analysis/analysis.h`), freestanding, which another tool or a
binding can call.

## Path

`tinyactors path INPUT... --to STATE` answers "how does an actor get
there?": the shortest sequence of transitions (by their number), found
breadth first over the configurations the model above gives, from the
chart's initial configuration or from `--from STATE` (that state entered
from the root, its defaults completing the configuration). Each step names
the events that take it (or that it is eventless), the condition it needs
(conditions are not evaluated: they are requirements), its source and
targets, and the atomic states active after it, so a parallel state's
regions show side by side.

```text
$ tinyactors path refund.scxml --to emailed
```

```markdown
- **Result**: found
- **Transitions**: 3
- **Start**: requested

| Step | Events | Eventless | Condition | Source | Targets | Active |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | review | no |  | requested | reviewing | reviewing |
| 2 | approve | no | `amount < 500` | reviewing | approved | transferring emailing |
| 3 | emailed | no |  | emailing | emailed | transferring emailed |
```

The result is `found`, `unreachable` (every configuration was searched,
and none holds the state: no events and no conditions make it happen) or
`unknown` (the search stopped at 20,000 configurations). The command exits
with 0 when it found a path and 1 otherwise; with `--unreachable` it asks
the opposite, for a CI check that something must never happen:

```sh
tinyactors path refund.scxml --from rejected --to refunded --unreachable
```

Since events are free, a path may take an event the chart only gets in a
later situation (`done.state.approved` before both regions are final):
read a path as the transitions to take, each of which needs its event and
its condition.

## Graph

`tinyactors graph` lays a chart out as scxmljs's `<scxml-view>` does (it is
that layout, ported to C): the whole chart at once, as nested boxes, each
container arranging its children in layers by the transitions between them
(cycles broken by a feedback arc set, crossings reduced by barycentre sweeps),
parallel regions in a grid, and every transition routed orthogonally from its
source's far side to its target's near side, its label on its route.
Transitions without a target, a container's transitions into its own
states, and transitions that cross three or more container borders are
listed in their source's box instead of routed. The same chart always gives
the same picture. Text is measured by estimate (no fonts are read), so a
name in a font much wider than the system's may run past its box.

```sh
tinyactors graph -f svg order.scxml > order.svg
tinyactors graph -f svg --chart payment shop.tai > payment.svg
tinyactors graph -f json order.scxml | jq '.charts[0].edges[].route'
```

Every answer ends with the drawing's **problems**, found by geometry alone
(`ta_diagram_check`): `startDetached`, `endDetached` and `initialDetached`
(an arrow that does not touch its box's drawn outline: the rectangle, a
final's pill, a history state's circle), `notOrthogonal`, `outsideParent`
and `boxesOverlap`, which no layout may have, and the faults the layout still
makes, to be worked down: `notCentred` (an edge alone on a side, off its
middle), `throughBox`, `arrowAlongSide`, `labelOverlap`, `labelOnBox` and
`labelOnEdge`. The SVG carries its own stylesheet, scoped: its classes and
ids are prefixed (`ta-`, `tinyactors-`) and its rules written under the
root's class `tinyactors-diagram`, so the drawing put inline in a page takes
none of the page's styles and gives it none.

The layout is checked by a corpus of charts and their expected layouts and
problems (`tests/diagram`, its README).

## How it is built

The layers, from the bottom:

- **`src/cli`**, freestanding like `src/base`: `arguments.h` reads a
  command line against a program's description (its commands, their
  options and arguments), and `report.h` builds an answer as one tree and
  writes it in the three formats. Neither uses the C library, recurses or
  allocates but from the arena it is given.
- **`src/analysis`**: what can be known about a chart without running
  it (`analysis.h`, freestanding like `src/cli`): `lint`'s findings and
  `path`'s search, as arrays in the arena given, knowing nothing of the
  report.
- **`src/diagram`**: a chart laid out (`diagram.h`, freestanding like
  `src/cli`) and drawn as SVG through `ta_xml_writer` (`svg.h`).
- **`tools/tinyactors`** but one file: the commands. They compile without
  the C library's headers and reach files only through the platform
  interface in `tool.h`. One invocation is one arena: the parsed line, the
  files read, the compiled image and the answer live in it, and it is freed
  once when the command ends.
- **`tools/tinyactors/platform.c`**: `main`, the allocator behind the
  arena, reading files and standard input, writing streams, and writing a
  file whole (a temporary file renamed over it). The only file that uses
  the C library.

The program is built with core (`mise run build`, `build/dev/bin/tinyactors`)
and tested by running it (`tests/unit/test_tool.c`). Each release carries
it for every native target ([distribution.md](distribution.md#the-tinyactors-command)).
