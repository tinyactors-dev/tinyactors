/**
 * The package version. The addon is being rebuilt step by step (ROADMAP.md);
 * so far createSystem(), System.close(), System.closed, System.stats(),
 * System.define(), System.findDefinition(), Definition, statechart(),
 * literal(), TinyactorsError, spawn(), inspect(), destroy(), flush(),
 * post() without data, and the `done`, `fault` and `deadLetter` hooks are
 * implemented. Until the JavaScript data
 * model lands (step 11), charts compile against core's null data model.
 */
export const version: string;

/* --------------------------------------------------------------------------
 * Systems and scheduling
 * -------------------------------------------------------------------------- */

export interface SystemOptions {
  /** Realtime uses performance.now(); manual time advances only when requested. */
  clock?: "realtime" | "manual";
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
  io?: Readonly<Record<string, IoProcessor>>;
  invokers?: Readonly<Record<string, Invoker>>;
  log?: LogSink;
}

/** An actor finished (ADR 0012). The actor is still resident while the hook runs. */
export interface DoneRecord<Data = unknown> {
  readonly actor: Actor;
  /** The top-level final state it reached (core ADR 0056). */
  readonly finalState: string;
  /** Its completion data; undefined until the JavaScript data model (step 11). */
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
  readonly target: ActorId | ActorAddress;
  readonly event: EventName;
  /** `unloaded`: a name whose actor is unloaded; loading it is ROADMAP step 14. */
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
  /** Maximum due sends dispatched during one pump turn. */
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
  /** Optional best-effort limit for retained JavaScript data. */
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
  inspect<Data extends object>(actor: ActorId): ActorInspection<Data>;
  /** True while a resident actor has this ID. */
  exists(actor: ActorId): boolean;
  /** Terminate `actor`. Throws `not-found` once it is gone. */
  destroy(actor: ActorId): void;
  /** Put an actor that faulted back into scheduling (ADR 0008). */
  release(actor: ActorId): void;

  load<Data extends object>(
    image: SessionImage<Data>,
    options?: { readonly binding?: Partial<Data> | Data },
  ): Actor<Data>;

  reconstruct<Data extends object>(
    image: SessionImage<Data>,
    options?: { readonly binding?: Partial<Data> | Data },
  ): Actor<Data>;

  /** Release the address and durable claim of an actor that remains unloaded. */
  removeUnloaded(address: ActorAddress): void;

  registerIo(type: string, processor: IoProcessor): void;
  registerInvoker(type: string, invoker: Invoker): void;
  setLogSink(sink?: LogSink): void;

  /**
   * Queue a message and return; a later pump turn delivers it (ADR 0005).
   * Messages to one target are delivered in the order they were accepted.
   * Mail for an actor that is gone is a dead letter, not an error. An
   * address is resolved through the directory when the post is accepted; a
   * name with no resident actor is a dead letter too (ADR 0006). Throws
   * `queue-full` at maxQueuedMessages. Until ROADMAP step 11, `data` must be
   * undefined.
   */
  post(target: ActorTarget, event: EventName, data?: unknown, options?: PostOptions): void;

  /** Queue a message and resolve after its macrostep becomes stable or done. */
  send(
    target: ActorTarget,
    event: EventName,
    data?: unknown,
    options?: SendOptions,
  ): Promise<SendResult>;

  /** Resolve after all currently reachable work is quiescent. */
  flush(): Promise<void>;

  /** Available only with a manual clock. Time must not move backwards. */
  advanceTo(time: number): Promise<void>;
  advanceBy(milliseconds: number): Promise<void>;

  actors(): readonly ActorInspection[];
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
  readonly allocations: number;
  readonly frees: number;
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
}

export type EventName = string;
export type ActorTarget = Actor | ActorId | ActorAddress;

export interface ActorAddress {
  readonly family: string;
  readonly name: string;
}

declare const definitionIdBrand: unique symbol;
declare const invocationIdBrand: unique symbol;
declare const sendIdBrand: unique symbol;

/**
 * An actor's ID: its slot and generation in the actor table, two small
 * integers (ADR 0002). Equality is by value; among live actors the slot alone
 * is unique.
 */
export interface ActorId {
  readonly system: System;
  readonly slot: number;
  readonly generation: number;
}
export type DefinitionId = bigint & { readonly [definitionIdBrand]: true };
export type InvocationId = bigint & { readonly [invocationIdBrand]: true };
export type SendId = bigint & { readonly [sendIdBrand]: true };

export interface PostOptions {
  signal?: AbortSignal;
}

export interface SendOptions extends PostOptions {
  /** Reject if the message has not completed by this many milliseconds. */
  timeout?: number;
}

export interface SendResult {
  readonly actor: Actor;
  readonly status: "stable" | "done";
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
  readonly id: DefinitionId;
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
  readonly id: DefinitionId;
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
  /** A JavaScript function needs the JavaScript data model (step 11); until then, data-model source such as "In('s')". */
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
  cancel(sendId: StringSpec<Data>): Self;

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
    array: Expression<Data, readonly Item[]>,
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
  /** Omit both name and address for an anonymous actor. */
  readonly name?: string;
  readonly address?: ActorAddress;
  readonly binding?: Partial<Data>;
}

/**
 * The view spawn() returns (ADR 0002): an ActorId with methods that ask the
 * system. It holds no native resource. So far equals(), inspect(), post()
 * (without data), destroyed, destroy() and dispose are implemented; the other
 * methods arrive with later ROADMAP steps.
 */
export interface Actor<Data extends object = Record<string, unknown>> extends ActorId {
  readonly destroyed: boolean;
  equals(other: ActorId): boolean;

  post(event: EventName, data?: unknown, options?: PostOptions): void;
  send(event: EventName, data?: unknown, options?: SendOptions): Promise<SendResult>;
  inspect(): ActorInspection<Data>;

  /** A retained reference to the actor's live data-model root. */
  data(): Data;

  /** Capture Tinyactors-owned state and retained host references in memory. */
  capture(): SessionImage<Data>;

  /** Evict this named actor using an immediately preceding capture. */
  unload(image: SessionImage<Data>): void;

  /** Prepare a fresh unpublished actor under another definition revision. */
  prepareReplacement(
    definition: Definition<Data>,
    options?: ReplacementOptions<Data>,
  ): PreparedActor<Data>;

  /** Terminate this interpreter. Invoked children are not destroyed implicitly. */
  destroy(): void;
  [Symbol.dispose](): void;
}

export interface PreparedActor<Data extends object> extends Actor<Data> {
  readonly published: false;

  /** Atomically publish this actor at the incumbent's named address. */
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
 * Invocations, delayed sends, the current event and completion data are
 * added by later ROADMAP steps.
 */
export interface ActorInspection<Data = unknown> {
  readonly actor: Actor;
  /** Stable across unload and load; the runtime ID is not (core ADR 0066). */
  readonly sessionId: bigint;
  readonly definition: DefinitionIdentity;
  readonly residency: ActorResidency;
  readonly published: boolean;
  readonly activeStates: readonly string[];
  readonly macrostepInProgress: boolean;
  readonly internalEventCount: number;
  readonly delayedSendCount: number;
  /** The addon's scheduling state (ADR 0005). */
  readonly scheduling: "none" | "idle" | "ready" | "runnable" | "quarantined";
  /** The top-level final state it reached, or null (core ADR 0056). */
  readonly finalState: string | null;
  readonly allocatedBytes: number;
  /** @internal reserved for completion data (step 11). */
  readonly completion?: Data;
}

export interface InvocationInspection {
  readonly id: InvocationId;
  readonly type: string;
  readonly active: boolean;
  readonly autoforward: boolean;
}

/** One pending delayed send; `delayedSends` is ordered by due time, then send order. */
export interface DelayedSendInspection {
  readonly id: SendId;
  readonly due: number;
  readonly event: EventName;
  readonly ioType: string;
  /** The resolved receiver for the built-in SCXML processor; absent for custom processors, which use `target`. */
  readonly destination?: ActorId | ActorAddress;
  readonly target: unknown;
  readonly data: unknown;
  /** The router refused this send once; it is waiting to be offered again. */
  readonly submitted: boolean;
}

/* --------------------------------------------------------------------------
 * In-memory capture, unload, and reconstruction
 * -------------------------------------------------------------------------- */

export interface SessionImage<Data extends object = Record<string, unknown>> {
  readonly origin: System;
  readonly actor: ActorId;
  readonly definition: DefinitionIdentity;
  readonly address?: ActorAddress;
  readonly nextDeadline?: number;
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
  readonly sessionId: string;
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

export interface ActorEvent<Data = unknown> {
  readonly name: EventName;
  readonly type: "external" | "internal" | "platform";
  readonly sendId?: string;
  readonly origin?: EventOrigin;
  readonly invocationId?: string;
  readonly data: Data;
}

export type EventOrigin =
  | { readonly kind: "actor"; readonly actor: ActorId | ActorAddress }
  | { readonly kind: "invocation"; readonly invocation: InvocationId }
  | { readonly kind: "io"; readonly value: bigint };

/* --------------------------------------------------------------------------
 * Statechart specs: the generated shape (ADR 0013)
 *
 * System.define() accepts this shape as a plain object; statechart() emits
 * it; core's definition export returns it. Member names follow core's C
 * spelling, and a tagged union is { kind, <member>: ... }. Expressions are
 * data-model source text; until step 11 that is the null data model, which
 * accepts only In(state).
 * -------------------------------------------------------------------------- */

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
  readonly script?: string;
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
  readonly condition?: string;
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
  | { readonly kind: "expr"; readonly expr: string }
  | { readonly kind: "location"; readonly location: string }
  | { readonly kind: "content"; readonly content: { readonly media_type?: string; readonly bytes: Uint8Array } };

/** An `x` / `xexpr` attribute pair. */
export type StringAttributeSpec =
  | { readonly kind: "literal"; readonly literal: string }
  | { readonly kind: "expr"; readonly expr: string };

export type ActionSpec =
  | { readonly kind: "raise"; readonly raise: { readonly event: string; readonly data?: ValueSourceSpec } }
  | { readonly kind: "send"; readonly send: SendSpec }
  | { readonly kind: "cancel"; readonly cancel: { readonly sendid: StringAttributeSpec } }
  | { readonly kind: "assign"; readonly assign: { readonly location: string; readonly value?: ValueSourceSpec } }
  | { readonly kind: "script"; readonly script: { readonly script: string } }
  | { readonly kind: "log"; readonly log: { readonly label?: string; readonly expr?: string } }
  | {
    readonly kind: "if";
    readonly conditional: {
      readonly condition: string;
      readonly then_actions?: readonly ActionSpec[];
      readonly else_actions?: readonly ActionSpec[];
    };
  }
  | {
    readonly kind: "foreach";
    readonly foreach: {
      readonly array: string;
      readonly item: string;
      readonly index?: string;
      readonly body?: readonly ActionSpec[];
    };
  };

export interface SendSpec {
  readonly id?: { readonly kind: "literal"; readonly literal: string } | { readonly kind: "location"; readonly location: string };
  readonly type?: StringAttributeSpec;
  readonly event?: { readonly kind: "name"; readonly name: string } | { readonly kind: "expr"; readonly expr: string };
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
  readonly id?: { readonly kind: "literal"; readonly literal: string } | { readonly kind: "location"; readonly location: string };
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

export interface IoProcessor {
  send(request: IoRequest, context: EffectContext): void | Promise<void>;
}

export interface IoRequest {
  readonly source: ActorId | ActorAddress;
  readonly sendId: SendId;
  readonly event: EventName;
  readonly type: string;
  readonly target: unknown;
  readonly data: unknown;
}

export interface EffectContext {
  readonly system: System;
  readonly signal: AbortSignal;
  post(target: ActorTarget, event: EventName, data?: unknown): void;
}

export interface Invoker {
  start(request: InvokeRequest, reporter: InvocationReporter): InvokedService;
}

export interface InvokeRequest {
  readonly system: System;
  readonly parent: Actor;
  readonly invocationId: InvocationId;
  readonly type: string;
  readonly source?: unknown;
  readonly content?: unknown;
  readonly params?: unknown;
  readonly autoforward: boolean;
}

export interface InvokedService {
  cancel(reason?: unknown): void | Promise<void>;
  forward?(event: ActorEvent): void | Promise<void>;
}

export interface InvocationReporter {
  readonly signal: AbortSignal;
  event(name: EventName, data?: unknown): void;
  done(data?: unknown): void;
  unavailable(error?: unknown): void;
}

export interface LogRecord {
  readonly source: ActorId | ActorAddress;
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
