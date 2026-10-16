# @tinyactors/node

**Status: being rewritten** from core's redesigned API ([ROADMAP.md](ROADMAP.md)).
The last published version, 0.2.0, was built from an earlier binding.

## Before you use it

- **Chart code is trusted code.** On the native datamodel (the default)
  a chart's expressions and scripts run in `node:vm` contexts of your own
  engine, and a `vm` context is not a security boundary. Run charts you do
  not trust on quickjs, the sandbox: `new Host(config, { datamodel:
  'quickjs', quickjs: { maxOps, memoryLimit } })`.
- **An open host with a pending deadline keeps the process alive** (a
  delayed `<send>`, a timeout): the host arms one timer at its next
  deadline, as `setTimeout` does. `host.close()` clears it, and the process
  can exit.
- **A handler's error ends the process by default.** An error thrown by
  a handler (an I/O processor's, the log handler, a typed method's)
  rejects a waiting `host.idle()`; with none waiting it is thrown from the
  host's turn as an uncaught exception, and a handler's promise that
  rejects is an unhandled rejection. Either ends a Node.js or Bun process
  unless the application handles `uncaughtException` and
  `unhandledRejection`.
- **`using host = new Host(...)` needs a runtime that parses `using`:**
  Bun 1.4 does, Node.js 22 does not. `Symbol.dispose` and
  `Symbol.asyncDispose` call `close()`, which works everywhere.
- **A `<script>` that declares top-level names gives each session a
  context of its own.** A chart whose scripts declare `var`, `function`,
  `let`, `const` or `class` at their top level runs each of its sessions
  in a `vm` context of its own (about 143 KB a session on Node.js, 60 KB
  on Bun), so the script's functions close over its own variables. A
  chart that declares nothing shares one context per host: declare the
  function as a `<data>` and assign it in the script
  (`<data id="price"/>`, `price = (sku) => ...`).
