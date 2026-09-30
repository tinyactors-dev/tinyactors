/// <reference types="node" />
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
 * fails. `grep -n "// status:" index.d.ts` lists what is outstanding.
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
  /**
   * Core's incarnation (core ADR 0088, napi ADR 0032): the first part of
   * every session ID's text, `<incarnation>.<session>.<family>`. Choose it
   * distinct from the systems and processes before this one, so a session
   * URI written into a log or into data never reaches an unrelated actor
   * later. A non-negative safe integer or a bigint of at most 64 bits;
   * default 64 random bits. Tests that compare session text pass 0.
   */
  incarnation?: number | bigint;
  /** System parameters, fixed at creation (core ADR 0092, napi ADR 0035); see SystemParameters. */
  parameters?: SystemParameters;
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
  /**
   * How unload() keeps an actor's JavaScript values: "serialize" (the
   * default) writes them with v8.serialize, "handles" keeps each value by
   * handle in this process. unload() throws `unsupported` for a value it
   * can do neither with.
   */
  unloadValues?: "serialize" | "handles";
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

/**
 * Why a loader is called (setLoader): mail for a name, an unloaded actor's
 * own timer, or mail a chart sent to a session of the family that has gone
 * (napi ADR 0032 section 2).
 */
export type LoadReason = "mail" | "timer" | "session";

/**
 * What a loader is asked for: a named address, or, for `why` "session", a
 * session of the family that has gone, by its text (ActorInspection.sessionID).
 * A session of another incarnation, or one whose ID another family holds, is
 * shown as this incarnation's session 0: `<incarnation>.0.<family>`.
 */
export type LoadTarget = ActorAddress | { readonly family: string; readonly sessionID: string };

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
  | { readonly reject: unknown }
  | {
      /**
       * Rebuild the actor from its input log (napi ADRs 0033 and 0034):
       * `inputs` is the log the sink stored for the address, byte chunks cut
       * anywhere, which napi reads a chunk at a time and never holds whole
       * (an async iterable such as a database cursor works). No records: a
       * fresh actor of `spawn` at the address. Otherwise a replaying actor is
       * made from the first record (`spawned`, or a checkpoint), the rest is
       * replayed into it, and it is published at the last record's time; the
       * messages the log shows queued and never delivered go back in its
       * mailbox, then the mail that waited for the load. A `destroyed` record
       * ends a life, and the next starts at the record after it. A history
       * that ends in a final state ends the load `done` (its mail becomes
       * dead letters with reason `done`). `onReport` gets what the replay
       * did. Any other failure is `load-failed`. For a named address only.
       */
      readonly replay: {
        readonly spawn: Definition<Data>;
        readonly inputs: Iterable<Uint8Array> | AsyncIterable<Uint8Array>;
        readonly onReport?: (report: ReplayReport) => void;
      };
    };

export interface LoaderOptions {
  /** Milliseconds the loader may take; after that its mail is `load-timeout` (default 5000). */
  readonly timeout?: number;
  /** Messages that may wait for one address; the rest are `waiting-full` (default 1000). */
  readonly maxWaiting?: number;
}

/**
 * A system's parameters (core ADR 0092, napi ADR 0035): each set once when
 * the system is created and never changed; an unknown name, a non-integer
 * or a value out of bounds throws `invalid` naming the parameter and its
 * bounds. parameterInfo() lists every one with its default, bounds and unit;
 * System.parameters reads them back.
 */
export interface SystemParameters {
  // Core's (core ADR 0092); their bounds come from core's own table.
  /**
   * Microsteps one macrostep may run before its actor is quarantined with
   * fault code `runaway` (core ADR 0075); 0 for no limit; default 100000. A
   * tenth of it writes a `tinyactors.macrostep.long` log. `release()` lets a
   * runaway finish its macrostep; the next one has the limit again.
   */
  microstepLimit?: number;
  /**
   * Event blocks core keeps for reuse (core ADR 0069): none at creation, one
   * made when a delivery finds none, kept when released while fewer than
   * this many are kept (core ADR 0093); default 1024, at most 65536. A warm
   * system allocates nothing per message while no more actors than this hold
   * an event at once; each kept block is about 792 bytes.
   */
  eventBlockCache?: number;
  /** Segments an event name may have; default 64, from 8 to 64. */
  eventNameDepth?: number;
  /** Bytes kept of a data model's refusal message; default 256, from 16 to 65536. */
  compileMessageBytes?: number;
  /**
   * Bytes of a compile error formatted as one line (ChartRefusal's message);
   * a larger value shows more of a deep path and of long IDs. Default 1024,
   * from 256 to 65536. napi keeps at most 1023 bytes of the line.
   */
  compileTextBytes?: number;
  /** Longest unknown target that gets a nearest-name suggestion; default 128, at most 65536. */
  compileNearestBytes?: number;
  /**
   * One interpreter's frames and sets, reserved at creation; a chart that
   * needs more is refused by define(), naming the figure. Default 65536,
   * from 1024 to 1 GiB.
   */
  interpreterBytes?: number;
  /** One run's temporaries whose size depends on content (rendered text, <param>s); default 16384, from 256 to 1 GiB. */
  renderBytes?: number;
  /** Interpreters running inside one another (cancelling nested invocations); default 4, from 1 to 64. */
  interpreterNesting?: number;
  /** Resident actors; default and maximum 2147483647 (no limit). */
  maxActors?: number;
  /** Pending delayed sends of one actor; default and maximum 2147483647 (no limit). */
  maxPendingSends?: number;
  /** Live definitions; default and maximum 2147483647 (no limit). */
  maxDefinitions?: number;
  // napi's.
  /**
   * Bytes of a value's JSON in a trace attribute (`values: true`) or a log
   * record: one limit for both (napi ADR 0036). A longer value is cut and
   * marked `<key>.truncated`. Default 1024, from 16 to 1 MiB.
   */
  valueBytes?: number;
  /**
   * The OTLP staging, reserved whole while a trace sink is installed (napi
   * ADR 0036). A turn whose spans and logs reach it ends early, so the sink
   * gets that turn's request and the next turn goes on; a call outside a turn
   * that fills it (a burst of spawn() calls, say) hands the sink a request
   * before it returns. So no request holds more than this plus one call's or
   * one macrostep's records, and nothing is dropped. Default 1 MiB, from 64
   * KiB to 1 GiB.
   */
  traceStagingBytes?: number;
  /** Data models registerDatamodel() may add; default 8, at most 1024. */
  registeredDatamodels?: number;
  /** Longest registered data model name, in bytes; default 32, from 8 to 256. */
  datamodelNameBytes?: number;
  /** Microsteps one resume may run before the actor yields (core's `budget`); default 64, at least 1. */
  resumeBudget?: number;
  /**
   * Units of executable content one resume may run before the actor pauses
   * inside a microstep (core ADR 0061); 0 means no limit; default 1024.
   */
  actionBudget?: number;
  /** Runnable actors resumed in one pump turn; default 1024, at least 1. */
  resumesPerTurn?: number;
  /** Due sends dispatched during one pump turn; default 1024, at least 1. */
  dueBudget?: number;
  /** Deliveries started, and dead letters reported, in one pump turn; default 1024, at least 1. */
  messageBudget?: number;
  /** Accepted messages not yet delivered or reported; post() throws `queue-full` beyond it. 0 (default): no limit. */
  maxQueuedMessages?: number;
  /**
   * Retained-bytes estimate of the data of accepted messages not yet
   * delivered or reported (ADR 0005 point 9); 0 (default): no limit. The
   * estimate: `options.bytes`, or 2 bytes per string code unit, byteLength
   * for buffers, 64 for other objects, 0 for primitives. Mail between actors
   * carries no estimate.
   */
  maxRetainedBytes?: number;
}

export interface ParameterInfo {
  readonly default: number;
  readonly min: number;
  readonly max: number;
  readonly unit: string;
  readonly owner: "core" | "napi";
}

/** Every system parameter, by name (napi ADR 0035). */
export declare function parameterInfo(): Readonly<Record<keyof SystemParameters, ParameterInfo>>;

export declare function createSystem(options?: SystemOptions): System;

export interface System {
  /** The one member besides close() that works after close(); the others throw `closed`. */
  readonly closed: boolean;
  readonly clock: "realtime" | "manual";
  readonly time: number;
  /** Core's incarnation (SystemOptions.incarnation). */
  readonly incarnation: bigint;
  /** Every parameter's value, fixed at creation (SystemOptions.parameters). */
  readonly parameters: Readonly<Required<SystemParameters>>;

  /**
   * Compile and register a chart: a statechart() builder, or a spec in the
   * generated shape (the shape core's definition export returns). A refused
   * chart throws `invalid` or `unsupported`, with core's compile error as
   * `detail` (ChartRefusal). `options.microstepLimit` overrides the system's
   * `microstepLimit` parameter for this definition; 0 for no limit.
   *
   * A spec's SCXML `<invoke>`s with a literal `<content>`, or a literal
   * `src` when `options.resolve` is given, are compiled and nested as an
   * imported document's are (napi ADR 0027), so `define(d.export())`
   * keeps d's children. A child that cannot be compiled refuses the spec,
   * with ImportRefusal as `detail`. `resolve` is kept for children compiled
   * at invoke time, as the other overload's is. A spec's `datamodel` may
   * name a data model registerDatamodel() registered.
   */
  define<Data extends object = Record<string, unknown>>(
    source: DefinitionSpec | DefinitionBuilder<Data>,
    options?: { microstepLimit?: number; resolve?: (src: string) => string | Uint8Array | undefined },
  ): Definition<Data>;
  /**
   * Decode a chart with a registered codec and define it (napi ADR 0027).
   * `format` names the codec by its name or one of its extensions; the
   * built-in SCXML codec is "application/scxml+xml", extension "scxml".
   * To read another syntax, register a codec written in JavaScript with
   * registerCodec(). A refusal throws `invalid` or `unsupported` with
   * ImportRefusal as `detail`, which names the codec and says where.
   *
   * With the SCXML codec (napi ADR 0019), `<scxml name>` is the
   * definition's family and is required; the revision is a hash of the
   * document. Its `datamodel` attribute picks `javascript` (also for a
   * document without one) or a data model registerDatamodel() registered;
   * `datamodel="null"` opts into core's null data model, which keeps no
   * data. lib/ ships no ECMAScript compiler, so under `javascript` an
   * expression may only be `In(state)` or a data name: register a data
   * model to compile others. A refused document throws `invalid` or
   * `unsupported` with ImportRefusal as `detail`, which says where.
   */
  define<Data extends object = Record<string, unknown>>(
    source: string | Uint8Array,
    options: {
      /** A registered codec's name or extension, such as "scxml". */
      format: string;
      /**
       * The identity to register the chart under. No chart syntax carries
       * one (core ADR 0053), so a round trip that keeps an explicit
       * identity passes EncodedChart.identity here. Without it, the codec's
       * rule applies: for SCXML, `<scxml name>` and a fingerprint.
       */
      identity?: DefinitionIdentity;
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
       * What the document is called (a codec other than SCXML gets it as
       * ChartDecodeContext.name), such as its path or a file:// URL: the
       * source its spans name (core ADR 0085), which sourceOf() answers and
       * a debugger opens (napi ADR 0026). Without it the spans name no
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
   * Register a data model (ADR 0019): `compile` turns each source
   * expression into what a builder chart would pass, and the built-in data
   * model runs it. An imported document chooses it by its `datamodel`
   * attribute, and a spec by its `datamodel` member (napi ADR 0027).
   * `javascript` and `null` are reserved.
   */
  registerDatamodel(name: string, datamodel: DatamodelCompiler): void;

  /**
   * Register a function that charts call by name (napi ADR 0029): source
   * text `name()`, which named(name) writes. It is called as a builder
   * function is, with `this` = the data and the context. A call is bound
   * when a chart is defined; a name nothing is registered under refuses
   * the chart. A name is registered once; `In` is reserved. A chart whose
   * functions are all named round-trips through export() and codecs, and
   * defining it again answers the same definition. This is the preferred
   * way to write guards (`when(named(...))`) and actions
   * (`script(named(...))`, and computed values); see statechart().
   */
  registerFunction(name: string, run: (this: any, context: EvaluationContext<any>) => unknown): void;

  /**
   * Register a class that charts construct by name (napi ADR 0031): source
   * text `new Name(arguments)`, with JSON arguments in which
   * `new Name(...)` may appear again. The built-in data model reads it
   * without evaluating anything, as a value or a condition: the arguments
   * are parsed when a chart is defined (an unregistered name refuses the
   * chart), and the constructor runs, with fresh arguments, each time the
   * expression is evaluated. An evaluating data model gets the class in
   * its compile scope (DatamodelScope). A value whose `[serialize]()`
   * names the class is printed so.
   *
   * `name` is an identifier other than `In`. Functions and classes share
   * one name space, and a name is registered once, whichever it names:
   * registering it again throws `invalid`. `Date`, `Map`, `Set`, `RegExp`
   * and the typed arrays are registered in every system.
   */
  registerClass(name: string, constructor: new (...args: any[]) => unknown): void;

  /**
   * Register a chart syntax (napi ADR 0027): define(text, { format }) then
   * decodes with it, and Definition.encode(format) writes with it. The
   * codec is written in JavaScript and runs synchronously inside those
   * calls. A name or extension already registered throws `invalid`;
   * "application/scxml+xml" and "scxml" are the built-in SCXML codec's.
   */
  registerCodec(codec: ChartCodec): void;
  /** The registered codecs, the built-in SCXML codec first. */
  codecs(): ChartCodecInfo[];

  /**
   * The unretired definition with this identity. define() of an identity
   * already registered returns that definition and takes another hold;
   * a different structure under it throws `definition-mismatch`. Functions,
   * locations and literal() values are compared by identity (napi ADR 0030,
   * core ADR 0087): the same ones answer the registered definition, while
   * new ones (closures made anew) register beside it, and this finds one of
   * them. Named functions (named(), napi ADR 0029) are the same whatever
   * code builds the chart.
   */
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
  /**
   * Terminate `actor`. Throws `not-found` once it is gone. With an input log,
   * a named actor of a logged family is recorded `destroyed` (napi ADR 0033).
   */
  destroy(actor: ActorID): void;
  /** Put an actor that faulted back into scheduling (ADR 0008). */
  release(actor: ActorID): void;

  /** The messages waiting in `actor`'s mailbox, oldest first, at most `limit` (default 100). */
  mailbox(actor: ActorID, options?: { readonly limit?: number }): readonly QueuedMessage[];
  /**
   * Abandon `actor`'s macrostep (core ADR 0082): the actor keeps the
   * configuration after its last completed microstep, and the send() that
   * follows the macrostep rejects with `abandoned`. `busy` while it is
   * paused inside a microstep (the pump finishes that microstep in its next
   * turns) or before its bootstrap finished; `invalid` with no macrostep in
   * progress.
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
   * (core ADR 0066). `binding` replaces the data root outright, it is not
   * merged: for an address it replaces the data the actor was unloaded
   * with, which it keeps without one; from an image it defaults to the
   * image's root. A chart on the null data model has no root, and a binding
   * for it throws `unsupported`. An invocation active at capture is
   * reported to the actor as `error.communication.invoke.<id>.unavailable`
   * (core ADR 0038).
   */
  load<Data extends object>(
    target: SessionImage<Data> | ActorAddress,
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

  /**
   * Bring actors of `family` back when mail arrives for them (napi ADR
   * 0020). Mail for a name in `family` with no resident actor (unloaded, or
   * never seen here: the family is virtual) waits while the loader runs,
   * and is delivered first, in order, once it has loaded or spawned the
   * actor. One load runs per address. `why` is "timer" when an unloaded
   * actor's own delayed send is due. `null` removes the loader.
   *
   * Mail a chart sends to a session of `family` that has gone or finished
   * also waits for the loader, with `why` "session" and the target
   * `{ family, sessionID }` (napi ADR 0032 section 2). For a session the
   * loader answers `{ spawn, binding }`, a new instance without a name that
   * takes the waiting mail in order, or "not-found", "reject" or `{ reject }`,
   * which make it dead letters; anything else is a `load-failed` dead letter.
   * One load runs per session; a chart of the family is written for a new
   * instance receiving mail meant for an earlier one. Without a loader, mail
   * to a gone session raises error.communication in the sender (ADR 0022).
   */
  setLoader(
    family: string,
    loader:
      | ((
          target: LoadTarget,
          why: LoadReason,
          load: { readonly offloaded: boolean },
        ) => LoadResult | Promise<LoadResult>)
      | null,
    options?: LoaderOptions,
  ): void;

  /**
   * Wake the unloaded actor at `address` through its family's loader at
   * `time` (core's clock). unload() does this for the image's next deadline.
   * Nothing happens at `time` for an address with no unloaded actor and no
   * loader.
   */
  wakeAt(address: ActorAddress, time: number): void;

  /**
   * Create an unpublished actor of `definition` (core ADR 0089, napi ADR
   * 0032): to replace the resident named actor `incumbent` in place, or to
   * take `address`, which must have no resident actor when it is published.
   * It bootstraps like a spawned actor. `binding` defaults to the
   * incumbent's live data root when `definition` uses the JavaScript data
   * model. With `at`, it is a replaying actor: its clock starts at `at` and
   * moves to the time of each input it takes (core ADR 0094): a message
   * sent to it is taken at the system's time, and replay() gives each
   * record its logged time. It crosses no
   * border (no sends, logs or invoker starts leave it) until it is
   * published. With `log`, the start of an actor's input log holding at
   * least its first record, it is a replaying actor made from that record
   * (napi ADR 0034): `spawned` gives its binding and clock, a checkpoint its
   * whole state, and replay() takes the rest of the log. Without either,
   * core holds its visible bootstrap effects until publish().
   */
  prepare<Data extends object>(definition: Definition<Data>, options: PrepareOptions<Data>): PreparedActor<Data>;

  /**
   * Publish a prepared actor. With an incumbent: it takes the incumbent's
   * address, mail still waiting for the incumbent, and the send()s it
   * carries, move in order to the front of its mailbox, and the incumbent is
   * destroyed; its delayed sends are discarded. Throws `busy` while the
   * incumbent is inside a macrostep; if publishing fails, the incumbent
   * keeps the address and the prepared actor is left for the caller to
   * destroy. Without an incumbent: it claims its address, or replaces an
   * unloaded actor there, and throws `busy` when another actor is resident.
   * `done` when it finished before it was published. A replaying actor's
   * held delayed sends are scheduled (one already due fires at the next
   * dispatch), and its active invocations are reported
   * `error.communication.invoke.<id>.unavailable`, and the messages its log
   * showed queued and never delivered go back in its mailbox. A replaying
   * actor is published at its clock, the last replayed record's time; `at`
   * moves the clock on first.
   */
  publish<Data extends object>(prepared: PreparedActor<Data>, options?: { readonly at?: number }): Actor<Data>;

  /**
   * Install a write-ahead log for the named actors of `families` (napi ADRs
   * 0032 to 0034). Once per turn, `sink.commit(batch)` gets { entries,
   * wakes }: append each entry's `bytes` (that address's records from the
   * turn, in core's portable layout, core ADR 0090) to its `address`'s log
   * as they are, and set each wake's `address` to `at` (null removes it), in
   * one transaction. Store the bytes without parsing them, and hand an
   * address's log back from its family's loader as
   * { replay: { spawn, inputs } }; readInputLog() decodes one for an audit
   * trail. After a restart, set the loaders, then call wakeAt(address, at)
   * for every stored wake. Values are written in the built-in data model's
   * durable text (napi ADR 0031): an input carrying a value with no durable
   * form (a function, a cycle, a class instance without `[serialize]()`) is
   * refused, and post() or send() throws `unsupported`. While a sink is
   * installed, a turn's services (I/O processor sends, invoker starts,
   * cancels and forwards, log records) wait until its batch's commit returns
   * or the promise it returns resolves; batches commit one at a time, in
   * turn order, while later turns run. A commit that throws or rejects stops
   * the system's services and quarantines every actor with inputs in that
   * batch, with a fault whose operation and code are "input-log". `null`
   * removes the sink; what it held runs once the commits before it are done.
   */
  setInputLog(sink: InputLog | null, options?: { readonly families: readonly string[] }): void;

  /**
   * Remove a stable named actor so that its input log becomes its only
   * state (napi ADRs 0032 and 0033): dispatch the system's due sends,
   * record `evicted` with the actor's wake (its first pending delayed
   * send's due time), destroy the actor and remove its directory entry, and
   * arm the wake with wakeAt(), so its family's loader brings it back when
   * a timer is due; mail for its address goes to the loader too. Returns
   * the eviction time. `busy` while it has mail waiting (a timer that fell
   * due included), a send still due, is inside a macrostep, or is
   * quarantined; `invalid` for an actor without a name.
   */
  evict(actor: ActorID): number;

  /**
   * Write a checkpoint of `actor` (core ADR 0091, napi ADR 0034 section 4):
   * an idle, published, named actor of a logged family, in core's portable
   * layout, in chunks through `write`, called synchronously; each chunk is
   * the caller's to keep. A log that starts with a checkpoint replays from
   * it, and records it covers are skipped, so a store may drop the records
   * before it once it is durable. A checkpoint loads only under its own
   * revision: an upgrade by replay needs the log from `spawned`. The
   * messages the actor's mailbox holds travel with it. Throws `busy` for an
   * actor that is not idle, `invalid` for one that is unnamed or not
   * logged, `unsupported` for a value with no durable form, and what `write`
   * throws; after a throw, discard the chunks already written.
   */
  checkpoint(actor: ActorID, write: (chunk: Uint8Array) => void): void;

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
   * spans that ended. The sink is also called outside a turn, synchronously
   * from inside the call that filled the staging (traceStagingBytes, napi
   * ADR 0036); an exception it throws there comes out of that call. Post it to an OTLP/HTTP `/v1/traces` endpoint as it
   * is, or read it with decodeTraces(). Without a sink core builds no record.
   * Each definition's spans share one scope, whose attributes name the
   * definition by family, revision and source (napi ADR 0028).
   */
  setTraceSink(sink?: (traces: Uint8Array, logs?: Uint8Array) => void, options?: TraceSinkOptions): void;
  /** Changes `detail`, `kinds` and `sample` of the installed sink in place; open spans are kept. */
  configureTraces(options: TraceConfiguration): void;
  /**
   * Traces matching deliveries in full, sampled whatever the ratio, until
   * stop(): to these sessions (their text, as `sessionID` shows it, or
   * actors), to the actors at these
   * addresses, and to the actors of these definition families. What a
   * watched delivery causes is watched too. An address is resolved once,
   * now, to its resident actor's session: an address with none throws
   * `not-found`, and a later session at it is not watched.
   */
  watchTraces(watch: {
    sessions?: (string | ActorID)[];
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
   * (`queue-full`), and on close() (`closed`) while it is pending. Abort
   * and timeout withdraw a message still queued; a delivered one is not
   * undone. Only misuse throws, and a call after close() throws `closed`.
   * A send() whose message becomes a dead letter rejects with the reason's
   * code: `not-found` (also for `destroyed`), `done`, `unloaded`,
   * `rejected`, `load-failed`, `load-timeout`, `waiting-full`, or `invalid`
   * for `refused`; a macrostep quarantined as a runaway rejects with
   * `runaway`.
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
   * This and actors() and definitions() are generators: `reentrant` inside
   * a core call, and an invalid `pageSize`, throw from the first next(),
   * not from the call.
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
  /** The system's parameters, as System.parameters (napi ADR 0035). */
  readonly parameters: Readonly<Required<SystemParameters>>;
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

export interface PostOptions {
  /** A W3C traceparent: the macrostep this message starts runs under it (ADR 0015). */
  traceparent?: string;
  /**
   * post() only checks it when called, throwing `aborted` if it already
   * aborted; send() also withdraws a message still queued when it aborts.
   */
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
   * An operator transition (core ADR 0082, ADR 0026): the message is taken
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
  /**
   * "stable" and "done": false when the message enabled no transition, so
   * the chart ignored it (core ADR 0089 section 8, napi ADR 0032 section 4).
   */
  readonly consumed?: boolean;
  /** "done": the actor's done data. */
  readonly completion?: unknown;
}

/* --------------------------------------------------------------------------
 * Definitions
 *
 * A definition is inspectable down to every element, and round-trips
 * (napi ADR 0027):
 *
 * - To render a chart, read Definition.export(): the spec it was compiled
 *   from, with where each element was written (`spans`, `sources`).
 * - To resolve the IDs a trace or an inspection names (`scxml.state.id`,
 *   `scxml.transition.id`, `scxml.action.id`, TracePosition), use
 *   Definition.state(), transition(), action() and expression().
 * - To read or write a syntax other than SCXML, register a codec written
 *   in JavaScript with System.registerCodec(), then define(text, { format })
 *   and Definition.encode(format).
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
 *
 * `scope` holds the functions and classes the system registered when the
 * chart is defined (napi ADR 0031), for a data model that puts them in its
 * expressions' scope. A registered function is meant to be called with
 * `this` = the data and the context as its argument, as the built-in data
 * model calls `name()`.
 */
export interface DatamodelCompiler {
  compile(
    source: string,
    role: ExpressionRole,
    scope: DatamodelScope,
  ): ((this: any, context: any) => unknown) | string | { get(this: any, context: any): unknown; set(this: any, value: unknown, context: any): void };
  /**
   * The inverse of compile (napi ADR 0031): a chart's host expression as
   * source text in this data model's language, for Definition.encode().
   * Throw a ChartCodecError with code `unrepresentable` to refuse one; any
   * other exception propagates from encode(). A value with `[serialize]()`
   * describes itself: print its name and arguments. A function is an
   * expression: print text that calls it with `this` = the data and the
   * context. It gets no role (core's printer does not know it), so the
   * text must serve as a value, a condition and a script alike. Without
   * print, charts cannot be encoded for this data model.
   */
  print?(expression: PrintableExpression): string;
}

/** What DatamodelCompiler.compile() gets as its scope (napi ADR 0031). */
export interface DatamodelScope {
  /** System.registerFunction()'s functions, by name. */
  readonly functions: Readonly<Record<string, (this: any, context: any) => unknown>>;
  /** System.registerClass()'s classes by name, the standard ones included. */
  readonly classes: Readonly<Record<string, new (...args: any[]) => unknown>>;
}

/** A host expression as DatamodelCompiler.print() receives it. */
export type PrintableExpression =
  | { readonly kind: "function"; readonly function: (this: any, context: any) => unknown }
  | { readonly kind: "literal"; readonly value: unknown }
  | { readonly kind: "location"; readonly location: Location<any, unknown> };

/**
 * `Symbol.for('tinyactors.serialize')` (napi ADR 0031). A value that
 * implements it describes itself for data models to print: the name its
 * constructor is registered under (System.registerClass) and the arguments
 * that rebuild it. Being a registered symbol, a class can implement it
 * without depending on this package.
 */
export declare const serialize: unique symbol;

/**
 * What `value[serialize]()` returns: `name`, the identifier its
 * constructor is registered under where the text will be read, and the
 * arguments that rebuild it, each printable in turn.
 */
export interface SerializedValue {
  readonly name: string;
  readonly arguments: readonly unknown[];
}

export interface Serializable {
  [serialize](): SerializedValue;
}

/** Why define(text, { format }) refused a document, as `detail`. */
export interface ImportRefusal {
  /** "decode": the codec refused the document; "compile": core refused the chart. */
  readonly stage: "decode" | "compile";
  readonly message: string;
  /** The codec's name, such as "application/scxml+xml". */
  readonly format: string;
  /** The codec's refusal, when `stage` is "decode". */
  readonly code?: ChartErrorCode;
  /** The element or production, and the attribute, the refusal is about. */
  readonly construct?: string;
  readonly attribute?: string;
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
   * Where element `id` of `kind` was written (core ADR 0085), or undefined
   * when the definition keeps no span for it. statechart() records the file,
   * line and column of the call that wrote each state, transition, initial
   * transition and action.
   */
  sourceOf(kind: ElementKind, id: number): SourcePlace | undefined;

  /**
   * The spec this definition was compiled from (core ADR 0042), in the
   * shape define() takes, with its spans and sources. `identity` is present
   * when the definition has an explicit one and absent when it was
   * fingerprinted. Expressions are as authored: source text, the same
   * functions, the same literal() values; a builder chart's child is its
   * DefinitionID.
   *
   * The round trip (napi ADR 0027): `system.define(d.export())` answers d
   * itself, for every chart. The export hands back the same functions,
   * locations and literal() values, which are the same handles (napi
   * ADR 0030, core ADR 0087).
   * `microstepLimit` and `resolve` are the host's settings, not the
   * chart's: pass them to define() again. Throws `not-found` once freed.
   */
  export(): DefinitionSpec;

  /**
   * The chart in a registered codec's syntax, by its name or extension
   * (napi ADR 0027). Defining `bytes` with `{ format, identity }` answers
   * a definition with this one's revision. Throws `unsupported` when the
   * codec only decodes, and `invalid` with the codec's ChartErrorCode as
   * `detail.code` when it cannot write the chart: SCXML cannot write a
   * function (`unrepresentable`). Host expressions are printed by the data
   * model (napi ADR 0031): the chart's own, or `options.datamodel`, which
   * the written chart then names. The built-in data model prints JSON
   * values, and `new Name(arguments)` for Date, Map, Set, RegExp, typed
   * arrays and values with `[serialize]()`; it refuses functions (name
   * them) and anything else. The text holds a copy of each value, not the
   * value, so defining it while the original is registered under the same
   * identity throws `definition-mismatch`; another system defines it under
   * that identity. Text written for another data model is another chart.
   * `options.datamodel` must name the built-in data model or a registered
   * one that has print(); otherwise encode() throws `unsupported`. With it,
   * a JavaScript codec gets the chart printed for that data model (every
   * expression source text, `datamodel` set); without it, the chart's
   * values, functions included, as export() gives them.
   */
  encode(format: string, options?: { datamodel?: string }): EncodedChart;

  /**
   * Core's lookups by ID (core ADR 0071), read on each call. IDs count from
   * 0 up to the DefinitionInspection counts; an ID out of range answers
   * undefined, and a freed definition throws `not-found`.
   */
  state(id: number): ChartState | undefined;
  /** The state whose author ID is `name`. */
  findState(name: string): ChartState | undefined;
  transition(id: number): ChartTransition | undefined;
  action(id: number): ChartAction | undefined;
  /**
   * In the export's own form: source text, a function, a literal(), or a
   * `{ get, set }` location object.
   */
  expression(id: number): ExpressionSpec | LocationSpec | undefined;
  /** A literal <content> body or send target, as the chart holds it. */
  content(id: number): { readonly bytes: Uint8Array; readonly mediaType?: string } | undefined;
  /** An <invoke> by declaration index. */
  invoke(index: number): ChartInvoke | undefined;

  /** Prevent new actors from using this definition. Existing actors continue. */
  retire(): void;
}

/** A state as core numbers it: IDs in document order. */
export interface ChartState {
  readonly id: number;
  /** The author's state ID. */
  readonly name: string;
  /** The parent state's ID; null for a top-level state. */
  readonly parent: number | null;
  readonly kind: StateKind;
}

/**
 * A transition. The <transition> elements come first, in document order;
 * then one initial transition per compound state, and the document's,
 * which the bootstrap takes.
 */
export interface ChartTransition {
  readonly id: number;
  readonly origin: "authored" | "initial" | "bootstrap";
  /** The state it leaves; null for the bootstrap. */
  readonly source: number | null;
  readonly type: "external" | "internal";
  /** The event descriptor as authored; absent for an eventless or initial one. */
  readonly event?: string;
  readonly targets: readonly number[];
  /** True when the targets are SCXML's default, the first child state. */
  readonly defaultTargets: boolean;
  /** The guard's expression ID; null when unguarded. */
  readonly condition: number | null;
  readonly actionCount: number;
  /** Its place among the <transition> elements; 0 for an initial one. */
  readonly documentOrder: number;
}

/** An action, and the block of executable content it sits in. */
export interface ChartAction {
  readonly id: number;
  readonly kind: "raise" | "send" | "cancel" | "assign" | "script" | "log" | "if" | "foreach";
  readonly block: "onentry" | "onexit" | "transition" | "then" | "else" | "foreach" | "finalize" | "script";
  /**
   * A state (onentry, onexit, finalize), a transition, or an action (then,
   * else, foreach); null for the document's script.
   */
  readonly owner: number | null;
  /** Which of the owner's blocks of that kind, from 0. */
  readonly blockIndex: number;
  /** Its place in the block, from 0. */
  readonly index: number;
}

export interface ChartInvoke {
  readonly index: number;
  /** The literal type, normalized; null when the type is computed. */
  readonly type: string | null;
  /** Literal content and src, as content() IDs; null when absent. */
  readonly content: number | null;
  readonly src: number | null;
  /** The child definition nested for it; null when none is. */
  readonly nested: DefinitionID | null;
}

/** What Definition.encode() wrote. */
export interface EncodedChart {
  readonly bytes: Uint8Array;
  /** The explicit identity, to pass back to define(); absent when fingerprinted. */
  readonly identity?: DefinitionIdentity;
  /** Where the codec wrote element `id` of `kind` in `bytes`, as sourceOf() answers. */
  placeOf(kind: ElementKind, id: number): SourcePlace | undefined;
}

/* --------------------------------------------------------------------------
 * Chart codecs (napi ADR 0027, core ADR 0053)
 *
 * A codec turns a chart syntax into a DefinitionSpec and back. To read or
 * write a syntax other than SCXML, write one in JavaScript and register it
 * with System.registerCodec():
 *
 *     system.registerCodec({
 *       name: "application/vnd.example.chart+json",
 *       extensions: ["chart.json"],
 *       decode(input, { name }) { return toSpec(JSON.parse(utf8(input)), name); },
 *       encode(spec) { return JSON.stringify(fromSpec(spec)); },
 *     });
 *     const order = system.define(text, { format: "chart.json", name: "orders.chart.json" });
 *     const { bytes, identity } = order.encode("chart.json");
 *
 * A codec is faithful when defining what it encoded answers a definition
 * with the same revision. It runs synchronously, inside define() and
 * encode(), and refuses by throwing a ChartCodecError.
 * -------------------------------------------------------------------------- */

/** Core's chart error codes (TA_CHART_ERROR_*), lowercase with dashes. */
export type ChartErrorCode =
  | "syntax" | "not-chart" | "unknown-construct" | "misplaced-construct"
  | "unknown-attribute" | "missing-attribute" | "conflicting-attribute"
  | "bad-value" | "foreign-content" | "src-unresolved" | "limit" | "unrepresentable";

export interface ChartCodecInfo {
  /** A registered media type where one exists, otherwise a stable identifier. */
  readonly name: string;
  /** File extensions without the dot; `format` may name the codec by one. */
  readonly extensions?: readonly string[];
  /** One line, for a tool's --help. */
  readonly description?: string;
  readonly decodes: boolean;
  readonly encodes: boolean;
}

export interface ChartCodec {
  readonly name: string;
  readonly extensions?: readonly string[];
  readonly description?: string;
  /**
   * The chart in `input` as a spec. Fill `spans` and `sources`, with
   * `context.name` as the first source, when you know where each element
   * was read (core ADR 0085). Absent when the codec only writes.
   */
  decode?(input: Uint8Array, context: ChartDecodeContext): DefinitionSpec;
  /**
   * `spec` (a Definition.export()) in this syntax. `spans`, parallel to
   * `spec.spans`, says where each element was written. A function has no
   * faithful source form: throw `unrepresentable`, or document that the
   * output does not round-trip. Absent when the codec only reads.
   */
  encode?(spec: DefinitionSpec, context: ChartEncodeContext):
    Uint8Array | string | { bytes: Uint8Array | string; spans?: readonly (SpanSpec | undefined)[] };
}

export interface ChartDecodeContext {
  /** define()'s `name`: the source the spans should name. */
  readonly name?: string;
  /** define()'s `resolve`, for `src` references; absent when none was given. */
  resolve?(src: string): string | Uint8Array | undefined;
}

export interface ChartEncodeContext {
  /** The definition being written, for exporting a nested child to write inline. */
  readonly definition: Definition;
}

/** A codec's refusal. */
export declare class ChartCodecError extends Error {
  constructor(code: ChartErrorCode, message: string, options?: {
    line?: number; column?: number; offset?: number; construct?: string; attribute?: string;
  });
  readonly code: ChartErrorCode;
  readonly line?: number;
  readonly column?: number;
  readonly offset?: number;
  readonly construct?: string;
  readonly attribute?: string;
}

/** The kinds of compiled element core numbers, as trace records name them. */
export type ElementKind = "state" | "transition" | "action" | "expression";

/**
 * A place in a source. Lines and columns count from 1; a column is the
 * engine's (UTF-16 code units in JavaScript). The column, the end and the
 * byte range are present when the front end knew them: statechart() and a
 * document codec record the column; a spec's span may give only a line.
 */
export interface SourcePlace {
  /** The source's name: a file, a URL, or "" when the spec named none. */
  readonly source: string;
  readonly line: number;
  readonly column?: number;
  readonly endLine?: number;
  readonly endColumn?: number;
  readonly offset?: number;
  readonly length?: number;
}

export interface DefinitionInspection {
  readonly id: DefinitionID;
  readonly identity: DefinitionIdentity;
  /** StatechartOptions.name, or an imported document's `<scxml name>`. */
  readonly name?: string;
  /** The data model the chart was compiled against, e.g. "javascript". */
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
  /** The upper bounds of the lookups' IDs (napi ADR 0027). */
  readonly transitionCount: number;
  readonly actionCount: number;
  readonly expressionCount: number;
  readonly contentCount: number;
  readonly invokeCount: number;
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
 *
 * Write guards and actions as named functions (napi ADR 0030): register each
 * once with System.registerFunction() and call it with named():
 *
 *     system.registerFunction('belowLimit', function () { return this.switches < 3; });
 *     statechart({ family: 'light', revision: 'v1' })
 *       .data('switches', 0)
 *       .state('off', (s) => s.on('toggle', (t) => t.when(named('belowLimit')).target('on')));
 *
 * Such a chart can be written as SCXML or another syntax and read back, a
 * misspelled name is reported by define(), and the same chart built twice
 * is one definition. An inline function also works; a chart with one
 * cannot be written as text, and building it again with new closures makes
 * another definition.
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

declare const namedFunctionBrand: unique symbol;
export interface NamedFunction {
  readonly [namedFunctionBrand]: true;
  readonly name: string;
}

/**
 * A call of the function registered under `name` (System.registerFunction,
 * napi ADR 0029), accepted wherever a builder takes a function. The spec
 * holds it as the source text `name()`, so, unlike an anonymous function,
 * it is part of the chart's content: its fingerprint, its export, and what
 * a codec writes.
 */
export declare function named(name: string): NamedFunction;

declare const contentValueBrand: unique symbol;
export interface ContentValue {
  readonly [contentValueBrand]: true;
  /** The content's bytes: a string's UTF-8, or a copy of the Uint8Array. */
  readonly bytes: Uint8Array;
  readonly mediaType?: string;
}

/**
 * Literal SCXML `<content>`, kept in the definition as bytes with an
 * optional media type; accepted wherever a value is. A send's data reaches
 * its I/O processor, and an invoke's content its invoker, as text with
 * `contentType` (IORequest, InvokeRequest). Anywhere else the content is a
 * value: parsed as JSON when it parses, otherwise the text with each run of
 * whitespace made one space and none leading or trailing (SCXML B.2.2).
 */
export declare function content(
  body: string | Uint8Array,
  options?: { readonly mediaType?: string },
): ContentValue;

export type ValueInput<Data extends object, Value = unknown> =
  | (Value extends (...arguments_: never[]) => unknown ? never : Value)
  | LiteralValue<Value>
  | ContentValue
  | NamedFunction
  | Expression<Data, Value>;

export interface DefinitionBuilder<Data extends object>
  extends StateContainerBuilder<Data, DefinitionBuilder<Data>> {
  /** `initial` is data even when it is a function; content() is parsed (see content()). */
  data<Key extends keyof Data & string>(name: Key, initial: Data[Key] | ContentValue): this;
  dataExpression<Key extends keyof Data & string>(
    name: Key,
    initial: Expression<Data, Data[Key]> | NamedFunction,
  ): this;
  initial(target: Target, actions?: ActionBlock<Data>): this;
  /** A script run once at initialization, after early-bound data (SCXML 3.2.2). */
  script(run: Script<Data> | NamedFunction): this;
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
  /**
   * A history state. `build` gives it its default transition, which it
   * needs exactly one of, with always(): `h => h.always(t => t.target("a"))`.
   */
  history(
    id: string,
    depth: "shallow" | "deep",
    build: StateBlock<Data>,
  ): Self;
}

export interface StateBuilder<Data extends object>
  extends StateContainerBuilder<Data, StateBuilder<Data>> {
  /** Override the kind normally inferred from the builder method and children. */
  kind(kind: StateKind): this;
  initial(target: Target, actions?: ActionBlock<Data>): this;

  /** `initial` is data even when it is a function; content() is parsed (see content()). */
  data<Key extends keyof Data & string>(name: Key, initial: Data[Key] | ContentValue): this;
  dataExpression<Key extends keyof Data & string>(
    name: Key,
    initial: Expression<Data, Data[Key]> | NamedFunction,
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
  /** A function, named(), or source text: "In('s')", a data name, or "name()". */
  when(condition: Condition<Data> | NamedFunction | string): this;
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

  script(run: Script<Data> | NamedFunction): Self;
  log(label?: string, value?: Expression<Data> | NamedFunction | string): Self;

  choose(
    condition: Condition<Data> | NamedFunction | string,
    then: ActionBlock<Data>,
    otherwise?: ActionBlock<Data>,
  ): Self;

  forEach<Item>(
    array: Expression<Data, readonly Item[]> | NamedFunction | (keyof Data & string) | LiteralValue<readonly Item[]>,
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
  /** id() and idLocation() replace each other: the last one called wins. */
  id(id: string): this;
  idLocation(location: keyof Data & string | Location<Data, string>): this;
  data(value: ValueInput<Data>): this;
  param(name: string, value: ValueInput<Data>): this;
}

export type InvokeBlock<Data extends object> = (
  invoke: InvokeBuilder<Data>,
) => void;

export interface InvokeBuilder<Data extends object> {
  /** id() and idLocation() replace each other: the last one called wins. */
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
   * of this actor, stored with its data; they survive JSON and structured
   * clone, and may be re-armed in another system or process. They keep
   * their IDs, due times and traces; a due time already past fires on the
   * next dispatch. A `destination` of "self" goes to this new actor, and an
   * address to whatever actor has that address when the send is due. An
   * ActorID destination (a send to an unnamed other actor) re-arms only as
   * a live ActorID of this system; any other form of it throws `invalid`
   * naming the entry, since a stored slot and generation would name
   * whichever actor holds that slot by then.
   */
  readonly pendingSends?: readonly DelayedSendInspection[];
}

/**
 * The view spawn() returns (ADR 0002): an ActorID with methods that ask the
 * system. It holds no native resource and no finalizer.
 */
export interface Actor<Data extends object = Record<string, unknown>> extends ActorID {
  /**
   * True once no resident actor has this ID: destroyed, finished and
   * destroyed, or unloaded. Throws `closed` after close() of its system.
   */
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
   * Terminate this interpreter. Its active invocations are cancelled: an
   * in-system SCXML child is destroyed with it, and a JavaScript invoker's
   * service gets cancel().
   */
  destroy(): void;
  [Symbol.dispose](): void;
}

export interface PreparedActor<Data extends object> extends Actor<Data> {
  /** False until System.publish() publishes it. */
  readonly published: boolean;
  /** The actor it replaces, when it was prepared with one. */
  readonly incumbent?: ActorID;
  /** True when it was prepared with `at` or `log` (core ADR 0089, napi ADR 0034). */
  readonly replaying: boolean;
  /**
   * Replay the rest of this actor's input log into it (napi ADR 0034
   * section 3): `inputs` is byte chunks cut anywhere, read a chunk at a
   * time. Core applies one record at a time (core ADR 0090 section 7): the
   * clock moves to its time, the input is taken, and the actor runs until
   * it is idle. Nothing it does crosses the border or is recorded in the
   * input log. Replay works on one actor: there is no replay of a system. A
   * macrostep that faults (a runaway) is followed by the `abandoned` or
   * `released` record the operator's action left. Rejects `invalid` for an
   * actor that is not replaying, a chunk that is not a Uint8Array, a log
   * that ends inside a record, or one that ends this actor's life
   * (`destroyed`); `corrupt` for bytes that are not records this build
   * reads; and what core answers for a record it cannot apply.
   */
  replay(inputs: Iterable<Uint8Array> | AsyncIterable<Uint8Array>): Promise<ReplayReport>;
}

/** What a replay did (napi ADR 0034 section 3). */
export interface ReplayReport {
  /** The records applied. */
  readonly inputs: number;
  /** The events the actor's revision took without a transition (SendResult.consumed false). */
  readonly ignored: readonly { readonly kind: "event"; readonly time: number; readonly name: EventName }[];
  readonly lastTime?: number;
  /** The history reached a final state: the actor takes nothing more, and publish() answers `done`. */
  readonly finished: boolean;
}

/** Where System.setInputLog() hands each turn's records (napi ADRs 0032 to 0034). */
export interface InputLog {
  /**
   * Once per turn with records or wakes, in turn order, one at a time. The
   * turn's services wait until it returns, or the promise it returns
   * resolves. Store the batch in one transaction.
   */
  commit(batch: InputBatch): void | Promise<void>;
}

export interface InputBatch {
  /**
   * Append each entry's `bytes`, one address's records from this turn in
   * core's portable layout, to its `address`'s log, in this order. Never
   * parse them: a loader hands the log back as it is.
   */
  readonly entries: readonly { readonly address: ActorAddress; readonly bytes: Uint8Array }[];
  /**
   * Set each `address`'s wake to `at`, or remove it for null: when napi
   * next needs the actor back (a timer due, or mail it has not taken).
   * After a restart, call System.wakeAt(address, at) for each stored wake.
   */
  readonly wakes: readonly { readonly address: ActorAddress; readonly at: number | null }[];
}

/**
 * One record of an input log as readInputLog() decodes it for an audit
 * trail (napi ADR 0034 section 5): the host kernel's vocabulary (kernel ADR
 * 0002, layout version 2), at logical time `time`, numbered `seq` in its
 * actor's life (no `seq` for one written while the actor was not resident).
 * `accepted` is a message taken into the actor's mailbox, or waiting for its
 * family's loader, with its content, once; its `delivered` (or `jump`)
 * record names the same `id` when it is delivered, its `dropped` record when
 * it never will be. An invocation's error report is an `accepted` message
 * with `report` set. `fired` is a delayed send the actor dispatched. A
 * checkpoint is its `checkpoint` record and the parts after it.
 */
export type InputRecord = { readonly time: number; readonly seq?: string } & (
  | { readonly kind: "spawned"; readonly address: ActorAddress; readonly revision: string; readonly binding?: unknown }
  | {
      readonly kind: "accepted";
      /** The message's identity, 64 bits as decimal text. */
      readonly id: string;
      /** Absent for an invocation's error report, which core names. */
      readonly name?: EventName;
      readonly type: "external" | "internal" | "platform";
      readonly data?: unknown;
      readonly sendID?: string;
      readonly origin?: string;
      readonly originType?: string;
      readonly invokeID?: string;
      /** An invocation's error report, not a message, and whether it ends the invocation. */
      readonly report?: "unavailable" | "quarantined" | "failed";
      readonly reason?: string;
      readonly ends?: boolean;
    }
  | { readonly kind: "delivered" | "dropped"; readonly id: string }
  | {
      /** An operator transition (core ADR 0082) delivering message `id`, by state names; no `source` for the root. */
      readonly kind: "jump";
      readonly id: string;
      readonly source?: string;
      readonly targets: readonly string[];
    }
  | { readonly kind: "withdrawn" | "fired"; readonly sendID: string }
  | { readonly kind: "abandoned" | "released" | "evicted" | "destroyed" }
  | { readonly kind: "finished"; readonly finalState?: string }
  | {
      readonly kind: "checkpoint";
      readonly address: ActorAddress;
      readonly revision: string;
      /** The definition's own state numbers, which its revision fixes. */
      readonly active: readonly number[];
      readonly late: readonly number[];
      readonly history: readonly { readonly history: number; readonly state: number }[];
      readonly data?: unknown;
      readonly [field: string]: unknown;
    }
  | { readonly kind: "checkpoint-send" | "checkpoint-invoke"; readonly [field: string]: unknown }
);

/**
 * Decode an input log (napi ADR 0034 section 5): `chunks`, the bytes a sink
 * stored, cut anywhere, yields its records in order, one at a time.
 * `classes` names the classes the log's values construct, beside the
 * standard ones (napi ADR 0031). For showing an actor's history; replay
 * never needs it.
 */
export declare function readInputLog(
  chunks: Iterable<Uint8Array> | AsyncIterable<Uint8Array>,
  options?: { readonly classes?: Readonly<Record<string, new (...args: any[]) => unknown>> },
): AsyncIterable<InputRecord>;

export type PrepareOptions<Data extends object> = {
  /**
   * Defaults to the incumbent actor's current data-model root, unless `at`
   * is given: a replay starts from the binding it is given and rebuilds the
   * data from its history.
   */
  readonly binding?: Partial<Data> | Data;
  /** A replaying actor, whose clock starts here: no later than the system's time. */
  readonly at?: number;
  /**
   * The start of an input log, holding at least its first record (napi ADR
   * 0034): a replaying actor made from `spawned` (its binding and clock) or
   * a checkpoint (its state). Not with `at` or `binding`.
   */
  readonly log?: Uint8Array;
} & ({ readonly incumbent: ActorID; readonly address?: never } | { readonly address: ActorAddress; readonly incumbent?: never });

/**
 * Core's residency: "idle" between macrosteps, "running" inside one, "done"
 * once finished. An unloaded actor is not resident: inspecting it throws
 * `not-found`.
 */
export type ActorResidency = "idle" | "running" | "done";

/**
 * A snapshot of core's view of one actor (ADR 0054: O(size of the actor)).
 */
export interface ActorInspection<Data = unknown> {
  readonly actor: Actor;
  /**
   * The session ID as text, `<incarnation>.<session>.<family>` (core ADR
   * 0088): what the chart reads as `_sessionid`. Stable across unload and
   * load; the runtime ID is not (core ADR 0066).
   */
  readonly sessionID: string;
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
  /**
   * The invoker's handle for the service. For the in-system SCXML invoker,
   * the child's runtime ID as one integer, (generation << 32) | slot: it
   * matches no actor once the child is unloaded or reconstructed.
   */
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
  /** Who sent it; absent when it has no origin, as after a post() without `origin`. */
  readonly origin?: EventOrigin;
  readonly data?: unknown;
}

/**
 * Who sent an event (core ADR 0068). `type` and `uri` are what the
 * receiver's expressions see as `event.originType` and `event.origin`, so a
 * reply can go to them; `uri` is absent once the origin's scope is closed.
 * `invocation` is the InvocationInspection.id of the invocation whose
 * service sent it; `scope` is the IOScope it was posted through.
 */
export type EventOrigin = { readonly type: string; readonly uri?: string } & (
  | { readonly kind: "actor"; readonly actor: ActorID | ActorAddress }
  | { readonly kind: "invocation"; readonly invocation: InvocationID }
  | { readonly kind: "io"; readonly scope?: { readonly index: number; readonly generation: number } }
);

/** One pending delayed send; `delayedSends` is ordered by due time, then send order. */
export interface DelayedSendInspection {
  /** The send ID as the chart names it (core ADR 0048), or null when it has none. */
  readonly id: string | null;
  readonly due: number;
  readonly event: EventName;
  readonly ioType: string;
  /**
   * The resolved receiver for the built-in SCXML processor: "self" for the
   * actor itself (a send without a target, or to its own session), an
   * address for a named receiver, or an ActorID for an unnamed other actor.
   * null for custom processors, which use `target`.
   */
  readonly destination: "self" | ActorAddress | ActorID | null;
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
 * first; it never outlives its system (ADR 0012). Serializing it across
 * processes is not supported: model network handoff with I/O processors.
 */
export interface SessionImage<Data extends object = Record<string, unknown>> {
  readonly origin: System;
  /** The actor at capture; its ID is stale once the actor is unloaded. */
  readonly actor: ActorID;
  /** The session ID as text (ActorInspection.sessionID); stable across unload and load. */
  readonly sessionID: string;
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
  /**
   * The logical time of the running macrostep, in milliseconds (core ADR
   * 0089 section 9): fixed for the macrostep, however many turns it spans.
   * Read this, not Date.now(), so a chart gives the same answers when its
   * inputs are replayed (napi ADR 0032).
   */
  readonly time: number;
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

export type StringSpec<Data extends object> = string | NamedFunction | Expression<Data, string>;

/**
 * A number is milliseconds, written as `${number}ms` for core, which takes
 * an SCXML duration: a non-negative decimal without an exponent, so a
 * number JavaScript prints with one (1e21, 1e-7) is refused at define(). A
 * function's result is checked only when the send runs; a negative or NaN
 * delay then raises `error.execution`. A named() function's result is the
 * delay as it is, so it returns an SCXML duration ("500ms"), as a delayexpr
 * does.
 */
export type DelaySpec<Data extends object> =
  | number
  | `${number}ms`
  | `${number}s`
  | NamedFunction
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

/* --------------------------------------------------------------------------
 * Statechart specs: the generated shape (ADR 0013)
 *
 * System.define() accepts this shape as a plain object; statechart() emits
 * it; core's definition export returns it. Member names follow core's C
 * spelling, and a tagged union is { kind, <member>: ... }.
 *
 * An expression is a JavaScript function (a condition, value or script,
 * called with `this` bound to the actor's data), a literal() value, or source
 * text: `In(state)`; a data name, which reads that property (ADR 0011);
 * `name()`, a call of a function registered with registerFunction(); or a
 * JSON value, parsed once when the chart is defined (napi ADR 0029). The
 * builder writes plain data as JSON, and named() as `name()`: unlike a
 * function or a literal(), both are content core fingerprints. A
 * location is a data name or a { get, set } object. A spec compiles against
 * the JavaScript data model ("javascript") unless it opts into core's null
 * data model with `datamodel: "null"`. A script, unlike other expressions,
 * must be a function.
 * -------------------------------------------------------------------------- */

/**
 * A function, a literal(), or source text: In(state), a data name,
 * `name()` (a call of a registered function), or a JSON value (napi ADR
 * 0029). Source text
 * that knows where it was written is `{ source, span }`, `span` an index
 * into the spec's `spans` from 1 (core ADR 0085); an imported chart's
 * export() writes it so, and define() keeps the span.
 */
export type ExpressionSpec =
  | string
  | { readonly source: string; readonly span?: number }
  | ((this: any, context: EvaluationContext<any>) => unknown)
  | LiteralValue<unknown>;
/**
 * A script: a function, or the source text `name()`, a call of a
 * registered function (napi ADR 0029). Other source text and literal()
 * compile, but running them fails with an internal `error.execution`.
 */
export type ScriptSpec = ((this: any, context: EvaluationContext<any>) => void) | string;
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
  /**
   * "javascript" when absent. "null" opts into core's null data model: no
   * data, no functions, and content that cannot become a value. Any other
   * name is a data model registerDatamodel() registered, chosen as an
   * imported document chooses it (napi ADR 0027).
   */
  readonly datamodel?: string;
  readonly identity?: {
    readonly family: { readonly name: string };
    readonly revision: string | Uint8Array;
  };
  /** "default" is early binding: inspection reports it as "early". */
  readonly binding?: "default" | "early" | "late";
  readonly initial?: InitialSpec;
  /** Document order is semantically significant. */
  readonly states?: readonly StateSpec[];
  readonly data?: readonly DataSpec[];
  readonly script?: ScriptSpec;
  /** Where the script was written: an index into `spans`, from 1. */
  readonly script_span?: number;
  /**
   * Where elements were written (core ADR 0085): each element's `span` is an
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
  | { readonly kind: "script"; readonly script: { readonly script: ScriptSpec } }
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

/**
 * One per system, except that a request carrying a traceparent gets its
 * own, whose post() carries that traceparent unless given another.
 * `signal` aborts on close().
 */
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
  /**
   * A literal `<invoke src>` as the text the chart wrote, never imported into
   * the data model, so a chart on the null data model can name one; or
   * `srcexpr`'s value.
   */
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
 * invocation and the parent gets error.communication. Every report waits in
 * the parent's mailbox, behind the mail before it, and reaches the parent
 * between macrosteps (napi ADR 0032 section 3). Each returns true when the
 * report was queued, and false once the invocation has ended (done,
 * unavailable, cancelled, closed) or its parent is gone; a queued report for
 * an invocation that ends before it is taken is dropped.
 *
 * A report that could not be taken throws and changes nothing: event() and
 * done() throw `queue-full` when the parent's mailbox is at
 * maxQueuedMessages or maxRetainedBytes, and any report throws
 * `out-of-memory` when the addon or core cannot allocate. The invocation
 * stays alive and its signal unaborted, so the service can report again,
 * for example once the parent has drained its mailbox.
 */
export interface InvocationReporter {
  /** Aborts when the invocation ends: cancel, a taken done() or unavailable(), or close(). */
  readonly signal: AbortSignal;
  event(name: EventName, data?: unknown): boolean;
  /** Ends the invocation once the report is taken. */
  done(data?: unknown): boolean;
  /** Ends the invocation once the report is taken. */
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
  | "busy"
  | "unsupported"
  | "definition-mismatch"
  | "unloaded"
  | "stale"
  | "queue-full"
  | "closed"
  | "reentrant"
  | "aborted"
  | "timeout"
  | "abandoned"
  | "runaway"
  /** A send()'s message became a dead letter with this reason (DeadLetterRecord). */
  | "rejected"
  | "load-failed"
  | "load-timeout"
  | "waiting-full";

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
   * "summary" (default): a span per macrostep, lifecycle spans, failures
   * (`actionFailed`, `stopped`, `longMacrostep`, `runaway`) and an
   * operator's interventions (`abandoned`, `withdrawn`); "decisions": adds
   * microsteps, conditions and raised events; "full": every kind. Watched
   * actors (watchTraces) always get every kind.
   */
  detail?: 'summary' | 'decisions' | 'full';
  /** The record kinds wanted, overriding `detail`. Core builds no other record. */
  kinds?: TraceKind[];
  /**
   * The ratio (0 to 1, default 1) of the traces this system starts that are
   * sampled, decided as OpenTelemetry's TraceIDRatioBased does. A
   * traceparent keeps its caller's decision. Lifecycle spans (spawned,
   * loaded, unloaded, finished, destroyed) are kept whatever the decision.
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
  /**
   * Resource attributes every request carries, such as `service.name`. The
   * system's parameters are added as one attribute, `tinyactors.params`: the
   * defaults' version, then `name=value` for each parameter that differs from
   * its default, in name order (`v1;valueBytes=16384`), or `v1` alone (napi
   * ADR 0036), unless this names the same key.
   */
  resource?: Record<string, string>;
  /**
   * Event data and assigned values as JSON attributes, cut at the
   * `valueBytes` parameter (SystemParameters) and marked
   * `<key>.truncated` when cut.
   */
  values?: boolean;
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

/**
 * A span as decodeTraces() reads it. IDs are lowercase hex. The addon's own
 * spans are all `internal`, with status `unset` or `error` (no message) and
 * links without attributes; the other values come from other producers'
 * bytes.
 */
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
  /**
   * The addon writes one scope per definition (napi ADR 0028), whose
   * attributes name it: `tinyactors.definition.family`, `.revision`,
   * `.id`, and `.name` and `.source` when it has them. Shared by the spans
   * of one scope.
   */
  scope: { name: string; version: string; attributes: Record<string, unknown> };
}

/** Reads OTLP protobuf trace bytes; throws on a malformed message. */
export declare function decodeTraces(bytes: Uint8Array): DecodedSpan[];

/**
 * A W3C `traceparent` header's fields, or undefined when it is not a valid
 * one; parsed by core, as a traceparent given to post(), send() or spawn()
 * is. Spaces and tabs around it are ignored and hex is lowercase. Version
 * ff, an all-zero trace or parent ID, and version 00 with anything after
 * the flags are refused. A later version is read by its first four fields,
 * ignoring what follows a dash after the flags (W3C Trace Context,
 * "Versioning of traceparent").
 */
export declare function parseTraceparent(header: string | undefined):
  { traceID: string; parentSpanID: string; flags: number; sampled: boolean } | undefined;
export declare function formatTraceparent(span: { traceID: string; spanID: string; sampled?: boolean; flags?: number }): string;
/** `session`: the numeric session ID, or a session ID's text (`scxml.session_id`, ActorInspection.sessionID). */
export declare function stepSpanID(session: bigint | number | string, macrostep: bigint | number, microstep?: number): string;

// ---------------------------------------------------------------------------
// The debug adapter (napi ADR 0026)

/**
 * A poke a debug session may be allowed on the live actor: `send` an event,
 * `set` its data model, `goto` (operator transitions, abandon, cancel and
 * replace), and `evaluate` expressions. With none, the session only watches.
 */
export type DebugPermission = 'send' | 'set' | 'goto' | 'evaluate';

/** One record for the audit hook. `time` is milliseconds since the epoch. */
export type DebugAuditEntry = {
  time: number;
  /** DebugSession.id. */
  session: number;
  /** While attached: the actor's label and its session ID. */
  actor?: string;
  /** As text (ActorInspection.sessionID). */
  sessionID?: string;
} & (
  | { type: 'attach'; allow: DebugPermission[]; [detail: string]: unknown }
  | { type: 'detach'; reason: string }
  /** A line breakpoint's source is `path`, or `sourceReference` for SCXML the session wrote. */
  | { type: 'breakpoints'; kind: 'line' | 'function' | 'data' | 'exception'; [detail: string]: unknown }
  /** A console line, before it runs. */
  | { type: 'command'; text: string }
  | { type: 'evaluate'; context: string; expression: string }
  /** A poke: send, set, goto, abandon, cancel, replace. */
  | { type: 'mutation'; operation: string; [detail: string]: unknown }
);

export interface DebugSessionOptions {
  /**
   * The pokes an attach may ask for (default none: watch only).
   *
   * `evaluate` runs arbitrary JavaScript in this process: watch, hover and
   * console expressions, breakpoint conditions and logpoint `{expression}`
   * parts. Grant it only to someone who may run code on the host. Without
   * it the Variables pane, the console commands and setVariable still work.
   * Each evaluation checks the permissions of the attach it runs under: a
   * condition set before an attach that did not keep `evaluate` never runs,
   * and the client hears that breakpoint changed to unverified; a logpoint's
   * parts are then written as they are.
   */
  allow?: DebugPermission[];
  /**
   * Hears every attach, detach, breakpoint change (exception filters
   * included), console command, evaluation and mutation before it happens;
   * throwing refuses it. Evaluations include a breakpoint condition and a
   * logpoint part at each hit (context "condition", "logpoint"), and a
   * setVariable or setExpression value that is not JSON (context
   * "setVariable", "setExpression"), before it runs. A refused condition
   * does not fire, a refused logpoint part is written as `<refused>`, and
   * the console says why. A detach is recorded after it happens and cannot
   * be refused.
   */
  audit?: (entry: DebugAuditEntry) => void;
  /** Takes each outgoing DAP message (an object). */
  send: (message: DAPMessage) => void;
}

/** A Debug Adapter Protocol message: a request, response or event. */
export interface DAPMessage {
  seq: number;
  type: 'request' | 'response' | 'event';
  [field: string]: unknown;
}

/**
 * A DAP session over one attached actor, which it watches and pokes and
 * never stops (ADR 0026): the client hears it stopped on entry, and after
 * each of its macrosteps a stop that keeps the focus and an `invalidated`
 * event for the variables; breakpoints are
 * tracepoints. It opens no socket and checks no credentials: put it behind
 * the application's transport and authentication, and decide each user's
 * permissions before creating it.
 */
export interface DebugSession {
  readonly id: number;
  readonly allow: DebugPermission[];
  /** True while an actor is attached. */
  readonly attached: boolean;
  /**
   * Takes one incoming message. Requests are answered in the order they
   * arrive; the promise settles once this one is answered.
   */
  receive(message: DAPMessage): Promise<void>;
  /** The transport closed: detach without answering. */
  close(): void;
}

export declare function createDebugSession(system: System, options: DebugSessionOptions): DebugSession;

/**
 * The base protocol's framing for a byte stream (stdio, a TCP socket):
 * push() each chunk; `onMessage` gets each whole message in order. Chunks
 * may split a header, a body or a UTF-8 sequence anywhere, and a string
 * chunk may split a surrogate pair. Header fields other than
 * Content-Length are ignored.
 *
 * A malformed stream throws an `invalid` TinyactorsError from push(), and
 * every later push() throws `invalid` too: a stream cannot be
 * resynchronized once a length is in doubt, so close the connection.
 * Malformed is: headers that are not UTF-8, longer than 8 KiB without the
 * blank line, or with a line that is not `Name: value`; no Content-Length,
 * two, or one that is not a decimal number of at most 64 MiB; a body that
 * is not UTF-8 JSON, or not an object with an integer `seq` and a `type`
 * of "request", "response" or "event". An exception from `onMessage`
 * propagates from push() but leaves the reader usable; messages already
 * buffered behind it arrive with the next push().
 */
export declare class DAPReader {
  constructor(onMessage: (message: DAPMessage) => void);
  push(chunk: Uint8Array | string): void;
}

/** One message as bytes: the Content-Length header, then its JSON. */
export declare function encodeDAPMessage(message: object): Uint8Array;
