# S13: A chart gives out its own address

**Status:** Sketch (2026-10-03), part of [the ring protocol proposal](../README.md)

A chart starts a charge with a payment provider and gives the provider a
webhook URL, `_ioprocessors.http.location`, so the provider can call this
same session back later. The binding renders the location: the processor
that parses inbound URLs back into addresses also formats outbound ones.
Core supplies only the session's ID and name. The session is unloaded
while it waits, and the callback brings it back.

**Sessions.** `s3` is `orders/42`. Nothing is recorded. The processor is
the one from [S12](S12-http-request.md), with an inbound router for
callbacks:

```js
// Inbound: the inverse of http.location.
server.on('request', async (req, res) => {
  const m = req.url.match(/^\/actors\/([^/]+\/[^/]+)$/);
  if (!m) return reply(res, 404);
  const { event, ...data } = JSON.parse(await readBody(req));
  await system.send(m[1], event, data, { origin: { type: 'http', uri: newReplyID(res) } });
});
```

**The chart:**

```xml
<state id="charging">
  <onentry>
    <send type="http" target="https://payments.example/charges" event="charge.create">
      <param name="amount"   expr="order.total"/>
      <param name="callback" expr="_ioprocessors.http.location"/>
    </send>
    <send event="charge.timeout" delay="10min" id="t"/>
  </onentry>
  <transition event="payment.completed" target="paid">
    <cancel sendid="t"/>
    <send typeexpr="_event.origintype" targetexpr="_event.origin" event="http.response">
      <param name="status" expr="204"/>
    </send>
  </transition>
  <transition event="charge.timeout" target="failed"/>
</state>
```

## Giving out the address

| Step | Ring | Record | What happens |
|---|---|---|---|
| S13.1 | – | – | `s3` enters `charging`. The data model evaluates `_ioprocessors.http.location`. napi's JavaScript data model asks core for the session's ID and name (`TA_DM_TEXT_SESSION_ID`, `_NAME`), and calls `http.location({id, address: "orders/42"})`. Result: `https://api.example/actors/orders/42`. This is a data model call, not ring traffic, and core stores nothing. |
| S13.2 | c2h | [`c2h.send`][c2h.send] `{io_type: "http", target: "https://payments.example/charges", name: "charge.create", data: v3}` | `v3` holds `amount` and `callback`. The pump calls `http.send()`. `target` is not a pending reply URI, so the processor POSTs to the provider. |
| S13.3 | – | – | The delayed send `t` is an envelope in the deadline index, due at t0+10min ([envelope](../reference/envelope.md#delayed-sends-envelopes-with-a-due-time)). |

## Waiting, unloaded

| Step | Ring | Record | What happens |
|---|---|---|---|
| S13.4 | h2c | [`h2c.unload`][h2c.unload] `{address: "orders/42", image}` | The host unloads idle sessions to bound memory ([unloading](../reference/unloading.md)). |
| S13.5 | c2h | [`c2h.unloaded`][c2h.unloaded] `{address: "orders/42", wake_at: t0+10min, flags: has_wake}` | `t` travels in the image. The URL the provider holds names an address, not a resident handle, so it stays valid. |

## The callback

| Step | Ring | Record | What happens |
|---|---|---|---|
| S13.6 | – | – | At t0+2min the provider POSTs `{"event": "payment.completed", ...}` to `/actors/orders/42`. The router parses the address `orders/42` and stores `res` under `req:9`. |
| S13.7 | h2c | [`h2c.event`][h2c.event] seq 88 `{target: "orders/42", name: "payment.completed", origin: "req:9", origin_type: "http", data: v11}` | |
| S13.8 | c2h | [`c2h.load_needed`][c2h.load_needed] `{address: "orders/42"}` | The envelope waits on the address's waiting list ([S07](S07-unloaded-tracker.md)). |
| S13.9 | h2c | [`h2c.load`][h2c.load] `{address: "orders/42", image, request_seq}` | The session is resident again, and `t` rejoins the deadline index. The waiting envelope joins the run queue. |
| S13.10 | c2h | [`c2h.send`][c2h.send] `{io_type: "http", target: "req:9", name: "http.response", data: v12}` | The chart cancels `t`, enters `paid`, and answers the webhook with 204 through `_event.origin`, as in S12.8. |
| S13.11 | c2h | [`c2h.input_completed`][c2h.input_completed] `{source_seq: 88, done}` | |

## What did not happen

- **Core rendered no URL.** The location format and the inbound route are
  one URL scheme, and it belongs to the processor (S13.1, S13.6). Core
  could not route inbound HTTP, so it does not format outbound URLs
  either. Today's `location` template and the native `text` callback that
  expands it are not needed.
- **The address outlived residency.** The provider held a URL to an
  address, not to a handle, so unloading the session (S13.4) did not
  invalidate it.
- **The location is still pure.** `http.location` depends only on the
  registration and the session, as ADR 0068 requires. A chart that stores
  its callback URL in its data gets the same URL when replayed.

## Who drops what, who retries how

| What was lost | Dropped by | Told how | Retried by |
|---|---|---|---|
| The provider never calls back | the provider | nobody | the chart's timeout `t` (S13.3) |
| A callback while core is not admitting | the host, at its edge | HTTP 503 | the provider's own webhook retry |
| A callback for an address whose load fails | core recycles the envelope ([S07](S07-unloaded-tracker.md)) | `c2h.input_refused` → HTTP 503 | the provider's own webhook retry |

[h2c.event]: ../../../../tinyactors-core/src/generated/MESSAGES.md#h2cevent
[h2c.unload]: ../../../../tinyactors-core/src/generated/MESSAGES.md#h2cunload
[h2c.load]: ../../../../tinyactors-core/src/generated/MESSAGES.md#h2cload
[c2h.send]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hsend
[c2h.unloaded]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hunloaded
[c2h.load_needed]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hload_needed
[c2h.input_completed]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hinput_completed
