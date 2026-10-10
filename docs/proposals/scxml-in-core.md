# SCXML in core: a small XML writer beside the compiler's parser

**Status:** Adopted → commits `bb603ff8` (tinyactors-scxml deleted, its
error cases test the compiler) and `77f39f31` (the writer and the JS
bindings on it), 2026-10-09. As built: the functions are named
`ta_xml_writer_open`, `_attribute`, `_text`, `_close`, `_finish` (the
compiler's internal `ta_xml_attribute` type took the shorter name), the JS
operations are a `Uint8Array` whose strings are taken in order (the web
module's Node-API shim has byte arrays only), and increments 2 and 3 landed
as one commit, so the writer arrived with its first caller.
**Touches:** tinyactors-core (`src/compile`, `compile.h`, the JavaScript
binding), tinyactors-scxml (deleted), the changelog components and the
docs that mention the parked codec.
**Not a plan:** nothing here is decided or scheduled. ADRs and roadmaps win.

## Goal

The maintainer's words: "Port tinyactors-scxml into src/. I don't want
every binding to have to worry about getting charts in and out."

## 1. What a binding is missing

What a binding does with charts, and who does it today:

| Need | Today | Missing? |
|---|---|---|
| SCXML text in, image out | `ta_compile_charts` (C): `src/compile/xml.c` (509 lines) parses, `scxml.c` compiles | no |
| Diagnostics with file, line, column | `ta_compile_diagnostics` (C) | no |
| `src` resolution | the resolver callback (C); where files live is the binding's policy | no |
| Inspection of a compiled chart (states, names, data model) | `image.h` (C) | no |
| A chart built in the binding's language, in | each binding writes and escapes XML text itself: `core.mjs` lines 70-96 (`escText`, `attrText`, `serialize`) | **yes** |
| SCXML text out, for a built chart | the same JS serializer | **yes** |
| SCXML text out, for a document read as text | the binding keeps the text it was given | no |

So two things are missing, and they are one thing: a way to hand an element
tree to C and get SCXML text back, without the binding writing or escaping
XML. Text in, compile, diagnostics, resolution and inspection exist.

One more piece of chart reading lives in JavaScript: `fromSCXML` finds the
chart's name with a regular expression (`core.mjs:694`). The compiled
image already has the name (`ta_image_chart_name`), so that moves to it.
`fromSCXML`'s other regular expression (`core.mjs:708`, which entries of
its `documents` map are charts) is part of that JS-only option and stays.

The PostgreSQL binding takes SCXML text from SQL, which is that binding's
natural form. It needs nothing new.

## 2. The design

### 2.1 A writer in `src/compile`

`src/compile/write.c` writes XML from calls: open an element, add an
attribute, add text, close. It escapes, indents and checks nesting. A
binding walks its own structure and calls it; the result is SCXML text,
which `ta_compile_charts` reads as today. The writer knows no SCXML: the
compiler validates the text it is given, with the positions it reports
today, and the binding can show that text (`toSCXML()`), so a diagnostic's
line and column point at something the user can read.

In `include/tinyactors/compile.h`:

```c
/* XML from calls, for charts and manifests a binding builds: it escapes,
 * indents two spaces per level and checks nesting. Feed the text to
 * ta_compile_charts / ta_compile_system. After an error every later call
 * returns the same error. */
typedef struct ta_xml_writer ta_xml_writer;

/* allocator NULL: the C library's malloc, as for ta_compile_options. */
TA_API ta_status ta_xml_writer_new(const ta_allocator *allocator, ta_xml_writer **out);
/* TA_ERROR_INVALID: not an XML name. */
TA_API ta_status ta_xml_open(ta_xml_writer *writer, ta_string_view name);
/* TA_ERROR_STATE: after the element's first child. */
TA_API ta_status ta_xml_attribute(ta_xml_writer *writer, ta_string_view name,
                                  ta_string_view value);
TA_API ta_status ta_xml_text(ta_xml_writer *writer, ta_string_view text);
/* TA_ERROR_STATE: nothing is open. */
TA_API ta_status ta_xml_close(ta_xml_writer *writer);
/* The document; valid until ta_xml_writer_free. TA_ERROR_STATE: an element
 * is still open, or there is none. */
TA_API ta_status ta_xml_finish(ta_xml_writer *writer, ta_string_view *out);
TA_API void ta_xml_writer_free(ta_xml_writer *writer);
```

Only pointers, views and fixed-width integers cross the ABI
(architecture.md section 11.5). The buffer is the compiler's `ta_vector`
(`compile_support.c`). It is hosted code in `libtinyactors_runtime`, like
the rest of `compile`.

Output rules, chosen to be simple and readable, not to match anything:

- Attributes are quoted with `"`. A value that contains `"` and no `'` is
  quoted with `'`, so an expression such as `above({"min":99})` stays
  readable. `&` and `<` are escaped, and `"` inside `"`-quoted values.
- Text escapes `&`, `<`, and `>` after `]]`. It is written where it is
  given, with no indentation added to it.
- An element without children is `<x a="1"/>`. Each child element starts
  on a new line, indented two more spaces. The end tag gets its own line
  when the last child was an element. So `<script>f()</script>` stays on
  one line.
- No XML declaration; namespaces are attributes the caller gives
  (`xmlns="http://www.w3.org/2005/07/scxml"`).

Everything it writes uses only what `xml.c` reads: the predefined entities,
both quote characters, no DOCTYPE.

### 2.2 The JavaScript binding on top of it

`Compiler` does not change: it still turns an xstate config into an `el()`
tree. What happens to the tree changes. `binding.c` gets one function,
`writeXML(ops, strings)`, which replays a flat list of operations into the
writer. One call per chart, so the web module does not cross into wasm once
per element:

```js
/* the el() tree as operations, written by C (ta_xml_writer) */
function serialize(root) {
  const ops = [], strings = [];
  const s = (x) => strings.push(String(x)) - 1;
  (function walk(node) {
    if (typeof node === 'string') return void ops.push(3, s(node));
    ops.push(1, s(node.tag));
    for (const [k, v] of Object.entries(node.attrs)) if (v !== undefined) ops.push(2, s(k), s(v));
    node.children.forEach(walk);
    ops.push(4);
  })(root);
  return native.writeXML(Uint32Array.from(ops), strings);
}
```

`escText`, `attrText` and the old `serialize` go (27 lines). Callers of
`serialize` (`StateMachine._compile`) do not change.

`SCXMLLogic` compiles in its constructor, which it does anyway on first
use, and takes its `id` from the compiled image's chart name instead of the
regular expression. A document that does not compile now throws from
`fromSCXML()` instead of from the first `createActor`.

### 2.3 Snapshots

Restore finds the machine whose generated XML equals the XML the snapshot
stored (`core.mjs:1528`). The comparison stays as it is: the C writer is
deterministic, so a snapshot taken after this change restores. A 0.2.0
snapshot whose machine text comes out different (for example `>` after
`]]`, or text next to elements) fails restore with the existing message
("is not defined ... or it changed since"). That is accepted; no shim.

### 2.4 Tests

- `tests/unit/test_compile.c`, `test_xml_writer`: escaping of `&`, `<`, `"`
  and `]]>`; the quote choice; layout; each refused call; and a chart built
  with the writer compiles to the same image as the same chart written by
  hand (with `TA_COMPILE_STRIP_DEBUG`).
- From tinyactors-scxml's `tests/ta_scxml_test.c`, its error cases with a
  line and column (`test_errors`, `test_compile_error_positions`), rewritten
  as compiler tests: each document must fail with a diagnostic at the line
  and column the case names. Positions are the one thing the compiler's
  tests do not check today. The cases that need what the compiler does not
  do (the codec's limits, its foreign-content lowering, its span table) are
  not ported.
- The binding's existing tests (`print-scxml.js`, `system.test.js`'s
  `toSCXML()` assertions, W3C, conformance, examples) run unchanged. If one
  asserts a byte the new rules write differently, the assertion changes.

## 3. tinyxml or `xml.c`

| | `src/compile/xml.c` (keep) | tinyxml (`txml`) |
|---|---|---|
| Lines | 509 | about 7,300 in `src/` (18 files) |
| wasm32, `-Os`, code and data (measured 2026-10-09) | 6.1 KB | 76.6 KB, 62 KB without its writer |
| Reads what the compiler needs (namespaces, CDATA, comments, predefined and numeric references) | yes | yes |
| Also has | | the internal subset's entities, incremental parsing, end positions and attribute spans, the XML conformance suite |
| Cost to adopt | none | rewrite `scxml.c`'s reads over cursors; a release dependency of `libtinyactors_runtime` on txml (ADR 0001 decision 17); `tinyactors.wasm` (304 KB) grows by about 60 KB |

Nothing in the goal needs what only tinyxml has. Keep `xml.c` as the
compiler's one parser. tinyxml stays a component of its own that core does
not use. The writer is about 120 lines; txml's writer would replace those
with a dependency and 14 KB of wasm, and it always quotes with `"`.

## 4. What happens to tinyactors-scxml

Its typed spec (`ta_scxml_spec`) is the earlier core's in-memory chart, and
this core's compiler goes from text to image without one. Its reader is a
second SCXML front end over txml, and its encoder writes from that spec.
None of it has a caller in this core. What is worth keeping is its error
cases with positions (section 2.4). The rest is deleted:

- `tinyactors-scxml/` (15 tracked files, 7,611 lines, of which 3,099 are
  generated: the amalgamation and the API description). The tag
  `tinyactors-scxml/v0.1.0` stays; its release line ends.
- The `scxml` component in `mise/lib/changelog/components.rb`.
- Mentions of the parked codec: `AGENTS.md` (the components list and the
  "parked" line), `README.md` (repository layout), `ROADMAP.md` item 4,
  the comments in `mise/tasks/build` and `mise/tasks/test/_default`,
  `docs/build/how-it-works.md`, `docs/build/how-to/upgrade-the-toolchain.md`,
  `mise/lib/c_release.rb`'s comment, the `release-changelog` skill's
  component list (then `agents sync`), and a row in this directory's index.

## 5. Lines added and deleted

Hand-written lines, estimated:

| File | Added | Deleted |
|---|---|---|
| `tinyactors-core/src/compile/write.c` (new) | 120 | |
| `tinyactors-core/include/tinyactors/compile.h` | 25 | |
| `tinyactors-core/mise/core.rb` (the compile layer's sources) | 1 | 1 |
| `tinyactors-core/tests/unit/test_compile.c` | 110 | |
| `tinyactors-core/bindings/shared/binding.c` (`writeXML`) | 40 | |
| `tinyactors-core/bindings/shared/core.mjs` | 14 | 30 |
| `tinyactors-core/bindings/surface.json` | 7 | |
| `tinyactors-scxml/` (hand-written part) | | 4,512 |
| `mise/lib/changelog/components.rb` | | 2 |
| docs, task comments and the skill (section 4) | 3 | 20 |
| **Total** | **about 320** | **about 4,565** |

Generated as well: `include/tinyactors_runtime.api.json` grows by the
writer's seven functions (`mise run api`), and 3,099 generated lines leave
with `tinyactors-scxml/`. `tinyactors.wasm` grows by about 2 KB.

## 6. Increments

Each one builds and passes `mise run test`. None runs `mise run test:full`.

1. **`chore: tinyactors-scxml is gone; its error cases test the
   compiler`.** Section 4's deletions, and the ported error-position cases
   in `test_compile.c`. Runs: `mise run test`.
2. **`feat(core): ta_xml_writer, XML from calls, for charts a binding
   builds`.** `write.c`, `compile.h`, `core.rb`, `test_xml_writer`,
   `tinyactors_runtime.api.json`, `surface.json`. Runs: `mise run test`,
   `mise run check:wasm`.
3. **`refactor(core,napi): JavaScript machines are written by
   ta_xml_writer`.** `writeXML` in `binding.c`; `serialize` over it in
   `core.mjs`; `fromSCXML` takes its name from the image. Runs: `mise run
   test`, then `mise run node:test`, `mise run web:test`, `mise run w3c:js`.
   W3C stays 191/191 in C (`mise run w3c`) and 158 + 21 in both JS
   backends.

## 7. Decisions

1. **Writer, not a second way into the compiler.** The binding's tree
   becomes text that the compiler parses, instead of a builder that fills
   the compiler's internal tree (`ta_xml_node`) directly. The second would
   skip one parse, but needs a writer anyway for `toSCXML()`, plus changes
   in `scxml.c`, which reads raw source text for `<content>` markup and
   literal documents. *Recommendation:* the writer. A parse of a generated
   chart is linear and small next to compiling it.
2. **Keep `xml.c`; do not adopt tinyxml in core.** *Recommendation:* keep
   it (section 3). Revisit only if a binding needs something only tinyxml
   has, such as entities from an internal subset.
3. **Calls, with the JS binding batching them.** The C API is five calls; a
   single call taking an operations array would be a second shape for the
   same thing. *Recommendation:* the calls. The flat array is the JS
   binding's own transport inside `binding.c`, not C API.
4. **Delete tinyactors-scxml now, keeping only its error cases.**
   *Recommendation:* yes. Its spec, reader and encoder have no caller in this
   core, and the tag keeps the code.
