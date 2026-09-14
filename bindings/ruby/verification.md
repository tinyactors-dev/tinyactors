# Ruby verification loop

The verifier captures the current source, builds the extension in a temporary directory, runs independent contract probes and each assignment's Minitest suite, then stores every verdict in a local SQLite database with the agent's context. Acceptance requires fresh passing evidence for every selected check and the shared gates. Missing code starts red; an agent cannot complete an assignment by reporting success or by running an empty suite.

## Start the local database

Run commands from the repository root. The harness needs Python 3.10 or newer with its standard-library SQLite module, clang, make, and Ruby 3.3 or newer with Minitest. The system Ruby on this machine is too old for the replacement extension. Configure the supported installation explicitly:

```sh
python3 tools/verify_ruby.py init \
  --ruby /Users/dhamidi/.local/share/mise/installs/ruby/4.0.6/bin/ruby
```

On another machine, supply that machine's Ruby executable. `init` persists its resolved path. No Python packages or SQLite command-line program are required to run the harness.

The default database is `bindings/ruby/.verification/checks.sqlite3`. Its parent directory also holds source archives and full stdout/stderr logs. This directory is ignored by Git. The existing task database is separate and is not modified. To share evidence between worktrees, put `--db /absolute/path/checks.sqlite3` before the subcommand. SQLite coordinates writers; builds use separate temporary directories.

## Agent iteration

Assign dependencies before consumers, following [the brief index](briefs/README.md). Before editing, inspect the current evidence:

```sh
python3 tools/verify_ruby.py status
python3 tools/verify_ruby.py next --brief bindings/ruby/briefs/raw/result.md --json
python3 tools/verify_ruby.py show behavior.result.TA_OK
```

After implementation and focused tests, run the assignment's acceptance checks:

```sh
python3 tools/verify_ruby.py run \
  --brief bindings/ruby/briefs/raw/result.md \
  --agent /root/raw_result --model gpt-5.6-luna --reasoning low \
  --task ruby-raw-result --iteration 1

python3 tools/verify_ruby.py gate \
  --brief bindings/ruby/briefs/raw/result.md --json
```

The loop consumes the gate's **exit code**, not prose from the agent. Exit `0` means every selected check and shared gate has a current pass. Exit `1` means acceptance failed. Exit `2` reports an invocation or harness setup error. An interrupted run exits `130`. A passing scoped gate establishes only that scope. Before accepting the rebuilt binding, run and gate the complete catalog:

```sh
python3 tools/verify_ruby.py run --agent /root/integrator \
  --task ruby-integration --iteration 1
python3 tools/verify_ruby.py gate --json
```

`--check 'behavior.runtime.*'` selects stable IDs for diagnosis. It still includes shared gates. Empty selections and mistyped patterns fail. A narrow selection must not replace the brief gate or full integration gate.

Use `--context-file /path/context.json` for additional context. It must contain a JSON object. Suggested keys are `assignment`, `parent_agent`, `previous_run`, `hypothesis`, `changed_files`, and `handoff`. The harness records this object under `extra`, alongside the command's agent, model, reasoning, task, and iteration fields. Context is supplied attribution, not verified agent identity. Do not include credentials.

`next` returns nonpassing checks with their owner brief, latest message, run ID, and agent context. Shared gates appear first. A missing dependency is a failure, not a skipped check or permission to add a stub. Preserve the failure and assign the dependency. An agent's completion report should include the run ID, scope, counts, and remaining failures.

## What runs

The initial catalog has 723 stable check IDs:

| Checks | Count | Evidence |
| --- | ---: | --- |
| Shared gates | 7 | Supported Ruby, compiled C constant oracle, fresh native build and load, coverage ledger, core regressions, harness regressions |
| Raw constants | 120 | Ruby values equal values printed by a compiled program that includes `src/tinyactors.h` |
| Raw procedures | 162 | 54 exact singleton exports, 54 native method implementations, 54 implementation ledger entries |
| Classes and methods | 226 | 31 required classes and 195 required public methods |
| Independent behaviors | 173 | Executable assertions on value/status distinctions, records, bounds, ownership, callbacks, and routing |
| Assignment suites | 35 | Nonempty, assertion-bearing Minitest suites for 31 classes and four infrastructure assignments |

API presence is reported separately from behavior. An export, `source_location == nil`, or an implemented ledger entry alone cannot establish correct runtime behavior.

The independent runtime fixtures call public Raw procedures. They build real byte-state charts, post a host trigger, send from one actor to another, inspect origin in a guard and action, and route a reply to the captured session address. Variants exercise false guards, forbidden guard effects, delayed replies at explicit nanosecond timestamps, eventless retention, internal-event replacement, byte writes, callback exceptions, escaped contexts, busy destruction, and stale actors. Convenience probes execute the public builder/System/Context path, including `_event` and missing-origin reply failure.

The other probes cover all status variants, native nil/false/zero, arbitrary bytes, copied event metadata, target parsing and constructor kinds, system-scoped uint64 references, memory bounds, arena siblings and deferred cleanup, explicit clock input, and registry token uses. `behaviors.json` contains concrete parameters for Ruby assertions, not TODOs or unconditional failures.

Each class still needs its brief's focused tests. These must cover its complete acceptance list, including paths beyond the independent probes, such as I/O lease completion, invocation authentication, upgrades, and snapshot policies. The integrator reviews those tests and their native coverage. A numeric check count is not a proof that every possible runtime path is tested.

## Evidence and invalidation

Each run records:

- Exact captured source manifest, SHA-256 digest, source archive, Git HEAD and working-tree summary.
- Catalog digest and immutable versions of each check specification.
- Ruby/compiler/Python/make versions, Ruby gem inventory, and selected build environment settings.
- Agent context, selected scope, timestamps, process exit status, assertion count, diagnostic, and log locations.

The captured source includes C sources/tests/examples, Ruby sources/tests/briefs, verifier sources, tools, and the root Makefile. Old extensions, object files, generated Makefiles, build directories, Python bytecode, and verifier data are excluded. The build must create exactly one `tinyactors_raw` native library. The shipped loader must load the fresh staged library.

Every Ruby check runs in a separate subprocess with a timeout and output limit. The harness requires a structured result with the current nonce/check ID, a clean exit, a positive assertion count, and no skips for a pass. A crash, signal, timeout, malformed result, or exit without evidence is an error. Process groups are terminated so compiler/test children do not continue after supervision ends. Minitest runs use seed `1` and the harness's reporter; agent test helpers are not loaded into independent probes.

A source, catalog, or environment change makes previous evidence **stale**, including previous passes. The conservative rule invalidates all checks after any captured source change. Editing even a test or brief therefore requires another run before acceptance. Edits during execution do not change the captured tree; the run remains historical and cannot pass the current gate. A build or test that changes the captured sources invalidates the whole run.

SQLite retains every attempt. The effective statuses are `unverified`, `pass`, `fail`, `error`, `stale`, and `incomplete`. Stored attempts also use `pending` and `running`. A run abandoned by a crash is recovered as interrupted once its operating-system lock is free. Pending/running checks become errors; an interrupted run cannot provide passing evidence, even if some checks finished first.

Concurrent runs use separate captures and artifact directories. Database transactions are short and use WAL mode. If runs overlap for a check, the latest-started attempt takes precedence; older completed evidence cannot hide a newer unfinished attempt.

This is a local verification tool, not a security boundary against an agent that controls the filesystem. Source/catalog hashes make changes reviewable. They do not stop a malicious owner from rewriting the verifier or database. Implementation agents must not alter the verifier, delete checks, relax assertions, or write verdicts to SQLite. The integrator owns verifier changes. Existing check IDs cannot silently disappear when the catalog is synchronized.

## Inspect the database

`checks` contains current assignments; `check_versions` keeps immutable specifications. `runs` contains attribution and source/environment evidence. `results` contains individual attempts. `check_history` joins check results to run context for queries.

```sh
python3 tools/verify_ruby.py runs --limit 5
python3 tools/verify_ruby.py show behavior.runtime.reply --limit 10
sqlite3 bindings/ruby/.verification/checks.sqlite3 \
  'SELECT check_id,status,agent,task,iteration,message FROM check_history WHERE check_id="behavior.runtime.reply" ORDER BY finished_at DESC;'
```

Do not edit statuses manually. `init` synchronizes the catalog; `run` writes verdicts; `gate` computes acceptance using the current hashes. Historical rows alone are insufficient to decide freshness. Logs and source archives are retained without automatic pruning; remove the entire local data directory only when deliberately discarding its history.

## Verify the verifier

```sh
python3 tools/test_ruby_verify.py
python3 tools/test_ruby_verify.py --ruby /path/to/supported/ruby
```

These tests exercise real subprocesses and SQLite transactions: evidence validation, assertion/skip handling, Minitest results, timeouts, signals, output limits, child cleanup, source capture and mutation, fingerprint invalidation, historical context, concurrent writers, interrupted-run recovery, selection errors, and the compiled C constant oracle. Synthetic namespace fixtures belong only to these harness tests and never count as binding acceptance evidence.
