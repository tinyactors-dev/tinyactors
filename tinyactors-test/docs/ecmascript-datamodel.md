# `datamodel.ecmascript`: design

Status: implemented (phase 2). This document describes the code in
`src/ecmascript_datamodel.c`, `src/value_codec.c`, and
the `invoker.scxml` adapter in `src/adapters.c`. Section 9 lists where the
implementation differs from the earlier review rounds.

This adds a second data-model adapter to the test host. It is backed by
quickjs-ng (vendored in `vendor/quickjs-ng`, v0.17.0). Its purpose is to run
the W3C SCXML IRP tests that need `datamodel="ecmascript"`: 444, 445, 446,
448, 449, 451, 452, 453, 456, 457, 459, 460, 558, 560, 562, 569, and 578.
Tests 557 and 561 need an XML DOM and stay skipped.

The adapter implements the whole `ta_datamodel_vtable`, and its `name` is
`"ecmascript"`. It is registered as `{"$adapter": "datamodel.ecmascript"}`.
The importer and harness already emit `datamodel: 'ecmascript'` charts and
pass that adapter to `compile_spec`.

**Governing rule.** The jq and ECMAScript data models are fully separate.
Neither adapter knows about the other's values. The RPC codec
(`binding.c` encode/decode) is the only serialization boundary. Each data
model gives the codec its own conversion to and from wire JSON.

---

## 0. Summary of decisions

| Topic | Decision |
|---|---|
| Engine topology | One `JSRuntime` per ECMAScript adapter context (one per host process, in `tat_bind`). One `JSContext` per data-model instance (one per session). One private utility context, used for encoding. |
| `ta_value` | A refcounted wrapper (`tat_js_value`) that holds one QuickJS reference. `retain` and `release` touch only the C count. The last release calls `JS_FreeValueRT`. |
| Lifetime | A value is tied to the runtime, not to its session. It can outlive the context that created it. |
| Sharing | Retain shares the wrapper; nothing is ever copied, and values stay ECMAScript values (ADR 0050). |
| Wire values | Decoded ECMAScript values hold their JSON text. Each session that imports one parses it in its own realm. |
| Serialization | A host-side codec registry: encode is selected by the value's vtable, and decode by the receiving actor's data model. There is no model tag on the wire. |
| Definition → model | Core API: `ta_actor_inspect` gives an actor's `definition_id`, and `ta_definition_inspect` gives the definition's `ta_datamodel` (ADR 0042). The host keeps no map. |
| Mixed models | `invoker.scxml` refuses a child whose model differs from the parent's, which gives `error.execution` in the parent. A child spec without `datamodel` inherits the parent's model. Each adapter, jq included, faults on foreign values. |
| System variables | Non-configurable accessors with C getters and throwing setters. `_event` is cached per event. `In()` is a C function. |
| Memory | QuickJS allocates through the adapter's `ta_allocator`, with a size header. `JS_RunGC` runs on each instance destroy. The context destroy force-frees the remaining wrappers. |

---

## 1. Value representation and lifetime

### 1.1 The wrapper

A `JSValue` is a 16-byte `{union, tag}`, and many JS values have no heap
pointer. Freeing one needs the `JSRuntime *`. For these reasons
`ta_value.self` points at a wrapper:

```c
typedef enum { TAT_JS_LIVE, TAT_JS_WIRE } tat_js_value_kind;

typedef struct tat_js_value {
    tat_js_datamodel_context *adapter;   /* owns the runtime and the wrapper pool */
    struct tat_js_value *prev, *next;    /* live list, for forced teardown (5.4) */
    size_t references;                   /* ta_value refcount */
    tat_js_value_kind kind;
    JSValue value;                       /* LIVE: the JS value. WIRE: a JS string holding JSON text */
    const char *text;                    /* value_text() cache (JS_ToCStringLen), or NULL */
    size_t text_len;
} tat_js_value;
```

- **`retain`** increments `references`.
- **`release`** decrements `references`. At zero it does the following:
  1. frees `text` with `JS_FreeCString(adapter->utility, …)`;
  2. calls `JS_FreeValueRT(adapter->runtime, value)`;
  3. unlinks the wrapper from the live list;
  4. returns the wrapper to the free list. Wrappers come from a `ta_arena`
     plus a free list, as in the jq adapter.
- There is no `clone`: a value is a handle, and `retain`/`release` is the
  whole contract (ADR 0050). Every JS value, including a WIRE JSON string,
  is safe to share within one runtime.

The wrapper has its own refcount. It does not dup the JSValue on every
`ta_value_retain`. This keeps `retain` and `release` cheap and free of
QuickJS calls, and it gives the `value_text` cache a lifetime equal to the
`ta_value`, as `value_text` requires ("valid while it is retained").

A wrapper is identified by its vtable pointer, `&TAT_JS_VALUE_VTABLE`.

### 1.2 LIVE and WIRE values

- **LIVE** values come from evaluation in a session: `eval_value`,
  `make_params`, `import_content`, `inspect`, the foreach snapshot, and so on.
- **WIRE** values come from the codec's decode (2.2). A WIRE value holds the
  JSON text as a QuickJS string, which is realm-free and immutable. When a
  session imports it (3, `tat_js_import`), `JS_ParseJSON` runs **in that
  session's context**. The result is a LIVE value in the right realm, so
  `_event.data instanceof Array` works for data that arrived over RPC.
  Encoding a WIRE value re-emits its text.

This two-kind design is internal to the ECMAScript model. It never involves
jq values.

### 1.3 Who owns what

- **Values belong to the runtime.** In QuickJS, objects live in the runtime's
  heap. A context is a realm: a global object plus intrinsics. Any context of
  the same runtime can read, call, and free an object from another context.
  `JS_FreeValueRT` needs no context, so a release never needs the creating
  session to exist.
- **Creation rule:** everything the adapter materializes for a session
  (imports, `_event`, snapshots, params, `inspect`) is created in that
  session's context. The utility context only reads values, when encoding.

### 1.4 Topology: one runtime, one context per instance

| Option | Verdict |
|---|---|
| **A. One runtime per adapter, one `JSContext` per instance** | **Chosen.** Each session has its own global scope (B.2.2). ECMAScript sessions can share values without conversion. There is one heap and one memory limit. A context costs a few tens of KB. |
| B. One runtime per instance | Rejected. Values cannot cross runtimes, and it would need a refcounted runtime for values that outlive the session. |
| C. One context, a swapped global object per session | Rejected. QuickJS cannot swap a realm's global object. `with` emulation breaks `var` and function semantics. |
| D. One runtime per `ta_system` | Rejected (open question 4). It would need a system→context registry and a system-destroy hook. |

**Context lifetime.** quickjs-ng contexts are refcounted and are collected by
the cycle collector. Every function object, including the `In` function and
the getters, holds `JS_DupContext(realm)`. The consequences are:

- `destroy_instance` calls `JS_SetContextOpaque(ctx, NULL)`, then
  `JS_FreeContext(ctx)`, then `JS_RunGC(rt)`. The realm is reclaimed unless
  one of its functions or objects is still shared elsewhere.
- Every C callback checks the opaque. After its session is destroyed, a
  surviving function sees `In()` throw and the system variables read as
  `undefined`. It never touches a freed instance or `ta_dm_host`.

### 1.5 Known limitation: cross-realm sharing

Because values are shared, an object passed from one ECMAScript session to another
keeps its creator's realm. This happens through `<send>` data to another
session, `#_parent` data, or `done.invoke` data. The receiver sees the
following:

- `x instanceof Array` is `false`, while `Array.isArray(x)` is `true`;
  `instanceof Object` and other constructor checks behave the same way;
- functions run against **their creator's** globals. After the creator's
  session is destroyed, they see an `In()` that throws and `undefined`
  system variables.

Mutations are not visible to the other session: a value leaving a session
is copied (core ADR 0097, `tat_js_copy_out`) with QuickJS's structured
serialization (`JS_WriteObject`, then `JS_ReadObject` in the utility
context), so the receiver gets objects of the utility realm, with the
`instanceof` caveat above. A value that serialization refuses (a function,
or an object holding one) is still shared, and aliases as before.

Mixed-model refusal (section 6) limits sharing to ECMAScript-to-ECMAScript
sessions. Values that cross RPC are WIRE values and do not have this
problem. Values passed through invoke `<param>`s also avoid it, because the
invoker goes through the codec (6.1).

### 1.6 Values that outlive their creator

| Case | What happens |
|---|---|
| Event data queued to another session | A copy made in the utility context (core ADR 0097), retained by the queue and freed through the runtime. The sender's context can already be gone. |
| Delayed send that outlives the sender (submitted record, or `sender == NULL` dispatch) | The record holds the wrapper. Delivery retains it. Only the runtime is needed. |
| Completion data read after the child is destroyed | `ta_actor_get_completion` retains while the child is alive. The wrapper stays valid after the child is gone. RPC encoding (2.1) uses the utility context. |
| A function called after its session died | Its realm stays alive (1.4). `In()` throws. System variables read as `undefined`. |
| A `ta_value` released after the adapter context is destroyed | Not supported, the same as for jq. See 5.4. |

### 1.7 Threading

QuickJS is single-threaded. The core requires external synchronisation (ADR
0017), and the test host is single-threaded. Because the runtime is shared,
all systems that use one ECMAScript adapter context must be driven from one
thread or under one lock. That includes `ta_value` retain and release, which
call into QuickJS. The header documents this, and the adapter adds no locks.

---

## 2. Codec: the only serialization boundary

### 2.1 Registry (`src/value_codec.{c,h}`)

```c
typedef struct {
    const char *name;                                   /* "jq", "ecmascript" */
    bool (*owns)(const ta_value *value);                /* by vtable */
    bool (*encode)(const ta_value *value, jv *out);     /* false: not this model's value */
    ta_result (*decode)(void *model, jv json, ta_value *out);  /* model = ta_datamodel.self */
    const ta_datamodel_vtable *(*datamodel)(void);      /* decode selector */
} tat_value_codec;

jv tat_value_encode(const ta_value *value);            /* {"$value":"foreign"} when no codec owns it */
ta_result tat_value_decode(ta_datamodel model, jv json, ta_value *out);
const tat_value_codec *tat_value_codec_for_value(const ta_value *value);
const tat_value_codec *tat_value_codec_for_model(ta_datamodel model);
ta_result tat_actor_datamodel(ta_system *system, ta_actor_id actor, ta_datamodel *out);
void tat_value_codec_select(tat_bind *bind, const char *method, jv params);
```

The registry is a static table with two entries.

- **jq** wraps the jq adapter's existing public functions
  `tat_json_value_to_jv` and `tat_json_value_from_jv`.
- **ecmascript** wraps two functions that the adapter exposes as **JSON
  text**, not as `jv`:
  - `tat_js_value_to_json`, whose result is freed with
    `tat_js_json_release`;
  - `tat_js_value_from_json`.

  `value_codec.c` converts between that text and `jv`. As a result the
  adapter, and its unit test, do not link jq.

`owns` checks the vtable. Encode finds the owning codec. Decode finds the
codec whose `datamodel()` equals `model.v` and passes it `model.self`.

### 2.2 Encode and decode in `binding.c`

- `tat_encode_ta_value` is `tat_value_encode`: `null` for a null vtable, the
  owning codec's JSON, or `{"$value": "foreign"}`. This covers every value
  the core hands out, such as `ta.actor.datamodel_root`, event, delayed-send,
  and completion data.
- `tat_decode_ta_value` decodes with **`bind->decode_model`**. It is reset
  to the jq model in `tat_bind_begin` and chosen per request by
  `tat_value_codec_select` (2.4).

### 2.3 The ECMAScript conversion

- **Decode (JSON → ECMAScript):** `jv_dump_string(json)` becomes a JS string
  in the utility context (strings are realm-free), wrapped as a WIRE value.
  The decode is lossless. No JavaScript runs.
- **Encode (ECMAScript → JSON):**
  - a WIRE value gives `jv_parse(text)`;
  - a LIVE value is converted in the utility context with
    `JSON.stringify(v, replacer)`, and the result goes through `jv_parse`.
    The replacer maps nested `undefined`, functions, and symbols to `null`,
    so object keys stay present. This matters for `inspect`: a declared but
    unset `Var2` appears as `"Var2": null`.
  - A top-level value that JSON cannot represent is encoded as a **marker**:
    `{"$value": "undefined"}`, `{"$value": "function"}`,
    `{"$value": "symbol"}`, or `{"$value": "unserializable"}`. The last
    covers a cycle, a BigInt, or a throwing `toJSON`/getter.
  - Markers are output-only. Decoding `{"$value": "undefined"}` produces
    that literal object.
  - Other losses follow `JSON.stringify`: `NaN` and `±Infinity` become
    `null`, `-0` becomes `0`, class instances become plain objects, `Date`
    becomes an ISO string, `Map` and `Set` become `{}`, and array holes
    become `null`.
  - Getters and `toJSON` run user code with the interrupt budget (5.3).

### 2.4 How decode finds the receiving actor's data model

The core exposes what the codec needs (ADR 0042). The lookups are:

- **Actor → model** (`tat_actor_datamodel`): `ta_actor_inspect` gives the
  `definition_id`, and `ta_definition_inspect` gives `.datamodel`.
  `ta_actor_inspect` is cheap and never calls the adapter. The `datamodel`
  field only identifies the adapter instance (`self`/`v`); the host never
  calls its operations through it.
- **Definition → model**: `ta_definition_inspect` directly.

`server.c` `tat_dispatch` calls `tat_value_codec_select(bind, method,
params)` after `tat_bind_begin` and before the generated handler. The
function reads the receiver from the **raw JSON params**, so member decode
order does not matter:

| Method | Receiver member | Decoded with its model |
|---|---|---|
| `ta.actor.deliver` | `actor` | `event.data` |
| `ta.actor.report_invoke` | `actor` (the invoking parent) | event, done, and error data |
| `ta.actor.spawn` | `definition` | `datamodel_binding` |
| `ta.actor.prepare_replacement` | `definition` | `datamodel_binding` |
| `ta.actor.load`, `ta.actor.reconstruct` | `image`, through its definition (`ta_session_image_definition`) | `datamodel_binding` |
| everything else, including `codec.roundtrip.*` | none | jq |

If the system handle, id, or lookup is bad, the jq default is kept. Any
decode error is cleared, so the generated handler reports the bad handle
itself.

## 3. Per-operation behaviour

All expressions are `TA_EXPRESSION_SOURCE` ECMAScript text. The instance
(fixed size) is:

```c
typedef struct {
    tat_js_datamodel_context *adapter;
    tat_js_shared *shared;           /* expressions + lazy state-name table */
    JSContext *context;              /* opaque = this instance */
    const ta_dm_host *host;          /* refreshed from params on every call */
    JSValue declared;                /* null-proto object used as an ordered name set, for inspect */
    JSValue bound;                   /* null-proto object: names supplied by the init binding */
    JSValue event;                   /* cached _event, or JS_UNDEFINED */
    tat_js_event_key event_key;      /* 4.2 */
    ta_value event_data;             /* retained data of the cached event */
    JSValue session_id, session_name, ioprocessors;  /* lazy */
} tat_js_instance;
```

**Import** (`tat_js_import(instance, const ta_value *, JSValue *out)`) is used
by every entry point that receives a `ta_value`:

- a null vtable gives `undefined`;
- a LIVE wrapper gives `JS_DupValue`;
- a WIRE wrapper gives `JS_ParseJSON` in the instance context;
- **any other vtable, including jq, is a fault** (6.2).

**Entry and exit.** Each call sets `instance->host = params->host` and resets
the interrupt budget. If QuickJS raised an exception, the adapter calls
`JS_GetException`, frees the exception, and returns `TA_OP_ERROR` with this
fault:

| Field | Value |
|---|---|
| `code` | `TA_ERR_INVALID`. `TA_ERR_NOMEM` for QuickJS OOM and for allocation failures. `TA_ERR_UNSUPPORTED` for foreign values or host expressions. |
| `message` | A static string that names the operation, for example `"ecmascript: assign threw"`. The core does not keep the message. |
| `data` | Empty. This matches jq (question 7). |

No exception stays pending after a call returns.

| Op | Behaviour |
|---|---|
| `prepare_definition` | Rejects host expressions (`TA_ERR_UNSUPPORTED`). Allocates `tat_js_shared {expressions, count, state_ids (lazy)}` from `params->allocator`. |
| `destroy_definition` | Frees `shared` and its state table. |
| `instance_size` / `_alignment` | `sizeof` / `_Alignof(tat_js_instance)`. |
| `init_instance` | See 3.1. |
| `destroy_instance` | 1. Releases `event_data`.<br>2. Frees the cached JSValues.<br>3. Clears the context opaque, calls `JS_FreeContext`, then `JS_RunGC`.<br>4. Decrements `live_instances`. |
| `declare` | 1. A system-variable name (`_event`, `_sessionid`, `_name`, `_ioprocessors`, `_x`) or `In` is a fault (question 11).<br>2. If `bound` has the name, sets `*bound = true` and leaves the value.<br>3. Otherwise defines the global `name = undefined` as writable, enumerable, and non-configurable (`var`-like). A failure on a non-writable builtin is a fault.<br>4. Records the name in `declared`.<br>Data in nested or parallel states becomes a global (test 448). |
| `set_named` | Imports the value, sets the global property (throwing), and records the name in `declared`. |
| `eval_value` | `tat_js_eval_expression` (3.3). The result, which can be `undefined`, is wrapped as LIVE. |
| `eval_condition` | `tat_js_eval_expression` followed by `JS_ToBool`. Side effects run once (444: `++var1==2`). `'foo'` is true (449). |
| `read_location` | The same as `eval_value`. |
| `assign` | Compiles `(function (__tat_assign_value) { "use strict"; <location> = __tat_assign_value; })` as global code in the instance context and calls it with the imported value. Any LHS works (452: `foo.bar`). An undeclared root throws a `ReferenceError` because of strict mode (question 5). Writing to a system variable calls the throwing setter. A bad LHS is a `SyntaxError`. Each of these is a fault. |
| `execute_script` | `JS_Eval(src, JS_EVAL_TYPE_GLOBAL)`. Function declarations and `var`s persist as globals (452 document `<script>`, 456 `Var1+=1`). |
| ~~`decode_scxml_target`~~ | Gone. Core owns the SCXML target URI grammar and parses it from the text `value_text` returns (ADR 0044), so this adapter no longer decodes targets at all. |
| `make_params` | A new object in the instance context. Each `(name, value)` is imported and defined (C, W, E). A later duplicate replaces an earlier one. **Values stay live objects** and are not snapshotted at send time (decided, question 6). Test 560 reads `_event.data.aParam`. |
| `inspect` | Returns an **ECMAScript** LIVE value: a plain object (ordinary `Object.prototype`) with one member per name in `declared`, in declaration order. `declared` comes from `declare`, `set_named`, the binding, and `foreach_bind`. Each member holds the current value (question 2). The codec turns it into JSON for RPC (2.3). Unset members encode as `null`, and `._irp_outcome` is read from that JSON. `inspect` fails only on OOM. |
| `value_text` | Only this model's wrappers are accepted; anything else is a fault. A string gives its UTF-8 text. Any other value gives `ToString(v)`. A WIRE value is imported first. The C string is cached in the wrapper. A symbol is a fault. |
| `text_value` | A JS string in the instance context, wrapped as LIVE. |
| `import_content` | `JS_ParseJSON` in the instance context. On a `SyntaxError`, returns a whitespace-normalized string: runs of `[ \t\r\n]` become one space, and the ends are trimmed. Tests: 446, 558, 562, 578. |
| `foreach_snapshot` | Evaluates the expression. If it is not `JS_IsArray`, that is a fault (457: `Var4 = 7`). Otherwise creates a new array of `length` elements (which must fit in `uint32_t`) in the instance context, with holes as `undefined`. `*count = length`. The shallow copy keeps test 460 stable. |
| `foreach_bind` | 1. Validates `item` and `index_name` (question 10). The name must contain no ASCII character outside `[A-Za-z0-9_$]`, and `var <name>;` must compile (`JS_EVAL_FLAG_COMPILE_ONLY`; the compiled function is freed). This rejects `continue` and `'continue'` (457).<br>2. Sets `item = snapshot[index]` and the index as global properties, creating them when missing (ADR 0033).<br>3. Records both names in `declared`. |

### 3.1 `init_instance`

1. `JS_NewContext(rt)` (NULL gives `TA_ERR_NOMEM`). Set the context opaque to
   the instance and increment `live_instances`.
2. Install `In` and the system variables (section 4).
3. Create `declared` and `bound` as two null-prototype objects.
4. Import `params->binding` when present. It must be an ECMAScript object:
   LIVE, or WIRE from `ta.actor.spawn` or the invoker. A jq value or other
   foreign value returns `TA_ERR_UNSUPPORTED`; a non-object returns
   `TA_ERR_INVALID`. Each own enumerable key becomes a global and is
   recorded in `bound` and `declared`.

This binding is also the hook for reconstruction and replacement (ADR 0038).

### 3.2 `bound` and `declare`

Names that the binding supplies (invoke `<param>`s matched to the child's
`<data>` IDs) make `declare` set `*bound = true`, and the core skips their
initializers.

### 3.3 Expression evaluation

1. Evaluate `"(" + src + "\n)"` as global code.
2. On a **`SyntaxError`** only, evaluate `src` as global code and take its
   completion value. quickjs-ng global scripts return it (`eval_ret_idx`).
   This handles `function(invar){…}` (453, through step 1) and
   `new testobject();` (452, through step 2). A compile error runs no code,
   so the retry cannot repeat side effects.
3. If step 2 also fails, report its exception.

Expressions and scripts run in sloppy mode. Only `assign` uses strict mode.

---

## 4. System variables and `In()`

### 4.1 Installation

`init_instance` does this once per context.

`_event`, `_sessionid`, `_name`, `_ioprocessors`, and `_x` are defined with
`JS_DefinePropertyGetSet` as non-configurable, enumerable accessors. The
getters are C functions, and the setter is a C function that throws
`TypeError("<name> is read-only")`.

- Values are computed on read, so nothing is re-installed per event.
- Assignment throws in both sloppy and strict code, so it becomes
  `error.execution`.
- User code cannot delete or redefine these properties.

`In` is a non-writable, non-configurable C function. `_x` is a frozen empty
object.

### 4.2 `_event`

1. If `host == NULL` or there is no event, the getter returns `undefined`.
2. The key is the event's `name`, `type`, `send_id`, `invoke_id`, `origin`,
   and `data.{self,v}`. If it equals the cached key, the getter returns the
   cached object.
3. Otherwise the getter builds a new object in the instance context:
   - `name`, `type`, `sendid`, `origin`, `origintype`, `invokeid`: from
     `ta_dm_host_text`. `NOT_FOUND` gives `undefined` (B.2.8).
   - `data`: `tat_js_import(event->data)`. A foreign value makes the getter
     throw, which gives `error.execution` for the expression that read it.

   It then freezes the object (shallow), caches it, and **retains
   `event->data`**, so the wrapper cannot be recycled while it is part of
   the cache key.

### 4.3 Constant variables

`_sessionid` and `_name` are strings from the host, and `NOT_FOUND` gives
`undefined`. `_ioprocessors` is
`{scxml: P, "<SCXML_IO_TYPE>": P}` with `P = {location: "<SCXML_IO_LOCATION>"}`,
deep-frozen (569). Each is computed on first read and cached. With a NULL
context opaque, every getter returns `undefined`.

### 4.4 `In(id)`

1. `ToString(id)`.
2. Look the string up in `shared->state_ids`, a sorted `{name, ta_state_id}`
   array. It is built on the first call by iterating
   `ta_dm_host_state_name(host, 0, 1, …)` until `NOT_FOUND`.
3. Return `ta_dm_host_in_state(host, id)`.

An unknown ID or `host == NULL` gives `false`. A NULL opaque throws.
Parallel regions are handled by the host (451).

---

## 5. Memory

### 5.1 Adapter context

The public header (`ecmascript_datamodel.h`) exposes only the following, so
host files never include `quickjs.h`:

```c
typedef struct { ta_allocator allocator; size_t memory_limit, stack_size; uint64_t interrupt_budget; } tat_js_datamodel_config;
typedef struct { tat_js_state *state; } tat_js_datamodel_context;
```

The private `tat_js_state`, allocated from the config allocator, holds:

- the runtime;
- the utility context and the stringify replacer;
- the wrapper arena and free list;
- the live wrapper list;
- `live_instances`;
- the interrupt counter;
- a `TAT_JS_TRACE` flag. When that environment variable is set, every
  exception behind a fault is printed to stderr (debugging aid).

`tat_bind` holds `tat_js_datamodel_context js` next to `json`.

### 5.2 Allocation bridge

`JS_NewRuntime2(&TAT_JS_MALLOC, context)` routes every QuickJS allocation
through the adapter's `ta_allocator`:

- `js_malloc` allocates `n + 16` bytes at alignment 16 and stores `n` in the
  header;
- `js_free` rebuilds the `{ptr, size, alignment}` that `ta_allocator.free`
  needs;
- `js_realloc` handles `NULL` (malloc) and size 0 (free); otherwise it
  allocates, copies, and frees;
- `js_calloc` is `malloc` plus `memset`;
- `js_malloc_usable_size` reads the header. That callback has no opaque,
  which is another reason for the header.

The defaults are a memory limit of 256 MiB (`JS_SetMemoryLimit`) and a stack
of 1 MiB (`JS_SetMaxStackSize`). Both limits raise faults.

### 5.3 Runaway scripts

QuickJS calls the interrupt handler about once every 10,000 bytecode
operations (`JS_INTERRUPT_COUNTER_INIT`). The budget therefore counts
**handler calls**. The default is 10,000 calls, roughly 10⁸ operations. The
counter is reset at the start of each vtable call, encode, and
`value_text`. An infinite loop becomes an uncatchable `InternalError`, which
gives `error.execution`. `JS_UpdateStackTop` is also called at the start of
each unit of work, so the stack limit is measured from the current C stack
depth.

### 5.4 GC and teardown

- `destroy_instance` runs `JS_RunGC` so that realm cycles are reclaimed
  deterministically.
- The context destroy works as follows:
  - if `live_instances == 0`, it force-frees the JSValues of the remaining
    wrappers (leaked by the RPC side), frees the utility context, runs
    `JS_RunGC`, and calls `JS_FreeRuntime`;
  - otherwise it intentionally leaks the runtime. Systems are not destroyed
    at server shutdown, and `JS_FreeRuntime` would assert on the live
    objects;
  - it then destroys the wrapper arena.

---

## 6. Mixed data models

### 6.1 `invoker.scxml`

The invoker crosses the model boundary only through the codec:

1. **Parent model.** `tat_actor_datamodel(system, request->parent)`, which
   uses `ta_actor_inspect` and then `ta_definition_inspect`.
2. **Child spec.** If `data` is present, it is encoded with the parent codec,
   and the resulting JSON object is the spec. Otherwise `src` is encoded
   (it must be a string such as `file:NAME.scxml`) and names
   `charts["irp_NAME"]`, as today.
3. **Child model** comes from the spec's `datamodel` field: `"ecmascript"`
   selects `tat_js_datamodel(&bind->js)`, and `"jq"` selects
   `tat_json_datamodel(&bind->json)`. **When the field is absent, the child
   inherits the parent's model.** Any other name is a fault.
4. **Refusal.** If the child model differs from the parent model, the start
   fails with the fault `"invoker.scxml: the child's data model differs from
   the parent's"` and returns `TA_OP_ERROR`.
5. The invoker compiles and registers the child with the child model. The
   cache key is `"<model name>:" + spec text`, because a spec without
   `datamodel` compiles once for each model that invokes it.
6. **Binding.** `params` is encoded with the parent codec. The object is
   filtered to the child's declared data IDs, as today, and **decoded with
   the child codec**. The result is a jq value for a jq child and an
   ECMAScript WIRE object for an ECMAScript child. That value is passed to
   `ta_actor_spawn`.

**How the core surfaces a refusal** (`tinyactors-core/src/invoke.c`,
`ta_invoke_start`): when `start` returns `TA_OP_ERROR`, the core removes the
invocation record it had added and calls `ta_interpreter_execution_error`
with the invoker's fault. The result is **one `error.execution` platform
event on the parent's internal queue**:

- `_event.type` is `"platform"`;
- `_event.data` is the fault's data, which is empty;
- `sendid` is empty, and `invokeid` is `TA_INVOKE_ID_NONE`;
- `done.invoke.<id>` and `error.communication` are not generated;
- no child is spawned.

### 6.2 The safety net in each adapter

- **ECMAScript:** importing any non-ECMAScript value throws a `TypeError`
  inside the session. The fault code is `TA_ERR_INVALID`, like every other JS
  exception, and the core raises `error.execution`. The `_event` getter
  throws. `value_text` faults with `TA_ERR_UNSUPPORTED`. `init_instance`
  returns `TA_ERR_UNSUPPORTED` for a foreign binding.
- **jq:** its value operations already fault when `tat_json_unwrap` fails.
  Phase 2 closes the one remaining gap. `_event.data` holding another
  model's value now faults (`TA_ERR_UNSUPPORTED`, "event data is not a JSON
  value") instead of reading as `null`. This happens in
  `tat_json_arguments`/`tat_json_run_source`, and every expression that
  binds `$_event` is affected.

## 7. Files

### New

| File | Contents |
|---|---|
| `src/ecmascript_datamodel.{c,h}` | The adapter; wire JSON in and out (`tat_js_value_to_json`, `tat_js_value_from_json`). |
| `src/value_codec.{c,h}` | The codec registry, the actor → model lookup, and receiver selection per request. |
| ~~`src/scxml_target.{c,h}`~~ | Deleted: the grammar is core's (`tinyactors-core/src/scxml_uri.{c,h}`, ADR 0044). |
| `tests/ecmascript_datamodel_test.c` | The unit test (`build/ecmascript_datamodel_test`, run by `mise run test`). |
| `ruby/scenarios/ecmascript_datamodel.rb` | The boundary scenarios. |

### Changed

| File | Change |
|---|---|
| `src/binding.h`, `binding.c` | `tat_bind.js` and `tat_bind.decode_model`. The value codecs delegate to the registry. |
| `src/server.c` | Calls `tat_value_codec_select` per request. `host.info` `datamodels` is now `["jq", "ecmascript"]`. |
| `src/adapters.c` | Registers `datamodel.ecmascript`. `invoker.scxml` changes as in 6.1. |
| `src/json_datamodel.c` | Faults on foreign `_event.data` (6.2). |
| `mise/tasks/build`, `mise/tasks/test` | New sources and the new unit-test binary. `-isystem` was not needed: `quickjs.h` compiles cleanly under `-std=c11 -Wall -Wextra -Werror -pedantic`. |
| `ruby/scxml_irp/baseline.json` | Regenerated: empty. |

### Tests

`tests/ecmascript_datamodel_test.c` uses a counting allocator and checks
that no bytes are outstanding after teardown. It covers:

- data: `declare`, ++ side effects (444, 445), ToBoolean (449), function
  data (453), and script functions with `assign foo.bar` (452);
- assignment errors: an undeclared root, `_event`, and `declare _event`;
- expressions: the object literal, a syntax error, and an interrupted loop;
- wire JSON: the markers and nested `null`;
- content (446, 558);
- wire values in the right realm;
- foreach (457, 459, 460);
- `make_params`, `value_text`, and SCXML targets;
- foreign values and a foreign binding;
- `inspect`;
- sharing: retain shares, and cross-realm arrays;
- a value outliving its session;
- a bound `declare`;
- a real `ta_system` chart for `_event`, `_sessionid`, `_name`,
  `_ioprocessors`, and `In()` across parallel regions, with a wire array as
  event data.

`ruby/scenarios/ecmascript_datamodel.rb`:

- a refused jq → ECMAScript invoke;
- a refused ECMAScript → jq invoke;
- an ECMAScript → ECMAScript invoke with params (an array reaching the
  child's realm), `#_parent` data, and `<donedata>`, where the child
  inherits the model;
- RPC delivery of an array payload to an ECMAScript actor.

## 8. Decisions (formerly open questions)

1. Values are shared, never copied; the cross-realm limitation (1.5) is accepted.
2. `inspect` reports declared names only: `declare`, `set_named`, the
   binding, and `foreach_bind`.
3. Markers `{"$value": ...}` at the top level; `null` when nested.
4. One runtime per host process.
5. Strict mode for `assign` only.
6. `make_params` keeps live objects.
7. `error.execution` data stays empty, as with jq. `TAT_JS_TRACE` prints the
   exception text.
8. Default limits; no determinism changes.
9. Expressions are recompiled on every call.
10. A `foreach` item or index must contain no ASCII character outside
    `[A-Za-z0-9_$]`, and `var <name>;` must compile.
11. `declare` of a system-variable name or `In` is a fault.
- N1. A child spec without `datamodel` inherits the parent's model.
- N2 and N4 are resolved by core definition inspection (ADR 0042); there is
  no host map.
- N3. The jq adapter faults on foreign `_event.data` (6.2).
- N5. RPC-decoded values keep their JSON text (WIRE) and are parsed per
  session.

## 9. Deviations from the review-round design

- **No host-side definition map, and no dispatch after-hook.** Core
  inspection replaced them (2.4).
- **The ECMAScript codec is JSON text, not `jv`.** The adapter does not link
  jq (2.1).
- **Interrupt budget units.** The budget counts handler calls, not checks of
  single operations; the default is 10,000 (5.3).
- **Foreign-value fault code.** Imports raise a JS `TypeError`, so the fault
  code is `TA_ERR_INVALID`, not `TA_ERR_UNSUPPORTED`. `value_text` and
  `init_instance` still report `TA_ERR_UNSUPPORTED` (6.2).
- **`In()` lookup** is a linear scan of a lazily built per-definition table,
  not a sorted array.
- **Parent model in the invoker** comes from the actor's definition, not from
  the request values' vtables (6.1).

