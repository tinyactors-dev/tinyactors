// The shared tests' cases that do not run against @tinyactors/wasm, each
// with its reason (docs/plan.xml, p5-api; docs/browser.md, "One API"):
// what a case needs that the wasm package or the runtime does not have.
export const REASONS = {
  native: 'needs the native "ecmascript" datamodel, which @tinyactors/wasm does not have yet (p5-datamodel)',
  quickjs: 'needs quickjs, which @tinyactors/wasm does not have yet (p5-quickjs)',
  fs: 'needs node:fs',
  childProcess: 'needs node:child_process',
  workerThreads: 'needs node:worker_threads',
};

// file -> case -> reason, or [reason, runtimes...] for a case excluded in
// those runtimes only ('node', 'bun', 'browser'); '*' for every case of
// the file.
//
// The test task runs each file once against @tinyactors/wasm, on the
// native datamodel: the quickjs pass every file makes against
// @tinyactors/node is not made (p5-quickjs), and a case below that names
// quickjs is one the native pass makes with a quickjs host of its own.
export const EXCLUDED = {
  'address.test.mjs': {
    "an Address inside a value a chart takes is its text there": 'native',
    "send and fail take an Address as target, send an Address as origin": 'native',
  },
  'compile.test.mjs': {
    "a manifest compiles into an image whose actors boot by name": 'native',
    "compileCharts parses each expression and script: a syntax error is a warning at its line, an error with warningsAsErrors": 'native',
    "the image is a Uint8Array copied on first read, the same after, and it outlives close()": 'native',
  },
  'convenience.test.mjs': {
    "a continuation runs after the turn, and may call the host again": 'native',
    "a method pending at close never settles": 'native',
    "a promise rejects with the TinyactorsError onError gets: code, reason, path": 'native',
    "a refusal at the call rejects: a closed host, another object": 'native',
    "a rejection is the caller's: idle() resolves, the turn throws nothing": 'native',
    "a rejection with no catch is an unhandled rejection, not an uncaught throw from the turn": 'native',
    "each promise resolves with the result its direct method gives": 'native',
    "the direct form is unchanged: handlers, no promise, no onResult refused": 'native',
  },
  'datamodel.test.mjs': {
    "<foreach> declares its item and index, and walks the items it began with": 'native',
    "<log> of an object is its JSON; eval_string of a symbol is error.execution": 'native',
    "In() reads the session's configuration": 'native',
    "XML content is a document: elements, attributes, text, and a copy of it in a send": 'native',
    "XML content larger than the parser's 4 KB input window is read whole": 'native',
    "a chart that declares nothing: its sessions' variables are their own": 'native',
    "a script in a transition runs again: its let, const and class are its own at each run": 'native',
    "a script's top-level declarations are the session's, closures included (W3C 452)": 'native',
    "a syntax error is its own evaluation's error.execution: the chart's other expressions run": 'native',
    "actor.context: each variable as a neutral value (to_wire)": 'native',
    "error.execution says why: the exception's text, or no data for a failure the host decides": 'native',
    "the native datamodel: _event is frozen, kept until the next event, its data the payload's value": 'native',
    "the native datamodel: a declaring chart's session gets its data and params copied into its own context": 'native',
    "the native datamodel: a send's data is the payload's value copied into the application's realm, once": 'native',
    "the native datamodel: a typed method is never called from a chart, which cannot reach the Host": 'native',
    "the native datamodel: a value that cannot cross is refused at the input, the Host included": 'native',
    "the native datamodel: charts run in a context of their own, never the application's realm": 'native',
    "the native datamodel: data from the application is the chart's own, of its realm, and a copy": 'native',
    "the scope refuses an undeclared assignment, a system variable and _event": 'native',
    "the system variables are each session's own, _ioprocessors one object per session": 'native',
  },
  'docs.test.mjs': {
    "README.md:203 runs as written": 'native',
    "README.md:278 runs as written": 'native',
    "README.md:322 runs as written": 'native',
    "README.md:454 runs as written": 'native',
    "README.md:60 runs as written": 'native',
    "docs/guide/capacity.md:47 runs as written": 'native',
    "docs/guide/charts.md:112 runs as written": 'native',
    "docs/guide/charts.md:185 runs as written": 'native',
    "docs/guide/charts.md:243 runs as written": 'native',
    "docs/guide/charts.md:31 runs as written": 'native',
    "docs/guide/charts.md:350 runs as written": 'native',
    "docs/guide/charts.md:382 runs as written": 'native',
    "docs/guide/charts.md:432 runs as written": 'native',
    "docs/guide/charts.md:489 runs as written": 'native',
    "docs/guide/charts.md:537 runs as written": 'native',
    "docs/guide/hibernation.md:144 runs as written": 'native',
    "docs/guide/hibernation.md:27 runs as written": 'native',
    "docs/guide/inspection.md:162 runs as written": 'native',
    "docs/guide/inspection.md:231 runs as written": 'native',
    "docs/guide/inspection.md:275 runs as written": 'native',
    "docs/guide/inspection.md:9 runs as written": 'native',
    "docs/guide/inspection.md:95 runs as written": 'native',
    "docs/guide/io-processors.md:110 runs as written": 'native',
    "docs/guide/io-processors.md:15 runs as written": 'native',
    "docs/guide/io-processors.md:172 runs as written": 'native',
    "docs/guide/io-processors.md:231 runs as written": 'native',
    "docs/guide/migration.md:112 runs as written": 'native',
    "docs/guide/migration.md:235 runs as written": 'native',
    "docs/guide/migration.md:25 runs as written": 'native',
    "docs/guide/other-systems.md:112 runs as written": 'native',
    "docs/guide/other-systems.md:211 runs as written": 'native',
    "docs/guide/other-systems.md:24 runs as written": 'native',
    "docs/guide/quickjs.md:19 runs as written": 'quickjs',
    "docs/guide/services.md:122 runs as written": 'native',
    "docs/guide/services.md:186 runs as written": 'native',
    "docs/guide/services.md:250 runs as written": 'native',
    "docs/guide/services.md:31 runs as written": 'native',
    "docs/guide/snapshots.md:116 runs as written": 'native',
    "docs/guide/snapshots.md:14 runs as written": 'native',
    "docs/guide/telemetry.md:116 runs as written": 'native',
    "docs/guide/telemetry.md:153 runs as written": 'native',
    "docs/guide/telemetry.md:196 runs as written": 'native',
    "docs/guide/telemetry.md:21 runs as written": 'native',
    "docs/guide/testing.md:118 runs as written": 'native',
    "docs/guide/testing.md:161 runs as written": 'native',
    "docs/guide/testing.md:209 runs as written": 'native',
    "docs/guide/testing.md:21 runs as written": 'native',
  },
  'effects.test.mjs': {
    "a handler returning a promise: its view is filled before the ack and answers after it; idle() waits for it": 'native',
    "a handler that answers, then throws: the answer stands": 'native',
    "a handler that throws: error.communication for the sender, the error rejects idle()": 'native',
    "a log entry not read in its handler is stale after the ack": 'native',
    "a promise that rejects after an answer fails nothing": 'native',
    "a promise that rejects before an answer fails the send and stays unhandled": 'native',
    "a send view: each field read on first access and cached, valid until the ack": 'native',
    "a view kept and used from a bare timer is stale there": 'native',
    "a view read after its ack throws TA_ERROR_STALE; what was read stays": 'native',
    "an I/O processor without a handler fails each send to it": 'native',
    "an untraced send has no traceparent": 'native',
    "answer and fail: once, refused after either; options are checked": 'native',
    "answer keeps the send's trace; traceparent: null starts a new one": 'native',
    "answer() and fail() give true when delivered and false when the sender ended; a closed host still throws": 'native',
    "log entries: session, label, text, stale after the ack; none without a handler": 'native',
    "send.data is the application's: its arrays are Arrays here, and XML content is its text": 'native',
    "send.message: a text field the send lacks is null, so encoded it is absent to every binding": 'native',
    "the transport without a handler fails each send to another system": 'native',
    "the transport: send.target and send.origin are Addresses, send.message crosses to the other host": 'native',
    "views are made by the host only": 'native',
  },
  'growth.test.mjs': {
    "1000 sends from one <foreach>": 'native',
    "200 actors that each send one event at entry to one actor": 'native',
    "2000 actors that each send one event at entry to one actor": 'native',
    "300 invokes at entry, each with its done.invoke": 'native',
  },
  'hibernation.test.mjs': {
    "a chart whose scripts declare with let: native keeps it resident, quickjs hibernates it": 'native',
    "a chart whose scripts declare: its var and function come back after the wake": 'native',
    "a delayed send wakes the actor it was sent to when it falls due": 'native',
    "a function declared in a transition's script, and a 'use strict' that starts one, hibernate and wake": 'native',
    "a value outside the table keeps the actor resident, and the error names the variable": 'native',
    "a wake handler that throws: its error rejects idle()": 'native',
    "an actor wakes with its last event: an <onexit> at terminate reads it": 'native',
    "an actor wakes with its variables: the blob is a Uint8Array, one WAKE brings it back": 'native',
    "an async wake handler wakes the actor later, from its promise": 'native',
    "events wait until the wake: one WAKE however many come, then they run in order": 'native',
    "hibernate between turns ends no effect undispatched": 'native',
    "hibernate from a call inside a call outside a turn leaves no batch open": 'native',
    "hibernate from a handler joins the open batch": 'native',
    "hibernate refuses an actor with events waiting, and one hibernated already": 'native',
    "native: a Map, a class instance, a Date or an XML document in a variable keeps the actor resident": 'native',
    "native: a hibernated actor holds nothing in the engine; its values come back with the blob": 'native',
    "sweep, hibernate and wake refuse numbers out of range": 'native',
    "the sweep returns the actors that did not run since the last one": 'native',
    "wake refuses a blob it cannot read, and a session that is not hibernated": 'native',
    "wake refuses an older blob of the session: a store that fell behind loses no state": 'native',
    "without a wake handler the effect is dropped: the actor stays hibernated until wake": 'native',
  },
  'host.test.mjs': {
    "a host that names no datamodel gets the native one": 'native',
    "a run-time fetch (TA_EFFECT_REQUEST) is answered in C: the parent gets error.execution": 'native',
    "addImage takes a Uint8Array, a Buffer or an ArrayBuffer, and returns the segment": 'native',
    "chart gives a frozen reference; an unknown name is refused": 'native',
    "every input of a closed host is refused": 'native',
    "fail gives the sender error.communication": 'native',
    "one 2 MB send leaves the host arena under its cap": 'quickjs',
    "options are checked": 'quickjs',
    "send refuses what it cannot take, and keeps nothing of it": 'native',
    "send: a session or a local address, data, options, and the message shape": 'native',
    "sendMany returns the accepted prefix, in slices": 'native',
    "spawn: a session, its name, its params, its options": 'native',
    "terminate ends an actor; a session that is gone is refused": 'native',
  },
  'liveness.test.mjs': {
    "a process with only an open host exits": ['childProcess', 'browser'],
    "a process exits after an idle turn, the host still open": ['childProcess', 'browser'],
    "a process exits with a simulated clock moved": ['childProcess', 'browser'],
    "a process exits when a waiting idle() is rejected by close()": ['childProcess', 'browser'],
    "a pending deadline keeps the process alive until it comes": ['childProcess', 'browser'],
    "close() lets a process with a pending deadline exit": ['childProcess', 'browser'],
  },
  'loop.test.mjs': {
    "a handler's error, with no idle() waiting, is thrown from the turn's callback; the host goes on": ['childProcess', 'browser'],
    "TA_RUN_EFFECTS: a sender goes on after the drain acknowledged its send": 'native',
    "TA_RUN_EFFECTS_FULL: an outbox of one is drained and run again": 'native',
    "TA_RUN_YIELDED and the turn's bound: the event loop runs between turns": 'native',
    "a handler's error is whatever it threw: a string, a number, undefined and null reject idle() too": 'native',
    "a host on a simulated clock arms no timer": 'native',
    "a send runs its actor with no driver called": 'native',
    "close() clears the timer at a deadline": 'native',
    "close() from a handler is deferred to the end of the turn": 'native',
    "idle() waits for the promises handlers return: a log handler's, a typed method's, a service's": 'native',
    "one timer at the deadline: the actor runs when it comes": 'native',
  },
  'methods.test.mjs': {
    "a getter of params may call the host again: both methods are answered": 'native',
    "a handler that throws: the error rejects idle(), and the host goes on": 'native',
    "a typed method answers its own handle: a second method from onResult is answered too": 'native',
    "a typed method called from a handler is answered in a later take, after the ack": 'native',
    "an error goes to onError as a TinyactorsError with code, reason, path and message": 'native',
    "an error without onError is the turn's: idle() rejects with it": 'native',
    "close from a handler waits for the end of the turn: a reply the turn takes is still answered": 'native',
    "each typed method is answered once, at a later drain, with the host and the result": 'native',
    "params pass to core as given: core refuses a bad one in the reply": 'native',
    "pending handlers are never called after close; a method on a closed host throws": 'native',
    "the arguments are checked at the call, and a refused call leaves nothing pending": 'native',
  },
  'migration.test.mjs': {
    "a chart the host does not have, or a session that ended, is refused": 'native',
    "a delayed send survives the move with its data": 'native',
    "a hibernated actor is asked for with its migration": 'native',
    "a migration unfinished at close never reaches its handler": 'native',
    "a migrationDone handler that throws: its error rejects idle()": 'native',
    "a timeout is rounded up to the next tick": 'native',
    "an actor moves with its bindings: one MIGRATED, then migrationDone": 'native',
    "an actor with a value outside the table fails and stays on version 1": 'native',
    "an event's WAKE names no migration": 'native',
    "migrate refuses bad arguments": 'native',
    "native: a declaring chart migrates: its old names go, the new scripts' are declared": 'native',
    "one session or one chart": 'native',
    "the Migrated view: fields on first read, stale after the ack": 'native',
    "without wake a hibernated actor is left at the timeout, in pending": 'native',
    "without wake or timeout a hibernated actor holds the migration": 'native',
  },
  'rules.test.mjs': {
    "every rule in AGENTS.md names tests that exist": ['fs', 'browser'],
    "the README says what no test can show: charts are trusted, deadlines keep a process alive, errors end it, using": ['fs', 'browser'],
    "the services guide says a handler returns the service, or a promise of it": ['fs', 'browser'],
    "a Host posted to a worker arrives as a plain object, which every method refuses": 'native',
    "a dropped host with rows (charts, sessions, payloads) is collected and frees them in its finalizer": 'native',
    "a payload whose last reference goes is let go after the call: its value is collected": 'native',
    "a worker that ends with its hosts open frees them at its teardown, with no call of JavaScript": 'native',
    "marks nest in the host's arena: an input whose data's getter calls the host keeps its own texts": 'native',
    "outputs read in a handler stay valid across the later calls of its batch": 'native',
    "rows are handles: a slot reused under a new generation gives each session and payload its own": 'native',
    "sessions are bigints from 1 to 2^64-1 or safe integers from 1, never a number that saturates": 'native',
  },
  'scale.test.mjs': {
    "a restore costs per session at 200,000 sessions at most 1.5 times what it costs at 10,000": 'native',
    "every operation costs at 40,000 actors at most 1.5 times what it costs at 20,000": 'native',
  },
  'services.test.mjs': {
    "a dropped host whose running services close over it is collected": 'native',
    "a failure to start an invocation cancelled in the same run is not reported to the invoker": 'native',
    "a handler that returns a promise of the service: what comes while it starts is given to it in order": 'native',
    "a handler that throws is a communication error; a send to it fails too, until the UNINVOKE": 'native',
    "a handler's promise that rejects is a failed start: the invoker and each queued send are told": 'native',
    "a promise that settles after close, or after the invocation was cancelled, tells nobody": 'native',
    "a receive that throws is a crash: error.communication with the sendid, dropped not stopped": 'native',
    "a restore of a restore reports a missing service once": 'native',
    "a restore with a stand-in service: the copy never reaches the real one": 'native',
    "a restore without the service tells the invoker, and throws nothing": 'native',
    "a restored host announces each running invocation again, and its service is started": 'native',
    "a service done before the snapshot is not started again: its done.invoke is delivered": 'native',
    "a service done in its handler is neither kept nor stopped": 'native',
    "a service needs neither receive nor stop": 'native',
    "a service that crashed is not started again": 'native',
    "an UNINVOKE while it starts stops the service once it resolves; the sends before it are dropped": 'native',
    "an actor talks to its service and gets its done data": 'native',
    "an async receive reads its send after the ack; its rejection is a crash": 'native',
    "an event from a service names it as its origin, which reaches the service": 'native',
    "autoforward reaches the service, and its own answers too": 'native',
    "leaving the invoking state stops the service, once": 'native',
    "no service for the type is a communication error for the invoker, and nothing is thrown": 'native',
    "sendBack and done refuse what they cannot take": 'native',
    "services left at close are dropped, not stopped, and collected": 'native',
    "stop and receive are called on the service, with the host": 'native',
    "terminating the invoker stops the service": 'native',
    "the invocation's strings are shared across a population; its traceparent is the invoke's": 'native',
  },
  'shapes.test.mjs': {
    "directoryResolver refuses what leaves its root: .., an absolute path, a symbolic link": ['fs', 'browser'],
    "directoryResolver: text is UTF-8 or refused; other errors of the file system pass; the root must exist": ['fs', 'browser'],
    "Address.toJSON: a transport's send.message goes through JSON and back to host.send": 'native',
    "addCharts compiles, adds and closes in one call; the first key is the boot chart": 'native',
    "addCharts: the resolver option, and what it refuses": 'native',
    "directoryResolver reads source relative to base under its root": 'native',
    "spawnChart spawns the chart's current version, with spawn's options": 'native',
  },
  'snapshot.test.mjs': {
    "a declaring chart: its var and function come back after a restore": 'native',
    "a hibernated actor wakes after a restore": 'native',
    "a host goes on after a restore: variables, _event, a waiting payload, a delayed send on a new clock": 'native',
    "a snapshot during a migration is refused": 'native',
    "a snapshot from a handler is refused": 'native',
    "a snapshot holds the time of its call, and host.now() reads that clock on both hosts": 'native',
    "a snapshot restores from an unaligned buffer and from an ArrayBuffer": 'native',
    "a snapshot with a reply pending is refused": 'native',
    "a value nests as deep in a snapshot as in a send": 'native',
    "a value outside the table refuses the snapshot, and the host goes on": 'native',
    "one image in two segments keeps each session its own chart": 'native',
    "restore refuses another system's config, another datamodel, another program's pack": 'native',
    "the runtime's clock resumes at the snapshot's time": 'native',
    "the snapshot of a restored host is the same bytes": 'native',
  },
  'telemetry.test.mjs': {
    "a close a handler asked for: its last take's error rejects the waiting idle()": 'native',
    "a handler that returns a promise runs in the turn; asyncDispose awaits the last take's": 'native',
    "a handler that throws: idle() rejects, the encoder goes on, close() throws the last take's": 'native',
    "a watch ends on the real clock, through the host's timer": 'native',
    "a watch traces what it matches until it ends, on a simulated clock": 'native',
    "actor spans are children of the application's span, and the trace goes out to it": 'native',
    "asyncDispose rejects with a promise's rejection in the last take": 'native',
    "close hands over the last take: trace.end and what is staged": 'native',
    "close() from the handler of the last take is the close running": 'native',
    "inputs from the handler of the last take end: it is bounded": 'native',
    "metrics are pulled outside a drain: its own batch, acknowledged at once": 'native',
    "metrics from a handler join the open batch": 'native',
    "metrics from the handler of the last take join its batch": 'native',
    "traces and logs reach the handler as Uint8Arrays, copied once, after flushInterval": 'native',
    "without a handler the requests are taken and dropped, so the encoder goes on": 'native',
  },
};

// Whole files that do not load in a page: what they import at their top
// needs the runtime (a file is excluded where it would not load).
export const EXCLUDED_FILES = {
  'docs.test.mjs': ['childProcess', 'browser'], // runs each example of the documentation as a process
  'links.test.mjs': ['fs', 'browser'], // reads the package's documents
  'scale.test.mjs': ['childProcess', 'browser'], // runs each size as a process (tools/scale.mjs)
};

const applies = (entry, runtime) => {
  const [reason, ...where] = Array.isArray(entry) ? entry : [entry];
  return where.length === 0 || where.includes(runtime) ? REASONS[reason] ?? reason : undefined;
};

/** The cases of `file` excluded under `runtime`: name -> reason's text. */
export function exclusionsOf(file, runtime) {
  const out = new Map();
  for (const [name, entry] of Object.entries(EXCLUDED[file] ?? {})) {
    const reason = applies(entry, runtime);
    if (reason !== undefined) out.set(name, reason);
  }
  return out;
}

/** Why `file` is excluded whole under `runtime`, or undefined. */
export function fileExcluded(file, runtime) {
  return EXCLUDED_FILES[file] === undefined ? undefined : applies(EXCLUDED_FILES[file], runtime);
}

/** How many entries each reason has (a whole file counts once). */
export function counts() {
  const out = {};
  const add = (entry) => {
    const [reason] = Array.isArray(entry) ? entry : [entry];
    out[reason] = (out[reason] ?? 0) + 1;
  };
  for (const cases of Object.values(EXCLUDED)) for (const entry of Object.values(cases)) add(entry);
  for (const entry of Object.values(EXCLUDED_FILES)) add(entry);
  return out;
}
