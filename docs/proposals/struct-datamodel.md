# A struct datamodel: fixed fields, the application's compiled functions

**Status:** Sketch (2026-11-21).
**Touches:** tinyactors-core (a new datamodel layer, `src/datamodel_struct`,
and its header), tinyactors-napi's `@tinyactors/wasm` (loading the
application's functions into its module), the website's anthill.
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
  game, a fleet of devices), and whose logic is already in a compiled
  language. They ask how many actors and decisions a machine holds; today
  the ECMAScript datamodel's work is the largest part of a decision
  (measured below).
- **Then the capacity planner:** a session's datamodel cost becomes 8
  bytes a field, known from the chart, in the module's memory where the
  page's memory chart already counts it.
- **Who pays:** core gains one datamodel layer (about the size of the null
  datamodel plus a small expression reader, payloads and CBOR; estimated,
  **unverified**), freestanding, linked only by those who use it. The
  wasm package gains the loader of a side module and one host option. Per
  event: nothing for charts on other datamodels; a chart on this one pays
  one indirect call per guard or action. Node.js and Python bindings: no
  work (below, "Out of scope").
- **Out of scope:** `@tinyactors/node` and `tinyactors` for Python. Their
  application functions would be JavaScript or Python, which is what the
  native datamodels already are; native functions there would need a
  loadable library per platform. Sandboxing: these functions are the
  application's trusted code, as a native chart's ECMAScript is. Charts
  written by someone else belong on quickjs, or on staticjs once its
  datamodel exists.

## What exists, and why it does not do this

| Datamodel | Where a guard runs | Cost per session | Why not this |
|---|---|---|---|
| `null` (core) | nowhere: no data, conditions are `In()` only | none | no variables; the ant's `hp` must become states |
| `"ecmascript"`, native (`@tinyactors/wasm`, `@tinyactors/node`) | the application's JavaScript engine, through the import `ta_web.datamodel` per vtable entry (`tinyactors-napi/wasm/src/datamodel.c:59-68`) | a scope object and its values on the JavaScript heap (Node: 197 B of heap at rest, `tinyactors-napi/AGENTS.md`, "Measured cost per session"; the wasm package's is **unverified**) | each entry crosses into JavaScript and runs the realm's code |
| `"ecmascript"`, quickjs | quickjs inside the module | quickjs's heap per session | an interpreter per evaluation |
| staticjs ([proposal](staticjs.md), its datamodel not built) | a pure, total subset of JavaScript compiled to bytecode in C | values by reference count | built for code written by someone else: no side effects, every evaluation ends. Its actions are `<assign>` of new values; it has no way to call the application's compiled code |

A struct datamodel is the fast path for the application's own code. It
does not replace staticjs: one runs logic the host trusts, the other
logic it does not.

## Measured

All on one machine (x86_64 Linux 6.18, 4 CPUs, Node.js 22.23.5, Bun
1.4.1, Chromium 157), 2026-11-21, the website's anthill
(`website/examples/anthill`, one colony of 500,000 ants on
`@tinyactors/wasm` 0.4.0, 300 frames of 50 ms, the last 200 timed):

| The ant on | Per decision | Per spawn |
|---|---|---|
| `"ecmascript"`, native | 7.36, 7.20 µs | 10.33, 10.83 µs |
| `null` (the same behaviour: `hp` as a parallel region, the stage as two charts) | 4.22, 4.12 µs | 3.69, 3.90 µs |

The null datamodel bounds what any datamodel can reach: a struct
datamodel adds an indirect call and a few loads per guard or action, so
it should land near the null row (**unverified** until built). A CPU
profile of the native run put about 21% of the time in the datamodel's
JavaScript and most of the host's JavaScript share in the crossings; of
the null run, 50% in the module, 20% in the world (the application's
I/O processor) and 22% in the package's JavaScript around effects.

A spike (in the session's scratchpad: `spike-table/`) settled how a
second module's functions reach the core's code without JavaScript:

- A function exported by a module that imports the core's memory, set
  into the core's function table, is called by `call_indirect` from C and
  reads and writes the core's memory in place: 3.43 ns a call on Node.js,
  2.84 on Bun, 2.88 on Chromium, against 6.47, 6.65 and 8.05 ns for the
  same work through a JavaScript import.
- **A module built the usual way corrupts the core.** Compiled with
  `--import-memory` alone, its data segment was written at address 1024 of
  the shared memory when it was instantiated (the core's own data), and a
  function using its stack wrote below its own stack pointer, at about
  66 KB (the core's stack): 186 bytes of the core's memory changed.
- **A side module does not** (the dynamic-linking convention of
  [WebAssembly's tool conventions](https://github.com/WebAssembly/tool-conventions/blob/main/DynamicLinking.md),
  `clang --target=wasm32 -fPIC -Wl,--experimental-pic -Wl,-shared`): it
  imports `memory`, `__indirect_function_table`, `__stack_pointer`,
  `__memory_base` and `__table_base`, and its `dylink.0` section says how
  much data it needs. Given a block the core allocated as its memory base
  and the core's own stack pointer, its data landed inside its block and
  its stack inside the core's stack; the same two functions ran correctly
  through the table.

## The datamodel

Named `"struct"` in a chart's `datamodel` attribute. Every rule below is
what the datamodel does with what core already gives a datamodel
(`include/tinyactors/datamodel.h`): it needs no change to core's
interface.

### Variables are a struct of doubles

- Every `<data>` of the chart, in document order, is a field of 8 bytes
  holding an IEEE double, at offset 8 × its index. One type, because the
  values that reach a chart are numbers in every binding and in CBOR, and
  the compiler keeps no type for a `<data>` (`ta_data_info`,
  `include/tinyactors/image.h:86-91`).
- A `<data expr>` is a number literal, `true` (1) or `false` (0); none is
  0. A spawn's or an invoke's params set the fields of the same name
  (SCXML 6.4), read from the init payload at `init_data`.
- A session is one row of a `ta_table` per chart (`table.h`), 8 bytes a
  field, rounded to 8: the ant's two fields are 16 bytes, its row's slot
  in the table a few more.

### Expressions are names

`chart_attach` reads every expression of the chart once
(`ta_image_expression`) and turns it into one of these; anything else is
an `error.execution` when it is evaluated, as SCXML has a malformed
expression do (5.9).

| Where | What it may be |
|---|---|
| `cond` | a guard's name; `In('state')`; `true`, `false` |
| `<script>` (in executable content) | an action's name |
| `<assign location>`, `<foreach item index>`, `idlocation` | a field's name (`<foreach>` and `idlocation` refuse: below) |
| `<assign expr>`, `<param expr>`, `<data expr>` | a number literal, a field's name, `_event.data.<name>` |
| `<log expr>` | a string literal (core has no number formatter to write a field with) |
| `eventexpr`, `targetexpr`, `delayexpr`, `typeexpr`, `srcexpr`, `<content>`, `<foreach array>`, `<script>` at the top | refused: no strings, no arrays, no global code |

A function is `int32_t f(void *fields)`. A guard answers nonzero for
true. An action answers 0, or nonzero to fail: an `error.execution` whose
message names it. A guard must not change the fields (SCXML B: conditions
have no side effects); the datamodel cannot enforce it, as the native
datamodel cannot either. Each call charges one unit of fuel
(`ta_datamodel_environment_charge`).

The names come from xstate's `setup({ guards, actions })`: a chart
refers to an implementation by name, and the application provides it.

### Events and payloads

- A payload is a row of up to 8 (name, number) pairs, names of at most 23
  bytes inline: fixed-size, like the fields. `capture` writes `<param>`s
  and namelists into one; more pairs, or a longer name, refuse
  (`error.execution`).
- `bind_event` keeps the event's payload (`retain`) as `_event.data`, read
  only through `_event.data.<name>` (absent: 0). The datamodel sets
  `TA_DATAMODEL_EVENT_DETACHABLE` and keeps the payload's handle, not
  core's event.
- `to_wire` writes a payload as a CBOR map of text to numbers;
  `from_wire` reads only such a map (anything else: `TA_ERROR_INVALID`),
  which is how a binding's `host.send(target, event, {food: 3})` reaches a
  chart here.

### Hibernation, migration, snapshots

- `serialize` writes `{"vars": {id: number}, "event": {name: number} |
  null}` in CBOR, by name, so `deserialize` under a newer chart keeps the
  fields both declare and `migrate` answers the old ones, as the other
  datamodels do.
- `payload_serialize` and `payload_deserialize` are `to_wire` and
  `from_wire`.
- A snapshot writes the tables' handle namespaces (`ta_table_snapshot`)
  and their rows as they are: the fields are already bytes.

### What a function may not do

It is called inside a run, so the rules of `datamodel.h` hold: it does no
I/O and calls nothing of the host. It must end: no fuel stops it. In the
wasm package a trap inside it (a bad memory access, `unreachable`) stops
the core in the middle of a run, so the package's module is poisoned and
every host in it ends, as a fault in core does (`lib/module.mjs`,
`#poison`).

## In core

A new layer beside `datamodel_ecmascript`, freestanding (it needs no C
library), with the shape of quickjs's header:

```c
/* include/tinyactors/datamodel_struct.h */
typedef int32_t (*ta_struct_function)(void *fields);

typedef struct ta_struct_function_entry {
    ta_string_view name;
    ta_struct_function function;
} ta_struct_function_entry;

typedef struct ta_struct_chart {          /* the functions of the chart of that name */
    ta_string_view chart;
    uint32_t function_count;
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

`chart_attach` resolves every name the chart uses among its chart's
functions. A name it does not find refuses the attach, so the host
refuses the image (`src/system/system.c:250-256`): a missing function is
found when the image is added, as a linker finds it, not at the first
event that needs it. A host passes the binding beside its other
datamodels (`ta_host_environment.datamodels`, at most 8).

## In `@tinyactors/wasm`

```js
import { compileCharts, Host } from '@tinyactors/wasm';

const ant = await WebAssembly.compile(await (await fetch('ant.wasm')).arrayBuffer());
const host = new Host(compiled, { system: 'anthill', structs: { ant } });
```

- **`structs: { [chart]: WebAssembly.Module }`**: the functions of each
  chart on the struct datamodel, a side module (above). The package
  instantiates it once per module and package instance: its data in a
  block from `ta_wasm_alloc` (`__memory_base`), its table slots grown in
  the core's table (`__table_base`), the core's `__stack_pointer`, then
  `__wasm_apply_data_relocs`; it sets each export into the table and gives
  the datamodel the indices as its function pointers.
- **The core's module exports two more names:** its table
  (`--export-table --growable-table`) and its `__stack_pointer`. Today it
  exports only `memory` and functions.
- **What the package refuses**, before it instantiates: a module that is
  not a side module (no `dylink.0`, or it does not import its memory
  base and stack pointer: the corruption above), one that imports
  anything else, an export that is not `(i32) -> i32` (read from its type
  section: a `call_indirect` of another type traps), and a chart the
  option names that has no such module. A chart on `"struct"` with none,
  or a function a chart names that its module does not export, refuses
  the image with an error that names the chart and the function.
- **Snapshots:** the package labels one datamodel state per pack today;
  it writes the struct datamodel's state beside the ECMAScript one in
  that state, as the Python binding packs its two
  (`tinyactors-cpython/AGENTS.md`, "Snapshots").

## The anthill on it

The ant and the queen move to `"struct"`; `ant.c` and `queen.c` hold six
functions (`isEgg`, `weak`, `hurt`, `eat`, `lay`, `canLay`), built into
side modules by the website's build with the root's pinned clang (it has
the wasm32 target and `wasm-ld`; the spike used it). The queen's
`food + _event.data.food` becomes `<assign location="gain"
expr="_event.data.food"/><script>eat</script>`. Expected: about 4.3 µs a
decision against 7.3 (the null bound), so four million ants near 0.6×
real time on this machine instead of 0.35× (**unverified**).

## Lifetimes

| Object | Owner | Created | Read by | Ends |
|---|---|---|---|---|
| a chart's resolved functions | the datamodel | `chart_attach` | every evaluation of the chart | `chart_detach` |
| a session's fields | the chart's table | `create`, `deserialize` | its functions, during a call | `destroy` (after `serialize` too) |
| a payload's row | the datamodel's payload table | `capture`, `from_wire`, `payload_deserialize` | `bind_event`'s `_event.data`, `to_wire` | `release` to 0 |
| `serialize`'s, `to_wire`'s bytes | the datamodel | the call | core, which copies them | the datamodel's next call (`datamodel.h:9-13`) |
| a side module's instance, data block, table slots (wasm) | the package's module, by `WebAssembly.Module` | the first host that names it | the datamodel's indirect calls | the package's module (a table never shrinks) |

## Decisions for the maintainer

1. **A callback in a public header.** `tinyactors-core/AGENTS.md` allows
   "no callbacks except the datamodel vtable" (and the allocator and the
   resolver exist). A struct datamodel's functions are callbacks by
   nature. Accept them as part of a datamodel's binding, or keep the
   function table out of the public header (each embedder builds the
   layer with its functions linked in)?
2. **The name** `"struct"`, and the option `structs`.
3. **Doubles only**, or typed fields (`i32`, `i64`, `f32`) declared
   somewhere the compiler keeps. Nothing in the image carries a type
   today.
4. **One module per chart**, or one per host with names qualified by
   chart. Per chart keeps two charts' layouts apart; a name shared by
   charts with different structs would read the wrong bytes.
5. **A layout check.** Nothing ties `struct ant` to the chart's `<data>`
   order. A module could export its field names (a custom section the
   package compares), at the cost of a convention of ours.

## Stages

1. The layer in core, with unit tests of every entry, and `ta-conform`'s
   scenarios that do not need strings, run on it.
2. The wasm package: the two exports, the side-module loader and its
   refusals, `structs`, snapshots; tests under Node.js, Bun and Chromium.
3. The anthill on it, measured against the native datamodel on the same
   machine, side by side.
