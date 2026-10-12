# Scenarios

What a host does with charts, judged as the W3C IRP judges its tests: the
charts check what happened themselves and end in the final state `pass` or
`fail`, whose `<onentry>` logs `Outcome`. Nothing outside the charts is
compared. A scenario adds to a W3C test only what a chart cannot do
itself: the host operations between runs (spawn and send from outside, the
clock, hibernation, snapshots, a new chart version, migration).

They are core's conformance scenarios (`tinyactors-core/tests/conformance`)
in this form. An expectation a chart cannot check from inside is not
ported; each scenario's `steps.txt` says which, and where it is covered.

## A scenario

A directory with:

- **documents** `<name>.txml`: SCXML in the W3C IRP's neutral notation,
  the `conf:` attributes and elements of the W3C stylesheets
  (`confEcma.xsl`, fetched with the suite into `build/w3c/irp`). A document
  is translated per datamodel by that language's stylesheet, as the W3C
  tests are (`suites/w3c/convert.py --scenarios`, into
  `build/scenarios/<language>/`); `<name>.txml` becomes `<name>.scxml`,
  the name `<invoke src>` uses. Every expression and condition is a
  `conf:` attribute or element that `confEcma.xsl` defines; attributes
  that are not expressions (`event`, `target`, `delay`, `src`, `id`,
  `name`) are plain SCXML, as in the W3C tests. So a language with a W3C
  stylesheet runs every scenario.
- **`steps.txt`**: the host operations, one per line, in order. After each
  the implementation runs the host until it is idle (`docs/driver.md`, "The
  scenario environment").

| Line | Step (`docs/driver.md`, "Scenario steps") |
|---|---|
| `image <document> …` | compile these documents (named without `.txml`) into one image and add it; a chart already present gets a new version |
| `spawn <alias> <chart> [<params>]` | spawn the current version of the chart named `<chart>` (its `<scxml name>`), as `<alias>` |
| `send <alias> <event> [<data>]` | an event from outside |
| `send-many` and indented `<alias> <event> [<data>]` lines | one batch |
| `advance <ms>` | the clock moves on |
| `hibernate <alias>` | the actor leaves memory; an event or a timer wakes it |
| `snapshot <name>`, `restore <name>` | the whole host to a blob, and back (a restart) |
| `migrate [wake] [timeout <ms>]` | actors on older versions move to the current one |

`<params>` and `<data>` are CBOR diagnostic notation (JSON is a subset of
it). A text `"@alias"` in them stands for that actor's address. Lines
starting with `#` are comments.

## Writing one

- One chart judges: it ends in `<conf:pass/>` only after the last step,
  and any chart may go to `<conf:fail/>` (`conf:targetfail`) as soon as
  something is wrong. The first `Outcome` decides.
- Check with `conf:` conditions (`conf:idVal="1=13"`, `conf:inState`),
  data with `conf:id`, `conf:expr`, `conf:location` and
  `conf:eventDataFieldValue`, arithmetic with `conf:incrementID` and
  `conf:sumVars`, text with `conf:quoteExpr` and `conf:concatVars`.
- A check that needs an outside step (the clock, a send) is an event the
  steps send, handled by a transition that fails when its condition is not
  met: `<transition event="check" conf:idVal="1=13"/>` then
  `<transition event="check" conf:targetfail=""/>`.
- `conf:expr` copies its text into every language unchanged: give it only
  what every language reads the same, integers. Text is
  `conf:quoteExpr`.
- Spawn params and event data name `<data>` as the stylesheets do:
  `conf:id="1"` is `Var1`, so params are `{"Var1": 10}`.
- When a timer could end the scenario early, make the judge's last check
  need a flag that only the last step's event sets (`snapshot`'s
  `check6`).
- `steps.txt` says, in comments, which of the original expectations a
  chart cannot check and where they are covered instead.

## Running them

Against an implementation, through the protocol it speaks
([docs/driver.md](../../docs/driver.md), `scenario.run`):

    mise run //tinyactors-test:run -- --suite scenarios <command…>

The runner converts the documents for the datamodel's language first
(`build/scenarios/<language>/`). An outcome other than `pass` fails the
scenario: `fail`, `compile` (a document did not compile) or `timeout` (the
steps ran out with no `Outcome`).
