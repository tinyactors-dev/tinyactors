// The types of @tinyactors/node. Every export has a doc comment, which an
// editor shows on hover; README.md and docs/guide/ teach the same API in
// order, with examples. Each declaration is a row of docs/api-map.md,
// which names the core behind it.
//
// Units: time is milliseconds (but telemetry.flushInterval, in the host's
// clock ticks, and traceWatch's seconds); sessions are bigints (inputs also
// take a safe integer from 1, and typed methods' results hold numbers);
// bytes the host hands out are Buffers, copied once, yours to keep.

// ---- values -------------------------------------------------------------

/** An actor's session, as an input takes it: a `bigint` from 1 to
 * 2^64-1, or a number that is a safe integer from 1 (as a database or
 * JSON gives one back). Anything else is a `TypeError`. The calls and
 * views return a `bigint`; a typed method's result holds a number, so
 * compare with `Number(session) === listed`. */
export type Session = bigint | number;

/** A value that crosses between your program and a chart: event data,
 * spawn params, a typed method's result, and what `encode` and `decode`
 * convert. Numbers that are safe integers stay integers; integers past
 * 2^53 are `bigint`s (up to 64 bits). Plain objects only: a function, a
 * `Map`, a `Date` or a class's instance is refused. Values are copied when
 * they cross. Inside a value an actor's address is its text. */
export type Value =
  | undefined
  | null
  | boolean
  | number
  | bigint
  | string
  | Uint8Array
  | Value[]
  | { [key: string]: Value };

/** A value as CBOR bytes, the form to store or send it in.
 *
 * Throws a `TinyactorsError`: `TA_ERROR_UNSUPPORTED` for what is not a
 * {@link Value} (a function, a symbol, a `Date`, a `Map`, a class's
 * instance, a typed array other than `Uint8Array`), `TA_ERROR_INVALID` for
 * what a value cannot hold (a string with a lone surrogate, a `bigint` past
 * 64 bits, a cycle or nesting deeper than 64). A getter's exception passes
 * through.
 *
 * @example
 * const bytes = encode({ sku: 'tea', qty: 2 });
 * decode(bytes); // { sku: 'tea', qty: 2 }
 */
export function encode(value: Value): Uint8Array;

/** The value of the one CBOR item that fills `bytes`: the inverse of
 * {@link encode}. Throws a `TinyactorsError` (`TA_ERROR_INVALID`) for bytes
 * that are not one such item, a `TypeError` for an argument that is not
 * bytes. */
export function decode(bytes: Uint8Array | ArrayBuffer): Value;

// ---- errors -------------------------------------------------------------

/** The reason a call was refused, as `TinyactorsError.status` names it:
 * - `TA_ERROR_INVALID`: an argument the host does not take (a chart it
 *   does not have, a config key, malformed bytes);
 * - `TA_ERROR_STALE`: something that ended or never was (a session, a
 *   view read after its handler returned, a closed compile result);
 * - `TA_ERROR_STATE`: not now (a snapshot from a handler, a second
 *   answer, hibernating an actor that is not at rest);
 * - `TA_ERROR_UNSUPPORTED`: a value outside the value table;
 * - `TA_ERROR_NO_MEMORY`: a pool at its largest size, or the system
 *   allocator refused. */
export type Status =
  | 'TA_ERROR_INVALID'
  | 'TA_ERROR_NO_MEMORY'
  | 'TA_ERROR_STATE'
  | 'TA_ERROR_STALE'
  | 'TA_ERROR_UNSUPPORTED'
  | 'TA_ERROR_EXECUTION'
  | 'TA_ERROR_COMMUNICATION'
  | 'TA_ERROR_ABORTED'
  | 'TA_ERROR_FATAL'
  | (string & {});

/** An error the host reported. A refused call has a `status` to branch
 * on; a typed method's error, and a refused config, have `code`, `reason`
 * and `path`, as the host wrote them. The message is for
 * people: do not match on it.
 *
 * @example
 * try { host.send(session, 'go'); }
 * catch (error) { if (error instanceof TinyactorsError && error.status === 'TA_ERROR_STALE') ... }
 */
export class TinyactorsError extends Error {
  constructor(
    message?: string,
    options?: { code?: number; reason?: string; path?: string; status?: Status },
  );
  /** A typed method's error: its JSON-RPC code (`-32001` for a session that
   * does not exist, `-32602` for a param it does not take, ...). */
  readonly code: number | undefined;
  /** A typed method's error: a word for its cause (`stale`, `type`,
   * `unknown`, `range`, `missing`, `state`, ...). */
  readonly reason: string | undefined;
  /** A typed method's error: the param it is about (`params.session`). */
  readonly path: string | undefined;
  /** A refused call's reason. */
  readonly status: Status | undefined;
}

// ---- compiling ----------------------------------------------------------

/** A document to compile: its name, which error messages and `src`
 * attributes name, and its text. */
export interface Source {
  name: string;
  text: string;
}

/** One message of the compiler about a document. */
export interface Diagnostic {
  readonly severity: 'error' | 'warning' | 'note';
  /** The compiler's code for the kind of problem. */
  readonly code: number;
  /** The document's name, as its {@link Source} gave it. */
  readonly file: string;
  /** From 1. */
  readonly line: number;
  /** From 1. */
  readonly column: number;
  readonly message: string;
}

/** A compilation found an error, and compiled nothing. Its message is the
 * first error as a compiler prints it, `file:line:column: error:
 * message`; its status is `TA_ERROR_INVALID`. */
export class CompileError extends TinyactorsError {
  /** Every diagnostic of the compilation: errors, warnings and notes. */
  readonly diagnostics: readonly Diagnostic[];
}

/** Thrown by a {@link Resolver} to refuse a document: the refusal becomes
 * a compile error of the resolved file, at `line` and `column` when given,
 * and the compile call throws a `CompileError`. */
export class ResolveError extends TinyactorsError {
  constructor(message: string, options?: { line?: number; column?: number });
  readonly line: number | undefined;
  readonly column: number | undefined;
}

/** Reads a document a chart names with `src` (`<invoke src>`, `<script
 * src>`, `<data src>`, a manifest's `<chart src>`): the text of `source`,
 * named by the document `base`. Called during the compile call only.
 * Throw a `ResolveError` to refuse; any other error is thrown from the
 * compile call as it is.
 *
 * @example
 * const resolver = (base, source) => readFileSync(join(root, source), 'utf8');
 */
export type Resolver = (base: string, source: string) => string;

/** What a compile call returns: the image and the compiler's warnings. */
export interface Compiled extends Disposable {
  /** The compiled image, for `host.addImage`: a `Buffer`, copied on the
   * first read and yours from then on (write it to a file, load it
   * elsewhere). A first read after `close()` throws `TA_ERROR_STALE`. */
  readonly image: Uint8Array;
  /** The warnings and notes of the compilation. */
  readonly diagnostics: readonly Diagnostic[];
  /** Frees the compiler's memory now; the garbage collector does it
   * otherwise. Read `image` first. */
  close(): void;
  /** `close()`. */
  [Symbol.dispose](): void;
}

/** Compiles SCXML documents into one image. Each chart is known by its
 * `<scxml name>`. A `src` is answered by a source of that name or by
 * `resolver`. The first source is the boot chart, which `host.boot`
 * spawns. Throws `CompileError` with every diagnostic.
 *
 * @example
 * const compiled = compileCharts([{ name: 'order.scxml', text }]);
 * const segment = host.addImage(compiled.image);
 * compiled.close();
 */
export function compileCharts(sources: readonly Source[], resolver?: Resolver): Compiled;

/** Compiles a system manifest: a `<system>` document of `<chart name
 * src>`s, read through `resolver`, and `<actor name chart>`s, which
 * `host.boot` spawns with their names. Throws `CompileError`. */
export function compileSystem(manifest: Source, resolver?: Resolver): Compiled;

// ---- addresses ----------------------------------------------------------

/** An actor's address: `tinyactors://<system>/#_scxml_<session>`, or
 * `tinyactors://<system>/@<name>` for a named actor. Immutable. Its parts
 * are read from the text on each access, `undefined` for a part the text
 * lacks (any text makes an `Address`; one that is not an address has no
 * parts). */
export class Address {
  constructor(text: string);
  /** The address of a session of system `system`. */
  static fromSession(system: string, session: Session): Address;
  /** The address of the actor named `name` in system `system`; the name is
   * percent-encoded. */
  static fromName(system: string, name: string): Address;
  readonly text: string;
  /** The system id. */
  readonly system: string | undefined;
  readonly session: bigint | undefined;
  /** The actor's name, decoded. */
  readonly name: string | undefined;
  /** The text. */
  toString(): string;
}

// ---- BasicHTTP ----------------------------------------------------------

/** A form field of a BasicHTTP message. */
export interface HTTPField {
  name: string;
  value: string;
}

/** A BasicHTTP POST, the request SCXML's HTTP processor makes for a send. */
export interface HTTPPost {
  /** `http://host[:port]/path`. */
  url: string;
  /** Written as the `_scxmleventname` field. */
  event?: string;
  fields?: readonly HTTPField[];
  /** The body, used when there are no fields; percent-encoded. */
  body?: string;
}

/** An incoming BasicHTTP request, read. */
export interface HTTPRequest {
  readonly method: string;
  readonly target: string;
  /** The body as received. */
  readonly body: string;
  /** The `_scxmleventname` field, else `HTTP.<METHOD>`. */
  readonly event: string;
  /** The decoded form fields, but `_scxmleventname`. */
  readonly fields: readonly HTTPField[];
}

/** The bytes of a BasicHTTP POST, to write to a socket. A URL that is not
 * `http://host[:port]/path` throws `TA_ERROR_INVALID`. */
export function encodeHTTPPost(post: HTTPPost): Uint8Array;

/** Reads an incoming BasicHTTP request: its event and form fields, for
 * `host.send`. A malformed request throws `TA_ERROR_INVALID`. */
export function parseHTTPRequest(message: Uint8Array | ArrayBuffer): HTTPRequest;

// ---- the clock ----------------------------------------------------------

/** A clock for tests and simulations, which moves only when you move it.
 * Give it to a host as `options.clock`; a host on it arms no timer.
 *
 * @example
 * const clock = new SimulatedClock();
 * const host = new Host(config, { clock });
 * clock.increment(30_000); // a 30-second timeout, at once
 * await host.idle();
 */
export class SimulatedClock {
  constructor();
  /** Milliseconds since the clock was made. */
  readonly now: number;
  /** Moves the clock `ms` forward and schedules a turn on every host made
   * with it; `await host.idle()` to see what ran. What is due runs once, at
   * the new time, and a delay it starts counts from there: to see each of
   * several deadlines, step to each. */
  increment(ms: number): void;
}

// ---- what handlers get ----------------------------------------------------

/** One `<send>` of a chart, as an I/O processor's handler, the transport
 * or a service's `receive` gets it. A view of the host's memory: each
 * field is read on first access, while the handler runs; after the
 * handler returned, a read throws `TA_ERROR_STALE`. When the handler
 * returns a promise, every field is read before the host moves on, and
 * the view stays valid, `answer` and `fail` included, while the host is
 * open. */
export interface Send {
  /** The sender. */
  readonly session: bigint;
  /** The event's name. */
  readonly event: string;
  /** `<send target>`: a string; an `Address` for the transport's sends;
   * `undefined` when the send has none. */
  readonly target: string | Address | undefined;
  /** `<send id>`, or the id the host generated; `undefined` when none. */
  readonly sendid: string | undefined;
  /** The `<param>`s as an object, or the `<content>`'s value. */
  readonly data: Value;
  /** The I/O processor's type; `undefined` for the transport and a service. */
  readonly type: string | undefined;
  /** The sender's address. */
  readonly origin: Address | undefined;
  /** The sending step's trace as W3C `traceparent` text, for the header of
   * the request you make; `undefined` when untraced. */
  readonly traceparent: string | undefined;
  /** The outgoing half, `{ target, event, data, sendid, origin,
   * traceparent }`: what `host.send(message)` takes on the other side,
   * unchanged. The transport's reason to exist; present on every send. Its
   * addresses are `Address`es, which `JSON.stringify` writes as `{ text }`:
   * convert them with `String()` for a wire. */
  readonly message: SendMessage;
  /** Sends `event` back to the sender, with this send's `sendid`. It
   * continues the send's trace unless `options.traceparent` is given
   * (`null`: a new trace). Once: a second answer, or one after `fail()`,
   * throws `TA_ERROR_STATE`. */
  answer(event: string, data?: Value, options?: AnswerOptions): void;
  /** Gives the sender `error.communication` with this send's `sendid`:
   * the request could not be done. */
  fail(): void;
}

/** The options of `send.answer`. */
export interface AnswerOptions {
  /** The answer's trace: W3C text, or `null` for a new trace. Absent: the
   * send's. */
  traceparent?: string | null;
  /** The answer's `_event.origin`. */
  origin?: string | Address;
  /** The message as you read it, for the chart's `_event.raw`. */
  raw?: string;
}

/** An event for `host.send(message)` and `host.sendMany(items)`, in the
 * shape `send.message` has. Keys it does not know are ignored. */
export interface SendMessage {
  /** A session, or an address (string or `Address`); a local address is
   * resolved to its actor. */
  target: Session | string | Address;
  event: string;
  data?: Value;
  /** The event's `_event.sendid`. */
  sendid?: string;
  /** The event's `_event.origin`: where an answer goes. */
  origin?: string | Address;
  /** The trace the event continues, as W3C text; text that does not parse
   * starts a new one. */
  traceparent?: string;
}

/** A `<log>` line: a view, valid while the log handler runs. */
export interface LogEntry {
  /** The actor that logged. */
  readonly session: bigint;
  /** `<log label>`; `''` when it has none. */
  readonly label: string;
  /** The value of `<log expr>`, as text. */
  readonly text: string;
}

/** An actor a migration moved: a view, valid while the handler runs. */
export interface Migrated {
  readonly session: bigint;
  /** The segment of its new version. */
  readonly segment: number;
  /** The chart's name. */
  readonly chart: string;
  /** The migration's id, as `host.migrate` returned it. */
  readonly migration: number;
}

/** A migration's end: how many actors it moved, how many failed (they stay
 * on their version), and the sessions its timeout left behind. */
export interface MigrationResult {
  readonly migration: number;
  readonly migrated: number;
  readonly failed: number;
  readonly pending: readonly bigint[];
}

/** An `<invoke>` of a service your program runs, as a service handler
 * gets it. A record: keep it and use it while the service runs. */
export interface Invocation {
  /** The invoking actor. */
  readonly session: bigint;
  /** The `<invoke id>`, or the id the host generated. */
  readonly invokeid: string;
  /** `<invoke type>`. */
  readonly type: string;
  /** `<invoke src>`. */
  readonly src: string;
  /** The `<param>`s as an object, or the `<content>`'s value. */
  readonly data: Value;
  /** The invoking step's trace, as W3C text. */
  readonly traceparent: string | undefined;
  /** True when a restored host announces an invocation that was running
   * at its snapshot: start it again, or a stand-in. */
  readonly restored: boolean;
  /** False once the invocation ended (done, cancelled, failed) or the host
   * closed. */
  readonly running: boolean;
  /** Sends `event` to the invoker, with `_event.invokeid` set. Continues
   * the invoke's trace unless given one (`null`: a new trace). Throws
   * `TA_ERROR_STALE` once the invocation ended. */
  sendBack(event: string, data?: Value, options?: { traceparent?: string | null }): void;
  /** Ends the service: the invoker gets `done.invoke.<invokeid>` with
   * `data`, and the service is dropped without `stop`. Throws
   * `TA_ERROR_STALE` once the invocation ended. */
  done(data?: Value, options?: { traceparent?: string | null }): void;
}

/** What a service handler returns: any object, kept by the host until the
 * invocation ends, whose methods the host calls. At `host.close()` a
 * running service is dropped, not stopped. */
export interface Service {
  /** A send of the invoker to the service (`<send target="#_<invokeid>">`,
   * or an autoforwarded event); `send.type` is `undefined`. A throw, or a
   * promise that rejects, is a crash: the invoker gets
   * `error.communication` and the service is dropped. */
  receive?(host: Host, send: Send): void | Promise<void>;
  /** The invoker left the state, which cancels the invocation. */
  stop?(host: Host): void;
}

// ---- the host -----------------------------------------------------------

/** A chart's current version in a host, from `host.chart(name)`, for
 * `host.spawn`. */
export interface ChartRef {
  /** The segment of the image that holds it. */
  readonly segment: number;
  /** Its index in that image. */
  readonly chart: number;
}

/** The host's config: plain values, checked by the host, every absent key
 * a default.
 * - `systemID` (required): this system's id, part of every address;
 * - `ioProcessors`: `[{ type, location? }]`, the I/O processors the charts
 *   may send to;
 * - `system`: each segment's first sizes (`maxActors`, `maxEvents`,
 *   `maxTimers`, `maxRequests`, `workspaces`, `instanceBytes`,
 *   `stringBytes`), `ticksPerSecond` (default 1e9), `tracing`, and
 *   `traceSeed` (default 0), the seed of the trace IDs: the same seed gives
 *   the same IDs, so give each process its own;
 * - `telemetry`: present to turn on the OTLP encoder (`flushInterval`,
 *   `resource`, `logs`, `values`, `linkParents`, ...);
 * - `migrateQuantum`: actors a migration looks at per run.
 *
 * A key the host does not take, or a value of the wrong type, throws a
 * `TinyactorsError` (`TA_ERROR_INVALID`) at `new Host`, with the host's
 * `reason` and the key as `path` (`system.maxActors`). `host.hostConfig` answers the config with every default. */
export type HostConfig = { [key: string]: Value };

/** What a handler returns: nothing, or a promise. A send whose handler
 * returns a promise is read in full before the host moves on, and may be
 * answered when the promise gets there. */
export type Handled = void | PromiseLike<unknown>;

/** The handlers and choices of a host. A handler's error is reported after
 * its turn: it rejects a waiting `host.idle()`, or else is thrown as an
 * uncaught exception. */
export interface HostOptions {
  /** The handler of each I/O processor type the config lists. A listed
   * type without a handler fails each send to it; a handler for a type the
   * config does not list is a `TypeError`. */
  ioProcessors?: { [type: string]: (host: Host, send: Send) => Handled };
  /** Sends to another system's address. Pass `send.message` to the other
   * side's `host.send`. Without one, each such send is failed. */
  transport?: (host: Host, send: Send) => Handled;
  /** `<log>` lines; without one they are dropped. */
  log?: (host: Host, entry: LogEntry) => void;
  /** A `SimulatedClock`; without one the host reads the runtime's clock
   * and arms a timer at its next deadline. */
  clock?: SimulatedClock;
  /** Your exporter: each OTLP traces or logs request the encoder wrote
   * (protobuf, for a collector's `/v1/traces` or `/v1/logs`), as a
   * `Buffer`. Needs `telemetry` in the config; without a handler the
   * requests are dropped. It runs in the host's turn: start the post and
   * return. At `close()` it gets the last requests; the promises it
   * returns then are awaited by `[Symbol.asyncDispose]`. */
  telemetry?: (host: Host, signal: 'traces' | 'logs', data: Uint8Array) => Handled;
  /** A hibernated actor has work: an event or a due delayed send
   * (`migration` undefined), or a migration asks for it (its id). Load its
   * blob and call `host.wake`, now or later. Called once per hibernation;
   * without it the actor stays hibernated, and its events wait, until
   * `host.wake`. */
  wake?: (host: Host, session: bigint, migration: number | undefined) => Handled;
  /** Each actor a migration moved. */
  migrated?: (host: Host, migrated: Migrated) => void;
  /** The end of each migration. */
  migrationDone?: (host: Host, result: MigrationResult) => void;
  /** The handler of each `<invoke type>` your program runs: it returns the
   * service, or a promise of it. Until the promise settles the sends to
   * the invocation wait, in order; a rejection is a failed start, and the
   * invoker gets `error.communication`. */
  services?: { [type: string]: (host: Host, invocation: Invocation) => Service | void | PromiseLike<Service | void> };
  /** Where chart code runs: `'native'` (the default), `node:vm` contexts
   * of your own engine, for trusted charts; or `'quickjs'`, the sandbox,
   * for charts you do not trust. */
  datamodel?: 'native' | 'quickjs';
  /** The sandbox's limits, with `datamodel: 'quickjs'` only (else a
   * `TypeError`). */
  quickjs?: QuickJSOptions;
}

/** The quickjs sandbox's limits. Absent: the engine's default. */
export interface QuickJSOptions {
  /** Interrupt checks one evaluation may make before it is stopped with
   * `error.execution` (default 1,000,000, which lets a busy loop run for
   * many seconds). */
  maxOps?: number;
  /** Bytes for the whole engine, every actor together (default: none). An
   * evaluation that needs more is stopped with `error.execution`. */
  memoryLimit?: number;
  /** Bytes of native stack for JavaScript. */
  stackLimit?: number;
  /** `'auto'` (the default): the actors of a chart that declares no
   * top-level names share one context and its builtins; `'isolated'`: a
   * context per actor. */
  realm?: 'auto' | 'isolated';
}

/** The options of `host.spawn`. */
export interface SpawnOptions {
  /** The actor's name: `host.actorByName` finds it, `_name` holds it, and
   * its address is `tinyactors://<system>/@<name>`. */
  name?: string;
  /** Initial values: each key that names a top-level `<data>` replaces that
   * variable's initial value; keys that name none are left out. */
  params?: Value;
  /** The trace the actor's start continues, as W3C text. */
  traceparent?: string;
}

/** The options of `host.send(target, event, data, options)`. */
export interface SendOptions {
  /** The event's `_event.sendid`. */
  sendid?: string;
  /** The trace the event continues, as W3C text; text that does not parse
   * starts a new one. */
  traceparent?: string;
  /** The event's `_event.origin`. */
  origin?: string | Address;
}

/** The options of `host.migrate`. */
export interface MigrateOptions {
  /** A chart's name; none: every chart. */
  chart?: string;
  /** One actor. */
  session?: Session;
  /** Ask the `wake` handler for the hibernated actors, with the
   * migration's id. */
  wake?: boolean;
  /** Milliseconds of the host's clock from now, rounded up to its next
   * tick; 0 or none: no timeout. */
  timeout?: number;
}

/** A typed method's params, passed to the host as given; `undefined` for
 * none. */
export type Params = { [key: string]: Value } | undefined;
/** A typed method's result handler, called once, in a later turn. */
export type OnResult = (host: Host, result: Value) => void;
/** A typed method's error handler. Without one, the error rejects a
 * waiting `idle()` or is thrown as an uncaught exception. */
export type OnError = (host: Host, error: TinyactorsError) => void;

/** Runs actors: many charts, many images, from a handful of actors to
 * millions, on the runtime's event loop. Inputs (`spawn`, `send`, ...)
 * apply at once and schedule a turn; in its turns the host runs the actors
 * and calls your handlers with what they sent out. There is no `run()`:
 * `await host.idle()` waits until there is nothing left to do.
 *
 * An open host with a pending deadline keeps the process alive, as a
 * `setTimeout` does; `close()` ends it.
 *
 * @example
 * const host = new Host({ systemID: 'shop' }, { log: (host, entry) => console.log(entry.text) });
 * host.addImage(compileCharts([{ name: 'order.scxml', text }]).image);
 * const order = host.spawn(host.chart('order'));
 * host.send(order, 'checkout');
 * await host.idle();
 * host.close();
 */
export class Host implements Disposable, AsyncDisposable {
  /** A host for `config`, with the handlers and choices of `options`.
   * Throws `TinyactorsError` (`TA_ERROR_INVALID`) for a config the host
   * does not take, `TypeError` for options it does not take. */
  constructor(config: HostConfig, options?: HostOptions);
  /** A new host running a snapshot `host.snapshot()` gave, with the config
   * it was taken with (its `systemID` is checked) and the options `new Host`
   * takes, its `datamodel` the snapshot's. Every session stays the same, and
   * its clock goes on from the snapshot's time, whatever clock reads it.
   * Invocations that were running are announced again to `services`, with
   * `invocation.restored` true. */
  static restore(snapshot: Uint8Array | ArrayBuffer, config: HostConfig, options?: HostOptions): Host;

  /** The host's clock rate, `system.ticksPerSecond`. */
  readonly ticksPerSecond: number;
  /** True once `close()` ran; every input of a closed host throws. */
  readonly closed: boolean;
  /** Milliseconds from the last idle turn to the next deadline (a delayed
   * send, a timeout); `undefined` for none. */
  readonly nextDeadline: number | undefined;
  /** Not built yet: reads `undefined` on every host. Planned: the quickjs
   * engine's heap in bytes. */
  readonly quickjsHeapBytes: number | undefined;

  /** Loads a compiled image, which it copies, and returns its segment, a
   * number. A chart whose name the host already has gets a new version:
   * new actors are of it, running ones stay on theirs until `migrate`.
   * Throws `TA_ERROR_INVALID` for bytes that are not an image. */
  addImage(image: Uint8Array | ArrayBuffer): number;
  /** The current version of chart `name`, for `spawn`. Throws
   * `TA_ERROR_INVALID` when the host has no chart of that name. */
  chart(name: string): ChartRef;
  /** Spawns the boot actors of the image of `segment`: a system manifest's
   * `<actor>`s, or an actor of `compileCharts`' first chart. */
  boot(segment: number): void;
  /** Starts an actor of `ref` and returns its session. It runs in a later
   * turn. */
  spawn(ref: ChartRef, options?: SpawnOptions): bigint;
  /** Puts `event`, with `data`, in the queue of the actor `target` names (a
   * session or an address). Throws `TA_ERROR_STALE` for an actor that
   * ended. */
  send(target: Session | string | Address, event: string, data?: Value, options?: SendOptions): void;
  /** The same, from a `SendMessage` (the transport's `send.message`). */
  send(message: SendMessage): void;
  /** Tells a sender that its send could not be delivered: the actor
   * `sender` (a session or its address, the send's `origin`) gets
   * `error.communication` with `sendid`. */
  fail(sender: Session | string | Address, sendid?: string): void;
  /** Sends each of `items` in order, and returns how many were taken: the
   * rest, from the first one refused, were not sent. */
  sendMany(items: readonly SendMessage[]): number;
  /** Ends the actor. Throws `TA_ERROR_STALE` for one that already ended. */
  terminate(session: Session): void;
  /** The running actor spawned with `name`, newest segment first, or
   * `undefined`. */
  actorByName(name: string): bigint | undefined;

  /** Resolves at the next turn that finds nothing to do; the same promise
   * to every caller until then. Rejects with the turn's first error (a
   * handler's, a typed method's without `onError`), or with
   * `TA_ERROR_STATE` when the host is closed first. It waits for the host
   * only, not for your promises. */
  idle(): Promise<void>;

  /** An actor's active states, `{ value }`: a state id, or `{ parent:
   * child }` for compound states. Params `{ session }`. The typed methods
   * read the host when called, and call `onResult` or `onError` once, in a
   * later turn; `close()` drops them uncalled. */
  actorState(params: Params, onResult: OnResult, onError?: OnError | null): void;
  /** An actor's variables, `{ variables }`. Params `{ session }`; refused
   * for a hibernated actor. */
  actorContext(params: Params, onResult: OnResult, onError?: OnError | null): void;
  /** The host's sessions a page at a time, `{ sessions, cursor }`, `cursor`
   * `null` after the last page. Params `{ cursor?, limit? }` (1 to 512,
   * default 256). */
  actorList(params: Params, onResult: OnResult, onError?: OnError | null): void;
  /** One actor, `{ session, segment, chart, parent, mailbox, invocations }`.
   * Params `{ session }`. */
  actorGet(params: Params, onResult: OnResult, onError?: OnError | null): void;
  /** The host's counters, `{ host, pools, charts }`: `pools` has one entry
   * per live segment with each pool's `capacity`, `used` and `highWater`.
   * No params. */
  hostStats(params: Params, onResult: OnResult, onError?: OnError | null): void;
  /** The config the host runs with, every default filled in. No params. */
  hostConfig(params: Params, onResult: OnResult, onError?: OnError | null): void;
  /** Sets what every actor traces: `{ detail, values, sample, records,
   * rules }`. */
  traceConfigure(params: Params, onResult: OnResult, onError?: OnError | null): void;
  /** Adds trace rules for a while: `{ rules, seconds }` (default 900);
   * answers `{ id }`. */
  traceWatch(params: Params, onResult: OnResult, onError?: OnError | null): void;
  /** Ends a watch early: `{ id }`. */
  traceUnwatch(params: Params, onResult: OnResult, onError?: OnError | null): void;
  /** Ends every open span now, marked incomplete, for the telemetry handler.
   * Refused without `telemetry` in the config. */
  traceEnd(params: Params, onResult: OnResult, onError?: OnError | null): void;
  /** The spans of a traces request: `{ traces }` (the bytes) answers `{
   * spans }`, each with `trace`, `span`, `parent`, `name`, `start`, `end`,
   * `attributes`, ... */
  traceSpansDecode(params: Params, onResult: OnResult, onError?: OnError | null): void;

  // The typed methods as promises (the convenience layer): each calls the
  // method above of its name, resolving with its result and rejecting with
  // its `TinyactorsError` (`code`, `reason`, `path`); a refusal at the call
  // rejects too. A rejection is yours: without a `catch` it is an unhandled
  // rejection, and `idle()` does not see it. A method still pending at
  // `close()` never settles.

  /** `actorState` as a promise: `const { value } = await
   * host.actorStateAsync({ session })`. */
  actorStateAsync(params?: Params): Promise<Value>;
  /** `actorContext` as a promise of `{ variables }`. */
  actorContextAsync(params?: Params): Promise<Value>;
  /** `actorList` as a promise of a page, `{ sessions, cursor }`. */
  actorListAsync(params?: Params): Promise<Value>;
  /** `actorGet` as a promise of the actor. */
  actorGetAsync(params?: Params): Promise<Value>;
  /** `hostStats` as a promise of the counters: `await host.hostStatsAsync()`. */
  hostStatsAsync(params?: Params): Promise<Value>;
  /** `hostConfig` as a promise of the effective config. */
  hostConfigAsync(params?: Params): Promise<Value>;
  /** `traceConfigure` as a promise. */
  traceConfigureAsync(params?: Params): Promise<Value>;
  /** `traceWatch` as a promise of `{ id }`. */
  traceWatchAsync(params?: Params): Promise<Value>;
  /** `traceUnwatch` as a promise. */
  traceUnwatchAsync(params?: Params): Promise<Value>;
  /** `traceEnd` as a promise. */
  traceEndAsync(params?: Params): Promise<Value>;
  /** `traceSpansDecode` as a promise of `{ spans }`. */
  traceSpansDecodeAsync(params?: Params): Promise<Value>;

  /** The host's counters now as an OTLP metrics request (a `Buffer`, for a
   * collector's `/v1/metrics`). `TA_ERROR_STATE` without `telemetry` in the
   * config. */
  metrics(): Uint8Array;
  /** Finds cold actors: at most `limit` sessions that did not run since
   * the previous sweep and can be hibernated now, looking at most at `maxScan`
   * actors (each an integer from 0 to 2^32-1). The first sweep after an
   * actor ran skips it. */
  sweep(maxScan: number, limit: number): bigint[];
  /** Takes the actor out of memory and returns its blob (a `Buffer`) for
   * your store. `TA_ERROR_STATE` when it is not at rest (events waiting, in
   * a step, running an invocation), `TA_ERROR_UNSUPPORTED` when a variable
   * holds what the datamodel cannot store (the message names it); it stays
   * in memory then. */
  hibernate(session: Session): Uint8Array;
  /** Puts a hibernated actor back from the blob `hibernate` gave; its
   * waiting events run at the next turn. */
  wake(session: Session, blob: Uint8Array | ArrayBuffer): void;
  /** Starts moving running actors (every chart's, `chart`'s, or one
   * `session`) to the current version of their chart, in later turns; each
   * gets `error.platform.upgrade` with its old variables. Returns the
   * migration's id, which `migrated`, `migrationDone` and the wake handler
   * name. */
  migrate(options?: MigrateOptions): number;
  /** The whole host as one `Buffer` (typed as its `Uint8Array`): every segment, the images, the system
   * id, the host's time and the datamodel's state, for `Host.restore` on the
   * same build of the package. Only when quiescent: from a handler, with an
   * effect or a reply pending, or during a migration it throws
   * `TA_ERROR_STATE`; a value outside the value table throws
   * `TA_ERROR_UNSUPPORTED`. */
  snapshot(): Uint8Array;

  /** Frees the host: the last telemetry requests go to the handler, the
   * timer is cleared, pending typed methods and running services are
   * dropped without being called. From a handler it waits for the end of
   * the turn. Again: nothing. */
  close(): void;
  /** `close()`. */
  [Symbol.dispose](): void;
  /** `close()`, then the promises the telemetry handler returned at it. */
  [Symbol.asyncDispose](): Promise<void>;
}
