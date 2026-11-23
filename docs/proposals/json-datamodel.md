# The JSON datamodel: one document per session, data paths, registered functions

**Status:** Sketch (2026-11-23). It replaces two earlier sketches, "A
struct datamodel" and "An application datamodel"
([application-datamodel.md](application-datamodel.md), marked dropped);
"History" says what each got wrong.
**Touches:** tinyactors-core (a new freestanding layer,
`src/datamodel_json`, and its header); every binding that offers it
(Swift, Kotlin and Go when they exist; `@tinyactors/wasm`; `ta-conform`);
tinyactors-test (a stylesheet and a function library for the W3C suite).
**Not a plan:** nothing here is decided or scheduled. ADRs and roadmaps win.

## Abstract

This document specifies a datamodel for tinyactors, named `"json"`, in
which each session's variables are one JSON value, stored as CBOR and
read and written by data paths in the style of JSONPath, and in which every computation is a function the
application registers by name and implements in its own language. The
datamodel is one freestanding C layer in core. A binding adds only the
calls between that layer and its language's functions.

## Introduction

A chart needs a language for its conditions and its values. Python and
Node have one at hand: each evaluates its own expressions at run time.
Swift, Kotlin, Go and C do not. An application in one of them writes its
charts in ECMAScript on quickjs today, and its own code runs only in
services and I/O processors, so its logic is written twice or split
across a process boundary it does not need.

Two earlier sketches tried to bring the application's types into the
datamodel. The first laid them out in core as structs; the second kept
them in each language and resolved names against them. Both made every
application describe its types a second time, by offsets or by resolvers,
and both left each binding with a datamodel of its own to write.

This datamodel keeps the data in JSON's value model, the one SCXML
already uses for data (its ECMAScript data model reads `<data>` content
as JSON, Appendix B.2), and stores it in the form core already reads,
writes, validates and keeps: the value table of
[`tinyactors-core/docs/protocol.md`](../../tinyactors-core/docs/protocol.md#values),
encoded as CBOR. CBOR is the representation, not the model: a chart
author sees JSON, and nothing in a chart depends on the encoding. A chart addresses that data by paths. Anything a path cannot
express (a comparison, a sum, a lookup in a catalog) is a function the
application registers, which takes values and returns one. The
application's types appear where they are useful, at a function's
parameters, decoded by the language's own serialization.

> The chart owns the data; the application owns the computation; JSON
> values are the only thing they share.

## Whom it serves, and who pays

- **First, the application developer in a compiled language:** Swift and
  Kotlin on iOS, macOS and Android, Go, C, and C, Rust or Zig compiled to
  wasm. Their chart logic becomes their own functions.
- **Then the binding author:** the datamodel is written once, in core.
  A binding's share is a trampoline and the decoding of arguments
  ("What a binding writes").
- **Then the maintainers:** one implementation is held to the conformance
  suite once, through `ta-conform`, and every binding inherits the result.
- **Then the operator and the capacity planner:** a session's variables are
  a document of known bytes, `actor.context` shows them as they are, and a
  hibernation blob holds them with no conversion.
- **Who pays:**
  - core: one layer (a parser of the expression grammar, path reads and
    writes over CBOR, payload rows, the vtable entries); size
    **unverified**;
  - per actor: core's record, a session row and its document (Appendix
    A's order is 74 bytes of CBOR); against a quickjs session, measured at
    891 B resident beside core's 287 B record (`tinyactors-napi/AGENTS.md`,
    "Measured cost per session"), the whole is **unverified** until built;
  - per assignment: the session's document copied once with the new value
    in place (its size, bounded by the datamodel's limit), and charged as
    fuel by the bytes copied ("Fuel");
  - per function call: its arguments written as one CBOR array, the
    function's decoding of them, its result read back;
  - per event on other datamodels: nothing.
- **Out of scope:** an expression language. Paths, literals and calls are
  the whole grammar; arithmetic and comparison belong to functions
  (decision 2). Python's and Node's datamodels, which run their language
  already.

## Conventions and terms

The key words MUST, MUST NOT, SHOULD and MAY are to be interpreted as
described in BCP 14 (RFC 2119, RFC 8174) when, and only when, they appear
in capitals.

A *session document* is the JSON object holding one session's variables,
stored as one CBOR map. A *path* names one value inside a session
document, an event, or a system variable. A *function* is a named
computation the application registers. The *datamodel* is this layer,
matched by a chart's `datamodel="json"`. Other terms (host, session, payload, hibernation, snapshot,
migration) are the [glossary's](../../GLOSSARY.md).

## Values

A value is a JSON value (RFC 8259): an object, an array, a string, a
number, `true`, `false` or `null`. It is stored as the CBOR the value
table writes for it (RFC 8949, section 6.2): an object as a map with text
keys, a number as an integer when it is one and as a float otherwise.

The value table has three values JSON lacks, and this datamodel keeps
them, because core hands them to a datamodel and a chart needs them:

| Value | Why | In a chart |
|---|---|---|
| `undefined` | a declared variable before its initialization (SCXML 5.2.2), and `actor.context`'s answer for one | the literal `undefined` |
| an actor's address (tag 32) | `_event.origin`, a `Spawner`'s answer, a target to send to | a value read from an event or returned by a function; it converts to its text wherever text is wanted |
| a byte string | data from another system that is bytes | a value read from an event or returned by a function; no literal |

Integers are exact to 64 bits, beyond JSON's usual 2^53, as the value
table keeps them.

> Every value the datamodel stores is in one canonical form: definite
> lengths, each head and float in its shortest form, keys in the order
> written, no key twice, no tag but 32, no simple value but `false`,
> `true`, `null` and `undefined`.

Bytes from outside (`from_wire`, `payload_deserialize`, a function's
result) are checked against the value table
([protocol.md](../../tinyactors-core/docs/protocol.md#values)), refused
with a key twice, and rewritten into that form when they are not in it,
since core's decoder accepts any integer width and does not look for
duplicate keys (`include/tinyactors/cbor.h:63-66`).

## The session document

A session's variables are one JSON object, stored as one CBOR map. Its
keys are the chart's `<data>` ids, in document order.

- **Every `<data>` is a key from `create` on,** holding `undefined` until
  its initialization (SCXML 5.2.2, early or late binding as the chart
  says). A `<foreach>` may add a key (below); every key of the document is
  a variable, for paths, hibernation and migration alike. A spawn's or an invoke's params that name a `<data>` id set it
  instead of the `<data>`'s own value; params naming anything else are
  ignored.
- **The document is canonical,** as every stored value is ("Values"), so
  a path write never meets a key twice.
- **A document is immutable once stored.** An assignment writes a new one
  and releases the old; a `<foreach>` keeps a reference on the document it
  iterates, which is the shallow copy SCXML 4.6 asks for at no cost.
- **Limits.** A document MUST NOT nest deeper than `TA_CBOR_DEPTH_MAX`
  (64) and MUST NOT exceed the datamodel's `max_document_bytes`. A write
  that would break either is `error.execution` with the reason `memory`
  (`TA_DATAMODEL_FAILURE_MEMORY`), and the document stays as it was. The
  same limit bounds every value the datamodel builds: a capture, a call's
  arguments, a function's result.

The system variables are not in the document. `_sessionid` and `_name`
are copied into the session's row at `create` (core's views of them live
only during that call, `include/tinyactors/datamodel.h:69-70`), and
`_ioprocessors` is kept once per chart when every session's processors
match the first's text, as the Node binding's template does
(`tinyactors-napi/AGENTS.md`, "The system variables cost a session
little until read").

## Expressions

Every expression of a chart on this datamodel is a path, a literal, a
call or `In()`. The grammar, in ABNF (RFC 5234), with whitespace allowed
between tokens:

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
number     = [ "-" ] int [ frac ] [ exp ]     ; int, frac, exp: RFC 8259, section 6
string     = DQUOTE *dchar DQUOTE / "'" *schar "'"
dchar      = unescaped-but-dquote / "'" / escape
schar      = unescaped-but-squote / DQUOTE / escape
escape     = "\" ( DQUOTE / "'" / "\" / "/" / %s"b" / %s"f" / %s"n"
           / %s"r" / %s"t" / %s"u" 4HEXDIG )
           ; unescaped-but-*: RFC 8259's unescaped, less the quote

name       = ( ALPHA / "_" ) *( ALPHA / DIGIT / "_" / "-" )
variable   = name                         ; not a reserved word
function   = name                         ; not a reserved word
```

Every quoted string in the grammar is case-sensitive (RFC 7405's `%s`).
The reserved words are `true`, `false`, `null`, `undefined`, `In` and
every name that starts with `_`; a `<data id>` or a registered function
that uses one is refused, so a token is a literal, a call or a path by
its first word alone. A map key that is not a name is reached with a
string index: `_ioprocessors["http://www.w3.org/TR/scxml/#SCXMLEventProcessor"].location`.
Nesting is bounded: calls and path indices to 16 levels, literals to 64.


The datamodel parses every expression of a chart once, at
`chart_attach`, and refuses the attach only when one does not parse or
calls a function nobody registered for the chart. SCXML lets a processor
reject a document whose expressions are syntactically ill-formed when it
loads it (5.9.4), and a call of an unknown function is the same mistake
found by the same pass. The error names the chart, the expression's line
and column, and what was wrong (`ta_datamodel_json_last_error`). A path
to a variable the chart does not declare is not refused at attach: it is
`error.execution` when it is evaluated, as W3C 286 and 311 require.

- **No operators.** `hp <= 1` is `weak(hp)`, a function (decision 2).
- **Calls nest:** `total(addItem(items, _event.data))`. Every call is
  evaluated innermost first, left to right, and an evaluation stops at the
  first failure. The evaluation's stack (a writer per level of nesting) is
  the datamodel's memory, made once, never the C stack.
- **Literals are constant.** An array or map literal holds literals only;
  a value built from paths is built by a function or by `<param>`s.
- **A decimal literal** is read exactly or refused at attach: the layer
  is freestanding and core has no correctly rounded reader (only a
  duration parser, `src/core/event_pool.c:58`), so a decimal with more
  than 15 significant digits or an exponent outside Clinger's fast path is
  an attach error (decision 3).

### Reading a path

A path's root is a variable, `_event`, or a system variable. Each segment
steps into a map by key (`.name`, `["text"]`) or into an array by
position (`[2]`). A path index steps by its value: text is a key, an
integer from 0 to 2^32 - 1 a position, anything else `error.execution`.

- **A step that finds nothing is `error.execution`:** a root the document
  lacks, a key the map lacks, a position past the array's end, a step into
  a value that is neither.
  There is no `undefined` from a missing key, so a misspelt key in event
  data fails where it is read.
- **`_event`** is a map of `name`, `type`, `sendid`, `origin`,
  `origintype`, `invokeid`, `raw` (text, absent fields as empty text) and
  `data` (the event's payload, `undefined` when it has none). Paths into
  `_event.data` read the payload's bytes in place.
- **`_ioprocessors.<type>.location`** is the session's address for that
  processor (SCXML 5.10; W3C 325, 326, 329, 346 and 569 read it).

### Writing a path

`<assign location>`, `<foreach item index>` and `idlocation` write a path.

> A write changes exactly the value its path names, or nothing.

- The root MUST be a key of the session document. A `<foreach>`'s `item`
  or `index` that is not one is added to the document as a new key, as
  SCXML 4.6 declares it (W3C 150, 151).
- Every segment but the last MUST find a value, as a read does. The last
  sets a map's key, adding it when absent, or an array's position: one
  inside the array replaces, one equal to its length appends, any other
  is `error.execution`.
- A write to a root the document lacks, to `_event` or to a system
  variable is `error.execution`, and changes nothing (W3C 286, 311, 329).

### Where each form goes

| SCXML | Takes | Gets |
|---|---|---|
| `cond` | any expression | `true` or `false`; any other value is `error.execution` (no truthiness) |
| `<data expr>`, `<assign expr>`, `<param expr>`, `<content expr>`, `<donedata>` | any expression | the value |
| `<data>`, `<assign>`, `<content>` with child text (`TA_EXPRESSION_CONTENT`) | text | the literal's value when the text is one; otherwise the text, its whitespace normalized, as a string (as SCXML's ECMAScript data model does with content that is not JSON); XML children refuse the attach |
| `eventexpr`, `targetexpr`, `typeexpr`, `srcexpr`, `delayexpr`, `sendidexpr`, `docexpr` | any expression | text; an address gives its text; any other value is `error.execution` |
| `<log expr>` | any expression | text as it is; any other value in CBOR diagnostic notation (`ta_cbor_diagnostic`, `cbor.h:89`) |
| `<foreach array>` | any expression | an array, iterated over the document, payload or result it was read from, by reference |
| `namelist` | paths to variables | a map of each variable's name to its value |
| a trace's value (`TA_CAPTURE_VALUE`) | a path | the value; a failure leaves it out of the record |
| `<script>` | refused at attach (decision 1) | |
| the vtable's `custom` | | `TA_ERROR_UNSUPPORTED`, which core makes `error.execution` |

## Functions

A function takes values and returns one value. The application registers
it under a name, for every chart or for the charts it names, before the
image whose charts call it is added, and implements it in its own
language. Every call of a chart reaches exactly one function registered
for that chart under that name, so a chart from one source cannot call a
function meant for another's.

> A function MUST NOT keep state between calls, call the host, or change
> anything but its result.

The rule makes SCXML B's "conditions have no side effects" hold for every
chart on this datamodel, since a function sees copies of values and the
only write a chart makes is an assignment of what a function returned.
The datamodel cannot check the rule; the application's functions are
trusted code, as a native chart's ECMAScript is.

### The function interface

Every function has one signature, in C:

```c
typedef struct ta_json_call {
    uint32_t size;
    uint32_t argument_count;
    void *context;           /* the function's own, as registered */
    ta_bytes arguments;      /* one CBOR array of the arguments, valid during the call */
    ta_cbor_writer *result;  /* write exactly one item */
    ta_string_view message;  /* a failing function sets it; copied by the datamodel */
} ta_json_call;

typedef ta_status (*ta_json_function)(ta_json_call *call);
```

| Field | Meaning |
|---|---|
| `context` | what the application registered with the function: nothing for a C function, the closure's handle for a binding's trampoline (a Swift box, an index into Kotlin's table, a Go `cgo.Handle`, a wasm table index) |
| `arguments` | the call's arguments as one CBOR array, in the order the chart wrote them, read with `ta_cbor_reader` or decoded by the language's serialization |
| `result` | a writer over the datamodel's scratch room; the function writes one item, which the datamodel checks against the value table |
| `message` | the text of a failure; the datamodel keeps a copy for its `failure` entry |

- **`TA_OK` with one item written** is the function's value. Any other
  status, or a result that is not exactly one item of the value table, is
  `error.execution`, with `message` as the event's `{"reason",
  "message"}` data (`src/core/core.c:211-240`).
- **The result room** is the datamodel's, grown through its allocator and
  kept between calls, so a warmed call allocates nothing. That needs a
  writer whose grown block its owner can keep, which `ta_cbor_writer` does
  not offer today (its fields are private, `cbor.h:142`): a small addition
  to `cbor.h`.

### Fuel

Core charges one unit per instruction of executable content
(`src/core/core.c:1307`) and burns what a datamodel reports
(`ta_datamodel_environment_charge`, `core.c:89-92`); it charges nothing
for a datamodel's work otherwise. So this datamodel reports its own:

- one unit per function call, and one per 64 bytes it reads, writes or
  copies (a path walked, a document rewritten, an argument array built);
- in the entries core calls with no environment (`foreach_item`,
  `assign_string`, `core.c:1450-1471`), the fuel is kept and reported at
  the next call that has one, so an `<assign>` inside a `<foreach>` pays
  for every document it copies.

A function must end: no fuel stops it once called.

### What a binding writes

A binding registers one trampoline per function, with the closure as its
`context`. The trampoline decodes `arguments` into the closure's
parameter types, calls it, and encodes what it returns into `result`.
Each language uses the serialization it already has:

| | Arguments and result | Failure |
|---|---|---|
| **Swift** | `Decodable` parameters and an `Encodable` result, one decoder over the arguments array | `throws` |
| **Kotlin** | `kotlinx.serialization`, reified parameter and result types, one overload per arity, as Go's helpers | an exception |
| **Go** | a CBOR library's struct tags (`fxamacker/cbor`), one helper per arity (`Func1`, `Func2`, ...) since Go generics have no variadic form | a returned `error` |
| **C** | `ta_cbor_reader` and `ta_cbor_writer` directly | a status and `message` |
| **wasm** | a side module's C function in the core's table, or a JavaScript function through an import | a status, or a thrown exception |

On Kotlin every call is a JNI call into the JVM, and on Go a cgo call;
their cost per call is **unverified** until each binding's first spike.

## Payloads and events

A payload is a row in the datamodel's payload table (`ta_table`): a
reference count and one CBOR block, allocated through the datamodel's
allocator. Session documents are blocks of the same kind.

- **`capture`** writes the `<param>`s or `namelist` as a map, or the
  `<content>`'s value, into a new block.
- **`to_wire`, `payload_serialize`** hand the block's bytes out as they
  are; **`from_wire`, `payload_deserialize`** check the bytes against the
  value table (well-formed, at most 64 deep) and keep them. Nothing is
  converted.
- **`bind_event`** keeps a reference on the event's payload, its type and
  core's id of its origintype (`origintype_id`, `datamodel.h:44-51`), and
  copies the text that lives only during the call (`datamodel.h:35-37`):
  its sendid, origin and `raw`, its name unless `name_id` gives the same
  text (a name the image lacks has no exact id, `event_pool.c:143-151`),
  and its invokeid unless it is the text kept for its `invoke_slot` (an id
  generated per execution differs, `datamodel.h:45-47`). The datamodel sets
  `TA_DATAMODEL_EVENT_DETACHABLE`; `detach_event` drops nothing, so
  `_event` stays bound until the next event (SCXML 5.10).
- **A `<foreach>`** is a row of the loop table: a reference on the array's
  block, the position, and the `item` and `index` paths; `foreach_end`
  removes it. A loop can stay open across a yield in a microstep
  (`core.c:121-128`, `:1440-1448`), so loops are in snapshots.

## Hibernation, migration, snapshots, inspection

- **`serialize`** writes `{"vars": <the session document>, "event": [...]
  | null}`, the form the other datamodels write; the document is copied in
  as it is.
- **`deserialize`** makes the session's document from the blob's: the
  variables the chart declares, and `undefined` for those it declares
  that the blob lacks. It keeps the blob's whole document beside it, since
  core calls `migrate` right after `deserialize` when the blob was written
  for an older version (`src/system/hibernate.c:1095`,
  `core.c:2235-2245`), as the Python binding keeps the old bindings in
  `row->extra`.
- **`migrate`** answers that kept document, unchanged, as one payload: the
  old chart's variables, which core gives `error.platform.upgrade` as its
  data after initializing the new chart's added `<data>`
  (`datamodel.h:214-221`). The kept document is dropped then, or at the
  first event when `migrate` is not called (a wake).
- **`variable`** (`actor.context`) answers a variable's value as a new
  payload; `undefined` answers no payload, as the vtable asks.
- **A snapshot** writes the tables' handle namespaces (`ta_table_snapshot`)
  and every row: sessions with their documents and `_event`, payloads as
  their bytes, loops with their position, and each session's chart by its
  attachment's segment and index (`ta_datamodel_chart.segment`), since two
  segments may run one image.

## The interface in core

A layer beside `datamodel_ecmascript`, freestanding (no C library, no
recursion: paths, nested calls and CBOR walks run on explicit stacks of
fixed depth), with the shape of quickjs's header. Every function takes at
most three parameters, and every struct the application fills starts with
its `size`.

```c
/* include/tinyactors/datamodel_json.h */
typedef struct ta_json_function_entry {
    uint32_t size;
    uint32_t pad;
    ta_string_view name;
    ta_string_view chart;          /* the chart it serves; empty: every chart */
    ta_json_function function;
    void *context;
} ta_json_function_entry;

typedef struct ta_datamodel_json_config {
    uint32_t size;
    uint32_t max_document_bytes;   /* 0: the default, set by ta_datamodel_json_config_normalize (decision 4) */
    ta_allocator allocator;        /* documents, payloads, rows, the result room */
} ta_datamodel_json_config;

TA_API ta_status ta_datamodel_json_new(const ta_datamodel_json_config *config,
                                       ta_datamodel_json **out);
TA_API ta_status ta_datamodel_json_register(ta_datamodel_json *datamodel,
                                            const ta_json_function_entry *entry);
TA_API void ta_datamodel_json_free(ta_datamodel_json *datamodel);
TA_API ta_datamodel_binding ta_datamodel_json_binding(ta_datamodel_json *datamodel);
TA_API ta_status ta_datamodel_json_last_error(const ta_datamodel_json *datamodel, ta_buffer *out);
TA_API ta_status ta_datamodel_json_snapshot(ta_datamodel_json *datamodel, ta_buffer *out);
TA_API ta_status ta_datamodel_json_restore(ta_datamodel_json *datamodel, ta_bytes state);
```

- **`ta_datamodel_json_register`** copies the names; a name registered
  twice for one chart, or a reserved word, is `TA_ERROR_INVALID`. The function and its context MUST stay valid until
  `ta_datamodel_json_free`. Registering after an image's charts attached
  affects only charts attached later.
- A host passes the binding beside its other datamodels
  (`ta_host_environment.datamodels`, at most 8, `include/tinyactors/host.h:112`).
  Nothing in `include/tinyactors/datamodel.h` changes.

## Lifetimes

| Object | Owner | Created | Ends |
|---|---|---|---|
| a function's name | the datamodel (copied) | `ta_datamodel_json_register` | `ta_datamodel_json_free` |
| a function and its context | the application | before registering | after `ta_datamodel_json_free`, which follows `ta_host_free` |
| a chart's parsed expressions | the chart's row | `chart_attach` | `chart_detach` |
| a session document | its block, by reference | `create`, an assignment, `deserialize` | the next assignment, or `destroy`, and the last `<foreach>` over it |
| `_event`: a payload reference, ids, copied text | the session's row | `bind_event` | the next `bind_event`, or `destroy` |
| an old document kept for `migrate` | the session's row | `deserialize` | `migrate`, or the first event |
| a `<foreach>` | the loop table, with a reference on its array's block | `foreach_begin` | `foreach_end` |
| a payload | the payload table | `capture`, `from_wire`, `payload_deserialize` | `release` to 0 |
| a call's `arguments` | the datamodel | before the call | the call's return |
| a call's `message` | the function | the call | the call's return (the datamodel copies it) |
| `serialize`'s and `to_wire`'s bytes | the datamodel | the call | its next call (`datamodel.h:9-13`) |

## Conformance

The W3C suite's charts use the `conf:` notation, which a stylesheet per
language translates (`tinyactors-test/suites/w3c`, `confPython.xsl`). A
`confJSON.xsl` writes variables as `<data>`, paths and literals, and each
comparison or arithmetic operation as a call of a small library (`eq`,
`lt`, `add`, `isBound`, ...) that `ta-conform` registers in C and each
binding registers in its language. Core's `mise run conform` then holds
this datamodel to the suite, beside quickjs. Which tests pass and the skip
list (XML content, every test of the ECMAScript data model) are
**unverified**.

## Open in core

- **A failed attach leaks the charts attached before it,** in three
  places: `attach_charts` (`src/system/system.c:243-259`), the trace rules
  after it (`:386-388`) and the restore path (`:1267-1268`). This
  datamodel refuses an attach for every syntax error and unknown function,
  so the leak needs fixing first, with a test of each.
- **Payloads cross between datamodels unchecked.** The event view's data
  is the event's payload whatever its datamodel
  (`src/core/event_pool.c:177`), as are invoke params
  (`src/system/route.c:296`) and done.invoke data (`route.c:831`), against
  ADR 0009's decision 7. Core should convert at the crossing (`to_wire` on
  one side, `from_wire` on the other); for a payload of this datamodel the
  first is its bytes. Until then only a host with this datamodel alone is
  safe: the host cannot keep a chart from sending to a chart on another
  datamodel, so a payload handle of one would be read as the other's.

## Security considerations

- **A chart from an untrusted source** can run on this datamodel under
  three conditions: the host runs no other datamodel until core converts
  payloads between datamodels ("Open in core"); the functions registered
  for that chart are safe for any input; and the host's fuel bounds what
  its runs cost. An expression has no loop and no recursion, every value
  it builds is bounded by `max_document_bytes`, and the datamodel charges
  fuel for the bytes it moves ("Fuel"), so a chart's evaluations cost
  what its fuel allows. It reaches nothing but its document, the event,
  the system variables and the functions registered for it.
- **Event data is untrusted input** to a function: it arrives from other
  actors and other systems. A binding's decoding refuses a value of the
  wrong shape (`error.execution`); a function validates the rest.
- **Memory:** `max_document_bytes` bounds each value this datamodel
  builds, not a session. The events queued for a session and the payloads
  of its delayed sends are bounded by the host's pools, not by this
  datamodel.

## Decisions for the maintainer

1. **`<script>`:** refused, since a function cannot change the session; or
   a script as a list of assignments.
2. **No operators.** A few on paths and literals (`==`, `!=`, `<`, `<=`,
   `!`, `+`) would spare small charts a function each, at the cost of
   semantics core must define for every pair of value types.
3. **Decimal literals:** read exactly or refused, as above; or a correctly
   rounded reader in core.
4. **`max_document_bytes`'s default,** set in one place, as every default
   is.
5. **A callback in a public header.** `tinyactors-core/AGENTS.md` allows
   "no callbacks except the datamodel vtable". Accept `ta_json_function` as
   a datamodel's?
6. **Where expressions are parsed:** at `chart_attach`, as here; or by the
   compiler, which runs hosted and could read decimals with the C
   library's correctly rounded `strtod` and report every error as a
   compile diagnostic at its line, writing the parsed form into the
   image's datamodel section (`ta_image_ahead_of_time_section`,
   `image.h:123`, which no compiler writes today). Function names would
   still be checked at attach.

## Review

An adversarial review (2026-11-23) changed: `deserialize` keeps the old
document for `migrate`, which had nothing left to answer; the attach
refuses syntax errors and unknown functions only, since W3C 286, 311 and
329 expect `error.execution` at run time for writes to undeclared
variables and system variables; fuel is charged by the bytes moved,
deferred from entries with no environment; arguments, results and
captures are bounded; functions are registered per chart; the security
claim names its conditions; `_event` copies its name and invokeid when
core's ids do not give the text, and gains `raw`; stored values are
canonical, with duplicate keys refused; nesting of path indices and
literals is bounded and the evaluation stack is the datamodel's memory;
the grammar is case-sensitive with reserved words, defined escapes and
string indices; loops are rows and in snapshots; the per-actor comparison
uses measured numbers; the Go example registers `shipsTo`; decision 6
(parsing in the compiler) is new.

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

74 bytes of CBOR. The functions, in each language (the API shapes are
sketches):

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

## Appendix B: history

- **A struct datamodel, first sketch** (2026-11-21): a layer in core for
  the wasm anthill; variables as doubles at 8 × index, functions `int32_t
  f(void *fields)` with no context and no arguments, expressions only
  names, no strings. Its spike still holds for wasm functions: a side
  module's function set into the core's table is called by
  `call_indirect` at 3.43 ns on Node.js, 2.84 on Bun and 2.88 on
  Chromium, against 6.47, 6.65 and 8.05 ns through a JavaScript import;
  a module linked with `--import-memory` alone writes into the core's
  stack, a side module does not (this file's predecessor in git, before
  `9ccdaa43`).
- **A struct datamodel, rewritten** (2026-11-23): the application's types
  described to core as layouts of offsets and kinds. Its review found what
  that costs: lengths and handles in bytes a function writes, a new chart
  version with no type of its own, and a descriptor every application
  writes for every type.
- **An application datamodel** (2026-11-23): each binding's own datamodel
  over the application's types, resolving the chart's names by
  reflection or by resolvers the application writes. Written out in four
  languages, it showed every field named three or four times, argument
  plumbing that needed a registration helper per arity, and a datamodel
  per binding.
- **This document** keeps the data in the value table, which core already
  reads and stores, lets the chart address it by paths, and confines the
  application's types to its functions' parameters, where its own
  serialization decodes them.
