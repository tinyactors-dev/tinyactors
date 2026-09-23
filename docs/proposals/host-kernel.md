# Host kernel: share the napi plumbing with other language hosts

**Status:** Sketch (2026-09-23)
**Touches:** tinyactors-napi, future hosts (CPython, CRuby, a browser build over WebAssembly)
**Not a plan:** nothing here is decided or scheduled. ADRs and roadmaps win.

## Goal

The napi addon contains a lot of hand-written C that never touches
Node-API: scheduling, mailboxes, routing, the directory, timers, enumeration
and effects. Hosts that keep actors in memory need the same code. This
proposal moves it into a shared C library before a second language host
starts, instead of copying it.

It does not cover the database hosts ([SQLite](sqlite-host.md),
[PostgreSQL](postgres-host.md)). There, mailboxes, the directory and timers
are tables, not in-memory containers.

## What is portable today

Sizes as of 2026-09-23, from `tinyactors-napi/src`:

| | Lines | Files |
|---|---|---|
| **Free of Node-API** | ~2,200 | `scheduler.c` (scheduling table, mailboxes, ready and runnable queues, dead letters, `send()` waiters), `router.c`, `directory.c`, `events.c`, `enumerate.c`, `effects.c`, `system.c` (clock and turn state), `allocator.c`, `containers.c`, `hooks.c`, `adapters/{io,invoker,log}.c` |
| **Node-specific** | ~4,600 | `napi_system.c`, `napi_services.c`, `napi_datamodel.c`, `values.c` (a `napi_ref` per box), `runtime.c` and `runtime_codecs.c`, `pump.c` (setImmediate and setTimeout), `addon.c` |
| **Generated, and JS** | ~6,000 C, ~1,500 generator Ruby, ~2,000 JS | control-plane bindings, the `lib/` facade and the builder |

The portable part is already covered by native tests under AddressSanitizer
and UBSan, because the step forks kept it free of Node-API on purpose.

## Proposed steps

1. **Split `pump.c`.** Its turn logic (budgets, turn order, quiet
   detection, deadlines) is portable. Only the setImmediate and setTimeout
   glue is Node-specific. The split moves more code into the kernel.
2. **Move the Node-API-free modules** into a shared library, for example
   `tinyactors-host/` or `tinyactors-core/host/`, with their native tests.
   The C prefix and the ownership need an ADR.
3. **Define the host interface.** Each host implements it:
   - values: make, retain, release, get (napi ADR 0011);
   - the data model adapter;
   - a pump driver: request a turn, arm a timer, read a monotonic clock;
   - error mapping;
   - hook calls into the host language.
4. **One generator emitter per host** over the same API description (napi
   ADR 0013). The policy file (`bindings.rb`) could be shared.
5. **Carry the napi ADRs over.** Most of them are about the kernel, not
   about Node: 0002 IDs, 0003 enumeration, 0004 interning, 0005 scheduling,
   0006 routing, 0007 the pump (with a different event loop), 0008 faults,
   0012 lifecycle and 0014 memory. Decide whether they move to the kernel's
   own ADR directory or stay in napi and are cited from there.

## What each language host still writes

| Piece | CPython | CRuby | Browser (WebAssembly) |
|---|---|---|---|
| Value boxes | `Py_INCREF` / `Py_DECREF` are the reference count | Ruby's GC must mark live values, so one marked array per system (napi ADR 0011's model B) fits | a JS array or an `externref` table |
| Data model | callables | procs | JS functions (the napi data model, reused) |
| Pump driver | asyncio `call_soon` / `call_later`, or a synchronous `run()` | a Fiber scheduler, a thread, or an explicit `run()` | `MessageChannel` or `scheduler.postTask`; `setTimeout` |
| Threading | GIL; a system stays on one thread | GVL; the same rule | one thread |
| Facade | builder, `ActorId`, `System` | the same | a host-neutral `lib/` shared with napi (a napi roadmap follow-up) |

## When to do it

Just before the second language host starts, not before. The extraction is
mechanical while the kernel has one user. Doing it earlier adds an interface
nothing tests against.
