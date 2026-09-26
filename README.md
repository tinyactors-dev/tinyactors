# Tinyactors

Tinyactors is an implementation of actor system based on the [SCXML](https://w3.org/TR/scxml) standard.

It is tiny, because it doesn't perform I/O, use threads, or directly allocate memory: the core is libc-free.

This design is chosen to make it easy to embed Tinyactors into existing runtimes, which
have already made decisions about all of these things.

Tinyactors just bring a robust implementation of the semantics to the table, usable everywhere:

- in the browser through a wasm-freestanding target,
- in your favorite programming language through bindings of the native binary,
- in games, because the implementation is resource-efficient.

The actor model makes modeling distributed systems much more tractable than purely
imperative code, or sequential code written in an functional style with async/await.

There purposely is no "let's pretend code is sequential" API for Tinyactors: you'll have
to model states and transitions explicitly.

Thanks to the statecharts formalism, you can safely compose multiple state machines without
drowning in a combinatorial explosion of states and transitions.

If you ever had to manage the state of a live websocket connection, you have experienced
the uneasiness of that state management logic not having clear home in your system. Retries,
reconnects, timeouts, reconnect storms all become major headaches as a result.

Using the actor model, you can model just how a websocket connection behaves on your server,
*without* having to keep the full system in your head.

This is good for humans and for agents.

## Status

Under active development - breaking changes are made frequently to land on a good API.

## Repository layout

- `tinyactors-core/` contains the freestanding-friendly C API and production
  interpreter implementation.
- `tinyactors-napi/` contains the Node-API binding for Node.js 22 and Bun.
- `tinyactors-test/` contains the instrumentable JSON-RPC test host and its
  Ruby driver.
- `tinyactors-vscode/` contains the VS Code extension that registers the
  debug type for both hosts' debug adapters.
- `docs/proposals/` holds outlines of possible future work, such as new
  hosts. They are not plans or decisions; see its README.
- `docs/research/` holds background research that informed the designs,
  with sources, so it need not be repeated.

Run `mise run build` to build both projects and `mise run test` to exercise the
test-host protocol.
