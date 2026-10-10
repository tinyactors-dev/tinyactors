// Type definitions for tinyactors (web binding: wasm, browsers and Node.js).
// The API follows xstate v5 where feasible. Context and event types flow from
// `types: {} as { context; events; input; output }` (or from `context`) into
// actions, guards, assigners and delays.

export interface EventObject {
  type: string;
}
export type MachineContext = Record<string, any>;
export type StateValue = string | { [key: string]: StateValue };

/** Arguments passed to actions, guards, assigners, delays, inputs and outputs. */
export interface ActionArgs<TContext, TEvent extends EventObject> {
  context: TContext;
  event: TEvent;
  /** The actor system the actor runs in (look up actors registered by systemID). */
  system: SystemRef;
  /** The actor running this code (put it in events so others can reply). */
  self: AnyActorRef;
}

/** What actions and guards see of the actor system. */
export interface SystemRef {
  get(systemID: string): AnyActor | undefined;
}

/** Something events can be sent to: a local actor or an ActorRef. */
export type AnyActorRef = AnyActor | ActorRef;

export type ActionFunction<TContext, TEvent extends EventObject, TParams = any> = (
  args: ActionArgs<TContext, TEvent>,
  params: TParams,
) => void;

export type GuardPredicate<TContext, TEvent extends EventObject, TParams = any> = (
  args: ActionArgs<TContext, TEvent>,
  params: TParams,
) => boolean;

/* ------------------------------------------------------------ action objects */

export type PartialAssigner<TContext, TEvent extends EventObject, K extends keyof TContext> =
  | TContext[K]
  | ((args: ActionArgs<TContext, TEvent>, params: any) => TContext[K]);

export type Assigner<TContext, TEvent extends EventObject> =
  | ((args: ActionArgs<TContext, TEvent>, params: any) => Partial<TContext>)
  | { [K in keyof TContext]?: PartialAssigner<TContext, TEvent, K> };

export interface AssignAction<TContext, TEvent extends EventObject> {
  readonly type: 'xstate.assign';
  readonly assignment: Assigner<TContext, TEvent>;
}

export type EventOrExpr<TContext, TEvent extends EventObject, TSent extends EventObject> =
  | TSent
  | ((args: ActionArgs<TContext, TEvent>) => TSent);

export type Delay<TContext, TEvent extends EventObject> =
  | number
  | string
  | ((args: ActionArgs<TContext, TEvent>) => number);

export interface SendOptions<TContext, TEvent extends EventObject> {
  delay?: Delay<TContext, TEvent>;
  id?: string;
}

export interface RaiseAction<TContext, TEvent extends EventObject> {
  readonly type: 'xstate.raise';
  readonly event: EventOrExpr<TContext, TEvent, EventObject>;
  readonly delay?: Delay<TContext, TEvent>;
  readonly id?: string;
}
/** A child invoke id, another actor (local, or remote via an ActorRef), or a function returning one. */
export type SendTarget<TContext, TEvent extends EventObject> =
  | string
  | AnyActorRef
  | ((args: ActionArgs<TContext, TEvent>) => string | AnyActorRef | undefined);

export interface SendToAction<TContext, TEvent extends EventObject> {
  readonly type: 'xstate.sendTo';
  readonly to: SendTarget<TContext, TEvent>;
  readonly event: EventOrExpr<TContext, TEvent, EventObject>;
  readonly delay?: Delay<TContext, TEvent>;
  readonly id?: string;
}
export interface SendParentAction<TContext, TEvent extends EventObject> {
  readonly type: 'xstate.sendParent';
  readonly event: EventOrExpr<TContext, TEvent, EventObject>;
  readonly delay?: Delay<TContext, TEvent>;
  readonly id?: string;
}
export interface LogAction<TContext, TEvent extends EventObject> {
  readonly type: 'xstate.log';
  readonly value: string | ((args: ActionArgs<TContext, TEvent>) => unknown);
  readonly label?: string;
}
export interface CancelAction {
  readonly type: 'xstate.cancel';
  readonly sendID: string;
}

/** A named action/guard reference, optionally with parameters. */
export interface ParameterizedObject {
  type: string;
  params?: unknown;
}

export type Action<TContext, TEvent extends EventObject> =
  | string
  | ParameterizedObject
  | ActionFunction<TContext, TEvent>
  | AssignAction<TContext, any>
  | RaiseAction<TContext, any>
  | SendToAction<TContext, any>
  | SendParentAction<TContext, any>
  | LogAction<TContext, any>
  | CancelAction;

export type Actions<TContext, TEvent extends EventObject> =
  | Action<TContext, TEvent>
  | ReadonlyArray<Action<TContext, TEvent>>;

/* -------------------------------------------------------------------- guards */

export interface StateInGuard {
  readonly type: 'xstate.stateIn';
  readonly stateValue: string;
}
export interface CompositeGuard<TContext, TEvent extends EventObject> {
  readonly type: 'xstate.and' | 'xstate.or' | 'xstate.not';
  readonly guards: ReadonlyArray<Guard<TContext, TEvent>>;
}
export type Guard<TContext, TEvent extends EventObject> =
  | string
  | ParameterizedObject
  | GuardPredicate<TContext, TEvent>
  | StateInGuard
  | CompositeGuard<TContext, TEvent>;

/* ------------------------------------------------------------------- configs */

export interface TransitionConfig<TContext, TEvent extends EventObject> {
  target?: string | ReadonlyArray<string>;
  guard?: Guard<TContext, TEvent>;
  actions?: Actions<TContext, TEvent>;
  /** Re-enter the source state when targeting itself or a descendant (default false). */
  reenter?: boolean;
  description?: string;
}

export type SingleOrArray<T> = T | ReadonlyArray<T>;

export type TransitionConfigOrTarget<TContext, TEvent extends EventObject> = SingleOrArray<
  string | TransitionConfig<TContext, TEvent>
>;

export type TransitionsConfig<TContext, TEvent extends EventObject> = {
  [K in TEvent['type'] | '*']?: TransitionConfigOrTarget<
    TContext,
    K extends '*' ? TEvent : Extract<TEvent, { type: K }>
  >;
};

export interface DoneActorEvent<TOutput = unknown> extends EventObject {
  output: TOutput;
}

export interface InvokeConfig<TContext, TEvent extends EventObject> {
  /** A machine, or the name of one in `implementations.actors`. */
  src: AnyStateMachine | string;
  id?: string;
  input?: unknown | ((args: ActionArgs<TContext, TEvent>) => unknown);
  onDone?: TransitionConfigOrTarget<TContext, DoneActorEvent<any>>;
}

export interface StateNodeConfig<TContext, TEvent extends EventObject> {
  id?: string;
  type?: 'atomic' | 'compound' | 'parallel' | 'final' | 'history';
  initial?: string;
  states?: { [key: string]: StateNodeConfig<TContext, TEvent> };
  on?: TransitionsConfig<TContext, TEvent>;
  always?: TransitionConfigOrTarget<TContext, TEvent>;
  after?: { [delay: string | number]: TransitionConfigOrTarget<TContext, TEvent> };
  entry?: Actions<TContext, TEvent>;
  exit?: Actions<TContext, TEvent>;
  invoke?: SingleOrArray<InvokeConfig<TContext, TEvent>>;
  /** Final states: output of the parent's done event (or of the machine). */
  output?: unknown | ((args: ActionArgs<TContext, TEvent>) => unknown);
  tags?: SingleOrArray<string>;
  /** History states. */
  history?: 'shallow' | 'deep';
  target?: string;
  description?: string;
  meta?: unknown;
}

export interface MachineTypes<TContext, TEvent extends EventObject, TInput, TOutput> {
  context?: TContext;
  events?: TEvent;
  input?: TInput;
  output?: TOutput;
}

export interface MachineConfig<
  TContext extends MachineContext,
  TEvent extends EventObject,
  TInput = any,
  TOutput = any,
> extends Omit<StateNodeConfig<TContext, TEvent>, 'output'> {
  id?: string;
  types?: MachineTypes<TContext, TEvent, TInput, TOutput>;
  context?: TContext | ((args: { input: TInput }) => TContext);
  output?: TOutput | ((args: ActionArgs<TContext, DoneActorEvent<any>>) => TOutput);
}

export interface MachineImplementations<TContext, TEvent extends EventObject> {
  actions?: { [name: string]: ActionFunction<TContext, TEvent> | Action<TContext, TEvent> };
  guards?: { [name: string]: GuardPredicate<TContext, TEvent> | Guard<TContext, TEvent> };
  delays?: { [name: string]: number | ((args: ActionArgs<TContext, TEvent>) => number) };
  actors?: { [name: string]: AnyStateMachine };
}

/* ------------------------------------------------------------------ machines */

export declare class StateMachine<
  TContext extends MachineContext,
  TEvent extends EventObject,
  TInput = any,
  TOutput = any,
> {
  readonly id: string;
  readonly config: MachineConfig<TContext, TEvent, TInput, TOutput>;
  readonly implementations: Required<MachineImplementations<TContext, TEvent>>;
  provide(implementations: MachineImplementations<TContext, TEvent>): StateMachine<TContext, TEvent, TInput, TOutput>;
  /** The SCXML document the machine compiles to. It reads like hand-written
   * SCXML: guards/actions by name, inline functions by their source, literals
   * as JSON. */
  toSCXML(): string;
  /** @internal type carriers */
  readonly __types?: { context: TContext; events: TEvent; input: TInput; output: TOutput };
}

export type AnyStateMachine = StateMachine<any, any, any, any>;

/* ----------------------------------------------------------- SCXML documents */

/** An SCXML event as sent to a document: `type` becomes `_event.name`; `data`
 * (or, without it, the other properties) becomes `_event.data`. */
export interface SCXMLEvent extends EventObject {
  data?: unknown;
  [key: string]: unknown;
}

/**
 * A W3C SCXML document as actor logic (fromSCXML). Its `datamodel` attribute
 * picks the semantics: "ecmascript" (SCXML Appendix B.2) or "null".
 */
export declare class SCXMLLogic<TContext = Record<string, any>, TEvent extends EventObject = SCXMLEvent> {
  /** The document's `<scxml name>`, else "scxml" (or the `id` option). */
  readonly id: string;
  /** The document as given. */
  toSCXML(): string;
  /** @internal type carriers */
  readonly __types?: { context: TContext; events: TEvent; input: Partial<TContext>; output: unknown };
}

export interface FromSCXMLOptions {
  /** The logic's id (default: `<scxml name>`, else "scxml"). */
  id?: string;
  /** Further documents by name: `<invoke src>` documents (linked alongside) and
   * any text a `src="..."` attribute names (e.g. `<data src>`). */
  documents?: Record<string, string>;
}

/**
 * Run an SCXML document. The snapshot's `context` is the datamodel's
 * variables, its `value` the active states (by id) as a nested state value;
 * `input` overrides `<data>` initial values; `<donedata>` is the output.
 */
export declare function fromSCXML<TContext = Record<string, any>, TEvent extends EventObject = SCXMLEvent>(
  xml: string,
  options?: FromSCXMLOptions,
): SCXMLLogic<TContext, TEvent>;

/* ------------------------------------------------------------- system images */

/**
 * A compiled system image: charts plus the actors to boot (a `<system>`
 * manifest). Store `bytes`; `loadImage()` reads it back, here or in a browser.
 */
export declare class SystemImage {
  /** The image's charts. */
  readonly charts: ReadonlyArray<{ name: string; src: string; datamodel: string }>;
  /** The actors `system.boot()` starts (chart: an index into `charts`). */
  readonly actors: ReadonlyArray<{ name: string; chart: number }>;
  /** The image as bytes. */
  readonly bytes: Uint8Array;
  /** Actor logic for one of its charts (by `<chart name>` or src), for createActor. */
  chart(name: string): SCXMLLogic;
}

/**
 * Compile a `<system>` manifest — `<chart name src/>` and `<actor name chart/>`
 * elements — with the documents its `src` attributes name in `files`.
 */
export declare function compileSystem(manifest: string, options?: { files?: Record<string, string> }): SystemImage;
/** A compiled image from bytes (`image.bytes`, or the C `tac` compiler's output). */
export declare function loadImage(bytes: Uint8Array): SystemImage;

/** Anything an actor can run: a machine or an SCXML document. */
export type AnyActorLogic = AnyStateMachine | SCXMLLogic<any, any>;
type TypesOf<M> = M extends { readonly __types?: infer T } ? NonNullable<T> : never;
export type ContextFrom<M> = TypesOf<M> extends { context: infer C } ? C : never;
export type EventFrom<M> = TypesOf<M> extends { events: infer E } ? E : never;
export type InputFrom<M> = TypesOf<M> extends { input: infer I } ? I : never;
export type OutputFrom<M> = TypesOf<M> extends { output: infer O } ? O : never;

export declare function createMachine<
  TContext extends MachineContext = MachineContext,
  TEvent extends EventObject = EventObject,
  TInput = any,
  TOutput = any,
>(
  config: MachineConfig<TContext, TEvent, TInput, TOutput>,
  implementations?: MachineImplementations<TContext, TEvent>,
): StateMachine<TContext, TEvent, TInput, TOutput>;

export declare function setup<
  TContext extends MachineContext = MachineContext,
  TEvent extends EventObject = EventObject,
  TInput = any,
  TOutput = any,
>(
  implementations: MachineImplementations<TContext, TEvent> & {
    types?: MachineTypes<TContext, TEvent, TInput, TOutput>;
  },
): {
  createMachine(
    config: MachineConfig<TContext, TEvent, TInput, TOutput>,
  ): StateMachine<TContext, TEvent, TInput, TOutput>;
};

/* -------------------------------------------------------------------- actors */

export interface Snapshot<TContext, TOutput = unknown> {
  value: StateValue;
  context: TContext;
  status: 'active' | 'done' | 'stopped';
  output?: TOutput;
  /** Last error thrown by user code (reported to the chart as error.execution). */
  error?: unknown;
  matches(value: StateValue): boolean;
  hasTag(tag: string): boolean;
  toJSON(): { value: StateValue; context: TContext; status: string; output?: TOutput };
}

export interface Observer<T> {
  next?: (value: T) => void;
  error?: (err: unknown) => void;
  complete?: () => void;
}
export interface Subscription {
  unsubscribe(): void;
}

export interface ActorOptions<TInput> {
  id?: string;
  input?: TInput;
  /** The system to start the actor in (default: the process-wide default system). */
  system?: System;
  /** Register the actor so other actors can find it with `system.get(systemID)`. */
  systemID?: string;
  /** Receives <log> output (default console.log). */
  logger?: (...args: unknown[]) => void;
  /** A W3C traceparent: the actor's first step continues that trace. */
  traceparent?: string;
}

/** Options of a send from host code. */
export interface HostSendOptions {
  /** A W3C traceparent: the step the event starts continues that trace (its span is the parent). */
  traceparent?: string;
}

export declare class Actor<M extends AnyActorLogic> {
  readonly id: string;
  readonly logic: M;
  readonly system: System;
  /** This actor's address (`tinyactors://<system>/#_scxml_<session>`), the same across versions and reachable across links. */
  readonly address: string | undefined;
  /** True while the actor is hibernated (unloaded to the system's store); it wakes by itself when it gets work. */
  readonly hibernated: boolean;
  start(): this;
  stop(): this;
  send(event: EventFrom<M>, options?: HostSendOptions): void;
  getSnapshot(): Snapshot<ContextFrom<M>, OutputFrom<M>>;
  subscribe(observer: Observer<Snapshot<ContextFrom<M>, OutputFrom<M>>>): Subscription;
  subscribe(
    next: (snapshot: Snapshot<ContextFrom<M>, OutputFrom<M>>) => void,
    error?: (err: unknown) => void,
    complete?: () => void,
  ): Subscription;
}

export type AnyActor = Actor<AnyActorLogic>;

/** Statistics of one native segment of a system. */
export interface SegmentStats {
  maxActors: number;
  actors: number;
  sessions: number;
  payloads: number;
  /** Actors currently hibernated. */
  hibernated: number;
  /** Totals for the whole system. */
  hibernations: number;
  wakes: number;
}

/**
 * Where hibernated actors go. The system never does I/O itself: it hands the
 * store opaque blobs and asks for them back when an actor gets work. Every
 * method may return a promise (IndexedDB, files, Redis, a database, ...).
 */
export interface HibernationStore {
  get(key: string): Uint8Array | undefined | Promise<Uint8Array | undefined>;
  set(key: string, blob: Uint8Array): void | Promise<void>;
  delete(key: string): void | Promise<void>;
}

export interface HibernationOptions {
  /** Default: an in-memory store (createMemoryStore()). */
  store?: HibernationStore;
  /** Unload actors idle for a whole sweep period, every `sweepMs` (default 30000; 0 = only on system.hibernate()). */
  sweepMs?: number;
}

export interface SystemOptions {
  /**
   * This system's id in actor addresses, `tinyactors://<id>/...` (default: a
   * random 12-character id). Lowercase a-z, 0-9, "-" and dots (see
   * docs/protocol.md); set it explicitly when other systems address this one.
   */
  id?: string;
  /** Unload idle actors (context, events and input must be encodable: no functions). */
  hibernation?: HibernationOptions;
  /**
   * Restore a `system.snapshot()`: the same id, actors (find them with
   * `get(systemID)`), timers and queued events. Machines are matched by id and
   * the SCXML they compile to; ones defined in this process are found by
   * themselves, `machines` names more. SCXML documents and images come from
   * the snapshot.
   */
  restore?: Uint8Array;
  machines?: AnyStateMachine[];
  /**
   * The system's clock in milliseconds (default: wall time). With a clock the
   * system arms no timers of its own: advance your clock and call
   * `system.flush()` — deterministic tests, simulations, replay.
   */
  clock?: () => number;
  /** Receives <log> output of actors without their own `logger` (default console.log). */
  logger?: (...args: unknown[]) => void;
  /** A trace sink from the start, and the trace ID generator's seed. */
  traces?: TraceSinkOptions & { sink?: TraceSink; seed?: bigint | number };
  /**
   * The OTLP/HTTP exporter (merged over what the platform gives: in Node.js
   * the OTEL_* environment variables; the browser build exports only with an
   * endpoint given here). `false`: no export.
   */
  telemetry?: TelemetryOptions | false;
}

/**
 * A reference to an actor anywhere: this system, another segment, or another
 * system across a link. Send to it from host code or use it as a sendTo()
 * target; actor references inside events arrive as ActorRefs.
 */
export declare class ActorRef {
  readonly id: string;
  readonly address: string;
  readonly system: System;
  send(event: EventObject | string, options?: HostSendOptions): void;
}

/**
 * An actor system: all its actors run inside one native tinyactors system
 * (machines defined after it started are linked into an extra segment, which
 * its actors reach transparently).
 */
export declare class System implements SystemRef {
  readonly id: string;
  readonly options: SystemOptions;
  get(systemID: string): AnyActor | undefined;
  /** The actor at `address` (a local Actor if it is one of ours, else an ActorRef). */
  ref(address: string): AnyActorRef;
  /**
   * Hand an envelope to the router as this system's host code (docs/protocol.md, "Envelope"):
   * delivered to a local actor or sent to the peer its target routes to. "queued" when called
   * from inside an action (it is handed over once the current step ends).
   */
  deliver(envelope: Envelope): 'delivered' | 'relayed' | 'duplicate' | 'refused' | 'queued';
  stats(): SegmentStats[];
  /**
   * Hibernate `actor` now, or — without an argument — every actor that has
   * been idle since the previous sweep. Returns how many were unloaded. An
   * actor whose data cannot be stored throws (the message names the variable,
   * and a function's line); the sweep leaves such actors resident.
   */
  hibernate(actor?: AnyActor): number;
  /**
   * Move `actor` out of this system, as bytes another system — in this or
   * another process, Node or web — takes in with `adopt()`: its states,
   * variables, delayed events with their data, the actors it invoked, and
   * what it runs. The actor stops here, without a done event. It must be
   * awake, not in a step, with nothing queued for it; data that cannot be
   * stored throws, naming the variable.
   */
  export(actor: AnyActor): Uint8Array;
  /**
   * Take in an actor another system exported: the same session (its address
   * under this system's id), states and variables; delayed events keep the
   * time they had left at export. Machines are found as for
   * `createSystem({ restore })`; SCXML documents and images travel with it.
   */
  adopt(bytes: Uint8Array, options?: { machines?: AnyStateMachine[] }): AnyActor;
  /**
   * Hot reload, CGI style: `machine` becomes the current version of the
   * machines with its id (compiled and linked now, so errors throw here).
   * Actors started afterwards run it; running actors finish on their version
   * (or move with migrate()); versions without actors are freed. Addresses
   * stay valid across versions. Returns the new version number (2, 3, ...).
   */
  reload(machine: AnyStateMachine | SCXMLLogic<any, any>): number;
  /**
   * Live migration to the current version: each actor moves at a stable
   * point, states matched by id, history and context kept (properties the new
   * machine does not declare are dropped, new ones initialized), same address.
   * It then receives `{ type: 'error.platform.upgrade', data: previousContext }`.
   * `target`: one actor, a machine (its actors), or all actors on old versions.
   * Runs as a job in time slices (a few ms each, actors walked lazily), so the
   * event loop stays responsive however many actors there are. Busy actors
   * move at their next stable point; hibernated ones are loaded first when
   * `wake` is on, else they move when they wake.
   */
  migrate(target?: AnyActor | AnyStateMachine, options?: MigrateOptions): Migration;
  /**
   * Send many events in one native call (batched per segment). `items`:
   * [actor, event] pairs; returns how many were accepted (actors that ended
   * are skipped).
   */
  sendMany(items: ReadonlyArray<readonly [AnyActor, EventObject | string] | readonly [AnyActor, EventObject | string, HostSendOptions]>): number;
  /**
   * The whole system as bytes: every actor (states, contexts and variables,
   * queued events, timers) and what it runs (machine SCXML, documents,
   * images). Restore with `createSystem({ restore })`. Not during a migration
   * or inside an action; contexts must be encodable (no functions).
   * Hibernated actors stay in the hibernation store: restore with the same store.
   */
  snapshot(): Uint8Array;
  /**
   * Run everything due now (events, eventless chains, timers due on the
   * system's clock) until the system is idle; true when it is. With a `clock`
   * option this is how time moves: advance the clock, then flush().
   */
  flush(maxSlices?: number): boolean;
  /** Start the actors a system image declares; returns them by name (also `get(name)`). */
  boot(image: SystemImage): Record<string, AnyActor>;
  /** Stop every actor, close the system's links, end its timers and free its native memory. Open spans end, marked incomplete. */
  stop(): void;
  /**
   * Hand traces to `sink` as OTLP protobuf: after each run, one Uint8Array
   * holding an ExportTraceServiceRequest (the spans that ended), and one
   * holding an ExportLogsServiceRequest (failures, <log>) when there are
   * logs. Post them to an OTLP/HTTP endpoint as they are, or read them with
   * decodeTraces(). `null` removes the sink. Without a sink or an exporter
   * core writes no records (traceparents still flow between actors).
   */
  setTraceSink(sink: TraceSink | null, options?: TraceSinkOptions): void;
  /** Change `detail`, `kinds`, `sample` and `rules` while traces flow; open spans are kept. */
  configureTraces(options: TraceConfiguration): void;
  /**
   * Trace matching steps in full, sampled whatever the ratio, and what they
   * cause (docs/adr/0007), until stop() or `for` ms.
   */
  watchTraces(watch: TraceWatch): { stop(): void };
  /** Resolves once every export this system started has been posted (or failed). */
  flushTraces(): Promise<void>;
  /**
   * Metrics as plain data (docs/adr/0007): per chart (`tinyactors.chart.name`)
   * its actors, spawns, ends, macrosteps, fuel, errors and faults; the
   * pools, memory, the run queue, overdue timers, delivered events, lost
   * trace records, traces started and sampled; and how long runs blocked
   * the loop. Sums are cumulative since `startTime`. Read it from an SDK's
   * observable instruments, or hand it to metricsToOTLP().
   */
  metrics(): MetricsSnapshot;
}

export interface MigrateOptions {
  /** Load hibernated actors to migrate them now (default: true when the system has a hibernation store). */
  wake?: boolean;
  /** End the job after this long; actors still busy (or asleep) stay on their version and are reported. */
  timeoutMs?: number;
}

export interface MigrationResult {
  /** Actors moved by the job. */
  migrated: number;
  /** Actors that could not be adopted by the new version (they stay, unchanged, where they were). */
  failed: number;
  /** Actors left on their version when the job ended (timeout, or the system stopped). */
  pending: AnyActor[];
}

export interface Migration {
  /** Actors moved during the call (the job's first slice). */
  readonly migrated: AnyActor[];
  /** Actors still on older versions (computed when read: a walk over the actors). */
  readonly pending: AnyActor[];
  /** Resolves when the job ends. */
  readonly done: Promise<MigrationResult>;
}

/** A new, empty actor system (the default system is used otherwise). */
export declare function createSystem(options?: SystemOptions): System;

/** A hibernation store kept in memory. */
export declare function createMemoryStore(): HibernationStore & { readonly size: number };

/** The tagged-JSON encoding used for hibernation and links (keeps Date, Map, Set, BigInt, cycles; actors as addresses). */
export declare function encodeValue(value: unknown): Uint8Array;
export declare function decodeValue(bytes: Uint8Array, system?: System): unknown;

/** A JSON value with the protocol's extensions: {"$ref": address}, {"$bytes": base64}, {"$": {...}}. */
export type WireValue = null | boolean | number | string | WireValue[] | { [key: string]: WireValue };
/**
 * A value as a neutral wire value (docs/protocol.md, "Values"): what systems in any language
 * understand. Actors become {"$ref"}, bytes {"$bytes"}; undefined, functions and symbols are
 * dropped, Map/Set become objects/arrays, Dates their JSON form; cycles throw.
 */
export declare function encodeWire(value: unknown): WireValue;
/** A neutral wire value as a JS value ({"$ref"} becomes an ActorRef of `system`, if given). */
export declare function decodeWire(value: WireValue, system?: System): unknown;

/* --------------------------------------------------------------------- links */

/** A WebSocket-like duplex (the browser/Node WebSocket fits). */
export interface LinkTransport {
  send(data: string): void;
  close(): void;
  onopen: ((ev?: unknown) => void) | null;
  onmessage: ((ev: { data: unknown }) => void) | null;
  onclose: ((ev?: unknown) => void) | null;
  onerror?: ((ev?: unknown) => void) | null;
}

/** A Node-style stream of bytes (the shape links use: a net.Socket, process.stdin, a child's stdout). */
export interface NodeByteStream {
  on(event: string, listener: (...args: any[]) => void): unknown;
  off(event: string, listener: (...args: any[]) => void): unknown;
  once(event: string, listener: (...args: any[]) => void): unknown;
  write(chunk: Uint8Array): unknown;
  end(): unknown;
  destroy?(): unknown;
}
/**
 * A byte stream as a link transport: length-prefixed frames (docs/protocol.md, "Framing").
 * A duplex (a net.Socket) or a pair: `{ readable: child.stdout, writable: child.stdin }` in the
 * parent, `{ readable: process.stdin, writable: process.stdout }` in the child.
 */
export type ByteStream = NodeByteStream | { readable: NodeByteStream; writable: NodeByteStream };

/** A frame the decoder refused (above max_frame, or not UTF-8): skipped, the stream stays in step. */
export interface RefusedFrame {
  reason: 'malformed';
  /** The frame's length in bytes. */
  length: number;
  detail: string;
}
/** One length-prefixed frame (docs/protocol.md, "Framing"): a 4-byte big-endian length, then the UTF-8 text. */
export declare function encodeFrame(text: string): Uint8Array;
/** Bytes in, frames out: for byte-stream transports of your own. */
export declare class FrameDecoder {
  /** `maxFrame`: frames above it are refused (default 1 MiB = 1048576). */
  constructor(options?: { maxFrame?: number });
  readonly maxFrame: number;
  /** Feed bytes as they arrive (any split); returns the frames they complete, in order. */
  push(chunk: Uint8Array): Array<string | RefusedFrame>;
}

export interface LinkOptions {
  /** ws:// or wss:// URL of a system serving links. */
  url?: string;
  /** Or: open a transport yourself (called again on every reconnect): WebSocket-like, or a byte
   * stream (e.g. a child process's stdio) that carries length-prefixed frames. */
  transport?: () => LinkTransport | ByteStream;
  /** Node: or connect over TCP (length-prefixed frames; host default 127.0.0.1). */
  tcp?: { host?: string; port: number };
  /** Largest frame accepted on a byte stream, announced in the hello (default 1 MiB). Envelopes
   * that do not fit the other side's announced limit fail with error.communication (malformed). */
  maxFrame?: number;
  /** The remote system's id, used in addresses created by link.get() (default "remote"; if the
   * server turns out to have another id, addresses with this name still reach it). */
  name?: string;
  /** Envelopes queued by the router while the link is down (default 1000; beyond that, or with 0
   * = fail fast, sends raise error.communication in the sender). */
  maxQueue?: number;
  /** How long (ms) a queued envelope may wait for the link; then error.communication (default: no limit). */
  queueTTLMs?: number;
  /** Trust the remote system: it may send to any local actor and relay through us (default true). */
  trusted?: boolean;
  /** Announce the systems reachable through us to the remote side (relaying; default false; a list: only those). */
  announce?: boolean | string[];
  /** Opaque credential sent in the hello (the other side's `trust(hello)` may check it). */
  credential?: unknown;
  /** Value encoding: "js" (default; used if the other side announces it too, else neutral) or
   * "neutral" (docs/protocol.md, "Values") — for peers in other languages. */
  profile?: 'js' | 'neutral';
  /** Ping interval for the round-trip time (default 5000 ms; 0 = off). */
  pingMs?: number;
}

export type LinkStatus = 'connecting' | 'open' | 'reconnecting' | 'closed';

export interface LinkContext {
  url: string | undefined;
  /** Failed attempts since the link was last open. */
  attempts: number;
  /** Round-trip time of the last ping (ms). */
  rtt: number | null;
  /** Events waiting for the link to open. */
  queued: number;
  /** When the link opened (ms since the system started), or null. */
  openedAt: number | null;
  /** The remote system's id once connected. */
  peer: string | null;
  error: string | null;
}

export type LinkEvent =
  | { type: 'OPENED'; peer: string; at: number }
  | { type: 'DROPPED'; error?: string }
  | { type: 'RTT'; ms: number }
  | { type: 'QUEUED'; n: number }
  | { type: 'CLOSE' };

export type LinkMachine = StateMachine<LinkContext, LinkEvent>;

/**
 * A link to another actor system. The link itself is an actor: its snapshot
 * value is the connection status (subscribe to show it), its context has
 * attempts, round-trip time and queue length. Actors on either side reach each
 * other transparently: `sendTo(link.get('lobby'), ...)`, replies to `self`
 * references carried in events.
 */
export interface Link extends Actor<LinkMachine> {
  /** The remote actor registered with `systemID` on the other side. */
  get(systemID: string): ActorRef;
  /** Close for good (status becomes "closed"). */
  close(): void;
  /** The router's view of this link (null once closed). */
  status(): PeerStatus | null;
}

/** One SCXML event on its way to one target (docs/protocol.md, "Envelope"). */
export interface Envelope {
  target: string;
  name: string;
  data?: unknown;
  /** Default: tinyactors://<this system>/@host */
  origin?: string;
  origintype?: string;
  sendid?: string;
  /** Unique per origin; errors and deduplication refer to it. */
  msgid?: string;
  /** A key to drop duplicates by instead of msgid (an idempotency key). */
  dedup?: string;
  hops?: number;
  /** A W3C traceparent: the step the envelope starts continues that trace (it travels across links). */
  traceparent?: string;
}

/** A route's status as the router sees it (docs/protocol.md, "Observability"). */
export interface PeerStatus {
  /** The peer's system id. */
  id: string;
  state: 'connecting' | 'open' | 'reconnecting' | 'closed';
  /** Envelopes waiting for the peer. */
  queued: number;
  /** Last measured round trip (ms; 0 = unknown). */
  rtt: number;
  /** How long the oldest queued envelope has waited (ms). */
  lag: number;
  sent: number;
  received: number;
  errors: number;
  duplicates: number;
}

/** The first frame of the link protocol (docs/protocol.md, "Link protocol"). */
export interface LinkHello {
  t: 'hello';
  v: number;
  id: string;
  reach: string[];
  caps?: { profile?: string; ordered?: boolean; acks?: boolean; max_frame?: number };
  cred?: unknown;
}

/** A parsed actor address (docs/protocol.md, "Addresses"). Session ids are BigInt: exact up to 2^64-1. */
export type ParsedAddress =
  | { system: string; local: string; kind: 'session'; session: bigint }
  | { system: string; local: string; kind: 'name'; name: string; encoded: string };
/** Parse `tinyactors://<system>/<local>`; null if it is not an address. */
export declare function parseAddress(text: string): ParsedAddress | null;
/** `tinyactors://<system>/#_scxml_<id>` for a session id, `.../@<name>` (percent-encoded) for a name. */
export declare function formatAddress(system: string, local: bigint | number | string): string;
export declare function createLink(system: System, options: LinkOptions): Link;

export interface ServeLinksOptions {
  /** Node: an http(s).Server to attach WebSocket upgrades to (with `path`, default "/link"). */
  server?: unknown;
  path?: string;
  /** Node: or listen on this port. */
  port?: number;
  /** Node: or accept TCP connections (length-prefixed frames) on this port (0: any free one). */
  tcp?: { port?: number; host?: string };
  /** Any runtime: register a source of already-open transports (a WebSocket server, MessagePorts,
   * byte streams such as a child process's stdio, ...). */
  accept?: (onConnection: (transport: LinkTransport | ByteStream) => void) => void | (() => void);
  /** Largest frame accepted on a byte stream, announced in the hello (default 1 MiB). */
  maxFrame?: number;
  /** What untrusted peers may send to: actor systemIDs ("*" = every name and session), optionally
   * limited to event descriptors (`{ lobby: 'JOIN LEAVE' }`). Default: nothing. */
  exports?: string[] | Record<string, string | string[]>;
  /** Trust every peer (send anything, relay through us). Default false. */
  trusted?: boolean;
  /** Or decide per peer from its hello (e.g. check `hello.cred`). */
  trust?: (hello: LinkHello) => boolean;
  /** A peer that reconnects with the same id within this time resumes, and receives what was
   * queued for it meanwhile (default 30000; 0 = fail fast as soon as it drops). Measured on
   * the system's clock: with a `clock` option, simulated time. */
  resumeMs?: number;
  /** Envelopes queued per absent peer (default 1000). */
  maxQueue?: number;
  /** Announce the systems reachable through us to connecting peers (relaying; default false);
   * a list announces only those (e.g. `[databaseID]`, not the other browsers). */
  announce?: boolean | string[];
  /** Value encoding offered to peers: "js" (default) or "neutral". */
  profile?: 'js' | 'neutral';
}

export interface LinkServer {
  /** Remote system ids currently connected. */
  connections(): string[];
  /** The router's view of the peer with this system id (also while it is away), or null. */
  status(id: string): PeerStatus | null;
  /** The port when listening (Node, `port` or `tcp` option). */
  readonly port?: number;
  close(): Promise<void>;
}

export interface StoreLinkOptions {
  /** Run a statement with $1.. parameters; resolve to the rows (objects by column name). */
  query(sql: string, params: unknown[]): Promise<Array<Record<string, unknown>>>;
  /** Optional: subscribe to a NOTIFY channel; resolve to (or return) a function that unsubscribes. */
  listen?(channel: string, onPayload: (payload: string) => void): Promise<() => unknown> | (() => unknown);
  /** The database's system id (default: SELECT tinyactors.system_id()). */
  id?: string;
  /** How often to look for envelopes without a NOTIFY (default 1000 ms; 0 = only on NOTIFY). */
  pollMs?: number;
  /** Envelopes claimed per query (default 100). */
  batch?: number;
  /** This claimer's name in the outbox (default: <system id>/<random>). */
  worker?: string;
  /** Trust the database's actors: they may send to any local actor and relay through us (default true). */
  trusted?: boolean;
  /** Tell the database which systems we reach (browsers on our links), so their envelopes come to us (default true). */
  announce?: boolean;
  /** Envelopes the router keeps while the database is unreachable (default 10000). */
  maxQueue?: number;
  queueTTLMs?: number;
}

export interface StoreLink {
  /** The database's system id (once known). */
  readonly id: string | null;
  /** Resolves once connected (rejects if the first query fails). */
  readonly ready: Promise<void>;
  /** Claim and deliver what the database has for us now. */
  drain(): Promise<void>;
  /** Resolves when everything handed to the link reached the database and nothing is being claimed. */
  flush(): Promise<void>;
  status(): (PeerStatus & { error: string | null; pending: number }) | null;
  close(): Promise<void>;
}

/**
 * Connect `system` to a database of actors (PostgreSQL with the tinyactors extension; docs/protocol.md,
 * "Store links") through your own PostgreSQL client. With node-postgres:
 * `createStoreLink(system, { query: (q, p) => pool.query(q, p).then((r) => r.rows) })`.
 */
export declare function createStoreLink(system: System, options: StoreLinkOptions): StoreLink;

/** Accept links from other systems (e.g. browsers) into `system`. */
export declare function serveLinks(system: System, options: ServeLinksOptions): LinkServer;

export declare function createActor<M extends AnyActorLogic>(
  logic: M,
  options?: ActorOptions<InputFrom<M>>,
): Actor<M>;

/* ---------------------------------------------------------- action creators */

export declare function assign<TContext extends MachineContext, TEvent extends EventObject = EventObject>(
  assignment: Assigner<TContext, TEvent>,
): AssignAction<TContext, TEvent>;
export declare function raise<
  TSent extends EventObject,
  TContext = any,
  TEvent extends EventObject = EventObject,
>(
  event: EventOrExpr<TContext, TEvent, TSent>,
  options?: SendOptions<TContext, TEvent>,
): RaiseAction<TContext, TEvent>;
export declare function sendTo<
  TSent extends EventObject,
  TContext = any,
  TEvent extends EventObject = EventObject,
>(
  to: SendTarget<TContext, TEvent>,
  event: EventOrExpr<TContext, TEvent, TSent>,
  options?: SendOptions<TContext, TEvent>,
): SendToAction<TContext, TEvent>;
export declare function sendParent<
  TSent extends EventObject,
  TContext = any,
  TEvent extends EventObject = EventObject,
>(
  event: EventOrExpr<TContext, TEvent, TSent>,
  options?: SendOptions<TContext, TEvent>,
): SendParentAction<TContext, TEvent>;
export declare function log<TContext = any, TEvent extends EventObject = EventObject>(
  value?: string | ((args: ActionArgs<TContext, TEvent>) => unknown),
  label?: string,
): LogAction<TContext, TEvent>;
export declare function cancel(sendID: string): CancelAction;
export declare function stateIn(stateValue: string): StateInGuard;
export declare function and<TContext = any, TEvent extends EventObject = EventObject>(
  guards: ReadonlyArray<Guard<TContext, TEvent>>,
): CompositeGuard<TContext, TEvent>;
export declare function or<TContext = any, TEvent extends EventObject = EventObject>(
  guards: ReadonlyArray<Guard<TContext, TEvent>>,
): CompositeGuard<TContext, TEvent>;
export declare function not<TContext = any, TEvent extends EventObject = EventObject>(
  guard: Guard<TContext, TEvent>,
): CompositeGuard<TContext, TEvent>;

// ---------------------------------------------------------------------------
// Tracing (docs/adr/0006 in the repository root): core writes the records,
// C encodes them as OTLP protobuf; no telemetry dependency.

/** Core's trace record kinds, in core's order. */
export type TraceKind =
  | 'dropped' | 'spawned' | 'finished' | 'destroyed' | 'unloaded' | 'loaded'
  | 'macrostepBegin' | 'macrostepEnd' | 'microstep' | 'condition' | 'raised'
  | 'sent' | 'invoked' | 'cancelled' | 'assign' | 'log' | 'actionFailed';

export declare const TRACE_KINDS: readonly TraceKind[];

export type TraceSink = (traces: Uint8Array, logs?: Uint8Array) => void;

export interface TraceConfiguration {
  /**
   * "full" (default): every kind; "decisions": a span per macrostep and
   * microstep, conditions, raised events, lifecycle spans and failures;
   * "summary": a span per macrostep, lifecycle spans and failures.
   */
  detail?: 'summary' | 'decisions' | 'full';
  /** The record kinds wanted, overriding `detail`. Core writes no other record. */
  kinds?: TraceKind[];
  /**
   * The ratio (0 to 1, default 1) of the traces this system starts that are
   * sampled, decided as OpenTelemetry's TraceIDRatioBased does. A
   * traceparent keeps its caller's decision. Lifecycle spans are kept
   * whatever the decision.
   */
  sample?: number;
  /**
   * Decisions where something new starts (docs/adr/0007): when a trace
   * starts and when an actor takes an event. At each point the first rule
   * that matches decides, and what the step causes follows it. Watches come
   * first.
   */
  rules?: TraceRule[];
}

/** The detail a rule or a watch gives the traces it samples. */
export type TraceDetail = 'summary' | 'decisions' | 'full' | 'off' | { kinds: TraceKind[]; values?: boolean };

export interface TraceRule {
  /** Where it is asked (default: both). */
  at?: 'start' | 'take' | ('start' | 'take')[];
  /** A chart's name (a machine's id), or the machine. */
  chart?: string | { id: string };
  /** An actor's name; a trailing "*" makes it a prefix. */
  actor?: string;
  /** An event descriptor, SCXML's prefix rule ("error", "webhook.*"). */
  event?: string;
  session?: number | string;
  from?: TraceOrigin | TraceOrigin[];
  /** true: sample; false: do not; a ratio: sample that share (by trace ID). */
  sample?: boolean | number;
  /** The detail of what it samples (default: as sampled traces). */
  detail?: TraceDetail;
  /** Event data and assigned values for what it samples. */
  values?: boolean;
  /** Matches at most this often (per second of the system's clock). */
  perSecond?: number;
  burst?: number;
}

export type TraceOrigin = 'host' | 'actor' | 'timer' | 'spawn' | 'sampled' | 'unsampled';

export interface TraceWatch {
  actors?: { readonly id?: string }[];
  sessions?: (number | string)[];
  /** Actor names; a trailing "*" makes one a prefix. */
  names?: string[];
  /** Chart names (machine ids). */
  charts?: string[];
  /** Event descriptors. */
  events?: string[];
  /** Default "full". */
  detail?: TraceDetail;
  values?: boolean;
  /** Milliseconds until it ends by itself (default 15 minutes; Infinity: until stop()). */
  for?: number;
  /** At most this many new traces a second. */
  perSecond?: number;
}

export interface TraceSinkOptions extends TraceConfiguration {
  /** Event data and assigned values as JSON attributes (off by default: they can hold personal data). */
  values?: boolean;
  /** Bytes of a value in an attribute, cut there (default 256). */
  valueLimit?: number;
  /** "cause" (default): a macrostep's parent is its cause; "link": a root, linked to its cause. */
  parent?: 'cause' | 'link';
  /** Failures and <log> as OTLP log records (default true). */
  logs?: boolean;
  /** Resource attributes every request carries, such as `service.name`. */
  resource?: Record<string, string>;
}

/** The OTLP/HTTP protobuf exporter's settings. */
export interface TelemetryOptions {
  /** The base URL: /v1/traces and /v1/logs are added (OTEL_EXPORTER_OTLP_ENDPOINT). */
  endpoint?: string;
  /** The traces URL, as it is (OTEL_EXPORTER_OTLP_TRACES_ENDPOINT). */
  tracesEndpoint?: string;
  /** The logs URL, as it is; "" sends no logs. */
  logsEndpoint?: string;
  /** The metrics URL, as it is; "" sends no metrics (OTEL_EXPORTER_OTLP_METRICS_ENDPOINT, OTEL_METRICS_EXPORTER=none). */
  metricsEndpoint?: string;
  /** Milliseconds between metrics exports; 0 exports only when the system stops (OTEL_METRIC_EXPORT_INTERVAL, default 60000). */
  metricsInterval?: number;
  /** The application's own metrics, exported with the system's at each interval. */
  metrics?: () => Metric[];
  /** Headers of every request (OTEL_EXPORTER_OTLP_HEADERS). */
  headers?: Record<string, string>;
  /** Resource attributes (OTEL_SERVICE_NAME, OTEL_RESOURCE_ATTRIBUTES). */
  resource?: Record<string, string>;
  /** The sampling ratio (OTEL_TRACES_SAMPLER_ARG); a sink's `sample` wins. */
  sample?: number;
  /** No export (OTEL_SDK_DISABLED=true). */
  disabled?: boolean;
  /** How to post: (url, headers, body) => a promise (default: the platform's HTTP client, or fetch). */
  post?: (url: string, headers: Record<string, string>, body: Uint8Array) => Promise<void>;
}

/** A sum's or gauge's value at one set of attributes. */
export interface MetricPoint {
  attributes: Record<string, string>;
  value: number;
}

/** A bucket's sample, with the span it was measured in. */
export interface Exemplar {
  /** The bucket's index in `buckets`. */
  bucket: number;
  value: number;
  /** Milliseconds since the epoch. */
  time: number;
  traceID: string;
  spanID: string;
}

/** A distribution with explicit bucket bounds (an OTLP Histogram point). */
export interface HistogramPoint {
  attributes: Record<string, string>;
  count: number;
  sum: number;
  /** Present once something was recorded. */
  min?: number;
  max?: number;
  /** Inclusive upper bounds; `buckets` has one more entry, for the rest. */
  bounds: number[];
  buckets: number[];
  exemplars: Exemplar[];
}

/** One metric: a cumulative monotonic sum, a gauge, or a histogram. */
export type Metric =
  | { name: string; unit: string; kind: 'sum' | 'gauge'; points: MetricPoint[] }
  | { name: string; unit: string; kind: 'histogram'; points: HistogramPoint[] };

export interface MetricsSnapshot {
  /** Milliseconds since the epoch: when the system started counting, and now. */
  startTime: number;
  time: number;
  metrics: Metric[];
}

/**
 * One OTLP ExportMetricsServiceRequest (protobuf) of a snapshot, to post to
 * `/v1/metrics`: system.metrics()'s, or the application's own in the same
 * shape. `resource` holds the resource's attributes.
 */
export declare function metricsToOTLP(
  snapshot: { startTime?: number; time?: number; metrics: Metric[] },
  options?: { resource?: Record<string, string> },
): Uint8Array;

/** A span as decodeTraces() reads it. IDs are lowercase hex. */
export interface DecodedSpan {
  traceID: string;
  spanID: string;
  parentSpanID?: string;
  name: string;
  kind: 'unspecified' | 'internal' | 'server' | 'client' | 'producer' | 'consumer';
  /** Milliseconds since the epoch. */
  startTime: number;
  endTime: number;
  startTimeUnixNano: bigint;
  endTimeUnixNano: bigint;
  attributes: Record<string, unknown>;
  events: { name: string; time: number; timeUnixNano: bigint; attributes: Record<string, unknown> }[];
  links: { traceID: string; spanID: string; attributes: Record<string, unknown> }[];
  status: { code: 'unset' | 'ok' | 'error'; message?: string };
  resource: { attributes: Record<string, unknown> };
  /** One scope per (system, chart): its attributes name the chart. */
  scope: { name: string; version: string; attributes: Record<string, unknown> };
}

/** Reads OTLP protobuf trace bytes (C's decoder); throws on a malformed message. */
export declare function decodeTraces(bytes: Uint8Array): DecodedSpan[];

/**
 * A W3C traceparent's fields, or undefined when it is not a valid one
 * (C's parser, the one every send uses). Spaces and tabs around it are
 * ignored; a later version is read by its first four fields.
 */
export declare function parseTraceparent(header: string | undefined):
  { traceID: string; parentSpanID: string; flags: number; sampled: boolean } | undefined;
/** A traceparent naming `spanID` as the parent; sampled unless `sampled: false` or `flags` says otherwise. */
export declare function formatTraceparent(span: { traceID: string; spanID: string; sampled?: boolean; flags?: number }): string;
/** The span ID of a step: its session, macrostep and microstep (0, the default: the macrostep's span). */
export declare function stepSpanID(session: bigint | number | string, macrostep: bigint | number | string, microstep?: number): string;
