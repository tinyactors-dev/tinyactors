# Infrastructure: callback and native-value bridge

Owner: callback adapter task. Status: planned.
Dependencies: bootstrap, record converters, Raw::Result, Raw::Value, and Raw::Memory.
Read [CONTRACT.md](CONTRACT.md) first.

## Outcome and files

Implement `ext/tinyactors_raw/callbacks.c` and `callbacks.h`.
Own `test/raw/callbacks_test.rb`.
The bridge helper `Raw.take_errors(system)` belongs to this assignment.
Use the bootstrap context factories and invalidation tokens before the Context procedures exist.

## Work

Implement the complete callback tables from the public header.
Include data-model, value, allocator, code-validation, diagnostics, terminal, trace, location, inspection, migration, and cleanup callbacks.
Delegate class-specific resource conversion to its owner.
Preserve each callback's input order and output/status convention.
Preserve action kind, code token, actor identity, phase, and host_user.

Call `rb_protect` or an equivalent protected Ruby API at every Ruby entry.
Always clear captured Ruby exception state before returning to core code.
Store diagnostic exceptions in a system-owned queue for `Raw.take_errors`.
Preserve failed context-effect statuses supplied by high-level Error objects.
Map ordinary user exceptions to the callback's documented failure behavior.
Define a deferred-reporting path for infallible callbacks without throwing through C.
Never return TA_AGAIN from a synchronous execution callback.

Invalidate contexts and mutable memory on success and failure.
Reject same-system reentry. Keep all operations under the GVL.
Preserve event metadata when bind_event cannot convert event data.
Implement native retain/release/snapshot counts through stable Ruby roots.
Use an explicit snapshot callback for isolation. Do not silently substitute shallow duplication.

## Acceptance

- Run distinct action tokens and false guards against the native interpreter.
- Throw from each callback family and verify cleanup plus error classification.
- Test failed context sends to missing and full destinations.
- Retain a context after success and after an exception. Verify both reject access.
- Trigger GC.start and GC.compact during callbacks and message lifetimes.
- Verify trace callback failures do not change statechart behavior.
- Verify failed data conversion preserves the incoming origin.

Do not implement application-level code lookup. CodeRegistry and DataModel own that policy.
