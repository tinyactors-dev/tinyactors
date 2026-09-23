# W3C SCXML IRP import

`ruby/scxml_irp` converts the W3C SCXML 1.0 conformance tests in
`vendor/scxml-irp` into scenario files for the scenario runner. Tests written
in datamodel-neutral `conf:` markup use the jq data model; tests that require
`datamodel="ecmascript"` use the ECMAScript data model. Each test becomes one file, `ruby/scenarios/scxml_irp/testNNN.rb`.
The file holds every document the test needs (its sub-documents and the test
document) as SCXML, followed by the test's scenarios. The host imports each
document with `ta_definition_import` through the SCXML codec that ships
(`tinyactors-scxml`, core ADR 0053): no Ruby code reads SCXML.

```text
mise run import-scxml-irp     # regenerate ruby/scenarios/scxml_irp
mise run scxml-irp-baseline   # run the IRP scenarios and record their failures in baseline.json
mise run scenarios -n scxml-irp            # run only the IRP scenarios
mise run scenarios -n 'scxml-irp: test144' # run one
```

The generated files are committed. `ruby/scxml_irp_check.rb` (part of
`mise run test`) fails when they are stale. It also loads every generated
file and imports every document in the host through the codec.

## Files

| File | Role |
|---|---|
| `importer.rb` | Reads `manifest.xml`, decides what to skip, and writes the scenario files and `INDEX.md` |
| `conf_transform.rb` | Rewrites one `.txml` document as SCXML for its data model (what `confEcma.xsl` does for ECMAScript) |
| `emitter.rb` | Writes the generated Ruby around the documents |
| `jq_expressions.rb` | Maps `conf:` markup and small ECMAScript expressions to jq |
| `ecma_expressions.rb` | Maps `conf:` markup to ECMAScript, for `datamodel="ecmascript"` tests |
| `expectations.rb` | Applies `baseline.json` to the generated scenarios |
| `baseline.json` | Known failures and their exact error messages (written by `scxml-irp-baseline`) |
| `../scenarios/scxml_irp/INDEX.md` | Status of all 200 tests: imported or skipped, with the reason |

## Conversion

The tests are written in datamodel-neutral `conf:` markup, which
`vendor/scxml-irp/confEcma.xsl` turns into ECMAScript. `jq_expressions.rb`
follows that stylesheet template by template, but produces jq for the
`datamodel.jq` adapter instead:

- **Data.** Data are fields of the actor's root object: `conf:id="1"` declares
  `Var1`, and `conf:location="1"` is the location `.Var1`.
- **System variables.** These are jq variables: `$_event`, `$_sessionid`,
  `$_name`, `$_ioprocessors`.
- **`In()`.** `In("id")` is a jq function that tests the configuration.
- **Evaluation errors.** Illegal expressions, locations, and conditions
  (`conf:illegalExpr`, `conf:invalidLocation`, `conf:nonBoolean`) become
  `error("conf:…")`. This is a runtime jq error, which the data model must
  report as `error.execution`.
- **Delays.** `conf:delay` becomes a literal delay in milliseconds.

The importer and the jq data model therefore share a contract. The adapter
must bind `$_event`, `$_sessionid`, `$_name`, and `$_ioprocessors`, and
define `In`. Tests that use these fail until it does.

**ECMAScript written directly.** A few tests write ECMAScript expressions
without `conf:` markup. Small ones are translated: literals, variables,
member and index access, arithmetic, comparisons, `&&`, `||`, and `In(...)`.
Anything else, including every `<script>` body, makes a jq test skipped.

**ECMAScript documents.** A document with `datamodel="ecmascript"` becomes
a chart with `datamodel: 'ecmascript'`, and the scenario harness compiles it
with the `datamodel.ecmascript` adapter (charts with `datamodel: 'jq'` or no
datamodel use `datamodel.jq`). Its expressions, locations, conditions, and
`<script>` bodies are ECMAScript already and pass through verbatim. Its
`conf:` markup is translated by `ecma_expressions.rb`, which follows
`confEcma.xsl` template by template and produces the stylesheet's text: data
are ECMAScript variables (`conf:id="1"` declares `Var1`, `conf:idVal="1=1"`
is `Var1==1`), and the outcome is assigned as `'pass'` or `'fail'`. A child
machine without a `datamodel` attribute uses its parent's data model. The
adapter has no XML DOM, so tests that call DOM methods such as
`getElementsByTagName` on XML data are skipped.

**Strict conversion.** The rewrite refuses every `conf:` construct and
expression it cannot express, and the codec refuses every SCXML element and
attribute it does not understand. A refused test is skipped with its reason
in `INDEX.md`. Nothing is dropped silently.

**Invoked child machines.** An inline machine (`<invoke><content><scxml>`)
stays inline, rewritten in the parent's dialect. The codec serializes it with
its namespace (SCXML G.6) as content of type `application/scxml+xml`, core
hands the bytes to the invoker, and the host's SCXML invoker imports them
through the same codec. A file-based machine (`src="file:testNNNsub1.scxml"`)
keeps its `src`, and its `.txml` dependency becomes the document
`irp_testNNNsub1`, which the invoker finds by that name.

**Inline `<scxml>` as a value.** An `<scxml>` document can also be the
content of `<assign>` or `<data>` (test530 assigns one and invokes it with
`<content expr>`). It stays inline too: the data model imports the markup as
a string, and the SCXML invoker reads a string value as SCXML text.

**`<data src>`.** The attribute is kept. The scenario file hands the file's
text to the host (`files:`), and the codec fetches it through the host's
`ta_chart_src_resolver` (the core performs no I/O).

## Outcome

A test passes when its machine enters the final state `pass`. The generated
charts declare `_irp_outcome`. The `pass` and `fail` final states assign
`"pass"` or `"fail"` to it on entry. Each scenario spawns the chart, advances
logical time from one delayed-send deadline to the next until the actor is
done (`run_to_completion`), and then asserts that `_irp_outcome` is `"pass"`.
SCXML would report the outcome as `<donedata>`. The data model is used because
it works before the core compiles `<donedata>`.

A few tests (457, 459, 460) write their own top-level `<final id="pass">` and
`<final id="fail">` instead of `conf:pass` and `conf:fail`. The importer adds
the same assignment to the end of their `<onentry>`.

## Expectations

Every imported scenario is expected to pass unless `baseline.json` lists it
with its exact error message:

- A listed scenario that fails differently is a **FAIL**.
- A listed scenario that starts passing is an **XPASS**.

After a core change, run `mise run scxml-irp-baseline` and review the
`baseline.json` diff. Removed entries are progress; new or changed entries
need a reason.

## Known failures

`baseline.json` currently records no known failures.

## Skipped tests

These tests are skipped; `INDEX.md` has the full list and reasons:

- **Manual tests.** A person judges their log output.
- **Optional BasicHTTP tests** (SCXML C.2).
- **Null-data-model tests.** These documents hard-code `datamodel="null"`,
  which the harness does not provide.
- **XML DOM tests** (557, 561). They call `getElementsByTagName` and
  `getAttribute` on XML data, and the ECMAScript data model has no XML DOM.

## Known limitations

- **Unbound versus null.** jq has no `undefined`. `conf:unboundVar` and
  `conf:eventFieldHasNoValue` therefore compare with `null`.
