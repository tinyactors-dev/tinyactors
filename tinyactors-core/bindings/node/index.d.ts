// Type definitions for tinyactors (Node.js binding).
// Charts are SCXML; the chart helpers are typed by the session's variables
// (`scxml.chart<Variables>()`), so c.expr functions see them, and actors'
// snapshots carry them as `context`.

/** What a c.expr function receives: the session's variables and SCXML's system variables. */
export type Scope<V> = V & {
  readonly _event: SCXMLEvent;
  readonly _sessionid: string;
  readonly _name: string;
  readonly _ioprocessors: Record<string, { location: string }>;
  In(stateID: string): boolean;
};

/** _event (SCXML §5.10.1). */
export interface SCXMLEvent {
  name: string;
  type: 'platform' | 'internal' | 'external';
  sendid?: string;
  origin?: string;
  origintype?: string;
  invokeid?: string;
  raw?: string;
  data?: any;
}

/** Code in a chart: written as the function's source, called with the scope by the native datamodel. */
export interface Expr<V, R = unknown> {
  readonly fn: (scope: Scope<V>) => R;
}
/** An expression: a c.expr function, or ECMAScript text; other values are written as JSON. */
export type Expression<V, R = unknown> = Expr<V, R> | string | number | boolean | null | readonly unknown[];

/** An SCXML element, made by a helper. */
export interface Element {
  readonly tag: string;
  readonly attrs: Record<string, string | undefined>;
  readonly children: readonly (Element | string)[];
}
type Attributes<V> = { [attribute: string]: Expression<V> | undefined };
type Child = Element | string | readonly Child[] | false | null | undefined;
type Rest<V> = readonly (Attributes<V> | Child)[];

/** The chart helpers: one per SCXML element, identifying attributes positionally. */
export interface ChartBuilder<V> {
  expr<R>(fn: (scope: Scope<V>) => R): Expr<V, R>;
  scxml(name: string, ...rest: Rest<V>): Chart<V>;
  state(id?: string, ...rest: Rest<V>): Element;
  parallel(id?: string, ...rest: Rest<V>): Element;
  final(id?: string, ...rest: Rest<V>): Element;
  history(id?: string, ...rest: Rest<V>): Element;
  initial(...rest: Rest<V>): Element;
  transition(event?: string | null, target?: string | null, ...rest: Rest<V>): Element;
  onentry(...rest: Rest<V>): Element;
  onexit(...rest: Rest<V>): Element;
  datamodel(data?: { [K in keyof V]?: Expression<V, V[K]> } | Element, ...rest: Rest<V>): Element;
  data(id: string, expr?: Expression<V>, ...rest: Rest<V>): Element;
  assign(location: string, expr?: Expression<V>, ...rest: Rest<V>): Element;
  donedata(...rest: Rest<V>): Element;
  content(textOrExpr?: string | Expr<V>, ...rest: Rest<V>): Element;
  param(name: string, expr?: Expression<V>, ...rest: Rest<V>): Element;
  script(code?: string | Expr<V>, ...rest: Rest<V>): Element;
  raise(event: string, ...rest: Rest<V>): Element;
  if(cond: Expression<V, unknown>, ...rest: Rest<V>): Element;
  elseif(cond: Expression<V, unknown>, ...rest: Rest<V>): Element;
  else(...rest: Rest<V>): Element;
  foreach(array: Expression<V>, item: string, ...rest: Rest<V>): Element;
  log(label?: string, expr?: Expression<V>, ...rest: Rest<V>): Element;
  send(event?: string, ...rest: Rest<V>): Element;
  cancel(sendid: string, ...rest: Rest<V>): Element;
  invoke(...rest: Rest<V>): Element;
  finalize(...rest: Rest<V>): Element;
}

export declare const scxml: { chart<V extends object = Record<string, any>>(): ChartBuilder<V> };

/** A chart: SCXML text, its c.expr functions, the documents its src attributes name. */
export declare class Chart<V = Record<string, any>> {
  constructor(xml: string, options?: { name?: string; expressions?: Map<string, Function>; documents?: Record<string, string> });
  readonly xml: string;
  readonly name: string | undefined;
  readonly expressions: Map<string, Function>;
  readonly documents: Record<string, string>;
  /** @internal phantom: the session's variables */
  readonly __variables?: V;
}
/** A chart of SCXML text, compiled as it is. */
export declare function fromSCXML<V = Record<string, any>>(xml: string, options?: { name?: string; documents?: Record<string, string> }): Chart<V>;

/** A compiled image (bytes of the tac compiler, or image.bytes()). */
export declare class Image {
  readonly hash: string;
  readonly charts: { name: string; source: string; datamodel: string }[];
  readonly actors: { name: string; chart: number }[];
  bytes(): Uint8Array;
}
export declare function loadImage(bytes: Uint8Array): Image;

/** An event: its name is `type`, its data the event itself. */
export interface EventObject {
  type: string;
  [key: string]: unknown;
}

export interface Snapshot<V> {
  /** An atomic state's id, { compound: value }, or { parallel: { region: value } }. */
  value: StateValue;
  context: V | undefined;
  status: 'active' | 'done' | 'error' | 'stopped';
  output: unknown;
  error: unknown;
}
export type StateValue = string | { [state: string]: StateValue };

export interface Observer<T> {
  next?: (value: T) => void;
  error?: (error: unknown) => void;
  complete?: () => void;
}
export interface Subscription {
  unsubscribe(): void;
}

/** A message a <send type> hands an I/O processor. */
export interface IOMessage {
  target: string;
  event: string;
  sendid: string;
  data: unknown;
  /** the sender's address */
  origin: string;
}
export interface IOProcessor {
  /** shown in _ioprocessors */
  location?: string;
  /**
   * TA_IO_PROCESSOR_SYNC: send() runs inside the run, right after the <send>,
   * before the sender goes on; events it sends (system.send, actor.send,
   * deliver) and a throw (error.communication, on the sender's internal
   * queue) come first. Other calls into the system throw there.
   */
  sync?: boolean;
  /** a throw or a rejection: error.communication for the sender */
  send(message: IOMessage, context: { system: System; signal: AbortSignal; traceparent?: string }): void | Promise<unknown>;
}
export interface Invocation {
  invokeid: string;
  type: string;
  src: string;
  params: unknown;
}
export interface InvocationContext {
  system: System;
  /** aborted when the invoking state exits, or the system stops */
  signal: AbortSignal;
  traceparent?: string;
  /** an event for the invoking session (_event.invokeid set) */
  send(event: string, data?: unknown): void;
  /** done.invoke.<invokeid> */
  done(data?: unknown): void;
  /** error.communication */
  fail(): void;
}
export interface Invoker {
  /** a throw or a rejection: error.communication */
  start(invocation: Invocation, context: InvocationContext): void | Promise<unknown>;
  /** sends to #_<invokeid>, and autoforwarded events */
  receive?(invokeid: string, event: { name: string; data: unknown }): void;
}

/** A blob store for hibernation (a Map will do). */
export interface BlobStore {
  get(key: string): Uint8Array | undefined | Promise<Uint8Array | undefined>;
  set(key: string, blob: Uint8Array): unknown;
  delete(key: string): unknown;
}

export type TraceKind = (typeof TRACE_KINDS)[number];
export type TraceDetail = 'summary' | 'decisions' | 'full' | 'off' | { kinds: TraceKind[]; values?: boolean };
export interface TraceRule {
  at?: 'start' | 'take' | ('start' | 'take')[];
  chart?: string | Chart<any>;
  actor?: string;
  event?: string;
  session?: string | number;
  from?: TraceFrom | TraceFrom[];
  sample?: boolean | number;
  detail?: TraceDetail;
  values?: boolean;
  perSecond?: number;
  burst?: number;
}
export type TraceFrom = 'host' | 'actor' | 'timer' | 'spawn' | 'sampled' | 'unsampled';
export interface TraceOptions {
  detail?: 'summary' | 'decisions' | 'full';
  kinds?: TraceKind[];
  values?: boolean;
  valueLimit?: number;
  sample?: number;
  parent?: 'cause' | 'link';
  logs?: boolean;
  resource?: Record<string, string>;
  rules?: TraceRule[];
}
export interface TelemetryOptions {
  endpoint?: string;
  tracesEndpoint?: string;
  logsEndpoint?: string;
  metricsEndpoint?: string;
  headers?: Record<string, string>;
  resource?: Record<string, string>;
  sample?: number;
  spanMetrics?: boolean;
  metricsInterval?: number;
  metrics?: () => Metric[];
  post?: (url: string, headers: Record<string, string>, body: Uint8Array) => Promise<unknown>;
  disabled?: boolean;
}

export interface SystemOptions {
  /** its id in addresses: tinyactors://<systemID>/... */
  systemID?: string;
  /** the "ecmascript" datamodel: JS values and c.expr functions, or the C datamodel (quickjs) */
  ecmascript?: 'native' | 'quickjs';
  /** a clock in ms: time moves only in flush() */
  clock?: () => number;
  ioProcessors?: Record<string, IOProcessor>;
  invokers?: Record<string, Invoker>;
  /** <log>, and exceptions in expressions ('error.execution') */
  logger?: (label: string, text: unknown) => void;
  hibernation?: { store: BlobStore };
  /** system.snapshot()'s bytes */
  restore?: Uint8Array;
  /** charts whose c.expr functions a restored system needs */
  charts?: Chart<any>[];
  capacities?: { maxActors?: number; maxEvents?: number; maxTimers?: number; stringBytes?: number };
  traces?: TraceOptions & { sink?: (traces: Uint8Array, logs: Uint8Array | undefined) => void; seed?: number | bigint };
  telemetry?: TelemetryOptions | false;
}

export interface Envelope {
  target: string;
  name: string;
  data?: unknown;
  origin?: string;
  origintype?: string;
  sendid?: string;
  msgid?: string;
  dedup?: string;
  hops?: number;
  traceparent?: string;
}

export interface SendOptions {
  sendid?: string;
  origin?: string;
  origintype?: string;
  raw?: string;
  traceparent?: string;
}

export interface Stats {
  actors: number;
  hibernated: number;
  runnable: number;
  jobs: number;
  regionBytes: number;
  delivered: number;
  routed: number;
  migrated: number;
  retired: number;
  sessions: number;
  payloads: number;
  heapBytes?: number;
  charts: {
    name: string;
    actors: number;
    hibernated: number;
    spawned: number;
    ended: number;
    turns: number;
    macrosteps: number;
    fuel: number;
    errorsExecution: number;
    errorsCommunication: number;
    errorsPlatform: number;
    faults: number;
  }[];
  segments: {
    segment: number;
    actors: number;
    current: number;
    actorsCapacity: number;
    events: number;
    eventsCapacity: number;
    timers: number;
    regionBytes: number;
    hibernated: number;
  }[];
}

export interface Metric {
  name: string;
  unit?: string;
  kind: 'sum' | 'gauge' | 'histogram';
  points: {
    attributes?: Record<string, string>;
    value?: number;
    count?: number;
    sum?: number;
    min?: number;
    max?: number;
    bounds?: number[];
    buckets?: number[];
    exemplars?: { bucket?: number; value: number; time: number; traceID: string; spanID: string }[];
  }[];
}
export interface MetricsSnapshot {
  startTime: number;
  time: number;
  metrics: Metric[];
}

/** One ta_host: its datamodel, its run loop, its actors. */
export declare class System {
  constructor(options?: SystemOptions);
  readonly id: string;
  readonly options: SystemOptions;
  /** Compile charts into one image and add it: their names get these versions. */
  load(charts: Chart<any> | Chart<any>[], options?: { documents?: Record<string, string> }): void;
  /** Boot a <system> manifest: its actors, by name. */
  boot(manifest: string, options?: { documents?: Record<string, string> }): Record<string, Actor<any>>;
  /** The actor spawned with `systemID`. */
  get(systemID: string): Actor<any> | undefined;
  /** Every actor, invoked ones included. */
  actors(): Actor<any>[];
  sendMany(items: [Actor<any>, EventObject | string, { traceparent?: string }?][]): number;
  /** An event for an address, with every field of ta_event_in; other systems' through the router. */
  send(address: string | Actor<any> | ActorRef, event: EventObject | string, options?: SendOptions): 'delivered' | 'relayed' | 'duplicate' | 'refused' | 'queued' | undefined;
  /** An envelope from host code to the router. */
  deliver(envelope: Envelope): 'delivered' | 'relayed' | 'duplicate' | 'refused' | 'queued';
  ref(address: string | Actor<any> | ActorRef): Actor<any> | ActorRef;
  /** Run what is due (with a `clock`: how time moves); true when idle. */
  flush(maxRounds?: number): boolean;
  /** Hibernate actors idle since the previous sweep; how many went to the store. */
  sweep(max?: number): number;
  /** Hibernate one actor now (throws, saying why, when it cannot be). */
  hibernate(actor: Actor<any>): boolean;
  export(actor: Actor<any>): Uint8Array;
  adopt(blob: Uint8Array): Actor<any>;
  migrate(target?: Actor<any> | Chart<any> | string, options?: { wake?: boolean; timeoutMs?: number }): {
    done: Promise<{ migrated: number; failed: number; pending: (Actor<any> | ActorRef)[] }>;
  };
  snapshot(): Uint8Array;
  stats(): Stats;
  metrics(): MetricsSnapshot;
  setTraceSink(sink: ((traces: Uint8Array, logs: Uint8Array | undefined) => void) | null, options?: TraceOptions): void;
  configureTraces(options?: { detail?: 'summary' | 'decisions' | 'full'; kinds?: TraceKind[]; sample?: number; rules?: TraceRule[] }): void;
  watchTraces(options: {
    actors?: (Actor<any> | number)[];
    sessions?: (string | number)[];
    names?: string[];
    charts?: string[];
    events?: string[];
    detail?: TraceDetail;
    values?: boolean;
    for?: number;
    perSecond?: number;
  }): number;
  unwatchTraces(id: number): void;
  flushTraces(): Promise<void>;
  stop(): void;
}
export declare function createSystem(options?: SystemOptions): System;

export interface ActorOptions {
  system: System;
  /** overrides <data> of the same ids */
  input?: Record<string, unknown>;
  /** its name: system.get(systemID), tinyactors://<system>/@<systemID> */
  systemID?: string;
  logger?: (label: string, text: unknown) => void;
  traceparent?: string;
}

export declare class Actor<V = Record<string, any>> {
  readonly system: System;
  readonly chart: Chart<V> | null;
  readonly options: ActorOptions | Record<string, unknown>;
  readonly sessionID: string | undefined;
  readonly address: string | undefined;
  readonly hibernated: boolean;
  start(): this;
  send(event: EventObject | string, options?: { traceparent?: string }): void;
  stop(): this;
  getSnapshot(): Snapshot<V>;
  subscribe(observer: Observer<Snapshot<V>> | ((snapshot: Snapshot<V>) => void), error?: (error: unknown) => void, complete?: () => void): Subscription;
  toJSON(): { $ref: string | undefined };
  toString(): string;
}
export declare function createActor<V>(chart: Chart<V>, options: ActorOptions): Actor<V>;
/** Settles when the actor is done (its output) or fails. */
export declare function toPromise(actor: Actor<any>): Promise<unknown>;

/** An actor of another system: send() and its address. */
export declare class ActorRef {
  readonly system: System;
  readonly address: string;
  send(event: EventObject | string, options?: SendOptions): unknown;
  toJSON(): { $ref: string };
  toString(): string;
}

export interface PeerStatus {
  id: string;
  state: 'connecting' | 'open' | 'reconnecting' | 'closed';
  queued: number;
  rtt: number;
  lag: number;
  sent: number;
  received: number;
  errors: number;
  duplicates: number;
}
/** A WebSocket-like transport, or a byte stream (Node.js streams). */
export type Transport = { send(text: string): void; close(): void; onopen: any; onmessage: any; onclose: any; onerror: any } | object;

export interface LinkOptions {
  url?: string;
  transport?: () => Transport;
  /** Node.js: TCP */
  tcp?: { host?: string; port: number };
  name?: string;
  trusted?: boolean;
  maxQueue?: number;
  queueTTLMs?: number;
  announce?: boolean | string[];
  pingMs?: number;
  credential?: unknown;
  maxFrame?: number;
}
export declare function createLink(system: System, options: LinkOptions): {
  readonly name: string;
  readonly opened: Promise<void>;
  get(systemID: string): Actor<any> | ActorRef;
  status(): (PeerStatus & { error: string | null; attempts: number }) | null;
  close(): void;
};

export interface ServeOptions {
  accept?: (onConnection: (transport: Transport) => void) => void | (() => unknown);
  /** Node.js: a WebSocket server */
  port?: number;
  server?: unknown;
  path?: string;
  /** Node.js: TCP */
  tcp?: { host?: string; port?: number };
  exports?: string[] | Record<string, string | string[]>;
  trusted?: boolean;
  trust?: (hello: unknown) => boolean;
  resumeMs?: number;
  maxQueue?: number;
  announce?: boolean | string[];
  maxFrame?: number;
}
export declare function serveLinks(system: System, options: ServeOptions): {
  readonly port?: number;
  connections(): string[];
  status(id: string): PeerStatus | null;
  close(): Promise<void>;
};

export declare function createStoreLink(system: System, options: {
  query(sql: string, params: unknown[]): Promise<any[]>;
  listen?: (channel: string, onPayload: (payload: string) => void) => unknown;
  id?: string;
  worker?: string;
  batch?: number;
  pollMs?: number;
  trusted?: boolean;
  maxQueue?: number;
  queueTTLMs?: number;
  announce?: boolean | string[];
}): {
  readonly id: string | null;
  readonly ready: Promise<void>;
  drain(): Promise<void>;
  flush(): Promise<void>;
  status(): (PeerStatus & { error: string | null; pending: number }) | null;
  close(): Promise<void>;
};

export type Address =
  | { system: string; local: string; kind: 'session'; session: bigint }
  | { system: string; local: string; kind: 'name'; name: string; encoded: string };
export declare function parseAddress(text: string): Address | null;
export declare function formatAddress(system: string, local: number | bigint | string): string;

export declare function encodeFrame(text: string): Uint8Array;
export declare class FrameDecoder {
  constructor(options?: { maxFrame?: number });
  readonly maxFrame: number;
  push(chunk: Uint8Array): (string | { reason: 'malformed'; length: number; detail: string })[];
}

export declare function parseTraceparent(header: string): { traceID: string; parentSpanID: string; flags: number; sampled: boolean } | undefined;
export declare function formatTraceparent(context: { traceID: string; spanID: string; sampled?: boolean; flags?: number }): string;
export declare function stepSpanID(session: string | number | bigint, macrostep: string | number | bigint, microstep?: number): string;

export interface Span {
  traceID: string;
  spanID: string;
  parentSpanID?: string;
  name: string;
  kind: 'unspecified' | 'internal' | 'server' | 'client' | 'producer' | 'consumer';
  startTime: number;
  endTime: number;
  startTimeUnixNano: bigint;
  endTimeUnixNano: bigint;
  attributes: Record<string, unknown>;
  events: { name: string; time: number; timeUnixNano: bigint; attributes: Record<string, unknown> }[];
  links: { traceID: string; spanID: string; attributes: Record<string, unknown> }[];
  status: { code: 'unset' | 'ok' | 'error'; message?: string };
  resource: { attributes: Record<string, unknown> };
  scope: { name: string; version: string; attributes: Record<string, unknown> };
}
export declare function decodeTraces(bytes: Uint8Array): Span[];
export declare function metricsToOTLP(snapshot: MetricsSnapshot, options?: { resource?: Record<string, string> }): Uint8Array;

export declare const TRACE_KINDS: readonly [
  'dropped', 'spawned', 'finished', 'destroyed', 'unloaded', 'loaded', 'macrostepBegin', 'macrostepEnd',
  'microstep', 'condition', 'raised', 'sent', 'invoked', 'cancelled', 'assign', 'log', 'actionFailed',
];
