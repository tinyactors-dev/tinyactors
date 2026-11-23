# The JSON datamodel: one document per session, data paths, registered functions

**Status:** Sketch (2026-11-23). Blocked on core: payloads cross between
datamodels unchecked ("Prerequisites in core"). It replaces "A struct
datamodel" and "An application datamodel"
([application-datamodel.md](application-datamodel.md), dropped); "History"
says why.
**Touches:** tinyactors-core (a datamodel layer, `src/datamodel_json`,
and its header; the fix for payloads between datamodels); every binding that offers
it (Swift, Kotlin and Go when they exist; `@tinyactors/wasm`;
`ta-conform`); tinyactors-test (a stylesheet and a function library for
the W3C suite).
**Not a plan:** nothing here is decided or scheduled. ADRs and roadmaps win.

## Abstract

This document specifies a datamodel named `"json"`. Each session's
variables are one JSON value, stored as CBOR, and a chart reads and writes
them with data paths in the style of JSONPath. Every computation is a
function the application registers by name and writes in its own
language. The datamodel is one C layer in core, beside the ECMAScript
datamodel; a binding adds one trampoline that calls its language's
functions.

## Introduction

A chart needs a language for its conditions and values. Python and Node
evaluate their own expressions at run time, so their bindings implement a
datamodel in that language (`tinyactors-core/docs/bindings.md`, section 8).
Swift, Kotlin, Go and C cannot. An application in one of them writes its
charts in ECMAScript on quickjs today, and its own code runs only in
services and I/O processors.

The architecture already names the shape for those languages: JNI, .NET,
Go and Swift bindings "typically implement [the datamodel] in C … or as a
thin trampoline" (`tinyactors-core/docs/architecture.md`, section 11.5).
This datamodel is that C layer. It holds the data in JSON's value model,
which SCXML already uses for data (its ECMAScript data model reads `<data>`
content as JSON, Appendix B.2), stores it in the CBOR of core's value
table ([protocol.md](../../tinyactors-core/docs/protocol.md#values)), and
leaves the computation to the application's functions.

> The chart owns the data; the application owns the computation; JSON
> values are all they share.

## Whom it serves, and who pays

- **First, the application developer in Swift, Kotlin, Go or C,** on iOS,
  macOS, Android and servers, and in C, Rust or Zig compiled to wasm: chart
  logic becomes their own functions, called with their own types.
- **Then the binding author:** the datamodel is written once. A binding
  writes a trampoline and its argument decoding ("What a binding writes").
- **Then the maintainers:** one implementation is held to the conformance
  suite through `ta-conform`, and every binding inherits the result.
- **Then the operator and the capacity planner:** a session's variables
  are a document of known bytes, `actor.context` shows them as they are,
  and a hibernation blob holds them unconverted.
- **Who pays:**
  - core: one layer (an expression parser, path reads and writes over
    CBOR, payload and loop rows, the vtable's 29 entries); size
    **unverified**;
  - per actor: core's record, a session row and the document (Appendix A's
    order is 74 bytes of CBOR). A quickjs session measured 891 B resident,
    core's 287 B record included (`tinyactors-napi/AGENTS.md`, "Measured
    cost per session"); this datamodel's figure is **unverified**;
  - per assignment: the document copied once with the new value in place;
  - per function call: the arguments written as one CBOR array (mostly
    byte ranges copied from the document or the event), decoded by the
    function's language, and the result encoded back. Python's datamodel
    passes Python objects and pays none of this. This is the datamodel's
    main cost per event, and it is **unverified**;
  - per event on other datamodels: nothing.
- **Out of scope:** an expression language. Paths, literals, calls and
  `In()` are the whole grammar; arithmetic and comparison are functions
  (decision 2). The datamodels of Python and Node, which run their
  language already.

## Conventions and terms

The key words MUST, MUST NOT, SHOULD and MAY are to be interpreted as
described in BCP 14 (RFC 2119, RFC 8174) when, and only when, they appear
in capitals.

A *session document* is the JSON object holding one session's variables.
A *path* names one value inside a session document, the current event or
a system variable. A *function* is a computation the application registers
by name. Other terms (host, session, payload, hibernation, snapshot,
migration) are the [glossary's](../../GLOSSARY.md).

## Values

A value is a JSON value (RFC 8259): an object, an array, a string, a
number, `true`, `false` or `null`, stored as the CBOR the value table
writes for it (RFC 8949, section 6.2;
[protocol.md](../../tinyactors-core/docs/protocol.md#values), lines
90-106). A number whose value is an integer from -2^64 to 2^64 - 1 is
an integer, any other a float, as the value table writes them. The value
table also has values
JSON lacks, and this datamodel keeps them, because other datamodels and
other systems send them:

| Value | Where it comes from | In a chart |
|---|---|---|
| `undefined` | a declared variable before its initialization (SCXML 5.2.2); `actor.context` answers it | the literal `undefined` |
| an actor's address (tag 32) | event data: a `Spawner`'s answer, a value another chart sent | read from data or returned by a function; converts to its text where text is wanted |
| a byte string | event data from another system | read from data or returned by a function; no literal |
| NaN, ±Infinity, -0.0 | floats another datamodel or a function produced | read from data or returned by a function; no literal |

A number literal follows the same rule: `1.0` and `1e2` are the integers
1 and 100, `-0` is the float -0.0 (as JavaScript reads it), and `1.5` a
float.

> Every value the datamodel stores is canonical: definite lengths, each
> head and float in its shortest form, keys in the order written, no key
> twice, no tag but 32, no simple value but `false`, `true`, `null` and
> `undefined`, and at most 63 levels deep.

The depth leaves one level for the single container the datamodel wraps
around a value it hands on (a call's argument array, a capture's map), so
nothing it writes goes past `TA_CBOR_DEPTH_MAX` (64), which core's reader
refuses (`cbor.h:40-41`, `:117-120`). Blobs and snapshots carry each
document as a byte string, which adds no depth.

Bytes from outside (`from_wire`, `payload_deserialize`, a function's
result) are checked against the value table, refused with a key twice, and
rewritten into that form when they are not in it: core's decoder accepts
any integer width and does not look for duplicate keys
(`include/tinyactors/cbor.h:63-66`).

## The session document

A session's variables are one JSON object, stored as one CBOR map, whose
keys are its variables.

- **Every `<data>` is a key from `create` on,** holding `undefined` until
  initialized, early or late as the chart binds (SCXML 5.2.2), unless a
  spawn's or invoke's params set it (below); params naming anything else
  are ignored.
- **A `<foreach>` may add a key** for its `item` or `index` (SCXML 4.6).
  The chart's *declared variables* are its `<data>` ids and the roots of
  its `<foreach>` items and indexes (`TA_EXPRESSION_ITEM`,
  `image.h:111-113`), so a key a loop added survives hibernation.
- **A stored document never changes.** An assignment writes a new one and
  releases the old; a `<foreach>` holds a reference on the block it
  iterates, which is the shallow copy SCXML 4.6 asks for.
- **Limits.** A document MUST NOT exceed the datamodel's
  `max_document_bytes`, nor nest past 63 levels ("Values"). The same
  limits bound every value the datamodel builds: a capture, a call's
  arguments, a function's result. A write that would break one is
  `error.execution`, with the reason `memory`
  (`TA_DATAMODEL_FAILURE_MEMORY`) for size and `exception` for depth, and
  nothing changes.
- **`create`** takes the spawn's or invoke's params (`init`, one
  reference, which it releases: core releases it only when no `create`
  runs, `core.c:2129-2137`). A map sets the declared variables it names
  and marks them, and `init_data` then skips their `<data>` expressions,
  early or late binding alike; params that are not a map set nothing.

The system variables are not in the document. `_sessionid` and `_name`
are copied into the session's row at `create`, since core's views of them
live only during that call (`include/tinyactors/datamodel.h:69-70`).
`_ioprocessors` is kept once per chart when every session's processors
match the first's text, as the Node binding's template does.

## Expressions

The compiler gives each expression an id and keeps its kind, source text
and position in the image's expression table; what an id means is the
datamodel's to decide (`architecture.md`, constraint C8 and section 9.2;
`ta_image_expression`, `include/tinyactors/image.h:95-121`). This
datamodel parses every expression of a chart once, at `chart_attach`,
shared by the chart's sessions, and resolves each call to a function
registered for the chart.

> An ill-formed expression MUST NOT refuse the chart. Its evaluation is
> `error.execution`, with the parse error as the message.

The W3C suite requires it: tests 156, 312 and 487 load a chart with an
illegal expression and expect `error.execution` when it is evaluated. The
same holds for a call of an unregistered function and a decimal that
cannot be read. The attach fails only for want of memory. A developer who
wants the errors before anything runs calls `ta_datamodel_json_check` on
a compiled image ("The interface in core").

### Grammar

In ABNF (RFC 5234, with RFC 7405's case-sensitive `%s`), whitespace
allowed between tokens:

```abnf
expression = path / literal / call / in-call
call       = function "(" [ expression *( "," expression ) ] ")"
in-call    = %s"In" "(" string ")"

path       = root *segment
root       = variable / %s"_event" / %s"_sessionid" / %s"_name"
           / %s"_ioprocessors"
segment    = "." name / "[" index "]"
index      = position / string / path
position   = "0" / %x31-39 *DIGIT

literal    = number / string / %s"true" / %s"false" / %s"null"
           / %s"undefined" / array / map
array      = "[" [ literal *( "," literal ) ] "]"
map        = "{" [ string ":" literal *( "," string ":" literal ) ] "}"
number     = [ "-" ] int [ frac ] [ exp ]   ; int, frac, exp: RFC 8259, section 6
string     = DQUOTE *dchar DQUOTE / "'" *schar "'"
dchar      = unescaped-but-dquote / "'" / escape
schar      = unescaped-but-squote / DQUOTE / escape
           ; both quotes, the backslash and controls are escaped
escape     = "\\" ( DQUOTE / "'" / "\\" / "/" / %s"b" / %s"f" / %s"n"
           / %s"r" / %s"t" / %s"u" 4HEXDIG )
unescaped-but-dquote = %x20-21 / %x23-26 / %x28-5B / %x5D-10FFFF
unescaped-but-squote = unescaped-but-dquote

name       = ( ALPHA / "_" ) *( ALPHA / DIGIT / "_" / "-" )
variable   = name                       ; not a reserved word
function   = name                       ; not a reserved word
```

- **Tokens** are names, numbers, strings and the punctuation of the
  grammar; whitespace may separate tokens and never splits one (`- 1` is
  no number). A name followed by `(` is a call, any other name a path.
- **Reserved words:** `true`, `false`, `null`, `undefined`, `In`, and every
  name that starts with `_` (SCXML reserves those for the platform, 5.10).
  A function cannot be registered under one. A `<data id>` that is one, or
  that `name` cannot spell (an XML id may hold `.` and letters outside
  ASCII), is a key like any other and is initialized, but no path names
  it; `ta_datamodel_json_check` reports it.
- **What else is the expression's error,** found at attach: a `\u` escape
  that leaves a lone surrogate, a key twice in a map literal, a position
  past 2^32 - 1, a LOCATION expression that is not a path, and
  `In('...')` of a state the chart lacks.
- **A key that is not a name** is reached by a string index:
  `_ioprocessors["http://www.w3.org/TR/scxml/#SCXMLEventProcessor"].location`.
- **No operators.** `hp <= 1` is `weak(hp)` (decision 2).
- **Calls nest** (`total(addItem(items, _event.data))`), evaluated
  innermost first, left to right, stopping at the first failure.
- **Nesting is bounded:** calls and path indices to 16 levels, literals to
  64. The evaluation's stack is the datamodel's memory, made once, never
  the C stack.
- **Literals are constant:** an array or map literal holds literals only.
- **A decimal literal** is read exactly or is its expression's error: core
  has no correctly rounded reader (only a duration parser,
  `src/core/event_pool.c:58`), so a decimal outside Clinger's fast path
  (more than 15 significant digits, or a large exponent) cannot be read
  (decision 3).

### Reading a path

A path's root is a variable, `_event` or a system variable. A segment
steps into a map by key (`.name`, `["text"]`) or into an array by position
(`[2]`). A path index steps by its value: text is a key, an integer from 0
to 2^32 - 1 a position, anything else `error.execution`.

- **A step that finds nothing is `error.execution`:** a root the document
  lacks, a key the map lacks, a position past the end, a step into a value
  that is neither. A missing key never reads as `undefined`, so a misspelt
  key in event data fails where it is read.
- **`_event`** is a map of `name`, `type`, `sendid`, `origin`,
  `origintype`, `invokeid`, `raw`, all text as core gives them (an absent
  field is empty text; `origin` too is text, `datamodel.h:55`), and
  `data` (the event's payload, `undefined` when it has none). Paths into
  `_event.data` read the payload's bytes in place. Before the first event
  `_event` is unbound (`architecture.md`, 9.4), and a path into it is
  `error.execution`.
- **`_ioprocessors[type].location`** is the session's address for that
  processor (SCXML 5.10; W3C 325, 326, 329, 346 and 569).

### Writing a path

`<assign location>`, `<foreach item index>` and `idlocation` write a path.

> A write changes exactly the value its path names, or nothing.

- A write to a root the document lacks, to `_event` or to a system
  variable is `error.execution` (W3C 286, 311, 329). A `<foreach>`'s `item`
  and `index` are the exception above: they add their key.
- Every segment but the last MUST find a value, as a read does. The last
  sets a map's key, adding it when absent, or an array's position: inside
  the array it replaces, at its length it appends, anywhere else it is
  `error.execution`.

### Where each form goes

| SCXML | Gets |
|---|---|
| `cond` | `true` or `false`; any other value is `error.execution` (no truthiness) |
| `<data expr>`, `<assign expr>`, `<param expr>`, `<content expr>`, `<donedata>` | the value |
| child text of `<data>`, `<assign>`, `<content>` (`TA_EXPRESSION_CONTENT`) | the literal's value when the text is one; otherwise the text, whitespace normalized, as a string, as SCXML's ECMAScript data model treats content that is not JSON; XML children are the expression's error |
| `eventexpr`, `targetexpr`, `typeexpr`, `srcexpr`, `delayexpr`, `sendidexpr`, `docexpr` | text; an address gives its text; any other value is `error.execution` |
| `<log expr>` | text as it is; any other value in CBOR diagnostic notation (`ta_cbor_diagnostic`, `cbor.h:89`) |
| `<foreach array>` | an array, iterated by reference over the block it came from; a function's result, which has no block, is copied into one at `foreach_begin` |
| `namelist` | a map of each variable's name to its value |
| a trace's value (`TA_CAPTURE_VALUE`) | the value; a failure leaves it out of the record |
| `<script>`, and a chart-level `<script>` (`TA_EXPRESSION_GLOBAL_SCRIPT`) at the start | `error.execution` (decision 1) |
| the vtable's `custom` | `TA_ERROR_UNSUPPORTED`, which core makes `error.execution` |

## Functions

A function takes values and returns one value. The application registers
it under a name, for every chart or for one chart by name, before adding
the image whose charts call it, and writes it in its own language. A
chart's call reaches the function registered for that chart under that
name, and an entry for every chart only when the chart has none of its
own, so a chart cannot call a function meant for another. Every version
of a chart shares its name and so its functions: a new version that needs
a function with other parameters calls it by a new name.

> A function MUST NOT keep state between calls, call the host, or change
> anything but its result.

A function sees copies of values, and a chart's only write is an
assignment of what a function returned, so SCXML B's "conditions have no
side effects" holds on this datamodel. The datamodel cannot check the
rule: functions are the application's trusted code, as a native chart's
ECMAScript is.

### The function interface

Every function has one signature, in C:

```c
typedef struct ta_json_call {
    uint32_t size;
    uint32_t argument_count;
    void *context;           /* the function's own, as registered */
    ta_bytes arguments;      /* one CBOR array of the arguments, valid during the call */
    ta_cbor_writer *result;  /* write exactly one item */
    ta_string_view message;  /* a failing function sets it; the datamodel copies it */
} ta_json_call;

typedef ta_status (*ta_json_function)(ta_json_call *call);
```

| Field | Meaning |
|---|---|
| `context` | what the application registered with the function: nothing for a C function, a closure's handle for a binding's trampoline |
| `arguments` | the arguments as one CBOR array, in the chart's order, read with `ta_cbor_reader` or decoded by the language's serialization |
| `result` | a writer over the datamodel's result room; the function writes one item, which the datamodel checks and makes canonical |
| `message` | a failure's text, for the datamodel's `failure` entry |

- **`TA_OK` with one item written** is the function's value. Any other
  status, or a result that is not one value of the table, is
  `error.execution`, with `message` in the event's `{"reason", "message"}`
  data (`src/core/core.c:211-240`).
- **The function runs inside a vtable entry,** synchronously, under the
  datamodel's rules: it may not re-enter the host (`datamodel.h:7-8`).
  It extends the datamodel's callback surface; it is no new kind of
  callback (decision 5).
- **The result room** is one block of `max_document_bytes`, allocated with
  the datamodel, which the writer fills without growing (a writer with no
  allocator, `cbor.h:126-130`, `:159-162`): a result that does not fit is
  `TA_ERROR_NO_MEMORY` there and `error.execution` with the reason
  `memory`. A call allocates nothing.

### Fuel

Core burns fuel for its own work (one unit per instruction of executable
content, `src/core/core.c:1307`, and its steps) and what a datamodel
reports (`ta_datamodel_environment_charge`, `core.c:89-92`); it does not
measure a datamodel's work. This datamodel reports one unit per function
call and one per 64 bytes it reads, writes or copies.

- Core calls `foreach_item` (`core.c:1450-1471`) and `assign_string`
  (`core.c:1055-1059`, `:1555-1559`) with no environment. Their fuel is
  kept in the session's row, so it is in snapshots and never billed to
  another session, and reported at the session's next evaluation, so an
  `<assign>` inside a `<foreach>` pays for each document it copies.
- It is never reported through `bind_event`, whose fuel core ignores
  (`datamodel.h:38-40`), `migrate`, which runs with an environment of its
  own (`core.c:2238`), or `init_data` during an adoption, which runs
  without a limit (`core.c:2228-2232`).

A function must end: no fuel stops it once called.

### What a binding writes

A binding registers one trampoline with each function, the closure as its
`context`. The trampoline decodes `arguments` into the closure's
parameter types, calls it, and encodes its result into `result`, with the
serialization the language already has:

| | Arguments and result | Failure |
|---|---|---|
| **Swift** | `Decodable` parameters, an `Encodable` result; a `@convention(c)` trampoline | `throws` |
| **Kotlin** | `kotlinx.serialization`, reified types, one overload per arity; a JNI trampoline | an exception |
| **Go** | a CBOR library's struct tags (`fxamacker/cbor`), one helper per arity (`Func1`, `Func2`); an `//export` trampoline | a returned `error` |
| **C** | `ta_cbor_reader` and `ta_cbor_writer` directly; no trampoline | a status and `message` |
| **wasm** | a side module's function set into the core's table and called directly, or a JavaScript function through an import | a status, or a thrown exception |

On Kotlin each call is one call into the JVM, and on Go one cgo call;
nothing else crosses, since every vtable entry runs in C. Their cost is
**unverified** until each binding's first spike. For wasm, a side
module's function through the table measured 3.43 ns a call on Node.js
against 6.47 ns through a JavaScript import ("History").

## Payloads, events and loops

A payload is a row in the datamodel's payload table (`ta_table`): a
reference count and one canonical CBOR block, allocated through the
datamodel's allocator. Session documents are blocks of the same kind.

- **`capture`** writes the `<param>`s or `namelist` as a map, or the
  `<content>`'s value, into a new block. A name given twice keeps its last
  value, one key (W3C 178, a manual test, expects both values; a JSON
  object cannot hold them).
- **`to_wire` and `payload_serialize`** hand a block's bytes out as they
  are; **`from_wire` and `payload_deserialize`** check and canonicalize
  ("Values").
- **`bind_event`** keeps a reference on the event's payload, its type and
  core's id of its origintype (`origintype_id`, `datamodel.h:44-51`), and
  copies the text that lives only during the call (`datamodel.h:35-37`):
  its sendid, origin and `raw`; its name, unless `name_id` gives the same
  text (a name the image lacks has no exact id, `event_pool.c:143-151`);
  its invokeid, unless it is the text kept for its `invoke_slot` (an id
  generated per execution differs, `datamodel.h:45-47`). It builds the
  new `_event` before it lets the old go; when memory fails, which core
  does not hear (`core.c:530` ignores the status), `_event` is unbound
  and a read of it is `error.execution` with the reason `memory`. The
  datamodel sets `TA_DATAMODEL_EVENT_DETACHABLE`; `detach_event` drops
  nothing, so `_event` stays bound until the next event (SCXML 5.10).
- **A `<foreach>`** is a row of the loop table: a reference on the array's
  block, the position, and the `item` and `index` paths. A run can yield
  at any instruction (`core.c:1306`) while a loop's frame stays in the
  workspace (`:1442-1449`), and a snapshot copies the whole region
  (`include/tinyactors/host.h:449`), so loops are in snapshots.

## Hibernation, migration, snapshots, inspection

- **`serialize`** writes `{"vars": <the document>, "event": [...] |
  null}`, the form the other datamodels write, with the document as a
  byte string of its bytes, so the blob adds no depth.
- **`deserialize`** makes the session's document from the blob's: the
  chart's declared variables ("The session document"), `undefined` for
  those the blob lacks. It
  keeps the blob's whole document beside it, because core calls `migrate`
  right after `deserialize` when the blob was written for an older version
  (`src/system/hibernate.c:1095`, `core.c:2235-2245`).
- **`migrate`** answers that kept document, unchanged, as one payload: the
  old chart's variables, which core gives `error.platform.upgrade` as data
  after initializing the new chart's added `<data>`
  (`datamodel.h:214-221`). The kept document goes then, or at the first
  event when no migration asks for it.
- **`variable`** (`actor.context`) answers a variable's value as a new
  payload; `undefined` answers none, and a name the document lacks
  `TA_ERROR_INVALID`, as the vtable asks (`datamodel.h:250-257`).
- **A snapshot** writes the tables' handle namespaces (`ta_table_snapshot`)
  and every row: sessions with their documents (as byte strings), pending
  fuel and `_event`, payloads,
  loops, and each session's chart by its attachment's segment and index
  (`ta_datamodel_chart.segment`), since two segments may run one image.

## The interface in core

A layer beside `datamodel_ecmascript`, freestanding (no C library; paths,
calls and CBOR walks run on explicit stacks of fixed depth). Every
function takes at most three parameters, and every struct the application
fills starts with its `size`.

```c
/* include/tinyactors/datamodel_json.h */
typedef struct ta_datamodel_json ta_datamodel_json;

typedef struct ta_json_function_entry {
    uint32_t size;
    uint32_t pad;
    ta_string_view name;
    ta_string_view chart;            /* the chart it serves; empty: every chart */
    ta_json_function function;
    void *context;
} ta_json_function_entry;

typedef struct ta_datamodel_json_config {
    uint32_t size;
    uint32_t max_document_bytes;     /* 0: the default (decision 4) */
    ta_allocator allocator;          /* blocks, rows, the result room */
} ta_datamodel_json_config;

/* The config with its defaults filled in: the one place a default lives. */
TA_API void ta_datamodel_json_config_normalize(ta_datamodel_json_config *config);
TA_API ta_status ta_datamodel_json_new(const ta_datamodel_json_config *config,
                                       ta_datamodel_json **out);
TA_API ta_status ta_datamodel_json_register(ta_datamodel_json *datamodel,
                                            const ta_json_function_entry *entry);
TA_API ta_datamodel_binding ta_datamodel_json_binding(ta_datamodel_json *datamodel);
/* Every expression of the image's charts on this datamodel, parsed and
 * resolved as at attach; each error as a CBOR array of
 * {chart, line, column, message} in `out`. */
TA_API ta_status ta_datamodel_json_check(ta_datamodel_json *datamodel, const ta_image *image,
                                         ta_buffer *out);
TA_API ta_status ta_datamodel_json_snapshot(ta_datamodel_json *datamodel, ta_buffer *out);
TA_API ta_status ta_datamodel_json_restore(ta_datamodel_json *datamodel, ta_bytes state);
TA_API void ta_datamodel_json_free(ta_datamodel_json *datamodel);
```

- **`ta_datamodel_json_register`** copies the names. A reserved word, or a
  name registered twice for one chart, is `TA_ERROR_INVALID`. The function
  and its context MUST stay valid until `ta_datamodel_json_free`, which
  follows `ta_host_free`. A function registered after a chart attached is
  seen by charts attached later.
- **The layer is freestanding** and checked as one: an entry in
  `tinyactors-core/mise/core.rb` with `freestanding: true`, so
  `check:freestanding` covers it. Its default lives in
  `ta_datamodel_json_config_normalize`, which the rule "a default lives in
  one place" (`tinyactors-core/AGENTS.md`) then lists beside the other two.
- **A host takes the binding** beside its other datamodels
  (`ta_host_environment.datamodels`, at most 8, `include/tinyactors/host.h:112`).
  Nothing in `datamodel.h` changes.

## Lifetimes

| Object | Owner | Created | Ends |
|---|---|---|---|
| a function's names | the datamodel (copied) | `ta_datamodel_json_register` | `ta_datamodel_json_free` |
| a function and its context | the application | before registering | after `ta_datamodel_json_free` |
| a chart's parsed expressions | the chart's row | `chart_attach` | `chart_detach` |
| a session document | its block, by reference | `create`, an assignment, `deserialize` | the next assignment or `destroy`, and the last loop over it |
| an old document kept for `migrate` | the session's row | `deserialize` | `migrate`, or the first event |
| `_event`: a payload reference, ids, copied text | the session's row | `bind_event` | the next `bind_event`, or `destroy` |
| a payload | the payload table | `capture`, `from_wire`, `payload_deserialize` | `release` to 0 |
| a loop | the loop table | `foreach_begin` | `foreach_end` |
| the result room | the datamodel | `ta_datamodel_json_new` | `ta_datamodel_json_free` |
| a call's `arguments` | the datamodel | before the call | its return |
| a call's `message` | the function | the call | its return (copied) |
| `serialize`'s and `to_wire`'s bytes | the datamodel | the call | its next call (`datamodel.h:9-13`) |

## Prerequisites in core

- **Payloads between datamodels. This blocks the datamodel.** Constraint C5
  promises that a system mixing datamodels without a codec is refused at
  link time (`architecture.md:28`), and ADR 0009 (decision 7) that "a
  payload is handed only to the datamodel whose index it carries". Neither
  holds: the event view's data is the event's payload whatever its
  datamodel (`src/core/event_pool.c:177`), as are an invoke's params
  (`src/system/route.c:296`) and done.invoke's data (`route.c:831`), and
  no check exists in `route.c` or `system.c`. Every binding registers
  quickjs beside its own datamodel (`bindings.md`, section 8), so a host
  running `"json"` charts would hand one datamodel's handle to the other.
  Core must convert at the crossing (`to_wire` on one side, `from_wire` on
  the other), or refuse the mix as C5 says, before this datamodel ships.
  The fix changes what the bindings rely on, so it updates
  `tinyactors-core/docs/bindings.md` (section 8, line 287: "a payload goes
  only to the datamodel it came from", stated as fact) and
  `docs/embedding.md` in the same commit.
- **Not blocking:** a failed attach leaks the charts attached before it,
  in `attach_charts` (`src/system/system.c:243-259`), after the trace
  rules (`:386-388`) and on restore (`:1267-1268`). This datamodel fails an
  attach only for want of memory, as the others do.

## Conformance

The W3C suite's charts use the `conf:` notation, which a stylesheet per
language translates (`tinyactors-test/suites/w3c`, `confPython.xsl`). A
`confJSON.xsl` writes variables as `<data>`, paths and literals, and each
comparison or arithmetic operation as a call of a small library (`eq`,
`lt`, `add`, `isBound`, ...) that `ta-conform` registers in C and each
binding registers in its language. Core's `mise run conform` then holds
this datamodel to the suite beside quickjs. Which tests pass, and the
skip list (XML content, the ECMAScript data model's tests), are
**unverified**.

## Security considerations

- **An untrusted chart** can run on this datamodel when three conditions
  hold: the host runs no other datamodel until core converts payloads
  between datamodels; the functions registered for that chart are safe for
  any input; and the host's fuel bounds what its runs cost. An expression
  has no loop and no recursion, every value it builds is bounded by
  `max_document_bytes`, and the datamodel charges fuel for the bytes it
  moves. A chart reaches its document, the event, the system variables and
  the functions registered for it, nothing else.
- **Event data is untrusted input** to a function: it comes from other
  actors and other systems. A binding's decoding refuses a value of the
  wrong shape (`error.execution`); the function validates the rest.
- **Memory:** `max_document_bytes` bounds each value the datamodel builds.
  The events queued for a session and its delayed sends' payloads are
  bounded by the host's pools.

## Decisions for the maintainer

1. **`<script>`:** `error.execution`, since a function cannot change the
   session; or a script as a list of assignments.
2. **No operators.** A few on paths and literals (`==`, `!=`, `<`, `<=`,
   `!`, `+`) would spare small charts a function each, at the cost of
   semantics core must define for every pair of value types.
3. **Decimal literals:** read exactly or the expression's error; or a
   correctly rounded reader in core.
4. **`max_document_bytes`'s default,** in `ta_datamodel_json_config_normalize`.
5. **The callback rule's wording.** `tinyactors-core/AGENTS.md` says "no
   callbacks except the datamodel vtable" in the public ABI; its letter is
   already broken by the allocator (`allocator.h:26-27`) and the compiler's
   resolver (`compile.h:57`), and C6 (`architecture.md:29`) still calls the
   allocator compile- and runtime-only although the host takes one.
   `ta_json_function` is called only inside a vtable entry, under the same
   rules. Accept it as part of the datamodel's callback surface, and update
   the rule and C6 to say which callbacks the public headers hold?

## Appendix A: an order, end to end

The chart:

```xml
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0"
       datamodel="json" name="order" initial="open">
  <datamodel>
    <data id="items" expr="[]"/>
    <data id="address" expr="{&quot;country&quot;: &quot;&quot;}"/>
  </datamodel>
  <state id="open">
    <transition event="add" cond="knownItem(_event.data)">
      <assign location="items" expr="addItem(items, _event.data)"/>
    </transition>
    <transition event="address">
      <assign location="address.country" expr="_event.data.country"/>
    </transition>
    <transition event="checkout" cond="hasItems(items)" target="checking"/>
  </state>
  <state id="checking">
    <transition cond="shipsTo(address.country)" target="charging"/>
    <transition target="open"/>
  </state>
  <state id="charging">
    <onentry>
      <send type="billing" event="charge">
        <param name="cents" expr="total(items)"/>
        <param name="country" expr="address.country"/>
      </send>
    </onentry>
    <transition event="charged" target="paid"/>
    <transition event="error.communication" target="open"/>
  </state>
  <final id="paid"/>
</scxml>
```

After two `add` events and an `address`, the session document is

```
{"items": [{"sku": "tea", "qty": 2, "cents": 450},
           {"sku": "mug", "qty": 1, "cents": 1200}],
 "address": {"country": "DE"}}
```

74 bytes of CBOR. The functions (the API shapes are sketches):

```swift
struct Item: Codable { var sku: String; var qty: Int; var cents: Int }
struct Add: Codable { var sku: String; var qty: Int }

let catalog = ["tea": 450, "mug": 1200]
let functions = JSONFunctions()
functions.register("knownItem") { (add: Add) in catalog[add.sku] != nil && (1...99).contains(add.qty) }
functions.register("addItem") { (items: [Item], add: Add) throws -> [Item] in
    guard let cents = catalog[add.sku] else { throw ChartError("unknown item \(add.sku)") }
    return items + [Item(sku: add.sku, qty: add.qty, cents: cents)]
}
functions.register("hasItems") { (items: [Item]) in !items.isEmpty }
functions.register("shipsTo") { (country: String) in ["DE", "FR", "NL"].contains(country) }
functions.register("total") { (items: [Item]) in items.reduce(0) { $0 + $1.cents * $1.qty } }
```

```kotlin
@Serializable data class Item(val sku: String, val qty: Int, val cents: Long)
@Serializable data class Add(val sku: String, val qty: Int)

val catalog = mapOf("tea" to 450L, "mug" to 1200L)
val functions = jsonFunctions {
    function("knownItem") { add: Add -> add.sku in catalog && add.qty in 1..99 }
    function("addItem") { items: List<Item>, add: Add ->
        val cents = catalog[add.sku] ?: throw ChartException("unknown item ${add.sku}")
        items + Item(add.sku, add.qty, cents)
    }
    function("hasItems") { items: List<Item> -> items.isNotEmpty() }
    function("shipsTo") { country: String -> country in setOf("DE", "FR", "NL") }
    function("total") { items: List<Item> -> items.sumOf { it.cents * it.qty } }
}
```

```go
type Item struct {
	SKU   string `cbor:"sku"`
	Qty   int    `cbor:"qty"`
	Cents int64  `cbor:"cents"`
}
type Add struct {
	SKU string `cbor:"sku"`
	Qty int    `cbor:"qty"`
}

var catalog = map[string]int64{"tea": 450, "mug": 1200}

func register(fns *ta.Functions) {
	ta.Func1(fns, "knownItem", func(add Add) (bool, error) {
		_, ok := catalog[add.SKU]
		return ok && add.Qty >= 1 && add.Qty <= 99, nil
	})
	ta.Func2(fns, "addItem", func(items []Item, add Add) ([]Item, error) {
		cents, ok := catalog[add.SKU]
		if !ok {
			return nil, fmt.Errorf("unknown item %q", add.SKU)
		}
		return append(items, Item{SKU: add.SKU, Qty: add.Qty, Cents: cents}), nil
	})
	ta.Func1(fns, "hasItems", func(items []Item) (bool, error) { return len(items) > 0, nil })
	ta.Func1(fns, "shipsTo", func(country string) (bool, error) {
		return country == "DE" || country == "FR" || country == "NL", nil
	})
	ta.Func1(fns, "total", func(items []Item) (int64, error) {
		var sum int64
		for _, it := range items {
			sum += it.Cents * int64(it.Qty)
		}
		return sum, nil
	})
}
```

In C, a function reads its arguments with `ta_cbor_reader` and writes its
result with `ta_cbor_writer`:

```c
/* hasItems(items): true when the array has an item */
static ta_status has_items(ta_json_call *call) {
    ta_cbor_reader reader;
    ta_cbor_item item;
    ta_cbor_reader_init(&reader, call->arguments);
    if (ta_cbor_read(&reader, &item) != 1 || item.kind != TA_CBOR_ARRAY || item.count != 1 ||
        ta_cbor_read(&reader, &item) != 1 || item.kind != TA_CBOR_ARRAY) {
        call->message = (ta_string_view){"hasItems: an array expected", 27};
        return TA_ERROR_INVALID;
    }
    ta_cbor_write_simple(call->result, item.count > 0 ? TA_CBOR_TRUE : TA_CBOR_FALSE);
    return TA_OK;
}
```

## History

- **A struct datamodel** (2026-11-21): a layer in core for the wasm
  anthill, with variables as doubles at fixed offsets and functions
  `int32_t f(void *fields)` with no context, no arguments and no strings.
  Its spike holds for wasm: a side module's function set into the core's
  table is called by `call_indirect` at 3.43 ns on Node.js, 2.84 on Bun and
  2.88 on Chromium, against 6.47, 6.65 and 8.05 ns through a JavaScript
  import; a module linked with `--import-memory` alone writes into the
  core's stack, a side module does not (`git show 9ccdaa43^:docs/proposals/struct-datamodel.md`).
  Rewritten the same day to describe the application's types to core as
  layouts, it had every application write a descriptor per type, and its
  review found lengths and handles in bytes a function writes.
- **An application datamodel** (2026-11-23): a datamodel in each binding
  over the application's types, resolving the chart's names by reflection
  or by resolvers the application writes. In four languages it named every
  field three or four times and gave each binding a datamodel of its own.
  A later suggestion to keep this layer as a library and have each binding
  implement the vtable was dropped for the same reason: the vtable answers
  synchronously (`core.c:446-461`), so each binding would still call the
  functions inside its own entries, and would write the 29 entries again.
- **This document** keeps the data in the value table, which core already
  reads and stores, and confines the application's types to its functions'
  parameters. Its reviews (2026-11-23) found the migration path, the
  W3C tests that need ill-formed expressions to load, fuel for the
  datamodel's own work, bounds on what it builds, function scope per
  chart, canonical values, a grammar without ambiguity, `_event`'s text,
  loops in snapshots, and the payload problem in core. A review of the
  rewrite found the depth a blob adds, the variables a `<foreach>`
  declares, the value table's floats and integer range, `_event.origin`'s
  type, where pending fuel lives, a result room that needs no new `cbor.h`
  call, `create`'s params, and the grammar's remaining cases; all are
  reflected above.
