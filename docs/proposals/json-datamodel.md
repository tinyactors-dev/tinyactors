# The JSON datamodel: one document per session, data paths, registered functions

**Status:** Sketch (2026-11-23), validated by a spike on the branch
`ccr-6b4276e2-wq2zzh` ("Validation"). Blocked on core: payloads cross
between datamodels unchecked ("Prerequisites in core"). It replaces "A struct
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
  (settled: no operators). The datamodels of Python and Node, which run their
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
index      = position / string / path / call
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
- **No operators** (the maintainer, 2026-11-23: "just JSON and function
  calls"). `hp <= 1` is `weak(hp)`. The W3C suite needed no more than 18
  small functions for that ("Validation").
- **An index may be a call,** and `length(x)` is built in (an array's
  items, a map's pairs, a text's bytes), so a chart appends without
  passing the array through a function:
  `<assign location="items[length(items)]" expr="newItem(_event.data)"/>`
  writes at the array's length, which appends ("Writing a path"). A
  function registered as `length` for a chart replaces it (decision 8).
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
    ta_buffer *result;       /* lent: one encoded item, its length set; TA_ERROR_NO_MEMORY
                                with length the room it needs */
    ta_string_view message;  /* a failing function sets it; the datamodel copies it */
} ta_json_call;

typedef ta_status (*ta_json_function)(ta_json_call *call);
```

| Field | Meaning |
|---|---|
| `context` | what the application registered with the function: nothing for a C function, a closure's handle for a binding's trampoline |
| `arguments` | the arguments as one CBOR array, in the chart's order, read with `ta_cbor_reader` or decoded by the language's serialization |
| `result` | the datamodel's result room, lent: the function writes one encoded item into it, however its binding encodes (a `ta_cbor_writer` over the room in C and Swift, the language's own encoder in Go and Kotlin), and sets its length; the datamodel checks it ("Checking a result") |
| `message` | a failure's text, for the datamodel's `failure` entry |

- **`TA_OK` with one item in the room** is the function's value. Any other
  status, or a result that is not one value of the table, is
  `error.execution`, with `message` in the event's `{"reason", "message"}`
  data (`src/core/core.c:211-240`).
- **The function runs inside a vtable entry,** synchronously, under the
  datamodel's rules: it may not re-enter the host (`datamodel.h:7-8`).
  It extends the datamodel's callback surface; it is no new kind of
  callback (decision 5).
- **The result room** is one block of `max_document_bytes`, allocated with
  the datamodel. A result that does not fit is `TA_ERROR_NO_MEMORY`, with
  the room's `length` the size it needs, and `error.execution` with the
  reason `memory`. A call allocates nothing.
- **Checking a result:** the datamodel reads the bytes once with
  `ta_cbor_reader` (well-formed, definite lengths, UTF-8 text, no tag but
  32, no simple value but `false`, `true`, `null` and `undefined`) before
  it uses them. A binding we write produces canonical value-table CBOR, so
  the check guards against a binding's bug; whether a release build skips
  it is decision 7.

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
parameter types, calls it, and puts its encoded result in the room, the
cheapest way for its runtime. Where a call into C costs nothing (C,
Swift, Python, Node) the binding streams the value through core's
`ta_cbor_writer` over the room, as `tinyactors-core/docs/bindings.md`
(section 9) asks of every binding. Where each call into C is a crossing
(cgo about 90 ns, JNI about 150) streaming would cost a crossing per item,
so Go and Kotlin encode with the language's own library and copy the bytes
in once: a language-side codec, which section 9 forbids today and would
allow for those runtimes. With the serialization the language already
has:

| | Arguments and result | Failure |
|---|---|---|
| **Swift** | `Decodable` parameters, an `Encodable` result; a `@convention(c)` trampoline | `throws` |
| **Kotlin** | `kotlinx.serialization`, reified types, one overload per arity; a JNI trampoline | an exception |
| **Go** | a CBOR library's struct tags (`fxamacker/cbor`), one helper per arity (`Func1`, `Func2`); an `//export` trampoline | a returned `error` |
| **C** | `ta_cbor_reader` and `ta_cbor_writer` directly; no trampoline | a status and `message` |
| **wasm** | a side module's function set into the core's table and called directly, or a JavaScript function through an import | a status, or a thrown exception |

On Kotlin each call is one call into the JVM, and on Go one cgo call;
nothing else crosses, since every vtable entry runs in C. Their measured
costs are under "Validation". For wasm, a side module's function through
the table measured 3.43 ns a call on Node.js against 6.47 ns through a
JavaScript import ("History").

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

## Validation

A spike on the branch `ccr-6b4276e2-wq2zzh` (2026-11-23) built the
datamodel as `tinyactors-core/src/datamodel_json` (1,956 lines),
offered it in `ta-conform` with a C library of the conf: notation's
functions, and measured it beside quickjs. The raw numbers, the machine
and the commands are in `tinyactors-core/tools/jsonbench/RESULTS.md`.

**What it answered:**

| Question | Answer |
|---|---|
| Does it conform? | Scenarios 10 of 10 (spawn, send, time, invoke, hibernation, migration, snapshots, send errors, outcome canaries). W3C: 167 of 167 translated tests pass; 24 are skipped: 19 of the ECMAScript data model (as `skip-python.txt`), 3 that need `<script>` to change state (302, 303, 304), 530 (XML in `<assign>`) and 567 (a form field `'2'` against the number 2, no coercion). `mise run conform` runs it beside quickjs and the test datamodel, unchanged |
| Is it freestanding? | `check:freestanding` builds and links it for wasm32, x86_64, thumb and riscv32 |
| How big is it? | 31 KB of code at `-Os`, 67 KB at `-O2`; the quickjs engine is 974 KB, its wrapper 63 KB |
| Memory per actor, 10,000 of Appendix A's order | 679 B idle and 874 B after the order's three events (the datamodel's share 361 and 515 B), against quickjs's 1,127 and 2,260 B (809 and 1,771 B) |
| Time per event (min to max of three runs) | a guard alone 494 to 505 ns against 1,389 to 1,796; a guard and an assignment 1,213 to 1,227 against 2,027 to 2,339; the order's `add` and `address` 1,723 to 2,489 against 3,119 to 3,256 |
| An assignment as the document grows | 636 ns at 64 B, 716 to 1,307 at 1 KB, 1,136 to 2,008 at 16 KB, 5,106 to 5,559 at 60 KB; quickjs stays at 1,257 to 2,500 throughout |
| How many functions did the W3C suite need? | 18 (`eq` 130 uses, `add` 55, `isDefined` 21, `has` 14, `and` 12, `not` 7, `contains` 6, `concat` 5, `append` 2, `lt`, `or`, `startsWith`, ...) |

**A function call, per language** (ns per call, 1,000,000 calls, the
least of 5 runs; arguments `[[two items]]` for `hasItems` and
`[[two items], {"sku", "qty"}]` for `addItem`, which returns three items):

| | empty | `hasItems` | `addItem` | allocations per call |
|---|---|---|---|---|
| C (`ta_cbor_reader`, `ta_cbor_writer`) | 22 | 201 | 577 | 0 |
| Go (cgo, `fxamacker/cbor`) | 161 | 1,341 | 3,170 | 5 and 9 |
| Kotlin (JNI, kotlinx-serialization) | 175 | 676 | 2,072 | about 1.2 and 3 KB (a sanity run) |
| Swift (a Codable decoder over core's reader) | 34 | 2,404 | 4,892 | 8 and 14 |
| Swift (the SwiftCBOR package) | | 71,651 | 110,423 | 458 and 739 |

**What it changes:**

1. **A function should hand back bytes, not write through
   `ta_cbor_writer`.** A language encodes its result with its own library
   and C then walks those bytes and writes them again, item by item, since
   the writer takes no encoded item: 566 ns for `addItem`'s result, as
   much as the whole C function. In Go each write is also a second
   crossing into C (90 ns). The function should fill a `ta_buffer` the
   datamodel lends (the result room), as the repository's rule for bytes
   out says, and the datamodel check and canonicalize it once (decision 6).
2. **The language's decoding is the cost of a call,** not the crossing:
   Go's crossing is 90 ns and Kotlin's round trip 175, against 1.3 to 3.2
   µs to decode and encode. Kotlin on the JIT is the fastest of the three;
   Swift's Codable is the slowest, because keyed access reads each item
   three or four times and the runtime instantiates generic metadata
   (callgrind: 48 % reading, 22 % metadata). A Swift binding needs a
   streaming decoder of its own (or Codable over a reader that keeps what
   it read), and must not use SwiftCBOR.
3. **Copying the document on each assignment is cheap to 16 KB and not
   at 60 KB** (4 times quickjs). `max_document_bytes`'s default should be
   16 KB, or an assignment that changes a value of the same size should
   write in place (decision 4).
4. **kotlinx-serialization writes indefinite lengths by default,** which
   core's reader refuses: a Kotlin binding sets
   `Cbor { useDefiniteLengthEncoding = true }`. kotlinx decodes only a
   whole `ByteArray`, so the arguments are copied into one of their exact
   length on each call.
5. **Swift imports core's anonymous-enum constants as `Int` and
   `TA_CBOR_COUNT_LATER` (a `UINT32_C` macro) not at all:** a binding
   casts and redefines them.

**Second round** (the same day, on the same branch, timed one program
after another on an idle machine; `tools/jsonbench/RESULTS.md`): the
result handed back as bytes in a lent room, with and without the check,
and appending through an index that is a call.

A function call, ns, the least of 5 runs of 1,000,000 (`newItem` returns
one item from `[{"sku", "qty"}]`; "writer": through `ta_cbor_writer`,
re-emitted item by item for Go and Kotlin; "bytes": copied into the room):

| | `addItem` writer | bytes | bytes + check | `newItem` writer | bytes | bytes + check |
|---|---|---|---|---|---|---|
| C | 560 | 561 | 798 | 167 | 166 | 249 |
| Go | 2,866 | 2,168 | 2,612 | 1,178 | 790 | 895 |
| Kotlin | 2,100 | 1,399 | 1,712 | 700 | 479 | 599 |

- **Bytes save what the re-emit cost:** 700 ns on Go's `addItem`, 700 on
  Kotlin's, 300 to 400 on `newItem`; C, which writes into the room
  either way, is unchanged. A Kotlin direct `ByteBuffer` over the room
  saves a little more (1,335 and 439 ns) but kotlinx cannot encode into
  one, so the copy only moves into Kotlin.
- **The check costs** 240 ns on a three-item result and 80 on one item in
  C (about 14 ns an item read), 7 to 20 on a boolean.

Appending, ns per event, averaged while each of 64 actors' arrays grows
to N items (two runs):

| N | json, `addItem(items, …)` | json, `items[length(items)]` | quickjs, `concat` | quickjs, `items[items.length]` |
|---|---|---|---|---|
| 10 | 4,267 to 4,361 | 2,143 to 2,241 | 3,453 to 4,751 | 2,596 to 2,765 |
| 50 | 13,352 to 14,484 | 3,251 to 3,412 | 4,171 to 4,305 | 2,587 to 2,709 |
| 200 | 50,515 to 51,563 | 7,559 to 8,493 | 6,972 to 7,763 | 2,527 to 4,821 |

- **Naming only the new item** makes appending 2 to 7 times cheaper than
  passing the array through a function and back, and cheaper than quickjs
  at 10 items. Turning the check off saves 5 % of an append and up to 20 %
  of an `addItem`.
- **It still grows with the array:** 7.6 µs at 200 items against
  quickjs's 2.5 to 4.8. Each write walks the containers it changes to find
  their ends (`item_size`) and copies the document; most likely the walk
  dominates at this size (4.4 KB copied), unverified. An index of each
  container's end, or a count kept with the document, would remove it.

**What the spike simplified,** so a real layer differs: `_event` is built
at `bind_event` as one map with its data copied in (the proposal keeps a
reference); a `<foreach>` copies its array (the proposal keeps a
reference); `_ioprocessors` is kept per session (the proposal keeps it per
chart: an estimate, not measured, puts about a third of the datamodel's
361 B idle per actor in those copies); duplicate keys are not refused; a
document's depth is not checked on writes; the parser recurses (to 16
levels). None of these changes a conformance result; the first three
overstate the memory and the time per event.

## Decisions for the maintainer

1. **`<script>`:** `error.execution`, since a function cannot change the
   session; or a script as a list of assignments.
2. **No operators:** settled by the maintainer (2026-11-23).
3. **Decimal literals:** read exactly or the expression's error; or a
   correctly rounded reader in core.
4. **`max_document_bytes`'s default,** in `ta_datamodel_json_config_normalize`:
   16 KB, where the spike measured an assignment within 2 µs, unless
   assignments write in place ("Validation", item 3).
5. **The callback rule's wording.** `tinyactors-core/AGENTS.md` says "no
   callbacks except the datamodel vtable" in the public ABI; its letter is
   already broken by the allocator (`allocator.h:26-27`) and the compiler's
   resolver (`compile.h:57`), and C6 (`architecture.md:29`) still calls the
   allocator compile- and runtime-only although the host takes one.
   `ta_json_function` is called only inside a vtable entry, under the same
   rules. Accept it as part of the datamodel's callback surface, and update
   the rule and C6 to say which callbacks the public headers hold?
6. **A function's result as bytes** in a `ta_buffer` the datamodel lends,
   in place of `ta_cbor_writer`: measured on the branch, it saves 300 to
   700 ns a call in Go and Kotlin and costs C nothing ("Validation",
   second round). With it, `bindings.md` section 9 allows a language-side
   codec where a call into C is a crossing (Go, the JVM).
7. **Checking a function's result:** always (240 ns on a three-item
   result, 5 to 20 % of an event), or only in debug builds and the
   conformance suite, trusting the binding's encoder in release.
8. **`length` built in,** and an index that may be a call: a reserved
   name, or one a chart's own function may replace (as the spike does).

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
