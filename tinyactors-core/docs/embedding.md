# Embedding the core

**Status:** Current (2026-10-13), the design after
[ADR 0012](../../docs/adr/0012-lifetimes-follow-the-loop.md).

How to use the core from C: the loop, the inputs, the effects and the
ack, the control plane, images, values, memory, threads, and what an
embedder must and must not do. It is for every C embedder: a game, a
server, a database extension, and every language binding, whose guide
([bindings.md](bindings.md)) builds on this page. The contract of each
call, one row per function, is in [boundary.md](boundary.md#the-contract-per-call);
the headers (`include/tinyactors/host.h`, `datamodel.h`, `cbor.h`,
`types.h`) are the reference.

## 1 · The pieces

- **A compiled image.** `ta_compile_charts` or `ta_compile_system`
  (`compile.h`) turn SCXML into `.tai` bytes (`ta_compile_image`); the
  `tac` command line tool does the same offline.
- **A host** (`host.h`): many systems as one actor system, one per image
  version. `ta_host_create` takes a config as one CBOR map
  ([host-config.md](host-config.md)) and an environment struct: the
  allocator, the datamodels, a trace policy, an error buffer.
- **Datamodels:** the ECMAScript one (`datamodel_ecmascript.h`, quickjs),
  or your own behind the vtable (`datamodel.h`), at most eight, each
  chosen by the chart's `datamodel` attribute.
- **Your loop:** inputs in, effects out. The host does no I/O, has no
  clock and starts no thread.

Static `src` references are resolved during compilation through
`ta_compile_options.resolve`. Its request lends the compiler's scratch
arena until the compile call returns. A resolver fills `ta_resolve_result`:
`source` on success; `error` with line, column and a message on refusal.
It may also supply `source.name` for the diagnostic's canonical filename.
The compiler copies diagnostics into the result, so they remain readable
until `ta_compile_free`. See [the resolver contract](boundary.md#12-compiling-and-the-resolver)
for view lifetimes and defaults. A host has no compile-time resolver.

## 2 · The loop

Lifetimes follow the loop: **take, handle, ack, run**. The ack means "I
have fully handled everything up until now"; everything the host handed
you about the batch ends there.

```
on wake (a timer fired, input arrived, I/O completed):
    feed inputs: ta_host_send / _send_many / _spawn / _terminate /
                 ta_host_resume / ta_host_post
    drain(host)                            # what those calls appended
    ta_host_advance_time(host, {monotonic, wall_ns})
    repeat:
        run = ta_host_run(host, budget)    # refused while a batch is open
        drain(host)                        # the run's effects, then its notifications
        TA_RUN_EFFECTS      -> senders waited for the batch drain acked: run again now
        TA_RUN_EFFECTS_FULL -> run again now
        TA_RUN_YIELDED      -> let the rest of the program run, then again
        TA_RUN_EXHAUSTED    -> the allocator refused (detail: the pool); again when it has memory
        TA_RUN_FAULT        -> report it (detail: the status)
        TA_RUN_IDLE         -> arm one timer at run.next_deadline (UINT64_MAX: none)

drain(host):
    while (n = ta_host_effects(host, &effects)) > 0:
        for i in 0 ..< n: handle(i)        # calls into the host are fine here
        ta_host_effects_ack(host)
```

- **One batch at a time.** A take with no batch open opens one with
  everything pending. While it is open, a take returns the same batch:
  effects your handlers cause (a reply to a message you post, a
  deliver's `PEER_ERROR`) are pending and come with the next take, after
  the ack. An empty take opens no batch.
- **Ack before you run.** `ta_host_run` while a batch is open answers
  `TA_RUN_FAULT` with `TA_ERROR_STATE` and runs nothing, with `used` 0 and `next_deadline` `UINT64_MAX`.
- **One timer.** `next_deadline` holds every deadline the host has:
  timers and delayed sends, migration timeouts, a trace watch's end, the
  encoder's flush.
- **Both clocks in one call.** `monotonic` in ticks at the config's
  `system.ticksPerSecond` (default 1e9, nanoseconds: pair it with
  `ta_runtime_now_ns`), and `wall_ns`, nanoseconds since the Unix epoch,
  that span times are stamped with. Call it before every run.
- **Bound each turn** with the budget, and let the rest of the program
  run between turns.

## 3 · Inputs

`ta_host_spawn`, `ta_host_send`, `ta_host_send_many`,
`ta_host_terminate`, `ta_host_resume` take C structs
(`ta_spawn`, `ta_event_in`, `ta_resume`) and **read them
during the call**: core copies the views (names, sendids, origins,
traceparents) before it returns. A payload handle in an input moves to
the host on success and stays yours on failure. Batching means
deferring the run: several inputs, then one `ta_host_run`.

Each text core keeps is at most `TA_SYSTEM_MAX_STRING` bytes (4087,
`system.h`): an event's name, sendid, origin, origintype and raw
message, and a spawned actor's name. A longer one is refused with
`TA_ERROR_INVALID` before anything is allocated, and no growth of the
segment makes room for it; a traceparent is parsed, not kept, so it has
no limit. A transport that passes text from the network either checks
the length or reports the refusal as any other. A chart's expression
that gives a longer string where core keeps it (`<send>`'s `eventexpr`
and `targetexpr`, `<cancel>`'s `sendidexpr`, `<invoke>`'s `typeexpr`
and `srcexpr`) raises `error.execution`. A `<log>` text that is longer
comes out cut to the longest text that fits, before a UTF-8 character
(the `TA_EFFECT_LOG` and its trace record alike); the chart goes on. Data
is not text: a payload belongs to the datamodel and has no such limit.

During a run, a datamodel's code may call spawn, send, send_many and
terminate (`host.h:151-170`): they take the paths of an internal
`<invoke>`, `<send>` and cancel, nothing grows, and a full pool is
`TA_ERROR_NO_MEMORY`. The same holds while core is inside a datamodel
outside a run (a post, hibernate, the ack's `release`). Lookups answer there; everything else is refused.

## 4 · Effects and the ack

`ta_host_effects(host, &effects)` returns the open batch; read each
effect through the accessors by index and field id
(`ta_host_effect_number`, `_text`, `_bytes`, `TA_EFFECT_FIELD_*`), or
from the `ta_host_effect` records directly in C. Every field, payload
handle, string, `WIRE` byte and `TRACEPARENT` text is valid until
`ta_host_effects_ack`.

| Effect | What you do | Answer with |
|---|---|---|
| `TA_EFFECT_SEND` | an I/O processor's send, numbered as `_ioprocessors`: `IO_PROCESSOR` `i + 1` is the config's `ioProcessors[i]`; 0, the SCXML processor, is a send to another system, for your transport; `TA_NIL` is a send to a service you run, keyed by (`SEGMENT`, `TOKEN`) | its reply events (`ta_host_send`), or `TA_EVENT_IN_ERROR_COMMUNICATION` with the `sendid`; its sender waits until the ack, so what you give before it comes first |
| `TA_EFFECT_INVOKE`, `TA_EFFECT_UNINVOKE` | start or stop the service of type `EVENT` with src `TARGET`, keyed by (`SEGMENT`, `TOKEN`); an `INVOKE` flagged `TA_EFFECT_FLAG_RESTORED` comes from a restored host's first run, for an invocation that was running at the snapshot: start the service again, or a stand-in | events with `TA_EVENT_IN_FROM_INVOKE` and the token; `TA_EVENT_IN_INVOKE_DONE` at its end; its failure (it cannot start, it crashed) `TA_EVENT_IN_FROM_INVOKE \| TA_EVENT_IN_ERROR_COMMUNICATION`: the invoker gets `error.communication`, and the token's later events are refused |
| `TA_EFFECT_REQUEST` | a fetch at run time (it stays, 2026-10-13): an `<invoke srcexpr>` whose name (`AUXILIARY`) core normalized and found no chart for in the requesting segment's image. The fetching is built out later (boundary.md section 12; `ROADMAP.md`, "Decided later"). Not the control plane | `ta_host_resume` with `TA_ERROR_INVALID` until then (the parent gets `error.execution`): a resume names only a chart of that image, where core has already looked |
| `TA_EFFECT_WAKE` | load the session's hibernation blob: an event or a timer came for it (`MIGRATION` 0), or a migration asks for it (`MIGRATION` its id), again when it finds the actor hibernated after a wake | `ta_host_wake` |
| `TA_EFFECT_LOG` | a `<log>`: label `AUXILIARY`, text `EVENT`; to your logging | nothing |
| `TA_EFFECT_DONE`, `TA_EFFECT_ERROR` | a root actor finished or faulted: release what you keep for the session | nothing |
| `TA_HOST_EFFECT_TRACES`, `_LOGS` | take the OTLP request (`ta_host_traces`, `ta_host_logs`) and post it | nothing; the next comes after a take |
| `TA_HOST_EFFECT_REPLY`, `_NOTIFICATION` | the control plane (section 5) | nothing |
| `TA_HOST_EFFECT_MIGRATED`, `_MIGRATION_DONE`, `_SEGMENT_FREED` | a migration's progress and end (`ta_host_migration_failed`, `ta_host_migration_pending`), a retired version | nothing |

**While handling, call the host as you need:** take traces, post,
send, hibernate. What those calls hand you lives
until the ack too, however many calls come between. What they append
waits for the next take. An output taken with no batch open (metrics on
a timer) opens a batch of its own: ack it before the next take or run.

## 5 · The control plane

Everything rarely called (stats, the effective config, inspection, state
values, trace configuration and watches, decoding spans)
is a method posted with `ta_host_post(host, message)`: a JSON-RPC 2.0
message without `"jsonrpc"`, in CBOR, `{"method", "params", "id"}`, read
during the call. The answer is an effect, `TA_HOST_EFFECT_REPLY`, the last
one the post appends, with `WIRE` `{"id", "result"}` or `{"id", "error":
{"code", "message", "data": {"reason", "path"}}}`. Match the reply to the
request by its `id`; it comes with the next take (in the same batch as
anything the method appended itself, before the reply).

The host's notifications are `TA_HOST_EFFECT_NOTIFICATION` effects
(`{"method", "params"}`); none is defined. Post `actor.state` for a
session's state. Drop a notification whose method you do not know.
[requests.md](requests.md) lists every method and notification.

There is no synchronous request: post, then read the reply in a later
drain. `TA_ERROR_STATE` from `ta_host_post` means a run or a datamodel
call is in progress; `TA_ERROR_NO_MEMORY` means nothing was applied.

## 6 · Images

`ta_host_add_image(host, bytes, &segment)` reads the `.tai` bytes during
the call and keeps a copy (ADR 0011 decision 7): free or reuse yours when
it returns (`ta_compile_free` a compile result at once). Adding an image
whose charts have names the host already runs makes it their current
version; new actors start there, and `ta_host_migrate` moves the old
ones. A segment whose charts all have newer versions and no actors left
is freed with its copy (`SEGMENT_FREED`). `ta_host_boot` spawns the
actors a system image declares.

## 7 · Values

- **Event data is a payload handle** (`ta_payload`), owned by the
  datamodel that made it: the ECMAScript datamodel's
  (`ta_datamodel_ecmascript_payload_eval`, `_payload_from_cbor`) or your
  own. Core hands a payload only to the datamodel whose index it carries;
  it never translates values between datamodels.
- **Neutral values are CBOR** ([protocol.md, Values](protocol.md#values)):
  what crosses to another system or into a trace. A datamodel writes them
  with `to_wire` and reads them with `from_wire`; the ECMAScript datamodel
  offers both without the vtable (`ta_datamodel_ecmascript_payload_to_cbor`,
  `_from_cbor`).
- **CBOR in C:** write with `ta_cbor_writer` (a room on your stack that
  grows through an allocator you pass, such as a `ta_growable_arena`'s
  view, section 8), read with `ta_cbor_reader` (a
  cursor), or index with `ta_cbor_decode` (`cbor.h`). Configs, posted
  messages, replies and notifications are CBOR; the calls made
  per event take C structs.

## 8 · Memory and allocators

- **The host gets every byte from `environment->allocator`**
  (`allocator.h`; `ta_runtime_malloc()` is the C library's) and gives
  every block back at `ta_host_free`. Requests are aligned to at most 8.
  When the allocator refuses, a call returns `TA_ERROR_NO_MEMORY` with the
  host as it was, and a run returns `TA_RUN_EXHAUSTED` and goes on where
  it stopped at the next: a bounded allocator is backpressure.
- **Nothing grows during a run.** The host grows its blocks between runs;
  below the host, the system never allocates after `ta_system_init`.
- **What the host hands out lives until the ack** (effects, their fields,
  traces, logs, metrics, hibernation blobs). The
  host holds a batch's outputs until then, so acking often bounds what it
  holds. Copy what you keep past the ack.
- **Snapshots and a refused config** are written into your `ta_buffer`;
  on `TA_ERROR_NO_MEMORY`, `length` is what it needs.
- **A datamodel's outputs** (`to_wire`, `serialize`, `payload_serialize`,
  `eval_string`, `log`) are views it owns until its next call; core copies
  what it keeps.
- **Base APIs you may use** (ADR 0012, decision 5), freestanding
  and allocating only through the allocator you give them
  ([boundary.md, section 11](boundary.md#11-base-apis-for-embedders-and-bindings)):
  - `ta_table` (`table.h`) for your own tables of what core names by
    integer (sessions, payloads of your datamodel): rows of your size,
    64-bit handles with a 31-bit generation, so a stale handle is
    refused; grown by doubling, so a row pointer is valid until the next
    `ta_table_add`: keep handles, not pointers. `ta_table_snapshot` and
    `ta_table_restore` save and restore a table's handle namespace for a
    host snapshot, whose regions name your rows by handle.
  - `ta_growable_arena` (`arena.h`) for what a call of yours converts:
    linked chunks, so an allocation never moves; a mark at your call's
    entry and a release to it at its exit, which nests; released chunks
    are kept, so after warm-up nothing reaches your allocator; and a
    `ta_allocator` view, so a `ta_cbor_writer` grows into it.
  - `ta_status_name` names a status for your messages (NULL for one
    `types.h` does not define).
  Their per-call contracts and a writer example are in
  [Base storage](base-storage.md). The headers state when row pointers,
  allocation pointers, marks and borrowed allocator objects expire.

## 9 · Threads

A host is used from the thread that made it, and only from there. It
starts no thread and takes no lock. Parallelism is shared-nothing:
several hosts, one per thread, each with a transport of yours: a
`TA_EFFECT_SEND` to another system's address (`io_processor`
0; fail it when you have no transport) goes
through your own queue to the thread that owns that system, which
resolves the target (`ta_host_resolve`) and calls `ta_host_send` with the
data made a payload of its datamodel and the origin
(`TA_EFFECT_FIELD_ORIGIN`); see [protocol.md](protocol.md#other-systems). A producer that cannot call the host (an interrupt handler)
writes into a queue of yours that the owning thread drains into the
inputs.

## 10 · What an embedder must and must not do

Must:

- call the host from its owning thread only;
- drain (take, handle, ack) before every run, and ack every batch;
- advance time before every run, with both clocks;
- answer what asks for an answer: a failed send before the ack (its
  sender waits for it), a `REQUEST` with `ta_host_resume`, a `WAKE` with
  `ta_host_wake`;
- release a payload a call refused (it stayed yours);
- copy what it keeps past the ack;
- free the host from the loop, never from a datamodel's code.

Must not:

- run while a batch is open;
- keep a pointer into an effect, an output or a field past the ack;
- read a datamodel's output past that datamodel's next call;
- call the host from another thread, or from a datamodel's code beyond
  the inputs and lookups;
- expect a reply at the call that posted it;
- encode inputs as bytes: the calls made per event take structs
  (`AGENTS.md`, "No serialization on the hot path").

## 11 · A complete loop in C

A chart that greets: `hello` makes it `<send>` `greeted` to the
application's I/O processor and `<log>` a line. Everything below is the
current API.
Error handling is cut to keep it short.

```c
#include <stdio.h>
#include <string.h>
#include <tinyactors/cbor.h>
#include <tinyactors/compile.h>
#include <tinyactors/datamodel_ecmascript.h>
#include <tinyactors/host.h>
#include <tinyactors/runtime.h>

#define APP "urn:example:app" /* our I/O processor, index 0 in "ioProcessors" */

static const char CHART[] =
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' version='1.0' datamodel='ecmascript'"
    " name='greeter' initial='idle'><state id='idle'>"
    "<transition event='hello'><log label='greeter' expr=\"'hello, ' + _event.data.who\"/>"
    "<send type='" APP "' target='out' event='greeted'><content expr='_event.data'/></send>"
    "</transition></state></scxml>";

static void handle(ta_host *host, uint32_t i) {
    switch (ta_host_effect_number(host, i, TA_EFFECT_FIELD_KIND)) {
    case TA_EFFECT_SEND: { /* output to the application, through its I/O processor */
        ta_string_view name = ta_host_effect_text(host, i, TA_EFFECT_FIELD_EVENT);
        printf("app got %.*s\n", (int)name.length, name.pointer);
        break;
    }
    case TA_EFFECT_LOG: { /* to the program's logging */
        ta_string_view text = ta_host_effect_text(host, i, TA_EFFECT_FIELD_EVENT);
        printf("log: %.*s\n", (int)text.length, text.pointer);
        break;
    }
    case TA_HOST_EFFECT_TRACES: { /* valid until the ack, whatever we call meanwhile */
        ta_bytes otlp;
        if (ta_host_traces(host, &otlp) == TA_OK && otlp.length) { /* post it to /v1/traces */ }
        break;
    }
    case TA_HOST_EFFECT_REPLY:        /* WIRE: {"id", "result"} or {"id", "error"} */
    case TA_HOST_EFFECT_NOTIFICATION: /* WIRE: {"method", "params"} */
        printf("control: %zu bytes\n", ta_host_effect_bytes(host, i, TA_EFFECT_FIELD_WIRE).length);
        break;
    default: /* DONE, ERROR: release what we keep for the session; nothing here */
        break;
    }
}

static void drain(ta_host *host) {
    const ta_host_effect *effects;
    uint32_t n;
    while ((n = ta_host_effects(host, &effects)) > 0) {
        for (uint32_t i = 0; i < n; i++)
            handle(host, i);
        ta_host_effects_ack(host); /* everything handed out ends here */
    }
}

int main(void) {
    ta_allocator allocator = ta_runtime_malloc();

    /* the image: compiled, given to the host, freed (the host keeps a copy) */
    ta_compile_options options = {sizeof options, 0, allocator, NULL, NULL, NULL};
    ta_source source = {ta_string("greeter.scxml"), {CHART, sizeof CHART - 1}};
    ta_compile_result *compiled = NULL;
    if (ta_compile_charts(&options, (ta_source_list){&source, 1}, &compiled) != TA_OK) return 1;
    ta_string_view image = ta_compile_image(compiled);

    /* the config: one CBOR map */
    unsigned char room[256];
    ta_cbor_writer writer;
    ta_cbor_writer_init(&writer, (ta_buffer){room, sizeof room, 0}, &allocator);
    ta_cbor_write_map(&writer, 2);
    ta_cbor_write_text(&writer, ta_string("systemID"));
    ta_cbor_write_text(&writer, ta_string("demo"));
    ta_cbor_write_text(&writer, ta_string("ioProcessors"));
    ta_cbor_write_array(&writer, 1);
    ta_cbor_write_map(&writer, 1);
    ta_cbor_write_text(&writer, ta_string("type"));
    ta_cbor_write_text(&writer, ta_string(APP));
    ta_bytes config;
    ta_cbor_writer_bytes(&writer, &config);

    /* the host, with the ECMAScript datamodel */
    ta_datamodel_ecmascript_config ecmascript_config;
    memset(&ecmascript_config, 0, sizeof ecmascript_config);
    ecmascript_config.size = sizeof ecmascript_config;
    ta_datamodel_ecmascript *ecmascript = ta_datamodel_ecmascript_new(&ecmascript_config);
    ta_datamodel_binding datamodels[1] = {ta_datamodel_ecmascript_binding(ecmascript)};
    ta_host_environment environment = {sizeof environment, 1, datamodels, allocator, NULL, NULL};
    ta_host *host = NULL;
    if (ta_host_create(config, &environment, &host) != TA_OK) return 1;
    ta_cbor_writer_free(&writer); /* read during the call */

    uint32_t segment = 0;
    ta_host_add_image(host, (ta_bytes){image.pointer, image.length}, &segment);
    ta_compile_free(compiled);

    /* an actor, and an event for it with data {"who": "world"} */
    ta_host_chart_ref chart;
    ta_host_chart(host, ta_string("greeter"), &chart);
    ta_spawn spawn = {chart.segment, chart.chart, ta_string("greeter-1"), 0, {NULL, 0}};
    uint64_t session = 0;
    ta_host_spawn(host, &spawn, &session);
    ta_event_in hello;
    memset(&hello, 0, sizeof hello);
    hello.size = sizeof hello;
    hello.session = session;
    hello.name = ta_string("hello");
    hello.data = ta_datamodel_ecmascript_payload_eval(ecmascript, 0, ta_string("({who: 'world'})"));
    if (ta_host_send(host, &hello) != TA_OK)
        datamodels[0].vtable->release(datamodels[0].datamodel, hello.data); /* still ours */

    /* the loop, until idle (a real program arms a timer at next_deadline) */
    for (;;) {
        drain(host);
        ta_host_advance_time(host, (ta_host_time){ta_runtime_now_ns(), 0});
        ta_run run = ta_host_run(host, 10000);
        drain(host);
        if (run.status == TA_RUN_IDLE) break;
        if (run.status == TA_RUN_FAULT) {
            const char *name = ta_status_name((ta_status)run.detail);
            fprintf(stderr, "fault: %s\n", name ? name : "unknown");
            break;
        }
        /* EFFECTS: senders waited for the batch handle() acked; EFFECTS_FULL,
         * YIELDED: again; EXHAUSTED: again once the allocator has memory */
    }

    ta_host_free(host);
    ta_datamodel_ecmascript_free(ecmascript); /* after every host using it */
    return 0;
}
```
