# An application datamodel: charts over the application's own types and functions

**Status:** Dropped (2026-11-23): replaced by [the JSON datamodel](json-datamodel.md), which keeps the data in the value table and the application's types at its functions' parameters; a datamodel per binding over the application's types needed every field named again, by reflection or by hand. It replaced "A struct datamodel" (this file's history).
**Touches:** each binding that offers it (Swift, Kotlin and Go when they
exist; `@tinyactors/wasm` for side modules written in C); tinyactors-core
only for a parser of the expression grammar, if the bindings share one;
tinyactors-test (a stylesheet and a function library for the W3C suite).
**Not a plan:** nothing here is decided or scheduled. ADRs and roadmaps win.

## Goal

The application has types and functions. A chart should use them, by name,
and nothing else:

```xml
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0"
       datamodel="application" name="ant" initial="worker">
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

```swift
struct Ant: Codable { var colony = 0; var hp = 2; var queen = "" }

let host = Host(charts, system: "anthill", datamodel: .application { app in
    app.state("ant", Ant.self)
    app.guard("weak") { (hp: Int) in hp <= 1 }
    app.value("hurt") { (hp: Int, damage: Int) in hp - damage }
})
```

The same chart runs in a Kotlin app that registers its own `Ant` and its
own `weak` and `hurt`: the chart names what the application provides, and
each application provides it in its language.

## Whom it serves, and who pays

- **First, the application developer in a compiled language** without an
  `eval`: Swift and Kotlin on iOS, macOS and Android, and Go. Today their
  charts are ECMAScript on quickjs, and their own code runs only in
  services and I/O processors. Here the chart's variables are their type,
  its guards and actions their functions.
- **Then the developer of an app on two platforms:** one chart, one
  grammar, an iOS and an Android app each registering the same names.
- **Then the binding author:** this is a native datamodel, as `"python"`
  is. Core needs no change: "a binding may define a datamodel of its own
  language under its own name, with no core change"
  (`tinyactors-core/docs/bindings.md`, section 8).
- **Who pays:**
  - each binding: one datamodel over the vtable
    (`include/tinyactors/datamodel.h`), in its language, as the Python
    binding's (`tinyactors-cpython/src/datamodel/`); it is smaller than
    Python's, since it compiles nothing: every expression is a name, a
    path, a literal or a call;
  - per actor: the application's value on the language's heap and a
    session row in the binding's table, as on Python's datamodel (433 B
    resident at 10,000 actors, `tinyactors-cpython/AGENTS.md`); core's
    region holds only the actor's record;
  - per guard or action: one call of the application's function, through
    the language's closure; on Kotlin each vtable entry is also a call from
    C into the JVM (JNI), whose cost is **unverified**;
  - per event on other datamodels: nothing.
- **Out of scope:** Python's and Node's datamodels, which already run
  their language. A sandbox: the functions are the application's trusted
  code; charts written by someone else belong on quickjs. A type system in
  SCXML: the chart names, the application's types say what the names are.

## What a chart may write

Every expression is one of four forms:

```
expression := path | literal | call | "In(" string ")"
call       := name "(" [ argument { "," argument } ] ")"
argument   := path | literal
path       := root { "." name | "[" ( integer | path ) "]" }
root       := name | "_event" | "_sessionid" | "_name" | "_ioprocessors"
literal    := integer | decimal | string | "true" | "false" | "null"
```

No operators and no nesting: what needs arithmetic or a comparison is the
application's function (decision 3). Every expression is parsed once, at
`chart_attach`, and every name in it resolved then.

| SCXML | Takes |
|---|---|
| `cond` | a guard, a boolean path, `In('state')`, `true`, `false` |
| `<data expr>`, `<assign expr>`, `<param expr>`, `<content expr>`, `<donedata>`, `<log expr>` | a path, a literal or a value function |
| `<assign location>`, `<foreach item index>`, `idlocation`, `namelist` | a path into the state |
| `eventexpr`, `targetexpr`, `typeexpr`, `srcexpr`, `delayexpr`, `sendidexpr`, `docexpr` | a path, a literal or a value function that gives text |
| `<foreach array>` | a path to a collection, in the state or in `_event.data` |
| `<script>`, in executable content or at the chart's top | an action |
| child content of `<data>`, `<assign>`, `<content>` | text, as a string literal; XML children refuse the chart |

- **The roots are the state's properties.** The chart's `<data>` ids are
  exactly the state type's stored properties: one missing on either side
  refuses the attach. `<data expr>` and a spawn's or an invoke's params
  set them (SCXML 5.2.2, 6.4.3).
- **A path into the state is checked at attach** against the type, so a
  misspelt name refuses the chart before an actor runs. A path into
  `_event.data` is read when evaluated; a key that is absent, or a value
  the argument's type cannot take, is `error.execution`.
- **A function is called with the arguments the chart names,** each
  converted to its parameter's type. A guard and a value function get
  only their arguments; an action also gets the state, to change.
- **A `<foreach>` iterates a copy** (SCXML 4.6), and binds `item` and
  `index` to properties the chart declares; a `<foreach>` that would
  declare a new variable refuses the chart, since a type cannot grow
  (W3C 150 and 151, on its skip list).

## What the application gives

Three registrations, in each language's own shapes:

| | What | Used for |
|---|---|---|
| **state** | a chart's name and a type | one value of it per session; its stored properties are the chart's variables |
| **guards and values** | a name and a function of plain values | `cond`, and every value or text expression |
| **actions** | a name and a function of the state (mutable) and plain values | `<script>` |

A type is anything the binding can read and write by property name and
convert to and from the value table (`tinyactors-core/docs/protocol.md`,
"Values"): numbers, text, booleans, byte strings, arrays, maps with text
keys, nested types, an actor's address. A property of another type (a
class with identity, a closure) refuses the registration.

**Several types may serve one chart name,** one per version: a new
version that adds a `<data>` registers its type before its image is
added, and an attach picks the type whose properties are the chart's
`<data>` ids. None or two that match refuses the attach.

## In each binding

The datamodel is the binding's, in its language, over the vtable, with
the rules the Python binding follows (`tinyactors-cpython/AGENTS.md`, "The
"python" datamodel"): rows in core's `ta_table` for charts, sessions and
payloads, handles never pointers, the last reference to a value dropped
after the outer call into core returns.

| | Reads and writes a property by name | Converts the state | Calls a function |
|---|---|---|---|
| **Swift** | a `WritableKeyPath` per property; `Mirror` reads but cannot write, so a macro on the type (`@ChartState`) writes the table of names to key paths | `Codable` | typed closures, their arguments converted from values by the closure's parameter types (parameter packs, SE-0393); vtable entries are `@convention(c)` functions |
| **Kotlin** | a table of property references; `kotlin-reflect` is large on Android, so KSP generates the table from an annotation (size **unverified**) | `kotlinx.serialization` | typed lambdas; every vtable entry is a JNI call into the JVM |
| **Go** | `reflect`'s `FieldByName` on an addressable value (exported fields) | `reflect`, or `encoding` | `func` values called through `reflect`; vtable entries are `//export` functions, each a cgo call |
| **C** (and wasm side modules in C) | no reflection: the application writes a table of names to getter and setter functions | the application's `serialize` and `deserialize` | function pointers with a context |

- **The state is the language's value,** held by the session's row (a
  Swift box, an index into the Kotlin binding's array of states as Node
  keeps its scopes, a Go slice index). The application never sees the
  row: an action gets the value, changes it, and the binding keeps it.
- **Payloads are the language's values,** copied through the value table
  when they cross to another actor (`capture`), as on Python's datamodel;
  `_event.data` is the payload's value; `to_wire` and `from_wire` convert
  to and from CBOR only at core's interfaces. No CBOR per guard.
- **Hibernation and snapshots** write the state by property name,
  `{"vars": {name: value}, "event": ...}`, as the other datamodels do, so
  a blob wakes under a newer version of the chart with the properties both
  declare, and `migrate` answers the old ones.
- **Failures:** a function that throws (Swift's `throws`, a Kotlin
  exception, a Go `error` result) is `error.execution`, its message the
  datamodel's `failure` text.

### What the bindings share

- **The grammar.** It is free of any language, so one parser in C, from
  text to a small tree in a caller's buffer, serves every binding (and the
  C version); it would be a freestanding layer beside core's others
  (decision 4). Resolving the names stays in each binding.
- **The conformance setup.** The W3C suite's charts use the `conf:`
  notation, which a stylesheet per language translates
  (`tinyactors-test/suites/w3c`, `confPython.xsl`). One `confApplication.xsl`
  turns each `conf:` operation into a call of a small function library,
  and each binding registers that library in its language, so the suite
  runs on every binding's application datamodel, beside quickjs, as the
  differential run asks. How many tests pass and the skip list (W3C 150
  and 151, XML content, the ECMAScript data model's tests) are
  **unverified**.

## Lifetimes

| Object | Owner | Created | Ends |
|---|---|---|---|
| the registrations (types, functions) | the binding, from the application | before the image that needs them | the host's close |
| a chart's parsed and resolved expressions | the binding's chart row | `chart_attach` | `chart_detach` |
| a session's state | the binding's session row, a value of the language | `create`, `deserialize` | `destroy` (after `serialize` too) |
| `_event` and its data | the session's row | `bind_event` | the next `bind_event`, or `destroy` |
| a payload's value | the binding's payload row | `capture`, `from_wire`, an input | `release` to 0, dropped after the outer call |
| a function's arguments | the call | before the call | its return |

## Open in core

Neither blocks a binding's datamodel, but both bite a host with more than
one datamodel and an attach that fails:

- **A failed attach leaks the charts attached before it,** in three
  places: `attach_charts` (`src/system/system.c:243-259`), the trace rules
  after it (`:386-388`) and the restore path (`:1267-1268`). This
  datamodel refuses an attach for every misspelt name.
- **Payloads cross between datamodels** without a check: the event view's
  data whatever its datamodel (`src/core/event_pool.c:177`), invoke params
  (`src/system/route.c:296`) and done.invoke data (`route.c:831`), against
  ADR 0009's decision 7. A host that runs these charts beside quickjs ones
  needs core to convert or refuse.

## History

- **The first sketch** (2026-11-21) was a C layer in core for the wasm
  anthill: variables as doubles at 8 × index, functions `int32_t f(void
  *fields)` with no context and no arguments, expressions only names, no
  strings. Its wasm spike (functions of a side module called through the
  core's table at 3.43 ns on Node.js; a module linked with
  `--import-memory` alone corrupting the core's stack, a side module not)
  is in this file's history and still holds for a C version under
  `@tinyactors/wasm`.
- **The second** (2026-11-23) kept it in core and described the
  application's types to it as layouts (offsets, kinds, capacities),
  with core checking them and converting to CBOR. Its review found what
  such a design must guard against: lengths in bytes a function writes,
  handles a function can forge, a new chart version with no new type.
  The maintainer's objection ended it: the application already has its
  types and functions, and the datamodel interface exists so a chart can
  use them. Describing them again to core was the mistake; the design
  above keeps them in the language and the binding.

## Decisions for the maintainer

1. **The name.** One name for every binding (`"application"` here), so a
   chart runs in an iOS and an Android app alike; or a name per language
   (`"swift"`, `"kotlin"`), as `"python"` is.
2. **Exact `<data>` to properties,** or properties the chart does not
   declare, kept by the application and invisible to paths. Exact is what
   lets an attach pick a version's type.
3. **No operators.** Every comparison is a function (`cond="weak(hp)"`,
   not `cond="hp <= 1"`). A few operators on paths and literals (`==`,
   `<`, `!`, `+`) would spare small charts a function each, at the cost of
   a grammar that grows and of semantics each binding must match.
4. **One parser in C, or one per binding.** In C it is written once and
   the grammar cannot drift; per binding, each is a few dozen lines with
   no C.
5. **Guards' purity** (SCXML B): a guard gets only its arguments, never
   the state, so it cannot change it; a value function likewise. Keep
   that, or let guards read the state whole.
6. **The C version:** with the first binding, for `ta-conform` and wasm
   side modules, or when a C application asks.

## Stages

1. **The first binding's datamodel** (Swift, which needs no glue C), with
   the grammar, its tests, and the conformance setup; the suite on it
   beside quickjs.
2. **Kotlin's, then Go's,** mirroring Swift's file by file; JNI's cost per
   entry measured, and Kotlin's design changed if it asks for it.
3. **The C version,** if decision 6 says so.
