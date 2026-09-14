# Infrastructure: native bootstrap and resource foundation

Owner: integration task. Status: planned. Dependencies: none.
Read [CONTRACT.md](CONTRACT.md) and `bindings/ruby/AGENTS.md` first.

## Outcome

Create the replacement native extension and the shared lifetime machinery.
Use `Init_tinyactors_raw` and the load path `tinyactors/tinyactors_raw`.
Never load the retired extension.

## Owned files

- `ext/tinyactors_raw/extconf.rb`
- `ext/tinyactors_raw/tinyactors_raw.c`
- `ext/tinyactors_raw/support.h`
- `ext/tinyactors_raw/support.c`
- `ext/tinyactors_raw/registrations.h`
- `lib/tinyactors/raw.rb`
- `test/test_helper.rb` and `test/support/*`
- `test/bootstrap_test.rb`

Paths are relative to `bindings/ruby/`.
Class assignments own their implementation files. The integrator owns central registration.
Expose each native class initializer as `void tar_init_<stem>(VALUE raw)`.
Load pure Ruby classes through the Ruby require list.
Call initializers in dependency order. Unimplemented classes have no fake exports.

## Required implementation

1. Compile the current C runtime with the hosted Ruby extension.
2. Locate repository sources relative to extconf, independently of the current directory.
3. Export the exact header constants and the inline text helper.
4. Define shared owner, lifetime-token, GC-root, and callback-cleanup primitives.
5. Enforce system identity and same-system reentry checks.
6. Keep Ruby exceptions out of C runtime frames.
7. Specify deferred cleanup behavior for abandoned native handles.
8. Provide fault-injection fixtures without modifying the C public API.

Provide native test fixtures that create real systems, definitions, actors, and scoped callback views.
These fixtures let class tests execute before all procedural wrappers exist.
Keep fixture entry points private to the test build.
Never substitute a fake scheduler, mailbox, or success result.

Support native Integer boundaries on 32-bit and 64-bit size_t.
Keep ABI checks tied to the compiled header.
Do not invent alternate copies of the C runtime or its enum values.
Define raw class representations before dependent assignments start.
Publish their TypedData layouts and private handle factories in support.h.
Include factories for System, Definition, Arena, Context, references, cursors, and scoped memory.
These factories do not register unimplemented runtime procedures.
Class assignments extend these representations through their own initializers.
Publish helper signatures in support.h before dependent assignments start.

## Acceptance

- Build on the supported Ruby version with compiler warnings enabled.
- Require each entry point twice without duplicate classes or registrations.
- Check ABI constants and every enum family against the compiled header.
- Verify nil, empty text, Unicode, embedded NUL, and overflow conversion boundaries.
- Confirm that no retired binary is discoverable through either require path.
- Exercise deferred cleanup without invoking Ruby from GC finalizers.

Return the build command, smoke-test command, shared helper interfaces, and remaining limitations.
Do not implement runtime procedures owned by class briefs.
