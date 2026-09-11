import * as native from "./lowlevel";
export { native as lowlevel };
export type ActorEvent<Data = any> = {
  type: string;
  data?: Data;
  trace?: native.TraceContext;
};
export type EventView<E extends ActorEvent = ActorEvent> = E & {
  origin?: string;
  originType?: string;
  sendId?: string;
  invokeId?: string;
  messageId: bigint;
};
export type Target = string | bigint | Actor<any, any> | native.Target;
export type Expression<
  S extends object,
  R,
  E extends ActorEvent = ActorEvent,
> = R | ((context: Context<S, E>) => R);
export type DataValue =
  | string
  | number
  | boolean
  | bigint
  | null
  | undefined
  | readonly unknown[]
  | Record<string, unknown>
  | Date
  | Map<unknown, unknown>
  | Set<unknown>
  | Uint8Array;
export type ValueExpression<
  S extends object,
  E extends ActorEvent = ActorEvent,
> = DataValue | ((context: Context<S, E>) => unknown);
/** Context getters and effects expire when the synchronous callback returns. */
export interface Context<S extends object, E extends ActorEvent = ActorEvent> {
  readonly state: S;
  readonly event: EventView<E> | undefined;
  readonly self: bigint;
  readonly now: bigint;
  readonly system: native.SystemValues;
  in(name: string): boolean;
  send(event: string | ActorEvent, options?: SendOptions): bigint;
  raise(event: string | ActorEvent, data?: unknown): void;
  cancel(id: string): void;
}
export interface SendOptions {
  to?: Target;
  target?: Target;
  processor?: string;
  data?: unknown;
  delayNs?: bigint;
  delayMs?: number;
  id?: string;
}
export interface SendActionOptions<
  S extends object = any,
  E extends ActorEvent = ActorEvent,
> {
  to?: Expression<S, Target, E>;
  target?: Expression<S, Target, E>;
  processor?: Expression<S, string, E>;
  data?: ValueExpression<S, E>;
  delayNs?: Expression<S, bigint, E>;
  delayMs?: Expression<S, number, E>;
  id?: string;
}
export type Action<S extends object, E extends ActorEvent = ActorEvent> =
  | ((context: Context<S, E>) => unknown)
  | ActionDescriptor<S, E>;
export interface ActionDescriptor<
  S extends object = any,
  E extends ActorEvent = ActorEvent,
> {
  readonly kind: "send" | "raise" | "cancel" | "assign" | "script" | "custom";
  readonly __stateType?: S;
  readonly __eventType?: E;
}
export type Actions<S extends object, E extends ActorEvent = ActorEvent> =
  | Action<S, E>
  | readonly Action<S, E>[];
export type Transition<S extends object, E extends ActorEvent = ActorEvent> =
  | string
  | Action<S, E>
  | {
      target?: string | readonly string[];
      guard?: (context: Context<S, E>) => boolean;
      internal?: boolean;
      actions?: Actions<S, E>;
    };
export interface Invoke<S extends object, E extends ActorEvent = ActorEvent> {
  id?: string;
  type?: Expression<S, string, E>;
  source?: Expression<S, string, E>;
  input?: ValueExpression<S, E>;
  data?: ValueExpression<S, E>;
  inlineDefinition?: native.DefinitionHandle;
  autoforward?: boolean;
  finalize?: Actions<S, E>;
}
/** Nested state keys get path-qualified IDs. Explicit id overrides are global. */
export interface State<S extends object, E extends ActorEvent = ActorEvent> {
  id?: string;
  type?: "state" | "parallel" | "final" | "history";
  parallel?: boolean;
  final?: boolean;
  history?: boolean | "shallow" | "deep";
  initial?: string | readonly string[];
  initialActions?: Actions<S, E>;
  states?: Record<string, State<S, E>>;
  on?: Record<string, Transition<S, E> | readonly Transition<S, E>[]>;
  always?: Transition<S, E> | readonly Transition<S, E>[];
  onDone?: Transition<S, E> | readonly Transition<S, E>[];
  entry?: Actions<S, E> | readonly Actions<S, E>[];
  exit?: Actions<S, E> | readonly Actions<S, E>[];
  data?: Record<string, ValueExpression<S, E>>;
  invoke?: Invoke<S, E> | readonly Invoke<S, E>[];
  doneData?: ValueExpression<S, E>;
}
export interface MachineConfig<
  S extends object,
  I = any,
  E extends ActorEvent = ActorEvent,
> {
  id: string;
  state?: (context: Context<S, E> & { readonly input: I }) => S;
  initial?: string | readonly string[];
  initialActions?: Actions<S, E>;
  states: Record<string, State<S, E>>;
  binding?: "early" | "late";
  data?: Record<string, ValueExpression<S, E>>;
  root?: (context: Context<S, E>) => unknown;
}
declare const machineType: unique symbol;
export interface Machine<
  S extends object = any,
  I = any,
  E extends ActorEvent = ActorEvent,
> {
  readonly id: string;
  readonly chart: native.Chart;
  readonly owner: object;
  readonly [machineType]?: { state: S; input: I; event: E };
}
export function defineMachine<
  S extends object = any,
  I = any,
  E extends ActorEvent = ActorEvent,
>(config: MachineConfig<S, I, E>): Machine<S, I, E>;
export function send<S extends object = any, E extends ActorEvent = ActorEvent>(
  event: Expression<S, string, E>,
  options?: SendActionOptions<S, E>,
): ActionDescriptor<S, E>;
export function raise(event: string): ActionDescriptor;
export function cancel<S extends object = any>(
  id: Expression<S, string>,
): ActionDescriptor<S>;
export function assign<
  S extends object = any,
  E extends ActorEvent = ActorEvent,
>(
  patch: Partial<S> | ((context: Context<S, E>) => Partial<S>),
): ActionDescriptor<S, E>;
export type ActorStatus =
  | "starting"
  | "ready"
  | "suspended"
  | "stopping"
  | "done"
  | "stopped"
  | "failed";
export interface Snapshot<S extends object = any> {
  id: bigint;
  status: ActorStatus;
  reason: number;
  revision: bigint;
  sessionId: string;
  name: string | null;
  state: S;
  stable: boolean;
  active?: string[];
  pending: {
    external: number;
    internal: number;
    timers: number;
    invocations: number;
  };
  memory: native.MemoryStats;
}
export type InspectionRecord =
  | { kind: "actor"; actor: bigint; info: Snapshot }
  | {
      kind: "active";
      actor: bigint;
      id: number;
      name: string;
      stateKind: number;
    }
  | {
      kind: "history";
      actor: bigint;
      id: number;
      recorded: boolean;
      target: number;
      ordinal: number;
    }
  | { kind: "event"; actor: bigint; event: EventView };
export interface InspectionOptions {
  actor?: Actor | bigint;
  revision?: bigint;
  stableOnly?: boolean;
  details?: number;
  workUnits?: number;
  cursor?: native.CursorHandle;
}
export interface InspectionResult {
  done: boolean;
  stopped: boolean;
  status: number;
  cursor: native.CursorHandle;
}
export interface RunOptions {
  nowNs?: bigint;
  workUnits?: number;
  actorQuantum?: number;
  budget?: native.RunBudget;
}
export interface RunResult {
  workUnits: number;
  microsteps: number;
  externalEvents: number;
  terminalActors: number;
  runnable: boolean;
  ioReady: boolean;
  nextDeadlineNs: bigint;
}
export interface SpawnOptions<I = any> {
  name?: string;
  input?: I;
  parameters?: native.Parameter[];
  limits?: Record<string, number>;
  trace?: native.TraceContext;
}
export interface UpgradeOptions<S extends object = any, N extends object = S> {
  expectedRevision?: bigint;
  migrate?: (input: {
    state: S;
    from: native.Description;
    to: native.Description;
    stateMap: native.MigrationState[];
  }) => N;
}
export class Actor<S extends object = any, E extends ActorEvent = ActorEvent> {
  constructor(system: System, id: bigint);
  readonly system: System;
  readonly id: bigint;
  send(event: E | E["type"], data?: E["data"]): bigint;
  stop(): void;
  release(): void;
  snapshot(options?: { active?: boolean }): Snapshot<S>;
  inspect(options?: { active?: boolean }): Snapshot<S>;
  upgrade<N extends object>(
    machine: Machine<N>,
    options?: UpgradeOptions<S, N>,
  ): void;
}
/** Lease completion and service returns are explicit; no transport runs in C. */
export class Lease {
  readonly system: System;
  readonly request: native.IORequest;
  readonly completed: boolean;
  complete(status?: number): void;
  emit(event: string | ActorEvent, data?: unknown): void;
  done(data?: unknown): void;
  fail(): void;
  isCancelled(): boolean;
}
export interface SystemOptions {
  sessionPrefix?: string;
  limits?: Record<string, number>;
  actorLimits?: Record<string, number>;
  ioprocessors?: native.IOProcessor[];
  services?: native.Service[];
  trace?: native.TraceHooks;
  snapshot?: (value: any) => any;
  diagnostic?: (record: native.Diagnostic) => void;
  terminal?: (record: native.Termination) => void;
  config?: native.SystemConfig;
}
export interface StartOptions {
  workUnits?: number;
  actorQuantum?: number;
  unref?: boolean;
  onIO?: (system: System) => void;
  onError?: (error: Error) => void;
}
export class System {
  constructor(options?: SystemOptions);
  readonly handle: native.SystemHandle;
  prepare(machine: Machine): native.DefinitionHandle;
  releaseDefinition(machine: Machine): void;
  spawn<S extends object, I, E extends ActorEvent>(
    machine: Machine<S, I, E>,
    options?: SpawnOptions<I>,
  ): Actor<S, E>;
  spawnPublished(key: string, options?: SpawnOptions): Actor;
  actor(nameOrId: string | bigint): Actor;
  bind(name: string, actor: Actor, expected?: Actor | bigint): void;
  unbind(name: string, expected?: Actor | bigint): void;
  send(name: string, event: string | ActorEvent, data?: unknown): bigint;
  run(options?: RunOptions): RunResult;
  nextDeadline(): bigint;
  stats(): native.SystemStats;
  takeErrors(): unknown[];
  inspect(
    visit: (
      record: InspectionRecord,
    ) => void | number | "pause" | "stop" | false,
    options?: InspectionOptions,
  ): InspectionResult;
  publish(
    machine: Machine,
    options?: string | { key?: string; expectedRevision?: bigint },
  ): void;
  unpublish(key: string, expectedRevision?: bigint): void;
  upgrade<S extends object, N extends object>(
    actor: Actor<S> | bigint,
    machine: Machine<N>,
    options?: UpgradeOptions<S, N>,
  ): void;
  ioNext(): Lease | null;
  drainIO(
    handler: (
      request: native.IORequest,
      lease: Lease,
    ) => void | number | Promise<void | number>,
    options?: { limit?: number; onError?: (error: unknown) => void },
  ): Promise<number>;
  receive(
    route: { processor: string; actor: Actor | bigint },
    event: ActorEvent & {
      origin?: string;
      originType?: string;
      sendId?: string;
    },
  ): void;
  start(options?: StartOptions): this;
  stop(): this;
  shutdown(): void;
  /** Graceful close refuses outstanding transport leases. force is explicit abandonment. */
  close(options?: { force?: boolean }): void;
}
export class TinyActorsError extends Error {
  readonly status: number;
  readonly result: unknown;
}
export function createSystem(options?: SystemOptions): System;
