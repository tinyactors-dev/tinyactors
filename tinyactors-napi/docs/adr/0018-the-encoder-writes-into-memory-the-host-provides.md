# ADR 0018: The encoder writes into memory the host provides

**Status:** Accepted; not yet implemented. Refines ADR 0016 point 3 and ADR 0017 section 4.
**Date:** 2026-09-25

## Context

The OTLP encoder (`src/otlp.{c,h}`, ADR 0016) stages a turn's finished
spans in a buffer it owns. At the end of the turn,
`tna_otlp_take()` and `tna_otlp_take_logs()` hand that buffer to napi,
which copies it into a new `ArrayBuffer` with `napi_create_arraybuffer`.
The engine zero-fills that `ArrayBuffer` first.

The profile behind ADR 0017's follow-up (its implementation notes) puts
about 80 ns per event in copying. A traced turn writes about 350 KB,
which is then zeroed and copied again, and those passes push core's and
V8's data out of the caches. That spreads the cost across everything
else, beyond the copy itself. With macrostep spans, the traced path is
about 540 ns per event over untraced, against ADR 0017's target of about
280.

The encoder is meant to move to a place every host links: the host
kernel, or a library of its own (ADR 0016 point 3). Who owns its output
is therefore a question for every host, not only napi.

## Decision

**The encoder never owns its output. It says how many bytes a request
needs, and writes the request into memory the host provides.**

1. **The size is free to ask.** The encoder keeps a running total of each
   pending request's encoded size as spans and log records finish. Asking
   for it costs nothing and walks nothing.
2. **The host allocates exactly that size** in memory its own language
   owns, with no zero-fill where the language allows it.
3. **The encoder writes into it** in one pass, and forgets the turn's
   spans. Its staging buffer is reset, not freed, so a steady stream makes
   no allocator calls.

This is two calls, not an "allocate" callback: the encoder calls nothing
back and allocates nothing for the output, so its host interface
(ADR 0016 point 3) does not grow. Traces and logs are separate requests
and use the same two calls.

**Staging stays.** Macrosteps finish at different times across a turn, so
spans are still encoded into a staging buffer as they finish. What goes
away is the copy into memory the encoder owns, the zero-fill, and
napi's second copy. What is left is one write, straight from the staging
buffer into the host's memory. Whether to encode compact span records
only at the end of the turn, removing the staging copy too, is left to
measurement (see "Testing").

## What each host does in step 2

| Host | Memory | Copies after encoding |
|---|---|---|
| napi (Node, Bun) | `napi_create_buffer`: a `Buffer`, not zero-filled | none |
| Browser (WASM) | a region of the module's linear memory, seen from JavaScript as a `Uint8Array` | none, if `fetch` gets the view straight away. The browser copies a request body when the request is created. Code that keeps the bytes across an `await` must `slice()` them. |
| CPython | `PyBytes_FromStringAndSize(NULL, n)`, then written into | none |
| CRuby | `rb_str_new(NULL, n)`, then written into | none |
| Database hosts | a reused C buffer, handed to the database or a collector | none |

In the host kernel, the encoder and its open-span state move unchanged;
each host only implements step 2.

**napi's sink still receives a `Uint8Array`.** A Node `Buffer` is one, so
`setTraceSink(sink)` is unchanged for users. The bytes belong to the sink:
it may keep them across an `await`, as `tools/otlp.js` does.

## Alternatives

- **Hand JavaScript the staging buffer as an external `ArrayBuffer`.** It
  would need no copy at all. But some runtimes refuse external buffers
  (Electron, and V8 builds with the memory cage), and the buffer's
  finalizer can run after the system and its allocator are gone. That
  needs a lifetime rule this ADR avoids.
- **An allocation callback in the host interface.** It works, but it
  makes the encoder call out in the middle of a write and grows the
  interface. The two-call form keeps the encoder a pure function of its
  state.
- **Keep the copy and reuse one `ArrayBuffer` across turns.** Then the
  bytes would only be valid during the sink call, and any sink that posts
  them asynchronously would have to copy them anyway.

## Consequences

- One zero-fill and one copy of each traced turn's bytes go away in napi.
- The encoder's interface is ready for the kernel. `docs/proposals/host-kernel.md`
  lists it as the encoder's contract.
- `tna_otlp_take()` and `tna_otlp_take_logs()` are replaced by the
  size-then-write pair (AGENTS.md: change APIs in place).

## Testing

- **Native:** the size the encoder reports equals the bytes it writes,
  for golden charts, across turns, and with logs. The write never goes
  past the size it reported (checked under AddressSanitizer with a buffer
  of exactly that size).
- **JavaScript, on Node and Bun:** the sink's bytes decode as before
  (`decodeTraces`), and a sink that keeps them across an `await` still
  reads them intact after later turns.
- **Remeasure** macrostep spans and every record, on mise's Node and on
  Bun, on a quiet machine, against the figures in ADR 0017's notes. If the
  gain is small, measure encoding compact span records only at the end of
  the turn, which would remove the staging copy, and record both.
