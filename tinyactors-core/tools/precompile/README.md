# Precompiled datamodels: a chart's expressions in Swift, Kotlin and Go

A spike on a branch, not a decided design. A chart says
`datamodel="swift"` (or `"kotlin"`, `"go"`) and its expressions are that
language's code, compiled with the application by the language's own
compiler: they use the application's types and functions directly, as a
Python chart uses Python and a Node chart uses JavaScript.

```
order.scxml ──ta-precompile-table──▶ expression table (JSON)
                                      │
             generator (per language) ▼
             OrderSession: the chart's <data> as typed properties,
             each expression a case of a switch on its index
                                      │ compiled with the app
core ──vtable──▶ datamodel_precompiled (C) ──one function──▶ the language
```

## The pieces

| Piece | What it is |
|---|---|
| `include/tinyactors/datamodel_precompiled.h`, `src/datamodel_precompiled/` | the C layer: the datamodel vtable around the generated code. It keeps the payloads (CBOR), builds `_event`, captures, `<foreach>` and the blobs' framing, and calls the language through one function (`ta_precompiled_request`: what to do, to which expression). Freestanding. |
| `table.c` (`ta-precompile-table`) | a chart's expression table, `<data>` declarations and fingerprint, as JSON, from core's compiler |
| `demo/` | the scripted order every example runs: three adds (one refused), a hibernation and a wake, the address, the place, the done data |
| `go/` | the runtime package (`tinyactors/`: the cgo bridge, `Value`, `As[T]`), the generator (`cmd/tinyactors-gen`, run by `go generate`), the example (`example/order/`, its generated `order_gen.go` checked in) |
| `swift/` | the runtime (`Sources/Tinyactors`: the bridge, a CBOR coder for `Codable`, `Value`), the generator (`Sources/tinyactors-gen`) and its SwiftPM build plugin, the example (`Sources/Order`) |
| `kotlin/` | the runtime (`src/main/kotlin/tinyactors`: the bridge, `Value`, the `late` delegate), the generator (the `generateCharts` task in `build.gradle.kts`), the JNI half (`native/bridge.c`), the example |
| `generated/` | snapshots of what the Swift and Kotlin generators wrote for the order (theirs is a build output) |

Each language's `run.sh` builds and runs its example after
`mise run build --release` in `tinyactors-core`; all three print the same
run.

## How each language gets what the chart needs

| | Go | Swift | Kotlin |
|---|---|---|---|
| a variable's type | `go/types`: the generator type-checks the expressions in a function whose locals are the variables | `lazy var items = [Item]()`: the compiler infers it; `initData` assigns it | `var items by itemsCell`, `itemsCell = late { emptyList<Item>() }`: inferred from the lambda, run at `initData` |
| a variable in an expression | rewritten by the generator: `items` → `s.items` (resolved by `go/types`, so a field selector is untouched) | `self`'s property | `this`'s property |
| a value assigned | `tinyactors.As[[]Item](value)`, the type written by the generator | `try cast(value)`, the type inferred from the target | `cast(value)`, reified, inferred from the target |
| errors in an expression | at the chart's line, from the generator, before any file is written; `//line` keeps the chart's lines in compile errors and panics | at the chart's line and column (`#sourceLocation`) | at the generated file's line; a comment names the chart's |
| values that cross | `fxamacker/cbor` | a CBOR coder for `Codable` (`CBOR.swift`, the standard library has none) | `kotlinx-serialization-cbor` (`useDefiniteLengthEncoding`) |
| how it is generated | `go generate` | a SwiftPM build plugin over the target's `.scxml` files | a Gradle task over `src/main/scxml` |

## What it does not do yet

- **The generated code repeats each value expression three times**
  (`value`, `encode`, `string`): the expression table does not say whether
  a value feeds an `<assign>`, a `<param>` or a `<log>`. Core could
  record each expression's role in the image.
- **`<foreach>`'s item and index must be declared `<data>`** (a type is
  needed); SCXML lets a loop declare them.
- **A chart changed after its code was generated is refused** at
  `ta_host_add_image` (the fingerprint), but core takes no message from
  `chart_attach`, so the reason does not reach the application.
- **Kotlin has no line directive**, so its compile errors name the
  generated file; Swift's runtime traps (an index out of range) end the
  process, where Go's panics and Kotlin's exceptions become
  `error.execution`.
- **A generic in an attribute is escaped** (`emptyList&lt;Item&gt;()`),
  as `<` is in any datamodel's conditions.
- Not measured: the cost per event. Not offered: `migrate`, snapshots,
  W3C conformance (the W3C tests are written for ECMAScript and would be
  rewritten per language).
