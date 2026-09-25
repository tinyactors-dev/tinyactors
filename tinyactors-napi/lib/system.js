'use strict';

// System facade over the native System (ADR 0012) and the generated core
// module (ADR 0013). Scheduling is native (ADR 0007): nothing here takes part
// in it; spawn() and destroy() only reach core, whose hooks tell the pump.

const native = require('./native');
const { TinyactorsError } = require('./errors');
const { Definition, identityOf } = require('./definition');
const { DefinitionBuilder, needsJavaScript } = require('./statechart');
const { ActorID } = require('./actor');
const { EventNames } = require('./events');
const { SessionImage, handleOf } = require('./image');
const { traceConfig } = require('./tracing');
const { buildSnapshot } = require('./metrics');

const NO_STATE = 4294967295;

const { core } = native;

// Compiles a spec in the generated shape. A refused chart throws with core's
// compile error formatted into the message (ADR 0058), and the error itself,
// with a readable path, as `detail`.
/* Core's TA_MICROSTEP_LIMIT_NONE: this definition has no limit (ADR 0074). */
const MICROSTEP_LIMIT_NONE = 0xffffffff;

/* `options.microstepLimit`: undefined, or a count from 0 (none) up. */
function microstepLimitOption(options, where) {
  if (options === null || typeof options !== 'object') {
    throw new TinyactorsError(`${where}: options must be an object`, 'invalid');
  }
  const limit = options.microstepLimit;
  if (limit !== undefined && !(Number.isInteger(limit) && limit >= 0 && limit < MICROSTEP_LIMIT_NONE)) {
    throw new TinyactorsError(`${where}: microstepLimit must be a whole number from 0 (none)`, 'invalid');
  }
  return limit;
}

function compile(system, spec) {
  try {
    return core.definition_compile_spec(system, spec);
  } catch (error) {
    const refusal = error instanceof TinyactorsError ? error.detail?.error : undefined;
    if (refusal === undefined) {
      throw error;
    }
    let text;
    try {
      text = core.compile_error_format(spec, refusal);
    } catch {
      text = `${refusal.code}: ${refusal.message}`;
    }
    throw new TinyactorsError(`chart refused: ${text}`, error.code, {
      error: refusal,
      code: refusal.code,
      path: pathText(refusal.path),
    });
  }
}

// `states[0].transitions[1].condition`, from core's path elements.
function pathText(path) {
  const noIndex = 4294967295;
  return (path ?? [])
    .map(({ member, index }, at) => `${at === 0 ? '' : '.'}${member}${index === noIndex ? '' : `[${index}]`}`)
    .join('');
}

// The native System behind a public one, for other lib/ modules and tests.
// index.js does not export it.
let nativeOf;
// handler(system, kind, payload): the pump hands every service effect to it
// (src/napi_services.h). It is one function for the environment, so no
// native reference keeps a System alive.
let effectHandler;

// The SCXML invoker type core normalizes "scxml" and its URI to (core
// ADR 0016); the in-system SCXML invoker is registered under it.
const SCXML_INVOKER = 'http://www.w3.org/TR/scxml/';
const SCXML_ALIASES = new Set(['scxml', 'http://www.w3.org/TR/scxml', SCXML_INVOKER]);

// Systems created in this process, to tell their metric streams apart.
let systemCount = 0;

// core's counters are uint64 (bigints across the binding); as numbers.
function numbers(counters) {
  const out = {};
  for (const [key, value] of Object.entries(counters)) {
    out[key] = Number(value);
  }
  return out;
}

class System {
  #native;
  #events;
  // Definition ID -> true when it runs the JavaScript data model; read from
  // core once per definition.
  #javascript = new Map();

  // Services (ROADMAP step 13): I/O processors by type, invokers by type,
  // live JavaScript invocations by service handle, and the log sink.
  #io = new Map();
  #invokers = new Map();
  #invocations = new Map();
  #logSink;
  #traceSink;
  #traceConfig = {};
  // Active watchTraces() handles, and every definition's identity, for the
  // watches and for metrics().
  #watches = new Set();
  #definitionIdentities = new Map();
  #startTime = Date.now();
  #metricsName = String((systemCount += 1));
  // Aborted on close(): the signal every EffectContext carries.
  #lifetime = new AbortController();
  #effectContext;

  static {
    nativeOf = (system) => system.#native;
    effectHandler = (system, kind, payload) => system.#effect(kind, payload);
  }

  /**
   * Options:
   * - scheduling: { resumeBudget, actionBudget, resumesPerTurn } (ADR 0007);
   * - finished: "destroy" (the default) or "retain" (ADR 0012);
   * - scheduling.messageBudget: deliveries per turn, and dead letters
   *   reported per turn; scheduling.maxQueuedMessages: post() throws
   *   `queue-full` beyond it (ADR 0005);
   * - done({ actor, finalState, data }), fault({ actor, operation, code,
   *   message }) and deadLetter({ target, event, reason }): hooks the pump
   *   calls at the end of a turn (ADRs 0008, 0012).
   */
  constructor(options) {
    if (options !== undefined && (options === null || typeof options !== 'object')) {
      throw new TinyactorsError('createSystem options must be an object', 'invalid');
    }
    this.#native = new native.System(this, options);
    this.#events = new EventNames(this.#native);
    const { io, invokers, log } = options ?? {};
    for (const [type, processor] of Object.entries(servicesOf(io, 'createSystem(): io'))) {
      this.registerIO(type, processor, { location: processor?.location, origin: processor?.origin });
    }
    for (const [type, invoker] of Object.entries(servicesOf(invokers, 'createSystem(): invokers'))) {
      this.registerInvoker(type, invoker);
    }
    if (log !== undefined) {
      this.setLogSink(log);
    }
  }

  /**
   * Make `processor` the I/O processor for `<send type>` (core ADRs 0015,
   * 0046). Core calls it while an actor runs, so the call is queued and
   * processor.send(request, context) runs at the end of that pump turn,
   * never inside core (ADR 0001 rule 3). `request` is { source, sendID,
   * event, type, target, data } (and { content, contentType } for a literal
   * <content>); `context` is { system, signal, post }, where `signal` aborts
   * on close(). A reply is an event the processor posts, now or later. A
   * processor that throws or whose promise rejects has failed the send: the
   * sender gets an external `error.communication` event whose data is
   * { sendID, type, error }.
   *
   * `options` says how core renders the processor's text without running
   * JavaScript (core ADR 0068):
   * - `location`: this session's address through the processor, listed in
   *   `_ioprocessors` (`context.ioProcessors`). `{session}` is the session
   *   ID, `{family}` and `{name}` its named address.
   * - `origin`: the URI of a computed origin (post with `origin: { type,
   *   token }`); `{token}` is the token.
   * A processor posts events with an origin so a chart can answer them
   * with `send(...).via(({ event }) => event.originType).to(({ event }) => event.origin)`: see post().
   */
  registerIO(type, processor, options = {}) {
    if (typeof type !== 'string' || type === '') {
      throw new TinyactorsError('registerIO(): expected a type string', 'invalid');
    }
    if (processor === null || typeof processor !== 'object' || typeof processor.send !== 'function') {
      throw new TinyactorsError('registerIO(): expected an I/O processor with a send(request, context) method', 'invalid');
    }
    const { location, origin } = options ?? {};
    for (const [name, value] of [['location', location], ['origin', origin]]) {
      if (value !== undefined && typeof value !== 'string') {
        throw new TinyactorsError(`registerIO(): options.${name} must be a string template`, 'invalid');
      }
    }
    this.#native.registerIO(type, location, origin);
    this.#io.set(type, processor);
  }

  /**
   * Open a scope for a connection or a session of processor `type`, storing
   * `uri` once (core ADR 0068). Events posted with `origin: { type, scope }`
   * name it; after closeScope() their origin is empty, so a late reply
   * reaches the processor with no target.
   */
  openScope(type, uri) {
    if (typeof uri !== 'string' || uri === '') {
      throw new TinyactorsError('openScope(): expected a URI string', 'invalid');
    }
    const { index, generation } = this.#native.openScope(type, uri);
    return Object.freeze({ type, uri, index, generation });
  }

  /** Close a scope from openScope(). Closing it again does nothing. */
  closeScope(scope) {
    if (scope === null || typeof scope !== 'object') {
      throw new TinyactorsError('closeScope(): expected a scope from openScope()', 'invalid');
    }
    this.#native.closeScope(scope.index, scope.generation);
  }

  /**
   * Make `invoker` the invoker for `<invoke type>` (core ADR 0016). Core
   * starts, cancels and forwards to invocations while an actor runs, so each
   * call is queued and runs at the end of that pump turn:
   * invoker.start(request, reporter) returns the service, whose cancel() runs
   * when the invoking state exits and whose optional forward(event) gets
   * autoforwarded events. The reporter's event(name, data) and done(data)
   * reach the parent as external events (done is done.invoke.<id>); its
   * unavailable(error) retires the invocation, and the parent gets
   * error.communication. Its signal aborts on cancel, done, unavailable and
   * close(). "scxml" names the in-system SCXML invoker's type; registering
   * it replaces that invoker.
   */
  registerInvoker(type, invoker) {
    if (typeof type !== 'string' || type === '') {
      throw new TinyactorsError('registerInvoker(): expected a type string', 'invalid');
    }
    if (invoker === null || typeof invoker !== 'object' || typeof invoker.start !== 'function') {
      throw new TinyactorsError('registerInvoker(): expected an invoker with a start(request, reporter) method', 'invalid');
    }
    const canonical = SCXML_ALIASES.has(type) ? SCXML_INVOKER : type;
    this.#native.registerInvoker(canonical);
    this.#invokers.set(canonical, invoker);
  }

  /**
   * Send `<log>` records to `sink` ({ source, label, hasValue, value }), at
   * the end of the pump turn that ran them (core ADR 0045). Without a sink a
   * <log> only evaluates its expression.
   */
  setLogSink(sink) {
    if (sink !== undefined && typeof sink !== 'function') {
      throw new TinyactorsError('setLogSink(): expected a function or undefined', 'invalid');
    }
    this.#native.setLogSink(sink !== undefined);
    this.#logSink = sink;
  }

  /**
   * Hand traces and logs to `sink(traces, logs)` as OTLP protobuf (napi
   * ADRs 0016 and 0017): once per pump turn, `traces` is one Uint8Array
   * holding one ExportTraceServiceRequest with the spans that ended in that
   * turn (empty when none ended), and `logs` one ExportLogsServiceRequest
   * with the turn's log records, or undefined. Post them to an OTLP/HTTP
   * endpoint's `/v1/traces` and `/v1/logs` as they are (Content-Type:
   * application/x-protobuf), or decode traces with decodeTraces(). Logs are
   * failures (an action that failed, an error.* raised, a run stopped for
   * lack of memory) and <log>, written whatever the sampling, at most
   * `logLimit` (default 20) per definition and name per second; the next
   * log that passes carries `tinyactors.suppressed`. `logs: false` writes
   * none. Without a sink (undefined) core builds no record.
   *
   * options:
   * - detail: "summary" (default: a span per macrostep, lifecycle, and
   *   failures), "decisions" (adds microsteps, conditions, raised events)
   *   or "full" (every kind); see TRACE_DETAIL. Watched actors
   *   (watchTraces) always get every kind;
   * - kinds: the record kinds wanted (TRACE_KINDS names), overriding
   *   `detail`: core builds no other record;
   * - sample: the ratio of the traces this system starts that are sampled,
   *   0 to 1 (default 1), decided as OpenTelemetry's TraceIDRatioBased
   *   does (core ADR 0073); a traceparent keeps its caller's decision;
   * - unsampled: also trace macrosteps whose traceparent is not sampled;
   * - resource: resource attributes, { "service.name": "orders", ... },
   *   strings only;
   * - values: put event data and assigned values in attributes, as JSON
   *   cut at `valueLimit` characters (default 256, at most 1023). Off by
   *   default: values may hold personal data;
   * - parent: "cause" (default) parents a macrostep under the step that
   *   caused it; "link" makes it a root and links it to its cause;
   * - seed: a bigint; trace IDs the system starts (for deliveries and
   *   spawns without a traceparent) become a fixed sequence, for tests.
   *
   * Replacing or removing a sink ends the spans still open, marked
   * `scxml.incomplete`, and hands them to the previous sink first.
   */
  setTraceSink(sink, options = {}) {
    if (sink !== undefined && typeof sink !== 'function') {
      throw new TinyactorsError('setTraceSink(): expected a function or undefined', 'invalid');
    }
    const { unsampled = false, resource = {}, values = false, valueLimit = 256, parent = 'cause', seed,
            logs = true, logLimit = 20 } = options ?? {};
    if (!Number.isInteger(logLimit) || logLimit < 1 || logLimit > 1e6) {
      throw new TinyactorsError('setTraceSink(): options.logLimit must be an integer from 1 to 1000000', 'invalid');
    }
    const config = traceConfig(options ?? {}, 'setTraceSink()');
    if (parent !== 'cause' && parent !== 'link') {
      throw new TinyactorsError('setTraceSink(): options.parent must be "cause" or "link"', 'invalid');
    }
    if (!Number.isInteger(valueLimit) || valueLimit < 0 || valueLimit > 1023) {
      throw new TinyactorsError('setTraceSink(): options.valueLimit must be an integer from 0 to 1023', 'invalid');
    }
    if (resource === null || typeof resource !== 'object') {
      throw new TinyactorsError('setTraceSink(): options.resource must be an object of strings', 'invalid');
    }
    const pairs = [];
    for (const [key, value] of Object.entries(resource)) {
      if (typeof value !== 'string') {
        throw new TinyactorsError(`setTraceSink(): resource attribute "${key}" must be a string`, 'invalid');
      }
      pairs.push(key, value);
    }
    const previous = this.#traceSink;
    const remaining = this.#native.setTraceSink(sink !== undefined, config.kinds, config.watchedKinds,
                                                unsampled === true ? config.kinds : 0, config.sample,
                                                values === true, valueLimit, parent === 'link', pairs, seed,
                                                logs === false ? 0 : logLimit);
    this.#traceSink = sink;
    this.#traceConfig = { detail: options?.detail, kinds: options?.kinds, sample: options?.sample,
                          unsampled: unsampled === true };
    if (remaining !== undefined) {
      previous?.(remaining.traces, remaining.logs);
    }
  }

  /**
   * Changes what the installed sink gets, without reinstalling it, so open
   * spans are not ended (napi ADR 0017 section 3). `options` takes
   * `detail`, `kinds` and `sample` as setTraceSink() does; what it leaves
   * out keeps its current value.
   */
  configureTraces(options = {}) {
    if (options === null || typeof options !== 'object') {
      throw new TinyactorsError('configureTraces(): expected an options object', 'invalid');
    }
    const merged = { ...this.#traceConfig };
    for (const key of ['detail', 'kinds', 'sample']) {
      if (key in options) {
        merged[key] = options[key];
      }
    }
    if ('detail' in options && !('kinds' in options)) {
      merged.kinds = undefined;
    }
    const config = traceConfig(merged, 'configureTraces()');
    this.#native.configureTraces(config.kinds, config.watchedKinds, merged.unsampled ? config.kinds : 0,
                                 config.sample);
    this.#traceConfig = merged;
  }

  /**
   * Traces what matches in full, sampled whatever the ratio, until the
   * returned handle's stop() (napi ADR 0017 section 3): deliveries to the
   * actors of `sessions` (session IDs or ActorIDs), to the named actors of
   * `addresses` ({ family, name }, resolved to their sessions now), and to
   * the actors of the definitions of `families` (family names; spawns of
   * those families are watched from their bootstrap). Everything a watched
   * delivery causes in this system is watched too. It needs a sink.
   */
  watchTraces({ sessions = [], families = [], addresses = [] } = {}) {
    const watch = { sessions: new Set(), families: new Set(families.map(String)) };
    for (const session of sessions) {
      watch.sessions.add(session instanceof ActorID ? BigInt(this.inspect(session).sessionID) : BigInt(session));
    }
    for (const address of addresses) {
      const actor = this.findActor(address);
      if (actor === undefined) {
        throw new TinyactorsError('watchTraces(): no resident actor has that address', 'not-found');
      }
      watch.sessions.add(BigInt(this.inspect(actor).sessionID));
    }
    this.#watches.add(watch);
    this.#applyWatches();
    return Object.freeze({
      stop: () => {
        if (this.#watches.delete(watch) && !this.closed) {
          this.#applyWatches();
        }
      },
    });
  }

  #watchedFamily(family) {
    for (const watch of this.#watches) {
      if (watch.families.has(family)) {
        return true;
      }
    }
    return false;
  }

  #applyWatches() {
    const sessions = new Set();
    const families = new Set();
    for (const watch of this.#watches) {
      watch.sessions.forEach((session) => sessions.add(session));
      watch.families.forEach((family) => families.add(family));
    }
    const definitions = [];
    for (const [id, { family }] of this.#definitionIdentities) {
      if (families.has(family)) {
        definitions.push(id);
      }
    }
    this.#native.setTraceWatch([...sessions], definitions);
  }

  // One service effect, in the order core made the calls (napi_services.h).
  #effect(kind, payload) {
    switch (kind) {
      case 'log':
        this.#logSink?.(payload);
        return;
      case 'trace':
        this.#traceSink?.(payload.traces, payload.logs);
        return;
      case 'io':
        this.#runIO(payload);
        return;
      case 'invoke-start':
        this.#startInvocation(payload);
        return;
      case 'invoke-cancel':
        this.#cancelInvocation(payload.handle, undefined);
        return;
      case 'invoke-forward':
        this.#forwardInvocation(payload);
        return;
      default:
        return;
    }
  }

  // The context every I/O processor call gets; made once per system.
  #context() {
    this.#effectContext ??= Object.freeze({
      system: this,
      signal: this.#lifetime.signal,
      post: (target, event, data, options) => this.post(target, event, data, options),
    });
    return this.#effectContext;
  }

  #replyContext(traceparent) {
    const base = this.#context();
    return Object.freeze({
      system: base.system,
      signal: base.signal,
      post: (target, event, data, options) =>
        this.post(target, event, data, options?.traceparent === undefined ? { ...options, traceparent } : options),
    });
  }

  #runIO(payload) {
    // payload.traceparent names the sending step, for the processor to carry along (ADR 0015).
    const request = payload;
    const processor = this.#io.get(request.type);
    if (processor === undefined) {
      this.#communicationError(request, new TinyactorsError(`no I/O processor for "${request.type}"`, 'not-found'));
      return;
    }
    // A reply the processor posts stays in the trace of the sending step
    // unless it names another (napi ADR 0017 section 6).
    const context = request.traceparent === undefined ? this.#context() : this.#replyContext(request.traceparent);
    let outcome;
    try {
      outcome = processor.send(request, context);
    } catch (error) {
      this.#communicationError(request, error);
      return;
    }
    if (outcome !== null && typeof outcome === 'object' && typeof outcome.then === 'function') {
      outcome.then(undefined, (error) => this.#communicationError(request, error));
    }
  }

  // A failed send reaches its sender as error.communication (ADR 0008:
  // failures after the call reach JavaScript through events or hooks).
  #communicationError(request, error) {
    if (this.closed || request.source === undefined) {
      return;
    }
    try {
      this.post(request.source, 'error.communication', { sendID: request.sendID, type: request.type, error });
    } catch {
      // The sender is gone or its mailbox is full: nobody is left to tell.
    }
  }

  #startInvocation(payload) {
    const { handle, parent, invocationID } = payload;
    const controller = new AbortController();
    const invocation = { controller, service: undefined, live: true };
    this.#invocations.set(handle, invocation);
    const report = (kind, name, data) => {
      if (!invocation.live || this.closed) {
        return false;
      }
      if (kind !== 'event') {
        this.#endInvocation(handle, invocation);
      }
      return this.#native.reportInvoke(parent.slot, parent.generation, invocationID, kind, name, data,
                                       payload.traceparent);
    };
    const reporter = Object.freeze({
      signal: controller.signal,
      event: (name, data) => report('event', this.#events.id(name), data),
      done: (data) => report('done', 0, data),
      unavailable: (error) => report('unavailable', 0, error),
    });
    const invoker = this.#invokers.get(payload.type);
    if (invoker === undefined) {
      reporter.unavailable(new TinyactorsError(`no invoker for "${payload.type}"`, 'not-found'));
      return;
    }
    const request = Object.freeze({
      system: this,
      parent,
      invocationID,
      invokeID: payload.invokeID,
      type: payload.type,
      source: payload.source,
      content: payload.content,
      contentType: payload.contentType,
      params: payload.params,
      autoforward: payload.autoforward,
      // The invoking step, for a service that starts work elsewhere (ADR 0015).
      traceparent: payload.traceparent,
    });
    try {
      invocation.service = invoker.start(request, reporter);
    } catch (error) {
      reporter.unavailable(error);
    }
  }

  #endInvocation(handle, invocation) {
    invocation.live = false;
    this.#invocations.delete(handle);
    invocation.controller.abort();
  }

  #cancelInvocation(handle, reason) {
    const invocation = this.#invocations.get(handle);
    if (invocation === undefined) {
      return;
    }
    this.#endInvocation(handle, invocation);
    try {
      const outcome = invocation.service?.cancel?.(reason);
      if (outcome !== null && typeof outcome === 'object' && typeof outcome.then === 'function') {
        outcome.then(undefined, () => {});
      }
    } catch {
      // A service that fails to cancel is gone as far as the actor is concerned.
    }
  }

  #forwardInvocation({ handle, event }) {
    const invocation = this.#invocations.get(handle);
    if (invocation === undefined || typeof invocation.service?.forward !== 'function') {
      return;
    }
    try {
      const outcome = invocation.service.forward(event);
      if (outcome !== null && typeof outcome === 'object' && typeof outcome.then === 'function') {
        outcome.then(undefined, () => {});
      }
    } catch {
      // Forwarding is best effort (SCXML 6.4: autoforward has no error path).
    }
  }

  /** True once close() has run. */
  get closed() {
    return this.#native.closed;
  }

  /** "realtime" (the default) or "manual" (ADR 0007 point 3). */
  get clock() {
    return this.#native.clock;
  }

  /**
   * Core's logical time in milliseconds. With the realtime clock it follows
   * a monotonic clock read natively, relative to system creation plus
   * `initialTime`.
   */
  get time() {
    return this.#native.time;
  }

  /**
   * Manual clock only: move time to `time` (never backwards) and resolve once
   * everything due has been dispatched and the system is quiet.
   */
  advanceTo(time) {
    return this.#native.advanceTo(time);
  }

  /** Manual clock only: advanceTo(time + milliseconds). */
  advanceBy(milliseconds) {
    if (typeof milliseconds !== 'number' || !(milliseconds >= 0)) {
      throw new TinyactorsError('advanceBy(): expected a non-negative number of milliseconds', 'invalid');
    }
    return this.#native.advanceTo(this.#native.time + milliseconds);
  }

  /**
   * Compile and register a chart: a statechart() builder, or a spec in the
   * generated shape (the shape core's definition export returns). A chart
   * with data, a script, or a JavaScript function, literal() or location
   * anywhere compiles against the JavaScript data model ("javascript", ADR
   * 0011); any other chart against core's null data model ("null"), which
   * costs nothing per actor. A spec's `datamodel` chooses explicitly.
   *
   * `options.microstepLimit` overrides the system's
   * `scheduling.microstepLimit` for this definition's actors (core ADR
   * 0074): the microsteps one macrostep may run before its actor is
   * quarantined as a runaway; 0 for no limit.
   */
  define(source, options = {}) {
    const limit = microstepLimitOption(options, 'define()');
    let spec = source;
    if (source instanceof DefinitionBuilder) {
      spec = source.build();
    } else if (source === null || typeof source !== 'object') {
      throw new TinyactorsError('define(): expected a statechart() builder or a spec object', 'invalid');
    }
    const datamodel = spec.datamodel ?? (needsJavaScript(spec) ? 'javascript' : 'null');
    this.#native.selectDatamodel(datamodel === 'null' ? 'null' : 'javascript');
    const compiled = compile(this.#native, spec);
    const id = core.system_register_definition(this.#native, compiled.out);
    if (limit !== undefined) {
      core.definition_set_microstep_limit(this.#native, id, limit === 0 ? MICROSTEP_LIMIT_NONE : limit);
    }
    const definition = new Definition(this, this.#native, id);
    const identity = definition.identity;
    this.#definitionIdentities.set(id, identity);
    if (this.#watchedFamily(identity.family)) {
      this.#applyWatches();
    }
    return definition;
  }

  /**
   * The newest registered, unretired definition with this identity, or
   * undefined.
   */
  findDefinition(identity) {
    if (identity === null || typeof identity !== 'object') {
      throw new TinyactorsError('findDefinition(): expected { family, revision }', 'invalid');
    }
    try {
      const id = core.system_find_definition(this.#native, {
        family: { name: identity.family },
        revision: identity.revision,
      });
      return new Definition(this, this.#native, id);
    } catch (error) {
      if (error instanceof TinyactorsError && error.code === 'not-found') {
        return undefined;
      }
      throw error;
    }
  }

  /**
   * Create an actor of `definition`. It is bootstrapped by a later pump turn,
   * never inside this call (ADR 0007); `await system.flush()` to see it in its
   * initial state.
   */
  spawn(definition, options = {}) {
    if (!(definition instanceof Definition) || definition.system !== this) {
      throw new TinyactorsError('spawn(): expected a Definition of this system', 'invalid');
    }
    let address;
    if (options.address !== undefined) {
      address = { family: { name: options.address.family }, name: options.address.name };
    } else if (options.name !== undefined) {
      address = { family: { name: definition.identity.family }, name: options.name };
    }
    const watched = this.#watches.size !== 0 && this.#watchedFamily(definition.identity.family);
    const trace = this.#native.spawnTrace(options.traceparent, watched);
    const id = core.actor_spawn(this.#native, definition.id, address, options.binding, undefined, trace);
    return new ActorID(this, id.slot, id.generation);
  }

  /**
   * The resident actor the directory has at `address` ({ family, name }), or
   * undefined (ADR 0006).
   */
  findActor(address) {
    const { family, name } = addressOf(address, 'findActor()');
    try {
      const id = core.actor_find(this.#native, { family: { name: family }, name });
      return new ActorID(this, id.slot, id.generation);
    } catch (error) {
      if (error instanceof TinyactorsError && (error.code === 'not-found' || error.code === 'unloaded')) {
        return undefined;
      }
      throw error;
    }
  }

  /** A snapshot of core's view of `actor` (ADR 0054: O(size of the actor)). */
  inspect(actor) {
    const { slot, generation } = actorOf(this, actor);
    const view = core.actor_inspect(this.#native, slot, generation);
    const stateName = (state) => core.definition_state(this.#native, view.definition_id, state).id;
    const inspection = {
      actor: new ActorID(this, view.actor.slot, view.actor.generation),
      sessionID: view.session_id,
      definition: identityOf(view.definition),
      residency: view.residency,
      published: view.published,
      activeStates: core.actor_active_states(this.#native, slot, generation).map(stateName),
      macrostepInProgress: view.macrostep_in_progress,
      internalEventCount: view.internal_events.length,
      delayedSendCount: view.delayed_sends.length,
      delayedSends: view.delayed_sends.map((send) => this.#delayedSend(view.definition_id, send)),
      scheduling: this.#native.scheduling(slot, generation),
      // The top-level final it reached, or null (core ADR 0056).
      finalState: view.final_state === NO_STATE ? null : stateName(view.final_state),
      allocatedBytes: view.allocated_bytes,
      // The live data object (JavaScript data model), shared, not copied.
      data: this.#dataRoot(view.definition_id, slot, generation),
      // A finished actor's completion data (<donedata>).
      completion: view.has_completion && view.completion?.has_data ? view.completion.data : undefined,
    };
    return inspection;
  }

  // The actor's data root, or undefined under the null data model.
  #dataRoot(definitionID, slot, generation) {
    return this.#usesJavaScript(definitionID) ? core.actor_datamodel_root(this.#native, slot, generation) : undefined;
  }

  // One pending delayed send, from core's ta_delayed_send_info (core ADR 0054).
  // A literal target is text in the owner's definition, named by content ID.
  #delayedSend(definitionID, send) {
    let id = null;
    try {
      id = core.send_id_format(this.#native, send.send_id);
    } catch (error) {
      if (!(error instanceof TinyactorsError && error.code === 'not-found')) {
        throw error;
      }
    }
    return {
      id,
      due: send.due,
      event: this.#events.name(send.event),
      ioType: core.system_atom_text(this.#native, send.io_type),
      destination: this.#destination(send.destination),
      target: send.has_target_text
        ? new TextDecoder().decode(core.definition_content(this.#native, definitionID, send.target_text).bytes)
        : send.target,
      data: send.data,
      submitted: send.submitted,
    };
  }

  // A resolved destination: an ActorID, an address, or null for a custom
  // I/O processor, whose destination core leaves as runtime ID 0.
  #destination(ref) {
    if (ref === undefined || ref === null) {
      return null;
    }
    if (ref.kind === 'named') {
      return ref.named === undefined ? null : { family: ref.named.family.name, name: ref.named.name };
    }
    const { slot, generation } = ref.runtime;
    return generation === 0 ? null : new ActorID(this, slot, generation);
  }

  /**
   * The resident actors as pages of IDs (ADR 0003): each page is a
   * Uint32Array of interleaved [slot, generation] pairs, with no object per
   * actor. Pages come from core's walk in slot order and continue from its
   * cursor, so a whole iteration is linear and holds one page at a time. A
   * page is a snapshot: an actor resident for the whole iteration appears
   * exactly once, one destroyed before the walk reaches it does not appear,
   * and one spawned meanwhile appears when its slot is ahead of the cursor
   * (core ADR 0062). Options: { pageSize } (default 1024, at most 65536).
   */
  *actorIDs(options = {}) {
    const pageSize = pageSizeOf(options, 'actorIDs()');
    let from = 0;
    while (from !== null) {
      const page = this.#native.idPage(from, pageSize);
      if (page.ids.length > 0) {
        yield page.ids;
      }
      from = page.next;
    }
  }

  /**
   * The resident actors' inspections, one page of IDs at a time (ADR 0003).
   * Each actor is inspected when the iterator reaches it, with inspect(), so
   * at most one page of IDs and one inspection are held at a time. An actor
   * destroyed after its page was taken, and before it was reached, is
   * skipped. Options: { pageSize } (default 1024, at most 65536).
   */
  *actors(options = {}) {
    for (const ids of this.actorIDs(options)) {
      for (let at = 0; at < ids.length; at += 2) {
        let inspection;
        try {
          inspection = this.inspect(new ActorID(this, ids[at], ids[at + 1]));
        } catch (error) {
          if (error instanceof TinyactorsError && error.code === 'not-found') {
            continue;
          }
          throw error;
        }
        yield inspection;
      }
    }
  }

  /** True when a resident actor has this ID. */
  exists(actor) {
    const { slot, generation } = actorOf(this, actor);
    try {
      core.actor_inspect(this.#native, slot, generation);
      return true;
    } catch (error) {
      if (error instanceof TinyactorsError && error.code === 'not-found') {
        return false;
      }
      throw error;
    }
  }

  /** Terminate `actor`. Throws `not-found` for an actor that is gone. */
  destroy(actor) {
    const { slot, generation } = actorOf(this, actor);
    core.actor_destroy(this.#native, slot, generation);
  }

  /**
   * Queue `event` for `target` and return (ADR 0005). Nothing runs inside
   * this call: a later pump turn delivers it, in the order posts to that
   * target were accepted. Mail for an actor that is gone or finished is a
   * dead letter (the `deadLetter` hook), not an error. `target` is an
   * ActorID or an address { family, name }, resolved through the directory
   * when the post is accepted (ADR 0006); a name with no resident actor is a
   * dead letter too. `data` is any JavaScript value, shared with the
   * receiver, not copied (ADR 0011); it is `_event.data`
   * (`context.event.data`) in the receiver's expressions. Throws
   * `queue-full` when scheduling.maxQueuedMessages or maxRetainedBytes would
   * be passed: post() has no promise to report it through (ADR 0008).
   */
  post(target, event, data, options) {
    if (options?.signal?.aborted) {
      throw new TinyactorsError('post(): the signal is already aborted', 'aborted');
    }
    this.#accept(target, event, data, options, false, 'post()');
  }

  /**
   * Queue `event` for `target` and return a promise (ADR 0005 point 11).
   * With `until: "completed"` (the default) it resolves with
   * { actor, status: "stable" | "done", completion } once the macrostep this
   * message starts has ended: the actor is stable again, or it finished
   * (`completion` is its done data). It rejects when the message becomes a
   * dead letter (`not-found`, `done`, `unloaded`, `invalid`), when the
   * macrostep faults (the fault's code; the actor is quarantined), on
   * `options.signal` (`aborted`), after `options.timeout` milliseconds
   * (`timeout`), and on close() (`closed`). Aborting or timing out withdraws
   * the message if it is still queued; a message the actor already took is
   * not undone. With `until: "accepted"` it resolves with
   * { actor, status: "accepted" } as soon as the message is queued.
   * A full queue rejects with `queue-full`; only misuse throws (ADR 0008).
   */
  send(target, event, data, options = {}) {
    const until = options.until ?? 'completed';
    if (until !== 'completed' && until !== 'accepted') {
      throw new TinyactorsError('send(): options.until must be "completed" or "accepted"', 'invalid');
    }
    const timeout = options.timeout;
    if (timeout !== undefined && !(typeof timeout === 'number' && timeout >= 0 && Number.isFinite(timeout))) {
      throw new TinyactorsError('send(): options.timeout must be a non-negative number of milliseconds', 'invalid');
    }
    const signal = options.signal;
    if (signal?.aborted) {
      // Validate the call first: misuse throws even with an aborted signal.
      if (!(target instanceof ActorID)) addressOf(target, 'send()');
      else actorOf(this, target);
      return Promise.reject(new TinyactorsError('send(): the signal is already aborted', 'aborted'));
    }
    let promise;
    try {
      promise = this.#accept(target, event, data, options, until === 'completed', 'send()');
    } catch (error) {
      if (error instanceof TinyactorsError && (error.code === 'queue-full' || error.code === 'out-of-memory')) {
        return Promise.reject(error);
      }
      throw error;
    }
    if (until === 'accepted') {
      return Promise.resolve({ actor: target instanceof ActorID ? target : undefined, status: 'accepted' });
    }
    if (signal === undefined && timeout === undefined) {
      return promise;
    }
    const native = this.#native;
    const token = native.lastSendToken;
    const cancel = (code) => {
      try {
        if (!native.closed) native.cancelSend(token, code);
      } catch {
        // The system closed in the meantime; close() settled the promise.
      }
    };
    const onAbort = () => cancel('aborted');
    signal?.addEventListener('abort', onAbort, { once: true });
    const timer = timeout === undefined ? undefined : setTimeout(() => cancel('timeout'), timeout);
    timer?.unref?.();
    const cleanup = () => {
      signal?.removeEventListener('abort', onAbort);
      if (timer !== undefined) clearTimeout(timer);
    };
    return promise.then(
      (result) => {
        cleanup();
        return result;
      },
      (error) => {
        cleanup();
        throw error;
      },
    );
  }

  // Accepts one message for post() or send(); returns send()'s native promise.
  #accept(target, event, data, options, completion, what) {
    const bytes = retainedBytes(data, options, what);
    const origin = originOf(options?.origin, what);
    const trace = options?.traceparent;
    if (!(target instanceof ActorID)) {
      if (target !== null && typeof target === 'object' && 'name' in target) {
        const { family, name } = addressOf(target, what);
        const id = this.#events.id(event);
        return completion
          ? this.#native.sendNamed(family, name, id, data, bytes, origin, trace)
          : this.#native.postNamed(family, name, id, data, bytes, origin, trace);
      }
      throw new TinyactorsError(`${what}: expected an actor of this system or an address`, 'invalid');
    }
    const { slot, generation } = actorOf(this, target);
    const id = this.#events.id(event);
    return completion
      ? this.#native.send(slot, generation, id, data, bytes, origin, trace)
      : this.#native.post(slot, generation, id, data, bytes, origin, trace);
  }

  /*
   * Persistence and replacement (ROADMAP step 14; core ADRs 0013, 0014,
   * 0043, 0051, 0052, 0066).
   */

  /**
   * Capture `actor` in memory: core's session image, plus the data-model
   * root it has now (core's image does not hold it). A macrostep in
   * progress is captured too; an unpublished (prepared) actor cannot be
   * captured (`busy`).
   */
  capture(actor) {
    const { slot, generation } = actorOf(this, actor);
    const view = core.actor_inspect(this.#native, slot, generation);
    const handle = core.actor_capture(this.#native, slot, generation);
    const self = this.#destination(view.self);
    return new SessionImage(this, handle, {
      actor: new ActorID(this, slot, generation),
      sessionID: view.session_id,
      definition: identityOf(view.definition),
      address: self instanceof ActorID || self === null ? undefined : self,
      data: this.#dataRoot(view.definition_id, slot, generation),
    });
  }

  /**
   * Evict a named actor (core ADR 0014): `image` must be a capture of it
   * taken since its last change (`stale` otherwise). The directory keeps its
   * address as unloaded, so mail to the address becomes a dead letter with
   * reason `unloaded` until load() brings it back; its delayed sends travel
   * in the image. Throws `busy` while the actor has mail waiting, is inside
   * a macrostep, or is quarantined, so unloading never drops accepted mail.
   * Its ActorID is stale afterwards.
   */
  unload(actor, image) {
    const { slot, generation } = actorOf(this, actor);
    const handle = handleOf(image, this, 'unload()');
    const scheduling = this.#native.scheduling(slot, generation);
    if (scheduling !== 'idle' && scheduling !== 'none') {
      throw new TinyactorsError(`unload(): the actor is ${scheduling}; unload it once it is idle`, 'busy');
    }
    core.actor_unload(this.#native, slot, generation, handle);
  }

  /**
   * Bring an unloaded named actor back from its image (core ADR 0051): the
   * same session ID, a new runtime ID (core ADR 0066), and the address
   * resident again. `binding` defaults to the data root the image kept. An
   * invocation that was active when it was captured does not come back: the
   * actor gets `error.communication.invoke.<id>.unavailable` for it (core
   * ADR 0038). The image stays the caller's to destroy.
   */
  load(image, options = {}) {
    const handle = handleOf(image, this, 'load()');
    const id = core.actor_load(this.#native, handle, this.#bindingFor(image, options));
    return new ActorID(this, id.slot, id.generation);
  }

  /**
   * Restore an image under a fresh runtime ID without touching the
   * directory (core ADR 0052). It keeps the session ID unless a resident
   * session holds it (core ADR 0066). `binding` defaults to the data root
   * the image kept, which is shared with the captured actor if that still
   * runs; pass one for an independent copy.
   */
  reconstruct(image, options = {}) {
    const handle = handleOf(image, this, 'reconstruct()');
    const id = core.actor_reconstruct(this.#native, handle, this.#bindingFor(image, options));
    return new ActorID(this, id.slot, id.generation);
  }

  /** Release the address of an actor that stays unloaded; its image is the caller's to destroy. */
  removeUnloaded(address) {
    const { family, name } = addressOf(address, 'removeUnloaded()');
    core.actor_remove_unloaded(this.#native, { family: { name: family }, name });
  }

  /**
   * Create a fresh, unpublished actor of `definition` to replace the named
   * actor `incumbent` in place (core ADR 0043; docs/design.md). It
   * bootstraps like a spawned actor, and core holds its visible bootstrap
   * effects until activate(). `binding` defaults to the incumbent's live
   * data root when the new definition uses the JavaScript data model.
   */
  prepareReplacement(incumbent, definition, options = {}) {
    const { slot, generation } = actorOf(this, incumbent);
    if (!(definition instanceof Definition) || definition.system !== this) {
      throw new TinyactorsError('prepareReplacement(): expected a Definition of this system', 'invalid');
    }
    let binding = options.binding;
    if (binding === undefined && this.#usesJavaScript(definition.id)) {
      const view = core.actor_inspect(this.#native, slot, generation);
      binding = this.#dataRoot(view.definition_id, slot, generation);
    }
    const id = core.actor_prepare_replacement(this.#native, slot, generation, definition.id, binding);
    return new PreparedActor(this, id.slot, id.generation, incumbent);
  }

  /**
   * Publish `prepared` at its incumbent's address, then destroy the
   * incumbent (docs/design.md). Mail still waiting for the incumbent moves,
   * in order, to the front of the replacement's mailbox, so nothing accepted
   * for the address is lost; the incumbent's delayed sends are discarded.
   * Throws `busy` while the incumbent is inside a macrostep. If activation
   * fails, the incumbent keeps the address and the prepared actor is left
   * for the caller to destroy.
   */
  activate(prepared) {
    if (!(prepared instanceof PreparedActor) || prepared.system !== this) {
      throw new TinyactorsError('activate(): expected a prepared actor of this system', 'invalid');
    }
    const { incumbent } = prepared;
    if (this.#native.scheduling(incumbent.slot, incumbent.generation) === 'runnable') {
      throw new TinyactorsError('activate(): the incumbent is inside a macrostep; activate once it is idle', 'busy');
    }
    core.actor_activate_replacement(this.#native, incumbent.slot, incumbent.generation,
      prepared.slot, prepared.generation);
    core.actor_destroy(this.#native, incumbent.slot, incumbent.generation);
    return new ActorID(this, prepared.slot, prepared.generation);
  }

  // The binding load() and reconstruct() pass: the option, or the image's root.
  #bindingFor(image, options) {
    if (options === null || typeof options !== 'object') {
      throw new TinyactorsError('expected options { binding }', 'invalid');
    }
    return options.binding !== undefined ? options.binding : image.data;
  }

  // True when the definition runs the JavaScript data model; cached.
  #usesJavaScript(definitionID) {
    let javascript = this.#javascript.get(definitionID);
    if (javascript === undefined) {
      javascript = core.definition_inspect(this.#native, definitionID).datamodel === 'javascript';
      this.#javascript.set(definitionID, javascript);
    }
    return javascript;
  }

  /** Put an actor that faulted back into scheduling (ADR 0008). */
  release(actor) {
    const { slot, generation } = actorOf(this, actor);
    this.#native.release(slot, generation);
  }

  /**
   * Resolves once the system is quiet: nothing runnable, nothing ready, no
   * dead letters to report, and nothing due at the current time (ADR 0007
   * point 4). A delayed send due later does not hold it. It spans as many
   * pump turns as needed and yields to the event loop between them. close()
   * rejects it with `closed`.
   */
  flush() {
    return this.#native.flush();
  }

  /**
   * Memory counters by category and the number of allocator calls
   * (ADRs 0009 and 0014). Throws `closed` after close().
   */
  stats() {
    return this.#native.stats();
  }

  /**
   * A metrics snapshot as plain data (napi ADR 0017 section 7, core ADR
   * 0072): core's counters per definition, state occupancy, and napi's
   * messages and memory, as { startTime, time, system, metrics }, where
   * each metric is { name, unit, kind: "sum" | "gauge", points:
   * [{ attributes, value }] }. Read it from an OpenTelemetry observable
   * instrument, or encode it with metricsToOTLP(). Cheap enough to call at
   * every export interval: it walks definitions, not actors.
   */
  metrics() {
    const native = this.#native;
    const definitions = [];
    for (const [id, identity] of this.#definitionIdentities) {
      let counters;
      try {
        counters = core.definition_counters(native, id);
      } catch (error) {
        if (error instanceof TinyactorsError && error.code === 'not-found') {
          this.#definitionIdentities.delete(id); // freed: its counts live on in the system's totals
          continue;
        }
        throw error;
      }
      const occupancy = [];
      core.definition_occupancy(native, id).forEach((actors, state) => {
        if (actors !== 0) {
          occupancy.push({ state: core.definition_state(native, id, state).id, actors });
        }
      });
      definitions.push({
        attributes: { 'tinyactors.definition.family': identity.family,
                      'tinyactors.definition.revision': identity.revision },
        counters: numbers(counters),
        occupancy,
      });
    }
    const stats = native.stats();
    const memory = {};
    for (const [category, counters] of Object.entries(stats.memory)) {
      if (category !== 'total') {
        memory[category] = counters.bytesInUse;
      }
    }
    return buildSnapshot({
      startTime: this.#startTime,
      time: Date.now(),
      systemName: this.#metricsName,
      system: numbers(core.system_counters(native)),
      definitions,
      host: { queued: stats.messages.queued, delivered: stats.messages.delivered, memory,
              native: native.hostMetrics() },
    });
  }

  /**
   * Destroy the native system and everything it holds. Idempotent and
   * authoritative: afterwards every method except close() throws `closed`.
   */
  close() {
    if (!this.#native.closed) {
      // Services first: their signals abort and live invocations are
      // cancelled; what core's teardown asks of them afterwards is dropped.
      this.#lifetime.abort();
      for (const handle of [...this.#invocations.keys()]) {
        this.#cancelInvocation(handle, 'closed');
      }
    }
    this.#native.close();
  }

  [Symbol.dispose]() {
    this.close();
  }
}

/*
 * The retained-bytes estimate of a message's data (ADR 0005 point 9):
 * `options.bytes` when given; otherwise 0 for no data and for numbers,
 * booleans, null and the like (they ride inline, ADR 0011), 2 bytes per
 * UTF-16 code unit for a string, byteLength for an ArrayBuffer or a view of
 * one, and a flat 64 bytes for any other object. Objects are not walked:
 * the estimate must stay O(1), so pass `bytes` for large objects.
 */
const OBJECT_BYTES = 64;
/*
 * The origin option of post() and send() (core ADR 0068): who a chart
 * answers with `_event.origin`. { type, uri } is one request's reply URI,
 * { type, scope } names a scope from openScope(), { type, token } a
 * computed origin rendered from the processor's origin template.
 */
function originOf(origin, what) {
  if (origin === undefined) return undefined;
  if (origin === null || typeof origin !== 'object' || typeof origin.type !== 'string') {
    throw new TinyactorsError(`${what}: options.origin must be { type, uri }, { type, scope } or { type, token }`, 'invalid');
  }
  if (origin.scope !== undefined) {
    return { type: origin.type, scope: { index: origin.scope.index, generation: origin.scope.generation } };
  }
  return origin;
}

function retainedBytes(data, options, what) {
  const hint = options?.bytes;
  if (hint !== undefined) {
    if (!(typeof hint === 'number' && hint >= 0 && Number.isFinite(hint))) {
      throw new TinyactorsError(`${what}: options.bytes must be a non-negative number`, 'invalid');
    }
    return Math.ceil(hint);
  }
  switch (typeof data) {
    case 'string':
      return data.length * 2;
    case 'object':
      if (data === null) return 0;
      if (data instanceof ArrayBuffer || ArrayBuffer.isView(data)) return data.byteLength;
      return OBJECT_BYTES;
    case 'function':
      return OBJECT_BYTES;
    case 'bigint':
      return 16;
    default:
      return 0;
  }
}

const DEFAULT_PAGE_SIZE = 1024;
const MAX_PAGE_SIZE = 65536;

// The page size of actors() and actorIDs().
function pageSizeOf(options, method) {
  if (options === null || typeof options !== 'object') {
    throw new TinyactorsError(`${method}: options must be an object`, 'invalid');
  }
  const { pageSize = DEFAULT_PAGE_SIZE } = options;
  if (!Number.isInteger(pageSize) || pageSize < 1 || pageSize > MAX_PAGE_SIZE) {
    throw new TinyactorsError(`${method}: pageSize must be an integer from 1 to ${MAX_PAGE_SIZE}`, 'invalid');
  }
  return pageSize;
}

// The (slot, generation) of an ActorID of `system` (ADR 0002).
function actorOf(system, actor) {
  if (!(actor instanceof ActorID) || actor.system !== system) {
    throw new TinyactorsError('expected an actor of this system', 'invalid');
  }
  return actor;
}

// A named address { family, name } with non-empty text (ADR 0006).
function addressOf(address, caller) {
  if (address === null || typeof address !== 'object' ||
      typeof address.family !== 'string' || address.family === '' ||
      typeof address.name !== 'string' || address.name === '') {
    throw new TinyactorsError(`${caller}: expected an address { family, name } of non-empty strings`, 'invalid');
  }
  return address;
}

// The io or invokers option: undefined or an object of services by type.
function servicesOf(value, what) {
  if (value === undefined) {
    return {};
  }
  if (value === null || typeof value !== 'object') {
    throw new TinyactorsError(`${what} must be an object of services by type`, 'invalid');
  }
  return value;
}

native.setupEffects(effectHandler);

// A fresh unpublished actor that replaces `incumbent` once activated (step 14).
class PreparedActor extends ActorID {
  constructor(system, slot, generation, incumbent) {
    super(system, slot, generation);
    this.incumbent = incumbent;
  }

  /** False until activate() publishes it at the incumbent's address. */
  get published() {
    return this.system.inspect(this).published;
  }

  /** Publish at the incumbent's address and destroy the incumbent (System.activate). */
  activate() {
    return this.system.activate(this);
  }
}

function createSystem(options) {
  return new System(options);
}

module.exports = { System, createSystem, nativeOf, ActorID, PreparedActor, SessionImage };
