# The CBOR datamodel: one document per session, data paths, registered functions

**Status:** Sketch (2026-11-23). It replaces two earlier sketches, "A
struct datamodel" and "An application datamodel"
([application-datamodel.md](application-datamodel.md), marked dropped);
"History" says what each got wrong.
**Touches:** tinyactors-core (a new freestanding layer,
`src/datamodel_cbor`, and its header); every binding that offers it
(Swift, Kotlin and Go when they exist; `@tinyactors/wasm`; `ta-conform`);
tinyactors-test (a stylesheet and a function library for the W3C suite).
**Not a plan:** nothing here is decided or scheduled. ADRs and roadmaps win.

## Abstract

This document specifies a datamodel for tinyactors in which each
session's variables are one CBOR document, read and written by data paths
in the style of JSONPath, and in which every computation is a function the
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

This datamodel keeps the data in the one form core already reads, writes,
validates and stores: the value table of
[`tinyactors-core/docs/protocol.md`](../../tinyactors-core/docs/protocol.md#values),
as CBOR. A chart addresses that data by paths. Anything a path cannot
express (a comparison, a sum, a lookup in a catalog) is a function the
application registers, which takes values and returns one. The
application's types appear where they are useful, at a function's
parameters, decoded by the language's own serialization.

> The chart owns the data; the application owns the computation; CBOR is
> the only thing they share.

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
  - per actor: a session row and its document (an order with two items is
    74 bytes of CBOR (Appendix A); quickjs's scope is about 0.5 KB,
    `tinyactors-core/docs/architecture.md:844`);
  - per assignment: the session's document copied once with the new value
    in place (its size, bounded by the datamodel's limit);
  - per function call: its arguments written as one CBOR array, the
    function's decoding of them, its result read back;
  - per event on other datamodels: nothing.
- **Out of scope:** an expression language. Paths, literals and calls are
  the whole grammar; arithmetic and comparison belong to functions
  (decision 3). Python's and Node's datamodels, which run their language
  already.

## Conventions and terms

The key words MUST, MUST NOT, SHOULD and MAY are to be interpreted as
described in BCP 14 (RFC 2119, RFC 8174) when, and only when, they appear
in capitals.

A *session document* is the CBOR map holding one session's variables. A
*path* names one value inside a session document, an event, or a system
variable. A *function* is a named computation the application registers.
The *datamodel* is this layer, registered with a host as `"cbor"`
(decision 1). Other terms (host, session, payload, hibernation, snapshot,
migration) are the [glossary's](../../GLOSSARY.md).

## The session document

A session's variables are one CBOR map. Its keys are the chart's `<data>`
ids, in document order, and its values are values of the value table:
integers, floats, text, byte strings, arrays, maps with text keys,
`false`, `true`, `null`, `undefined`, and an actor's address as tag 32.

- **Every `<data>` is a key from `create` on,** holding `undefined` until
  its initialization (SCXML 5.2.2, early or late binding as the chart
  says). A spawn's or an invoke's params that name a `<data>` id set it
  instead of the `<data>`'s own value; params naming anything else are
  ignored.
- **The document is stored as written:** definite lengths, each head in
  its shortest form, keys in the order they were added, as
  `ta_cbor_writer` writes them (`include/tinyactors/cbor.h`).
- **A document is immutable once stored.** An assignment writes a new one
  and releases the old; a `<foreach>` keeps a reference on the document it
  iterates, which is the shallow copy SCXML 4.6 asks for at no cost.
- **Limits.** A document MUST NOT nest deeper than `TA_CBOR_DEPTH_MAX`
  (64) and MUST NOT exceed the datamodel's `max_document_bytes`. A write
  that would break either is `error.execution`, and the document stays as
  it was.

The system variables are not in the document. `_sessionid` and `_name`
are copied into the session's row at `create` (core's views of them live
only during that call, `include/tinyactors/datamodel.h:69-70`), and
`_ioprocessors` is kept once per chart when every session's processors
match the first's, as the Python binding keeps it.

## Expressions

Every expression of a chart on this datamodel is a path, a literal, a
call or `In()`. The grammar, in ABNF (RFC 5234), with whitespace allowed
between tokens:

```abnf
expression = path / literal / call / in-call
call       = name "(" [ expression *( "," expression ) ] ")"
in-call    = "In(" string ")"

path       = root *segment
root       = name / "_event" / "_sessionid" / "_name" / "_ioprocessors"
segment    = "." name / "[" index "]"
index      = 1*DIGIT / string / path

literal    = number / string / "true" / "false" / "null" / "undefined"
           / array / map
array      = "[" [ literal *( "," literal ) ] "]"
map        = "{" [ string ":" literal *( "," string ":" literal ) ] "}"
number     = [ "-" ] int [ frac ] [ exp ]          ; as JSON, RFC 8259
string     = DQUOTE *dchar DQUOTE / "'" *schar "'" ; JSON's escapes in both
name       = ( ALPHA / "_" ) *( ALPHA / DIGIT / "_" / "-" )
```

The datamodel parses every expression of a chart once, at
`chart_attach`, and refuses the attach when one does not parse, names a
function nobody registered, or names a variable the chart does not
declare. SCXML lets a processor reject such a document when it loads it
(5.9.4); the error names the chart, the expression's line and column, and
what was wrong (`ta_datamodel_cbor_last_error`).

- **No operators.** `hp <= 1` is `weak(hp)`, a function (decision 3).
- **Calls nest,** to a depth of 16: `total(addItem(items, _event.data))`.
  Every call is evaluated innermost first, left to right, and an
  evaluation stops at the first failure.
- **Literals are constant.** An array or map literal holds literals only;
  a value built from paths is built by a function or by `<param>`s.
- **A decimal literal** is read exactly or refused at attach: the layer
  is freestanding and core has no correctly rounded reader (only a
  duration parser, `src/core/event_pool.c:58`), so a decimal with more
  than 15 significant digits or an exponent outside Clinger's fast path is
  an attach error (decision 4).

### Reading a path

A path's root is a declared variable, `_event`, or a system variable.
Each segment steps into a map by key (`.name`, `["text"]`) or into an
array by position (`[2]`, or a path whose value is an integer).

- **A step that finds nothing is `error.execution`:** a key the map lacks,
  a position past the array's end, a step into a value that is neither.
  There is no `undefined` from a missing key, so a misspelt key in event
  data fails where it is read.
- **`_event`** is a map of `name`, `type`, `sendid`, `origin`,
  `origintype`, `invokeid` (text, absent fields as empty text) and `data`
  (the event's payload). Paths into `_event.data` read the payload's
  bytes in place.
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
- A root of `_event` or a system variable is refused at attach when the
  path is static, and is `error.execution` otherwise.

### Where each form goes

| SCXML | Takes | Gets |
|---|---|---|
| `cond` | any expression | `true` or `false`; any other value is `error.execution` (no truthiness) |
| `<data expr>`, `<assign expr>`, `<param expr>`, `<content expr>`, `<donedata>` | any expression | the value |
| `<data>`, `<assign>`, `<content>` with child text (`TA_EXPRESSION_CONTENT`) | a literal, as text | the literal's value; XML children refuse the attach |
| `eventexpr`, `targetexpr`, `typeexpr`, `srcexpr`, `delayexpr`, `sendidexpr`, `docexpr` | any expression | text; an address gives its text; any other value is `error.execution` |
| `<log expr>` | any expression | text as it is; any other value in CBOR diagnostic notation (`ta_cbor_diagnostic`, `cbor.h:89`) |
| `<foreach array>` | a path | an array, iterated over the document or payload it was read from |
| `namelist` | paths to variables | a map of each variable's name to its value |
| a trace's value (`TA_CAPTURE_VALUE`) | a path | the value; a failure leaves it out of the record |
| `<script>` | refused at attach (decision 2) | |

## Functions

A function takes values and returns one value. The application registers
it under a name before the image whose charts call it is added, and
implements it in its own language. Every call of a chart reaches exactly
one registered function by that name.

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
typedef struct ta_cbor_call {
    uint32_t size;
    uint32_t argument_count;
    void *context;           /* the function's own, as registered */
    ta_bytes arguments;      /* one CBOR array of the arguments, valid during the call */
    ta_cbor_writer *result;  /* write exactly one item */
    ta_string_view message;  /* a failing function sets it; copied by the datamodel */
} ta_cbor_call;

typedef ta_status (*ta_cbor_function)(ta_cbor_call *call);
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
- **Fuel.** Core charges after each datamodel call already
  (`src/core/core.c:89-92`); the datamodel charges one more unit for each
  function call, and nothing for paths and literals. A function must end:
  no fuel stops it once called.
- **The result room** is the datamodel's, grown through its allocator and
  kept between calls, so a warmed call allocates nothing.

### What a binding writes

A binding registers one trampoline per function, with the closure as its
`context`. The trampoline decodes `arguments` into the closure's
parameter types, calls it, and encodes what it returns into `result`.
Each language uses the serialization it already has:

| | Arguments and result | Failure |
|---|---|---|
| **Swift** | `Decodable` parameters and an `Encodable` result, one decoder over the arguments array | `throws` |
| **Kotlin** | `kotlinx.serialization`, reified parameter and result types | an exception |
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
- **`bind_event`** keeps a reference on the event's payload, its type,
  core's ids of its name, invoke and origintype (`name_id`,
  `invoke_slot`, `origintype_id`, `datamodel.h:41-50`), and copies its
  sendid and origin, whose text lives only during the call
  (`datamodel.h:35-37`). The datamodel sets
  `TA_DATAMODEL_EVENT_DETACHABLE`; `detach_event` drops nothing, so
  `_event` stays bound until the next event (SCXML 5.10).

## Hibernation, migration, snapshots, inspection

- **`serialize`** writes `{"vars": <the session document>, "event": [...]
  | null}`, the form the other datamodels write; the document is copied in
  as it is. **`deserialize`** keeps the variables the chart declares and
  adds the ones it declares that the blob lacks, as `undefined`.
- **`migrate`** answers the old document as one payload, without the
  variables the new chart does not declare; core initializes the new
  chart's added `<data>` and raises `error.platform.upgrade` with it
  (`datamodel.h:214-221`).
- **`variable`** (`actor.context`) answers a variable's value as a new
  payload; `undefined` answers no payload, as the vtable asks.
- **A snapshot** writes the tables' handle namespaces (`ta_table_snapshot`)
  and every row, documents and payloads as their bytes.

## The interface in core

A layer beside `datamodel_ecmascript`, freestanding (no C library, no
recursion: paths, nested calls and CBOR walks run on explicit stacks of
fixed depth), with the shape of quickjs's header. Every function takes at
most three parameters, and every struct the application fills starts with
its `size`.

```c
/* include/tinyactors/datamodel_cbor.h */
typedef struct ta_cbor_function_entry {
    uint32_t size;
    uint32_t pad;
    ta_string_view name;
    ta_cbor_function function;
    void *context;
} ta_cbor_function_entry;

typedef struct ta_datamodel_cbor_config {
    uint32_t size;
    uint32_t max_document_bytes;   /* 0: the default (decision 5) */
    ta_allocator allocator;        /* documents, payloads, rows, the result room */
} ta_datamodel_cbor_config;

TA_API ta_status ta_datamodel_cbor_new(const ta_datamodel_cbor_config *config,
                                       ta_datamodel_cbor **out);
TA_API ta_status ta_datamodel_cbor_register(ta_datamodel_cbor *datamodel,
                                            const ta_cbor_function_entry *entry);
TA_API void ta_datamodel_cbor_free(ta_datamodel_cbor *datamodel);
TA_API ta_datamodel_binding ta_datamodel_cbor_binding(ta_datamodel_cbor *datamodel);
TA_API ta_status ta_datamodel_cbor_last_error(const ta_datamodel_cbor *datamodel, ta_buffer *out);
TA_API ta_status ta_datamodel_cbor_snapshot(ta_datamodel_cbor *datamodel, ta_buffer *out);
TA_API ta_status ta_datamodel_cbor_restore(ta_datamodel_cbor *datamodel, ta_bytes state);
```

- **`ta_datamodel_cbor_register`** copies the name; a name registered twice
  is `TA_ERROR_INVALID`. The function and its context MUST stay valid until
  `ta_datamodel_cbor_free`. Registering after an image's charts attached
  affects only charts attached later.
- A host passes the binding beside its other datamodels
  (`ta_host_environment.datamodels`, at most 8, `include/tinyactors/host.h:112`).
  Nothing in `include/tinyactors/datamodel.h` changes.

## Lifetimes

| Object | Owner | Created | Ends |
|---|---|---|---|
| a function's name | the datamodel (copied) | `ta_datamodel_cbor_register` | `ta_datamodel_cbor_free` |
| a function and its context | the application | before registering | after `ta_datamodel_cbor_free`, which follows `ta_host_free` |
| a chart's parsed expressions | the chart's row | `chart_attach` | `chart_detach` |
| a session document | its block, by reference | `create`, an assignment, `deserialize` | the next assignment, or `destroy`, and the last `<foreach>` over it |
| `_event`: a payload reference, ids, sendid, origin | the session's row | `bind_event` | the next `bind_event`, or `destroy` |
| a payload | the payload table | `capture`, `from_wire`, `payload_deserialize` | `release` to 0 |
| a call's `arguments` | the datamodel | before the call | the call's return |
| a call's `message` | the function | the call | the call's return (the datamodel copies it) |
| `serialize`'s and `to_wire`'s bytes | the datamodel | the call | its next call (`datamodel.h:9-13`) |

## Conformance

The W3C suite's charts use the `conf:` notation, which a stylesheet per
language translates (`tinyactors-test/suites/w3c`, `confPython.xsl`). A
`confCBOR.xsl` writes variables as `<data>`, paths and literals, and each
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
  datamodel refuses an attach for every unknown function or variable, so
  the leak needs fixing first, with a test of each.
- **Payloads cross between datamodels unchecked.** The event view's data
  is the event's payload whatever its datamodel
  (`src/core/event_pool.c:177`), as are invoke params
  (`src/system/route.c:296`) and done.invoke data (`route.c:831`), against
  ADR 0009's decision 7. Core should convert at the crossing (`to_wire` on
  one side, `from_wire` on the other); for a payload of this datamodel the
  first is its bytes. Until then a host running these charts beside
  quickjs charts MUST NOT let data cross between them.

## Security considerations

- **A chart on this datamodel is safe to run from an untrusted source**
  as far as the datamodel goes: an expression has no loop and no
  recursion, so it ends in time bounded by its length and the documents'
  limits, and it reaches nothing but the session's document, the event,
  the system variables and the registered functions. Whether a chart can
  harm the application is a question about those functions.
- **Event data is untrusted input** to a function: it arrives from other
  actors and other systems. A binding's decoding refuses a value of the
  wrong shape (`error.execution`); a function validates the rest.
- **Memory** is bounded per session by `max_document_bytes`; an
  application that spawns without bound is bounded by the host's limits,
  not by this datamodel.

## Decisions for the maintainer

1. **The name** `"cbor"`, which names the representation; or one that
   names what a chart author sees (`"data"`, `"values"`).
2. **`<script>`:** refused, since a function cannot change the session; or
   a script as a list of assignments.
3. **No operators.** A few on paths and literals (`==`, `!=`, `<`, `<=`,
   `!`, `+`) would spare small charts a function each, at the cost of
   semantics core must define for every pair of value types.
4. **Decimal literals:** read exactly or refused, as above; or a correctly
   rounded reader in core.
5. **`max_document_bytes`'s default,** set in one place, as every default
   is.
6. **A callback in a public header.** `tinyactors-core/AGENTS.md` allows
   "no callbacks except the datamodel vtable". Accept `ta_cbor_function` as
   a datamodel's?

## Appendix A: an order, end to end

The chart:

```xml
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0"
       datamodel="cbor" name="order" initial="open">
  <datamodel>
    <data id="items" expr="[]"/>
    <data id="address" expr="{}"/>
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
let functions = CBORFunctions()
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
val functions = cborFunctions {
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
static ta_status has_items(ta_cbor_call *call) {
    ta_cbor_reader reader;
    ta_cbor_item item;
    ta_cbor_reader_init(&reader, call->arguments);
    if (ta_cbor_read(&reader, &item) != 1 || item.kind != TA_CBOR_ARRAY || item.count != 1 ||
        ta_cbor_read(&reader, &item) != 1 || item.kind != TA_CBOR_ARRAY) {
        call->message = (ta_string_view){"hasItems: an array expected", 27};
        return TA_ERROR_INVALID;
    }
    ta_cbor_write_simple(call->result, item.count > 0 ? 21 : 20);  /* true, false */
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
