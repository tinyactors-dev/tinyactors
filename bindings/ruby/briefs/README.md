# Ruby implementation briefs

Status: namespace scaffold only. All runtime procedures are planned.

The replacement needs 31 classes and four infrastructure assignments.
Every class has its own brief. Raw C data records use Hashes instead of additional descriptor classes.

## Delegate one assignment

Use Luna with low reasoning effort for an individual assignment.
Select the model and reasoning setting when creating that future agent.
No subagents are started by this plan.

```text
Implement bindings/ruby/briefs/<selected-brief>.md.
Read bindings/ruby/AGENTS.md and bindings/ruby/briefs/CONTRACT.md first.
Confirm that the listed dependencies exist before implementation.
Read the relevant public declarations in src/tinyactors.h.
Change only the assignment files and its approved registration or coverage entries.
Do not add fake behavior for missing dependencies.
Run the required behavioral tests against the real C runtime.
Read bindings/ruby/verification.md.
Run tools/verify_ruby.py run for this brief with your agent/model/task context.
Require a fresh scoped gate pass before reporting acceptance.
Do not modify the independent verifier or its check catalog.
Report the verification run ID and remaining failed checks.
Report changed files, commands, test results, and remaining limitations.
Do not spawn other agents.
```

If a dependency is missing, select its assignment first.
If a class needs several iterations, complete its interface and tests in the same assignment.
Do not mark partial behavior as an implemented C procedure.

## Shared contract and infrastructure

- [Shared contract](CONTRACT.md): names, records, statuses, callbacks, ownership, and reply behavior.
- [00: Bootstrap](00-bootstrap.md): build, class representations, shared helpers, and native lifetimes.
- [01: Records](01-records.md): complete struct conversion and Raw.ta_target_parse.
- [02: Callbacks](02-callbacks.md): protected callbacks, rooting, status preservation, and expiry.
- [99: Integration](99-integration.md): full raw and convenience acceptance scenarios.

## Dependency order

1. Complete bootstrap representations and helper interfaces.
2. Implement Raw::Result, Raw::Value, and Raw::Memory.
3. Implement record conversion against the bootstrap handle factories.
4. Implement callbacks against bootstrap context and lifetime primitives.
5. Implement the remaining raw classes and their assigned procedures.
6. Implement Error, Event, Target, Message, and CodeRegistry.
7. Implement Action, Send, and Invoke.
8. Implement StateBuilder, then MachineBuilder, then Definition.
9. Implement Context, then DataModel. Implement Snapshot and Inspection.
10. Implement Actor and Invocation, then IORequest.
11. Implement the convenience System and the complete integration suite.

Bootstrap owns raw type representations before record conversion starts.
A raw class assignment adds its public behavior and procedures to those representations.
This separation prevents cycles between record conversion and resource procedures.
Within a step, delegate only assignments whose complete dependencies exist.
The integration owner serializes changes to shared headers, registration, and loader lists.

## Raw classes

Each raw procedure is a singleton method on TinyActors::Raw.
For example, call `Raw.ta_actor_stop(system, actor_ref)`, not `actor_ref.stop`.
Raw handle classes provide representation and lifetime checks.

| Class | Brief | Main responsibility |
| --- | --- | --- |
| `TinyActors::Raw::Result` | [result](raw/result.md) | Exact status plus one output |
| `TinyActors::Raw::Allocator` | [allocator](raw/allocator.md) | Explicit hosted or custom allocator adapter |
| `TinyActors::Raw::Arena` | [arena](raw/arena.md) | Arena ownership and allocation procedures |
| `TinyActors::Raw::Memory` | [memory](raw/memory.md) | Bounded native byte access |
| `TinyActors::Raw::Value` | [value](raw/value.md) | Rooted Ruby native values |
| `TinyActors::Raw::System` | [system](raw/system.md) | Configuration, run, time, shutdown, destruction |
| `TinyActors::Raw::Definition` | [definition](raw/definition.md) | Build, prepare, retain, publish, inspect |
| `TinyActors::Raw::Context` | [context](raw/context.md) | Callback views and actor effects |
| `TinyActors::Raw::ActorRef` | [actor_ref](raw/actor_ref.md) | Scoped actor IDs, routing, lifecycle, upgrade |
| `TinyActors::Raw::RequestRef` | [request_ref](raw/request_ref.md) | I/O leases and ingress |
| `TinyActors::Raw::InvocationRef` | [invocation_ref](raw/invocation_ref.md) | Authenticated service returns |
| `TinyActors::Raw::InspectCursor` | [inspect_cursor](raw/inspect_cursor.md) | Opaque resumable inspection |

## Convenience classes

| Class | Brief | Main responsibility |
| --- | --- | --- |
| `TinyActors::Error` | [error](high_level/error.md) | Status-bearing exceptions |
| `TinyActors::Event` | [event](high_level/event.md) | Complete current-event metadata |
| `TinyActors::Target` | [target](high_level/target.md) | Explicit routing kinds |
| `TinyActors::Message` | [message](high_level/message.md) | Host-originated message inputs |
| `TinyActors::CodeRegistry` | [code_registry](high_level/code_registry.md) | Per-definition callable tokens |
| `TinyActors::Action` | [action](high_level/action.md) | Declarative executable content |
| `TinyActors::Send` | [send](high_level/send.md) | Actor sends and checked reply descriptors |
| `TinyActors::Invoke` | [invoke](high_level/invoke.md) | State-owned invocation descriptors |
| `TinyActors::StateBuilder` | [state_builder](high_level/state_builder.md) | One state and its transitions |
| `TinyActors::MachineBuilder` | [machine_builder](high_level/machine_builder.md) | Reusable machine authoring |
| `TinyActors::Definition` | [definition](high_level/definition.md) | Compile and own a native definition |
| `TinyActors::Context` | [context](high_level/context.md) | Safe callback access and replies |
| `TinyActors::DataModel` | [data_model](high_level/data_model.md) | Native Ruby state and callback adaptation |
| `TinyActors::Snapshot` | [snapshot](high_level/snapshot.md) | Durable isolated actor observations |
| `TinyActors::Inspection` | [inspection](high_level/inspection.md) | Streaming resumable scans |
| `TinyActors::Actor` | [actor](high_level/actor.md) | One exact actor incarnation |
| `TinyActors::IORequest` | [io_request](high_level/io_request.md) | One explicit host I/O lease |
| `TinyActors::Invocation` | [invocation](high_level/invocation.md) | One live external service |
| `TinyActors::System` | [system](high_level/system.md) | Public host API around Raw |

## C procedure ownership

The machine-readable assignment ledger is [api-coverage.json](../api-coverage.json).
It contains every extern procedure and its exact C type signature.
Every entry is currently planned. A Markdown assignment does not establish implementation.
`ta_text_from_cstr` is an inline helper assigned to bootstrap and tested separately.

| Owner | C procedures |
| --- | --- |
| [TinyActors::Raw::Arena](raw/arena.md) | `ta_arena_create`, `ta_arena_open`, `ta_arena_alloc`, `ta_arena_defer`, `ta_arena_close`, `ta_arena_stats`, `ta_arena_set_limit` |
| [TinyActors::Raw::System](raw/system.md) | `ta_system_config_init`, `ta_system_create`, `ta_system_set_limits`, `ta_system_run`, `ta_system_next_deadline`, `ta_system_stats`, `ta_system_shutdown`, `ta_system_destroy` |
| [TinyActors::Raw::Definition](raw/definition.md) | `ta_definition_prepare`, `ta_definition_build`, `ta_definition_retain`, `ta_definition_release`, `ta_definition_revision`, `ta_definition_description`, `ta_definition_footprint`, `ta_definition_publish`, `ta_definition_unpublish`, `ta_definition_lookup` |
| [TinyActors::Raw::Context](raw/context.md) | `ta_context_inspect`, `ta_context_in`, `ta_context_host_user`, `ta_context_bytes`, `ta_context_arena`, `ta_context_raise`, `ta_context_send`, `ta_context_cancel` |
| [TinyActors::Raw::ActorRef](raw/actor_ref.md) | `ta_actor_spawn`, `ta_actor_inspect`, `ta_actor_set_limits`, `ta_actor_stop`, `ta_actor_release`, `ta_name_bind`, `ta_name_unbind`, `ta_name_lookup`, `ta_session_lookup`, `ta_post`, `ta_actor_upgrade` |
| [TinyActors::Raw::RequestRef](raw/request_ref.md) | `ta_io_next`, `ta_io_complete`, `ta_io_receive`, `ta_io_receive_error` |
| [TinyActors::Raw::InvocationRef](raw/invocation_ref.md) | `ta_invocation_emit`, `ta_invocation_done`, `ta_invocation_fail`, `ta_invocation_is_cancelled` |
| [TinyActors::Raw::InspectCursor](raw/inspect_cursor.md) | `ta_system_inspect` |
| [Record adapter](01-records.md) | `ta_target_parse` |

## Verification

Run `make ruby-interface` from the repository root.
The check validates signatures, assignment files, and implementation evidence for completed entries.
It also compares implemented entries with actual Raw exports.
Run `ruby tools/check_ruby_interface.rb --require-complete` before declaring the binding complete.
The strict check intentionally fails while any entry remains planned.
The final integration brief owns examples and behavioral acceptance.
