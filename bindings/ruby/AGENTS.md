# Ruby binding rebuild

The user retired the previous Ruby implementation. Do not restore it from Git.
The current code defines namespaces only. The briefs describe future implementation.

## Required architecture

- Put procedural C wrappers on `TinyActors::Raw`.
- Preserve each C function name: `TinyActors::Raw.ta_context_send`, for example.
- Put opaque handle classes under `TinyActors::Raw::*`.
- Put convenience classes under `TinyActors::*`.
- Implement convenience classes in Ruby through `Raw` only.
- Use C field names in raw record hashes.
- Return C statuses at the raw boundary. Raise status-bearing errors in the convenience layer.
- Keep the runtime in C. Do not implement another scheduler, router, or statechart interpreter.

Read [the shared contract](briefs/CONTRACT.md) before implementing a brief.
Read the relevant declarations and comments in `src/tinyactors.h`.
The public C header controls execution, ownership, and callback semantics.

## Assignment rules

Each class has one brief. Infrastructure has separate briefs.
Read the assignment's dependencies before editing files.
Implement dependencies first. Do not replace missing dependencies with fake behavior.
Limit changes to the assignment's owned files and its registration or coverage entries.
Describe a shared-interface change before modifying an interface owned by another assignment.
Do not spawn agents unless the current user assignment requests delegation.

Future agents must provide code, focused tests, and a short completion report.
The report must name the changed files, commands, results, and remaining limitations.
A definition that compiles does not prove that its guards or actions execute correctly.
Never accept a descriptor field and then silently discard it.

## Runtime safety

Invalidate callback contexts on every callback exit, including exception exits.
Never expose a native address as a Ruby Integer.
Keep Ruby objects rooted while the core owns their values or callbacks.
Handle `GC.start` and `GC.compact` without losing roots.
Catch Ruby exceptions before returning through the C runtime.
Preserve context-effect failure statuses instead of converting every failure to `TA_EXECUTION_ERROR`.
Do not call Ruby from a GC finalizer.
Do not close an arena beneath a live system, definition, actor, or I/O lease.
Perform transport work outside execution callbacks.

## Coverage and verification

`api-coverage.json` assigns every public C procedure to one brief.
`make ruby-interface` checks the assignment ledger and any declared implementation evidence.
It does not treat planned entries as working bindings.
`ruby tools/check_ruby_interface.rb --require-complete` rejects incomplete coverage.

Keep the raw layer's function names and exported constants separate from convenience names.
Use meaningful behavioral tests for routing, origin, guards, timers, ownership, and failures.
Run the relevant tests before marking a ledger entry implemented.

## Verification loop

Read [verification.md](verification.md) before starting an assignment.
Run `python3 tools/verify_ruby.py run --brief <repository-relative-brief> --agent <agent-id>`
with model, reasoning, task, iteration, and optional context-file attribution.
Run the matching `gate` command before reporting the assignment accepted.
Report the run ID and any remaining failures. Missing dependencies remain failures.
A scoped pass does not establish complete binding acceptance.

The integrator owns `tools/verify_ruby.py`, `tools/ruby_verify/`, and
`tools/test_ruby_verify.py`. Implementation agents must not remove checks,
relax assertions, substitute test doubles in independent probes, or write
verification verdicts to SQLite. Propose a correction to the integrator when
an independent check contradicts the C header or shared contract.
Use Minitest in the brief's assigned test file. Empty suites and skips do not pass.
