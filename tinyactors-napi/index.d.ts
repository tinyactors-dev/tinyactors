/**
 * @file The desired public API of tinyactors: the design target, not a
 * description of what is built.
 *
 * Parts of this file are knowingly not implemented yet. Each declaration
 * that lib/ does not implement carries a marker on the line directly above
 * it (after its JSDoc, if it has one):
 *
 *     // status: unimplemented
 *     // status: partial: <what is missing>
 *
 * Do not delete a declaration because lib/ lacks it: implement it, or
 * leave it marked. `mise run check-api-status` (part of `mise run test`)
 * keeps the markers honest where the runtime can answer: a marked
 * declaration that exists fails, and an unmarked one that is missing
 * fails. `grep -n "^ *// status:" index.d.ts` lists what is outstanding.
 */

/** The package version. */
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

/**
 * A core call for an actor failed; the actor is quarantined (ADR 0008). An
 * invoked child's parent also gets error.communication.invoke.<id>.quarantined
 * with `{ code, message }` from this record (napi ADR 0025).
 */
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
  /** The message's data, absent when it had none: post it again with it. */
  readonly data?: unknown;
  /** The message's traceparent, absent when it was not traced. */
  readonly traceparent?: string;
  /**
   * `unloaded`: a name whose actor is unloaded and whose family has no
   * loader (setLoader), or mail left for an actor unloaded through the raw
   * layer. Mail that waited for a loader (napi ADR 0020) ends as `rejected`
   * (the loader said "not now"), `load-failed`, `load-timeout`, or
   * `waiting-full` (the address already had maxWaiting messages waiting).
   */
  readonly reason:
    | "not-found"
    | "done"
    | "destroyed"
    | "refused"
    | "unloaded"
    | "rejected"
    | "load-failed"
    | "load-timeout"
    | "waiting-full";
  /** Why the load failed: the rejection's reason, the loader's error, or the timeout. */
  readonly detail?: string;
}

/** Why a loader is called (setLoader). */
export type LoadReason = "mail" | "timer";

/**
 * What a loader returns (setLoader, napi ADR 0020). Session images are
 * process-local: after a process exit, rebuild an actor from the data the
 * application stored ({ spawn, binding }), or replay a log.
 */
export type LoadResult<Data extends object = object> =
  | SessionImage<Data>
  | { readonly image: SessionImage<Data>; readonly restore?: (actor: ActorID) => unknown }
  | "offloaded"
  | {
      readonly spawn: Definition<Data>;
      readonly binding?: Partial<Data> | Data;
      readonly pendingSends?: readonly DelayedSendInspection[];
    }
  | "not-found"
  | "reject"
  | { readonly reject: unknown };

export interface LoaderOptions {
  /** Milliseconds the loader may take; after that its mail is `load-timeout` (default 5000). */
  readonly timeout?: number;
  /** Messages that may wait for one address; the rest are `waiting-full` (default 1000). */
  readonly maxWaiting?: number;
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
   * Microsteps one macrostep may run before its actor is quarantined with
   * fault code `runaway` (core ADR 0075); 0 for no limit, default 100000. A
   * tenth of it writes a `tinyactors.macrostep.long` log. `release()` lets a
   * runaway finish its macrostep; the next one has the limit again.
   */
  microstepLimit?: number;
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
   * `detail` (ChartRefusal). `options.microstepLimit` overrides the system's
   * `scheduling.microstepLimit` for this definition; 0 for no limit.
   */
  define<Data extends object = Record<string, unknown>>(
    source: DefinitionSpec | DefinitionBuilder<Data>,
    options?: { microstepLimit?: number },
  ): Definition<Data>;
  /**
   * Import an SCXML document (napi ADR 0019): its `datamodel` attribute picks
   * `null`, `javascript`, or a data model registerDatamodel() registered; a
   * document without one gets `null`. A refused document throws `invalid`
   * or `unsupported` with ImportRefusal as `detail`, which says where.
   */
  define<Data extends object = Record<string, unknown>>(
    source: string | Uint8Array,
    options: {
      format: "scxml";
      /**
       * Fetches `<data src>` and `<script src>` while the document is
       * defined: the content as a string or a Uint8Array, or undefined for a
       * src it does not know, which refuses the document there. It also
       * fetches a literal `<invoke type="scxml" src>`, whose child chart is
       * compiled with the parent (napi ADR 0024). The definition keeps it
       * for children compiled at invoke time from `srcexpr`, and its
       * children inherit it.
       */
      resolve?: (src: string) => string | Uint8Array | undefined;
      /**
       * What the document is called, such as its path or a file:// URL: the
       * source its spans name (core ADR 0086), which sourceOf() answers and
       * a debugger opens (napi ADR 0027). Without it the spans name no
       * source, and a debug session shows the chart as SCXML it writes from
       * the definition, by `sourceReference`. A child fetched by `src` or `srcexpr` is named by
       * that src; a child inline in `<content>` has no name, since its lines
       * count from the start of its content.
       */
      name?: string;
      microstepLimit?: number;
    },
  ): Definition<Data>;

  /**
   * Register a data model for imported documents (ADR 0019): `compile`
   * turns each source expression into what a builder chart would pass, and
   * the built-in data model runs it. `javascript` and `null` are reserved.
   */
  registerDatamodel(name: string, datamodel: DatamodelCompiler): void;

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
   * Hold `actor` between macrosteps (ADR 0027): its mail waits in its
   * mailbox until unhold() or step(). An actor inside a macrostep runs it to
   * its end first, so a hold never stops a macrostep a send() waits for: a
   * send() with a timeout on a held actor is withdrawn when it times out,
   * never taken later. Returns true when it is held now, false when it will
   * be once its macrostep ends. A fault while held still quarantines it.
   * With `anywhere`, it is held at its next pause inside a macrostep too
   * (setBudgets() makes it pause): only for an actor no caller waits on,
   * such as a debugger's copy in a system of its own. Throws `done` for a
   * finished actor, `not-found` for one that is gone.
   */
  hold(actor: ActorID, options?: { readonly anywhere?: boolean }): boolean;
  /** Let a held actor take its mail again (ADR 0027). */
  unhold(actor: ActorID): void;
  /**
   * A held actor takes exactly one message and is held again when that
   * message's macrostep ends (ADR 0027): "message". One held inside a
   * macrostep (hold anywhere), or woken by core while held, runs on in that
   * macrostep up to its next pause instead: "macrostep". With an empty
   * mailbox it takes the next message that comes: "waiting". Throws
   * `invalid` when the actor is not held.
   */
  step(actor: ActorID): "message" | "macrostep" | "waiting";
  /**
   * Budgets for one actor (ADR 0027): it resumes with these instead of the
   * system's `resumeBudget` and `actionBudget` (0: no limit); undefined
   * gives it the system's again. With `microstepStart`, every microstep a
   * resume begins pauses at its start, after selection and before its first
   * unit (core ADR 0087); inspection then reports that first unit.
   */
  setBudgets(actor: ActorID, budgets?: {
    readonly resumeBudget: number;
    readonly actionBudget?: number;
    readonly microstepStart?: boolean;
  }): void;
  /** The messages waiting in `actor`'s mailbox, oldest first, at most `limit` (default 100). */
  mailbox(actor: ActorID, options?: { readonly limit?: number }): readonly QueuedMessage[];
  /**
   * Abandon `actor`'s macrostep (core ADR 0082): the actor keeps the
   * configuration after its last completed microstep, and the send() that
   * follows the macrostep rejects with `abandoned`. `busy` while it is
   * paused inside a microstep (setBudgets() with resumeBudget 1 and
   * actionBudget 0 finishes that microstep first) or before its bootstrap
   * finished; `invalid` with no macrostep in progress.
   */
  abandon(actor: ActorID): void;
  /**
   * Withdraw `actor`'s pending delayed sends with this ID (core ADR 0084),
   * the `id` DelayedSendInspection shows. `not-found` when it has none;
   * `done` when the only ones were refused once and belong to their
   * receiver now.
   */
  withdrawSend(actor: ActorID, sendID: string): void;

  /**
   * Capture `actor` in memory (core ADR 0013), with the data-model root it
   * has now. `busy` for an unpublished (prepared) actor.
   */
  capture<Data extends object>(actor: ActorID): SessionImage<Data>;

  /**
   * Offload a stable named actor from memory (core ADR 0074): its data,
   * history and pending delayed sends go to bytes this system keeps, and
   * mail to its address or its own next delayed send loads it back (the
   * family's loader decides when it has one; see setLoader). Throws `busy`
   * while it has mail waiting, is inside a macrostep or is quarantined. The
   * bytes are for this process only: to survive a restart, store its data
   * and inspect().delayedSends, and spawn it again with
   * { binding, pendingSends }.
   */
  unload(actor: ActorID): void;

  /**
   * Bring an unloaded named actor back: from the bytes unload() kept for an
   * address, or from a SessionImage; the same session ID, a new runtime ID
   * (core ADR 0066). From an image, `binding` defaults to its root. An
   * invocation active at capture is reported to the actor as
   * `error.communication.invoke.<id>.unavailable` (core ADR 0038). An image
   * captured in another system loads as a new session of this one (core
   * ADR 0085).
   */
  load<Data extends object>(
    target: SessionImage<Data> | ActorAddress,
    options?: { readonly binding?: Partial<Data> | Data },
  ): Actor<Data>;

  /**
   * Restore an image under a fresh runtime ID without touching the directory
   * (core ADR 0052). `binding` defaults to the root the image kept, which is
   * shared with the captured actor if it still runs; pass one, such as a
   * structuredClone of `image.data`, for an independent copy.
   *
   * The image may come from another system (core ADR 0085), as a debugger's
   * fork does: the copy is then a new session of this system, its delayed
   * sends fire on this system's clock and go through this system's I/O
   * processors, and a send to another actor of the image's system fails as
   * one to a destroyed actor does. The values the image holds are copied
   * into this system's boxes (ADR 0026), so either system may close first.
   */
  reconstruct<Data extends object>(
    image: SessionImage<Data>,
    options?: { readonly binding?: Partial<Data> | Data },
  ): Actor<Data>;

  /** Release the address of an actor that stays unloaded; load() of its image then fails with `stale`. */
  removeUnloaded(address: ActorAddress): void;

  /**
   * Bring actors of `family` back when mail arrives for them (napi ADR
   * 0020). Mail for a name in `family` with no resident actor (unloaded, or
   * never seen here: the family is virtual) waits while the loader runs,
   * and is delivered first, in order, once it has loaded or spawned the
   * actor. One load runs per address. `why` is "timer" when an unloaded
   * actor's own delayed send is due. `null` removes the loader.
   */
  setLoader(
    family: string,
    loader:
      | ((
          address: ActorAddress,
          why: LoadReason,
          load: { readonly offloaded: boolean },
        ) => LoadResult | Promise<LoadResult>)
      | null,
    options?: LoaderOptions,
  ): void;

  /**
   * Wake the unloaded actor at `address` through its family's loader at
   * `time` (core's clock). unload() does this for the image's next deadline.
   */
  wakeAt(address: ActorAddress, time: number): void;

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
  /**
   * Every definition the system holds, as inspections, one page of IDs at
   * a time (core ADR 0078): builder and imported charts, their nested
   * inline children, and children compiled when an <invoke> ran. Retired
   * definitions still in use are included. A definition freed after its
   * page was taken is skipped.
   */
  definitions(options?: PageOptions): IterableIterator<DefinitionInspection>;

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
  /** Items per page; default 1024, at most 65536. */
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
  /**
   * An operator transition (core ADR 0082, ADR 0027): the message is taken
   * by a transition to `targets`, states of the target actor's chart, from
   * the active state `source` or the root, instead of by the chart's
   * transitions, with every exit, entry, history and invocation a chart
   * transition would have. It queues behind the mail before it. When it
   * arrives, an actor whose `source` is no longer active, or that runs
   * another revision by then, refuses it: a dead letter with reason
   * `refused` (a send() rejects with `invalid`), and the actor is not
   * quarantined. Only for an ActorID target.
   */
  transition?: { readonly targets: readonly string[]; readonly source?: string };
}

/** A message waiting in a mailbox (System.mailbox). */
export interface QueuedMessage {
  readonly event: EventName;
  readonly data?: unknown;
  /** A send() waits for it. */
  readonly awaited: boolean;
  /** Its origin's scope, when it was posted with `origin: { type, scope }`. */
  readonly scope?: { readonly index: number; readonly generation: number };
  /** The operator transition it carries, by state names; `source` null is the root. */
  readonly transition?: { readonly source: string | null; readonly targets: readonly string[] };
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

/** How the chart uses a source expression (core ADR 0076). */
export type ExpressionRole = "value" | "condition" | "location" | "script";

/**
 * A data model for imported documents (napi ADR 0019). For "value",
 * "condition" and "script", compile returns a function called with `this` =
 * the data and one argument, the context; for "location", a data name or
 * `{ get(context), set(value, context) }`. Throwing refuses the chart; an
 * error with numeric `offset` and `length` points into `source`.
 */
export interface DatamodelCompiler {
  compile(
    source: string,
    role: ExpressionRole,
  ): ((this: any, context: any) => unknown) | string | { get(this: any, context: any): unknown; set(this: any, value: unknown, context: any): void };
}

/** Why define(xml, { format: "scxml" }) refused a document, as `detail`. */
export interface ImportRefusal {
  /** "decode": the document is not SCXML the codec reads; "compile": core refused the chart. */
  readonly stage: "decode" | "compile";
  readonly message: string;
  /** Where in the document, 1-based, when the refusal names a place. */
  readonly line?: number;
  readonly column?: number;
  /** The byte offset in the UTF-8 document. */
  readonly offset?: number;
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

  /**
   * Where element `id` of `kind` was written (core ADR 0086), or undefined
   * when the definition keeps no span for it. statechart() records the file,
   * line and column of the call that wrote each state, transition, initial
   * transition and action.
   */
  sourceOf(kind: ElementKind, id: number): SourcePlace | undefined;

  /** Prevent new actors from using this definition. Existing actors continue. */
  retire(): void;
}

/** The kinds of compiled element core numbers, as trace records name them. */
export type ElementKind = "state" | "transition" | "action" | "expression";

/**
 * A place in a source. Lines and columns count from 1; a column is the
 * engine's (UTF-16 code units in JavaScript). The end and the byte range are
 * present when the front end knew them, as a document codec does.
 */
export interface SourcePlace {
  /** The source's name: a file, a URL, or "" when the spec named none. */
  readonly source: string;
  readonly line: number;
  readonly column: number;
  readonly endLine?: number;
  readonly endColumn?: number;
  readonly offset?: number;
  readonly length?: number;
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
  /** Holds taken by define() and not given back (core ADR 0077). */
  readonly holds: number;
  /** How many definitions nest this one as an inline child. */
  readonly nestedBy: number;
  /** The children this chart's <invoke>s nest, when it has any (core ADR 0078). */
  readonly nested?: readonly DefinitionID[];
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
  /**
   * Delayed sends to re-arm before the actor bootstraps (core ADR 0074
   * section 8): entries of inspect().delayedSends from an earlier session
   * of this actor, stored with its data. They keep their IDs, due times and
   * traces; a due time already past fires on the next dispatch.
   */
  readonly pendingSends?: readonly DelayedSendInspection[];
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

  /** Offload this stable named actor from memory (System.unload, core ADR 0074). */
  unload(): void;

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
 */
export interface ActorInspection<Data = unknown> {
  readonly actor: Actor;
  /** Stable across unload and load; the runtime ID is not (core ADR 0066). */
  readonly sessionID: bigint;
  readonly definition: DefinitionIdentity;
  /** The definition's ID (Definition.id), which names its compiled states, transitions and actions. */
  readonly definitionID: DefinitionID;
  readonly residency: ActorResidency;
  readonly published: boolean;
  readonly activeStates: readonly string[];
  readonly macrostepInProgress: boolean;
  /** Paused inside a microstep, in executable content (core ADR 0061). */
  readonly microstepInProgress: boolean;
  /**
   * The step key of where a paused macrostep stands (core ADR 0083);
   * (session, 0, 0) outside a macrostep and before one begins.
   */
  readonly step: { readonly session: bigint; readonly macrostep: number; readonly microstep: number };
  /**
   * The unit a paused macrostep runs next (core ADR 0083): `at` is "none"
   * between microsteps and outside a macrostep; otherwise the phase, the
   * state, and the transition and action by compiled ID (core ADR 0071).
   */
  readonly position: TracePosition;
  /** The external event of the macrostep in progress, or null. */
  readonly currentEvent: InspectedEvent | null;
  /** The internal queue, oldest first. */
  readonly internalEvents: readonly InspectedEvent[];
  readonly internalEventCount: number;
  /** Active and finished invocations of active states. */
  readonly invocations: readonly InvocationInspection[];
  readonly delayedSendCount: number;
  /** Pending delayed sends, by due time then send order (core ADR 0054). */
  readonly delayedSends: readonly DelayedSendInspection[];
  /** The addon's scheduling state (ADR 0005); "held" by System.hold() (ADR 0027). */
  readonly scheduling: "none" | "idle" | "ready" | "runnable" | "quarantined" | "held";
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
  /** The invoker's handle for the service; for the in-system SCXML invoker, the child's session ID. */
  readonly serviceHandle: bigint;
}

export interface TracePosition {
  readonly at: "none" | "select" | "exit" | "transition" | "enter" | "bootstrap" | "finalize" | "invoke";
  readonly state: string | null;
  readonly transition: number | null;
  readonly action: number | null;
  readonly iteration: number;
}

/** An event as inspection reports it. */
export interface InspectedEvent {
  readonly name: EventName;
  readonly type: "external" | "internal" | "platform";
  readonly sendID?: string;
  /** Who sent it: an actor, or an I/O origin's URI. */
  readonly origin?: ActorID | ActorAddress | string;
  readonly data?: unknown;
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
  /** A literal target's content ID in the definition, or null (core ADR 0074 section 8). */
  readonly targetContent: number | null;
  /** A literal <content>'s ID in the definition, or null. */
  readonly content: number | null;
  /** The sending step's trace, or null. */
  readonly traceparent: string | null;
}

/* --------------------------------------------------------------------------
 * In-memory capture, unload, and reconstruction
 * -------------------------------------------------------------------------- */

/**
 * A capture held in memory (ROADMAP step 14). Its handle is destroyed by
 * destroy(), by its finalizer, or by close() of its system, whichever comes
 * first; it never outlives its system (ADR 0012). Any system in this thread
 * may load or reconstruct it while it lives (core ADR 0085). Serializing it
 * across processes is not supported: model network handoff with I/O
 * processors.
 */
export interface SessionImage<Data extends object = Record<string, unknown>> {
  /** The system that captured it, and whose close() destroys it. */
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
  /** Where the script was written: an index into `spans`, from 1. */
  readonly script_span?: number;
  /**
   * Where elements were written (core ADR 0086): each element's `span` is an
   * index into this list, from 1, and each span's `source` an index into
   * `sources`. None of it changes the revision. statechart() fills them in.
   */
  readonly spans?: readonly SpanSpec[];
  readonly sources?: readonly { readonly name: string }[];
}

export interface SpanSpec {
  readonly source?: number;
  readonly line: number;
  readonly column?: number;
  readonly end_line?: number;
  readonly end_column?: number;
  readonly offset?: bigint;
  readonly length?: number;
}

export interface StateSpec {
  readonly kind: StateKind;
  readonly span?: number;
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
  readonly span?: number;
}

export interface TransitionSpec {
  readonly kind: "external" | "internal";
  readonly span?: number;
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

export type ActionSpec = { readonly span?: number } & (
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
  }
);

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
  | "timeout"
  | "abandoned";

export class TinyactorsError extends Error {
  readonly code: TinyactorsErrorCode;
  readonly detail?: unknown;
}

// ---------------------------------------------------------------------------
// Tracing (core ADR 0067, napi ADR 0016). OTLP bytes and a decoder; no
// telemetry dependency.

/**
 * Core's trace record kinds, in core's order (TRACE_KINDS). The three
 * `definition*` kinds (core ADR 0080) are for hosts that follow core's
 * record stream; the addon's OTLP output reports definitions through the
 * `tinyactors.definition.*` logs instead, so no detail preset asks for them,
 * and naming them in `kinds` adds nothing to the output.
 */
export type TraceKind =
  | 'spawned' | 'loaded' | 'unloaded' | 'finished' | 'destroyed'
  | 'macrostepBegin' | 'macrostepEnd' | 'condition' | 'microstep' | 'raised'
  | 'dataInit' | 'assign' | 'script' | 'foreach' | 'actionFailed'
  | 'sent' | 'cancelled' | 'invoked' | 'invokeCancelled'
  | 'paused' | 'resumed' | 'stopped' | 'ignored' | 'forwarded' | 'longMacrostep' | 'runaway'
  | 'definitionRegistered' | 'definitionRetired' | 'definitionFreed'
  | 'abandoned' | 'withdrawn';

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

/** A bucket's latest sample, with the sampled trace step it happened in. */
export interface Exemplar {
  /** The bucket's index in `buckets`. */
  bucket: number;
  value: number;
  /** Milliseconds since the epoch. */
  time: number;
  traceID: string;
  spanID: string;
}

/** A distribution with explicit bucket bounds (OTLP Histogram). */
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

// ---------------------------------------------------------------------------
// The debug adapter (napi ADR 0027)

/** An operation a debug session may be allowed on the live actor. */
export type DebugPermission = 'send' | 'set' | 'goto' | 'hold' | 'evaluate';

/**
 * The most a session may do: `watch` never stops or changes the actor;
 * `fork` debugs a copy in a debug System of its own; `live` works on the
 * actor itself, as `allow` permits, and stops it only between macrosteps.
 */
export type DebugMode = 'watch' | 'fork' | 'live';

/** One record for the audit hook. `time` is milliseconds since the epoch. */
export type DebugAuditEntry = {
  time: number;
  /** DebugSession.id. */
  session: number;
  /** While attached: the actor's label, its session ID and the mode. */
  actor?: string;
  /** In decimal. */
  sessionID?: string;
  mode?: DebugMode;
} & (
  | { type: 'attach'; allow: DebugPermission[]; [detail: string]: unknown }
  | { type: 'detach'; reason: string }
  /** A line breakpoint's source is `path`, or `sourceReference` for SCXML the session wrote. */
  | { type: 'breakpoints'; kind: 'line' | 'function' | 'data'; [detail: string]: unknown }
  /** A console line, before it runs. */
  | { type: 'command'; text: string }
  | { type: 'evaluate'; context: string; expression: string }
  /**
   * A change to the actor, a hold or a step: send, set, goto, abandon,
   * cancel, replace, clock, hold, release (also on the hold timeout, with
   * `reason: 'hold timeout'`), step, restart, stepBack, reverseContinue,
   * restartFrame.
   */
  | { type: 'mutation'; operation: string; [detail: string]: unknown }
);

export interface DebugSessionOptions {
  /** The highest mode an attach may ask for. */
  mode: DebugMode;
  /**
   * What an attach may ask to do on a live actor (default none). A fork
   * allows everything on its copy but `evaluate`, which it needs granted.
   *
   * `evaluate` runs arbitrary JavaScript in this process: watch, hover and
   * console expressions, breakpoint conditions and logpoint `{expression}`
   * parts. Grant it only to someone who may run code on the host. Without
   * it the Variables pane, the console commands and setVariable still work.
   */
  allow?: DebugPermission[];
  /**
   * Hears every attach, detach, breakpoint change, console command,
   * evaluation and mutation before it happens; throwing refuses it.
   */
  audit?: (entry: DebugAuditEntry) => void;
  /** Takes each outgoing DAP message (an object). */
  send: (message: DAPMessage) => void;
  /** Milliseconds after which a live hold ends by itself (default 30,000). */
  holdTimeout?: number;
}

/** A Debug Adapter Protocol message: a request, response or event. */
export interface DAPMessage {
  seq: number;
  type: 'request' | 'response' | 'event';
  [field: string]: unknown;
}

/**
 * A DAP session over one attached actor. It opens no socket and checks no
 * credentials: put it behind the application's transport and
 * authentication, and decide each user's mode and permissions before
 * creating it.
 */
export interface DebugSession {
  readonly id: number;
  readonly mode: DebugMode;
  readonly allow: DebugPermission[];
  /** True while an actor is attached. */
  readonly attached: boolean;
  /**
   * Takes one incoming message. Requests are answered in the order they
   * arrive; the promise settles once this one is answered.
   */
  receive(message: DAPMessage): Promise<void>;
  /** The transport closed: detach, releasing a hold and closing a fork, without answering. */
  close(): void;
}

export declare function createDebugSession(system: System, options: DebugSessionOptions): DebugSession;

/**
 * The base protocol's framing for a byte stream (stdio, a TCP socket):
 * push() each chunk; `onMessage` gets each whole message in order. A
 * malformed header or body throws an `invalid` TinyactorsError.
 */
export declare class DAPReader {
  constructor(onMessage: (message: DAPMessage) => void);
  push(chunk: Uint8Array | string): void;
}

/** One message as bytes: the Content-Length header, then its JSON. */
export declare function encodeDAPMessage(message: object): Uint8Array;
