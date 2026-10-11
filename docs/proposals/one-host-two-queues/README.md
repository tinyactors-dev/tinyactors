# One host, two queues

**Status:** Proposed (2026-10-11). Supersedes [ADR 0008](../../adr/0008-bindings-only-do-io.md) if adopted.

The core already has the interface every binding needs: `ta_host` takes inputs and gives effects, and holds them until they are acknowledged. The bindings grew because we put more objects beside the host for them to wire, and because rich data crossed as one C struct per topic. This proposal makes the host the only interface, moves rich data into one encoding the core owns, and starts the bindings again from nothing.

## Whom it serves, and who pays

**Served first:** the binding author and the maintainers. Today a binding is
11,620 lines (Node) or 4,576 (the web twin), mostly wiring of core objects and
one hand-written converter per struct; each restructure so far has moved code
between layers without removing it. With one interface, a binding is a queue
loop, a datamodel or none, and a CBOR module, and nothing C does is done again
in a language. **Then** the C embedder (one object, requests as bytes), the
application developer (the same API in every runtime of a language), the
operator and the observability backend (the same OTLP, now encoded once into
the binding's buffer).

**What already answers them:** `ta_host` takes inputs and gives effects and
holds them until they are acknowledged; ADR 0008 already moved I/O out of
core. This proposal removes what grew beside the host, not the host.

**Who pays:**

| Cost | What |
|---|---|
| per event | nothing: the hot path (spawn, send, deliver, run, effects) stays structs |
| per request | one CBOR encode and decode, for rarely called operations only |
| per actor | nothing |
| memory | the caller's token list and buffers; the CBOR functions allocate nothing |
| code size | a CBOR module in core; 1,811 lines of C leave core (link, JSON, framing, the batcher) |
| API surface | `host.h` regrouped into structs (27 functions over three parameters), effect accessors, `ta_host_request` and its catalogue |
| every binding | regenerated from the guide; the three sketches below show the work for Node, CPython and the browser |

**Out of scope:** durability (snapshots and hibernation take actors out of
memory for a while, export moves them; none of them is persistence), any
transport protocol in core, several datamodels in one host, a multi-threaded
host, and the PostgreSQL binding for now.

## The documents

- [bindings.md](bindings.md): “Writing a binding”, the draft of
  `tinyactors-core/docs/bindings.md`.
- [napi.md](napi.md): the Node addon and the JavaScript package, designed from
  the guide alone.
- [cpython.md](cpython.md): the CPython extension and its `python` datamodel.
- [browser.md](browser.md): the wasm addon under the shared JavaScript package.

Each sketch lists its findings: where the guide had no answer, or the answer
needed something core does not have. The guide and this page include their
fixes.

## What we agreed

**`ta_host` is the universal interface.** Every runtime talks to the same object; there is no second engine or wrapper layer.

**A binding defines how it works the queues.** Feed inputs, run, take effects, do the I/O they ask for, arm one timer from the next deadline.

**The link protocol disappears; JSON leaves core, the bindings and the examples.** `src/link`, `link.h`, framing, `src/base/json` and the JSON forms of neutral values go, and nothing replaces the link protocol.

**Rich data goes through one generic encoding.** Trace configuration first; the core is told what is needed and owns the schema.

**That encoding is CBOR, with token lists.** The encoder and decoder work on a flat token list in a caller-allocated buffer.

**Delete every current binding and its examples.** They are regenerated cheaply against the new interface, without the past to confuse them.

## What the decision implies

Points from the discussion that the summary leaves implicit. None of them change the decision; each is needed for it to hold.

**The hot path keeps its structs.** spawn, send, send_many, deliver, advance_time, resume, run and the effect structs. CBOR is for requests and replies, not for every event.

**Requests cover every rarely-called call, not only tracing.** Watches, stats, metrics, inspection, the state value, snapshot contents and decoded spans become requests and replies. Defaults (a watch’s 15 minutes, the sampling scale), validation and error messages move into core.

**The host owns the encoder; there is no batcher.** Journals drain into `ta_otlp` inside the host’s run, the same work the binding does today with fewer calls across. When the encoder’s staging fills (`TA_OTLP_FLUSH`) or a flush deadline passes, an effect says traces are ready, and the request is written straight into a buffer the binding supplies (`ta_otlp_put_traces` does this today): encoded once, written once. Retries, caps and failure logging around the HTTP post are I/O policy, so they are the binding’s.

**Inputs during a run are appended, not refused.** Between runs every call is already safe (effects taken stay borrowed until the ack). During a run, today’s `TA_ERROR_STATE` becomes a mailbox append for send, send_many, spawn and terminate, the path an internal `<send>` or `<invoke>` takes: no new queue, no copy, no growth mid-run (a full pool is `TA_ERROR_NO_MEMORY`). This replaces the running states 0–3 and `later()`; structural calls stay refused during a run.

**One buffer convention, and requests are atomic.** Every call that writes for the caller takes a `ta_buffer` (today: `buffer, capacity, *length` in five forms). `TA_ERROR_NO_MEMORY` keeps meaning “nothing changed”: a request whose reply does not fit is not applied, envelopes and traces stay with the host until they are written.

**Core changes found by the stress test.** Designing the Node addon from the guide needed: structs for the 27 `host.h` functions over three parameters; a `CHANGED` effect for subscribed sessions; the invocation token on every send to an invoked service; both clocks in `ta_host_advance_time`; `ta_host_metrics` writing the counters on demand (no metrics timer or effect); `ta_host_create` taking CBOR config plus a struct of pointers; and effect accessors (`ta_host_effect_number`, `_text`) so no binding decodes a host struct. See [napi.md](napi.md).

**The datamodel vtable stays the one call out.** A binding with its own datamodel implements it; one that uses the quickjs datamodel implements nothing that core calls.

**Datamodels belong to bindings, by name.** Core already picks a chart’s datamodel by the vtable’s `name` and the compiler takes a checker per name, so a binding defines a datamodel of its own language without a core change: the CPython binding registers `"python"`, whose values are CPython objects held by handle. A host runs one datamodel; mixing them in one host is not supported. See [cpython.md](cpython.md).

**Conformance becomes request and reply vectors.** Pairs of CBOR (shown as diagnostic notation) that every regenerated binding must reproduce, next to the C conformance and W3C runs that stay.

## Three parts per binding, everything else in the host

Today each binding wires the host, the link layer, the encoder and the batcher, and converts one struct per topic. Proposed, a binding is a queue loop, a datamodel (or none) and a CBOR module.

    TODAY                                         PROPOSED

    bindings, each wiring the pump by hand        a binding, regenerated per runtime
    +-------------+ +-----------+ +-----------+   +------------+ +-----------+ +-------------+
    | Node addon  | | web:      | | PostgreSQL|   | queue loop | | datamodel | | CBOR module |
    | + core.mjs  | | native.mjs| | extension |   | inputs,run,| | language  | | tokens <->  |
    | 11,620 lines| | 4,576     | | 5,935     |   | effects,   | | values, or| | values,     |
    |             | | (twin)    | | own route |   | one timer  | | quickjs   | | written once|
    +------+------+ +-----+-----+ +-----+-----+   +-----+------+ +-----+-----+ +------+------+
           |  each wires all of these  |                |   inputs     |  vtable      |
    +------v------+ +----------+ +-----v-----+    +-----v--------------v--------------v------+
    | ta_host     | | src/link | | ta_otlp_  |    | ta_host: the universal interface         |
    | effects,    | | hello, ev| | batch     |    |  input queue   hot: structs, cold: CBOR  |
    | router      | | err, JSON| |           |    |  effect queue  hot: structs, cold: CBOR  |
    +-------------+ +----------+ +-----------+    |  owns: run loop, OTLP encoder, request   |
    +--------------+ +------------------------+   |  schemas, defaults, validation, errors   |
    | src/base/json| | one C struct per topic |   +------------------------------------------+
    | neutral text | | and a converter per    |   | ta_system, segments, router | ta_otlp |  |
    +--------------+ | struct in each binding |   | src/base/cbor                            |
                     +------------------------+   +------------------------------------------+
       (everything in the left column but ta_host is deleted)

*Left: today, of which everything but `ta_host` goes. Right: proposed.*

## The two queues

Inputs go in by call; effects come out as a list the binding reads and then acknowledges. The hot path stays plain structs. Requests and replies, envelopes for other systems and OTLP bodies are bytes.

                  INPUTS (the binding calls)          EFFECTS (the binding takes, then acks)
                  --------------------------          --------------------------------------
      hot   spawn · send · send_many        --->  +---------+  --->  send to an I/O processor       hot
      hot   deliver (an envelope)           --->  |         |  --->  invoke · uninvoke · done ·     hot
      hot   advance_time · resume           --->  | ta_host |          error · log
      hot   wake (a blob) · completions     --->  |         |  --->  peer envelope (CBOR) ·         out
      cold  request (CBOR)                  --->  | run(budget) ->   traces ready
                                                  | next deadline  ->  reply (CBOR)                  cold
                                                  +---------+
      Inputs during a run are appended, not refused. The datamodel vtable is the only call out.
      CBOR or bytes: requests, replies, peer envelopes, OTLP bodies. The rest stays structs.

*Every arrow is a call or an effect a binding already knows how to make or read; the CBOR ones are new.*

## CBOR as token lists

The decoder turns a message into a flat list of tokens in memory the caller provides; the encoder does the reverse. A binding walks the list in its own language, so each language needs one generic CBOR module and no CBOR library, and pays one FFI call per message instead of one per item. Bytes and 64-bit integers are exact, and `$ref` can be a tag.

    the request (CBOR diagnostic notation)     token list, in the caller's buffer
    {                                          kind   count  value (a view into the bytes)
      "op": "trace.configure",                 MAP      4
      "detail": "summary",                     TEXT     2    "op"
      "sample": 0.01,                          TEXT    15    "trace.configure"
      "rules": [                               TEXT     6    "detail"
        { "at": "start",                       TEXT     7    "summary"
          "from": "host",                      TEXT     6    "sample"
          "perSecond": 5 }                     FLOAT    0    0.01
      ]                                        TEXT     5    "rules"
    }                                          ARRAY    1    ...
                decode / encode: one call each way
    bytes on the wire: a4 62 6f 70 ...         Text and byte tokens point into the bytes:
                                               nothing is copied. FFI cost is per message,
                                               not per item.

*A trace configuration request, its token list and its bytes.*

### The C API · `include/tinyactors/cbor.h`

Every function takes at most three parameters; what belongs together is grouped in `ta_bytes`, `ta_buffer`, `ta_cbor_document` and `ta_cbor_writer`.

```
/* include/tinyactors/cbor.h — CBOR (RFC 8949) as token lists in caller memory:
 * the encoding of neutral values and of host requests and replies.
 * Freestanding (src/base/cbor.c): no C library, no allocation. */

/* ---- in types.h: views of bytes and strings, buffers the caller lends ---- */
/* A view of a NUL-terminated string (core is freestanding: no strlen). */
static inline ta_string_view ta_string(const char *text) {
    uint32_t length = 0;
    while (text[length]) length++;
    return (ta_string_view){ text, length };
}
typedef struct ta_bytes  { const void *pointer; size_t length; } ta_bytes;
typedef struct ta_buffer { void *pointer; size_t capacity, length; } ta_buffer;
                           /* on TA_ERROR_NO_MEMORY, length is what it needs */

/* ---- tokens ------------------------------------------------------------- */
enum {
    TA_CBOR_UINT = 1, /* value: the integer (0 … 2^64-1) */
    TA_CBOR_NEGINT,   /* value: n, for the integer -1 - n */
    TA_CBOR_BYTES,    /* count: length; value: offset into the document's bytes */
    TA_CBOR_TEXT,     /* the same, UTF-8 (checked) */
    TA_CBOR_ARRAY,    /* count: items; the items follow, each with its children */
    TA_CBOR_MAP,      /* count: pairs; key and value tokens follow, in turn */
    TA_CBOR_TAG,      /* value: the tag; the tagged item follows */
    TA_CBOR_SIMPLE,   /* value: TA_CBOR_FALSE, _TRUE, _NULL, _UNDEFINED */
    TA_CBOR_FLOAT     /* value: the double's bits (half and single are widened) */
};
enum { TA_CBOR_FALSE = 20, TA_CBOR_TRUE = 21, TA_CBOR_NULL = 22, TA_CBOR_UNDEFINED = 23 };
enum { TA_CBOR_TAG_URI = 32 };   /* an actor reference ({"$ref"} today): its address */
#define TA_CBOR_DEPTH_MAX 64     /* nesting deeper than this is TA_ERROR_INVALID */

typedef struct ta_cbor_token {
    uint32_t kind;  /* TA_CBOR_* */
    uint32_t count;
    uint64_t value;
} ta_cbor_token;

/* A message and its tokens. Decoding: `bytes` is the message, the tokens are
 * written. Encoding: the tokens are read, text and byte tokens point into `bytes`. */
typedef struct ta_cbor_document {
    ta_bytes bytes;
    ta_cbor_token *tokens;     /* the caller's */
    uint32_t capacity, count;
} ta_cbor_document;

/* ---- decoding ----------------------------------------------------------- */
/* TA_OK; TA_ERROR_NO_MEMORY: count is the tokens it needs; TA_ERROR_INVALID:
 * malformed, bad UTF-8, too deep, or bytes after the item; TA_ERROR_UNSUPPORTED:
 * indefinite lengths. Nothing is copied: text and bytes stay in place. */
TA_API ta_status ta_cbor_decode(ta_cbor_document *document);

/* The text or bytes of token `at`, as a view into the document's bytes. */
TA_API ta_string_view ta_cbor_string(const ta_cbor_document *document, uint32_t at);
/* The index after the item at `at` and all its children. */
TA_API uint32_t ta_cbor_next(const ta_cbor_document *document, uint32_t at);
/* In the map at `map`, the index of the value whose key is the text `key`; TA_NIL if none. */
TA_API uint32_t ta_cbor_member(const ta_cbor_document *document, uint32_t map, ta_string_view key);

/* ---- encoding ------------------------------------------------------------ */
/* The tokens as CBOR, in core deterministic form (RFC 8949 §4.2.1): shortest
 * integers and lengths, definite lengths, floats in the shortest exact width.
 * TA_ERROR_INVALID when the tokens do not form one item. */
TA_API ta_status ta_cbor_encode(const ta_cbor_document *document, ta_buffer *out);

/* ---- writing tokens from C ------------------------------------------------ */
/* A document being built: tokens go to `document`, their text and bytes to
 * `strings`. The first error sticks in `status`; check it once, at the end. */
typedef struct ta_cbor_writer {
    ta_cbor_document document;
    ta_buffer strings;
    ta_status status;
} ta_cbor_writer;
TA_API void ta_cbor_write_map(ta_cbor_writer *w, uint32_t pairs);
TA_API void ta_cbor_write_array(ta_cbor_writer *w, uint32_t items);
TA_API void ta_cbor_write_text(ta_cbor_writer *w, ta_string_view text);
TA_API void ta_cbor_write_bytes(ta_cbor_writer *w, ta_bytes bytes);
TA_API void ta_cbor_write_uint(ta_cbor_writer *w, uint64_t value);
TA_API void ta_cbor_write_int(ta_cbor_writer *w, int64_t value);
TA_API void ta_cbor_write_float(ta_cbor_writer *w, double value);
TA_API void ta_cbor_write_simple(ta_cbor_writer *w, uint32_t simple);
TA_API void ta_cbor_write_tag(ta_cbor_writer *w, uint64_t tag);
/* Encode what was written: status, then ta_cbor_encode. */
TA_API ta_status ta_cbor_finish(ta_cbor_writer *w, ta_buffer *out);
```

### From C: build a request, send it, read the reply

```
/* configure tracing from C: build the request, send it, read the reply */
ta_cbor_token tokens[32];
char strings[256], request[512], reply[512];
ta_cbor_writer w = {
    .document = { .tokens = tokens, .capacity = 32 },
    .strings = { .pointer = strings, .capacity = sizeof strings },
};
ta_cbor_write_map(&w, 3);
ta_cbor_write_text(&w, ta_string("op"));     ta_cbor_write_text(&w, ta_string("trace.configure"));
ta_cbor_write_text(&w, ta_string("detail")); ta_cbor_write_text(&w, ta_string("summary"));
ta_cbor_write_text(&w, ta_string("sample")); ta_cbor_write_float(&w, 0.01);

ta_buffer out = { .pointer = request, .capacity = sizeof request };
ta_buffer answer = { .pointer = reply, .capacity = sizeof reply };
if (ta_cbor_finish(&w, &out) == TA_OK &&
    ta_host_request(host, (ta_bytes){ out.pointer, out.length }, &answer) == TA_OK) {
    ta_cbor_token reply_tokens[32];
    ta_cbor_document d = { .bytes = { answer.pointer, answer.length },
                           .tokens = reply_tokens, .capacity = 32 };
    if (ta_cbor_decode(&d) == TA_OK) {
        uint32_t error = ta_cbor_member(&d, 0, ta_string("error"));
        if (error != TA_NIL) /* {"code", "path", "message"} */
            report(ta_cbor_string(&d, ta_cbor_member(&d, error, ta_string("message"))));
    }
}
```

### The host calls that use it · `host.h`

```
/* the rarely-called API: one request in, one reply out, both CBOR maps.
 * A request names its operation in "op"; a reply is the result, or
 * {"error": {"code", "path", "message"}}. */
TA_API ta_status ta_host_request(ta_host *host, ta_bytes request, ta_buffer *reply);
/* after a traces-ready effect: the OTLP request, written into the caller's buffer;
 * the spans leave the encoder's staging only on TA_OK */
TA_API ta_status ta_host_traces(ta_host *host, ta_buffer *out);
/* a PEER effect's envelope as CBOR, written into the caller's buffer (the effect
 * keeps its payload handle, borrowed until the ack, as today) */
TA_API ta_status ta_host_envelope(ta_host *host, const ta_host_effect *peer, ta_buffer *out);

/* Every call that writes for its caller takes a ta_buffer. TA_ERROR_NO_MEMORY:
 * out->length is what it needs and nothing changed (a request is not applied).
 * Requests are refused during a run (TA_ERROR_STATE). */
```

## Through the three lenses

The lenses the alternatives were compared with: runtimes differ, other languages, protocols out of core.

### Runtimes differ

*(Strong)*

Node, browsers and PostgreSQL differ only in how they do the I/O the effects ask for. The host decides the same things for all of them; a runtime that cannot do something answers with a failed completion, which becomes `error.communication`.

### Other languages

*(Strong)*

Plain structs on the hot path, one byte buffer per request and reply, and a token list to walk. With the quickjs datamodel there are no upcalls at all: Python `ctypes`, Java’s FFM API and Swift call it directly, with no CBOR or JSON library of their own.

### Protocols out of core

*(Strong)*

Core speaks no transport protocol, and nothing replaces the link protocol: envelopes leave as effects and arrive as inputs, and how they travel between systems is the application’s. Traces leave as OTLP written into the binding’s buffer.

## What goes, what stays

| Path                                                                                                       | Files | Lines  | Why, and what replaces it                                                                                                                                                                                                                                    |
|------------------------------------------------------------------------------------------------------------|-------|--------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `bindings/node`                                                                                            | 40    | 11,620 | regenerated: a queue loop, the JavaScript datamodel, a CBOR module                                                                                                                                                                                           |
| `bindings/web`                                                                                             | 8     | 4,576  | regenerated the same way, over the wasm build                                                                                                                                                                                                                |
| `bindings/postgres`                                                                                        | 29    | 5,935  | regenerated later; jsonb at its edge                                                                                                                                                                                                                         |
| `bindings/` itself, with `README.md` and `surface.json`                                                    | 2     | 373    | the directory goes: each binding is its own project at the repository root, like `tinyxml`; the binding contract moves to `tinyactors-core/docs/bindings.md` (drafted in [bindings.md](bindings.md)), and request and reply vectors replace the surface check |
| `examples/` (chess, ui-elements, access-reconciler)                                                        | 30    | 4,757  | regenerated against the new bindings                                                                                                                                                                                                                         |
| `src/link`, `link.h`                                                                                       | 2     | 1,251  | the link protocol disappears completely; nothing replaces it                                                                                                                                                                                                 |
| `src/otlp/batch.c` and `ta_otlp_batch`                                                                     | 1     | 161    | the encoder’s staging already batches; posting policy is I/O                                                                                                                                                                                                 |
| `src/base/json.*`                                                                                          | 2     | 284    | JSON leaves core                                                                                                                                                                                                                                             |
| `src/base/frame.c`, `frame.h`                                                                              | 2     | 115    | framing is transport (open decision)                                                                                                                                                                                                                         |
| JS runners: `tests/conformance/run_js.mjs`, `tests/w3c/run_js.mjs`, `tools/bench_*.mjs`                    | 4     | 714    | they drive the deleted bindings                                                                                                                                                                                                                              |
| mise tasks: `node:*`, `web:*`, `pg:*`, `example:*`, `examples:test`, `conformance:js`, `w3c:js`, `surface` | —     | —      | with what they build and test                                                                                                                                                                                                                                |

Deleted · line counts from main at f2302d9

### Kept

- the kernel, `ta_system`, the compiler and images
- `ta_host`: segments, router, migration, hibernation, snapshots
- the OTLP encoder, now owned by the host
- the quickjs datamodel (its value functions switch to CBOR)
- the C test programs, the C conformance scenarios and W3C (all 191)
- the torture chamber (C, its own web UI), minus the PostgreSQL scenario

## Settled in review

### Neutral values become CBOR.

In the same change, so there is one value encoding: the datamodel vtable’s `to_wire`/`from_wire`, the quickjs datamodel’s value functions, hibernation, export and snapshot blobs, and the Values section of `docs/protocol.md`.

### Between systems: envelopes only.

The router keeps peers, routes, deduplication and error envelopes; an envelope leaves as a CBOR effect and arrives as an input. The link protocol disappears completely, and framing (`frame.h`, `src/base/frame.c`) with it.

### PostgreSQL is deleted too.

With the torture chamber’s `postgres-millions` scenario and the store link. A regenerated extension maps CBOR to `jsonb` at its edge.

### Text keys.

`"op"`, `"rules"`: requests stay readable in diagnostic notation and logs.

### Writer helpers for C embedders.

Functions that fill a token list (`ta_cbor_map`, `ta_cbor_text`, …), so C builds requests without a second API.

### No batcher.

`ta_otlp_batch` goes: the encoder’s staging is the batch, and the request is written into the binding’s buffer.

### No `bindings/` directory.

Each binding is a project at the repository root with its own `mise.toml`. Recommended; confirm when the first one is regenerated.

### One datamodel per host in the API.

Mixing datamodels in a host is not supported, yet `ta_host_config` takes an array of up to eight and every effect and envelope carries a datamodel index. Making the config take one binding would let those indices go. Suggested, not decided.

### At most three parameters.

Group the rest in structs, as the CBOR and request functions above do. Worth recording in `AGENTS.md`; it is not written there yet.

## Order of work, in commits that each pass the tests

1.  **ADR** Supersede ADR 0008: roles served first (C embedder, binding author, maintainers), costs, what is out of scope.
2.  **Delete** Remove the bindings and the `bindings/` directory, the examples, the link layer, JSON, framing, `ta_otlp_batch` and their tasks in one commit; core’s C tests must still pass.
3.  **CBOR** `src/base/cbor`: decode to and encode from token lists, with unit tests and RFC 8949 test vectors.
4.  **Values** Neutral values as CBOR: the datamodel vtable, the quickjs datamodel, blobs and snapshots, `docs/protocol.md`.
5.  **Host** The host owns the encoder (traces written into the caller’s buffer), queues inputs during a run, and takes requests: trace configuration, watches, stats, metrics, inspection, state, spans.
6.  **Vectors** Request and reply vectors in `tests/conformance`, run by a C runner.
7.  **Guide** Write `tinyactors-core/docs/bindings.md` from the draft in [bindings.md](bindings.md), before any binding is regenerated.
8.  **Bindings** Regenerate the JavaScript binding as one package at the repository root over two addons (Node-API, wasm), then the examples; then CPython with its python datamodel; PostgreSQL and other languages after.

Sources: [`host.h`](../../../tinyactors-core/include/tinyactors/host.h) · [`datamodel.h`](../../../tinyactors-core/include/tinyactors/datamodel.h) · [`datamodel_ecmascript.h`](../../../tinyactors-core/include/tinyactors/datamodel_ecmascript.h) · [`ADR 0008`](../../../docs/adr/0008-bindings-only-do-io.md) · [`protocol.md`](../../../tinyactors-core/docs/protocol.md). A proposal, not a decision.
