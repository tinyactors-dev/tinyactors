# tinyactors-test

The conformance suite: every test of tinyactors that is data, run against
one command per implementation. The C reference (`ta-conform`, in
`tinyactors-core/tools/conform/`) and each binding's adapter are such
commands; an implementation conforms when the whole suite passes against
it ([ROADMAP.md](../ROADMAP.md), step 8).

## What it holds

| Suite                                            | What                                                                                                                                                                              | Judged by                                                            |
| ------------------------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------- |
| [`suites/protocol`](suites/protocol/README.md)   | what crosses core's API as CBOR: posted messages, replies and notifications, host configs, neutral values                                                                | the bytes, compared with the expected ones (tag 39999 is a wildcard) |
| [`suites/scenarios`](suites/scenarios/README.md) | core's conformance scenarios (spawn, send, time, invoke, hibernation, migration, snapshots) as charts that check themselves, with the host operations between runs in `steps.txt` | the outcome the charts log: `pass` or not                            |
| [`suites/w3c`](suites/w3c/irp/SOURCE)            | the W3C SCXML IRP suite, vendored in `irp/` (its `SOURCE` and `LICENSE`) and converted per datamodel (`skip.txt`, `convert.py`)                                                   | the outcome its charts log, as for scenarios                         |

Nothing compares effects. As in the W3C IRP, a chart checks what happened
and ends in `pass` or `fail`; the implementation runs it with its own loop
and effect handlers in the environment that [docs/driver.md](docs/driver.md)
describes, and answers the outcome.

## Running it against a command

    mise run //tinyactors-test:run -- <command…>
    mise run //tinyactors-test:run -- <command…> --suite protocol --suite scenarios
    mise run //tinyactors-test:run -- <command…> --datamodel native
    mise run //tinyactors-test:run -- <command…> --kind reference
    mise run //tinyactors-test:run -- <command…> --milestone M1

The command runs in the directory you call mise from, and speaks the
[driver protocol](docs/driver.md) on its standard input and output:
length-prefixed CBOR calls and replies in JSON-RPC 2.0's shape. Options:

| Option                                | Default              |                                                                                                                                                                     |
| ------------------------------------- | -------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `--milestone M1\|…\|M5`               | every case           | the set of one milestone of the bindings' plan ([`suites/milestones.txt`](suites/milestones.txt)): its vector files, scenarios and suites and those of the milestones before it; `--suite` narrows it |
| `--suite protocol\|scenarios\|w3c`    | all three            | repeat it for several                                                                                                                                               |
| `--datamodel quickjs\|native\|<name>` | `quickjs`            | the datamodel scenarios, W3C tests and `values.txt` run on; an implementation may offer others by name (`ta-conform`: `test`, core's tests' datamodel)              |
| `--realm auto\|isolated`              | the implementation's | quickjs only                                                                                                                                                        |
| `--skip <file>`                       | none                 | W3C test ids with reasons (the format of `suites/w3c/skip.txt`) skipped besides that list: what a datamodel that implements less cannot pass; repeat it for several |
| `--kind binding\|reference`          | `binding`            | the methods the command must offer ([docs/driver.md](docs/driver.md), "Kinds"): `ta-conform` is the reference, a binding's adapter a binding                                     |
| `--timeout <s>`                       | 60                   | how long one reply may take before the command counts as hung                                                                                                       |

The scenarios and the W3C suite are converted for the language of the
datamodel under test: the name `host.create` answers for it
([docs/driver.md](docs/driver.md), "Datamodels"), so a run on a native
datamodel runs charts only that datamodel takes.

Each case prints `pass`, `fail` (with why: a diff for bytes, the outcome
and the command's standard error for a chart) or `skip` (a method the
command answers -32601 to, once per method). A crash or a hang fails that
case; the runner starts the command again and goes on. The exit status is
1 when any case fails, and when the run proves nothing: a suite that
passed no case, or a method the command's kind must offer that it did not
([docs/driver.md](docs/driver.md), "Kinds"). A binding's run also skips
the protocol suite's envelope-level cases, each marked `only reference`
in its file: requests and configs whose bytes no binding's API makes (not
CBOR, not one request map, an id of the caller's, messages without an id), which
`ta-conform` runs ([suites/protocol/README.md](suites/protocol/README.md),
"Envelope-level cases").

A binding's `test` task runs the set of the milestone it is working on
(`--milestone M1`), named once in its `conform` task, and moves it on
when the milestone is done; `ta-conform` runs everything. Every vector
file and scenario is in some milestone (`test/milestones_test.rb`).

The W3C suite is in git (`suites/w3c/irp`, from w3.org under W3C's
3-clause BSD license: `SOURCE`, `LICENSE`). It is converted into
`build/w3c/<language>/` (`mise run w3c`) when `convert.py`, the stylesheet
or the suite changed, and the scenarios into `build/scenarios/<language>/`
when a template or the stylesheet changed. The echo setup's chart (`suites/protocol/setups/echo.txml`) is converted
the same way, into `build/setups/<language>/`. The conversions need the
pinned `uv` (`mise.toml`) for SaxonC: the W3C stylesheets are XSLT 2.0.
Runs in parallel share them: each conversion is written into a new
directory and `build/<kind>/<language>` is a link moved to it in one
rename, under a lock, so a run never reads half a conversion
(`lib/w3c.rb`, `W3C.publish`).

## Languages

A chart language is a stylesheet from the W3C suite's `conf:` notation to
that language: W3C's `confEcma.xsl` (ecmascript) and `confXPath.xsl`
(xpath) in `suites/w3c/irp`, ours in `suites/w3c` (`confPython.xsl`,
python). The language's name is the datamodel its `conf:datamodel`
template writes, and the runner picks it by the name `host.create`
reports for the datamodel under test. The W3C tests and the
scenarios are written in that notation, so each language runs all of
them, but for the tests its skip list names.

Adding a language takes three things:

1. **A stylesheet**, `suites/w3c/conf<Language>.xsl`, with a template for
   every template of `confEcma.xsl` (the vocabulary the tests and the
   scenarios use), each expression on one line with no leading space. Its
   header lists what it assumes the datamodel exposes where that is not
   decided yet (`confPython.xsl`: `_event` by attribute, `_event.data` a
   dict, a value never given `None`).
2. **A skip list**, `suites/w3c/skip-<language>.txt` (the format of
   `skip.txt`): the W3C tests the stylesheet cannot translate, each with
   its reason. The tests of the ECMAScript data model (B.2) name
   `datamodel="ecmascript"` and are written in ECMAScript, so a language
   other than ECMAScript skips them (`skip-python.txt`).
3. **A compile check** in `suites/w3c/check.py` (`CHECKS`): the call that
   compiles an expression, a location and a script in the language
   without running it (Python's `compile()`).

`mise run check-languages` (part of `mise run test`) converts the suite,
the scenarios and the setups' templates with every stylesheet and checks the result: each
document is well-formed XML with no `conf:` element or attribute left, and
for a language with a compile check, every test it does not skip is in
that language and every expression, location and script in it compiles,
but for the values meant to be invalid (`conf:illegalItem`,
`conf:invalidNamelist`; `check.py` finds them by translating a probe).

## Tasks

- `mise run run -- <command…>`: the suites against a command (above).
- `mise run test`: the vectors' hex against their diag lines
  (`encode --check`), each language's translation (`check-languages`),
  the adapters' lint rules against their tests (`lint`), and the
  runner's own tests against a fake implementation (`test/`).
- `mise run encode`: write the vectors' hex from their diag lines.
- `mise run w3c [language]`: convert the W3C suite for a language
  (ecmascript by default); prints its directory.
- `mise run lint [path…]`: the adapters' lint (`lint/`, ast-grep): its
  rules' tests, then the adapter files given, which must import only the
  binding's public module (`@tinyactors/node`, `tinyactors`) and the
  language's standard library (`node:` built-ins; Python's, listed in
  `lint/utils/`), so the adapter frames with the binding's own CBOR
  functions. Each
  binding's lint task runs it on its adapter.
- `mise run check-languages [language…]`: translate the suite and the
  scenarios with each stylesheet and check the result (above).

## Layout

```
docs/driver.md          the protocol a conforming command speaks
suites/milestones.txt   the cases each milestone of the bindings' plan runs (--milestone)
suites/protocol/        vectors, and setups/ (the charts of their setups; echo.txml in the
                        conf: notation, converted per language)
suites/scenarios/       <name>/<document>.txml and steps.txt
suites/w3c/             convert.py (both conversions), check.py (check-languages),
                        conf<Language>.xsl (ours: confPython.xsl), skip.txt,
                        skip-<language>.txt
suites/w3c/irp/         the W3C IRP suite (vendored): manifest.xml, <id>/*.txml,
                        confEcma.xsl, confXPath.xsl, SOURCE, LICENSE
lib/                    the runner: driver.rb (process, frames), runner.rb (suites, report),
                        vectors.rb, scenarios.rb, cbor.rb, w3c.rb (stylesheets, convert),
                        milestones.rb (--milestone)
lint/                   the adapters' lint: sgconfig.yml, rules/ (ast-grep), tests/
mise/tasks/             run, test, encode, w3c, check-languages, lint
test/                   the runner's tests and fake_implementation.rb
build/                  the converted suites (not in git)
```
