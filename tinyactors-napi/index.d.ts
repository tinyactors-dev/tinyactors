/**
 * The package version. The addon is being rebuilt step by step (ROADMAP.md);
 * so far createSystem(), System.close(), System.closed, System.stats() and
 * TinyactorsError are implemented.
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
  io?: Readonly<Record<string, IoProcessor>>;
  invokers?: Readonly<Record<string, Invoker>>;
  log?: LogSink;
}

export interface SchedulingOptions {
  /** Maximum interpreter operations given to one actor before it yields. */
  resumeBudget?: number;
  /** Maximum due sends dispatched during one pump turn. */
  dueBudget?: number;
  /** Maximum envelopes started during one pump turn. */
  messageBudget?: number;
  /** Maximum queued external envelopes. New submissions fail when full. */
  maxQueuedMessages?: number;
  /** Optional best-effort limit for retained JavaScript data. */
  maxRetainedBytes?: number;
}

export declare function createSystem(options?: SystemOptions): System;

export interface System {
  readonly closed: boolean;
  readonly clock: "realtime" | "manual";
  readonly time: number;

  define<Data extends object = Record<string, unknown>>(
    source: DefinitionSpec<Data> | DefinitionBuilder<Data>,
  ): Definition<Data>;

  findDefinition(identity: DefinitionIdentity): Definition | undefined;

  spawn<Data extends object>(
    definition: Definition<Data>,
    options?: SpawnOptions<Data>,
  ): Actor<Data>;

  findActor(address: ActorAddress): Actor | undefined;

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

  /** Queue a message and return immediately after the binding owns its data. */
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
}

export type EventName = string;
export type ActorTarget = Actor | ActorId | ActorAddress;

export interface ActorAddress {
  readonly family: string;
  readonly name: string;
}

declare const actorIdBrand: unique symbol;
declare const definitionIdBrand: unique symbol;
declare const invocationIdBrand: unique symbol;
declare const sendIdBrand: unique symbol;

export type ActorId = bigint & { readonly [actorIdBrand]: true };
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

export interface DefinitionIdentity {
  readonly family: string;
  readonly revision: string | Uint8Array;
}

export interface DefinitionSpec<Data extends object> extends DefinitionIdentity {
  /** The statechart document name exposed through the data-model context. */
  readonly name?: string;
  readonly binding?: "early" | "late";
  readonly initial?: InitialTransition<Data>;
  readonly data?: readonly DataDeclaration<Data>[];
  /** Document order is semantically significant. */
  readonly states: readonly StateSpec<Data>[];
}

export interface Definition<Data extends object = Record<string, unknown>> {
  readonly system: System;
  readonly id: DefinitionId;
  readonly identity: DefinitionIdentity;
  readonly retired: boolean;

  inspect(): DefinitionInspection;

  /** Prevent new actors from using this definition. Existing actors continue. */
  retire(): void;
}

export interface DefinitionInspection {
  readonly id: DefinitionId;
  readonly identity: DefinitionIdentity;
  readonly name?: string;
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
  build(): DefinitionSpec<Data>;
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
  when(condition: Condition<Data>): this;
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
  log(label?: string, value?: Expression<Data>): Self;

  choose(
    condition: Condition<Data>,
    then: ActionBlock<Data>,
    otherwise?: ActionBlock<Data>,
  ): Self;

  forEach<Item>(
    array: Expression<Data, readonly Item[]>,
    item: keyof Data & string,
    build: ActionBlock<Data>,
    index?: keyof Data & string,
  ): Self;

  invoke(build: InvokeBlock<Data>): Self;
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

export interface Actor<Data extends object = Record<string, unknown>> {
  readonly system: System;
  readonly id: ActorId;
  readonly definition: Definition<Data>;
  readonly address?: ActorAddress;
  readonly destroyed: boolean;

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

export type ActorResidency = "resident" | "unloaded";

export interface ActorInspection<Data = unknown> {
  readonly actor: ActorId;
  readonly address?: ActorAddress;
  readonly definition: DefinitionIdentity;
  readonly residency: ActorResidency;
  readonly published: boolean;
  readonly activeStates: readonly string[];
  readonly macrostepInProgress: boolean;
  readonly currentEvent?: ActorEvent;
  readonly internalEventCount: number;
  readonly invocations: readonly InvocationInspection[];
  readonly delayedSends: readonly DelayedSendInspection[];
  readonly completion?: Data;
  readonly allocatedBytes: number;
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
 * Statechart authoring model
 * -------------------------------------------------------------------------- */

export type StateKind =
  | "atomic"
  | "compound"
  | "parallel"
  | "final"
  | "history-shallow"
  | "history-deep";

export interface StateSpec<Data extends object> {
  readonly id: string;
  readonly kind?: StateKind;
  readonly states?: readonly StateSpec<Data>[];
  readonly initial?: InitialTransition<Data>;
  readonly transitions?: readonly TransitionSpec<Data>[];
  /** Each nested array is one separately-failing executable-content block. */
  readonly onEntry?: readonly (readonly Action<Data>[])[];
  readonly onExit?: readonly (readonly Action<Data>[])[];
  readonly data?: readonly DataDeclaration<Data>[];
  readonly invokes?: readonly InvokeSpec<Data>[];
  readonly doneData?: DoneDataSpec<Data>;
}

export interface InitialTransition<Data extends object> {
  readonly target: string | readonly string[];
  readonly actions?: readonly Action<Data>[];
}

export interface TransitionSpec<Data extends object> {
  /** Omit for an eventless transition. Wildcard segments are supported. */
  readonly event?: EventName;
  readonly kind?: "external" | "internal";
  readonly guard?: Condition<Data>;
  readonly target?: string | readonly string[];
  readonly actions?: readonly Action<Data>[];
}

export interface DataDeclaration<Data extends object> {
  readonly name: keyof Data & string;
  readonly initial?: ValueSpec<Data>;
}

export type Action<Data extends object> =
  | RaiseAction<Data>
  | SendAction<Data>
  | CancelAction<Data>
  | AssignAction<Data>
  | ScriptAction<Data>
  | LogAction<Data>
  | ChooseAction<Data>
  | ForEachAction<Data>
  | InvokeAction<Data>;

export interface RaiseAction<Data extends object> {
  readonly type: "raise";
  readonly event: EventName;
  readonly data?: ValueSpec<Data>;
}

export interface SendAction<Data extends object> {
  readonly type: "send";
  readonly event: StringSpec<Data>;
  readonly target?: StringSpec<Data>;
  readonly io?: StringSpec<Data>;
  readonly delay?: DelaySpec<Data>;
  readonly id?: string;
  readonly idLocation?: Location<Data, string>;
  readonly data?: ValueSpec<Data>;
  readonly params?: readonly ParameterSpec<Data>[];
}

export interface CancelAction<Data extends object> {
  readonly type: "cancel";
  readonly sendId: StringSpec<Data>;
}

export interface AssignAction<Data extends object> {
  readonly type: "assign";
  readonly location: Location<Data>;
  readonly value: ValueSpec<Data>;
}

export interface ScriptAction<Data extends object> {
  readonly type: "script";
  readonly run: Script<Data>;
}

export interface LogAction<Data extends object> {
  readonly type: "log";
  readonly label?: string;
  readonly value?: Expression<Data>;
}

export interface ChooseAction<Data extends object> {
  readonly type: "if";
  readonly condition: Condition<Data>;
  readonly then: readonly Action<Data>[];
  readonly else?: readonly Action<Data>[];
}

export interface ForEachAction<Data extends object> {
  readonly type: "foreach";
  readonly array: Expression<Data, readonly unknown[]>;
  readonly item: keyof Data & string;
  readonly index?: keyof Data & string;
  readonly actions: readonly Action<Data>[];
}

export interface InvokeAction<Data extends object> {
  readonly type: "invoke";
  readonly invoke: InvokeSpec<Data>;
}

export interface ParameterSpec<Data extends object> {
  readonly name: string;
  readonly value: ValueSpec<Data>;
}

export interface InvokeSpec<Data extends object> {
  readonly id?: string;
  readonly idLocation?: Location<Data, string>;
  readonly type?: StringSpec<Data>;
  readonly source?: StringSpec<Data>;
  readonly content?: ValueSpec<Data>;
  readonly params?: readonly ParameterSpec<Data>[];
  readonly autoforward?: boolean;
  readonly finalize?: readonly Action<Data>[];
}

export type DoneDataSpec<Data extends object> =
  | { readonly content: ValueSpec<Data> }
  | { readonly params: readonly ParameterSpec<Data>[] };

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
