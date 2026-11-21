# A struct datamodel: fixed fields, the application's compiled functions

**Status:** Sketch (2026-11-21), revised after an adversarial review the
same day (what it changed is under "Review").
**Touches:** tinyactors-core (a new datamodel layer, `src/datamodel_struct`,
and its header), tinyactors-napi's `@tinyactors/wasm` (its C and its
JavaScript), the website's anthill.
**Not a plan:** nothing here is decided or scheduled. ADRs and roadmaps win.

## Goal

A chart whose variables are a C struct and whose guards and actions are
functions the application compiled, called by name from the chart. A
session's state is a fixed number of bytes; a guard is one indirect call
on those bytes, with no runtime of another language in between.

```xml
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0"
       datamodel="struct" name="ant" initial="worker">
  <datamodel>
    <data id="colony" expr="0"/>
    <data id="hp" expr="2"/>
  </datamodel>
  <state id="worker">
    <transition event="fight" cond="weak" target="dead"/>
    <transition event="fight"><script>hurt</script></transition>
    ...
  </state>
  <final id="dead"/>
</scxml>
```

```c
struct ant { double colony, hp; };            /* the <data> in document order */
int32_t weak(struct ant *ant) { return ant->hp <= 1; }
int32_t hurt(struct ant *ant) { ant->hp -= 1; return 0; }
```

## Whom it serves, and who pays

- **First, the C embedder and the application developer on
  `@tinyactors/wasm`** whose actors are many and simple (a simulation, a
  game, a fleet of devices), and whose logic is in a compiled language
  already. They ask how many actors and decisions a machine holds; today
  the ECMAScript datamodel's work is the largest single part of a
  decision (measured below).
- **Then the capacity planner:** a session's datamodel cost is a fixed
  number of bytes known from the chart, in the module's memory, where the
  anthill's memory chart already counts it.
- **Who pays:**
  - core: one datamodel layer, freestanding, linked only by those who use
    it: the null datamodel's entries plus a reader of names and integer
    literals, two tables, payload rows and their CBOR (size
    **unverified**);
  - `@tinyactors/wasm`: more than one option. Its host takes one datamodel
    today (`wasm/src/host.c:227`), accepts input data only for an
    `"ecmascript"` chart (`ta_web_host_takes_data`, `host.c:693-718`;
    `lib/host.mjs:314-324`), and reads every SEND's payload with its one
    ECMAScript datamodel, whatever the effect's datamodel index says
    (`lib/host.mjs:300-309`). It needs a second binding, payloads routed by
    the chart's datamodel in C and JavaScript, effects read by their
    datamodel index, the side-module loader, and snapshots of two states;
  - per event: nothing for charts on other datamodels; one indirect call
    per guard or action on this one.
- **Out of scope:** `@tinyactors/node` and `tinyactors` for Python. Their
  application code is JavaScript or Python, which their native datamodels
  already run; native functions there would need a loadable library per
  platform. Sandboxing: the functions are the application's trusted code,
  as a native chart's ECMAScript is. Charts written by someone else belong
  on quickjs, or on staticjs once its datamodel exists. Mixed images, where
  a struct chart exchanges data with a chart on another datamodel (below,
  "Open in core").

## What exists, and why it does not do this

| Datamodel | Where a guard runs | Why not this |
|---|---|---|
| `null` (core) | nowhere: conditions are `In()` only | no variables; the ant's `hp` must become states |
| `"ecmascript"`, native (`@tinyactors/wasm`, `@tinyactors/node`) | the application's JavaScript engine, through the import `ta_web.datamodel` for each vtable entry (`wasm/src/datamodel.c:59-76`) | each entry crosses into JavaScript; a session is a scope object on the JavaScript heap (Node: 197 B at rest, `tinyactors-napi/AGENTS.md`; the wasm package's is **unverified**) |
| `"ecmascript"`, quickjs | quickjs inside the module | an interpreter per evaluation, a heap per session |
| staticjs ([proposal](staticjs.md), its datamodel not built) | a pure, total subset of JavaScript compiled to bytecode in C | see below |

staticjs aims at the same speed for actors as a control layer ("a guard
costs tens of nanoseconds, not a call into another runtime"), with no
side effects, modules and a sandbox. Once its datamodel exists it could
run the anthill's ant as it is (`hp <= 1`, `hp - 1`) with no compiled
module at all. What a struct datamodel adds is different: it calls code
the application already compiled, mutates state in place, and a session's
size is fixed by its chart. Which of the two the anthill should show is
decision 6.

## Measured

On one machine (x86_64 Linux 6.18, 4 CPUs, Node.js 22.23.5, Bun 1.4.1,
Chromium 157), 2026-11-21: the website's anthill
(`website/examples/anthill`), one colony of 500,000 ants on
`@tinyactors/wasm` 0.4.0, 300 frames of 50 ms, the last 200 timed. The
review reran it and got 6.89 to 7.22 µs and 4.08 µs.

| The ant on | Per decision | Per spawn |
|---|---|---|
| `"ecmascript"`, native | 7.36, 7.20 µs | 10.33, 10.83 µs |
| `null` (`hp` as a parallel region, the stage as two charts; the queen still on ECMAScript) | 4.22, 4.12 µs | 3.69, 3.90 µs |

What the null row bounds: the cost of the datamodel's entries per event
(`bind_event`, `detach_event`, a cond per transition tried). One colony
has no enemies, so no `fight` came and no guard or action of the ant's
ran in either row; a struct datamodel's indirect calls are not in the
bound, and its estimate (near 4.3 µs) is **unverified** until built.

Profiles of the two runs: the datamodel's JavaScript is about 21% of the
native run (`datamodel.mjs` and the realm); the null run spends 50% in the
module, 20% in the world (the application's I/O processor) and about 23%
in the package's JavaScript around effects. The native profile also holds
15% of shutdown work: 0.4.0's `close()` throws once per actor (fixed in
`lib/host.mjs`, not released), which the profiled whole run included.

A spike (session scratchpad, `spike-table/`) settled how a second
module's functions reach the core's code without JavaScript:

- A function exported by a module that imports the core's memory, set
  into the core's function table, is called by `call_indirect` from C and
  reads and writes the core's memory in place: 3.43 ns a call on Node.js,
  2.84 on Bun, 2.88 on Chromium, against 6.47, 6.65 and 8.05 ns for the
  same work through a JavaScript import.
- **A module built the usual way corrupts the core.** Linked with
  `--import-memory` alone, it places its data at address 1024 and its
  stack near 66 KB of the shared memory: it overwrote the spike's data
  when it was instantiated and 186 bytes when one function ran. In
  `tinyactors.wasm` (stack first: `__stack_pointer` at 393216, data from
  there) both land in the low part of the core's stack.
- **A side module does not** (the dynamic-linking convention of
  [WebAssembly's tool conventions](https://github.com/WebAssembly/tool-conventions/blob/main/DynamicLinking.md),
  `clang --target=wasm32 -fPIC -Wl,--experimental-pic -Wl,-shared`): it
  imports its memory base (and, when it uses them, the stack pointer, the
  table and the table base), and its `dylink.0` section says how many
  bytes of data and zeroed data it needs. Given a block the core allocated
  as its memory base and the core's own stack pointer, its data landed in
  its block and its stack in the core's stack, and both functions ran
  correctly through the table.

## The datamodel

Named `"struct"` in a chart's `datamodel` attribute. It needs no change to
core's datamodel interface (`include/tinyactors/datamodel.h`); what core
must settle first is under "Open in core".

### Variables are a struct of doubles

- Every `<data>` of the chart, in document order, is a field of 8 bytes
  holding an IEEE double, at offset 8 × its index. One type, because the
  compiler keeps none for a `<data>` (`ta_data_info`,
  `include/tinyactors/image.h:86-91`) and every binding's numbers are
  doubles or integers within them.
- A `<data expr>` is an integer literal, `true` (1) or `false` (0); none
  is 0. Integers only, because core has a formatter of doubles
  (`ta_cbor_diagnostic`, `include/tinyactors/cbor.h:80-89`) but no
  correctly rounded reader, and a freestanding layer has no `strtod`: a
  fraction is an action's work.
- A spawn's or an invoke's params set the fields of the same name (SCXML
  6.4.3): `create` keeps the init payload, `init_data` reads it.
- Storage: one session table (`ta_table`, `table.h`) whose row is the
  chart's index and the handle of the session's fields in its chart's
  table of fields (8 bytes a field). The instance handle core keeps is the
  session row's: a handle has all 64 bits (`table.c:65`), and the
  per-instance calls carry no chart (`datamodel.h:126-130`), so the chart
  must be in the row. The ant: 16 bytes of fields and a 16-byte session
  row, each with its slot's metadata.

### Expressions are names

`chart_attach` reads every expression of the chart once
(`ta_image_expression`) and turns it into one of these. A name with no
function, or an expression of another form, refuses the attach: SCXML lets
a processor reject a document with ill-formed expressions when it loads
it (5.9.4), and a missing function is then found when the image is added,
as a linker finds it.

| Where | What it may be |
|---|---|
| `cond` | a guard's name; `In('state')` (`ta_image_state_by_name`); `true`, `false` |
| `<script>` in executable content | an action's name |
| `<assign location>` | a field's name |
| `<assign expr>`, `<param expr>`, `<data expr>` | an integer literal, a field's name, `_event.data.<name>` |
| `<log expr>` | a field's name, written with `ta_cbor_diagnostic`, or a string literal |
| `eventexpr`, `targetexpr`, `delayexpr`, `typeexpr`, `srcexpr`, `idlocation`, `<content>`, `<foreach>`, `<script>` at the top | refused: no strings, no arrays, no global code |

A function is `int32_t f(void *fields)`. A guard answers nonzero for
true. An action answers 0, or nonzero to fail, which is an
`error.execution` (its data: below). Each call charges one unit of fuel
(`ta_datamodel_environment_charge`).

**SCXML B says a datamodel's condition language "MUST not have side
effects".** A guard here is the application's function: the datamodel
cannot keep it from writing the fields. This datamodel does not meet B by
construction; a chart that relies on it, as on the native ECMAScript
datamodel's, trusts the application (decision 7).

The names come from xstate's `setup({ guards, actions })`: a chart
refers to an implementation by name, and the application provides it.

### Events and payloads

- A payload is a row: one number, or a map of up to 8 (name, number)
  pairs whose names are at most 23 bytes, inline. `capture` writes
  `<param>`s and namelists into a map, a `TA_CAPTURE_VALUE` (a trace's
  value) and `variable` (actor.context) into one number. More pairs or a
  longer name refuse (`error.execution`).
- `bind_event` keeps the event's payload (`retain`) as `_event.data`, read
  only through `_event.data.<name>` (absent: 0). The datamodel sets
  `TA_DATAMODEL_EVENT_DETACHABLE`; core releases its own event after
  `detach_event` (`src/core/core.c:534-543`), and the retained row stays.
- `to_wire` writes a number or a CBOR map of text to numbers; `from_wire`
  reads only those (anything else: `TA_ERROR_INVALID`).
- **An action's failure carries no data.** Core builds an
  `error.execution`'s data as `{"reason", "message"}`, two texts, and
  reads it with the datamodel's own `from_wire`
  (`src/core/core.c:211-247`), which refuses text here: the event comes
  without data, and the failing function's name reaches no one
  (decision 8).

### Hibernation, migration, snapshots

- `serialize` writes `{"vars": {id: number}, "event": payload | null}` in
  CBOR, by name, so `deserialize` under a newer chart keeps the fields
  both declare. `migrate` answers the old fields as one map payload: a
  chart with more than 8 `<data>` has none to answer, and core raises
  `error.platform.upgrade` without data (`src/system/hibernate.c:1095`).
- `payload_serialize` and `payload_deserialize` are `to_wire` and
  `from_wire`.
- A snapshot writes the tables' handle namespaces (`ta_table_snapshot`)
  and their rows as they are: the fields are already bytes.

### What a function may not do

It runs inside a run, so the rules of `datamodel.h` hold: no I/O, no call
of the host. It must end: no fuel stops it. In the wasm package a trap
inside it (a bad memory access, `unreachable`) unwinds through the core
to the export JavaScript called; the package catches it there and refuses
every later call into its module (`lib/module.mjs:105-147`, `#poison`):
every host of that module ends, not only the one whose actor trapped.

## In core

A new layer beside `datamodel_ecmascript`, freestanding, with the shape of
quickjs's header:

```c
/* include/tinyactors/datamodel_struct.h */
typedef int32_t (*ta_struct_function)(void *fields);

typedef struct ta_struct_function_entry {
    uint32_t size;
    ta_string_view name;
    ta_struct_function function;
} ta_struct_function_entry;

typedef struct ta_struct_chart {          /* the functions of the chart of that name */
    uint32_t size;
    uint32_t function_count;
    ta_string_view chart;
    const ta_struct_function_entry *functions;
} ta_struct_chart;

typedef struct ta_datamodel_struct_config {
    uint32_t size;
    uint32_t chart_count;
    const ta_struct_chart *charts;
    ta_allocator allocator;               /* its tables' rows */
} ta_datamodel_struct_config;

TA_API ta_datamodel_struct *ta_datamodel_struct_new(const ta_datamodel_struct_config *config);
TA_API void ta_datamodel_struct_free(ta_datamodel_struct *datamodel);
TA_API ta_datamodel_binding ta_datamodel_struct_binding(ta_datamodel_struct *datamodel);
TA_API ta_status ta_datamodel_struct_snapshot(ta_datamodel_struct *datamodel, ta_buffer *out);
TA_API ta_status ta_datamodel_struct_restore(ta_datamodel_struct *datamodel, ta_bytes state);
```

A host passes the binding beside its other datamodels
(`ta_host_environment.datamodels`, at most 8, `host.h:112`).

### Open in core

- **A failed attach leaks the charts attached before it.**
  `attach_charts` returns at the first failure without detaching charts
  `0..c-1` (`src/system/system.c:245-258`), and the host frees the
  segment (`src/host/host.c:666-680`). No datamodel fails an attach in
  practice today; this one would, for every misspelt name. A fix in core
  first, with a test.
- **The attach's refusal says only a status.** A message naming the chart
  and the function needs the datamodel to keep its last error (as
  quickjs's `ta_datamodel_ecmascript_last_error` does), or the binding to
  check the names itself before `addImage` (it reads the image's
  expressions with the same calls).
- **Payloads between datamodels (plausible, unverified).** ADR 0009,
  decision 7: "a payload is handed only to the datamodel whose index it
  carries". The review found paths that may not hold to it: the event
  view's data is the event's payload whatever its datamodel
  (`src/core/event_pool.c:177`), an invoke's params reach the child's
  `create` as `init` (`src/system/route.c:296`, `src/core/core.c:2115-2136`),
  and done.invoke's data goes back to the parent (`route.c:830-831`). A
  core test settles it: an ECMAScript parent that invokes, or sends data
  to, a chart on another datamodel. The Python binding already runs two
  datamodels in one host. Until it is settled, a struct chart exchanges
  data only with struct charts and the host.

## In `@tinyactors/wasm`

```js
import { compileCharts, Host } from '@tinyactors/wasm';

const ant = await WebAssembly.compile(await (await fetch('ant.wasm')).arrayBuffer());
const host = new Host(compiled, { system: 'anthill', structs: { ant } });
```

- **`structs: { [chart]: WebAssembly.Module }`**: the functions of each
  chart on the struct datamodel, a side module. The package instantiates
  it once per module and package instance: its data and zeroed data in a
  block from `ta_wasm_alloc` (`__memory_base`), table slots grown for it
  (`__table_base`), the core's `__stack_pointer`; then
  `__wasm_apply_data_relocs` and `__wasm_call_ctors`, when it exports
  them. It sets each function it exports into the table and gives the
  datamodel the indices as its function pointers.
- **The core's module changes how it links.** The package links it with
  `zig cc` (`mise/wasm.rb:67`), which refuses `--growable-table`, and an
  exported table that cannot grow refuses `Table.grow` (the review's
  spike). So the module imports its table (`--import-table`), which the
  package makes growable and passes in, and exports its
  `__stack_pointer` (`--export=__stack_pointer` works under `zig cc`).
- **What the package refuses**, before it instantiates: a module with no
  `dylink.0` section or no `__memory_base` import (the corruption above);
  one that imports anything but the five names of the convention and
  `memory`; and an export other than the convention's own
  (`__wasm_call_ctors`, `__wasm_apply_data_relocs`, `__dso_handle`) whose
  type, read from its type section, is not `(i32) -> i32`, since a
  `call_indirect` of another type traps. A chart on `"struct"` with no
  module, or a function a chart names that its module does not export,
  refuses `addImage` with an error that names both.
- **Not refused, and not checked:** a function that calls `memory.grow`
  (whether the core's allocator tolerates memory another module grew is
  **unverified**), or that recurses deep enough to run the core's stack
  below 0 (a trap, which poisons the module).
- **Snapshots:** the package labels one datamodel state per pack today
  (`wasm/src/host.c:651-656`); it would write the struct datamodel's state
  beside the ECMAScript one in that state, as the Python binding packs its
  two (`tinyactors-cpython/AGENTS.md`, "Snapshots").

## The anthill on it

The ant and the queen move to `"struct"`; `ant.c` and `queen.c` hold six
functions (`isEgg`, `weak`, `hurt`, `eat`, `lay`, `canLay`), built into
side modules by the website's build with the root's pinned clang (it
targets wasm32 and links with `wasm-ld`; the spike used it). The queen's
`food + _event.data.food` becomes `<assign location="gain"
expr="_event.data.food"/><script>eat</script>`. Both charts on one
datamodel, so the open question of mixed images does not arise.
Expected near 4.3 µs a decision against 7.3 µs (**unverified**).

## Lifetimes

| Object | Owner | Created | Read by | Ends |
|---|---|---|---|---|
| a chart's resolved functions | the datamodel | `chart_attach` | every evaluation of the chart | `chart_detach` |
| a session's row and its fields | the session table and its chart's table | `create`, `deserialize` | its functions, during a call | `destroy` (after `serialize` too) |
| a payload's row | the payload table | `capture`, `from_wire`, `payload_deserialize` | `bind_event`'s `_event.data`, `to_wire` | `release` to 0 |
| `serialize`'s, `to_wire`'s bytes | the datamodel | the call | core, which copies them | the datamodel's next call (`datamodel.h:9-13`) |
| a side module's instance, data block, table slots (wasm) | the package's module, by `WebAssembly.Module` | the first host that names it | the datamodel's indirect calls | the package's module (a table never shrinks) |

## Decisions for the maintainer

1. **A callback in a public header.** `tinyactors-core/AGENTS.md` allows
   "no callbacks except the datamodel vtable" (the allocator and the
   resolver are there too). The functions are callbacks by nature: accept
   them as part of a datamodel's binding, or keep them out of the public
   header and have each embedder build the layer with its functions
   linked in?
2. **The name** `"struct"`, and the option `structs`.
3. **Doubles only**, or typed fields declared somewhere the compiler
   keeps. Nothing in the image carries a type today.
4. **One module per chart**, or one per host with names qualified by
   chart. Per chart keeps two charts' layouts apart.
5. **No layout check.** Nothing ties `struct ant` to the `<data>` order;
   a check would need a convention of ours (a custom section), which the
   bindings' rule to invent nothing weighs against.
6. **The anthill: this, or staticjs.** staticjs's datamodel would run the
   ant unchanged, with no compiled module; this datamodel shows the
   application's own compiled code. Both need work in core before the
   anthill can use them.
7. **SCXML B.** Guards that are the application's functions can have side
   effects. State the deviation in the datamodel's documentation, as the
   native datamodel trusts its charts, or refuse something?
8. **An action's failure.** Without data, or payloads that also hold
   text (a message), or core building the error's data in a way that
   does not go through `from_wire`.

## Stages

1. Core: the attach leak fixed, the mixed-datamodel test written; then
   the layer, with unit tests of every entry and `ta-conform`'s scenarios
   that need no strings run on it.
2. The wasm package: the table import and stack pointer export, the
   side-module loader and its refusals, `structs`, payloads and effects
   routed by datamodel, snapshots; tests under Node.js, Bun and Chromium.
3. The anthill on it, measured against the native datamodel on the same
   machine, side by side, with fights (two colonies), so the guards run.

## Review

An adversarial review (2026-11-21) against the repository, the spikes and
the SCXML recommendation changed this sketch:

- the table export (`--growable-table`) does not link under the
  package's `zig cc`: the module imports its table instead;
- side modules import the stack pointer only when they use it, and export
  the convention's own functions: the refusals were rewritten so they do
  not refuse a correct module;
- an action's failure cannot carry its message through this
  `from_wire` (decision 8);
- core does have a number formatter; what it lacks is a decimal reader,
  so literals are integers;
- a table per chart cannot name its chart in a 64-bit handle: one
  session table with the chart in its rows;
- the wasm package's cost was understated (one datamodel per host, data
  for ECMAScript charts only, SEND payloads read with one datamodel);
- mixed images, the attach leak, the 5.9.4 and B citations, the staticjs
  comparison, and what the null measurement bounds (no guards ran).
