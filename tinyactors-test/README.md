# tinyactors-test

The conformance suite: every test of tinyactors that is data, run against
one command per implementation. The C reference (`ta-conform`, in
`tinyactors-core/tools/conform/`) and each binding's adapter are such
commands; an implementation conforms when the whole suite passes against
it ([ROADMAP.md](../ROADMAP.md), step 8).

## What it holds

| Suite | What | Judged by |
|---|---|---|
| [`suites/protocol`](suites/protocol/README.md) | what crosses core's API as CBOR: requests and replies, host configs, envelopes, neutral values | the bytes, compared with the expected ones (tag 39999 is a wildcard) |
| [`suites/scenarios`](suites/scenarios/README.md) | core's conformance scenarios (spawn, send, time, invoke, hibernation, migration, snapshots) as charts that check themselves, with the host operations between runs in `steps.txt` | the outcome the charts log: `pass` or not |
| `suites/w3c` | the W3C SCXML IRP suite, fetched from w3.org and converted per datamodel (`skip.txt`, `convert.py`) | the outcome its charts log, as for scenarios |

Nothing compares effects. As in the W3C IRP, a chart checks what happened
and ends in `pass` or `fail`; the implementation runs it with its own loop
and effect handlers in the environment that [docs/driver.md](docs/driver.md)
describes, and answers the outcome.

## Running it against a command

    mise run //tinyactors-test:run -- <command…>
    mise run //tinyactors-test:run -- <command…> --suite protocol --suite scenarios
    mise run //tinyactors-test:run -- <command…> --datamodel native

The command runs in the directory you call mise from, and speaks the
[driver protocol](docs/driver.md) on its standard input and output:
length-prefixed CBOR calls and replies in JSON-RPC 2.0's shape. Options:

| Option | Default | |
|---|---|---|
| `--suite protocol\|scenarios\|w3c` | all three | repeat it for several |
| `--datamodel quickjs\|native` | `quickjs` | the datamodel scenarios, W3C tests and `values.txt` run on |
| `--realm auto\|isolated` | the implementation's | quickjs only |
| `--language ecmascript` | `ecmascript` | the W3C stylesheet the scenarios are converted with; others join with their stylesheet |
| `--timeout <s>` | 60 | how long one reply may take before the command counts as hung |

Each case prints `pass`, `fail` (with why: a diff for bytes, the outcome
and the command's standard error for a chart) or `skip` (a method the
command answers -32601 to, once per method). A crash or a hang fails that
case; the runner starts the command again and goes on. The exit status is
1 when any case fails.

The W3C suite is downloaded once into `build/w3c/` (`mise run w3c`), and
the scenarios are converted into `build/scenarios/<language>/` when a
template changed. Both conversions need the pinned `uv` (`mise.toml`) for
SaxonC: the W3C stylesheets are XSLT 2.0.

## Tasks

- `mise run run -- <command…>`: the suites against a command (above).
- `mise run test`: the vectors' hex against their diag lines
  (`encode --check`), and the runner's own tests against a fake
  implementation (`test/`).
- `mise run encode`: write the vectors' hex from their diag lines.
- `mise run w3c`: fetch and convert the W3C suite; prints its directory.

## Layout

```
docs/driver.md          the protocol a conforming command speaks
suites/protocol/        vectors, and setups/ (the charts of their setups)
suites/scenarios/       <name>/<document>.txml and steps.txt
suites/w3c/             convert.py (both conversions), skip.txt
lib/                    the runner: driver.rb (process, frames), runner.rb (suites, report),
                        vectors.rb, scenarios.rb, cbor.rb, w3c.rb (fetch, convert)
mise/tasks/             run, test, encode, w3c
test/                   the runner's tests and fake_implementation.rb
build/                  the fetched and converted suites (not in git)
```
