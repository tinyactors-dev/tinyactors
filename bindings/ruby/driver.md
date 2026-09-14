# Run the Ruby implementation loop

Run the driver from the repository root:

```sh
python3 tools/drive_ruby.py
```

Implementation uses `gpt-5.6-luna` with low reasoning effort. Independent review uses `gpt-5.6-sol` with medium reasoning effort. The driver uses the Ruby executable configured by `tools/verify_ruby.py init`. On this machine that is Ruby 4.0.6. It uses your existing Codex CLI login. It does not retrieve or create credentials.

Preview the configuration without running agents:

```sh
python3 tools/drive_ruby.py --dry-run
```

The driver creates a branch named `ruby-loop/<id>` and a retained Git worktree under `bindings/ruby/.verification/driver/<id>/worktree`. It copies the verifier's current source snapshot into that worktree, including uncommitted Ruby/verifier fixes. If necessary, it creates an explicitly labelled input-snapshot commit. That snapshot is not an acceptance decision. Your original working tree and branch remain unchanged.

## Each iteration

1. The driver runs the full verifier against the candidate worktree.
2. It selects the first incomplete milestone in dependency order.
3. Luna receives the brief, allowed files, exact failing checks, and prior feedback.
4. Luna implements one attempt and runs focused tests. Luna does not commit.
5. The driver checks file ownership, protected files, Git HEAD, and ledger scope.
6. It runs the complete verifier again with the Codex session ID and agent context.
7. It checks the milestone and every previously passing check.
8. If those checks pass, Sol reviews the actual source and tests in a separate session.
9. If Sol approves, the driver commits the exact verified candidate on its branch.
10. Otherwise, it retains the candidate and sends concrete feedback to the next attempt.

After all checks pass, Sol performs a separate full-binding review. Only a fresh complete pass and final approval produce `DONE` and exit code `0`. A model's assertion that work is complete is never sufficient.

A milestone is narrower than a whole-brief gate where later dependencies are necessary. `behavior.runtime.*` and `behavior.high_reply.*` are deferred until integration. For example, the Memory byte-state probe needs actors and contexts that do not yet exist at the Memory milestone. Every deferred check remains in the full run and final gate. No check is deleted, skipped, or converted into a pass. Milestone selection is implemented in `tools/ruby_verify/loop.py`.

The original root checkout supplies the trusted verifier executable. Its tools, the candidate's verifier, the C core, and the assignment briefs cannot change during the loop. The record adapter's authored inventory, `briefs/raw-records.md`, is an explicit exception. It is documentation output, not a new assignment or a check definition. Other shared edits are limited to the declared loader, native registration/support, build, and test-fixture files. Sol reviews those edits too.

## Progress and evidence

The terminal reports timestamps, assignment names, agent messages, a heartbeat every 30 seconds, verification counts, regressions, reviews, and commits. Full process stdout/stderr, prompts, schemas, and reports remain in the loop directory. `progress.log` provides a compact history.

The existing verification database retains each native check attempt. Agent context includes loop ID, assignment, previous run, model, reasoning effort, Codex session ID, reported blockers, and token usage when Codex supplies it. `driver_loops` holds resumable loop state. `driver_iterations` holds implementation/review reports, verification IDs, feedback, status, and commit attribution.

```sh
sqlite3 bindings/ruby/.verification/checks.sqlite3 \
  'SELECT loop_id, json_extract(state_json,"$.status"), json_extract(state_json,"$.branch") FROM driver_loops;'
```

Only one driver can own a database at a time. Verification workers remain concurrent. Candidate processes receive timeouts, logs have a size limit, and process groups are cleaned up when interrupted. Exit zero without a completed Codex turn or a valid structured report is an error.

The script uses Codex's JSON event stream and schema-constrained final reports. Its command options were checked against the installed CLI and [official non-interactive documentation](https://learn.chatgpt.com/docs/non-interactive-mode).

## Stop and resume

Press Ctrl-C to stop. The driver retains the branch, candidate edits, logs, database history, and pending feedback. Resume with the loop ID printed by the driver:

```sh
python3 tools/drive_ruby.py --resume <loop-id>
```

Resume runs fresh verification. Saved model settings persist unless you explicitly override them. The driver refuses to resume if its trusted tools changed or the candidate's Git HEAD changed outside its commit procedure. Inspect that worktree before starting a replacement loop. A crash between a Git commit and its SQLite checkpoint can require this inspection too.

The default has no total iteration limit. It stops with an error after five consecutive attempts without verified progress, on infrastructure failure, or on a protected-file violation. These stops do not mean completion. Inspect the recorded feedback and resume after addressing the cause. The consecutive-stall counter resets on explicit resume.

Optional limits and overrides:

```sh
python3 tools/drive_ruby.py --max-iterations 3
python3 tools/drive_ruby.py --resume <loop-id> --max-iterations 10
python3 tools/drive_ruby.py --agent-timeout 2400 --stall-limit 8
```

`--max-iterations 0` continues until completion or an error. The default agent timeout is 1,800 seconds per implementation/review session. Verification gets 900 seconds. Exit `1` means a requested iteration limit paused the loop. Exit `2` means a driver error or blocked run. Exit `130` means interruption.

On completion, inspect and integrate the printed branch through your normal Git workflow. The driver does not push, merge into your original branch, or deploy anything.

## Testing and limits

```sh
python3 tools/test_ruby_driver.py
python3 tools/test_ruby_verify.py --ruby /path/to/supported/ruby
```

The verifier's harness gate includes the driver regression tests. Tests use real temporary Git repositories, worktrees, SQLite databases, and subprocesses. Synthetic Codex sessions exercise model selection, structured reports, review rejection, repair, commit acceptance, timeouts, context retention, scope violations, and completion. They do not invoke paid model sessions or count as binding evidence.

This local tool is not a security boundary against a malicious filesystem owner. It detects contract changes and evidence inconsistencies under the expected cooperative-agent workflow. Code review improves coverage but is not a proof of correctness. The full native checks remain mandatory.
