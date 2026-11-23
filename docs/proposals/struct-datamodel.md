# A struct datamodel: the application's types, functions and data paths

**Status:** Sketch (2026-11-21; rewritten 2026-11-23 and revised after an
adversarial review the same day: see "What changed from the first sketch"
and "Review").
**Touches:** tinyactors-core (a new freestanding layer,
`src/datamodel_struct`, and its header; three attach leaks fixed first);
each binding that offers it (`@tinyactors/wasm` first; Swift, Kotlin and Go
when they exist); the website's anthill.
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

The variables are a fixed number of bytes known before the first spawn; a
guard is its arguments copied and one indirect call; the chart says what
flows where, and the code says only how to compute.

## Whom it serves, and who pays

- **First, the application developer in a compiled language:** Swift and
  Kotlin on iOS, macOS and Android, Go, C, and C, Rust or Zig compiled to
  wasm under `@tinyactors/wasm`. Today their charts must be ECMAScript on
  quickjs (or JavaScriptCore on Apple platforms, JavaScript again), with
  their own code only in services and I/O processors. Here their code is
  the chart's logic, in their types, with nothing interpreted between.
- **Then the capacity planner:** a session's variables cost their struct's
  size, known from the types; the rest of a session's cost (below) is
  bounded by core's string limits.
- **Then the binding author:** the datamodel is one C layer every binding
  links. A binding adds only how its language describes a type and how it
  calls a function (below, "Per language"); nothing of the datamodel is
  written twice.
- **Who pays:**
  - core: one freestanding layer (a path parser, a checker run at attach,
    the conversions between the struct and CBOR) and its tables: sessions,
    a table of blocks per type, payloads, and `<foreach>` iterators; size
    **unverified**;
  - per actor: the struct's size, a session row, the `VALUE` handles of
    its type, and copies of `_sessionid` and `_name` (each at most
    `TA_SYSTEM_MAX_STRING`), since core's system variables live only
    during `create` (`datamodel.h:69-70`);
  - per event on this datamodel: `bind_event` keeps a reference on the
    payload and copies the event's sendid and origin (its name, invoke
    and origintype are kept as core's ids, as the Python binding does);
    each guard or action is its arguments copied and one indirect call;
  - per send: one allocation for the captured payload's CBOR;
  - per event on other datamodels: nothing;
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

### Types are descriptors the application's language fills

Core does not lay out memory. A type is a descriptor: its name, size,
alignment and fields, each with a name, a kind and an offset. C fills it
with `offsetof` and `sizeof`; a binding fills it from its language's own
type, with the language's own size (`MemoryLayout<T>.size`,
`unsafe.Sizeof`), since the application's code writes the whole struct
through a pointer (below, "Per language").

| Kind | In memory at its offset | As a neutral value (CBOR) |
|---|---|---|
| `BOOL` | 1 byte, 0 or 1 | `false`, `true` |
| `I32`, `I64` | two's complement, the machine's byte order | an integer (refused out of range) |
| `F64` | IEEE double | a float, or an integer exactly representable |
| `TEXT` (capacity N) | a `uint32_t` length, then N bytes of UTF-8 | text (refused when longer than N or not UTF-8) |
| `ADDRESS` (capacity N) | as `TEXT`, holding an actor's address | tag 32 on the text |
| `STRUCT` (a type) | the nested type's bytes | a map of its fields by name |
| `VALUE` | nothing: the handle lives in the session's row (below) | any neutral value |

Any field but a `VALUE` may be an **array** of at most `count` elements:
a `uint32_t` count at its `count_offset`, then the elements at its
`stride`. An array of text gives each element its own capacity; an array
of arrays is an array of structs.

**Core checks every descriptor once**, in 64-bit arithmetic: the type's
alignment is at most 8 (the host's allocator gives no more,
`allocator.h:9`); every field, every array's elements
(`offset + count × stride`), every count and every text's length word lies
inside the size; a stride is at least its element's size; no two of them
overlap; nested types exist and do not contain themselves, and nest at
most 16 deep, so every walk over a value is a loop with a bound. Core
reads and writes every field with `memcpy`, so a field needs no alignment
(Go lays out an `int64` on 4 bytes on 32-bit targets).

**Core trusts no length the struct holds.** Functions write the struct,
and `deserialize` and a restore fill it, so every read of an array's count
or a text's length is checked against its capacity: one past it is
`error.execution` in an evaluation, and `TA_ERROR_UNSUPPORTED` in
`serialize`, `capture` and `variable` (the actor stays resident, as a
value outside the table keeps a Python actor resident). Text is checked
for UTF-8 where it leaves the struct. A wrong descriptor or a function
that writes garbage gives wrong values, never a read or write outside the
session's bytes by core.

**`VALUE` fields are not in the struct's bytes.** Each session's row holds
one payload handle per `VALUE` field of its type (0 for undefined), so a
function cannot overwrite, forge or copy one: it reads a `VALUE` argument
as CBOR bytes, valid during the call, and only the chart's `<assign>`
sets one. Assigning a struct, or zeroing it, releases the handles it
replaces. A `VALUE` field cannot be an array element or inside an array's
struct, so a type's handle count is fixed.

**Which type a chart's sessions use:** the registered type whose name is
the chart's name and whose fields are exactly the chart's `<data>` ids.
Several types may carry one name (one per version of a chart); a new
version that adds a `<data>` needs its type registered before its image is
added (`ta_datamodel_struct_add`, below), and its attach picks the type
that matches. None or two that match refuses the attach.

### Functions are called by name, with arguments the chart names

A function is registered under a name with its kind, its argument type (a
descriptor like any other: argument *i* is field *i*) and, for a value
function, its result's kind:

| Kind | Used in | Gets | Answers |
|---|---|---|---|
| guard | `cond` | the session's fields, read only; its arguments | a `BOOL` result |
| value | any value or text expression | the same | its result, converted to where it goes; never a `VALUE` |
| action | `<script>` (executable content, and the chart's top-level `<script>` once at the start) | the session's fields, writable but for `VALUE`s; its arguments | nothing |

Every function has one signature, in every language:

```c
typedef struct ta_struct_call {
    uint32_t size;
    uint32_t pad;
    void *context;          /* the function's own, as registered: a closure, a handle, an index */
    void *fields;           /* the session's struct; a guard and a value function must not write it */
    const void *arguments;  /* laid out as the function's argument type */
    void *result;           /* a value function's result, laid out as its result field */
    ta_bytes values;        /* the VALUE arguments' CBOR, one item each in argument order */
    ta_string_view message; /* a failing function may set it: error.execution's message */
} ta_struct_call;

typedef int32_t (*ta_struct_function)(ta_struct_call *call); /* 0: done; nonzero: error.execution */
```

- **`context` is what makes it work in every language.** A C function
  ignores it; a binding registers one trampoline and passes its closure's
  handle (a Swift box, an index into the Kotlin binding's table, a Go
  `cgo.Handle`, a wasm table index). The first sketch's `int32_t
  f(void *fields)` had no context, so only C and wasm side modules could
  provide functions.
- **Arguments, not scratch fields.** A function reads `_event.data`
  through the arguments the chart passes it (`hurt(hp, _event.data.damage)`),
  converted to the argument's kind before the call. It never parses CBOR
  but a `VALUE`'s, and never needs a field that exists only to carry an
  event's value in.
- **A failure has a message.** A nonzero return is `error.execution`; the
  datamodel copies `message` for its `failure` entry
  (`TA_DATAMODEL_FAILURE_EXCEPTION`), and core gives the event its
  `{"reason", "message"}` data (`core.c:211-240`), which this datamodel's
  `from_wire` reads like any value.
- **Fuel:** core already charges after each datamodel call
  (`core.c:89-92`); the layer charges one unit more per function call, and
  nothing per path. A function must end: no fuel stops it once called.
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
root       := name | "_event" | "_sessionid" | "_name" | "_ioprocessors"
literal    := integer | decimal | string | "true" | "false" | "null"
```

No operators and no nesting: what needs arithmetic or a comparison is a
function. That keeps the parser small, every expression's cost one copy
and at most one call, and every check at attach.

- **A path into the fields is typed:** `order.items[2].qty` is checked
  against the descriptors at attach, so a misspelt name refuses the chart
  before any actor runs. An index, a literal or a path to an integer, past
  the array's count at run time is `error.execution`.
- **A path into the event is not:** `_event.data.damage` is read from the
  payload's CBOR when evaluated and converted to the kind it is used as;
  a key that is absent, or a value of another kind, is `error.execution`.
  `_event.name`, `.type`, `.sendid`, `.origin`, `.origintype` and
  `.invokeid` are text; `_event.data` alone is the whole value.
- **`_ioprocessors.<type>.location`** is text, as SCXML 5.10 gives it
  (W3C 325, 326, 329, 346 and 569 read it).
- **Literals:** integers; decimals only where Clinger's fast path is exact
  (at most 15 significant digits and a small exponent), since a
  freestanding layer has no `strtod` and core has no correctly rounded
  reader (only a duration parser, `event_pool.c:58`); strings in single or
  double quotes; `null` only where it goes to a `VALUE`, refused at attach
  elsewhere.

Where each form may appear:

| SCXML | Takes |
|---|---|
| `cond` | a guard call, a `BOOL` path, `In('state')`, `true`, `false` |
| `<data expr>`, `<assign expr>`, `<param expr>`, `<content expr>`, `<donedata>` | a path, a literal or a value call |
| `<log expr>` | the same; a value that is not text is written as CBOR diagnostic notation (`ta_cbor_diagnostic`, `cbor.h:89`) |
| child content of `<data>`, `<assign>`, `<content>` (`TA_EXPRESSION_CONTENT`) | text only, as a `TEXT` literal; XML children refuse the attach |
| `<assign location>`, `<foreach item index>`, `namelist` | a path into the fields (`namelist`: read, and named by its last segment) |
| `idlocation` | a path to a `TEXT` field (core sets it with `assign_string`, `core.c:1055`) |
| `eventexpr`, `targetexpr`, `typeexpr`, `srcexpr`, `delayexpr`, `sendidexpr`, `docexpr` | a path, a literal or a value call of kind `TEXT` or `ADDRESS` (`delayexpr`'s text goes to core's duration parser) |
| `<foreach array>` | a path to an array field, or into `_event.data` |
| `<script>` | an action call |
| a trace's value (`TA_CAPTURE_VALUE`) | a path, written as CBOR; a failure leaves it out of the record |

- **Assignment converts by kind.** A struct path assigned a map sets the
  fields the map names and zeroes the rest, releasing the `VALUE`s it
  replaces; a key with no field is `error.execution`. An array assigned an
  array longer than its capacity is `error.execution`.
- **A spawn's or an invoke's params** set the fields of the same name
  (SCXML 5.2.2, 6.4.3), as an assignment of the params map to the whole
  struct but without zeroing fields it does not name.
- **`<foreach>` iterates a copy** (SCXML 4.6: "a shallow copy"):
  `foreach_begin` copies the array's elements, or keeps a reference on
  the event's payload, into the iterator's row, so a body that changes the
  array does not move the loop. Its `item` and `index` are fields the
  chart declares: a struct cannot grow, so a `<foreach>` whose item is not
  a `<data>` refuses the attach; SCXML 4.6 would declare it (W3C 150 and
  151 test that), which goes on this datamodel's skip list.
- **The vtable's `custom` entry** answers `TA_ERROR_UNSUPPORTED`.

### Payloads are CBOR

A payload is a row in the datamodel's payload table (`ta_table`, fixed
rows, `table.h:16`): a reference count and a block of one neutral value's
CBOR bytes, allocated through the host's allocator when it is made and
freed at its last release.

- `capture` writes the `<param>`s, `namelist` or `<content>` straight from
  the struct to CBOR through the descriptors; `to_wire` and
  `payload_serialize` hand the bytes out as they are; `from_wire` and
  `payload_deserialize` check them (well-formed, at most 16 deep) and keep
  them.
- **Nothing is limited by count:** the first sketch's eight pairs of
  23-byte names were a layout of its own; here a payload is the value
  table's.
- **A payload is already a neutral value,** so a payload that crosses to a
  chart on another datamodel needs only `to_wire` on one side and
  `from_wire` on the other. Core does not do that today, and nothing stops
  such a payload reaching this datamodel (below, "Open in core").

### `_event` and the system variables

The datamodel sets `TA_DATAMODEL_EVENT_DETACHABLE`. `bind_event` keeps the
event's payload (a reference), its type, core's ids of its name, invoke
and origintype (`name_id`, `invoke_slot`, `origintype_id`,
`datamodel.h:41-50`), and copies its sendid and origin, whose text lives
only during the call (`datamodel.h:35-37`); `detach_event` drops nothing
of that, so `_event` stays bound until the next event (SCXML 5.10).
`create` copies `_sessionid` and `_name` into the session's row;
`_ioprocessors` is the chart's, kept once per chart when every session's
match the first's text, as the Python binding keeps it.

### Hibernation, migration, snapshots

- `serialize` writes `{"vars": {name: value}, "event": [...] | null}`, the
  form the other datamodels write, from the descriptors, `VALUE`s
  included. By name, so `deserialize` under another version of the chart,
  or in a host whose language laid the struct out differently, sets the
  fields both declare. Raw bytes would be shorter and are refused for that
  reason: the layout belongs to one build of the application.
- `migrate` answers the old fields as one map payload; core initializes
  the new chart's added `<data>` and raises `error.platform.upgrade` with
  it (`datamodel.h:214-221`).
- `variable` (`actor.context`) is a field's value as a new payload.
- A snapshot writes the tables' handle namespaces (`ta_table_snapshot`)
  and every row by name, as hibernation does.

## In core

A layer beside `datamodel_ecmascript`, freestanding, with the shape of
quickjs's header. Every function and callback takes at most three
parameters (root `AGENTS.md`); every struct the application fills starts
with its `size` (`tinyactors-core/AGENTS.md`, "Public ABI").

```c
/* include/tinyactors/datamodel_struct.h */
enum {
    TA_STRUCT_BOOL = 1, TA_STRUCT_I32, TA_STRUCT_I64, TA_STRUCT_F64,
    TA_STRUCT_TEXT, TA_STRUCT_ADDRESS, TA_STRUCT_STRUCT, TA_STRUCT_VALUE
};

typedef struct ta_struct_field {
    uint32_t size;
    uint32_t kind;         /* TA_STRUCT_* */
    ta_string_view name;
    uint32_t offset;       /* from the start of the struct, as the language laid it out */
    uint32_t capacity;     /* TEXT, ADDRESS: bytes of each value; else 0 */
    uint32_t type;         /* STRUCT: the index of its type */
    uint32_t count;        /* an array's capacity in elements; 0: a single value */
    uint32_t count_offset; /* an array's uint32_t count */
    uint32_t stride;       /* an array's element size */
} ta_struct_field;

typedef struct ta_struct_type {
    uint32_t size;
    uint32_t field_count;
    ta_string_view name;   /* a chart's name, for a session's type; any name for arguments */
    uint32_t bytes, align; /* the language's sizeof and alignof */
    const ta_struct_field *fields;
} ta_struct_type;

enum { TA_STRUCT_GUARD = 1, TA_STRUCT_VALUE_FUNCTION = 2, TA_STRUCT_ACTION = 3 };

typedef struct ta_struct_function_entry {
    uint32_t size;
    uint32_t kind;            /* TA_STRUCT_GUARD / _VALUE_FUNCTION / _ACTION */
    ta_string_view name;
    uint32_t arguments;       /* the index of its argument type; TA_NIL for none */
    uint32_t pad;
    ta_struct_field result;   /* a value function's result, at offset 0; a guard's is BOOL */
    ta_struct_function function;
    void *context;
} ta_struct_function_entry;

typedef struct ta_struct_definitions {     /* what ta_datamodel_struct_add registers */
    uint32_t size;
    uint32_t type_count, function_count;
    uint32_t pad;
    const ta_struct_type *types;
    const ta_struct_function_entry *functions;
} ta_struct_definitions;

TA_API ta_status ta_datamodel_struct_new(ta_allocator allocator, ta_datamodel_struct **out);
TA_API ta_status ta_datamodel_struct_add(ta_datamodel_struct *datamodel,
                                         const ta_struct_definitions *definitions);
TA_API void ta_datamodel_struct_free(ta_datamodel_struct *datamodel);
TA_API ta_datamodel_binding ta_datamodel_struct_binding(ta_datamodel_struct *datamodel);
TA_API ta_status ta_datamodel_struct_last_error(ta_datamodel_struct *datamodel, ta_buffer *out);
TA_API ta_status ta_datamodel_struct_snapshot(ta_datamodel_struct *datamodel, ta_buffer *out);
TA_API ta_status ta_datamodel_struct_restore(ta_datamodel_struct *datamodel, ta_bytes state);
```

- `ta_datamodel_struct_add` checks every descriptor and function as
  above, all or nothing, and copies the descriptors and names: the
  application's arrays need not outlive the call; its functions and
  contexts must outlive the datamodel. A function name registered twice
  refuses the call; a new version's functions take new names (or the
  same function under one name, when its arguments did not change).
- `last_error` names what it refused, and why an attach was refused (the
  chart, the expression's line and column, the name).
- A host passes the binding beside its other datamodels
  (`ta_host_environment.datamodels`, at most 8, `host.h:112`). Nothing in
  `include/tinyactors/datamodel.h` changes.
- Storage: a session table whose row is the type, the handle of the
  session's block in that type's table of blocks, its `VALUE` handles,
  `_sessionid`, `_name` and `_event`; the instance handle core keeps is
  the session row's.

### Open in core

- **A failed attach leaks the charts attached before it,** in three
  places: `attach_charts` returns at the first failure without detaching
  charts `0..c-1` (`src/system/system.c:243-259`, checked 2026-11-23), the
  trace rules that can fail after it (`system.c:386-388`), and the restore
  path (`system.c:1267-1268`). No datamodel fails an attach in practice
  today; this one does for every misspelt name. A fix in core first, with
  a test of each.
- **Payloads between datamodels.** ADR 0009, decision 7: "a payload is
  handed only to the datamodel whose index it carries". It does not hold:
  the event view's data is the event's payload whatever its datamodel
  (`src/core/event_pool.c:177`), an invoke's params reach the child's
  `create` (`src/system/route.c:296`, `src/core/core.c:2115-2136`), and
  done.invoke's data goes back to the parent (`route.c:831`). Two
  datamodels' handles can be equal, so the receiver reads another row
  with no refusal. Core must compare the payload's datamodel with the
  receiver's and convert (`to_wire`, then `from_wire`) or refuse; until
  it does, a host that runs struct charts beside charts on another
  datamodel is unsafe, and the binding must refuse such a host.

## Per language

What each binding writes: a **describer** (its type to a `ta_struct_type`,
with the language's own size) and a **trampoline** (one C function that
calls the closure in `context`). Each binding tests its describer against
core's check: a struct of each kind, nested, with arrays and text,
round-tripped through `capture` and `from_wire`. Each cost below is
**unverified** until its binding's spike.

| | Types | Functions | A call costs |
|---|---|---|---|
| **C** | descriptors by hand, with `offsetof` and `sizeof` | plain C functions | an indirect call |
| **wasm** (`@tinyactors/wasm`) | a side module's C, Rust or Zig (below) | the side module's functions, through the core's table | `call_indirect`: 3.4 ns on Node.js (spike, below) |
| **Swift** | `MemoryLayout<T>.size`, `.alignment` and `.offset(of: \T.field)` (SE-0210); fixed text and arrays as `InlineArray` (SE-0453, Swift 6.2), or a struct imported from a C header; a macro could write the describer | typed closures, `(Int32, Int32) -> Int32`; one `@convention(c)` trampoline reads `arguments` into the closure's parameters | the trampoline and the closure; the fields by pointer, not copied |
| **Kotlin** (JVM and Android, through JNI) | no value types over native memory: the binding lays the struct out itself (C's rules) and gives a typed view over it | typed lambdas; one JNI trampoline, `context` an index into the binding's table of functions | a JNI call into Kotlin per guard or action, and the fields copied into and out of a reused `ByteArray` (`SetByteArrayRegion`), or a direct buffer; the binding's first spike measures which |
| **Go** | `reflect` and `unsafe.Offsetof`, from a struct of fixed-size fields | `func` values; one `//export` trampoline, `context` a `cgo.Handle` | a cgo call into Go per guard or action; or, with no cgo, the core's wasm module under a Go wasm runtime, functions as host functions |

- **Swift and Go read the session's bytes as their own struct,** through
  a pointer, since their descriptor is their compiler's layout. Kotlin
  cannot, and pays a copy or a buffer view; its gain is still that the
  logic is Kotlin's and not JavaScript's. Kotlin/Native would read C
  structs through cinterop instead, which is not this binding.

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
- **It exports one function the package calls,** `ta_struct_definitions`,
  returning the address of its `ta_struct_definitions` in the shared
  memory: the same value a C embedder builds, compiled against the same
  header for wasm32, so the package reads nothing of its own format.
- **The core's module changes how it links:** it imports its table
  (`--import-table`), which the package makes growable (`zig cc` refuses
  `--growable-table`, per the first sketch's review), and exports
  `__stack_pointer` and the C library functions a side module's compiler
  emits for struct copies (`memcpy`, `memset`, `memmove`).
- **What the package refuses before it instantiates:** a module with no
  `dylink.0` section or no `__memory_base` import; an import that is
  neither the convention's (`memory`, `__memory_base`, `__table_base`,
  `__stack_pointer`, `__indirect_function_table`, `GOT.mem.*`,
  `GOT.func.*`) nor one of the core's exports above; a function in the
  definitions whose table entry's type is not `(i32) -> i32`, since a
  `call_indirect` of another type traps. Which imports Rust's and Zig's
  side modules need besides these is **unverified**.
- **A trap in a function** unwinds through the core to the export
  JavaScript called; the package refuses every later call into its module
  (`lib/module.mjs:81-144`, `#poison`), so every host of that module ends.
- Not checked: a function that grows memory (whether the core's allocator
  tolerates it is **unverified**), or that runs the core's stack below 0.

The spike behind this (2026-11-21, a session's scratchpad, not kept;
its figures are as reported then):

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
| the registered descriptors and names | the datamodel (copied) | `ta_datamodel_struct_add` | every attach and call | `ta_datamodel_struct_free` |
| the functions and their contexts | the application | before `ta_datamodel_struct_add` | the datamodel, at every call | after `ta_datamodel_struct_free`, which the application calls after `ta_host_free` |
| a chart's compiled expressions | the datamodel | `chart_attach` | every evaluation of the chart | `chart_detach` |
| a session's row, block and copies of its system variables | the datamodel's tables | `create`, `deserialize` | its functions, during a call | `destroy` (after `serialize` too) |
| `_event`: a payload reference, ids, sendid and origin | the session's row | `bind_event` | paths into `_event` | the next `bind_event`, or `destroy` |
| a call's `arguments`, `result` and `values` bytes | the datamodel | before the call | the function | the call's return |
| a failing call's `message` | the function | the call | the datamodel, which copies it | the call's return |
| a payload's row and its CBOR block | the payload table | `capture`, `from_wire`, `payload_deserialize`, a `VALUE` assignment | `_event.data`, `to_wire`, `VALUE` fields | `release` to 0 |
| a `<foreach>`'s copy | the iterator table | `foreach_begin` | `foreach_item` | `foreach_end` |
| `serialize`'s and `to_wire`'s bytes | the datamodel | the call | core, which copies them | the datamodel's next call (`datamodel.h:9-13`) |

## Conformance

The W3C suite's charts use the `conf:` notation, which `confPython.xsl`
translates for the python datamodel (`tinyactors-test/suites/w3c`). A
`confStruct.xsl` could translate each `conf:` operation into a call of a
small library of functions the adapter registers, with every variable a
`VALUE` field, so the suite runs on this datamodel through the same
runner. How many tests that passes, and the skip list it needs (W3C 150
and 151, XML content, every test of the ECMAScript data model), is
**unverified**. The scenarios that need no strings beyond paths run on it
as they are.

## What changed from the first sketch

| The first sketch | Now |
|---|---|
| fields are doubles only, at 8 × index | the application's types, with their language's layout, described and checked |
| `int32_t f(void *fields)`, no context | `int32_t f(ta_struct_call *)` with a context: closures in any language |
| a function sees only the fields; event data reaches it through a scratch field | arguments named by the chart: `hurt(hp, _event.data.damage)` |
| `<assign location>` a bare field name; no strings, so no `eventexpr`, `targetexpr`, `<content>`, `<foreach>` | data paths everywhere; `TEXT`, `ADDRESS` and arrays; every SCXML element but a `<foreach>` that declares a variable and XML content |
| payloads of at most 8 pairs of 23-byte names | CBOR, the value table, no limit |
| an action's failure carries no data | a message, given to `error.execution` |
| no layout check | descriptors checked by core; every name and path checked at attach; every stored length checked at each read |
| one set of functions, fixed at creation | types and functions added before the image that needs them, so a new version of a chart gets its own type |
| wasm only, other bindings out of scope | one layer for C, wasm, Swift, Kotlin and Go |

## Review

An adversarial review of the rewrite (2026-11-23) changed it in these
places: a new chart version could not get a new type (types are now
matched by name and fields, and added with `ta_datamodel_struct_add`);
counts and lengths in the struct were trusted (now checked at every read,
and descriptors in 64-bit arithmetic); `VALUE` handles were in bytes a
function writes (now in the session's row); arrays of text could not be
described (a separate `count`); per-session and per-event costs left out
the system variables and `_event`; `_ioprocessors`, child content,
`sendidexpr`, `docexpr`, `TA_CAPTURE_VALUE`, `custom`, `<log>` of a
number and `null` were unspecified; a `<foreach>` body could move its
loop (it iterates a copy); payload rows need a block each; alignment
above 8 is refused; a side module's usual imports were refused; the
`size` fields of the public structs; the attach leak is in three places;
a payload from another datamodel cannot be refused by this one.

## Decisions for the maintainer

1. **Callbacks in a public header.** `tinyactors-core/AGENTS.md` allows
   "no callbacks except the datamodel vtable" (the allocator and the
   resolver are there too). Accept `ta_struct_function` as part of a
   datamodel's definitions?
2. **The name** `"struct"`, and the wasm option `structs`.
3. **The kinds.** Unsigned integers, `F32`, or bytes are left out; `VALUE`
   is in. Add or remove before the first binding uses them.
4. **Guards' purity.** By contract (as above), or by copy: a guard and a
   value function get a copy of the fields, which costs the struct's size
   per call and makes SCXML B hold by construction.
5. **Exact `<data>` to fields,** or fields the chart does not declare,
   kept by the application (an index, a cache) and invisible to paths.
   Exact is what lets an attach pick a version's type by its fields.
6. **No operators.** Every comparison is a function (`cond="weak(hp)"`,
   not `cond="hp <= 1"`). A few operators on paths and literals (`==`,
   `<`, `!`, `+`) would make small charts readable without a function
   each, at the cost of a grammar that grows.
7. **The anthill: this, or the null datamodel it measures against,** once
   this is built.

## Stages

1. **Core:** the three attach leaks fixed, the payload check between
   datamodels; the layer, with unit tests of every vtable entry and every
   refusal (a descriptor that overflows, a count past its capacity, a
   `VALUE` replaced), and `ta-conform` registering it with a C test
   library so the scenarios run on it.
2. **`@tinyactors/wasm`:** the table import, the side-module loader, the
   anthill moved, and both rows measured again.
3. **Each new binding** (Swift, Kotlin, Go): its describer and trampoline,
   the conformance run, and the cost per call measured beside quickjs.
