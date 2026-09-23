# Virtual LLM conversation actors

A named conversation actor may be resident only while it is processing a turn.
Between turns, the host stores its session image and data-model binding locator,
marks its directory entry unloaded, and releases all resident interpreter
memory. The logical actor address remains claimed.

## Separation of state

Do not put a large transcript in Tinyactors interpreter state. Store it in an
application database, append-only conversation log, object store, or persistent
data structure. Bind the actor to a small host object such as:

```c
typedef struct {
    host_conversation_store *store;
    host_conversation_id id;
    uint64_t committed_version;
    host_turn_buffer *current_turn;
} conversation_binding;
```

The data-model adapter exposes expressions and locations over that binding. The
binding may lazily read compact projections, but slow database operations should
remain explicit asynchronous effects rather than blocking expression callbacks.
The checkpoint stores only a stable binding locator such as the conversation ID
and committed version. It does not copy the transcript.

## Host adapters

A typical LLM host registers these boundaries:

- **External-event router** — accepts normalized SCXML-session events from
  core's built-in SCXML Event I/O Processor. It retains the deliveries it keeps (`ta_external_delivery_retain()`),
  resolves resident or unloaded destinations, and eventually calls
  `ta_actor_deliver()`. Its queue, fencing, and backpressure policy are
  host-owned; it is not registered as an I/O processor.
- **Client-stream I/O processor** — publishes normalized deltas and terminal
  messages to SSE, WebSocket, or another client transport. It retains values
  needed after the synchronous callback returns. Delivery acknowledgement is an
  ordinary external event when the chart needs to observe it.
- **Transcript/storage I/O processor** — appends user, assistant, and tool
  records with stable command IDs. Completion or failure returns as an event,
  making partial commits explicit in the chart.
- **LLM invoker** — owns one streaming provider request. It reports normalized
  chunk events, normal completion, or unavailability and supports cancellation.
- **Tool invokers** — own cancellable tool operations used by individual
  tool-call actors.
- **Durability adapter** — encodes/decodes session images, resolves host values,
  stores binding locators, and indexes non-resident deadlines. This is residency
  infrastructure, not a statechart I/O processor and not part of core.

## One-time setup

The following omits error-handling boilerplate but uses the public Tinyactors
call shapes:

```c
ta_system *system;
ta_system_create(&(ta_system_config){
    .allocator = allocator,
    .initial_time = host_monotonic_time(),
}, &system);

ta_system_set_actor_directory(system, host_directory);
ta_system_set_external_event_router(system, host_external_event_router);

ta_system_intern_atom(system, HOST_STR("client-stream"), &client_io_type);
ta_system_intern_atom(system, HOST_STR("transcript"), &transcript_io_type);
ta_system_intern_atom(system, HOST_STR("llm"), &llm_invoke_type);
ta_system_intern_atom(system, HOST_STR("tool"), &tool_invoke_type);

ta_system_register_io(system, client_io_type, client_stream_io);
ta_system_register_io(system, transcript_io_type, transcript_io);
ta_system_register_invoker(system, llm_invoke_type, llm_invoker);
ta_system_register_invoker(system, tool_invoke_type, tool_invoker);

/* Automatically installed and used for omitted <send> types. */
ta_atom scxml_io_type = ta_system_scxml_io_type(system);

ta_definition_compile_spec(system, &(ta_definition_compile_params){
    .spec = &conversation_spec,
    .datamodel = conversation_datamodel,
    .allocator = allocator,
    .out = &conversation_definition,
});
ta_system_register_definition(system, conversation_definition,
                              &conversation_definition_id);
```

The directory callbacks must implement atomic comparisons. For example,
`replace(address, expected, replacement)` changes the record only if its current
entry equals `expected`; otherwise it returns `TA_ERR_STALE`.

The address a callback receives is an interned handle (ADR 0065): equal
addresses are the same pointer, so a directory can key its map by the pointer
and never hash text. The handle is borrowed for the call. A directory that
keeps an entry retains the address (`ta_address_retain`) and releases it when
the entry goes, and releases every address it still holds before the system is
destroyed. `ta_address_view` gives the strings, for a directory that stores
them.

## Creating and running a conversation

```c
ta_actor_id actor;
ta_value binding = host_open_conversation_binding(conversation_id);

ta_actor_spawn(system, &(ta_actor_spawn_params){
    .definition = conversation_definition_id,
    .address = &conversation_address,
    .datamodel_binding = &binding,
    .out = &actor,
});

ta_actor_deliver(system, actor, &user_message_event);

ta_run_status status;
do {
    ta_actor_resume(system, &(ta_actor_resume_params){
        .actor = actor,
        .budget = 1024,
        .status = &status,
    });
} while (status == TA_RUN_PAUSED);
```

The conversation actor invokes a turn actor; the turn actor invokes inference
actors and routes work to tool-call actors. Once the turn completes and any
transcript append has been acknowledged, the conversation returns to a stable
waiting state.

## Unloading between turns

The address lock below is a host operation shared by inbound routing and the
directory backend. The checkpoint calls belong to the optional durability
adapter, not Tinyactors core.

```c
host_lock_actor(conversation_address);

ta_session_image *image;
ta_actor_capture(system, &(ta_actor_capture_params){
    .actor = actor,
    .out = &image,
});

ta_time next_deadline;
bool has_deadline =
    ta_session_image_next_deadline(image, &next_deadline) == TA_OK;

/*
 * Persist image structure, referenced host values, the binding locator, and
 * next_deadline. The transcript itself already lives in conversation_store.
 */
host_checkpoint_stage(conversation_address, image,
                      conversation_binding_locator,
                      has_deadline, next_deadline);

ta_result result = ta_actor_unload(system, &(ta_actor_unload_params){
    .actor = actor,
    .image = image,
});

if (result == TA_OK) {
    host_checkpoint_commit(conversation_address);
    /* actor is now invalid; the directory contains UNLOADED. */
} else {
    host_checkpoint_discard_staged(conversation_address);
}

ta_session_image_destroy(image);
host_unlock_actor(conversation_address);
```

If capture and unload are separated by an actor mutation,
`ta_actor_unload()` returns `TA_ERR_STALE` and leaves the actor resident. The
host retries from capture. The host must not destroy the image until its durable
encoder has finished with all retained values.

Unloading cancels runtime invocation handles and removes the actor's delayed
sends from the resident timer heap. Their logical records remain in the image.
For the normal between-turn case there should be no active inference invocation;
if there is one, loading later produces the specified
`error.communication.invoke.<id>.unavailable` event.

## Loading for a new message

Inbound routing first attempts the fast resident path:

```c
ta_actor_id actor;
ta_result result = ta_actor_find(system, &(ta_actor_find_params){
    .address = &conversation_address,
    .out = &actor,
});
```

When it returns `TA_ERR_UNLOADED`, the host fences the address and restores the
checkpoint:

```c
host_lock_actor(conversation_address);

/* Another worker may have loaded it before this lock was acquired. */
result = ta_actor_find(system, &(ta_actor_find_params){
    .address = &conversation_address,
    .out = &actor,
});

if (result == TA_ERR_UNLOADED) {
    ta_session_image *image =
        host_checkpoint_decode(conversation_address, allocator);
    ta_value binding =
        host_resolve_conversation_binding(conversation_binding_locator);

    result = ta_actor_load(system, &(ta_actor_load_params){
        .image = image,
        .datamodel_binding = &binding,
        .out = &actor,
    });

    if (result == TA_OK)
        host_checkpoint_mark_loaded(conversation_address);

    ta_session_image_destroy(image);
    ta_value_release(&binding);
}

if (result == TA_OK)
    ta_actor_deliver(system, actor, &user_message_event);

host_unlock_actor(conversation_address);
```

The host then resumes the actor with the normal bounded loop. Load returns a new
runtime actor ID, while `_sessionid`, logical self-address, configuration,
history, queued internal events, and delayed-send semantics come from the image.

If the durable deadline index fires first, the host follows the same load path,
advances logical time, and calls `ta_system_dispatch_due()` instead of initially
delivering a user event.

## Transaction boundary

Core guarantees the in-process directory swap and resident-state transition;
it does not make a database checkpoint and a distributed router atomic. A
durability implementation should use a fenced address record with staged and
committed checkpoint generations. After a crash it decides from that record
whether the resident process, the committed checkpoint, or recovery replay is
authoritative. Stable input and effect IDs prevent duplicate user messages,
transcript appends, tool executions, and streamed terminal responses.
