---
title: Embedding
description: "The C API: contexts, limits, fuel, values, globals, natives, modules, the clock, streaming and inspection."
---

# Embedding

How a C program uses staticjs: what each object is, who owns what, and how
to pass values in and get them out. `include/staticjs.h` is the reference.
Every declaration there has a comment, and this guide does not repeat them
all. Each section names the example in `examples/` that shows it in a whole
program.

## The pieces

Each stage is a state machine that its caller drives. None blocks, reads a
file or a clock, or calls back for more input:

```
source bytes --push--> parser --events--> compiler --> program
program + fuel --push--> evaluator --> value --+--> the value API: kinds, elements, properties, entries
                                                +--> writer --> text
```

| Object | Made by | Ended by | Lifetime and sharing |
|---|---|---|---|
| `sjs_context` | `sjs_context_init` | `sjs_context_destroy` | Owns the allocator that values are made with. Destroy it last, once every value, program and stage made with it is gone. One thread at a time. |
| `sjs_program` | `sjs_compile`, or `sjs_compiler_finish` | `sjs_program_release` | Reference counted (`sjs_program_retain`). Immutable: compile once, evaluate many times, also on several evaluators at once. |
| `sjs_vm` (evaluator) | `sjs_vm_init` | `sjs_vm_destroy` | Runs one evaluation at a time; keep it and start the next evaluation on it. It holds the clock and the random generator. |
| `sjs_value` | the evaluator, or the value API | `sjs_release` | Reference counted, immutable once shared. A value of one context never meets another context. |
| `sjs_writer` | `sjs_writer_init` | `sjs_writer_destroy` | Writes one value as text. |
| `sjs_parser`, `sjs_compiler` | `sjs_parser_init`, `sjs_compiler_init` | `..._destroy` | The two halves of `sjs_compile`, for a source that arrives in pieces. |

Most calls return an `sjs_result`; the evaluator's `sjs_vm_run` returns an
`sjs_run`, and the readers (`sjs_length`, `sjs_array_at`, ...) return what
they read:

| Result | Means |
|---|---|
| `SJS_OK` | done |
| `SJS_NEED_SPACE` | the writer filled the buffer: take its bytes and call again |
| `SJS_ERR_PROGRAM` | the program is not staticjs, or its evaluation faulted: read the stage's `sjs_error` |
| `SJS_ERR_LIMIT` | an `sjs_limits` value was reached |
| `SJS_ERR_NOMEM` | the allocator returned `NULL` |
| `SJS_ERR_NOT_FOUND` | no such name, key, index or error |
| `SJS_ERR_INVALID` | the API was misused: a null handle, a call out of order, a value from another context. A program never causes it. |

## Contexts and allocators

staticjs allocates only through the allocator you give the context: two
functions and a `self` pointer. `free` gets back the size and alignment
that `alloc` was asked for, so an arena or a pool needs no headers of its
own. The library asks for 16-byte alignment.

```c
static void *allocate(void *self, size_t size, size_t alignment);              /* NULL when refused */
static void release(void *self, const sjs_allocator_free_params *params);     /* ptr, size, alignment */
static const sjs_allocator_vtable heap = { allocate, release };

sjs_context *context;
sjs_context_config config = { .allocator = { &my_state, &heap }, .limits = SJS_LIMITS_DEFAULT };
sjs_context_init(&context, &config);
```

An allocator that refuses gives `SJS_ERR_NOMEM` from the call that asked,
or an `SJS_ERROR_MEMORY` error from a stage. Nothing is leaked, and the
context can go on. `sjs_context_live_bytes` gives what the context's
values, programs and stages hold now: 0 when everything is released. That
makes a cheap check for leaks in an embedder's tests. `examples/eval.c`
counts the peak through its allocator.

## Limits

Every construct a program controls has a limit in `sjs_limits`, set per
context. `SJS_LIMITS_DEFAULT` gives:

| Limit | Default | Bounds |
|---|---:|---|
| `max_source_bytes` | 1 MiB | bytes pushed to one parser |
| `max_token_bytes` | 64 KiB | one identifier, number, string or template piece |
| `max_nesting` | 256 | brackets, operators, functions, blocks and ifs open at once |
| `max_lookahead` | 256 | tokens of an arrow function's parameter list, defaults included |
| `max_held_events` | 65,536 | events of a `const`'s pattern held until its value is read |
| `max_stack` | 4,096 | evaluator value slots |
| `max_frames` | 256 | evaluator frames: calls and looping built-ins |
| `max_value_items` | 1,048,576 | elements of an array, object or set; a map's entries count two each |
| `max_string_bytes` | 16 MiB | bytes of one string |
| `max_bigint_bits` | 262,144 | bits of one BigInt (about 79,000 decimal digits) |
| `max_regex_states` | 1,048,576 | states one regular expression's automaton may hold |

A program that reaches a limit is refused, or faults, with an
`SJS_ERROR_LIMIT` error: never a crash, and never a partial value. The
limits bound memory, and fuel bounds an evaluation's time. Lower both for
programs you do not trust.

## Compiling

A program is compiled for a goal:

- `SJS_GOAL_EXPRESSION`: one expression, read as JavaScript reads
  `(` source `)`. Its value is the expression's.
- `SJS_GOAL_MODULE`: imports, `const` declarations and exports. Its value is
  its namespace, an object of its exports with keys sorted as JavaScript
  sorts a namespace's.

```c
sjs_program *program;
sjs_error error = { 0 };
sjs_compile_params params = {
    .compiler = { .context = context, .goal = SJS_GOAL_EXPRESSION,
                  .globals = names, .global_count = 2,       /* optional: see Globals */
                  .modules = { &state, &modules_vtable } },  /* optional: see Modules */
    .source = { text, length },
    .error = &error,                                         /* optional */
};
sjs_result r = sjs_compile(&params, &program);
if (r == SJS_ERR_PROGRAM) {
    /* error.code, error.at.line, error.at.column, error.message */
} else if (r != SJS_OK) {
    /* SJS_ERR_NOMEM or SJS_ERR_INVALID: no program, and error is not filled */
}
```

An `SJS_ERR_PROGRAM` result fills `error`, whose code is one of:

| `error.code` | Means |
|---|---|
| `SJS_ERROR_SYNTAX` | not JavaScript: JavaScript refuses it too |
| `SJS_ERROR_UNSUPPORTED` | JavaScript that staticjs leaves out: a loop, `var`, `this`, an assignment where staticjs has none, a lookbehind |
| `SJS_ERROR_NAME` | a name bound nowhere or bound twice, a module the host does not have, an export the module does not have |
| `SJS_ERROR_LIMIT` | a limit was reached |
| `SJS_ERROR_MEMORY` | the allocator refused |

`error.at` is where the error is: the offset in bytes, and the line and
column, counted from 1, the column in bytes. `error.message` is a fixed
English phrase owned by the library. `error.subject` names the token or
name concerned, when there is one. It points into the stage that reported
the error and is valid only until that stage is called again or
destroyed. `sjs_compile` destroys its parser and compiler before it
returns, so do not read `subject` from its error. To report it (`this name
is not bound (rows)`), compile with the parser and the compiler yourself
(see Streaming), and read `sjs_compiler_error` or `sjs_parser_error` before
destroying them.

## Evaluating, on fuel

Fuel is the evaluator's measure of work: one unit per instruction, and per
element for the built-ins that loop. You push it: `sjs_vm_run(vm, fuel)`
runs until the program is done, until it faults, or until it has used the
fuel it was given, and then returns. An evaluation that ran out of fuel is
paused, and the next `sjs_vm_run` resumes it where it stopped.

The fuel pushed in total bounds the work of an evaluation, and the same
program on the same inputs uses the same fuel on every machine. Each call
does a bounded piece of work and returns, so an evaluation shares its
thread with other evaluations and with an event loop.


```c
sjs_vm *vm;
sjs_vm_init(&vm, &(sjs_vm_config){ .context = context });

sjs_vm_start(vm, &(sjs_run_params){ .program = program, .globals = values, .global_count = 2 });
sjs_run status;
while ((status = sjs_vm_run(vm, 1000)) == SJS_RUN_PAUSED) {
    if (sjs_vm_fuel_used(vm) > budget) { sjs_vm_reset(vm); break; }   /* give up */
    /* other work */
}
if (status == SJS_RUN_DONE) {
    sjs_value result;
    sjs_vm_take_result(vm, &result);       /* yours: sjs_release it */
} else if (status == SJS_RUN_FAULT) {
    sjs_error error;
    sjs_vm_error(vm, &error);              /* error.subject is valid until the next call on vm */
}
```

- **Every evaluation ends.** There are no loops but the built-ins over
  collections, and code jumps only forward. A function cannot call itself,
  and a function that is already running cannot be entered again. Fuel
  bounds the time an evaluation takes, not whether it ends.
- **A pause costs nothing.** An evaluation stops anywhere, inside a
  callback of `map` or `reduce` too, and resumes on the next `sjs_vm_run`
  as if it had not stopped. A run may use up to 2 units more than it was
  given.
- **Reuse the evaluator.** Start the next evaluation on the same `sjs_vm`
  once you have taken the result; `sjs_vm_reset` abandons a paused one and
  releases what it holds. One evaluator runs one evaluation at a time; a
  program may run on several evaluators at once.
- **How much fuel.** A filter such as `event.f > 25`, with `event` a
  global, takes 5 units, and a callback over an array a few units per
  element: `xs.map((x) => x * 2)` over 10 numbers takes 55. `examples/eval.c --stats` prints what a program
  used; [Performance](./performance.md) has more costs.

`examples/stream.c` runs a module in slices of 500 units.

### Fuel in an event loop

One slice per turn of an event loop leaves the loop its timers and
sockets in between. With libuv, an idle handle calls back once per turn:

```c
typedef struct {
    uv_idle_t idle;
    sjs_vm *vm;
    uint64_t budget;
} evaluation;

static void slice(uv_idle_t *idle) {
    evaluation *e = idle->data;
    sjs_run status = sjs_vm_run(e->vm, 10000);
    if (status == SJS_RUN_PAUSED && sjs_vm_fuel_used(e->vm) < e->budget) return;   /* again next turn */
    uv_idle_stop(idle);
    finish(e, status);   /* take the result or the error, or sjs_vm_reset past the budget */
}

/* after sjs_vm_start(e->vm, ...) */
uv_idle_init(loop, &e->idle);
e->idle.data = e;
uv_idle_start(&e->idle, slice);
```

Each evaluation in flight needs an evaluator of its own. To bound a turn
by time, run small slices until it is up, as [the wasm guide](./wasm.md#fuel-and-the-event-loop) does in
JavaScript.

## Values

An `sjs_value` is a small struct. Its `kind` is one of `SJS_UNDEFINED`,
`SJS_NULL`, `SJS_BOOLEAN` (`value.boolean`), `SJS_NUMBER`
(`value.as.number`), `SJS_STRING`, `SJS_BIGINT`, `SJS_ARRAY`, `SJS_OBJECT`,
`SJS_SET`, `SJS_MAP`, `SJS_ITERATOR`, `SJS_FUNCTION`, `SJS_REGEXP` and
`SJS_TEMPORAL` (a Temporal value or a Date). Strings and the kinds after
them point to a reference-counted block.

**Ownership.** Copying the struct does not retain the value. Who holds a
reference:

| You get a value from | It is |
|---|---|
| `sjs_vm_take_result`, `sjs_string_new`, `sjs_bigint_new`, `sjs_array_new`, `sjs_object_new`, `sjs_set_new`, `sjs_map_new`, `sjs_function_native` | yours: `sjs_release` it |
| `sjs_array_at`, `sjs_set_at`, `sjs_object_at`, `sjs_object_get`, `sjs_map_at`, `sjs_map_get` | borrowed, valid while its container is: `sjs_retain` it to keep it longer |
| a native's `call->arguments` | borrowed for the call |

| You give a value to | It |
|---|---|
| `sjs_array_append`, `sjs_object_put`, `sjs_set_add`, `sjs_map_set` | takes over your reference (on an error, you keep it) |
| `sjs_run_params.globals` | borrows it until the evaluation ends |
| a native's `call->result` | takes over your reference: `sjs_retain` a borrowed value before returning it |
| a modules adapter's `find` | borrows the exports while the compiler runs; the program retains what it imports |
| `sjs_writer_config.value` | borrows it until the writer is destroyed |

`sjs_release` and `sjs_retain` never recurse, however deeply a value
nests.

**Building values.** A new array, object, set or map can be added to while
you hold its only reference:

```c
sjs_value event, name;
sjs_object_new(context, 2, &event);
sjs_string_new(context, SJS_STR("reading"), &name);                 /* UTF-8, copied */
sjs_object_put(context, &event, &(sjs_property){ SJS_STR("name"), name });
sjs_object_put(context, &event, &(sjs_property){ SJS_STR("f"), sjs_number(79) });
```

`sjs_object_put` sets a key as JavaScript does on a fresh object: a key that
is already there keeps its place, and keys that are array indices come
first. It refuses the key `__proto__` with `SJS_ERR_INVALID`. A set takes primitives only. A map's key is
any primitive, compared as SameValueZero. `sjs_bigint_new` reads a BigInt's
text as `BigInt(text)` does.

**Reading values.** `sjs_length` gives the elements of an array, the
properties of an object, the members of a set or the entries of a map.
`sjs_array_at`, `sjs_object_at` (in property order), `sjs_object_get` (by
key), `sjs_set_at` and `sjs_map_at` (in insertion order) and `sjs_map_get`
read them. `sjs_string_text` gives a string's UTF-8. `sjs_bigint_text`
writes a BigInt's decimal digits into an `sjs_buffer` of at least
`sjs_bigint_text_bound` bytes. A value a program froze or sealed refuses
to be added to (`SJS_ERR_INVALID`), as JavaScript would refuse it.

## Writing values as text

The writer writes a value as JavaScript source that evaluates to an equal
value. It writes JSON where JSON can say it, and otherwise `undefined`,
`NaN`, `-0`, `10n`, `new Set([...])`, `new Map([[k, v], ...])`,
`[...].values()` for an iterator, and
`Temporal.PlainDate.from("2024-01-31")` or
`new Date("2024-01-31T00:00:00.000Z")`. It fills a buffer of any size and
resumes where it stopped:

```c
sjs_writer *writer;
sjs_writer_init(&writer, &(sjs_writer_config){ .context = context, .value = value,
                                               .function_text = SJS_STR("<function>") });
uint8_t bytes[4096];
sjs_result r;
do {
    sjs_buffer buffer = { bytes, sizeof bytes, 0 };
    r = sjs_writer_write(writer, &buffer);
    consume(bytes, buffer.length);
} while (r == SJS_NEED_SPACE);
sjs_writer_destroy(writer);
```

A function has no source to write: without `function_text`, the writer
refuses one with `SJS_ERR_INVALID`.

## Globals

A program reads the host's data through *globals*: names given to the
compiler, and values given for each evaluation, in the same order. Compile
once, evaluate with new values each time:

```c
static const sjs_str names[] = { SJS_STR("event"), SJS_STR("limit") };
sjs_compile_params params = {
    .compiler = { .context = context, .goal = SJS_GOAL_EXPRESSION, .globals = names, .global_count = 2 },
    .source = SJS_STR("event.data.f > limit"),
};
/* ... for each event: */
sjs_value values[] = { event, sjs_number(25) };
sjs_vm_start(vm, &(sjs_run_params){ program, values, 2 });
```

A global is a value like any other. A program cannot change it: a method
that would change it faults, as the host holds it too. A function a
program makes keeps the globals of the evaluation that made it, as a
JavaScript closure keeps its bindings: a function returned as the result
and passed to a later evaluation, with other values for those names or
none, still reads the values it was made with. Such a function holds those
globals, and their values stay alive while it does. A name the program
reads that is neither a global, nor declared, nor a built-in is refused
when compiled, never at run time.

## Native functions

A native function is the one way a program calls into the host: a lookup
in the host's tables for a user's plugin, or a tool for a program an agent
wrote. Make one with `sjs_function_native` and pass it as a global:

```c
/* rate(currency): one unit of a currency, in euros, from the host's table. */
static sjs_result rate(void *self, sjs_native_call *call) {
    const rate_row *row = self;
    if (call->count == 1 && call->arguments[0].kind == SJS_STRING) {
        sjs_str code = sjs_string_text(call->arguments[0]);
        for (size_t i = 0; i < rate_count; i++) {
            if (strlen(row[i].currency) == code.length && memcmp(row[i].currency, code.ptr, code.length) == 0) {
                call->result = sjs_number(row[i].euros);
                return SJS_OK;
            }
        }
    }
    call->fault = SJS_STR("rate() takes a currency the host has a rate for");
    return SJS_ERR_PROGRAM;                      /* faults the evaluation */
}
static const sjs_native_vtable rate_vtable = { rate };

sjs_value rate_fn;
sjs_function_native(context, (sjs_native){ (void *)rates, &rate_vtable }, &rate_fn);
```

A native is called synchronously and must return quickly: fuel does not
count its time. It must not change anything a program can see. The library
cannot check that rule, so a native that keeps it keeps the program
deterministic. Its arguments are borrowed. Never add to one, even one with
a single reference. A native that returns `SJS_ERR_PROGRAM` faults the
evaluation: the error's message is "a native function failed", and its
`subject` is the native's `fault`. `SJS_ERR_NOMEM` and `SJS_ERR_LIMIT`
fault it as running out of memory or reaching a limit, and any other result
as "the evaluation failed".

## Modules

A module imports another by name, through a modules adapter you give the
compiler. The adapter answers with the exports of a module that has already
been compiled and evaluated, so imports cannot form a cycle:

```c
static sjs_result find_module(void *self, sjs_str name, sjs_value *exports) {
    struct my_modules *m = self;
    /* look `name` up; SJS_ERR_NOT_FOUND when there is none */
    *exports = found;                      /* borrowed: the program retains what it imports */
    return SJS_OK;
}
static const sjs_modules_vtable modules_vtable = { find_module };
```

Evaluating a module gives its namespace, which is what `find` answers for
it later. `find` may also answer with an object you built (`sjs_object_new`),
in any order: the compiler sorts it into a namespace. The exports must be
an object, and one with an array-index key or a `__proto__` key is refused
as unsupported. Any result of `find` other than `SJS_OK`, `SJS_ERR_NOMEM`
included, refuses the import as a name error ("there is no module of this
name"). A namespace is a value. To call a function a module exports, read
it from the namespace (`sjs_object_get`) and pass it to an expression as a
global (`celsius(event.data.f) > limit`). There is no call from C into a
staticjs function: a program calls it.

## The clock and the random seed

The library reads no clock and no entropy. `Temporal.Now`, `Date.now()` and
`new Date()` read a clock the host pushes into the evaluator, and
`Math.random` draws from a seed the host sets. A program that reads either
before it is set faults.

```c
sjs_vm_set_clock(vm, &(sjs_epoch){ .seconds = 1706695200, .nanoseconds = 0 });  /* 2024-01-31T10:00Z */
sjs_vm_advance_clock(vm, 90ull * 60 * 1000000000);                           /* 90 minutes on */
sjs_vm_set_seed(vm, 7);   /* Math.random gives what `node --random-seed=7` gives */
```

The clock does not move while a program runs: two reads with no call
between them agree. Both stay set across evaluations, pauses and
`sjs_vm_reset`, so a second evaluation continues the first one's random
sequence. Set the seed before each `sjs_vm_start` to make every evaluation
draw the same values. To move a paused evaluation elsewhere, save and
restore both with `sjs_vm_clock` and `sjs_vm_set_clock`, and `sjs_vm_random`
and `sjs_vm_set_random`.

## Streaming: the parser, the compiler and sinks

`sjs_compile` is a parser feeding a compiler. Drive them yourself to compile
a source as it arrives, or to see its syntax:

```c
sjs_compiler *compiler;
sjs_compiler_init(&compiler, &(sjs_compiler_config){ .context = context, .goal = SJS_GOAL_MODULE });
sjs_parser *parser;
sjs_parser_config pc = { allocator, limits, SJS_GOAL_MODULE, sjs_compiler_sink(compiler) };
sjs_parser_init(&parser, &pc);
sjs_result r = SJS_OK;
while (r == SJS_OK && more) r = sjs_parser_push(parser, next_chunk());   /* any size, down to one byte */
if (r == SJS_OK) r = sjs_parser_finish(parser);
sjs_program *program;
sjs_compiler_finish(compiler, &program);
sjs_parser_destroy(parser);
sjs_compiler_destroy(compiler);
```

The parser takes its allocator and limits from its own config, not from
the context: give it the allocator and the limits you gave the context.
`sjs_compile` does so for you. The parser's memory comes from that
allocator directly, so `sjs_context_live_bytes` does not count it.

The parser sends a program to a *sink* as a stream of events, in evaluation
order: operands before their operator, a `const`'s value before its
pattern. The compiler is a sink. Your own sink can count, lint or format,
and pass each event on to the compiler. Each event names its syntax node
(`node`) and where that node starts (`start`), so a sink can tell which
subtree an event closes without counting events. The header lists every
event and its order. When `sjs_parser_push` or `sjs_parser_finish` returns
`SJS_ERR_PROGRAM`, the compiler may have refused an event: ask
`sjs_compiler_error` first, then `sjs_parser_error`.

`examples/stream.c` pushes a module in 7-byte chunks through a counting
sink.

## Inspecting an evaluation

A paused evaluation can say where it is, and a compiled program what it
holds: for a debugger, a profiler, a step-by-step view, or a host that
reports where a program ran out of fuel. Every inspection call reads on
demand. An evaluation that nobody inspects pays nothing for them.

```c
/* Where is it? Frame 0 is the program, the last the innermost. */
for (uint32_t i = 0; i < sjs_vm_frame_count(vm); i++) {
    sjs_frame frame;
    sjs_vm_frame(vm, i, &frame);
    if (frame.kind == SJS_FRAME_LOOP)          /* map, reduce, Array.from, ... */
        printf("%u:%u %.*s, element %u of %u\n", frame.at.line, frame.at.column,
               (int)frame.name.length, frame.name.ptr, frame.index, frame.length);
    else                                       /* code: the next instruction, or the call it waits on */
        printf("%u:%u\n", frame.at.line, frame.at.column);
}
```

- **Frames** exist while an evaluation is paused, and after a fault, where
  they show where it faulted, until the next `sjs_vm_start` or
  `sjs_vm_reset`. A function frame gives the source position of the
  instruction it runs next, or of the call it waits on. A loop frame gives
  where its built-in was called, the built-in's name (`"map"`, or
  `"Array"` and `"from"`), and, for a loop that passes over its elements in
  order, how many it has passed of how many.
- **Functions:** `sjs_program_function` gives each function of a program,
  its main code first: where its code begins, its parameters, and the shape
  the compiler tagged it with (`"PLUCK"` for `x => x.key`), or none. A
  callback with a shape runs in C ([Performance](./performance.md#shapes-callbacks-run-in-c)).

Run on one unit of fuel at a time, and the frames trace an evaluation step
by step.

## Threads

A context, and everything made with it, is used by one thread at a time.
The library has no global mutable state, so separate contexts can run on
separate threads with no locking.

## Examples

| Example | Shows |
|---|---|
| `examples/hello.c` | the smallest whole program |
| `examples/eval.c` | a command-line evaluator: error reports, fuel, the clock, the seed |
| `examples/host.c` | a library module, a user's plugin compiled once and run per record, globals, a native, a module imported by name, the clock |
| `examples/stream.c` | chunks pushed to the parser, a sink before the compiler, slices of fuel, a small output buffer |
| `examples/wasm/` | the same from JavaScript, through `staticjs.wasm` |
