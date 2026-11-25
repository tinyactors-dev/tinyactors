# The tinyactors command

`tinyactors` compiles charts and system manifests into images, looks
into charts, manifests and images (what they hold, how their executable
content runs, how two versions differ), and writes charts and manifests in
one canonical layout. It also reads what a running host writes: a
snapshot of a whole system, a hibernation blob, an export blob. Every
command answers in Markdown for people, or in JSON or XML for programs,
with the same members in the same order. And it runs charts: `tinyactors
run` serves one host over JSON-RPC 2.0 on standard input and output, for
an agent or a CI job to drive ([Run](#run)).

It serves the application developer first (why does this chart do that,
what changes if I deploy this version), then the operator and the C
embedder, who get an image they did not compile and want to know what it
is, or a snapshot or a stored blob and want to know what is in it, and the
maintainers, who read the compiler's output instruction by instruction. It
costs nothing at run time: it is a program beside the libraries, built
from them, and in none of them.

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
- one snapshot (below): the image one of its segments runs, the newest
  live one, or segment N with `-s` / `--segment N`; every image of it is
  verified;
- one system manifest (a `<system>` root): its charts and boot actors,
  compiled;
- or SCXML charts (`<scxml>` roots), compiled together into one image, the
  first the default boot chart.

What a host writes is read too, by `inspect` and `actors`:

| Input | Written by | Starts with | Read by |
|---|---|---|---|
| a snapshot | `ta_host_snapshot_pack`: the host's snapshot, every live segment's image, the system id, the host's time and the datamodels' state under its label | `tinyactors-snapshot ` and the pack's format | `inspect`, `actors`, and the image commands |
| a hibernation blob | `ta_host_hibernate` (`ta_actor_hibernate`): one session's instance and datamodel state, which the application stores | `TAHB` | `inspect` |
| an export blob | `ta_actor_export`: a session tree leaving its system, with its delayed sends (a migration's, inside a host; no host call hands one out) | `TAEX` | `inspect` |
| a host snapshot alone | `ta_host_snapshot`: the regions without the images | `TAHS` | `inspect` (its segments only) |

The kind is read from the bytes, not the file's name: an image starts with
`TAI1`, a blob or a host snapshot with its four bytes of magic (as a
little-endian machine writes them), a snapshot with its text, and XML is
told by its root element. `-` reads standard input, once: a command that
reads it again gets the same bytes. Compiled inputs give their diagnostics
with the answer.

Snapshots and blobs are raw memory of one build of the library, each
format with a version ([durability.md](durability.md#blob-formats)): the
command reads the formats of the library it was built with, and of another
says which format it is and that it cannot read it.

A chart's static `src` (an `<invoke src>`, a `<script src>`, a `<data
src>`, a manifest's `<chart src>`) is read relative to the file that names
it, then in each directory given with `-I` / `--include DIR`, in the order
given; an absolute `src` only where it says. A file found through the
include path is named by where it was found, in diagnostics and in the
image (`inspect`'s sources). A `src` found nowhere is refused at the path
beside the file that names it, as without `-I`, and the message lists
every path tried:

```sh
tinyactors compile -I shared/payments -I vendor/charts shop/checkout.scxml -o checkout.tai
```

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

A value a datamodel wrote (a binding's snapshot state, a blob's
variables) is shown when it is one CBOR item: in Markdown in CBOR's
diagnostic notation, which reads as JSON for JSON's values (`null`,
`undefined`, `h'00ff'` for bytes, `32("...")` for an address), as code; in
JSON as the JSON value, what JSON has none for as an object
(`{"bytes": "00ff"}`, `{"undefined": true}`, `{"tag": 32, "value": ...}`,
`{"float": "NaN"}`); in XML as typed elements (`<map>` of
`<entry key="...">`, `<array>`, `<text>`, `<integer>`, `<float>`,
`<bytes>`, `<true/>`, `<false/>`, `<null/>`, `<undefined/>`, `<tag
number="...">`). Bytes that are not one CBOR item (the ECMAScript
datamodel's, which are quickjs's own serialization) are said to be opaque,
with their length.

## Commands

| Command | Arguments | What it answers |
|---|---|---|
| `compile` | INPUT... | Compiles charts or one manifest into an image written to `-o FILE` (required; `-` for standard output, the answer then going to standard error), as the image's bytes or, with `--emit c`, an 8-aligned C array named by `--symbol` for ROM or flash. The answer: whether it compiled, the image's size, hash, charts and boot actors, and the diagnostics. A file is written whole or not at all. |
| `check` | INPUT... | Compiles the inputs or verifies the image, writing nothing: valid or not, and the diagnostics. |
| `inspect` | INPUT... | The image: its size, format version, hash, sections (offset, bytes, records), charts (source, datamodel, binding, counts of states, transitions, data, expressions, sends, invokes and code), boot actors. Of a snapshot, a blob or a host snapshot, what it holds: see [Snapshots and blobs](#snapshots-and-blobs). |
| `states` | INPUT... | Each chart's states in document order: id, kind, parent, depth, initial targets, counts of transitions, invokes and data, whether it has onentry and onexit, its done event. |
| `transitions` | INPUT... | Each chart's transitions: kind (transition, initial, history default), source, event descriptors, condition, targets, type, whether it has actions. |
| `events` | INPUT... | Every event name the selected charts use, and how many transitions handle it and raises and sends make it by name. |
| `data` | INPUT... | Each chart's `<data>`: id, the state declaring it, its expression and line. |
| `expressions` | INPUT... | Each chart's expressions and scripts: kind, line, column, text; `--kind` keeps some (`condition`, `value`, `location`, `script`, `string`, `content`, `item`, `global-script`). |
| `disassemble` | INPUT... | Each chart's executable content as instructions: their place, the entry points starting there (onentry and onexit of a state, a transition's actions, an invoke's finalize, the chart's script), the instruction and its operands. |
| `graph` | INPUT... | Each chart drawn: nested boxes for its states, orthogonal routes for its transitions, labels on their routes, top to bottom (`--direction right`: left to right). `-f svg` answers the picture (every selected chart, one above the other, under its name; light or dark by the reader's preference, or `--theme light` / `dark`); the other formats answer the drawing as data: each box (its state, kind, how it is drawn and its rectangle), each edge (source, target, events, condition and its route as points) and the transitions a box lists inside it (those without a target, and those crossing three or more container borders). `--highlight STATE` marks states, `--path --to STATE [--from STATE]` marks the path `path` finds, its steps numbered; `--depth N` and `--fold STATE` fold containers into one box. See [Graph](#graph). |
| `diff` | OLD NEW | Two versions, each an image, a manifest or a chart, compared chart by chart (matched by name) as statecharts: states, transitions, data, executable content, invokes and the events each state handles, each change marked breaking for actors running the old version or not, and what a migration does to them. `--breaking` lists only the breaking changes, `--all` the unchanged charts too, and `--exit-code` exits with 1 when they differ (with `--breaking`, when a change breaks running actors). See [Diff](#diff). |
| `lint` | INPUT... | What cannot work as written, found without running the charts: each finding's rule, severity (warning or note), chart, state, transition, event, line (0 when the image has none) and a sentence saying what it means. `--deny RULE` (repeatable, `all` for every rule) exits with 1 when that rule finds something; `--allow RULE` (repeatable, `all`) leaves its findings out. See [Lint](#lint). |
| `path` | INPUT... | The shortest sequence of transitions from the chart's initial configuration (or `--from STATE`) to a configuration holding `--to STATE`: each step's events (or eventless), condition, source, targets and the atomic states active after it; or that no path exists, or that the search stopped at its limit. `--unreachable` asks the opposite: success when no path exists. `--chart` names the chart (default: the first). See [Path](#path). |
| `fmt` | FILE... | Writes charts and system manifests in one canonical layout: the canonical text of one file to standard output, `-w` / `--write` rewrites the files in place (each whole or not at all), `--check` writes nothing and exits with 1 when a file is not canonical. With `-w` or `--check` the answer lists the files: whether each changed, whether it was refused, and whether its canonical text compiles to the same image. See [Fmt](#fmt). |
| `actors` | SNAPSHOT | Every actor of a snapshot: its session, name, chart, segment and state, its parent and running invocations, its configuration, the events queued in its mailbox and its pending delayed sends. `--chart`, `--segment N` and `--session ID` (repeatable) narrow the list. See [Snapshots and blobs](#snapshots-and-blobs). |
| `run` | [INPUT...] | One host, with quickjs, serving JSON-RPC 2.0 on standard input and output, one message per line: `run.*` methods are the host's calls, every other method its control plane; what actors do comes back as `effect.*` notifications. Experimental. See [Run](#run). |
| `help` | [COMMAND] | The program's commands, or a command's usage, options and examples. |
| `version` | | The program's version (a development build's names its commit: `0.4.0-dev.<date>.<n>.g<sha>`), its library's version and ABI, and the image format version it reads. |

The commands that take inputs also take `-W` / `--warnings-as-errors` (a
compiled input with a warning is refused) and `-I` / `--include DIR`,
repeatable (where a `src` is looked for next, [Inputs](#inputs)), and all
but `compile`, `check` and `fmt` take `-c` / `--chart NAME`, repeatable, to
look at those charts only, and `-s` / `--segment N`, which of a snapshot's
segments to read (`lint` and `path` included). `check` of a snapshot
verifies every image in it; `compile` refuses one, and `fmt` refuses it as
not XML. `fmt` takes `-I` too: it compiles each file to compare images.

Program options: `-f` / `--format FORMAT`, `-h` / `--help` (any command:
describe it), `--version`.

## Exit status

| Status | When |
|---|---|
| 0 | the command did what was asked |
| 1 | an input was refused: a chart did not compile, an image did not verify, a file is none of the kinds above, a snapshot or blob is of another format or damaged; or `diff --exit-code` found differences (breaking ones, with `--breaking`); or `lint` found something a `--deny` rule names; or `path` found no path (with `--unreachable`: found one), or stopped at its limit; or `fmt` refused a file, or `fmt --check` found one not canonical |
| 2 | the command line was not understood (and `tinyactors` alone, after printing the commands) |
| 3 | a file could not be read or written, or memory ran out |

`run` has its own: [Run, Exit status](#exit-status-1).

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
tinyactors fmt -w charts/*.scxml system.xml
tinyactors fmt --check charts/*.scxml                               # in CI
tinyactors actors --chart order shop.snapshot          # where every order is
tinyactors inspect order-17.blob shop.snapshot         # a stored actor
tinyactors run --io-processor payments order.scxml < session.jsonl  # run it
```

## Snapshots and blobs

An operator holding a snapshot, or a blob from the application's store,
asks what is in it: which system and when, which versions it runs, where
each actor is and what it waits for, and whether a blob can be woken
where it is meant to go. The command answers with the library's own
readers, running nothing.

### A snapshot

`inspect SNAPSHOT` unpacks it (`ta_host_snapshot_unpack`), and restores it
into a host in its own memory (`ta_host_restore`, the images from the
pack) without restoring the datamodels: each datamodel the images name is
bound as one that does nothing, and nothing runs, so none is called. It
answers:

- the pack's format, and the host snapshot's inside it;
- the system id, the host's time when it was packed (in ticks) and the
  ticks per second;
- the host's identity, which every blob it hibernated carries;
- whether it was restored, and why not (another build's format, damage);
- every segment: live or retired, its image's hash and size, its charts,
  its actors and how many are hibernated; the totals;
- the datamodels' state: the label it was packed under (`ecmascript` for
  the C ECMAScript datamodel's, a binding's name for a binding's), its
  size, and its value when it is one CBOR item (a binding's is a CBOR map);
  quickjs's state is opaque.

`actors SNAPSHOT` lists, for every actor of every live segment, read from
the restored regions:

- its session id, the name it was spawned with (an invoked session's is
  its invoke id), its chart and segment, and its state: `idle`,
  `runnable`, waiting, `done` or `hibernated`;
- its parent (an invoked session's invoker) and how many invocations it
  runs;
- its configuration: the active states, in document order (a hibernated
  actor's is in its blob, in the application's store);
- its mailbox: the events queued, in the order it takes them, with their
  sendids;
- its pending delayed sends, in firing order: event, sendid, when they
  are due (ticks) and in how long from the snapshot's time.

The images are the image commands' too: `states --segment 1
shop.snapshot` reads segment 1's charts.

```text
$ tinyactors inspect shop.snapshot
# Inspect

- **From**: snapshot
- **Format**: 3
- **Host format**: 4
- **System ID**: shop
- **Time**: 0
- **Ticks per second**: 1000
- **Identity**: 000000008f43601b
- **Restored**: yes

## Segments

| Index | Live | Image hash | Image bytes | Charts | Actors | Hibernated |
| --- | --- | --- | --- | --- | --- | --- |
| 0 | yes | 07841bf1b335dc2d | 2208 | order, payment | 3 | 1 |

## Datamodel state

- **Label**: ecmascript
- **Bytes**: 407
- **Encoding**: opaque

$ tinyactors actors --session 1 shop.snapshot
### Actor 1: cart-ann

- **Session**: 1
- **Name**: cart-ann
- **Chart**: order
- **State**: runnable
- **Invocations**: 1
- **Queued**: 1

#### Configuration

- paying

#### Mailbox

| Event | Sendid |
| --- | --- |
| item.added |  |

#### Delayed

| Event | Sendid | Due | In |
| --- | --- | --- | --- |
| payment.reminder | reminder | 30000 | 30s |
```

### A blob

`inspect BLOB` reads a hibernation or export blob in place: its format and
whether this build reads it, the session it holds (an export's root), the
hash of the image it was written with, and for each session its parent,
name (an export's), chart index, macrosteps run, delayed sends (an
export's: event, sendid, target, due time, place in the tree's firing
order, and their data's value when it is CBOR) and its datamodel's state
(a value when it is one CBOR item, as a binding writes it; quickjs's is
opaque). A hibernated actor's delayed sends stay in the host, so a
snapshot lists them (`actors`), not its blob.

A hibernation blob wakes only in the host that hibernated it, or one
restored from that host's snapshot: the answer gives the identity it was
written with. Given the image it was written with after it (`inspect BLOB
IMAGE`: an image, the charts, a manifest, or a snapshot), the answer names
the chart and gives the configuration, says whether the image matches,
and, for a snapshot, whether the blob wakes in the host restored from it.

A host snapshot alone (`ta_host_snapshot`'s bytes) holds no image: `inspect`
gives its format and its segments' image hashes only.

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

## Fmt

`tinyactors fmt` writes a chart or a system manifest the way
`ta_xml_writer` writes XML, so two people's edits of a chart differ only
where they change it, and a reviewer reads one layout:

- two spaces per element level, one element, comment or processing
  instruction per line, and the file ending with a line end;
- one empty line where the source has one or more between two elements,
  comments or processing instructions (two or more line ends between
  them, spaces on the lines aside), inside an element or around the root;
  none before an element's first child or after its last, and none where
  the source has none;
- attributes in the order written, one space apart, each value byte for
  byte as written between its quotes (`cond="total &gt; 100"` stays so),
  quoted with `"` unless the value holds one;
- comments where they are, each on a line of its own; the XML declaration
  and processing instructions kept; namespaces and their prefixes kept;
- the whole content of `<script>`, `<data>`, `<content>` and `<assign>`,
  of an element of another namespace, and of any element with text or
  CDATA of its own, byte for byte: a script's CDATA, its indentation and
  its line ends stay as they are.

So only the whitespace between elements changes. Before it writes
anything, `fmt` compiles each file and its canonical text, as the file
(its `src` read as `compile` reads it, `-I` included), with
`TA_COMPILE_STRIP_DEBUG`: an image records each expression's line and
column, which a new layout moves, and nothing else. The two images must
be the same bytes; when they are not, the file is refused and left as it
is (a fault in `fmt`). A file that does not compile as it is (a `src`
found nowhere) is formatted without that comparison, and the answer says
`not compiled`. A file that is not well-formed XML, holds a document type
declaration, or a comment XML does not allow (`--` inside), is refused.

With one file and neither option, the canonical text is the answer, on
standard output. Several files need `-w` or `--check` (written one after
another to standard output, they would be no document). `-w` writes only
the files that change, each through a temporary file renamed over it.

```sh
tinyactors fmt order.scxml > order.formatted.scxml
tinyactors fmt -w charts/*.scxml system.xml
tinyactors fmt --check charts/*.scxml          # exits with 1, naming the files
tinyactors fmt --check -f json -I shared charts/*.scxml | jq '.results[] | select(.changed) | .file'
```

The answer, with `-w` or `--check`:

```markdown
- **Mode**: check
- **Files**: 2
- **Changed**: 1
- **Refused**: 0

## Results

| File | Changed | Refused | Image |
| --- | --- | --- | --- |
| order.scxml | yes | no | same |
| payment.scxml | no | no | same |
```

Every chart in the repository is formatted by core's tests
(`tests/unit/test_tool.c`): its canonical text compiles to the same image,
and formatting it a second time changes nothing.

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

Its options, each applying to every format: the data formats answer the
layout the picture draws.

| Option | What it does |
|---|---|
| `--direction down\|right` | How the layers run: top to bottom (`down`, the default) or left to right. A chart with long labels and few levels may read better `right`. |
| `--theme auto\|light\|dark` | The picture's colours. `auto` (the default) writes both sets of tokens, the dark one under `@media (prefers-color-scheme: dark)`, so the reader's preference chooses; `light` or `dark` writes only that set. |
| `--highlight STATE` | Marks the state's box (class `ta-highlight`, the theme's `--ta-highlight` accent); repeat it for more. |
| `--path --to STATE [--from STATE]` | Marks the path the `path` command finds (from the initial configuration, or from `--from`): the states it passes through, and the transitions it takes, drawn in the `--ta-path` accent (class `ta-path`), each edge with a badge numbering its steps from 1 (`2,5` for a transition taken twice). A step whose transition is not drawn (it stays inside a folded box) keeps its number. No path: exit 1, as `path` does. |
| `--depth N` | Folds every compound or parallel state nested deeper than `N` levels (a top-level state is at level 1): `--depth 0` draws the top-level states only. |
| `--fold STATE` | Folds that compound or parallel state; repeat it for more. |

A folded container is drawn as one box on a stack, with the number of
states it stands for under its name (`drawnAs: "folded"` in the data, with
`hides`, the count, on every box when a fold was asked). The transitions of
its hidden states that leave it start at its box, those that enter it end
at its box, an initial state inside it is its box, and those that stay
inside it are not drawn. With `--highlight` or `--path`, every box answers
its `marks` (`highlight`, `path`, both or none); with `--path`, every edge
and every transition listed in a box its `steps`. A state these options
name that no chart drawn has, a `--fold` of a state that is no container,
a `--to` or `--from` that is a history state, `--path` without `--to` and
`--to` or `--from` without `--path` exit with 2.

```sh
tinyactors graph -f svg --direction right --theme dark order.scxml > order.svg
tinyactors graph -f svg --path --to shipped --highlight cancelled order.scxml > shipping.svg
tinyactors graph -f svg --depth 1 --fold checkout shop.scxml > overview.svg
```

Every answer ends with the drawing's **problems**, found by geometry alone
(`ta_diagram_check`): `startDetached`, `endDetached` and `initialDetached`
(an arrow that does not touch its box's drawn outline: the rectangle, a
final's pill, a history state's circle), `notOrthogonal`, `outsideParent`
and `boxesOverlap`, which no layout may have, and the faults the layout still
makes, to be worked down: `notCentred` (an edge alone on a side, off its
middle), `throughBox`, `arrowAlongSide`, `labelOverlap`, `labelOnBox` and
`labelOnEdge`, `labelAstray` (a label further from its own transition's
line than its own height) and `edgesOverlap` (two transitions' lines less
than 3 px apart for more than 10 px, so they read as one; where both end at
one point, the 10 px before it are where they meet, not counted). The SVG carries its own stylesheet, scoped: its classes and
ids are prefixed (`ta-`, `tinyactors-`) and its rules written under the
root's class `tinyactors-diagram`, so the drawing put inline in a page takes
none of the page's styles and gives it none.

The layout is checked by a corpus of charts and their expected layouts and
problems (`tests/diagram`, its README), each laid out down, and one with
folding and a path.

## Run

**Experimental.** `tinyactors run` and its protocol may change in any
release, without a deprecation period, until a second client uses them.
The schema (`tinyactors run --schema`) describes the release you have.

`tinyactors run [options] [INPUT...]` makes one host with the ECMAScript
datamodel (quickjs), adds the inputs' image, and serves **JSON-RPC 2.0 on
standard input and output, one message per line**. It is a workbench: an
agent or a CI job loads charts, spawns actors, sends events, answers the
actors' sends, moves the clock and reads what happened, without a language
binding. Its own methods are the host's calls, under `run.`; every other
method is the host's control plane ([requests.md](requests.md)),
forwarded. What actors do comes back as `effect.*` notifications, each
written before the response to the request that caused it. The clock is
simulated: it moves only when the client says so.

The protocol is defined by a JSON Schema (2020-12),
[`tools/tinyactors/run.schema.json`](../tools/tinyactors/run.schema.json),
which `tinyactors run --schema` writes and the method `run.schema` answers:
every message either way, so a client can be generated from it. The server
checks the params of every method the schema has against it before it
calls anything.

### Lines

- Each message is one JSON text (RFC 8259) in UTF-8 on one line, ended by
  `\n`; the server writes compact JSON. A blank line is ignored, a `\r`
  before the `\n` dropped, and an unended last line at the end of input is
  a message. A line longer than `--max-message` (64 MiB) is answered with
  -32600 `limit` and skipped to its newline.
- A request has `"jsonrpc": "2.0"`, `method`, `params` (an object; absent:
  none) and `id` (an integer, a string or null), and no other member;
  without `id` it is a notification, applied with no response. A message
  that is not one is answered with -32600 and `"id": null`; one that is not
  JSON, or repeats a key in an object, with -32700.
- A batch (an array) runs its elements in order, as if each came alone;
  their notifications are written as they come, and the responses as one
  array after the last (none when every element is a notification). An
  empty batch is one -32600.
- The server reads a line, answers it, then reads the next: responses come
  in request order. Standard error is free text for people.

### Startup and the end of input

```text
tinyactors run [--config FILE] [--system ID] [--io-processor TYPE[=LOCATION]]...
               [-W] [-I DIR]... [--boot] [--wall-start NS] [--seed N] [--max-ops N]
               [--memory-limit BYTES] [--realm auto|isolated] [--max-message BYTES]
               [--schema] [INPUT...]
```

Nothing is required: `tinyactors run order.scxml` works.

| Option | Meaning | Default |
|---|---|---|
| `--config FILE` | the host config ([host-config.md](host-config.md)) as JSON, its `ioProcessors` a map of each processor's type to `{"location": ...}` (`location` optional), written as core's list in the object's order | every key core's |
| `--system ID` | the config's `systemID`; refused when the config names another | `tinyactors-run` |
| `--io-processor TYPE[=LOCATION]` | an I/O processor the client serves, after the config's; repeatable; a type named twice is refused | none |
| `INPUT...` | an image, a manifest or charts, read as every command reads them ([Inputs](#inputs); not `-`, standard input being the client's), added as segment 0; `-W` refuses one with a warning | none: the client adds images with `run.addImage` |
| `--boot` | the manifest's boot actors spawned (`ta_host_boot`) | off |
| `--wall-start NS` | the wall clock's start, nanoseconds since the Unix epoch | 0 |
| `--seed N` | `Math.random`'s seed; 0 is the engine's own, different on every run | 1 |
| `--max-ops`, `--memory-limit`, `--realm` | quickjs's limits and realm (`datamodel_ecmascript.h`) | quickjs's |
| `--max-message BYTES` | the longest line taken | 64 MiB |
| `--schema` | write the protocol's JSON Schema and exit | |

The default system id is `tinyactors-run`: core has none, so that two
systems never share one by accident, and a lone workbench's id appears only
in its own addresses. A client that connects two servers as each other's
transport names each with `--system`.

The server gives the host the platform's allocator, quickjs with the seed
and the host's clock (`Date` and `performance.now()` read the simulated
clocks), the default trace policy and a nonce of 0. With the config's
`system.traceSeed` absent too (core's default is 0), a session repeats byte
for byte, trace ids included; `Date`'s local-time methods follow `TZ`. It
reads `host.config` once for `system.ticksPerSecond`, the system id and the
I/O processors' types. A config core refuses, or an input that does not
compile, is told on standard error and the command exits with 1 before it
reads a message (3 for a file it cannot read).

The end of standard input ends the server: there is no `shutdown` method.
It acks a held batch (its senders go on, as if the client had taken their
sends without an answer: success is silence), writes what is pending; with
telemetry, ends the open spans (`trace.end`) and writes every span and log
record the host still holds ([Telemetry](#telemetry)); then frees the host
and quickjs and exits with 0. Nothing runs at the end: what it writes
follows from the last message alone.

### Methods

A `run.*` method is a host call, its params the call's struct fields in
camelCase. Values are JSON, or `cbor` (below). Every byte string is
base64url without padding (RFC 4648, section 5). A target is a session (an
integer) or a local address (`ta_host_resolve`). Every result is an object.

| Method | Params | Result | Calls |
|---|---|---|---|
| `run.addImage` | one of `files` (paths, read as `INPUT`s are), `sources` (`[{name, text}]`, the first the default boot chart, or a manifest; a `src` names another by its name), `image` (a `.tai`); `warningsAsErrors` | `segment`, `charts` (`[{name, chart}]`), `diagnostics` (as `check -f json` writes them) | `ta_compile_charts` or `ta_compile_system`, `ta_host_add_image` |
| `run.boot` | `segment` | `{}` | `ta_host_boot` (a manifest's boot actors) |
| `run.chart` | `name` | `segment`, `chart` | `ta_host_chart` |
| `run.spawn` | `chart` (a name: its current version) or `segment` and `chart` (an index); `name`, `params` or `paramsCBOR`, `traceparent` | `session`, `address` | `ta_host_chart`, the chart's datamodel's `from_wire`, `ta_host_spawn` |
| `run.send` | `target`, `event`; `data` or `cbor`; `sendid`, `origin`, `origintype`, `raw`, `traceparent`; as a service the client runs, `invocation` (the token of its `effect.invoke`), and `done` (no `event`: core's `done.invoke.<id>`) | `{}` | `ta_host_resolve`, `ta_host_locate`, the target's chart's datamodel's `from_wire`, `ta_host_send` (`TA_EVENT_IN_FROM_INVOKE`, `TA_EVENT_IN_INVOKE_DONE`) |
| `run.sendMany` | `events`: `run.send`'s params each | `accepted`: the accepted prefix's length | every event read first (one the server cannot read refuses the request, and nothing is sent), then `ta_host_send_many` |
| `run.fail` | `target` and `sendid` (a send), or `target` and `invocation` (an invocation the client runs) | `{}` | `ta_host_send` with `TA_EVENT_IN_ERROR_COMMUNICATION` (and `TA_EVENT_IN_FROM_INVOKE`): `error.communication` for the sender or the invoker |
| `run.terminate` | `target` | `{}` | `ta_host_terminate` |
| `run.resolve` | `address` | `session` (`null`: none) | `ta_host_resolve` |
| `run.actorByName` | `name` | `session` (`null`: none) | `ta_host_actor_by_name` |
| `run.advanceTime` | `by` or `to`: ticks (an integer), or a duration as SCXML writes a delay (`"30s"`, `"150ms"`); `wallNs` | `time`, `wallNs` | `ta_host_advance_time` |
| `run.turn` | `budget` (fuel per run, default 10,000), `runs` (at most, default 1,000), `hold` | `status`, `detail`, `runs`, `fuel`, `time`, `nextDeadline` (`null`: none, or not known: below) | the turn below |
| `run.ack` | — | `{}` | `ta_host_effects_ack` of the held batch, then what it lets come is written |
| `run.hibernate` | `session` | `blob`: the actor's hibernation blob, the client's to keep | `ta_host_hibernate`, the blob copied, then the ack of the batch it opened (below) |
| `run.wake` | `session`, `blob` (its latest) | `{}` | `ta_host_wake` |
| `run.sweep` | `maxScan`, `limit` | `sessions`: those at rest since the previous sweep, to hibernate | `ta_host_sweep` |
| `run.migrate` | `chart` (a name; none: every chart), `session` (one actor), `wake`, `timeout` (ticks or a duration) | `migration`: its id | `ta_host_migrate` (`TA_HOST_MIGRATE_WAKE`); the actors move in later runs |
| `run.snapshot` | `path` (optional: a file, written whole) | `snapshot`, or `bytes` with `path` | `ta_host_snapshot`, `ta_datamodel_ecmascript_snapshot`, `ta_host_snapshot_pack` (below) |
| `run.restore` | `snapshot` or `path` | `segments` (the live ones), `time`, `wallNs` | `ta_host_snapshot_unpack`, `ta_host_snapshot_image`, `ta_host_restore`, `ta_datamodel_ecmascript_restore`; the old host and quickjs freed after |
| `run.traces` | — | `spans`, as `trace.spans.decode` answers them | `ta_host_traces`, `trace.spans.decode` posted with its bytes, the ack, the drain that answers it |
| `run.metrics` | — | `otlp`: an ExportMetricsServiceRequest | `ta_host_metrics`, copied, then the ack |
| `run.post` | `message`: CBOR (base64url) | `reply`: the reply's CBOR, with the message's id; none when core answered nothing | `ta_host_post`, the message as given but its id (below) |
| `run.schema` | — | `schema`: the protocol's JSON Schema | none |
| any other | as core's | core's `result` or `error`, with the client's id | `ta_host_post` (`actor.state`, `host.stats`, `trace.*`, ...) |

A `run.*` method the server does not have is -32601 of its own.
The only defaults the server owns are `budget` and `runs`, so that a chart
that never idles cannot hang the client, and `--max-message`.

**Forwarded methods** go to `ta_host_post` as the message without
`jsonrpc`, its id replaced by the handle of a row of the server's pending
posts; the reply's result or error comes back with the client's id. Core
answers a method it does not know with -32601. A notification (no `id`)
is posted and answered with nothing, as in both protocols.

**`run.post`** posts a message the client wrote in CBOR, for what JSON
cannot carry (a byte string in an event's data) and to read a reply's exact
values (`undefined`, which JSON writes as `null`). A request's id is
replaced by a row's handle, as a forwarded one's is, and the reply comes
back as CBOR with the client's id in its place. A message core cannot read,
or that is not a request, is posted as it is, and core's reply, its id
null, is the answer; a notification core can read is answered with no
`reply`. Text that is not base64url is -32006.

**Time.** Monotonic time starts at 0 and moves only with
`run.advanceTime`; wall time starts at `--wall-start` and moves by the same
duration (`wallNs` sets it). Neither goes back. `time`, `nextDeadline`, `by`
and `to` are the host's ticks at `system.ticksPerSecond` (default 1e9,
nanoseconds), as `actor.get`'s `due` is; a duration is converted to ticks as
core converts a delay. Advancing and running are separate:
`run.advanceTime` then `run.turn`, or both in one batch; `{"to":
nextDeadline}` moves to the last turn's next deadline.

**The turn.** `run.turn` is the host's loop: it takes, handles and acks
what is pending, then, at most `runs` times, advances the clocks, runs
(`ta_host_run` with `budget`), and takes, handles and acks again, writing
every effect as a notification before its batch's ack. It goes on after
`TA_RUN_EFFECTS`, `TA_RUN_EFFECTS_FULL` and `TA_RUN_YIELDED`, and after an
idle run whose effects gave the host an input (a refused request) or
whose telemetry it took (which moves the deadline the run gave); it
answers `status` `idle` (with `nextDeadline`), `yielded` (the runs spent),
`held` (below), `exhausted` (`detail` the pool) or `fault` (`detail` the
status's name). A run's outcome is not an error: the host goes on.
With telemetry, the clocks are advanced after each run too, so the run's
span records are stamped at the time it ran, not stretched to the next
advance. Without `hold`, a send's waiting sender goes on at the ack, so an answer
the client gives later arrives as an ordinary event.

### Hibernation and migration

The blobs are the client's: the server keeps none. `run.hibernate`
answers an actor's blob (an actor at rest: one parked after a waiting send
is not, -32002), copied into the response before the ack that ends the
host's copy. With no batch open, taking it opens a batch of its own, which
the server acks before it answers; while a batch is held, the blob joins
that batch, and the client's `run.turn` or `run.ack` ends both. An event
for a hibernated actor waits in the host, as do its delayed sends, and
`effect.wake` asks for the actor; the client answers with `run.wake` and
the latest blob it took of it (an earlier one is -32001, another
session's or another host's -32005, both at `params.blob`). Forwarded
methods about a hibernated actor are core's refusal (`actor.state`:
-32002, "wake it first"). `run.sweep` finds the actors at rest since the
previous sweep, `limit` at a time.

`run.migrate` starts a migration to the current version of each chart,
moved in later runs: each actor moved is an `effect.migrated`, a
hibernated one waits for its blob (an `effect.wake` naming the migration,
with `wake`) or the timeout, and `effect.migrationDone` reports how many
moved, those that could not (with the status) and those left pending. A
segment with no actors left and no current chart retires
(`effect.segmentFreed`).

### Snapshots

`run.snapshot` packs the whole host (`ta_host_snapshot_pack`: every
segment's region, the images, the system id and the host's time) with the
server's state, labelled `tinyactors-run`: quickjs's state (every
session's variables and `_event`, the payloads, `Math.random`'s state) and
the wall clock, as CBOR `{"quickjs": bytes, "wallNs": uint}`. It answers
the pack as `snapshot`, or writes it whole to `path` and answers its
`bytes`; a path of `-` is refused (-32602), standard input and output
being the protocol's. Core takes it only from a quiescent host: while a
batch is held it is refused (-32002), and so it is while a migration runs,
until its `effect.migrationDone`. A variable quickjs cannot store (a
function held in a `<data>`) refuses it with -32007, the message naming
the variable. `tinyactors inspect` and `tinyactors actors` read the file it
writes ([Snapshots and blobs](#snapshots-and-blobs)).

`run.restore` replaces the server's host, in place, with one made from a
pack `run.snapshot` wrote: a new quickjs with the server's datamodel
limits, `ta_host_restore` with the server's config and the pack's images,
then quickjs's state restored before anything runs. Only once the new
host is whole are the old host and its quickjs freed; a refused restore
leaves the server as it was. A pack another program wrote (another label),
of another system id, or that does not unpack is -32005; it is refused
while a batch is held (-32002). The clock goes on from the snapshot's
time, monotonic and wall, which may be earlier than the replaced host's:
the result says where. Sessions, names and delayed sends are the
snapshot's, and `Math.random` draws what it drew after the snapshot. The
restored host keeps the snapshot's identity, so a blob taken before the
snapshot, or after it, wakes in it. The first `run.turn` after a restore
announces each invocation that was running as an `effect.invoke` with
`restored`: the client starts its stand-in again, or fails it. With
telemetry, once the new host is whole the old one's open spans are ended
and what it still holds is written (`effect.traces`, `effect.logs`) before
the response, as at the end of input. Spans the snapshot held unsent come
again from the restored host, with the same ids.

### Telemetry

A config with a `telemetry` key gives the host an encoder
([host-config.md](host-config.md)): its trace records become OTLP spans,
its `<log>` lines and failed actions log records, staged until a request is
ready (the staging is full, or `flushInterval` has passed since its first
record, which is a deadline of `run.turn`). The server takes each request
during dispatch and writes it: spans decoded by core (`effect.traces`,
written when the decode's reply comes, after the batch's ack, with the
`TRACES` effect's `batch` and `index`), log records as OTLP's bytes
(`effect.logs`: core has no decoder for logs). `run.traces` takes the spans
staged now, decoded, and `run.metrics` the host's counters; both are
refused (-32002) while a batch is held and without telemetry. Forwarded
`trace.configure`, `trace.watch` and `trace.unwatch` choose what is traced,
and a forwarded `trace.end` ends the open spans. The server posts
`trace.end` itself, with an encoder, at the end of input and to a host a
restore replaces, and writes what it gives, taking until a take writes
nothing (at most 64 takes of each). A take the allocator refuses is told
on standard error and taken again at the next `run.turn`: core gives no
second effect until a take. Span times are the wall clock's, simulated: a
record written between runs (a spawn's) is stamped with the time of the
last run before it that did work. The raw OTLP of `effect.logs` and `run.metrics` names the
library's version.

### Held batches

With `hold`, `run.turn` leaves open a batch that has a send whose sender
waits (`waiting` in its `effect.send`: to an I/O processor, to another
system or to a service the client runs): every notification of the batch
is written, and the turn answers `status` `held`. The senders stay parked
until the client's next `run.turn` (with or without `hold`) or `run.ack`
acks the batch, or the input ends. What the client sends meanwhile is in
their queues when they go on: an answer (`run.send` with the send's
`sendid`), or `run.fail`'s `error.communication`, which is how SCXML
describes a `<send>` that failed. A held turn's `nextDeadline` is not to
be relied on (`null`, or the deadline the segments had before): the run
that stops at waiting sends does not compute its segment's, and the next
`run.turn` answers it.

`run.ack` acks the held batch and writes what the ack lets come, without
holding it, so that its response leaves no batch open; nothing runs.
`run.ack` with no batch held is -32002 `state`.

| While a batch is held | |
|---|---|
| `run.send`, `run.sendMany`, `run.fail`, `run.terminate`, `run.spawn`, `run.boot`, `run.wake` | allowed: inputs apply now, their effects come after the ack |
| `run.chart`, `run.resolve`, `run.actorByName`, `run.schema` | allowed: lookups |
| `run.hibernate` | allowed: the blob joins the held batch; it is copied into the response at once |
| `run.sweep`, `run.migrate` | allowed: a migration's effects come in later runs |
| `run.advanceTime` | allowed: the clock moves, nothing runs |
| `run.turn`, `run.ack` | allowed: they ack the held batch |
| forwarded methods | refused, -32002 `state`: core answers a post made while a batch is open only after its ack, so the response could not come before the next request; an actor's state cannot be read while its sends wait |
| `run.post` | refused, -32002 `state`, as forwarded methods are |
| `run.traces`, `run.metrics` | refused, -32002 `state`: they take an output in a batch of their own and ack it, which would ack the client's batch |
| `run.addImage`, `run.snapshot`, `run.restore` | refused, -32002 `state`: images are added to, and snapshots taken of and restored to, a quiescent host |

### Notifications

Every notification carries `batch` (the server's count of batches handed
out, from 1) and `index` (the effect's place in its batch). A field the
effect does not set is left out.

| Effect | Notification `params` | The client may |
|---|---|---|
| `TA_EFFECT_SEND` to an I/O processor of the config | `effect.send`: `session`, `type`, `target`, `event`, `sendid`, `data`/`cbor`, `traceparent`, `waiting` | answer: `run.send` to `session` with `sendid`, `origintype` the `type`, `traceparent` |
| `TA_EFFECT_SEND` to another system (the transport) | `effect.send`: `session`, `message` (`target`, `event`, `data`/`cbor`, `sendid`, `origin`, `traceparent`), `waiting` | pass `message` to another server's `run.send` as it is; nothing (success is silence) |
| `TA_EFFECT_SEND` to an invoked service | `effect.send`: `session`, `invocation` (the token), `segment`, `target`, `event`, `sendid`, `data`/`cbor`, `traceparent`, `waiting` | answer as the service: `run.send` with `invocation` |
| `TA_EFFECT_INVOKE` | `effect.invoke`: `session` (the invoker), `segment`, `invocation` (the token), `type`, `src`, `invokeid`, `data`/`cbor` (the invoke's params), `restored` | run the service: `run.send` with `invocation` (and `done` to end it), or `run.fail` with `invocation` |
| `TA_EFFECT_UNINVOKE` | `effect.uninvoke`: `session` (the invoker), `segment`, `invocation` | stop the service; its token is stale from now on |
| `TA_EFFECT_LOG` | `effect.log`: `session`, `label`, `text` | |
| `TA_EFFECT_DONE` | `effect.done`: `session`, `name`, `final`, `data`/`cbor` (the donedata) | |
| `TA_EFFECT_ERROR` | `effect.error`: `session`, `status` (the fault's name) | |
| `TA_EFFECT_REQUEST` | `effect.request`: `session`, `src`; the server answers `ta_host_resume` with `TA_ERROR_INVALID` (the parent gets `error.execution`) | |
| `TA_EFFECT_WAKE` | `effect.wake`: `session`, `migration` (when a migration asked for it) | `run.wake` with the actor's latest blob |
| `TA_HOST_EFFECT_MIGRATED` | `effect.migrated`: `session`, `segment` and `chart` (where it runs now), `migration` | |
| `TA_HOST_EFFECT_MIGRATION_DONE` | `effect.migrationDone`: `migration`, `migrated` (how many), `failed` (`[{session, status}]`), `pending` (sessions); the lists read during dispatch (`ta_host_migration_failed`, `ta_host_migration_pending`) | wake the pending and migrate them again |
| `TA_HOST_EFFECT_SEGMENT_FREED` | `effect.segmentFreed`: `segment` | |
| `TA_HOST_EFFECT_TRACES` | `effect.traces`: `spans`, as `trace.spans.decode` answers them; the server takes the request (`ta_host_traces`) and posts its decode during dispatch, and writes the notification when the reply comes, after the batch's ack | |
| `TA_HOST_EFFECT_LOGS` | `effect.logs`: `otlp`, the ExportLogsServiceRequest the server takes (`ta_host_logs`) | |
| `TA_HOST_EFFECT_NOTIFICATION` | written as it is, its `method` and `params` (none is defined) | |

The client runs every service a chart invokes (`<invoke>` of a type
that is not a chart): an invocation is its segment and token, never its
invoke id. Every view an effect's notification reads is read and written before the
ack, held batch or not.

### Values

Neutral values are CBOR ([protocol.md](protocol.md#values)), converted at
the edge with RFC 8949's rules (sections 6.1 and 6.2):

| CBOR | JSON written | JSON read |
|---|---|---|
| integers | a number, exact over -2^64 … 2^64-1 | a number whose value is an integer in that range, written with or without a fraction or exponent (`7`, `7.0`, `7e0`) |
| floats | the shortest decimal that reads back as the double (`-0.0` kept); NaN, ±Infinity as `null` | any other number, the nearest double, ties to even; `-0` and `-0.0` |
| text | a string | a string: UTF-8 checked, a lone surrogate refused |
| bytes | base64url text | — |
| arrays, maps with text keys | arrays, objects | the same; a repeated key refused |
| `false`, `true`, `null` | themselves | themselves |
| `undefined` | `null` | — |
| a tag (an address, 32) | its item | — |

Where writing changed a value (bytes, `undefined`, a non-finite float, a
tag), its CBOR follows as the field's twin: `data` and `cbor`, `params` and
`paramsCBOR`. On input, `cbor` instead of `data` gives a value JSON cannot
write. A payload is made by the datamodel of the chart that reads it (the
target's, the spawned chart's): data for a chart of the null datamodel is
-32602 `range`. A client that reads session ids with a JavaScript
`JSON.parse` loses bits past 2^53 (segment k numbers its sessions from
k * 2^32 + 1): parse integers as big integers.

### Errors

The error object is core's: `{"code", "message", "data": {"reason",
"path"}}`, with core's codes and reasons. The server's own errors add
`data.status`, the name of the status a C call refused with.

| Condition | Code | Reason |
|---|---|---|
| not JSON, or a repeated key | -32700 | `malformed` |
| not a request: not an object, `jsonrpc` not `"2.0"`, no `method`, `params` not an object, an `id` of another type, a member a request does not have, an empty batch, nested too deep, a line past `--max-message` | -32600 | `type`, `missing`, `unknown`, `limit` |
| no such method | -32601 | `unknown` |
| a param missing, of the wrong type, unknown, out of range; `data` for a chart that reads no values; the clock moved back | -32602 | `missing`, `type`, `unknown`, `range` |
| `TA_ERROR_STALE` (no such session or name; a blob not the session's latest) | -32001 | `stale` |
| `TA_ERROR_STATE` (a snapshot while a migration runs; telemetry without an encoder); a method refused while a batch is held; `run.ack` with none held | -32002 | `state` |
| `TA_ERROR_LIMIT` | -32003 | `limit` |
| `TA_ERROR_NO_MEMORY` | -32004 | `memory` |
| `TA_ERROR_INVALID` (another system's address, a chart no image has, charts that do not compile: the message holds their diagnostics; a blob of another session or host; a snapshot that is not the server's, or a file that cannot be read or written) | -32005 | `invalid` |
| data the datamodel cannot read, `cbor` that is not base64url | -32006 | `malformed` |
| `TA_ERROR_UNSUPPORTED` (a value outside the value table; a variable a snapshot cannot store) | -32007 | `unsupported` |
| a refusal no call documents | -32603 | `internal` |

Memory a message cannot get is that message's -32004, and the server goes
on.

### Exit status

| Status | When |
|---|---|
| 0 | the input ended |
| 1 | an input or the config was refused at startup |
| 2 | the command line was not understood |
| 3 | an input could not be read, standard input could not be read, standard output could not be written (the client went away), or memory ran out at startup or for the line buffer |

### Example

`order.scxml` asks the payment processor to charge its total and waits at
most 30 seconds; `config.json` is `{"system": {"ticksPerSecond": 1000},
"ioProcessors": {"payments": {}}}` (milliseconds). `>` is a line the
client writes, `<` one the server writes; `…` elides a traceparent. The
whole session is `tests/run/order`, which core's tests run.

```text
$ tinyactors run --config config.json order.scxml
> {"jsonrpc":"2.0","id":1,"method":"run.spawn","params":{"chart":"order","name":"order-17","params":{"total":4200}}}
< {"jsonrpc":"2.0","id":1,"result":{"session":1,"address":"tinyactors://tinyactors-run/#_scxml_1"}}
> {"jsonrpc":"2.0","id":2,"method":"run.send","params":{"target":1,"event":"checkout","data":{"card":"tok_visa"}}}
< {"jsonrpc":"2.0","id":2,"result":{}}
> {"jsonrpc":"2.0","id":3,"method":"run.turn","params":{"hold":true}}
< {"jsonrpc":"2.0","method":"effect.send","params":{"batch":1,"index":0,"session":1,"type":"payments","target":"charges","event":"charge","sendid":"pay","data":{"amount":4200,"card":"tok_visa"},"traceparent":"00-…-03","waiting":true}}
< {"jsonrpc":"2.0","id":3,"result":{"status":"held","runs":1,"fuel":11,"time":0,"nextDeadline":null}}
> {"jsonrpc":"2.0","id":4,"method":"actor.state","params":{"session":1}}
< {"jsonrpc":"2.0","id":4,"error":{"code":-32002,"message":"a batch is held: answer its sends, then run.turn or run.ack","data":{"reason":"state","path":""}}}
> {"jsonrpc":"2.0","id":5,"method":"run.send","params":{"target":1,"event":"charge.ok","sendid":"pay","origintype":"payments","data":{"charge":"ch_81"}}}
< {"jsonrpc":"2.0","id":5,"result":{}}
> {"jsonrpc":"2.0","id":6,"method":"run.turn"}
< {"jsonrpc":"2.0","method":"effect.log","params":{"batch":2,"index":0,"session":1,"label":"order","text":"paid ch_81"}}
< {"jsonrpc":"2.0","method":"effect.done","params":{"batch":2,"index":1,"session":1,"name":"order-17","final":"paid","data":{"charge":"ch_81"}}}
< {"jsonrpc":"2.0","id":6,"result":{"status":"idle","runs":1,"fuel":17,"time":0,"nextDeadline":null}}
```

The order is parked after its `<send>` while the batch is held, and its
state cannot be read. The answer to the charge, given before the next
`run.turn` acks the batch, is in the order's queue when it goes on, and
the order ends paid. The rest of the session spawns a second order in a
batch with its `checkout` and a `run.turn` without `hold`, never answers
its charge, moves the clock 30 seconds and reads the order back in `open`
with the forwarded `actor.state`. `tests/run` holds more sessions:
refusals, values and their CBOR twins, a send to another system, a refused
request, sends answered and failed while held (`hold`), and services the
client runs (`services`).

## How it is built

The layers, from the bottom:

- **`src/cli`**, freestanding like `src/base`: `arguments.h` reads a
  command line against a program's description (its commands, their
  options and arguments), and `report.h` builds an answer as one tree and
  writes it in the three formats; `json.h` reads JSON text into CBOR and
  writes it back (RFC 8949, sections 6.1 and 6.2), its numbers read by
  `decimal.h`, a port of Go's `strconv`; `rpc.h` cuts standard input into
  lines and reads JSON-RPC 2.0 envelopes. None uses the C library, recurses
  or allocates but from the arena or allocator it is given.
- **`src/analysis`**: what can be known about a chart without running
  it (`analysis.h`, freestanding like `src/cli`): `lint`'s findings and
  `path`'s search, as arrays in the arena given, knowing nothing of the
  report.
- **`src/diagram`**: a chart laid out (`diagram.h`, freestanding like
  `src/cli`) and drawn as SVG through `ta_xml_writer` (`svg.h`).
- **`tools/tinyactors`** but one file: the commands. They compile without
  the C library's headers and reach files only through the platform
  interface in `tool.h`. One invocation is one arena: the parsed line, the
  files read, the compiled image, a restored snapshot's host and the answer
  live in it, and it is freed once when the command ends. `snapshot.c`
  reads snapshots and blobs with core's readers: `ta_host_snapshot_unpack`
  and `ta_host_restore`, then the system's inspection readers
  (`ta_actor_config`, `ta_system_actor_timers`, `ta_system_actor_mailbox`,
  `ta_system_actor_name`), and the blob readers (`ta_blob_read`,
  `ta_blob_session_at`, `ta_blob_configuration`, `ta_blob_delayed_at`,
  `src/system/system_internal.h`).
  `run.c`, `serve.c` and `effects.c` are `run`: its startup and message
  loop, its methods, and its drain, which writes each effect as a
  notification before the ack. A message's CBOR, tokens, converted values
  and the lines written for it live in a request arena, released once it
  is answered; a `run.addImage` reads and compiles there
  (`tool_image_make`). `schema_check.c` checks params against tables
  generated from `run.schema.json`.
- **`tools/tinyactors/platform.c`**: `main`, the allocator behind the
  arena, reading files and standard input (whole, or as it comes for
  `run`), writing streams, ignoring `SIGPIPE`, and writing a file whole (a
  temporary file renamed over it). The only file that uses the C library.

The program is built with core (`mise run build`, `build/dev/bin/tinyactors`)
and tested by running it (`tests/unit/test_tool.c`). Each release carries
it for every native target ([distribution.md](distribution.md#the-tinyactors-command)).
