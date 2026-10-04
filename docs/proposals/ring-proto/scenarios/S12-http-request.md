# S12: An HTTP request and its response

**Status:** Sketch (2026-10-03), part of [the ring protocol proposal](../README.md)

An HTTP I/O processor delivers a request to a chart as an event. The chart
answers with a `<send>` to `_event.origin`. Core keeps no state for the
reply target: the origin is text, and the processor alone decides whether
it is still live. This is why core needs only a set of I/O type names,
with no I/O callbacks, templates or origin scopes
([building blocks](../reference/building-blocks.md#io-processors-and-invokers-one-type-registry-twice)).

**Sessions.** `s3` is `orders/42`, a resident session. Nothing is recorded.

**The processor** (napi, JavaScript). It keeps a map from reply URI to
response object. It invents the URI when a request arrives and forgets it
when it answers.

```js
const pending = new Map();      // reply URI → http.ServerResponse
let next = 0;

const http = {
  location: ({ id, address }) => address ? `https://api.example/actors/${address}`
                                         : `https://api.example/sessions/${id}`,
  send(request) {               // each c2h.send with io_type "http"
    const res = pending.get(request.target);
    if (!res) return;           // gone, timed out or already answered: drop
    pending.delete(request.target);
    res.writeHead(request.data?.status ?? 200, request.data?.headers);
    res.end(request.content ?? JSON.stringify(request.data?.body ?? null));
  },
};
system.registerIO('http', http);  // posts h2c.type_declare; JavaScript keeps the object

server.on('request', async (req, res) => {
  if (!system.admitting) return reply(res, 503);
  const id = `req:${++next}`;
  const abort = new AbortController();
  pending.set(id, res);
  res.on('close', () => { pending.delete(id); abort.abort(); });
  try {
    const result = await system.send(addressFrom(req), `http.${req.method.toLowerCase()}`,
      { path: req.url, headers: req.headers, body: await readBody(req) },
      { origin: { type: 'http', uri: id }, signal: abort.signal, timeout: 5000 });
    if (result.ignored && pending.delete(id)) reply(res, 404);
  } catch (err) {
    if (pending.delete(id)) reply(res, err.code === 'runaway' ? 500 : err.code === 'timeout' ? 504 : 503);
  }
});
```

**The chart:**

```xml
<state id="ready">
  <transition event="http.get">
    <send typeexpr="_event.origintype" targetexpr="_event.origin" event="http.response">
      <param name="status" expr="200"/>
      <param name="body"   expr="order"/>
    </send>
  </transition>
</state>
```

## Registration

| Step | Ring | Record | What happens |
|---|---|---|---|
| S12.1 | h2c | [`h2c.type_declare`][h2c.type_declare] `{registry: io, type: "http"}` | `registerIO('http', http)` posts this. Core adds the atom `http` to its I/O type registry, and nothing else. The processor object, its `send` and its `location` stay in JavaScript ([building blocks](../reference/building-blocks.md#io-processors-and-invokers-one-type-registry-twice)). Every input after this record sees `http` declared. |

## The request

| Step | Ring | Record | What happens |
|---|---|---|---|
| S12.2 | – | – | `GET /orders/42` arrives. `admitting` is true and the submission table has room. The processor stores `res` under `req:1`. |
| S12.3 | h2c | [`h2c.event`][h2c.event] seq 41 `{target: "orders/42", name: "http.get", origin: "req:1", origin_type: "http", data: v7}` | The host retains its original for retry until a terminal record names `source_seq` 41. A `traceparent` header on the request becomes the trace fields (`ta_traceparent_parse`), so s3's macrostep joins the client's trace ([telemetry](../reference/telemetry.md#trace-context-across-the-rings)). |
| S12.4 | – | – | `ta_run` admits the event. Envelope e12 holds `http.get`, `req:1` and `http` in its text chain, and owns v7. |
| S12.5 | – | – | e12 is taken. Its name resolves to the atoms `[http, get]` by lookup ([names](../reference/names.md#matching-without-interning)). |
| S12.6 | – | – | The data model reads `_event.origin` and `_event.origintype` as text from the envelope (`req:1`, `http`). No processor code runs. |
| S12.7 | – | – | The `<send>` evaluates `typeexpr` to `http`. `ta_atom_find` finds it in the type set, so the type is supported. Core reserves the record in c2h. |
| S12.8 | c2h | [`c2h.send`][c2h.send] `{s3.m3, io_type: "http", target: "req:1", name: "http.response", data: v8}` | The pump may execute it at once: `http.send()` finds `req:1`, writes `200` and the body, and releases v8. The record's trace fields name the sending step; a processor that makes an outgoing request writes them as its `traceparent` header. |
| S12.9 | c2h | [`c2h.macrostep_ended`][c2h.macrostep_ended] `{s3.m3, stable}` | Reports the end. It does not gate the response, which may already be sent. |
| S12.10 | c2h | [`c2h.input_completed`][c2h.input_completed] `{source_seq: 41, stable}` | `send()` resolves; `req:1` left `pending` when S12.8 answered it. The host drops its retained original. Core frees e12 and releases v7. |

## When it goes wrong

| Step | Situation | Core | Processor |
|---|---|---|---|
| S12.11 | `admitting` is false | – | answers 503 without posting ([S09.5](S09-edge-backpressure.md#filling-up)) |
| S12.12 | The pool fills after S12.3 | [`c2h.bounced`][c2h.bounced] `{source_seq: 41, reason: pool}` | `send()` rejects; answers 503 |
| S12.13 | The client disconnects before take | [`h2c.withdraw`][h2c.withdraw] `{target_seq: 41}` → [`c2h.input_refused`][c2h.input_refused] `{withdrawn}` | `req:1` is already removed; nothing to answer |
| S12.14 | The client disconnects after take | the chart runs, and S12.8 is written | `req:1` is not in `pending`: drop |
| S12.15 | The chart replies twice | two `c2h.send` records to `req:1` | the second finds nothing: drop |
| S12.16 | No transition matches `http.get` | `c2h.input_completed {flags: ignored}` | answers 404 at once, not after the timeout |
| S12.17 | The chart writes `type="htp"` | `error.execution` in the chart's own block, during its macrostep, because `htp` is not in the type set | – |
| S12.18 | c2h is full at the `<send>` | the slot parks before the `<send>` evaluates anything, and runs it once after the host drains ([macrostep](../reference/macrostep.md#a-full-c2h-pauses-the-slot)); the chart sees nothing | answers when the record arrives; answers 504 only if its timeout removed `req:1` first, because the host stopped draining |
| S12.19 | The chart loops | `c2h.input_completed {runaway}` ([macrostep](../reference/macrostep.md#runaways-the-chain-limit)) | answers 500 |
| S12.20 | The chart replies after calling another service | `c2h.input_completed` first, `c2h.send` later | the reply finds `req:1` until the timeout removes it |

## What did not happen

- **Core stored no reply target.** `req:1` is text in one envelope, gone
  when e12 is freed. Today's MESSAGE origin entry, its reference count and
  its page are not needed.
- **Nothing closed a scope.** A SCOPED origin made a late reply render an
  empty target. Here, removing `req:1` from `pending` makes a late reply a
  no-op (S12.14, S12.20). To the client the two behave the same. A
  WebSocket works the same way: its origin is `ws:<connection>`, and the
  connection map decides whether it is still live.
- **No processor code ran inside the macrostep.** The type check needs only
  the type set (S12.7). The response is a record, executed on receipt (S12.8).

## Who drops what, who retries how

| What was lost | Dropped by | Told how | Retried by |
|---|---|---|---|
| A request while core is not admitting | the host, at its edge (S12.11) | HTTP 503 | the client |
| A bounced request | core (S12.12) | `c2h.bounced` → HTTP 503 | the client |
| A reply to a client that left | the processor (S12.14) | nobody | nobody: the client has gone |
| A second reply | the processor (S12.15) | nobody | – |
| A reply delayed by a full c2h | nobody: it is late, not lost (S12.18) | – | the client after 504, if the host stopped draining |

[h2c.event]: ../../../../tinyactors-core/src/generated/MESSAGES.md#h2cevent
[h2c.type_declare]: ../../../../tinyactors-core/src/generated/MESSAGES.md#h2ctype_declare
[h2c.withdraw]: ../../../../tinyactors-core/src/generated/MESSAGES.md#h2cwithdraw
[c2h.send]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hsend
[c2h.macrostep_ended]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hmacrostep_ended
[c2h.input_completed]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hinput_completed
[c2h.input_refused]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hinput_refused
[c2h.bounced]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hbounced
