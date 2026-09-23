'use strict';

// System facade over the native System (ADR 0012) and the generated core
// module (ADR 0013). Scheduling is native (ADR 0007): nothing here takes part
// in it; spawn() and destroy() only reach core, whose hooks tell the pump.

const native = require('./native');
const { TinyactorsError } = require('./errors');
const { Definition, identityOf } = require('./definition');
const { DefinitionBuilder, needsJavaScript } = require('./statechart');
const { ActorId } = require('./actor');
const { EventNames } = require('./events');

const NO_STATE = 4294967295;

const { core } = native;

// Compiles a spec in the generated shape. A refused chart throws with core's
// compile error formatted into the message (ADR 0058), and the error itself,
// with a readable path, as `detail`.
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
      this.registerIo(type, processor);
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
   * never inside core (ADR 0001 rule 3). `request` is { source, sendId,
   * event, type, target, data } (and { content, contentType } for a literal
   * <content>); `context` is { system, signal, post }, where `signal` aborts
   * on close(). A reply is an event the processor posts, now or later. A
   * processor that throws or whose promise rejects has failed the send: the
   * sender gets an external `error.communication` event whose data is
   * { sendId, type, error }.
   */
  registerIo(type, processor) {
    if (typeof type !== 'string' || type === '') {
      throw new TinyactorsError('registerIo(): expected a type string', 'invalid');
    }
    if (processor === null || typeof processor !== 'object' || typeof processor.send !== 'function') {
      throw new TinyactorsError('registerIo(): expected an I/O processor with a send(request, context) method', 'invalid');
    }
    this.#native.registerIo(type);
    this.#io.set(type, processor);
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

  // One service effect, in the order core made the calls (napi_services.h).
  #effect(kind, payload) {
    switch (kind) {
      case 'log':
        this.#logSink?.(payload);
        return;
      case 'io':
        this.#runIo(payload);
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

  #runIo(request) {
    const processor = this.#io.get(request.type);
    if (processor === undefined) {
      this.#communicationError(request, new TinyactorsError(`no I/O processor for "${request.type}"`, 'not-found'));
      return;
    }
    let outcome;
    try {
      outcome = processor.send(request, this.#context());
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
      this.post(request.source, 'error.communication', { sendId: request.sendId, type: request.type, error });
    } catch {
      // The sender is gone or its mailbox is full: nobody is left to tell.
    }
  }

  #startInvocation(payload) {
    const { handle, parent, invocationId } = payload;
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
      return this.#native.reportInvoke(parent.slot, parent.generation, invocationId, kind, name, data);
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
      invocationId,
      invokeId: payload.invokeId,
      type: payload.type,
      source: payload.source,
      content: payload.content,
      contentType: payload.contentType,
      params: payload.params,
      autoforward: payload.autoforward,
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
   */
  define(source) {
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
    return new Definition(this, this.#native, id);
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
    const id = core.actor_spawn(this.#native, definition.id, address, options.binding);
    return new ActorId(this, id.slot, id.generation);
  }

  /**
   * The resident actor the directory has at `address` ({ family, name }), or
   * undefined (ADR 0006).
   */
  findActor(address) {
    const { family, name } = addressOf(address, 'findActor()');
    try {
      const id = core.actor_find(this.#native, { family: { name: family }, name });
      return new ActorId(this, id.slot, id.generation);
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
      actor: new ActorId(this, view.actor.slot, view.actor.generation),
      sessionId: view.session_id,
      definition: identityOf(view.definition),
      residency: view.residency,
      published: view.published,
      activeStates: core.actor_active_states(this.#native, slot, generation).map(stateName),
      macrostepInProgress: view.macrostep_in_progress,
      internalEventCount: view.internal_events.length,
      delayedSendCount: view.delayed_sends.length,
      delayedSends: view.delayed_sends.map((send) => this.#delayedSend(send)),
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
  #dataRoot(definitionId, slot, generation) {
    let javascript = this.#javascript.get(definitionId);
    if (javascript === undefined) {
      javascript = core.definition_inspect(this.#native, definitionId).datamodel === 'javascript';
      this.#javascript.set(definitionId, javascript);
    }
    return javascript ? core.actor_datamodel_root(this.#native, slot, generation) : undefined;
  }

  // One pending delayed send, from core's ta_delayed_send_info (core ADR 0054).
  #delayedSend(send) {
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
      target: send.target,
      data: send.data,
      submitted: send.submitted,
    };
  }

  // A resolved destination: an ActorId, an address, or null for a custom
  // I/O processor, whose destination core leaves as runtime ID 0.
  #destination(ref) {
    if (ref === undefined || ref === null) {
      return null;
    }
    if (ref.kind === 'named') {
      return ref.named === undefined ? null : { family: ref.named.family.name, name: ref.named.name };
    }
    const { slot, generation } = ref.runtime;
    return generation === 0 ? null : new ActorId(this, slot, generation);
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
  *actorIds(options = {}) {
    const pageSize = pageSizeOf(options, 'actorIds()');
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
    for (const ids of this.actorIds(options)) {
      for (let at = 0; at < ids.length; at += 2) {
        let inspection;
        try {
          inspection = this.inspect(new ActorId(this, ids[at], ids[at + 1]));
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
   * ActorId or an address { family, name }, resolved through the directory
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
      if (!(target instanceof ActorId)) addressOf(target, 'send()');
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
      return Promise.resolve({ actor: target instanceof ActorId ? target : undefined, status: 'accepted' });
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
    if (!(target instanceof ActorId)) {
      if (target !== null && typeof target === 'object' && 'name' in target) {
        const { family, name } = addressOf(target, what);
        const id = this.#events.id(event);
        return completion
          ? this.#native.sendNamed(family, name, id, data, bytes)
          : this.#native.postNamed(family, name, id, data, bytes);
      }
      throw new TinyactorsError(`${what}: expected an actor of this system or an address`, 'invalid');
    }
    const { slot, generation } = actorOf(this, target);
    const id = this.#events.id(event);
    return completion
      ? this.#native.send(slot, generation, id, data, bytes)
      : this.#native.post(slot, generation, id, data, bytes);
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

// The page size of actors() and actorIds().
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

// The (slot, generation) of an ActorId of `system` (ADR 0002).
function actorOf(system, actor) {
  if (!(actor instanceof ActorId) || actor.system !== system) {
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

function createSystem(options) {
  return new System(options);
}

module.exports = { System, createSystem, nativeOf, ActorId };
