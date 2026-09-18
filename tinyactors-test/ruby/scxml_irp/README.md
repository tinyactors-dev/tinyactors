# W3C SCXML IRP import

`ruby/scxml_irp` converts the W3C SCXML 1.0 conformance tests in
`vendor/scxml-irp` into chart and scenario files for the scenario runner, using
the jq data model.

```text
mise run import-scxml-irp     # regenerate ruby/charts/scxml_irp and ruby/scenarios/scxml_irp
mise run scxml-irp-baseline   # run the IRP scenarios and record their failures in baseline.json
mise run scenarios -n scxml-irp            # run only the IRP scenarios
mise run scenarios -n 'scxml-irp: test144' # run one
```

The generated files are committed. `ruby/scxml_irp_check.rb` (part of
`mise run test`) fails when they are stale. It also loads every generated
chart through the DSL's checks and decodes every chart spec in the host.

## Files

| File | Role |
|---|---|
| `importer.rb` | Reads `manifest.xml`, decides what to skip, and writes the charts, scenarios, and `INDEX.md` |
| `document.rb` | Converts one `.txml` document to chart DSL source |
| `jq_expressions.rb` | Maps `conf:` markup and small ECMAScript expressions to jq |
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
Anything else, including every `<script>` body, makes the test skipped.

**Strict conversion.** Every element and attribute must be understood. Any
other construct makes the test skipped, and its reason is recorded in
`INDEX.md`. Nothing is dropped silently.

**Invoked child machines.** An inline machine (`<invoke><content><scxml>`)
becomes a chart of its own, named `irp_testNNN_childN`. The invoke refers to
it with `chart:`, which embeds that chart's spec as JSON in the invoke's
`<content>`. A file-based machine (`src="file:testNNNsub1.scxml"`) keeps its
`src` unchanged, and its `.txml` dependency is converted to the chart
`irp_testNNNsub1`. An invoker in the test host maps `file:NAME.scxml` to the
chart `irp_NAME`.

**`<data src>`.** The importer reads the file (the core performs no I/O) and
passes it as inline content.

## Outcome

A test passes when its machine enters the final state `pass`. The generated
charts declare `_irp_outcome`. The `pass` and `fail` final states assign
`"pass"` or `"fail"` to it on entry. Each scenario spawns the chart, advances
logical time from one delayed-send deadline to the next until the actor is
done (`run_to_completion`), and then asserts that `_irp_outcome` is `"pass"`.
SCXML would report the outcome as `<donedata>`. The data model is used because
it works before the core compiles `<donedata>`.

## Expectations

Every imported scenario is expected to pass unless `baseline.json` lists it
with its exact error message:

- A listed scenario that fails differently is a **FAIL**.
- A listed scenario that starts passing is an **XPASS**.

After a core change, run `mise run scxml-irp-baseline` and review the
`baseline.json` diff. Removed entries are progress; new or changed entries
need a reason.

## Skipped tests

These tests are skipped; `INDEX.md` has the full list and reasons:

- **Manual tests.** A person judges their log output.
- **Optional BasicHTTP tests** (SCXML C.2).
- **ECMAScript-only and null-data-model tests.** These documents hard-code
  `datamodel="ecmascript"` or `"null"`.

## Known limitations

- **Handler blocks.** A state with several `<onentry>` or `<onexit>`
  handlers gets one concatenated action list, because the spec keeps one list
  per state. An error in one handler therefore also skips the handlers after
  it. `INDEX.md` notes the affected tests.
- **Unbound versus null.** jq has no `undefined`. `conf:unboundVar` and
  `conf:eventFieldHasNoValue` therefore compare with `null`.
