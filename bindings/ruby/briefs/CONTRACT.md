# Shared implementation contract

Status: design for the replacement binding. No class or runtime procedure exists yet.

## Boundary

`TinyActors::Raw` is a module with procedural singleton methods.
Each public C procedure maps to exactly one identically named Ruby method.
For example, `ta_actor_spawn` maps to `TinyActors::Raw.ta_actor_spawn`.
Handle classes identify resources. They do not duplicate procedures as instance methods.
Convenience classes call `Raw`. They never call the native extension through another interface.

Use Ruby 3.3 or newer for the replacement extension.
The namespace scaffold and coverage checker also run on the machine's system Ruby.
Use the Ruby C API and the public TinyActors C header.
Do not depend on `src/internal.h` from the binding.
Keep native helper functions at three parameters or fewer.

## Arguments and results

Preserve C input argument order. Remove output-only pointer arguments.
Preserve mutable input/output arguments, including `ta_inspect_cursor_t`.
Use these result rules consistently:

| C declaration | Ruby result |
| --- | --- |
| Status, no output | Integer with the exact `TA_*` status |
| Status, one output | `Raw::Result.new(status:, value:)` |
| Void, one output | The output value |
| Void, no output | `nil` |
| Scalar or borrowed pointer return | The scalar or a safe Ruby representation |

Every public procedure currently has at most one output argument.
A struct output remains one value, even when that struct has several fields.
Keep only outputs that the C contract declares valid for the returned status.
Use `nil` for other outputs. Do not manufacture success-shaped outputs after failure.
Definition build and prepare preserve the diagnostic record on failure.
`ta_system_inspect` returns a status and mutates its opaque cursor.

Two bridge helpers supplement the C procedures without using the `ta_` prefix.
`Raw.take_errors(system)` returns captured callback exceptions.
`Raw.context_code_owner(context)` returns the active definition's rooted Raw::Value.
Code-owner access checks context expiry and does not let Ruby reenter definition procedures during execution.

```ruby
result = Raw.ta_definition_build(system, chart)
result.status                  # TA_OK or a C failure status
result.value[:definition]      # Owned Raw::Definition, or nil on failure
result.value[:error]           # Copied diagnostic when the C contract provides one

result = Raw.ta_actor_spawn(system, { definition: definition })
result.value                   # Raw::ActorRef on success
Raw.ta_actor_stop(system, result.value) # Integer status
Raw.ta_system_config_init      # Hash with the actual C defaults
```

Reject malformed Ruby types with `TypeError` or `ArgumentError` before calling C.
Reject integer overflow with `RangeError` before narrowing to a C type.
Expired handles raise `RuntimeError` before dereferencing native memory.
A well-formed request that the runtime rejects returns its C status.
Do not convert `EMPTY`, `AGAIN`, `BUSY`, or `STALE` to success.
The high-level `Error` brief defines convenience exception policy.

## Raw data records

Use symbol-keyed Hash records with exact C field names.
Do not create a Ruby class for every public C struct.
Use Arrays for counted tables. Derive their counts at the boundary.
Append C sentinels in native storage. Reject user-supplied sentinel entries inside tables.
Keep definition order and duplicate parameter names.
Reject unknown fields, unknown union tags, and unsupported supplied fields.
Return copied, frozen record containers. Do not freeze an actor's live native state as a side effect.

| C value | Raw Ruby value |
| --- | --- |
| Enum, ID, size, timestamp, code token | Range-checked Integer |
| `uint8_t` semantic boolean | `true` or `false` |
| `ta_text_t`, authored C string | String, or nil for absence |
| `ta_bytes_t` | Binary String, copied when it escapes native storage |
| `ta_mut_bytes_t`, allocated memory | Scoped `Raw::Memory` |
| `ta_value_t` | `Raw::Value`, or nil for handle zero |
| `ta_actor_ref_t` | `Raw::ActorRef` |
| `ta_request_ref_t` | `Raw::RequestRef` |
| `ta_invocation_ref_t` | `Raw::InvocationRef` |
| Opaque pointers | Their dedicated Raw handle classes |
| Callback table | Hash of callables using C field names |

`Raw::Value.new(nil)` is a native Ruby nil value. Bare nil means an absent C handle.
Keep payload absence distinct from native nil and an empty byte string.
Keep absent text distinct from present empty text.
Validate UTF-8 and forbidden NUL bytes where the C contract requires them.
Bytes can contain NUL and invalid UTF-8.
Preserve tagged unions as `{ kind:, as: { ... } }` when the header names an `as` union.
Use the header's active member names inside that union.

Enumerations, sentinel constants, ABI version, and processor URIs live on `Raw` with exact C names.
Export `ta_text_from_cstr` as a pure conversion helper in addition to the extern procedures.
Its String result contains bytes before the first NUL. Nil remains absent.
The coverage ledger audits extern procedures. The bootstrap tests cover inline helpers and constants separately.

## Callback convention

Preserve callback input order. Replace `void *user` with the supplied Ruby user object.
Remove output-only callback arguments using the same result rules as procedures.
A status callback with an output returns `Raw::Result`.
A status callback without an output returns an Integer status.
Infallible void callbacks return nil. Boolean outputs are actual Ruby booleans.
No raw callback adapter applies Ruby truthiness to a status code.
High-level adapters construct these results for application Procs.

Protect every call into Ruby. No Ruby exception may unwind through core frames.
Capture and clear exceptions, including conversion and allocation failures.
Ordinary application exceptions produce `TA_EXECUTION_ERROR` where the callback permits a status.
Preserve `TinyActors::Error#status` from failed context effects.
Keep the error object available through the explicit bridge helper `Raw.take_errors(system)`.
Use the documented resource policy for allocation failures.
Define a safe deferred-reporting path for callbacks that cannot return a failure status.
Invalidate context and memory tokens in a cleanup path that runs after every callback.

Keep callbacks synchronous. Reject attempted same-system reentry at the bridge boundary.
Only expose the context effects permitted by the C execution phase.
No I/O, sleeps, Fibers that suspend execution, or host scheduler runs inside callbacks.

## Ownership and GC

Every pointer wrapper stores an owner and an explicit lifetime token.
Do not expose pointers, native handles, or memory addresses as Ruby Integers.
Reference IDs remain integers inside scoped reference objects. They are not pointers.
A reference object does not retain an actor incarnation in the core.
A reference from another system cannot address an actor with the same numeric ID.

Root callbacks, user objects, native values, code owners, and descriptor storage for their complete C lifetimes.
Implement both marking and compaction support for TypedData roots.
Keep C retain/release counts separate from Ruby wrapper reachability.
Track every owned definition reference, including lookups and explicit retains.
Preserve BORROW storage until the core calls its release hook.
Copy scratch strings before another native call can overwrite them.

Explicit release and completion procedures implement deterministic cleanup.
A GC finalizer must not call Ruby or free a live parent's arena.
The foundation owns deferred native cleanup and its safe drain boundaries.
Outstanding leases keep their owners alive. GC must not fabricate successful I/O completion.
System destruction must preserve `TA_BUSY` and leave every handle usable after that failure.
Successful destruction invalidates all borrowed system views.

## Namespace classes

The index lists every required class. Add no additional public class without updating this contract and its index.
C descriptor structs use Hash records, not placeholder classes such as `Raw::Chart` or `Raw::SystemConfig`.
Statuses and enum values use constants, not wrapper classes.
Transport-specific HTTP and WebSocket classes are outside this rebuild.

## Sender and event semantics

Raw event records expose all `ta_event_t` fields, including `origin` and `origintype`.
High-level callbacks use `context.event.origin`. `context._event` aliases `context.event`.
`Event#name` is the event name. `Event#type` is `:external`, `:internal`, or `:platform`.
Do not confuse the name with the SCXML event type.
Before the first dequeue, the event is nil.
An eventless transition sees the last dequeued event.
An internal event replaces that event and has no origin.

A reply uses the current event's captured origin and origintype.
A missing origin must produce an explicit high-level failure, never a self-send.
A delayed reply captures its destination when the action executes.
Name rebinding cannot redirect a reply to another incarnation.
Autoforward preserves origin. An explicit new send identifies the actor executing the send.
Keep SCXML invoke authentication separate from origin text.

## Implementation and acceptance

Each brief names owned files, dependencies, its interface, and behavioral tests.
The C header wins if a brief contradicts its runtime contract.
Report a contradiction before changing the shared Ruby interface.
Use Minitest for the replacement suite. Keep support fixtures under `test/support/`.
Exercise the native runtime in raw tests. Mocks alone do not establish boundary correctness.
Native test fixtures can create core resources before their Ruby procedure wrappers exist.
Those fixtures must call the public C API. Do not expose them as production Raw methods.
Add high-level tests that exercise Raw as well as focused convenience tests.

Run each changed test file and the relevant native checks.
Update only the assigned coverage entries after wrappers and tests pass.
An implemented entry must name its source file and a behavioral test file.
The coverage checker verifies files and callable exports. The tests establish behavior.
Do not present the coverage checker as a semantic test.
