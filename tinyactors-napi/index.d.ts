/**
 * The package version. The addon is being rebuilt step by step (ROADMAP.md);
 * so far createSystem(), System.close(), System.closed, System.stats(),
 * System.define(), System.findDefinition(), Definition, statechart(),
 * literal(), TinyactorsError, spawn(), inspect(), destroy(), flush(),
 * post() with data, the `done`, `fault` and `deadLetter` hooks, the
 * realtime and manual clocks (time, advanceTo(), advanceBy()), delayed
 * sends, paged enumeration, the JavaScript data model, send(), services
 * (I/O processors, invokers, the log sink), and capture, unload, load,
 * reconstruct and replacement are implemented: ROADMAP steps 1-14.
 */
export const version: string;

/* --------------------------------------------------------------------------
 * Systems and scheduling
 * -------------------------------------------------------------------------- */

export interface SystemOptions {
  /**
   * "realtime" (the default) follows a monotonic clock read natively; one
   * unreferenced timer fires due sends, so an idle system does not keep the
   * process alive. "manual" time moves only with advanceTo()/advanceBy()
   * (ADR 0007 point 3).
   */
  clock?: "realtime" | "manual";
  /** Core's logical time at creation, in milliseconds; default 0. */
  initialTime?: number;
  scheduling?: SchedulingOptions;
  /**
   * What happens to an actor that reaches a top-level final state (ADR 0012):
   * "destroy" (the default) destroys it after the `done` hook returns;
   * "retain" keeps it inspectable until it is destroyed.
   */
  finished?: "destroy" | "retain";
  /** Called at the end of the pump turn in which an actor finished. */
  done?: (record: DoneRecord) => void;
  /** Called at the end of the pump turn in which an actor faulted (ADR 0008). */
  fault?: (record: FaultRecord) => void;
  /**
   * Called for a message whose target is gone, finished, or refused it, at
   * most `messageBudget` times per pump turn (ADR 0008). Without a hook,
   * dead letters are only counted in stats().
   */
  deadLetter?: (record: DeadLetterRecord) => void;
  io?: Readonly<Record<string, IOProcessor>>;
  invokers?: Readonly<Record<string, Invoker>>;
  log?: LogSink;
}

/** An actor finished (ADR 0012). The actor is still resident while the hook runs. */
export interface DoneRecord<Data = unknown> {
  readonly actor: Actor;
  /** The top-level final state it reached (core ADR 0056). */
  readonly finalState: string;
  /** Its completion data (<donedata>), or undefined. */
  readonly data: Data | undefined;
}

/** A core call for an actor failed; the actor is quarantined (ADR 0008). */
export interface FaultRecord {
  readonly actor: Actor;
  readonly operation: string;
  readonly code: string;
  readonly message: string;
}

/**
 * A message that could not be delivered (ADR 0008). `target` is the actor it
 * was for, or the address when it was sent to a name with no resident actor
 * (ADR 0006).
 */
export interface DeadLetterRecord {
  readonly target: ActorID | ActorAddress;
  readonly event: EventName;
  /**
   * `unloaded`: a name whose actor is unloaded (load() brings it back; mail is
   * not held for it), or mail left for an actor unloaded through the raw layer.
   */
  readonly reason: "not-found" | "done" | "destroyed" | "refused" | "unloaded";
}

export interface SchedulingOptions {
  /** Microsteps one resume may run before the actor yields (core's `budget`); default 64. */
  resumeBudget?: number;
  /**
   * Units of executable content one resume may run before the actor pauses
   * inside a microstep (core ADR 0061); 0 means no limit; default 1024.
   */
  actionBudget?: number;
  /** Runnable actors resumed in one pump turn; default 1024. */
  resumesPerTurn?: number;
  /** Maximum due sends dispatched during one pump turn; at least 1, default 1024. */
  dueBudget?: number;
  /**
   * Deliveries started during one pump turn, and dead letters reported in
   * one turn; at least 1, default 1024.
   */
  messageBudget?: number;
  /**
   * Maximum accepted messages not yet delivered or reported; post() throws
   * `queue-full` beyond it. 0 or absent: no limit.
   */
  maxQueuedMessages?: number;
  /**
   * Maximum retained-bytes estimate of the data of accepted messages not yet
   * delivered or reported (ADR 0005 point 9); 0 or absent: no limit. The
   * estimate: `options.bytes`, or 2 bytes per string code unit, byteLength
   * for buffers, 64 for other objects, 0 for primitives. Mail between actors
   * carries no estimate.
   */
  maxRetainedBytes?: number;
}

export declare function createSystem(options?: SystemOptions): System;

export interface System {
  readonly closed: boolean;
  readonly clock: "realtime" | "manual";
  readonly time: number;

  /**
   * Compile and register a chart: a statechart() builder, or a spec in the
   * generated shape (the shape core's definition export returns). A refused
   * chart throws `invalid` or `unsupported`, with core's compile error as
   * `detail` (ChartRefusal).
   */
  define<Data extends object = Record<string, unknown>>(
    source: DefinitionSpec | DefinitionBuilder<Data>,
  ): Definition<Data>;

  /** The newest registered, unretired definition with this identity. */
  findDefinition(identity: DefinitionIdentity): Definition | undefined;

  spawn<Data extends object>(
    definition: Definition<Data>,
    options?: SpawnOptions<Data>,
  ): Actor<Data>;

  /** The resident actor the directory has at `address`, or undefined (ADR 0006). */
  findActor(address: ActorAddress): Actor | undefined;

  /** A snapshot of core's view of `actor`. Throws `not-found` once it is gone. */
  inspect<Data extends object>(actor: ActorID): ActorInspection<Data>;
  /** True while a resident actor has this ID. */
  exists(actor: ActorID): boolean;
  /** Terminate `actor`. Throws `not-found` once it is gone. */
  destroy(actor: ActorID): void;
  /** Put an actor that faulted back into scheduling (ADR 0008). */
  release(actor: ActorID): void;

  /**
   * Capture `actor` in memory (core ADR 0013), with the data-model root it
   * has now. `busy` for an unpublished (prepared) actor.
   */
  capture<Data extends object>(actor: ActorID): SessionImage<Data>;

  /**
   * Evict a named actor with a capture taken since its last change (`stale`
   * otherwise). Throws `busy` while it has mail waiting, is inside a
   * macrostep, or is quarantined. Mail to its address is then a dead letter
   * with reason `unloaded` until load(); its ActorID is stale.
   */
  unload(actor: ActorID, image: SessionImage): void;

  /**
   * Bring an unloaded named actor back: the same session ID, a new runtime
   * ID (core ADR 0066). `binding` defaults to the root the image kept. An
   * invocation active at capture is reported to the actor as
   * `error.communication.invoke.<id>.unavailable` (core ADR 0038).
   */
  load<Data extends object>(
    image: SessionImage<Data>,
    options?: { readonly binding?: Partial<Data> | Data },
  ): Actor<Data>;

  /**
   * Restore an image under a fresh runtime ID without touching the directory
   * (core ADR 0052). `binding` defaults to the root the image kept, which is
   * shared with the captured actor if it still runs.
   */
  reconstruct<Data extends object>(
    image: SessionImage<Data>,
    options?: { readonly binding?: Partial<Data> | Data },
  ): Actor<Data>;

  /** Release the address of an actor that stays unloaded; load() of its image then fails with `stale`. */
  removeUnloaded(address: ActorAddress): void;

  /** Create an unpublished replacement for the named actor `incumbent` (Actor.prepareReplacement). */
  prepareReplacement<Data extends object>(
    incumbent: ActorID,
    definition: Definition<Data>,
    options?: ReplacementOptions<Data>,
  ): PreparedActor<Data>;

  /** Publish a prepared actor and destroy its incumbent (PreparedActor.activate). */
  activate<Data extends object>(prepared: PreparedActor<Data>): Actor<Data>;

  registerIO(type: string, processor: IOProcessor, options?: IOProcessorOptions): void;
  /** Open a scope that stores `uri` once, for a connection or a session (core ADR 0068). */
  openScope(type: string, uri: string): IOScope;
  /** Close a scope; events naming it then carry no origin. Idempotent. */
  closeScope(scope: IOScope): void;
  registerInvoker(type: string, invoker: Invoker): void;
  setLogSink(sink?: LogSink): void;
  /**
   * Hand traces to `sink` as OTLP protobuf (napi ADR 0016): once per pump
   * turn, one Uint8Array holding one ExportTraceServiceRequest with the
   * spans that ended. Post it to an OTLP/HTTP `/v1/traces` endpoint as it
   * is, or read it with decodeTraces(). Without a sink core builds no record.
   */
  setTraceSink(sink?: (traces: Uint8Array, logs?: Uint8Array) => void, options?: TraceSinkOptions): void;
  /** Changes `detail`, `kinds` and `sample` of the installed sink in place; open spans are kept. */
  configureTraces(options: TraceConfiguration): void;
  /**
   * Traces matching deliveries in full, sampled whatever the ratio, until
   * stop(): to these sessions (IDs or actors), to the actors at these
   * addresses, and to the actors of these definition families. What a
   * watched delivery causes is watched too.
   */
  watchTraces(watch: {
    sessions?: (bigint | number | ActorID)[];
    families?: string[];
    addresses?: { family: string; name: string }[];
  }): { stop(): void };
  /** Metrics as plain data: core's counters and occupancy, and napi's messages and memory. */
  metrics(): MetricsSnapshot;

  /**
   * Queue a message and return; a later pump turn delivers it (ADR 0005).
   * Messages to one target are delivered in the order they were accepted.
   * Mail for an actor that is gone is a dead letter, not an error. An
   * address is resolved through the directory when the post is accepted; a
   * name with no resident actor is a dead letter too (ADR 0006). Throws
   * `queue-full` at maxQueuedMessages or maxRetainedBytes. `data` is any JavaScript value, shared
   * with the receiver, not copied (ADR 0011); the receiver's expressions see
   * it as `context.event.data`.
   */
  post(target: ActorTarget, event: EventName, data?: unknown, options?: PostOptions): void;

  /**
   * Queue a message and return a promise (ADR 0005 point 11). With
   * `until: "completed"` (the default) it resolves once the macrostep the
   * message starts has ended, with the actor stable or done; with
   * `until: "accepted"` once the message is queued. It rejects when the
   * message becomes a dead letter, when its macrostep faults, on `signal`
   * (`aborted`), after `timeout` (`timeout`), when the queue is full
   * (`queue-full`), and on close() (`closed`). Abort and timeout withdraw a
   * message still queued; a delivered one is not undone. Only misuse throws.
   */
  send(
    target: ActorTarget,
    event: EventName,
    data?: unknown,
    options?: SendOptions,
  ): Promise<SendResult>;

  /** Resolve after all currently reachable work is quiescent. */
  flush(): Promise<void>;

  /**
   * Available only with a manual clock; throws `invalid` otherwise, or when
   * time would move backwards. Resolves once everything due has been
   * dispatched and the system is quiet.
   */
  advanceTo(time: number): Promise<void>;
  advanceBy(milliseconds: number): Promise<void>;

  /**
   * Resident actors as pages of IDs: each page is a Uint32Array of
   * interleaved [slot, generation] pairs (ADR 0003). Pages continue from
   * core's cursor, in slot order; a page is a snapshot (core ADR 0062).
   * Throws `reentrant` inside a core call.
   */
  actorIDs(options?: PageOptions): IterableIterator<Uint32Array>;
  /**
   * Resident actors' inspections, taken when the iterator reaches each
   * actor. Holds one page of IDs at a time; an actor destroyed after its
   * page was taken is skipped.
   */
  actors(options?: PageOptions): IterableIterator<ActorInspection>;
  definitions(): readonly DefinitionInspection[];

  /** Memory by category and allocator calls (ADRs 0009, 0014). Throws `closed` after close(). */
  stats(): SystemStats;

  /**
   * Stop scheduling, reject pending sends, destroy the native system, and
   * release every retained JavaScript value. Idempotent.
   */
  close(): void;
  [Symbol.dispose](): void;
}

export interface MemoryCounters {
  readonly bytesInUse: number;
  /** High-water mark of bytesInUse. */
  readonly peakBytes: number;
  /** The largest single allocation so far; in `total`, the largest of any category. */
  readonly largestBlock: number;
  readonly allocations: number;
  readonly frees: number;
}

export interface PageOptions {
  /** Actors per page; default 1024, at most 65536. */
  readonly pageSize?: number;
}

export interface SystemStats {
  readonly memory: {
    readonly total: MemoryCounters;
    readonly core: MemoryCounters;
    readonly envelopes: MemoryCounters;
    readonly scheduling: MemoryCounters;
    readonly values: MemoryCounters;
    readonly directory: MemoryCounters;
    readonly scratch: MemoryCounters;
    readonly other: MemoryCounters;
  };
  /** Allocations plus frees: zero growth in steady state is the test signal. */
  readonly allocatorCalls: number;
  readonly messages: {
    /** Accepted messages not yet delivered or reported as dead letters. */
    readonly queued: number;
    readonly delivered: number;
    readonly deadLetters: number;
  };
  /** JavaScript values core and the mailboxes hold (ADR 0011). */
  readonly values: {
    /** Value boxes live now; numbers, booleans, null and undefined need none. */
    readonly live: number;
    /** Value boxes made since the system was created. */
    readonly made: number;
  };
}

export type EventName = string;
export type ActorTarget = Actor | ActorID | ActorAddress;

export interface ActorAddress {
  readonly family: string;
  readonly name: string;
}

declare const definitionIDBrand: unique symbol;
declare const invocationIDBrand: unique symbol;
declare const sendIDBrand: unique symbol;

/**
 * An actor's ID: its slot and generation in the actor table, two small
 * integers (ADR 0002). Equality is by value; among live actors the slot alone
 * is unique.
 */
export interface ActorID {
  readonly system: System;
  readonly slot: number;
  readonly generation: number;
}
export type DefinitionID = bigint & { readonly [definitionIDBrand]: true };
export type InvocationID = bigint & { readonly [invocationIDBrand]: true };
export type SendID = bigint & { readonly [sendIDBrand]: true };

export interface PostOptions {
  /** A W3C traceparent: the macrostep this message starts runs under it (ADR 0015). */
  traceparent?: string;
  signal?: AbortSignal;
  /** The retained-bytes estimate of `data`, instead of the default (ADR 0005 point 9). */
  bytes?: number;
  /**
   * Who the receiver answers with `event.origin` and `event.originType`
   * (core ADR 0068): an I/O processor's type plus one request's reply URI,
   * a scope from openScope(), or a token its origin template renders.
   */
  origin?: IOOrigin;
}

export type IOOrigin =
  | { readonly type: string; readonly uri: string }
  | { readonly type: string; readonly scope: IOScope }
  | { readonly type: string; readonly token: number | bigint };

/** A scope from System.openScope(); stale once closed. */
export interface IOScope {
  readonly type: string;
  readonly uri: string;
  readonly index: number;
  readonly generation: number;
}

/**
 * Templates the native adapter renders without running JavaScript (core ADR
 * 0068): `location` is this session's address through the processor,
 * listed in `ioProcessors` ({session}, {family}, {name}); `origin` is the
 * URI of a computed origin ({token}).
 */
export interface IOProcessorOptions {
  location?: string;
  origin?: string;
}

export interface SendOptions extends PostOptions {
  /** "completed" (default): the message's macrostep ended; "accepted": it is queued. */
  until?: "completed" | "accepted";
  /** Reject with `timeout`, and withdraw the message if still queued, after this many milliseconds. */
  timeout?: number;
}

export interface SendResult {
  /** The actor that ran the macrostep; for "accepted", the target when it was an ActorID. */
  readonly actor?: Actor;
  readonly status: "accepted" | "stable" | "done";
  /** "done": the actor's done data. */
  readonly completion?: unknown;
}

/* --------------------------------------------------------------------------
 * Definitions
 * -------------------------------------------------------------------------- */

/**
 * A revision is stored as bytes. It reads back as a string when the bytes are
 * valid UTF-8, and as a Uint8Array otherwise.
 */
export interface DefinitionIdentity {
  readonly family: string;
  readonly revision: string | Uint8Array;
}

/** Core's compile error, attached as `detail` when define() refuses a chart. */
export interface ChartRefusal {
  /** The compile error code, e.g. "expression_rejected" or "unknown_state". */
  readonly code: string;
  /** Where in the spec, e.g. "states[0].transitions[1].condition". */
  readonly path: string;
  /** Core's compile error as the generated layer returns it. */
  readonly error: {
    readonly code: string;
    readonly path: readonly { readonly member: string; readonly index: number }[];
    readonly subject?: string;
    readonly related?: string;
    readonly message: string;
  };
}

export interface Definition<Data extends object = Record<string, unknown>> {
  readonly system: System;
  readonly id: DefinitionID;
  readonly identity: DefinitionIdentity;
  /**
   * True once retire() has run. Core frees a retired definition when no
   * actor uses it; a freed definition still reads as retired.
   */
  readonly retired: boolean;

  /** Read from core. Throws `not-found` once a retired definition is freed. */
  inspect(): DefinitionInspection;

  /** Prevent new actors from using this definition. Existing actors continue. */
  retire(): void;
}

export interface DefinitionInspection {
  readonly id: DefinitionID;
  readonly identity: DefinitionIdentity;
  readonly name?: string;
  /** The data model the chart was compiled against, e.g. "null". */
  readonly datamodel: string;
  readonly binding: "early" | "late";
  readonly retired: boolean;
  readonly actorCount: number;
  readonly stateCount: number;
  readonly actorFixedBytes: number;
  readonly definitionBytes: number;
}

/* --------------------------------------------------------------------------
 * Fluent definition builder
 * -------------------------------------------------------------------------- */

export interface StatechartOptions extends DefinitionIdentity {
  readonly name?: string;
  readonly binding?: "early" | "late";
}

/**
 * Begin an ordered JavaScript-authored statechart definition. The builder is
 * mutable until build() or System.define() consumes it.
 */
export declare function statechart<Data extends object>(
  options: StatechartOptions,
): DefinitionBuilder<Data>;

export type Target = string | readonly string[];
declare const literalValueBrand: unique symbol;
export interface LiteralValue<Value> {
  readonly [literalValueBrand]: true;
  readonly value: Value;
}

/** Force a callable JavaScript value to be data rather than an expression. */
export declare function literal<Value>(value: Value): LiteralValue<Value>;

export type ValueInput<Data extends object, Value = unknown> =
  | (Value extends (...arguments_: never[]) => unknown ? never : Value)
  | LiteralValue<Value>
  | Expression<Data, Value>;

export interface DefinitionBuilder<Data extends object>
  extends StateContainerBuilder<Data, DefinitionBuilder<Data>> {
  data<Key extends keyof Data & string>(name: Key, initial: Data[Key]): this;
  dataExpression<Key extends keyof Data & string>(
    name: Key,
    initial: Expression<Data, Data[Key]>,
  ): this;
  initial(target: Target, actions?: ActionBlock<Data>): this;
  /** A script run once at initialization, after early-bound data (SCXML 3.2.2). */
  script(run: Script<Data>): this;
  build(): DefinitionSpec;
}

export type ActionBlock<Data extends object> = (
  actions: ActionBuilder<Data>,
) => void;

export type StateBlock<Data extends object> = (
  state: StateBuilder<Data>,
) => void;

export interface StateContainerBuilder<Data extends object, Self> {
  state(id: string, build?: StateBlock<Data>): Self;
  parallel(id: string, build: StateBlock<Data>): Self;
  final(id: string, build?: StateBlock<Data>): Self;
  history(
    id: string,
    depth: "shallow" | "deep",
    build?: StateBlock<Data>,
  ): Self;
}

export interface StateBuilder<Data extends object>
  extends StateContainerBuilder<Data, StateBuilder<Data>> {
  /** Override the kind normally inferred from the builder method and children. */
  kind(kind: StateKind): this;
  initial(target: Target, actions?: ActionBlock<Data>): this;

  data<Key extends keyof Data & string>(name: Key, initial: Data[Key]): this;
  dataExpression<Key extends keyof Data & string>(
    name: Key,
    initial: Expression<Data, Data[Key]>,
  ): this;

  /** Each call creates one separately-failing executable-content block. */
  entry(build: ActionBlock<Data>): this;
  exit(build: ActionBlock<Data>): this;

  /** Add an external transition in document order. */
  on(event: EventName, build?: TransitionBlock<Data>): this;
  /** Add an eventless transition in document order. */
  always(build: TransitionBlock<Data>): this;
  invoke(build: InvokeBlock<Data>): this;
  doneData(build: DoneDataBlock<Data>): this;
}

export type TransitionBlock<Data extends object> = (
  transition: TransitionBuilder<Data>,
) => void;

export interface TransitionBuilder<Data extends object>
  extends ActionSequenceBuilder<Data, TransitionBuilder<Data>> {
  /** A function, or source text: "In('s')" or a data name. */
  when(condition: Condition<Data> | string): this;
  target(target: Target): this;
  internal(): this;
  external(): this;
}

export interface ActionBuilder<Data extends object>
  extends ActionSequenceBuilder<Data, ActionBuilder<Data>> {}

export interface ActionSequenceBuilder<Data extends object, Self> {
  raise(event: EventName, data?: ValueInput<Data>): Self;

  send(event: StringSpec<Data>, build?: SendBlock<Data>): Self;
  cancel(sendID: StringSpec<Data>): Self;

  /** Assign a declared top-level data property. */
  assign<Key extends keyof Data & string>(
    location: Key,
    value: ValueInput<Data, Data[Key]>,
  ): Self;
  /** Assign through an explicit location, for nested or computed storage. */
  assignAt<Value>(
    location: Location<Data, Value>,
    value: ValueInput<Data, Value>,
  ): Self;

  script(run: Script<Data>): Self;
  log(label?: string, value?: Expression<Data> | string): Self;

  choose(
    condition: Condition<Data> | string,
    then: ActionBlock<Data>,
    otherwise?: ActionBlock<Data>,
  ): Self;

  forEach<Item>(
    array: Expression<Data, readonly Item[]> | (keyof Data & string) | LiteralValue<readonly Item[]>,
    item: keyof Data & string,
    build: ActionBlock<Data>,
    index?: keyof Data & string,
  ): Self;
}

export type SendBlock<Data extends object> = (send: SendBuilder<Data>) => void;

export interface SendBuilder<Data extends object> {
  to(target: StringSpec<Data>): this;
  via(ioType: StringSpec<Data>): this;
  after(delay: DelaySpec<Data>): this;
  id(id: string): this;
  idLocation(location: keyof Data & string | Location<Data, string>): this;
  data(value: ValueInput<Data>): this;
  param(name: string, value: ValueInput<Data>): this;
}

export type InvokeBlock<Data extends object> = (
  invoke: InvokeBuilder<Data>,
) => void;

export interface InvokeBuilder<Data extends object> {
  id(id: string): this;
  idLocation(location: keyof Data & string | Location<Data, string>): this;
  type(type: StringSpec<Data>): this;
  source(source: StringSpec<Data>): this;
  content(value: ValueInput<Data>): this;
  param(name: string, value: ValueInput<Data>): this;
  autoforward(enabled?: boolean): this;
  finalize(build: ActionBlock<Data>): this;
}

export type DoneDataBlock<Data extends object> = (
  done: DoneDataBuilder<Data>,
) => void;

export interface DoneDataBuilder<Data extends object> {
  content(value: ValueInput<Data>): this;
  param(name: string, value: ValueInput<Data>): this;
}

/* --------------------------------------------------------------------------
 * Actors and lifecycle
 * -------------------------------------------------------------------------- */

export interface SpawnOptions<Data extends object> {
  /** A W3C traceparent: the bootstrap runs under it (ADR 0015). */
  traceparent?: string;
  /** Omit both name and address for an anonymous actor. */
  readonly name?: string;
  readonly address?: ActorAddress;
  readonly binding?: Partial<Data>;
}

/**
 * The view spawn() returns (ADR 0002): an ActorID with methods that ask the
 * system. It holds no native resource and no finalizer.
 */
export interface Actor<Data extends object = Record<string, unknown>> extends ActorID {
  readonly destroyed: boolean;
  equals(other: ActorID): boolean;

  post(event: EventName, data?: unknown, options?: PostOptions): void;
  send(event: EventName, data?: unknown, options?: SendOptions): Promise<SendResult>;
  inspect(): ActorInspection<Data>;

  /** A retained reference to the actor's live data-model root. */
  data(): Data;

  /** Capture Tinyactors-owned state and the data-model root in memory (System.capture). */
  capture(): SessionImage<Data>;

  /** Evict this named actor using a capture taken since its last change (System.unload). */
  unload(image: SessionImage<Data>): void;

  /**
   * Prepare a fresh unpublished actor of `definition` to replace this named
   * actor in place. It bootstraps like a spawned actor, and core holds its
   * visible bootstrap effects until activation. `binding` defaults to this
   * actor's live data root when `definition` uses the JavaScript data model.
   */
  prepareReplacement(
    definition: Definition<Data>,
    options?: ReplacementOptions<Data>,
  ): PreparedActor<Data>;

  /** Terminate this interpreter. Invoked children are not destroyed implicitly. */
  destroy(): void;
  [Symbol.dispose](): void;
}

export interface PreparedActor<Data extends object> extends Actor<Data> {
  /** False until activate() publishes it. */
  readonly published: boolean;
  readonly incumbent: ActorID;

  /**
   * Publish this actor at the incumbent's address, then destroy the
   * incumbent. Mail still waiting for the incumbent, and the send()s it
   * carries, move in order to the front of this actor's mailbox, so nothing
   * accepted for the address is lost; the incumbent's delayed sends are
   * discarded. Throws `busy` while the incumbent is inside a macrostep. If
   * activation fails, the incumbent keeps the address and this actor is left
   * for the caller to destroy.
   */
  activate(): Actor<Data>;
}

export interface ReplacementOptions<Data extends object> {
  /** Defaults to the incumbent actor's current data-model root. */
  readonly binding?: Partial<Data> | Data;
}

/** Core's residency: "idle" between macrosteps, "running" inside one, "done" once finished. */
export type ActorResidency = "idle" | "running" | "done" | "unloaded";

/**
 * A snapshot of core's view of one actor (ADR 0054: O(size of the actor)).
 * Invocations and the current event are not reported yet.
 */
export interface ActorInspection<Data = unknown> {
  readonly actor: Actor;
  /** Stable across unload and load; the runtime ID is not (core ADR 0066). */
  readonly sessionID: bigint;
  readonly definition: DefinitionIdentity;
  readonly residency: ActorResidency;
  readonly published: boolean;
  readonly activeStates: readonly string[];
  readonly macrostepInProgress: boolean;
  readonly internalEventCount: number;
  readonly delayedSendCount: number;
  /** Pending delayed sends, by due time then send order (core ADR 0054). */
  readonly delayedSends: readonly DelayedSendInspection[];
  /** The addon's scheduling state (ADR 0005). */
  readonly scheduling: "none" | "idle" | "ready" | "runnable" | "quarantined";
  /** The top-level final state it reached, or null (core ADR 0056). */
  readonly finalState: string | null;
  readonly allocatedBytes: number;
  /** The live data object (JavaScript data model), shared, not copied; undefined under the null data model. */
  readonly data?: Data;
  /** A finished actor's completion data (<donedata>), or undefined. */
  readonly completion?: unknown;
}

export interface InvocationInspection {
  readonly id: InvocationID;
  readonly type: string;
  readonly active: boolean;
  readonly autoforward: boolean;
}

/** One pending delayed send; `delayedSends` is ordered by due time, then send order. */
export interface DelayedSendInspection {
  /** The send ID as the chart names it (core ADR 0048), or null when it has none. */
  readonly id: string | null;
  readonly due: number;
  readonly event: EventName;
  readonly ioType: string;
  /** The resolved receiver for the built-in SCXML processor; null for custom processors, which use `target`. */
  readonly destination: ActorID | ActorAddress | null;
  readonly target: unknown;
  readonly data: unknown;
  /** The router refused this send once; it is waiting to be offered again. */
  readonly submitted: boolean;
}

/* --------------------------------------------------------------------------
 * In-memory capture, unload, and reconstruction
 * -------------------------------------------------------------------------- */

/**
 * A capture held in memory (ROADMAP step 14). Its handle is destroyed by
 * destroy(), by its finalizer, or by close() of its system, whichever comes
 * first; it never outlives its system (ADR 0012). Serializing it across
 * processes is not supported: model network handoff with I/O processors.
 */
export interface SessionImage<Data extends object = Record<string, unknown>> {
  readonly origin: System;
  /** The actor at capture; its ID is stale once the actor is unloaded. */
  readonly actor: ActorID;
  /** Stable across unload and load (core ADR 0066). */
  readonly sessionID: bigint;
  readonly definition: DefinitionIdentity;
  readonly address?: ActorAddress;
  /** The data-model root at capture (shared, not copied), or undefined under the null data model. */
  readonly data?: Data;
  /** The earliest pending delayed send's due time, or undefined. */
  readonly nextDeadline?: number;
  /** True after destroy() or close() of its system. */
  readonly destroyed: boolean;

  destroy(): void;
  [Symbol.dispose](): void;
}

/* --------------------------------------------------------------------------
 * JavaScript data-model callbacks
 * -------------------------------------------------------------------------- */

export interface EvaluationContext<Data extends object> {
  readonly data: Data;
  readonly event?: ActorEvent;
  readonly sessionID: string;
  readonly name?: string;
  readonly ioProcessors: Readonly<Record<string, { readonly location: string }>>;
  inState(state: string): boolean;
}

export type Expression<Data extends object, Result = unknown> = (
  this: Data,
  context: EvaluationContext<Data>,
) => Result;

export type Condition<Data extends object> = Expression<Data, unknown>;

export type Script<Data extends object> = (
  this: Data,
  context: EvaluationContext<Data>,
) => void;

export interface Location<Data extends object, Value = unknown> {
  get(this: Data, context: EvaluationContext<Data>): Value;
  set(this: Data, value: Value, context: EvaluationContext<Data>): void;
}

export type ValueSpec<Data extends object, Value = unknown> =
  | { readonly value: Value }
  | { readonly expression: Expression<Data, Value> }
  | { readonly location: Location<Data, Value> }
  | { readonly content: string | Uint8Array };

export type StringSpec<Data extends object> = string | Expression<Data, string>;

export type DelaySpec<Data extends object> =
  | number
  | `${number}ms`
  | `${number}s`
  | Expression<Data, number | string>;

/**
 * The event an expression sees as `context.event` (SCXML _event). `origin`
 * is a URI a send can target: `#_scxml_<sessionID>` or
 * `#_actor_<family>/<name>` (core ADR 0044), so replying is
 * `send("reply").to(({ event }) => event.origin)`.
 */
export interface ActorEvent<Data = unknown> {
  readonly name: EventName;
  readonly type: "external" | "internal" | "platform";
  readonly sendID?: string;
  readonly origin?: string;
  readonly originType?: string;
  readonly invocationID?: string;
  readonly data: Data;
}

export type EventOrigin =
  | { readonly kind: "actor"; readonly actor: ActorID | ActorAddress }
  | { readonly kind: "invocation"; readonly invocation: InvocationID }
  | { readonly kind: "io"; readonly value: bigint };

/* --------------------------------------------------------------------------
 * Statechart specs: the generated shape (ADR 0013)
 *
 * System.define() accepts this shape as a plain object; statechart() emits
 * it; core's definition export returns it. Member names follow core's C
 * spelling, and a tagged union is { kind, <member>: ... }.
 *
 * An expression is a JavaScript function (a condition, value or script,
 * called with `this` bound to the actor's data), a literal() value, or source
 * text: `In(state)`, or a data name, which reads that property (ADR 0011). A
 * location is a data name or a { get, set } object. A spec with data, a
 * script, or any function, literal() or location compiles against the
 * JavaScript data model ("javascript"); any other spec against core's null
 * data model ("null"). `datamodel` chooses explicitly.
 * -------------------------------------------------------------------------- */

/** A function, a literal(), or source text (In(state) or a data name). */
export type ExpressionSpec =
  | string
  | ((this: any, context: EvaluationContext<any>) => unknown)
  | LiteralValue<unknown>;
/** A data name, or a { get, set } location. */
export type LocationSpec = string | Location<any, unknown>;

export type StateKind =
  | "atomic"
  | "compound"
  | "parallel"
  | "final"
  | "history_shallow"
  | "history_deep";

export interface DefinitionSpec {
  /** The statechart document name. */
  readonly name?: string;
  /** The data model the chart requires; absent means no requirement. */
  readonly datamodel?: string;
  readonly identity?: {
    readonly family: { readonly name: string };
    readonly revision: string | Uint8Array;
  };
  readonly binding?: "default" | "early" | "late";
  readonly initial?: InitialSpec;
  /** Document order is semantically significant. */
  readonly states?: readonly StateSpec[];
  readonly data?: readonly DataSpec[];
  readonly script?: ExpressionSpec;
}

export interface StateSpec {
  readonly kind: StateKind;
  readonly id: string;
  readonly states?: readonly StateSpec[];
  readonly initial?: InitialSpec;
  readonly transitions?: readonly TransitionSpec[];
  /** Each handler is one separately-failing executable-content block. */
  readonly onentry?: readonly HandlerSpec[];
  readonly onexit?: readonly HandlerSpec[];
  readonly data?: readonly DataSpec[];
  readonly invokes?: readonly InvokeSpec[];
  readonly done_data?: DoneDataSpec;
}

export interface InitialSpec {
  readonly targets: readonly string[];
  readonly actions?: readonly ActionSpec[];
}

export interface TransitionSpec {
  readonly kind: "external" | "internal";
  /** Omit for an eventless transition. Wildcard segments are supported. */
  readonly event?: string;
  readonly condition?: ExpressionSpec;
  readonly targets?: readonly string[];
  readonly actions?: readonly ActionSpec[];
}

export interface HandlerSpec {
  readonly actions: readonly ActionSpec[];
}

export interface DataSpec {
  readonly name: string;
  readonly initializer?: ValueSourceSpec;
}

export type ValueSourceSpec =
  | { readonly kind: "expr"; readonly expr: ExpressionSpec }
  | { readonly kind: "location"; readonly location: LocationSpec }
  | { readonly kind: "content"; readonly content: { readonly media_type?: string; readonly bytes: Uint8Array } };

/** An `x` / `xexpr` attribute pair. */
export type StringAttributeSpec =
  | { readonly kind: "literal"; readonly literal: string }
  | { readonly kind: "expr"; readonly expr: ExpressionSpec };

export type ActionSpec =
  | { readonly kind: "raise"; readonly raise: { readonly event: string; readonly data?: ValueSourceSpec } }
  | { readonly kind: "send"; readonly send: SendSpec }
  | { readonly kind: "cancel"; readonly cancel: { readonly sendid: StringAttributeSpec } }
  | { readonly kind: "assign"; readonly assign: { readonly location: LocationSpec; readonly value?: ValueSourceSpec } }
  | { readonly kind: "script"; readonly script: { readonly script: ExpressionSpec } }
  | { readonly kind: "log"; readonly log: { readonly label?: string; readonly expr?: ExpressionSpec } }
  | {
    readonly kind: "if";
    readonly conditional: {
      readonly condition: ExpressionSpec;
      readonly then_actions?: readonly ActionSpec[];
      readonly else_actions?: readonly ActionSpec[];
    };
  }
  | {
    readonly kind: "foreach";
    readonly foreach: {
      readonly array: ExpressionSpec;
      readonly item: string;
      readonly index?: string;
      readonly body?: readonly ActionSpec[];
    };
  };

export interface SendSpec {
  readonly id?: { readonly kind: "literal"; readonly literal: string } | { readonly kind: "location"; readonly location: LocationSpec };
  readonly type?: StringAttributeSpec;
  readonly event?: { readonly kind: "name"; readonly name: string } | { readonly kind: "expr"; readonly expr: ExpressionSpec };
  readonly target?: StringAttributeSpec;
  /** A literal delay is an SCXML duration such as "500ms" or "5s". */
  readonly delay?: StringAttributeSpec;
  readonly data?: ValueSourceSpec;
  readonly params?: readonly ParamSpec[];
}

export interface ParamSpec {
  readonly name: string;
  readonly value?: ValueSourceSpec;
}

export interface InvokeSpec {
  readonly kind: "invoke";
  readonly id?: { readonly kind: "literal"; readonly literal: string } | { readonly kind: "location"; readonly location: LocationSpec };
  readonly type?: StringAttributeSpec;
  readonly src?: StringAttributeSpec;
  readonly data?: ValueSourceSpec;
  readonly autoforward?: boolean;
  readonly params?: readonly ParamSpec[];
  readonly finalize?: readonly ActionSpec[];
}

export interface DoneDataSpec {
  readonly content?: ValueSourceSpec;
  readonly params?: readonly ParamSpec[];
}

/* --------------------------------------------------------------------------
 * Host extensions
 * -------------------------------------------------------------------------- */

/**
 * An I/O processor for `<send type>` (ROADMAP step 13). Core calls it while an
 * actor runs, so the call is queued and send() runs at the end of that pump
 * turn, never inside core. A reply is an event the processor posts, now or
 * later. Throwing, or returning a promise that rejects, fails the send: the
 * sender gets an external `error.communication` event with data
 * { sendID, type, error }.
 */
export interface IOProcessor extends IOProcessorOptions {
  send(request: IORequest, context: EffectContext): void | Promise<void>;
}

export interface IORequest {
  /** The sending actor: an ActorID, or its address when it is named. */
  readonly source: ActorID | ActorAddress;
  /** The send's ID text (`<send id>` or core's generated one), or null for a send without one. */
  readonly sendID: string | null;
  readonly event: EventName;
  readonly type: string;
  readonly target: unknown;
  readonly data: unknown;
  /** A literal <content>, as text, with its media type. */
  readonly content?: string;
  readonly contentType?: string;
}

/** One per system: `signal` aborts on close(). */
export interface EffectContext {
  readonly system: System;
  readonly signal: AbortSignal;
  post(target: ActorTarget, event: EventName, data?: unknown, options?: PostOptions): void;
}

/**
 * An invoker for `<invoke type>` (ROADMAP step 13). start() runs at the end
 * of the pump turn in which core started the invocation; the service's
 * cancel() runs when the invoking state exits, and forward() gets
 * autoforwarded events.
 */
export interface Invoker {
  start(request: InvokeRequest, reporter: InvocationReporter): InvokedService | void;
}

export interface InvokeRequest {
  readonly system: System;
  readonly parent: Actor;
  readonly invocationID: InvocationID;
  /** The invoke ID's text: `<invoke id>` or the one core generated. */
  readonly invokeID?: string;
  readonly type: string;
  readonly source?: unknown;
  /** `<content expr>`'s value, or a literal <content> as text (then `contentType` is set). */
  readonly content?: unknown;
  readonly contentType?: string;
  readonly params?: unknown;
  readonly autoforward: boolean;
}

export interface InvokedService {
  cancel?(reason?: unknown): void | Promise<void>;
  forward?(event: { readonly name: EventName; readonly data?: unknown }): void | Promise<void>;
}

/**
 * Reports from an invocation to its parent. event() and done() arrive as
 * external events (done as done.invoke.<id>); unavailable() retires the
 * invocation and the parent gets error.communication. Each returns false
 * once the invocation has ended (done, unavailable, cancelled, closed).
 */
export interface InvocationReporter {
  /** Aborts when the invocation ends: cancel, done, unavailable, or close(). */
  readonly signal: AbortSignal;
  event(name: EventName, data?: unknown): boolean;
  done(data?: unknown): boolean;
  unavailable(error?: unknown): boolean;
}

export interface LogRecord {
  readonly source: ActorID | ActorAddress;
  readonly label?: string;
  readonly hasValue: boolean;
  readonly value?: unknown;
}

export type LogSink = (record: LogRecord) => void;

/* --------------------------------------------------------------------------
 * Public errors
 * -------------------------------------------------------------------------- */

export type TinyactorsErrorCode =
  | "done"
  | "invalid"
  | "out-of-memory"
  | "not-found"
  | "not-reconstructible"
  | "busy"
  | "unsupported"
  | "corrupt"
  | "definition-mismatch"
  | "unloaded"
  | "stale"
  | "queue-full"
  | "closed"
  | "reentrant"
  | "aborted"
  | "timeout";

export class TinyactorsError extends Error {
  readonly code: TinyactorsErrorCode;
  readonly detail?: unknown;
}

// ---------------------------------------------------------------------------
// Tracing (core ADR 0067, napi ADR 0016). OTLP bytes and a decoder; no
// telemetry dependency.

export type TraceKind =
  | 'spawned' | 'loaded' | 'unloaded' | 'finished' | 'destroyed'
  | 'macrostepBegin' | 'macrostepEnd' | 'condition' | 'microstep' | 'raised'
  | 'dataInit' | 'assign' | 'script' | 'foreach' | 'actionFailed'
  | 'sent' | 'cancelled' | 'invoked' | 'invokeCancelled'
  | 'paused' | 'resumed' | 'stopped';

export declare const TRACE_KINDS: readonly TraceKind[];

export interface TraceConfiguration {
  /**
   * "summary" (default): a span per macrostep, lifecycle spans and failures;
   * "decisions": adds microsteps, conditions and raised events; "full":
   * every kind. Watched actors (watchTraces) always get every kind.
   */
  detail?: 'summary' | 'decisions' | 'full';
  /** The record kinds wanted, overriding `detail`. Core builds no other record. */
  kinds?: TraceKind[];
  /**
   * The ratio (0 to 1, default 1) of the traces this system starts that are
   * sampled, decided as OpenTelemetry's TraceIDRatioBased does. A
   * traceparent keeps its caller's decision.
   */
  sample?: number;
}

export interface TraceSinkOptions extends TraceConfiguration {
  /** Failures and <log> as OTLP log records, the sink's second argument (default true). */
  logs?: boolean;
  /** At most this many logs per definition and name per second (default 20). */
  logLimit?: number;
  /** Also trace macrosteps whose traceparent is not sampled. */
  unsampled?: boolean;
  /** Resource attributes every request carries, such as `service.name`. */
  resource?: Record<string, string>;
  /** Event data and assigned values as JSON attributes, cut at `valueLimit` (default 256, at most 1023). */
  values?: boolean;
  valueLimit?: number;
  /** "cause" (default): a macrostep's parent is its cause; "link": a root, linked to its cause. */
  parent?: 'cause' | 'link';
  /** Trace IDs the system starts become a fixed sequence (tests). */
  seed?: bigint;
}

export interface MetricPoint {
  attributes: Record<string, string>;
  value: number;
}

/** One metric: a cumulative monotonic sum, or a gauge. */
export interface Metric {
  name: string;
  unit: string;
  kind: 'sum' | 'gauge';
  points: MetricPoint[];
}

export interface MetricsSnapshot {
  /** Milliseconds since the epoch: when the system started counting, and now. */
  startTime: number;
  time: number;
  /** Core's totals, freed definitions included (core ADR 0072). */
  system: Record<string, number>;
  metrics: Metric[];
}

/** One OTLP ExportMetricsServiceRequest, to post to `/v1/metrics`. */
export declare function metricsToOTLP(
  snapshot: MetricsSnapshot,
  options?: { resource?: Record<string, string>; version?: string },
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
  /** Shared by the spans of one resource. */
  resource: { attributes: Record<string, unknown> };
  scope: { name: string; version: string };
}

/** Reads OTLP protobuf trace bytes; throws on a malformed message. */
export declare function decodeTraces(bytes: Uint8Array): DecodedSpan[];

export declare function parseTraceparent(header: string | undefined):
  { traceID: string; parentSpanID: string; flags: number; sampled: boolean } | undefined;
export declare function formatTraceparent(span: { traceID: string; spanID: string; sampled?: boolean; flags?: number }): string;
export declare function stepSpanID(session: bigint | number, macrostep: bigint | number, microstep?: number): string;
