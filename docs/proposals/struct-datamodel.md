# A struct datamodel: the application's types, functions and data paths

**Status:** Sketch (2026-11-21; rewritten 2026-11-23, see "What changed
from the first sketch").
**Touches:** tinyactors-core (a new freestanding layer,
`src/datamodel_struct`, and its header); each binding that offers it
(`@tinyactors/wasm` first; Swift, Kotlin and Go when they exist); the
website's anthill.
**Not a plan:** nothing here is decided or scheduled. ADRs and roadmaps win.

## Goal

A chart whose variables are a struct the application declares in its own
language, whose `<data>`, `<assign>` and every `*expr` attribute are data
paths into that struct and the event, and whose decisions and actions are
the application's functions, called by name with arguments the chart
names:

```xml
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0"
       datamodel="struct" name="ant" initial="worker">
  <datamodel>
    <data id="colony" expr="0"/>
    <data id="hp" expr="2"/>
    <data id="queen"/>
  </datamodel>
  <state id="worker">
    <transition event="fight" cond="weak(hp)" target="dead"/>
    <transition event="fight">
      <assign location="hp" expr="hurt(hp, _event.data.damage)"/>
    </transition>
    <transition event="food">
      <send targetexpr="queen" event="food">
        <param name="amount" expr="_event.data.amount"/>
      </send>
    </transition>
  </state>
  <final id="dead"/>
</scxml>
```

The state is a fixed number of bytes known before the first spawn; a guard
is one indirect call on those bytes; the chart says what flows where, and
the code says only how to compute.

## Whom it serves, and who pays

- **First, the application developer in a compiled language without an
  `eval`:** Swift and Kotlin (no expression language to run in either,
  on iOS and Android), Go, C, and C, Rust or Zig compiled to wasm under
  `@tinyactors/wasm`. Today their charts must be ECMAScript on quickjs,
  with their own code only in services and I/O processors. Here their
  code is the chart's logic, in their types, with nothing interpreted
  between.
- **Then the capacity planner:** a session's datamodel cost is its
  struct's size, known from the types, not measured after the fact.
- **Then the binding author:** the datamodel is one C layer every binding
  links. A binding adds only how its language describes a type and how it
  calls a function (below, "Per language"); nothing of the datamodel is
  written twice.
- **Who pays:**
  - core: one freestanding layer (a path parser, a type checker run at
    attach, the conversions between the struct and CBOR, two `ta_table`s),
    linked only by those who register it; size **unverified**;
  - per event, on this datamodel: `bind_event` keeps a reference on the
    payload; each guard or action is its arguments copied into a buffer and
    one indirect call; per event on other datamodels: nothing;
  - per actor: the struct's size, rounded to its alignment, and a session
    row;
  - each binding: a type describer and a function trampoline (below).
- **Out of scope:** Python's and Node's own datamodels, which already run
  their language. A sandbox: the functions are the application's trusted
  code, as a native ECMAScript chart's code is; charts written by someone
  else belong on quickjs. A type system in SCXML: the chart names
  variables and functions, and the application's types say what they are.

## What exists, and why it does not do this

| Datamodel | Where a guard runs | Why not this |
|---|---|---|
| `null` (core) | nowhere: conditions are `In()` only | no variables |
| `"ecmascript"` on quickjs | an interpreter inside core | the application's logic is written again in JavaScript; a heap per session |
| a binding's native datamodel (`"python"`, Node's `"ecmascript"`) | the language's own runtime | needs a language that evaluates text at run time; Swift, Kotlin, Go and C have none |
| staticjs (its datamodel not built) | a pure subset of JavaScript compiled in C | an expression language of its own; not chosen now |

## The model

Three things, and nothing else: **types** the application declares,
**functions** it registers by name, and **data paths** the chart writes.
The layer is configured once per host with all three; each chart on
`"struct"` names its session's type by the chart's name.

### Types are descriptors the application's language fills

Core does not lay out memory. A type is a descriptor: its size, its
alignment, and its fields, each with a name, a kind and an offset. C fills
it with `offsetof` and `sizeof`; a binding fills it from its language's
own type (below). Core checks every descriptor once: fields inside the
size, aligned, not overlapping. It then reads and writes only through
those offsets, so a wrong descriptor gives wrong values but never a write
outside a session's bytes.

| Kind | In memory at its offset | As a neutral value (CBOR) |
|---|---|---|
| `BOOL` | 1 byte, 0 or 1 | `false`, `true` |
| `I32`, `I64` | two's complement, the machine's byte order | an integer (refused out of range) |
| `F64` | IEEE double | a float, or an integer exactly representable |
| `TEXT` (capacity N) | a `uint32_t` length, then N bytes of UTF-8 | text (refused when longer than N or not UTF-8) |
| `ADDRESS` (capacity N) | as `TEXT`, holding an actor's address | tag 32 on the text |
| `STRUCT` (a type) | the nested type's bytes | a map of its fields by name |
| an array of any of these (capacity N) | a `uint32_t` count at its `count_offset`, then N elements at its stride | an array of at most N |
| `VALUE` | a payload handle (8 bytes), 0 for undefined | any neutral value |

- **Fixed capacity, inline.** Text and arrays live inside the struct with a
  capacity the descriptor states, so a session is one block of known size,
  hibernation and snapshots copy no pointer, and every language can read
  it as bytes. A write past a capacity is `error.execution`.
- **`VALUE` is the escape hatch:** a field that holds any neutral value
  as a payload reference (the datamodel's own rows, below), for data the
  application does not want to type, such as a whole `_event.data` kept
  for later. A function receives it as CBOR bytes, valid during the call.
  It is the only field that is not inline, and the only one whose size a
  capacity planner cannot read from the type.
- **The chart's `<data>` ids are exactly the type's fields.** A `<data>`
  with no field, or a field with no `<data>`, refuses the attach: the
  chart declares the state (SCXML 5.2), the type lays it out. `<data
  expr>` initializes the field (below, "Data paths"); no `expr` leaves it
  zero.

### Functions are called by name, with arguments the chart names

A function is registered under a name with its kind, its argument type
(a descriptor like any other: argument *i* is field *i*) and, for a
value function, its result's kind:

| Kind | Used in | Gets | Answers |
|---|---|---|---|
| guard | `cond` | the session's fields, read only; its arguments | a `BOOL` result |
| value | any value expression: `<assign expr>`, `<param expr>`, `<data expr>`, `<log expr>`, `*expr` | the same | its result, converted to the location's kind |
| action | `<script>` (executable content, and the chart's top-level `<script>` once at the start) | the session's fields, writable; its arguments | nothing |

Every function has one signature, in every language:

```c
typedef struct ta_struct_call {
    void *context;          /* the function's own, as registered: a closure, a handle, an index */
    void *fields;           /* the session's struct; a guard and a value function must not write it */
    const void *arguments;  /* laid out as the function's argument type */
    void *result;           /* a value function's result, laid out as its result kind */
    ta_string_view message; /* a failing function may set it: error.execution's message */
    ta_bytes (*value)(const struct ta_struct_call *call, uint64_t handle); /* a VALUE field's CBOR */
} ta_struct_call;

typedef int32_t (*ta_struct_function)(ta_struct_call *call); /* 0: done; nonzero: error.execution */
```

- **`context` is what makes it work in every language.** A C function
  ignores it; a binding registers one trampoline and passes its closure's
  handle (a Swift box, a JNI global reference or an index, a Go
  `cgo.Handle`, a wasm table index). The first sketch's `int32_t
  f(void *fields)` had no context, so only C and wasm side modules could
  provide functions.
- **Arguments, not scratch fields.** A function reads `_event.data`
  through the arguments the chart passes it (`hurt(hp, _event.data.damage)`),
  converted to the argument's kind before the call. It never parses CBOR
  and never needs a field that only exists to carry an event's value in.
- **A failure has a message.** A nonzero return is `error.execution`; the
  datamodel keeps `message` for its `failure` entry
  (`TA_DATAMODEL_FAILURE_EXCEPTION`), and core gives the event its
  `{"reason", "message"}` data, which this datamodel's `from_wire` reads
  like any value.
- **Each call charges one unit of fuel.** A function must end: no fuel
  stops it once called.
- **A guard can write the fields in C** (the pointer is not enforced), so
  SCXML B's "no side effects" holds by contract, as on the native
  ECMAScript datamodel; decision 4 is whether to enforce it by copy.

### Data paths and the expression grammar

Every expression of a chart on `"struct"` is one of four forms, parsed and
checked once at `chart_attach`:

```
expression := path | literal | call | "In(" string ")"
call       := name "(" [ argument { "," argument } ] ")"
argument   := path | literal
path       := root { "." name | "[" ( integer | path ) "]" }
root       := name | "_event" | "_sessionid" | "_name"
literal    := integer | decimal | string | "true" | "false" | "null"
```

No operators and no nesting: what needs arithmetic or a comparison is a
function. That keeps the parser small, every expression's cost one copy
and at most one call, and every check at attach.

- **A path into the fields is typed:** `order.items[2].qty` is checked
  against the descriptors at attach, so a misspelt name refuses the
  chart before any actor runs. An index that is a path or out of the
  array's count at run time is `error.execution`.
- **A path into the event is not:** `_event.data.damage` is read from the
  payload's CBOR when evaluated and converted to the kind it is used as;
  a key that is absent, or a value of another kind, is `error.execution`.
  `_event.name`, `.type`, `.sendid`, `.origin`, `.origintype` and
  `.invokeid` are text; `_event.data` alone is the whole value.
- **Literals:** integers; decimals only where Clinger's fast path is exact
  (at most 15 significant digits and a small exponent), since a
  freestanding layer has no `strtod` and core has no correctly rounded
  reader; strings in single or double quotes.

Where each form may appear:

| SCXML | Takes |
|---|---|
| `cond` | a guard call, a `BOOL` path, `In('state')`, `true`, `false` |
| `<data expr>`, `<assign expr>`, `<param expr>`, `<content expr>`, `<log expr>`, `<donedata>` | a path, a literal or a value call |
| `<assign location>`, `<foreach item index>`, `idlocation`, `namelist` | a path into the fields (`namelist`: read, and named by its last segment) |
| `eventexpr`, `targetexpr`, `typeexpr`, `srcexpr`, `delayexpr` | a path, a literal or a value call of kind `TEXT` or `ADDRESS` |
| `<foreach array>` | a path to an array field |
| `<script>` | an action call |

- **Assignment converts by kind.** A struct path assigned a map sets the
  fields the map names and zeroes the rest; a key with no field is
  `error.execution`. An array assigned an array of more than its capacity
  is `error.execution`.
- **A spawn's or an invoke's params** set the fields of the same name
  (SCXML 5.2.2, 6.4.3), as an assignment of the params map to the whole
  struct but without zeroing fields it does not name.
- **`<foreach>` binds `item` and `index` to fields the chart declares.** A
  struct cannot grow, so a `<foreach>` whose item is not a `<data>`
  refuses the attach; SCXML 4.6 would declare it (W3C 150 and 151 test
  that), which goes on this datamodel's skip list.

### Payloads are CBOR

A payload is a row holding one neutral value's CBOR bytes and a reference
count, in the datamodel's payload table (`ta_table`) through the host's
allocator.

- `capture` writes the `<param>`s, `namelist` or `<content>` straight
  from the struct to CBOR through the descriptors; `to_wire` and
  `payload_serialize` hand the bytes out as they are; `from_wire` and
  `payload_deserialize` check them and keep them.
- **Nothing is limited by count:** the first sketch's eight pairs of
  23-byte names were a layout of its own; here a payload is the value
  table's.
- **A payload is already a neutral value,** so a payload that crosses to a
  chart on another datamodel needs only `to_wire` on one side and
  `from_wire` on the other. Whether core does that today is open (below,
  "Open in core").

### Hibernation, migration, snapshots

- `serialize` writes `{"vars": {name: value}, "event": [...] | null}`, the
  form the other datamodels write, from the descriptors. By name, so
  `deserialize` under another version of the chart, or in a host whose
  language laid the struct out differently, sets the fields both
  declare. Raw bytes would be shorter and are refused for that reason:
  the layout belongs to one build of the application.
- `migrate` answers the old fields as one map payload; core initializes
  the new chart's added `<data>` and raises `error.platform.upgrade` with
  it (`include/tinyactors/datamodel.h`, `migrate`).
- `variable` (`actor.context`) is a field's value as a new payload.
- A snapshot writes the tables' handle namespaces (`ta_table_snapshot`)
  and every row by name, as hibernation does.

## In core

A layer beside `datamodel_ecmascript`, freestanding, with the shape of
quickjs's header. Every function and callback takes at most three
parameters (root `AGENTS.md`).

```c
/* include/tinyactors/datamodel_struct.h */
enum {
    TA_STRUCT_BOOL = 1, TA_STRUCT_I32, TA_STRUCT_I64, TA_STRUCT_F64,
    TA_STRUCT_TEXT, TA_STRUCT_ADDRESS, TA_STRUCT_STRUCT, TA_STRUCT_VALUE
};

typedef struct ta_struct_field {
    ta_string_view name;
    uint32_t kind;         /* TA_STRUCT_* */
    uint32_t offset;       /* from the start of the struct, as the language laid it out */
    uint32_t capacity;     /* TEXT, ADDRESS: bytes; an array: elements; else 0 */
    uint32_t count_offset; /* an array's uint32_t count; TA_NIL for a single value */
    uint32_t stride;       /* an array's element size */
    uint32_t type;         /* STRUCT: the index of its type */
} ta_struct_field;

typedef struct ta_struct_type {
    ta_string_view name;
    uint32_t size, align;
    uint32_t field_count;
    const ta_struct_field *fields;
} ta_struct_type;

enum { TA_STRUCT_GUARD = 1, TA_STRUCT_VALUE_FUNCTION = 2, TA_STRUCT_ACTION = 3 };

typedef struct ta_struct_function_entry {
    ta_string_view name;
    uint32_t kind;            /* TA_STRUCT_GUARD / _VALUE_FUNCTION / _ACTION */
    uint32_t arguments;       /* the index of its argument type; TA_NIL for none */
    ta_struct_field result;   /* a value function's result, at offset 0; a guard's is BOOL */
    ta_struct_function function;
    void *context;
} ta_struct_function_entry;

typedef struct ta_struct_chart {
    ta_string_view chart;     /* a chart's name */
    uint32_t type;            /* its session's type */
} ta_struct_chart;

typedef struct ta_datamodel_struct_config {
    uint32_t size;
    uint32_t type_count, function_count, chart_count;
    const ta_struct_type *types;
    const ta_struct_function_entry *functions;
    const ta_struct_chart *charts;
    ta_allocator allocator;   /* its tables' rows and sessions' bytes */
} ta_datamodel_struct_config;

TA_API ta_status ta_datamodel_struct_new(const ta_datamodel_struct_config *config,
                                         ta_datamodel_struct **out);
TA_API void ta_datamodel_struct_free(ta_datamodel_struct *datamodel);
TA_API ta_datamodel_binding ta_datamodel_struct_binding(ta_datamodel_struct *datamodel);
TA_API ta_status ta_datamodel_struct_last_error(ta_datamodel_struct *datamodel, ta_buffer *out);
TA_API ta_status ta_datamodel_struct_snapshot(ta_datamodel_struct *datamodel, ta_buffer *out);
TA_API ta_status ta_datamodel_struct_restore(ta_datamodel_struct *datamodel, ta_bytes state);
```

- `ta_datamodel_struct_new` checks every descriptor (inside the size,
  aligned, no overlap, a nested type that exists and does not contain
  itself) and every function's argument type; `last_error` names what it
  refused, and why an attach was refused (the chart, the expression's line
  and column, the name).
- A host passes the binding beside its other datamodels
  (`ta_host_environment.datamodels`, at most 8, `host.h:112`). Nothing in
  `include/tinyactors/datamodel.h` changes.
- Storage: a session table whose row is the chart's type and the handle of
  the session's bytes in a table of blocks of that type's size; the
  instance handle core keeps is the session row's.

### Open in core

- **A failed attach leaks the charts attached before it.**
  `attach_charts` returns at the first failure without detaching charts
  `0..c-1` (`src/system/system.c:243-258`, checked 2026-11-23). No
  datamodel fails an attach in practice today; this one does for every
  misspelt name. A fix in core first, with a test.
- **Payloads between datamodels (plausible, unverified).** ADR 0009,
  decision 7: "a payload is handed only to the datamodel whose index it
  carries". The first sketch's review found paths that may not hold to it:
  the event view's data whatever its datamodel
  (`src/core/event_pool.c:177`), an invoke's params reaching the child's
  `create` (`src/system/route.c:296`, `src/core/core.c:2115-2136`), and
  done.invoke's data going back to the parent (`route.c:830-831`). A core
  test settles it. Until then a struct chart exchanges data only with
  struct charts and the host.

## Per language

What each binding writes: a **describer** (its type to a `ta_struct_type`)
and a **trampoline** (one C function that calls the closure in
`context`). Each cost below is **unverified** until its binding's spike.

| | Types | Functions | A call costs |
|---|---|---|---|
| **C** | descriptors by hand, with `offsetof` and `sizeof` | plain C functions | an indirect call |
| **wasm** (`@tinyactors/wasm`) | a side module's C, Rust or Zig, which exports the descriptors (below) | the side module's functions, through the core's table | `call_indirect`: 3.4 ns on Node.js (spike, below) |
| **Swift** | `MemoryLayout<T>.size`, `.alignment` and `.offset(of: \T.field)` (SE-0210); fixed text and arrays as `InlineArray` (SE-0453, Swift 6.2), or a struct imported from a C header; a macro could write the describer | typed closures, `(Int32, Int32) -> Int32`, read from `arguments` by the binding; one `@convention(c)` trampoline | the trampoline and the closure; no copy |
| **Kotlin** | no value types over native memory: the binding lays the struct out itself (C's rules) and gives a typed view over it | typed lambdas; one JNI trampoline, `context` an index into the binding's table of functions | a JNI call into Kotlin per guard or action, and the session's bytes copied into and out of a reused `ByteArray` (`SetByteArrayRegion`), or a direct buffer; S0's spike measures which |
| **Go** | `reflect` and `unsafe.Offsetof`, from a struct of fixed-size fields; Go's own layout, which may differ from C's on 32-bit targets, is what the descriptor says | `func` values; one `//export` trampoline, `context` a `cgo.Handle` | a cgo call into Go per guard or action; or, with no cgo, the core's wasm module under a Go wasm runtime, functions as host functions |

- **Swift and Go read the session's bytes as their own struct,** through
  a pointer, since their descriptor is their compiler's layout. Kotlin
  cannot, and pays a copy or a buffer view; its gain is still that the
  logic is Kotlin's and not JavaScript's.
- **Every binding tests its describer against core's check:** a struct of
  each kind, nested, with arrays and text, round-tripped through `capture`
  and `from_wire`.

### In `@tinyactors/wasm`

```js
import { compileCharts, Host } from '@tinyactors/wasm';

const ant = await WebAssembly.compile(await (await fetch('ant.wasm')).arrayBuffer());
const host = new Host(compiled, { system: 'anthill', structs: ant });
```

- **`structs: WebAssembly.Module`**, a side module (WebAssembly's
  [dynamic-linking convention](https://github.com/WebAssembly/tool-conventions/blob/main/DynamicLinking.md)),
  instantiated once per module and package instance: its data in a block
  from `ta_wasm_alloc` (`__memory_base`), table slots grown for it
  (`__table_base`), the core's `__stack_pointer`; then
  `__wasm_apply_data_relocs` and `__wasm_call_ctors`.
- **It exports one function the package calls,** `ta_struct_config`,
  returning the address of its `ta_datamodel_struct_config` in the shared
  memory: the same value a C embedder builds, compiled against the same
  header for wasm32, so the package reads nothing of its own format.
- **The core's module changes how it links:** it imports its table
  (`--import-table`), which the package makes growable (`zig cc` refuses
  `--growable-table`), and exports `__stack_pointer`.
- **What the package refuses before it instantiates:** a module with no
  `dylink.0` section or no `__memory_base` import; one that imports
  anything but the convention's names and `memory`; a function in the
  config whose table entry's type is not `(i32) -> i32`, since a
  `call_indirect` of another type traps.
- **A trap in a function** unwinds through the core to the export
  JavaScript called; the package refuses every later call into its module
  (`lib/module.mjs:105-147`, `#poison`), so every host of that module ends.
- Not checked: a function that grows memory (whether the core's allocator
  tolerates it is **unverified**), or that runs the core's stack below 0.

The spike behind this (2026-11-21, session scratchpad `spike-table/`):

- A function of a module that imports the core's memory, set into the
  core's table, is called by `call_indirect` from C and reads and writes
  the core's memory in place: 3.43 ns a call on Node.js, 2.84 on Bun,
  2.88 on Chromium, against 6.47, 6.65 and 8.05 ns through a JavaScript
  import.
- A module linked with `--import-memory` alone places its data at 1024
  and its stack near 66 KB of the shared memory, inside the core's stack
  (`tinyactors.wasm`: `__stack_pointer` at 393216): it overwrote the
  spike's data. A side module does not: its data landed in the block the
  core allocated, its stack in the core's stack, and its functions ran
  correctly through the table.

## The anthill on it

The ant and the queen move to `"struct"`, their functions (`weak`, `hurt`,
`isEgg`, `canLay`, `lay`, `add`) in a side module the website builds with
the root's pinned clang. The queen's `food + _event.data.food` becomes
`<assign location="food" expr="add(food, _event.data.food)"/>`.

Measured on `@tinyactors/wasm` 0.4.0 (x86_64 Linux 6.18, 4 CPUs, Node.js
22.23.5, 2026-11-21; one colony of 500,000 ants, 300 frames of 50 ms, the
last 200 timed):

| The ant on | Per decision | Per spawn |
|---|---|---|
| `"ecmascript"`, native | 7.36, 7.20 µs | 10.33, 10.83 µs |
| `null` (`hp` as a parallel region) | 4.22, 4.12 µs | 3.69, 3.90 µs |

The null row bounds the datamodel entries' cost per event; no `fight`
came, so no guard ran in either row. This datamodel's figure is
**unverified** until built.

## Lifetimes

| Object | Owner | Created | Read by | Ends |
|---|---|---|---|---|
| the config's descriptors, names, functions and contexts | the application | before `ta_datamodel_struct_new` | the datamodel, at every call | `ta_datamodel_struct_free`, which the application calls after `ta_host_free` |
| a chart's compiled expressions | the datamodel | `chart_attach` | every evaluation of the chart | `chart_detach` |
| a session's row and its bytes | the datamodel's tables | `create`, `deserialize` | its functions, during a call | `destroy` (after `serialize` too) |
| a call's `arguments`, `result` and `value` bytes | the datamodel | before the call | the function | the call's return |
| a failing call's `message` | the function | the call | the datamodel, which copies it | the call's return |
| a payload's row | the payload table | `capture`, `from_wire`, `payload_deserialize`, a `VALUE` assignment | `_event.data`, `to_wire`, `VALUE` fields | `release` to 0 |
| `serialize`'s and `to_wire`'s bytes | the datamodel | the call | core, which copies them | the datamodel's next call (`datamodel.h:9-13`) |

## Conformance

The W3C suite's charts use the `conf:` notation, which `confPython.xsl`
translates for the python datamodel (`tinyactors-test/suites/w3c`). A
`confStruct.xsl` could translate each `conf:` operation into a call of a
small library of functions the adapter registers, with every variable a
`VALUE` field, so the suite runs on this datamodel through the same
runner. How many tests that passes, and the skip list it needs (W3C 150
and 151, and every test of the ECMAScript data model), is **unverified**.
The scenarios that need no strings beyond paths run on it as they are.

## What changed from the first sketch

| The first sketch | Now |
|---|---|
| fields are doubles only, at 8 × index | the application's types, with their language's layout, described |
| `int32_t f(void *fields)`, no context | `int32_t f(ta_struct_call *)` with a context: closures in any language |
| a function sees only the fields; event data reaches it through a scratch field | arguments named by the chart: `hurt(hp, _event.data.damage)` |
| `<assign location>` a bare field name; no strings, so no `eventexpr`, `targetexpr`, `<content>`, `<foreach>` | data paths everywhere; `TEXT`, `ADDRESS` and arrays; every SCXML element but `<foreach>` declaring a variable |
| payloads of at most 8 pairs of 23-byte names | CBOR, the value table, no limit |
| an action's failure carries no data | a message, given to `error.execution` |
| no layout check | descriptors checked by core; every name and path checked at attach |
| wasm only, other bindings out of scope | one layer for C, wasm, Swift, Kotlin and Go |

## Decisions for the maintainer

1. **Callbacks in a public header.** `tinyactors-core/AGENTS.md` allows
   "no callbacks except the datamodel vtable" (the allocator and the
   resolver are there too). Accept `ta_struct_function` as part of a
   datamodel's configuration?
2. **The name** `"struct"`, and the wasm option `structs`.
3. **The kinds.** Unsigned integers, `F32`, or bytes are left out; `VALUE`
   is in. Add or remove before the first binding uses them.
4. **Guards' purity.** By contract (as above), or by copy: a guard and a
   value function get a copy of the fields, which costs the struct's size
   per call and makes SCXML B hold by construction.
5. **Exact `<data>` to fields,** or fields the chart does not declare,
   kept by the application (an index, a cache) and invisible to paths.
6. **No operators.** Every comparison is a function (`cond="weak(hp)"`,
   not `cond="hp <= 1"`). A few operators on paths and literals (`==`,
   `<`, `!`, `+`) would make small charts readable without a function
   each, at the cost of a grammar that grows.
7. **The anthill: this, or the null datamodel it measures against,** once
   this is built.

## Stages

1. **Core:** the attach leak fixed, the mixed-datamodel test written; the
   layer, with unit tests of every vtable entry and every refusal, and
   `ta-conform` registering it with a C test library so the scenarios run
   on it.
2. **`@tinyactors/wasm`:** the table import, the side-module loader, the
   anthill moved, and both rows measured again.
3. **Each new binding** (Swift, Kotlin, Go): its describer and trampoline,
   the conformance run, and the cost per call measured beside quickjs.
