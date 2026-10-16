// The types of @tinyactors/node's direct layer: the addon, which mirrors
// core's C API (AGENTS.md, "Layers and the resolver"). Every declaration
// here is a row of docs/api-map.md, which names the core behind it; a row
// still marked *planned* there is declared ahead of its implementation, and
// index.mjs exports only the items built so far (docs/plan.xml).
//
// Time is milliseconds; sessions are bigints; bytes the host hands out are
// Buffers (a Uint8Array subclass) copied once, never views of host memory.

// ---- values -------------------------------------------------------------

/** A value of the value table (docs/values.md): what crosses as event data,
 * spawn params, a reply's result, and `encode`/`decode`. Inside a value an
 * actor reference is its address string. */
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

/** CBOR of a value, written by core's streaming writer. Throws a
 * `TinyactorsError`: `TA_ERROR_UNSUPPORTED` for what the value table has no
 * row for (a function, a symbol, a Date, a class's instance, a typed array
 * other than Uint8Array), `TA_ERROR_INVALID` for what it cannot hold (a
 * lone surrogate, a bigint past 64 bits, a cycle or nesting deeper than
 * 64). A getter's exception passes through. */
export function encode(value: Value): Uint8Array;

/** The value of one CBOR item filling `bytes`, read by core's streaming
 * reader. Throws a `TinyactorsError` for what it cannot read, a
 * `TypeError` for an argument that is not bytes. */
export function decode(bytes: Uint8Array | ArrayBuffer): Value;

// ---- errors -------------------------------------------------------------

/** The status names core gives (`ta_status_name`). */
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

/** An error core reported: a refused call, a thin view read after its ack,
 * a run's fault or an exhausted pool, or a typed method's error. Branch on
 * `status`; a typed method's error also has `code`, `reason` and `path` as
 * the host's reply wrote them. */
export class TinyactorsError extends Error {
  constructor(
    message?: string,
    options?: { code?: number; reason?: string; path?: string; status?: Status },
  );
  readonly code: number | undefined;
  readonly reason: string | undefined;
  readonly path: string | undefined;
  readonly status: Status | undefined;
}

// ---- compiling ----------------------------------------------------------

/** A document to compile: its name (used in diagnostics and to resolve
 * `src`) and its text. */
export interface Source {
  name: string;
  text: string;
}

/** One message of the compiler about a document. */
export interface Diagnostic {
  readonly severity: 'error' | 'warning' | 'note';
  /** The compiler's code for the kind of problem. */
  readonly code: number;
  readonly file: string;
  /** From 1. */
  readonly line: number;
  /** From 1. */
  readonly column: number;
  readonly message: string;
}

/** A compilation found an error; nothing was compiled. */
export class CompileError extends TinyactorsError {
  /** Every diagnostic of the compilation: errors, warnings and notes. */
  readonly diagnostics: readonly Diagnostic[];
}

/** Thrown by a resolver to refuse a document: the refusal becomes a
 * diagnostic of the resolved file, at `line` and `column` when given. */
export class ResolveError extends TinyactorsError {
  constructor(message: string, options?: { line?: number; column?: number });
  readonly line: number | undefined;
  readonly column: number | undefined;
}

/** The compile-time resolver: the text of the document `source` names,
 * relative to the document `base` that names it. Any callable; it lives
 * only during the compile call. */
export type Resolver = (base: string, source: string) => string;

/** A compile result. */
export interface Compiled extends Disposable {
  /** The image: a Buffer copied once on first read, the application's
   * from then on. A first read after `close()` throws `TA_ERROR_STALE`. */
  readonly image: Uint8Array;
  /** The warnings and notes of a compilation that succeeded. */
  readonly diagnostics: readonly Diagnostic[];
  /** Frees the result (the collector does it as a fallback). */
  close(): void;
  [Symbol.dispose](): void;
}

/** Compiles charts into one image; the first source is the default boot
 * chart. Throws `CompileError` with every diagnostic. */
export function compileCharts(sources: readonly Source[], resolver?: Resolver): Compiled;

/** Compiles a `<system>` manifest, its charts read through `resolver`,
 * into one image whose boot actors `host.boot` spawns. */
export function compileSystem(manifest: Source, resolver?: Resolver): Compiled;

// ---- addresses ----------------------------------------------------------

/** An actor's address, immutable; its parts are read by core's parser on
 * each access, `undefined` for a part the text lacks. */
export class Address {
  constructor(text: string);
  /** An address made from a system id and a session. */
  static fromSession(system: string, session: bigint): Address;
  /** An address made from a system id and an actor's name. */
  static fromName(system: string, name: string): Address;
  readonly text: string;
  readonly system: string | undefined;
  readonly session: bigint | undefined;
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

/** The POST of a BasicHTTP send (SCXML C.2.1). */
export interface HTTPPost {
  /** `http://host[:port]/path`. */
  url: string;
  event?: string;
  fields?: readonly HTTPField[];
  /** Used when there are no fields; percent-encoded. */
  body?: string;
}

/** An incoming BasicHTTP request (SCXML C.2.2). */
export interface HTTPRequest {
  readonly method: string;
  readonly target: string;
  readonly body: string;
  /** `_scxmleventname`, else `HTTP.<METHOD>`. */
  readonly event: string;
  /** The decoded form fields but `_scxmleventname`. */
  readonly fields: readonly HTTPField[];
}

/** The HTTP request of a BasicHTTP POST, as bytes to write to a socket. */
export function encodeHTTPPost(post: HTTPPost): Uint8Array;

/** The event of an incoming BasicHTTP request. */
export function parseHTTPRequest(message: Uint8Array | ArrayBuffer): HTTPRequest;

// ---- the clock ----------------------------------------------------------

/** A clock for tests and simulations, given as `options.clock`. */
export class SimulatedClock {
  constructor();
  /** Milliseconds since the clock was made. */
  readonly now: number;
  /** Moves the clock and schedules a turn on every host made with it;
   * `await host.idle()` to see what ran. */
  increment(ms: number): void;
}

// ---- effects as views ---------------------------------------------------

/** A send to an I/O processor, the transport or a service: a thin view
 * until the batch's ack (a read after it throws `TA_ERROR_STALE`), or, when
 * the handler returned a promise, filled before the ack and valid while the
 * host lives. */
export interface Send {
  readonly session: bigint;
  readonly event: string;
  /** An `Address` for the transport's sends; `undefined` when empty. */
  readonly target: string | Address | undefined;
  /** `undefined` when the send has none. */
  readonly sendid: string | undefined;
  readonly data: Value;
  /** The I/O processor's type; `undefined` for the transport and a service. */
  readonly type: string | undefined;
  readonly origin: Address | undefined;
  /** The sending step's trace as W3C text; `undefined` when untraced. */
  readonly traceparent: string | undefined;
  /** The outgoing half, as `host.send(message)` takes it on the other side. */
  readonly message: SendMessage;
  /** An event back to the sender with this send's sendid and, unless
   * `traceparent` is given (`null`: a new trace), its trace. Once: a second
   * answer, or one after `fail()`, throws `TA_ERROR_STATE`. */
  answer(event: string, data?: Value, options?: AnswerOptions): void;
  /** `error.communication` for the sender. */
  fail(): void;
}

export interface AnswerOptions {
  traceparent?: string | null;
  origin?: string | Address;
  raw?: string;
}

/** An event for `host.send(message)` and `host.sendMany(items)`. */
export interface SendMessage {
  target: bigint | string | Address;
  event: string;
  data?: Value;
  sendid?: string;
  origin?: string | Address;
  traceparent?: string;
}

/** A `<log>` line: a thin view until the ack. */
export interface LogEntry {
  readonly session: bigint;
  readonly label: string;
  readonly text: string;
}

/** An actor a migration moved: a thin view until the ack. */
export interface Migrated {
  readonly session: bigint;
  readonly segment: number;
  /** The chart's name. */
  readonly chart: string;
  readonly migration: number;
}

export interface MigrationResult {
  readonly migration: number;
  readonly migrated: number;
  readonly failed: number;
  readonly pending: readonly bigint[];
}

/** An `<invoke>` of an application's service: a record made before the
 * ack. */
export interface Invocation {
  readonly session: bigint;
  readonly invokeid: string;
  readonly type: string;
  readonly src: string;
  readonly data: Value;
  readonly traceparent: string | undefined;
  /** A restored host's announcement of an invocation that was running. */
  readonly restored: boolean;
  /** An event to the invoker; `TA_ERROR_STALE` once the invocation ended. */
  sendBack(event: string, data?: Value, options?: { traceparent?: string | null }): void;
  /** `done.invoke.<invokeid>` for the invoker; the service is dropped. */
  done(data?: Value, options?: { traceparent?: string | null }): void;
}

/** What a service handler returns: kept until the invocation ends. */
export interface Service {
  receive?(host: Host, send: Send): void;
  stop?(host: Host): void;
}

// ---- the host -----------------------------------------------------------

/** A chart's current version: `host.chart(name)`, for `host.spawn`. */
export interface ChartRef {
  readonly segment: number;
  readonly chart: number;
}

/** The host's config (tinyactors-core/docs/protocol.md), written as CBOR. */
export type HostConfig = { [key: string]: Value };

/** A handler's result: nothing, or a promise the binding watches. */
export type Handled = void | PromiseLike<unknown>;

export interface HostOptions {
  /** The handler of each I/O processor type the config lists. */
  ioProcessors?: { [type: string]: (host: Host, send: Send) => Handled };
  /** Sends to another system's address; without one each is failed. */
  transport?: (host: Host, send: Send) => Handled;
  /** `<log>` lines; without one they are discarded. */
  log?: (host: Host, entry: LogEntry) => void;
  clock?: SimulatedClock;
  /** The application's exporter of OTLP requests. */
  telemetry?: (host: Host, signal: 'traces' | 'logs', data: Uint8Array) => Handled;
  /** A hibernated session is wanted: load its blob, call `host.wake`. */
  wake?: (host: Host, session: bigint, migration: number | undefined) => Handled;
  migrated?: (host: Host, migrated: Migrated) => void;
  migrationDone?: (host: Host, result: MigrationResult) => void;
  /** The handler of each `<invoke>` type the application runs. */
  services?: { [type: string]: (host: Host, invocation: Invocation) => Service | void };
  /** The ECMAScript datamodel: the application's engine (default) or the
   * quickjs sandbox. Until the native datamodel is built, a native host
   * throws `TA_ERROR_UNSUPPORTED`. */
  datamodel?: 'native' | 'quickjs';
  /** The sandbox's limits, with `datamodel: "quickjs"` only. */
  quickjs?: QuickJSOptions;
}

export interface QuickJSOptions {
  /** Interrupt checks per evaluation before it aborts. */
  maxOps?: number;
  /** Bytes for the whole engine. */
  memoryLimit?: number;
  /** Bytes of native stack for JavaScript. */
  stackLimit?: number;
  realm?: 'auto' | 'isolated';
}

export interface SpawnOptions {
  name?: string;
  params?: Value;
  traceparent?: string;
}

export interface SendOptions {
  sendid?: string;
  traceparent?: string;
  origin?: string | Address;
}

export interface MigrateOptions {
  /** A chart's name; none: every chart. */
  chart?: string;
  session?: bigint;
  /** Ask the application to wake hibernated actors. */
  wake?: boolean;
  /** Milliseconds of the host's clock; 0: none. */
  timeout?: number;
}

/** A typed method's params, passed to core as given. */
export type Params = { [key: string]: Value } | undefined;
export type OnResult = (host: Host, result: Value) => void;
export type OnError = (host: Host, error: TinyactorsError) => void;

/** Many systems run as one, driven by the runtime's event loop. */
export class Host implements Disposable, AsyncDisposable {
  constructor(config: HostConfig, options?: HostOptions);
  /** A new host running a snapshot `snapshot()` gave. */
  static restore(snapshot: Uint8Array | ArrayBuffer, config: HostConfig, options?: HostOptions): Host;

  readonly ticksPerSecond: number;
  readonly closed: boolean;
  /** Milliseconds from now to the next deadline after the last idle turn;
   * `undefined` for none. */
  readonly nextDeadline: number | undefined;
  /** The quickjs engine's heap in bytes; `undefined` on a native host. */
  readonly quickjsHeapBytes: number | undefined;

  addImage(image: Uint8Array | ArrayBuffer): number;
  chart(name: string): ChartRef;
  boot(segment: number): void;
  spawn(ref: ChartRef, options?: SpawnOptions): bigint;
  send(target: bigint | string | Address, event: string, data?: Value, options?: SendOptions): void;
  send(message: SendMessage): void;
  fail(target: bigint | string | Address, sendid?: string): void;
  /** The length of the accepted prefix. */
  sendMany(items: readonly SendMessage[]): number;
  terminate(session: bigint): void;
  actorByName(name: string): bigint | undefined;

  /** Settles at the next idle turn (the same promise to every caller until
   * then); rejects with the turn's first error, or `TA_ERROR_STATE` when the
   * host is closed first. It observes the turns; the event loop drives them. */
  idle(): Promise<void>;

  actorState(params: Params, onResult: OnResult, onError?: OnError): void;
  actorContext(params: Params, onResult: OnResult, onError?: OnError): void;
  actorList(params: Params, onResult: OnResult, onError?: OnError): void;
  actorGet(params: Params, onResult: OnResult, onError?: OnError): void;
  hostStats(params: Params, onResult: OnResult, onError?: OnError): void;
  hostConfig(params: Params, onResult: OnResult, onError?: OnError): void;
  traceConfigure(params: Params, onResult: OnResult, onError?: OnError): void;
  traceWatch(params: Params, onResult: OnResult, onError?: OnError): void;
  traceUnwatch(params: Params, onResult: OnResult, onError?: OnError): void;
  traceEnd(params: Params, onResult: OnResult, onError?: OnError): void;
  traceSpansDecode(params: Params, onResult: OnResult, onError?: OnError): void;

  metrics(): Uint8Array;
  sweep(maxScan: number, limit: number): bigint[];
  hibernate(session: bigint): Uint8Array;
  wake(session: bigint, blob: Uint8Array | ArrayBuffer): void;
  /** Core's migration id. */
  migrate(options?: MigrateOptions): number;
  snapshot(): Uint8Array;

  close(): void;
  [Symbol.dispose](): void;
  /** `close()`, then the promises the last take's handlers returned. */
  [Symbol.asyncDispose](): Promise<void>;
}
