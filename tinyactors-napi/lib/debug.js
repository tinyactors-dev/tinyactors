'use strict';

// The debug adapter (docs/proposals/debug-adapter.md; napi ADR 0027): a
// Debug Adapter Protocol session over one attached actor of a System.
//
// napi opens no socket and checks no credentials (core ADR 0002, napi ADRs
// 0015 and 0016). The application puts a session behind its own transport
// and authentication, decides the mode and the permissions of the user
// before it creates the session, and passes each incoming DAP message to
// receive(); the session hands each outgoing one to `send`. An `audit` hook
// hears attach, detach, every console command, every evaluation and every
// mutation, before it happens: a hook that throws refuses it.
//
// Modes: `watch` never stops the actor and changes nothing; breakpoints
// report where they would have stopped. `live` works on the actor itself,
// as the permissions allow: `send`, `set`, `goto` (with abandon, cancel and
// replace), `hold` (pause, stepping and breakpoints that stop), and
// `evaluate`. A live actor stops only between macrosteps, so a send()
// waiting for a macrostep is never left waiting by a debugger; a hold ends
// by itself after `holdTimeout`. `fork` waits for core ADR 0085.
//
// Evaluating an expression runs arbitrary code in the process that hosts
// the actor. It is the `evaluate` permission, off unless granted: without
// it the Variables pane and the console commands still work.
//
// This module is host neutral, so the browser build can share it: it uses
// the System facade and core's generated bindings, and no Node.js module.

const { core } = require('./native');
const { TinyactorsError } = require('./errors');
const { ActorID } = require('./actor');
const { nativeOf, observe } = require('./system');
const { Labels } = require('./debug/labels');
const { Macrosteps, summarize } = require('./debug/macrosteps');
const { Breakpoints, EXCEPTION_FILTERS } = require('./debug/breakpoints');
const { Variables } = require('./debug/variables');
const { frames, leaves } = require('./debug/frames');
const expressions = require('./debug/expressions');
const commands = require('./debug/console');
const { DAPReader, encodeDAPMessage } = require('./debug/protocol');

const MODES = ['watch', 'fork', 'live'];
const PERMISSIONS = ['send', 'set', 'goto', 'hold', 'evaluate'];
const DEFAULT_HOLD_TIMEOUT = 30000;
const SCXML_INVOKER = 'http://www.w3.org/TR/scxml/';
const ATTACHED = 1; // the attached actor's thread ID
const OPERATOR_EVENT = 'operator.goto';

const CAPABILITIES = Object.freeze({
  supportsConfigurationDoneRequest: true,
  supportsFunctionBreakpoints: true,
  supportsConditionalBreakpoints: true,
  supportsHitConditionalBreakpoints: true,
  supportsEvaluateForHovers: true,
  exceptionBreakpointFilters: EXCEPTION_FILTERS.map(({ filter, label, description }) => ({ filter, label, description })),
  supportsSetVariable: true,
  supportsSetExpression: true,
  supportsCompletionsRequest: true,
  completionTriggerCharacters: ['.', ' '],
  supportsModulesRequest: true,
  supportsExceptionInfoRequest: true,
  supportsDelayedStackTraceLoading: true,
  supportsLoadedSourcesRequest: true,
  supportsTerminateRequest: true,
  supportsRestartRequest: true,
  supportsDataBreakpoints: true,
  supportsClipboardContext: true,
  supportsSteppingGranularity: true,
  supportsGotoTargetsRequest: true,
  supportTerminateDebuggee: false,
  supportSuspendDebuggee: false,
});

// Requests the adapter answers with an error, and why. Their capabilities
// are not declared.
const REFUSED = Object.freeze({
  launch: 'napi\'s adapter attaches to a running actor: use "attach" with an address (the test host launches workbenches)',
  breakpointLocations: 'breakpoint locations need source spans (core ADR 0086)',
  stepBack: 'stepping back needs a fork, whose images restore into a debug system (core ADR 0085)',
  reverseContinue: 'reverse continue needs a fork, whose images restore into a debug system (core ADR 0085)',
  restartFrame: 'restarting a frame needs a fork, whose images restore into a debug system (core ADR 0085)',
  cancel: 'every request answers at once, so there is nothing to cancel',
  stepInTargets: 'an action has one way in',
  terminateThreads: 'ending a thread would destroy an actor',
  setInstructionBreakpoints: 'a chart has no instructions',
  locations: 'no value in a chart names a place in a source',
  readMemory: 'a chart has no memory to show',
  writeMemory: 'a chart has no memory to show',
  disassemble: 'a chart has no instructions',
});

// A request the session refuses; its message is what the client shows.
class Refusal extends Error {}

const refuse = (message) => new Refusal(message);
const jsonText = (value) => JSON.stringify(value, (key, inner) => (typeof inner === 'bigint' ? String(inner) : inner));
const nextTurn = () => new Promise((resolve) => setTimeout(resolve, 0));

let sessionCount = 0;
// The request handlers, by command; set in the class's static block, where
// they can reach its private members.
let HANDLERS;

class DebugSession {
  #system;
  #native;
  #labels;
  #mode;
  #allow;
  #audit;
  #send;
  #holdTimeout;
  #seq = 0;
  #queue = Promise.resolve();
  #client = {};
  #closed = false;

  // While attached: { actor, label, address, sessionID, mode, allow,
  // console, observer, watch }.
  #attached = null;
  #threads = new Map();
  #nextThread = ATTACHED + 1;
  #macrosteps;
  #lastEvent = new Map();
  #lastMicrostep = new Map();
  #breakpoints = new Breakpoints();
  #variables;
  #frames = new Map();
  #nextFrame = 0;
  #gotoTargets = new Map();
  #modules = new Map();
  #sources = new Map();

  // Holding (live): where the actor stopped, what the next hold means, and
  // whether it is held after each macrostep for the breakpoints.
  #stop = null;
  #pending = null;
  #gate = false;
  #hits = [];
  #skipTaking = false;
  #holdTimer;
  #waiting = false;

  constructor(system, options) {
    if (system === null || typeof system !== 'object' || typeof system.hold !== 'function') {
      throw new TinyactorsError('createDebugSession(): expected a System', 'invalid');
    }
    const { mode, allow = [], audit, send, holdTimeout = DEFAULT_HOLD_TIMEOUT } = options ?? {};
    if (!MODES.includes(mode)) {
      throw new TinyactorsError('createDebugSession(): options.mode must be "watch", "fork" or "live"', 'invalid');
    }
    if (!Array.isArray(allow) || allow.some((entry) => !PERMISSIONS.includes(entry))) {
      throw new TinyactorsError(`createDebugSession(): options.allow lists ${PERMISSIONS.join(', ')}`, 'invalid');
    }
    if (typeof send !== 'function') {
      throw new TinyactorsError('createDebugSession(): options.send must be a function that takes each outgoing message', 'invalid');
    }
    if (audit !== undefined && typeof audit !== 'function') {
      throw new TinyactorsError('createDebugSession(): options.audit must be a function', 'invalid');
    }
    if (!(typeof holdTimeout === 'number' && holdTimeout > 0 && holdTimeout <= 2147483647)) {
      throw new TinyactorsError('createDebugSession(): options.holdTimeout must be a positive number of milliseconds', 'invalid');
    }
    this.#system = system;
    this.#native = nativeOf(system);
    this.#labels = new Labels(this.#native);
    this.#mode = mode;
    this.#allow = new Set(allow);
    this.#audit = audit;
    this.#send = send;
    this.#holdTimeout = holdTimeout;
    sessionCount += 1;
    this.id = sessionCount;
    this.#macrosteps = new Macrosteps((session) => this.#threadOfSession(session) !== undefined);
    this.#variables = new Variables({
      system,
      native: this.#native,
      labels: this.#labels,
      thread: (id) => this.#thread(id),
      eventOf: (thread, view) => this.#eventOf(thread, view),
      mailboxNote: (message) => this.#mailboxNote(message),
      threadOfSession: (session) => this.#threadOfSession(session),
      documentName: (view) => this.#documentName(view),
    });
  }

  /** The mode and permissions this session was created with. */
  get mode() {
    return this.#mode;
  }

  get allow() {
    return [...this.#allow];
  }

  /** True while an actor is attached. */
  get attached() {
    return this.#attached !== null;
  }

  /**
   * Takes one incoming DAP message (an object). Requests are answered in
   * the order they arrive; the promise settles once this one is answered.
   */
  receive(message) {
    const run = this.#queue.then(() => this.#dispatch(message));
    this.#queue = run.catch(() => {});
    return run;
  }

  /** The transport closed: detach, releasing a hold, without answering. */
  close() {
    if (this.#closed) return;
    this.#closed = true;
    this.#detach('the connection closed');
  }

  // ------------------------------------------------------------------
  // Messages

  #write(message) {
    if (this.#closed) return;
    this.#seq += 1;
    this.#send({ seq: this.#seq, ...message });
  }

  #event(event, body) {
    this.#write(body === undefined ? { type: 'event', event } : { type: 'event', event, body });
  }

  #output(category, text) {
    this.#event('output', { category, output: `${text}\n` });
  }

  async #dispatch(message) {
    if (this.#closed || message === null || typeof message !== 'object' || message.type !== 'request') {
      return;
    }
    const { command } = message;
    const respond = (body, success = true, error) => this.#write({
      type: 'response', request_seq: message.seq, success, command,
      ...(success ? { body: body ?? {} } : { message: error, body: { error: { id: 1, format: error, showUser: true } } }),
    });
    if (Object.hasOwn(REFUSED, command)) {
      respond(undefined, false, REFUSED[command]);
      return;
    }
    const handler = HANDLERS[command];
    if (handler === undefined) {
      respond(undefined, false, `${command} is not supported by napi's debug adapter`);
      return;
    }
    let after;
    try {
      const outcome = await handler.call(this, message.arguments ?? {});
      after = outcome?.after;
      respond(outcome?.body);
    } catch (error) {
      if (!(error instanceof Refusal || error instanceof TinyactorsError)) {
        this.#output('stderr', `${command}: ${error?.stack ?? error}`);
      }
      respond(undefined, false, error?.message ?? String(error));
      return;
    }
    after?.();
  }

  // ------------------------------------------------------------------
  // Audit and permissions

  #record(entry) {
    if (this.#audit === undefined) return;
    const attached = this.#attached;
    this.#audit({
      ...entry,
      time: Date.now(),
      session: this.id,
      ...(attached === null ? {} : { actor: attached.label, sessionID: attached.sessionID, mode: attached.mode }),
    });
  }

  // The audit hook hears it first; one that throws refuses it.
  #audited(entry) {
    try {
      this.#record(entry);
    } catch (error) {
      throw refuse(`the audit hook refused it: ${error?.message ?? error}`);
    }
  }

  #requireAttached() {
    if (this.#attached === null) throw refuse('attach to an actor first');
    return this.#attached;
  }

  #requireLive(permission, what) {
    const attached = this.#requireAttached();
    if (attached.mode !== 'live') {
      throw refuse(`${what} is not offered in ${attached.mode} mode, which never changes or stops the actor`);
    }
    if (!attached.allow.has(permission)) {
      throw refuse(`${what} needs the "${permission}" permission, which this session was not given`);
    }
    return attached;
  }

  #requireEvaluate(what) {
    const attached = this.#requireAttached();
    if (!attached.allow.has('evaluate')) {
      throw refuse(`${what} evaluates an expression, which runs code in the process; it needs the "evaluate" permission`);
    }
    return attached;
  }

  // ------------------------------------------------------------------
  // Attaching

  #resolve(args) {
    const system = this.#system;
    const { address, actor, sessionID } = args;
    if (address !== undefined) {
      const parsed = parseAddress(address);
      if (parsed === undefined) {
        throw refuse('"address" is { family, name }, "family/name" or "#_actor_family/name"');
      }
      const found = system.findActor(parsed);
      if (found === undefined) throw refuse(`no resident actor has the address ${parsed.family}/${parsed.name}`);
      return { actor: found, address: parsed };
    }
    if (actor !== undefined) {
      const id = new ActorID(system, Number(actor.slot), Number(actor.generation));
      if (!system.exists(id)) throw refuse(`no resident actor is ${actor.slot}:${actor.generation}`);
      return { actor: id, address: undefined };
    }
    if (sessionID !== undefined) {
      let wanted;
      try {
        wanted = BigInt(sessionID);
      } catch {
        throw refuse('"sessionID" is a session ID in decimal');
      }
      const found = actorOfSession(system, wanted);
      if (found === undefined) throw refuse(`no resident actor holds session ${wanted}`);
      return { actor: found, address: undefined };
    }
    throw refuse('attach needs "address", "actor" { slot, generation } or "sessionID"');
  }

  #attach(args) {
    if (this.#attached !== null) throw refuse(`already attached to ${this.#attached.label}; disconnect first`);
    const mode = args.mode ?? this.#mode;
    if (!MODES.includes(mode)) throw refuse('"mode" is "watch", "fork" or "live"');
    if (MODES.indexOf(mode) > MODES.indexOf(this.#mode)) {
      throw refuse(`this session allows ${this.#mode} mode at most, not ${mode}`);
    }
    if (mode === 'fork') {
      throw refuse('a fork needs a debug system of its own, which needs images that restore into another system ' +
                   '(core ADR 0085); attach in watch or live mode');
    }
    const asked = Array.isArray(args.allow) ? new Set(args.allow) : this.#allow;
    const allow = new Set([...this.#allow].filter((permission) => asked.has(permission)));
    const { actor, address } = this.#resolve(args);
    const view = this.#system.inspect(actor);
    const label = address !== undefined ? `${address.family}/${address.name}`
      : view.published && typeof args.label === 'string' ? args.label : `actor ${actor.slot}:${actor.generation}`;
    const attached = { actor, label, address, sessionID: String(view.sessionID), mode, allow };
    this.#attached = attached;
    try {
      this.#audited({ type: 'attach', allow: [...allow] });
    } catch (error) {
      this.#attached = null;
      throw error;
    }
    this.#threads.set(ATTACHED, { id: ATTACHED, actor, label, session: attached.sessionID });
    attached.observer = observe(this.#system, {
      traces: (traces) => this.#onTraces(traces),
      log: (record) => this.#onLog(record),
      held: (held) => this.#onHeld(held),
    });
    attached.watch = this.#system.watchTraces({ sessions: [actor] });
    if (mode === 'live' && (allow.has('send') || allow.has('goto'))) {
      attached.console = commands.openConsole(this.#system, `debug://session/${this.id}`, (request) => this.#onReply(request));
    }
    return attached;
  }

  // Ends the attachment: releases what this session holds, stops watching.
  #detach(reason) {
    const attached = this.#attached;
    if (attached === null) return;
    this.#clearHoldTimer();
    const exists = !this.#system.closed && this.#system.exists(attached.actor);
    if (exists && (this.#gate || this.#pending !== null || this.#stop !== null)) {
      this.#system.unhold(attached.actor);
    }
    this.#gate = false;
    this.#pending = null;
    this.#stop = null;
    this.#hits = [];
    attached.observer?.stop();
    if (!this.#system.closed) {
      attached.watch?.stop();
      attached.console?.close();
    }
    try {
      this.#record({ type: 'detach', reason });
    } catch {
      // Detaching cannot be refused.
    }
    this.#attached = null;
    this.#threads.clear();
    this.#resetViews();
  }

  // The attached actor went away: the session ends.
  #ended(why) {
    if (this.#attached === null) return;
    const label = this.#attached.label;
    this.#detach(why);
    this.#output('important', `${label} ${why}; the debug session ended`);
    this.#event('terminated');
  }

  // ------------------------------------------------------------------
  // Threads

  #thread(id) {
    const thread = this.#threads.get(Number(id));
    if (thread === undefined) throw refuse(`no thread ${id}`);
    return thread;
  }

  #threadOfSession(session) {
    const text = String(session);
    for (const thread of this.#threads.values()) {
      if (thread.session === text) return thread;
    }
    return undefined;
  }

  #threadArgument(args) {
    this.#requireAttached();
    return args.threadId === undefined ? this.#threads.get(ATTACHED) : this.#thread(args.threadId);
  }

  #inspect(thread) {
    try {
      return this.#system.inspect(thread.actor);
    } catch (error) {
      if (error instanceof TinyactorsError && error.code === 'not-found') return undefined;
      throw error;
    }
  }

  // The attached actor and the in-system children of its invocations, and
  // the thread events for what changed.
  #refreshThreads() {
    const attached = this.#attached;
    if (attached === null) return;
    const view = this.#inspect(this.#threads.get(ATTACHED));
    if (view === undefined) {
      this.#ended('is gone');
      return;
    }
    const children = new Map();
    for (const invocation of view.invocations) {
      if (invocation.type !== SCXML_INVOKER || !invocation.active) continue;
      const child = actorOfSession(this.#system, invocation.serviceHandle, false);
      if (child !== undefined) children.set(String(invocation.serviceHandle), { child, invocation });
    }
    for (const thread of [...this.#threads.values()]) {
      if (thread.id !== ATTACHED && !children.has(thread.session)) {
        this.#threads.delete(thread.id);
        this.#macrosteps.forget(thread.session);
        this.#event('thread', { reason: 'exited', threadId: thread.id });
      }
    }
    for (const [session, { child, invocation }] of children) {
      if (this.#threadOfSession(session) === undefined) {
        const id = this.#nextThread;
        this.#nextThread += 1;
        this.#threads.set(id, { id, actor: child, label: `${attached.label} invocation ${invocation.id}`, session });
        this.#event('thread', { reason: 'started', threadId: id });
      }
    }
  }

  #threadName(thread) {
    const view = this.#inspect(thread);
    if (view === undefined) return `${thread.label} (gone)`;
    const where = view.residency === 'done' ? `done in ${view.finalState ?? 'its final state'}`
      : leaves(view, this.#labels).join(' ');
    return `${thread.label} · ${where}`;
  }

  // ------------------------------------------------------------------
  // What the actor did: traces, <log> records, replies

  #onTraces(bytes) {
    if (this.#attached === null) return;
    let read;
    try {
      read = this.#macrosteps.read(bytes);
    } catch (error) {
      this.#output('stderr', `reading the traces failed: ${error.message}`);
      return;
    }
    for (const macrostep of read.macrosteps) {
      this.#onMacrostep(macrostep);
    }
    let gone;
    for (const entry of read.lifecycle) {
      if (entry.session === this.#attached?.sessionID && ['finished', 'destroyed', 'unloaded'].includes(entry.kind)) {
        gone = entry.kind;
      }
    }
    if (read.macrosteps.length > 0) {
      this.#resetViews();
      if (this.#client.supportsInvalidatedEvent) this.#event('invalidated', { areas: ['threads', 'variables'] });
    }
    if (gone === 'finished' && this.#attached !== null && this.#system.exists(this.#attached.actor)) {
      this.#output('important', `${this.#attached.label} finished`);
    } else if (gone !== undefined) {
      this.#ended(gone === 'unloaded' ? 'was unloaded' : gone === 'finished' ? 'finished' : 'was destroyed');
      return;
    }
    this.#refreshThreads();
    this.#refreshCatalog();
  }

  #onMacrostep(macrostep) {
    const thread = this.#threadOfSession(macrostep.session);
    if (thread === undefined) return;
    if (macrostep.event !== null) this.#lastEvent.set(macrostep.session, macrostep.event);
    const last = macrostep.microsteps.at(-1);
    if (last !== undefined) this.#lastMicrostep.set(macrostep.session, { macrostep: macrostep.number, microstep: last });
    const location = (definition, id) => this.#labels.expression(BigInt(definition), id);
    for (const line of summarize(macrostep, thread.label, location)) {
      this.#output('console', line);
    }
    if (thread.id !== ATTACHED) return;
    const skipTaking = this.#skipTaking;
    this.#skipTaking = false;
    const hits = this.#breakpoints.hits(macrostep, {
      label: thread.label,
      location: (id) => {
        const text = location(macrostep.definition, id);
        return text === undefined ? undefined : expressions.pathOf(text);
      },
      fires: (breakpoint) => this.#condition(breakpoint, thread),
      skipTaking,
    });
    if (hits.length === 0) return;
    if (this.#gate) {
      this.#hits.push(...hits.map((hit) => ({ ...hit, macrostep: macrostep.number })));
    } else {
      const why = this.#attached.mode === 'watch' ? '' : ' (stopping needs the "hold" permission)';
      for (const hit of hits) {
        this.#output('console', `would have stopped after macrostep ${macrostep.number}: ${hit.description}${why}`);
      }
    }
  }

  // A breakpoint's condition, in the data model after the macrostep. One
  // that throws stops, so its author sees it fail.
  #condition(breakpoint, thread) {
    try {
      return Boolean(expressions.evaluate(breakpoint.condition, this.#scope(thread)));
    } catch (error) {
      this.#output('stderr', `the condition of breakpoint ${breakpoint.id} failed: ${error.message}`);
      return true;
    }
  }

  #onLog(record) {
    const attached = this.#attached;
    if (attached === null) return;
    const source = record.source;
    const ours = source instanceof ActorID ? source.equals(attached.actor)
      : attached.address !== undefined && source?.family === attached.address.family && source?.name === attached.address.name;
    if (!ours) return;
    const value = record.hasValue ? ` ${typeof record.value === 'string' ? record.value : jsonText(record.value)}` : '';
    this.#output('stdout', `${record.label ?? 'log'}:${value}`);
  }

  #onReply(request) {
    const data = request.data === undefined ? '' : ` ${jsonText(request.data)}`;
    this.#output('console', `${this.#attached?.label ?? 'an actor'} replied ${request.event}${data}`);
  }

  // ------------------------------------------------------------------
  // Holding a live actor (napi ADR 0027)

  // A hold stopped the attached actor between macrosteps; the traces of
  // the macrostep that just ended were read already.
  #onHeld(actor) {
    const attached = this.#attached;
    if (attached === null || !actor.equals(attached.actor) || this.#stop !== null) return;
    const hits = this.#hits;
    this.#hits = [];
    if (this.#pending !== null) {
      const reason = this.#pending;
      this.#pending = null;
      const description = reason === 'goto' ? 'the jump\'s macrostep ended' : reason === 'step' ? 'the macrostep ended' : 'paused';
      this.#halt(reason, { description, hits });
      return;
    }
    if (hits.length > 0) {
      this.#halt(undefined, { hits });
      return;
    }
    if (this.#gate) {
      this.#advance();
      return;
    }
    this.#halt('pause', { description: 'held' });
  }

  // Held for the breakpoints and nothing hit: stop before the next message
  // when an `on:` breakpoint names it, and let it take the message otherwise.
  #advance() {
    const actor = this.#attached.actor;
    const [next] = this.#system.mailbox(actor, { limit: 1 });
    const skip = this.#skipTaking;
    if (next !== undefined && !skip) {
      const thread = this.#threads.get(ATTACHED);
      const taking = this.#breakpoints.taking(next.event, (breakpoint) => this.#condition(breakpoint, thread));
      if (taking !== undefined) {
        this.#halt(undefined, { hits: [taking] });
        return;
      }
    }
    this.#skipTaking = skip || next !== undefined;
    this.#waiting = this.#system.step(actor) === 'waiting';
  }

  // The actor is held: tell the client, and arm the hold's timeout.
  #halt(reason, { description, hits = [] }) {
    const hit = hits[0];
    const kind = reason ?? (hit.kind === 'exception' ? 'exception' : hit.kind === 'data' ? 'data breakpoint' : 'function breakpoint');
    const text = [description, ...hits.map((entry) => entry.description)].filter((part) => part !== undefined).join('; ');
    this.#stop = { reason: kind, description: text, exception: hits.find((entry) => entry.kind === 'exception')?.exception,
                   breakpoints: hits.flatMap((entry) => entry.breakpoints) };
    this.#resetViews();
    const body = { reason: kind, threadId: ATTACHED, allThreadsStopped: false, preserveFocusHint: false };
    if (text !== '') body.description = text;
    if (this.#stop.exception !== undefined) body.text = hit?.description;
    if (this.#stop.breakpoints.length > 0) body.hitBreakpointIds = this.#stop.breakpoints;
    this.#event('stopped', body);
    this.#clearHoldTimer();
    this.#holdTimer = setTimeout(() => this.#holdExpired(), this.#holdTimeout);
    this.#holdTimer.unref?.();
  }

  #holdExpired() {
    this.#holdTimer = undefined;
    const attached = this.#attached;
    if (attached === null || this.#stop === null) return;
    try {
      this.#record({ type: 'mutation', operation: 'release', reason: 'hold timeout', holdTimeout: this.#holdTimeout });
    } catch {
      // The release happens whatever the audit hook says: it protects production.
    }
    this.#output('important', `${attached.label} was held for ${this.#holdTimeout} ms, the most this session may ` +
                              'hold it: it runs again');
    this.#release();
    this.#event('continued', { threadId: ATTACHED, allThreadsContinued: false });
  }

  #clearHoldTimer() {
    if (this.#holdTimer !== undefined) {
      clearTimeout(this.#holdTimer);
      this.#holdTimer = undefined;
    }
  }

  // Lets the stopped actor run again: through the breakpoints when any are
  // set, freely otherwise.
  #release() {
    const attached = this.#attached;
    const stopped = this.#stop;
    this.#stop = null;
    this.#pending = null;
    this.#clearHoldTimer();
    this.#resetViews();
    if (attached === null || this.#system.closed || !this.#system.exists(attached.actor)) return;
    if (this.#gate) {
      // A stop before an `on:` breakpoint's message lets that message through.
      this.#skipTaking = stopped?.breakpoints.length > 0 && stopped.reason === 'function breakpoint' &&
        stopped.description.includes('about to take');
      if (this.#system.inspect(attached.actor).scheduling === 'held') {
        this.#advance();
      }
    } else {
      this.#system.unhold(attached.actor);
    }
  }

  // Holds after each macrostep while breakpoints are set in live mode with
  // the `hold` permission, so they stop between macrosteps.
  #updateGate() {
    const attached = this.#attached;
    const wanted = attached !== null && attached.mode === 'live' && attached.allow.has('hold') && this.#breakpoints.any();
    if (wanted === this.#gate) return;
    this.#gate = wanted;
    if (this.#stop !== null || attached === null || !this.#system.exists(attached.actor)) return;
    if (wanted) {
      if (this.#system.hold(attached.actor)) this.#advance();
    } else if (this.#pending === null) {
      this.#system.unhold(attached.actor);
    }
  }

  // After an operation outside a turn (abandon): a pending hold may have
  // taken effect at once.
  #checkHeld() {
    const attached = this.#attached;
    if (attached !== null && this.#stop === null && this.#system.exists(attached.actor) &&
        this.#system.inspect(attached.actor).scheduling === 'held') {
      this.#onHeld(attached.actor);
    }
  }

  // ------------------------------------------------------------------
  // Views of the actor

  #resetViews() {
    this.#variables.reset();
    this.#frames.clear();
  }

  #documentName(view) {
    try {
      return core.definition_inspect(this.#native, view.definitionID).document_name || view.definition.family;
    } catch {
      return view.definition.family;
    }
  }

  // _event of the step the thread is in, or of its last macrostep.
  #eventOf(thread, view = this.#inspect(thread)) {
    if (view?.currentEvent) {
      return view.currentEvent;
    }
    const last = this.#lastEvent.get(thread.session);
    if (last === undefined) return undefined;
    let data;
    try {
      data = last.data === undefined ? undefined : JSON.parse(last.data);
    } catch {
      data = last.data;
    }
    return { name: last.name, type: last.type, ...(data === undefined ? {} : { data }), taken: 'in its last macrostep' };
  }

  #mailboxNote(message) {
    const scope = this.#attached?.console?.scope;
    const console = scope !== undefined && message.scope?.index === scope.index && message.scope?.generation === scope.generation;
    return {
      event: message.event,
      ...(message.data === undefined ? {} : { data: message.data }),
      ...(message.transition === undefined ? {} : { transition: message.transition }),
      ...(message.awaited ? { awaited: true } : {}),
      ...(console ? { from: 'this console' } : {}),
    };
  }

  // What an expression sees for a thread.
  #scope(thread) {
    const view = this.#inspect(thread);
    if (view === undefined) throw refuse(`${thread.label} is gone`);
    return {
      data: view.data,
      event: this.#eventOf(thread, view),
      sessionID: String(view.sessionID),
      name: this.#documentName(view),
      activeStates: view.activeStates,
    };
  }

  #frameThread(frameId) {
    if (frameId === undefined) return this.#threads.get(ATTACHED);
    const frame = this.#frames.get(Number(frameId));
    if (frame === undefined) throw refuse(`frame ${frameId} is not valid (any more)`);
    return this.#thread(frame.thread);
  }

  // ------------------------------------------------------------------
  // Modules and sources

  #refreshCatalog() {
    const seen = new Set();
    for (const definition of this.#system.definitions()) {
      const key = String(definition.id);
      seen.add(key);
      const revision = typeof definition.identity.revision === 'string' ? definition.identity.revision
        : [...definition.identity.revision].map((byte) => byte.toString(16).padStart(2, '0')).join('');
      const module = { id: key, name: `${definition.identity.family}${definition.retired ? ' (retired)' : ''}`,
                       version: revision };
      const known = this.#modules.get(key);
      if (known === undefined) {
        this.#modules.set(key, module);
        const reference = this.#sources.size + 1;
        this.#sources.set(reference, { definition: definition.id, source: sourceOf(definition, revision, reference) });
        if (this.#attached !== null) {
          this.#event('module', { reason: 'new', module });
          this.#event('loadedSource', { reason: 'new', source: this.#sources.get(reference).source });
        }
      } else if (known.name !== module.name) {
        this.#modules.set(key, module);
        if (this.#attached !== null) this.#event('module', { reason: 'changed', module });
      }
    }
    for (const [key, module] of [...this.#modules]) {
      if (!seen.has(key)) {
        this.#modules.delete(key);
        if (this.#attached !== null) this.#event('module', { reason: 'removed', module });
        for (const [reference, entry] of [...this.#sources]) {
          if (String(entry.definition) === key) {
            this.#sources.delete(reference);
            if (this.#attached !== null) this.#event('loadedSource', { reason: 'removed', source: entry.source });
          }
        }
      }
    }
  }

  // ------------------------------------------------------------------
  // Console operations

  #post(event, data, options) {
    const attached = this.#attached;
    const origin = attached.console === undefined ? undefined : { type: commands.TYPE, scope: attached.console.scope };
    this.#system.post(attached.actor, event, data, { ...options, origin });
    const waiting = this.#system.mailbox(attached.actor).length;
    if (this.#stop !== null) {
      return `; it waits in the mailbox (${waiting} message${waiting === 1 ? '' : 's'}): continue or step to deliver it`;
    }
    return '';
  }

  #consoleSend({ event, data }) {
    const attached = this.#requireLive('send', 'send');
    this.#audited({ type: 'mutation', operation: 'send', event, data });
    const note = this.#post(event, data);
    return `sent ${event} to ${attached.label}${note}`;
  }

  #jump(targets, from) {
    const attached = this.#requireLive('goto', 'goto');
    const definition = this.#system.inspect(attached.actor).definitionID;
    for (const state of [...targets, ...(from === undefined ? [] : [from])]) {
      if (this.#labels.stateID(definition, state) === undefined) throw refuse(`${state} is not a state of the chart`);
    }
    this.#audited({ type: 'mutation', operation: 'goto', targets, from });
    const data = { targets, ...(from === undefined ? {} : { from }) };
    const held = this.#stop !== null;
    const alone = held && this.#system.mailbox(attached.actor).length === 0;
    const note = this.#post(OPERATOR_EVENT, data, { transition: { targets, source: from } });
    const text = `${attached.label}: goto ${targets.join(' ')}${from === undefined ? '' : ` from ${from}`}`;
    if (alone && attached.allow.has('hold')) {
      // Nothing waits before it: take the jump now and stop after it.
      return { text, then: () => this.#stepNow('goto') };
    }
    return { text: `${text} queued${note}` };
  }

  async #abandon() {
    const attached = this.#requireLive('goto', 'abandon');
    const actor = attached.actor;
    const view = this.#system.inspect(actor);
    if (!view.macrostepInProgress) throw refuse(`${attached.label} has no macrostep to abandon`);
    this.#audited({ type: 'mutation', operation: 'abandon', macrostep: view.step.macrostep });
    if (view.microstepInProgress) {
      // Core abandons only between microsteps: let this one finish first.
      this.#system.setBudgets(actor, { resumeBudget: 1, actionBudget: 0 });
      try {
        for (let tries = 0; tries < 100 && this.#system.exists(actor) && this.#system.inspect(actor).microstepInProgress;
          tries += 1) {
          await nextTurn();
        }
      } finally {
        if (this.#system.exists(actor)) this.#system.setBudgets(actor, undefined);
      }
      if (!this.#system.exists(actor) || !this.#system.inspect(actor).macrostepInProgress) {
        return `${attached.label} finished its macrostep while finishing its microstep`;
      }
    }
    try {
      this.#system.abandon(actor);
    } catch (error) {
      if (error instanceof TinyactorsError && error.code === 'busy') {
        throw refuse(`${attached.label} has not finished its bootstrap, or is still inside a microstep; try again`);
      }
      throw error;
    }
    this.#checkHeld();
    return `${attached.label}: abandoned macrostep ${view.step.macrostep}; it keeps the configuration after its last ` +
           'completed microstep';
  }

  #cancel({ id }) {
    const attached = this.#requireLive('goto', 'cancel');
    this.#audited({ type: 'mutation', operation: 'cancel', sendID: id });
    try {
      this.#system.withdrawSend(attached.actor, id);
    } catch (error) {
      if (error instanceof TinyactorsError && error.code === 'done') {
        throw refuse(`send ${id} was offered to its target once and waits to be offered again; it belongs to its receiver now`);
      }
      throw error;
    }
    return `${attached.label}: withdrew send ${id}`;
  }

  async #replace({ revision }) {
    const attached = this.#requireLive('goto', 'replace');
    if (attached.address === undefined) throw refuse('only a named actor can be replaced in place');
    const definition = this.#system.findDefinition({ family: attached.address.family, revision });
    if (definition === undefined) throw refuse(`no registered, unretired ${attached.address.family} revision ${revision}`);
    const view = this.#system.inspect(attached.actor);
    if (view.macrostepInProgress) throw refuse(`${attached.label} is inside a macrostep: \`abandon\` it first`);
    this.#audited({ type: 'mutation', operation: 'replace', revision });
    const dropped = view.delayedSends.filter((send) => !send.submitted).map((send) => send.id ?? send.event);
    const prepared = this.#system.prepareReplacement(attached.actor, definition);
    let replacement;
    try {
      // The replacement bootstraps in a pump turn before it can be activated.
      for (let tries = 0; tries < 100 && this.#system.inspect(prepared).scheduling === 'runnable'; tries += 1) {
        await nextTurn();
      }
      replacement = this.#system.activate(prepared);
    } catch (error) {
      if (!this.#system.closed && this.#system.exists(prepared)) this.#system.destroy(prepared);
      throw error;
    }
    // Follow the address to the replacement.
    const args = { address: attached.address, mode: attached.mode, allow: [...attached.allow] };
    this.#detach('replaced');
    const next = this.#attach(args);
    this.#refreshThreads();
    const lost = dropped.length === 0 ? '' : `; its delayed sends ${dropped.join(', ')} were dropped`;
    return `${next.label} now runs ${revision} from its initial state, with its data${lost} (actor ` +
           `${replacement.slot}:${replacement.generation})`;
  }

  // A held actor takes its next message now, and stops after it.
  #stepNow(reason) {
    const attached = this.#attached;
    this.#stop = null;
    this.#clearHoldTimer();
    this.#resetViews();
    this.#pending = reason;
    this.#waiting = this.#system.step(attached.actor) === 'waiting';
    if (this.#waiting) {
      this.#output('console', `${attached.label} waits for its next message, and stops after it`);
    }
  }

  // next, stepIn and stepOut: on a live actor a step is one macrostep, the
  // next message's, because it stops only between macrosteps (stepping
  // inside one is a fork's, core ADR 0085). Whatever the granularity.
  #stepRequest(args) {
    const attached = this.#requireLive('hold', 'stepping');
    const thread = this.#threadArgument(args);
    if (thread.id !== ATTACHED) throw refuse('only the attached actor steps; its invoked children run');
    if (this.#stop === null) throw refuse(`${attached.label} is not stopped: pause it first`);
    this.#audited({ type: 'mutation', operation: 'step' });
    return { after: () => this.#stepNow('step') };
  }

  // A console line (evaluate, context repl).
  async #repl(line, thread) {
    const parsed = commands.parse(line);
    const text = (result, after) => ({ body: { result, variablesReference: 0 }, after });
    switch (parsed.command) {
      case 'help':
        return text(commands.HELP.join('\n'));
      case 'send':
        return text(this.#consoleSend(parsed));
      case 'goto': {
        const jump = this.#jump(parsed.targets, parsed.from);
        return text(jump.text, jump.then);
      }
      case 'abandon':
        return text(await this.#abandon());
      case 'cancel':
        return text(this.#cancel(parsed));
      case 'replace':
        return text(await this.#replace(parsed));
      case 'clock':
        throw refuse('clock moves a fork\'s own clock (core ADR 0085); on a live actor it would move time for ' +
                     'every actor of the system');
      case 'fork':
        throw refuse('fork needs a debug system of its own, which needs images that restore into another system ' +
                     '(core ADR 0085)');
      default: {
        this.#requireEvaluate('an expression');
        return { body: this.#variables.present(expressions.evaluate(parsed.text, this.#scope(thread))) };
      }
    }
  }

  // Why a function breakpoint cannot be verified, or undefined.
  #checkBreakpoint(breakpoint, definition) {
    if ((breakpoint.kind === 'entry' || breakpoint.kind === 'exit') && definition !== undefined &&
        this.#labels.stateID(definition, breakpoint.target) === undefined) {
      return `${breakpoint.target} is not a state of the chart`;
    }
    return this.#checkCondition(breakpoint);
  }

  #checkCondition(breakpoint) {
    const condition = breakpoint.condition;
    if (condition !== undefined && condition !== null && String(condition).trim() !== '' &&
        (this.#attached === null || !this.#attached.allow.has('evaluate'))) {
      return 'a condition is an expression, which runs code in the process; it needs the "evaluate" permission';
    }
    return undefined;
  }

  // ----------------------------------------------------------------------
  // Requests. Each answers { body, after }: `after` runs once the response
  // is out, for events that must follow it.

  static {
    HANDLERS = {
      initialize(args) {
        this.#client = args;
        return { body: CAPABILITIES, after: () => this.#event('initialized') };
      },

      configurationDone() {
        return {};
      },

      attach(args) {
        const attached = this.#attach(args);
        const permissions = attached.allow.size === 0 ? 'no permissions' : `allowed: ${[...attached.allow].join(', ')}`;
        return {
          after: () => {
            this.#event('process', { name: attached.label, startMethod: 'attach', isLocalProcess: true });
            this.#refreshCatalog();
            this.#refreshThreads();
            this.#output('console', `attached to ${attached.label} (session ${attached.sessionID}) in ${attached.mode} mode; ` +
                                    `${permissions}`);
            this.#updateGate();
          },
    };
  },

  restart(args) {
    const previous = this.#requireAttached();
    const config = args.arguments ?? { address: previous.address, actor: previous.address === undefined ? previous.actor : undefined,
                                       mode: previous.mode, allow: [...previous.allow] };
    this.#detach('restart');
    const attached = this.#attach(config);
    return { after: () => { this.#refreshThreads(); this.#output('console', `reattached to ${attached.label}`); this.#updateGate(); } };
  },

  disconnect() {
    this.#detach('disconnect');
    return {};
  },

  terminate() {
    this.#detach('terminate');
    return { after: () => this.#event('terminated') };
  },

  threads() {
    if (this.#attached === null) return { body: { threads: [] } };
    this.#refreshThreads();
    return { body: { threads: [...this.#threads.values()].map((thread) => ({ id: thread.id, name: this.#threadName(thread) })) } };
  },

  stackTrace(args) {
    const thread = this.#threadArgument(args);
    const view = this.#inspect(thread);
    let note;
    if (thread.id === ATTACHED && view !== undefined && view.scheduling === 'held') {
      const waiting = this.#system.mailbox(thread.actor).length;
      note = `held; ${waiting} message${waiting === 1 ? '' : 's'} waiting`;
    }
    const list = frames({ view, labels: this.#labels, seen: this.#lastMicrostep.get(thread.session), note });
    const start = Number(args.startFrame ?? 0);
    const levels = Number(args.levels ?? 0);
    const shown = list.slice(start, levels > 0 ? start + levels : undefined).map((frame) => {
      this.#nextFrame += 1;
      this.#frames.set(this.#nextFrame, { thread: thread.id });
      return { id: this.#nextFrame, name: frame.name, line: 0, column: 0, presentationHint: frame.hint };
    });
    return { body: { stackFrames: shown, totalFrames: list.length } };
  },

  scopes(args) {
    this.#requireAttached();
    return { body: { scopes: this.#variables.scopes(this.#frameThread(args.frameId)) } };
  },

  variables(args) {
    this.#requireAttached();
    return { body: { variables: this.#variables.variables(args.variablesReference, {
      start: Number(args.start ?? 0), count: Number(args.count ?? 0), filter: args.filter,
    }) } };
  },

  setVariable(args) {
    this.#requireLive('set', 'setVariable');
    const text = String(args.value);
    let value;
    const where = this.#variables.location(args.variablesReference, String(args.name));
    if (where === undefined) throw refuse('only the Data model can be written; the other scopes are views of core state');
    try {
      value = JSON.parse(text);
    } catch {
      this.#requireEvaluate('a value that is not JSON');
      value = expressions.evaluate(text, this.#scope(where.thread));
    }
    this.#audited({ type: 'mutation', operation: 'set', location: expressions.locationText(where.path), value: jsonText(value) });
    return { body: this.#variables.set(args.variablesReference, String(args.name), value) };
  },

  setExpression(args) {
    this.#requireLive('set', 'setExpression');
    this.#requireEvaluate('setExpression');
    const thread = this.#frameThread(args.frameId);
    const text = String(args.value);
    const scope = this.#scope(thread);
    let value;
    try {
      value = JSON.parse(text);
    } catch {
      value = expressions.evaluate(text, scope);
    }
    this.#audited({ type: 'mutation', operation: 'set', expression: String(args.expression), value: jsonText(value) });
    expressions.assign(String(args.expression), value, scope);
    const body = this.#variables.present(value);
    return { body: { value: body.result, type: body.type, variablesReference: body.variablesReference } };
  },

  async evaluate(args) {
    this.#requireAttached();
    const expression = String(args.expression ?? '');
    const thread = this.#frameThread(args.frameId);
    if (args.context === 'repl') {
      this.#audited({ type: 'command', text: expression });
      return this.#repl(expression, thread);
    }
    this.#requireEvaluate(`evaluate (${args.context ?? 'watch'})`);
    this.#audited({ type: 'evaluate', context: args.context ?? 'watch', expression });
    const value = expressions.evaluate(expression, this.#scope(thread));
    if (args.context === 'clipboard') {
      return { body: { result: typeof value === 'string' ? value : JSON.stringify(value, null, 2) ?? String(value),
                       variablesReference: 0 } };
    }
    return { body: this.#variables.present(value) };
  },

  completions(args) {
    this.#requireAttached();
    const thread = this.#frameThread(args.frameId);
    const columnsFrom = this.#client.columnsStartAt1 === false ? 0 : 1;
    const column = Number(args.column) - columnsFrom;
    const view = this.#inspect(thread);
    const targets = commands.completions(String(args.text ?? ''), column, {
      events: () => (view === undefined ? [] : this.#labels.events(view.definitionID)),
      states: () => (view === undefined ? [] : this.#labels.states(view.definitionID).map((state) => state.name)),
      sendIDs: () => (view === undefined ? [] : view.delayedSends.map((send) => send.id).filter((id) => id !== null)),
      data: () => view?.data,
    });
    return { body: { targets: targets.map((target) => ({ ...target, start: target.start + columnsFrom })) } };
  },

  exceptionInfo(args) {
    const thread = this.#threadArgument(args);
    const stop = this.#stop;
    if (stop === null || stop.exception === undefined || thread.id !== ATTACHED) {
      throw refuse(`${thread.label} did not stop for an exception`);
    }
    const exception = stop.exception;
    return { body: {
      exceptionId: exception.name ?? exception.filter,
      description: stop.description,
      breakMode: 'always',
      details: { message: stop.description, typeName: exception.filter,
                 ...(exception.data === undefined ? {} : { innerException: [{ message: String(exception.data) }] }) },
    } };
  },

  pause(args) {
    const attached = this.#requireLive('hold', 'pause');
    this.#threadArgument(args);
    if (this.#stop !== null) return {};
    this.#audited({ type: 'mutation', operation: 'hold' });
    const now = this.#system.hold(attached.actor);
    if (now) {
      return { after: () => this.#halt('pause', { description: 'paused between macrosteps' }) };
    }
    this.#pending = 'pause';
    return { after: () => this.#output('console', `${attached.label} is inside a macrostep; it stops when the macrostep ` +
                                                  'ends (a macrostep that never ends needs `abandon`)') };
  },

  continue() {
    this.#requireLive('hold', 'continue');
    if (this.#stop === null && this.#pending === null) return { body: { allThreadsContinued: false } };
    this.#audited({ type: 'mutation', operation: 'release' });
    this.#release();
    return { body: { allThreadsContinued: false } };
  },

  next(args) {
    return this.#stepRequest(args);
  },

  stepIn(args) {
    return this.#stepRequest(args);
  },

  stepOut(args) {
    return this.#stepRequest(args);
  },

  gotoTargets() {
    const attached = this.#requireAttached();
    const definition = this.#system.inspect(attached.actor).definitionID;
    const targets = this.#labels.states(definition)
      .map((state) => {
        const id = this.#gotoTargets.size + 1;
        this.#gotoTargets.set(id, { definition: String(definition), state: state.name });
        return { id, label: state.name, line: 0 };
      });
    return { body: { targets } };
  },

  goto(args) {
    const attached = this.#requireLive('goto', 'goto');
    const target = this.#gotoTargets.get(Number(args.targetId));
    if (target === undefined) throw refuse(`no goto target ${args.targetId}; ask gotoTargets first`);
    if (target.definition !== String(this.#system.inspect(attached.actor).definitionID)) {
      throw refuse(`${attached.label} runs another revision than the target's`);
    }
    const { text, then } = this.#jump([target.state]);
    return { after: () => { this.#output('console', text); then?.(); } };
  },

  setBreakpoints(args) {
    const lines = Array.isArray(args.breakpoints) ? args.breakpoints : (args.lines ?? []).map((line) => ({ line }));
    return { body: { breakpoints: lines.map(() => ({
      verified: false, message: 'line breakpoints need source spans (core ADR 0086); use function breakpoints',
    })) } };
  },

  setFunctionBreakpoints(args) {
    const specs = Array.isArray(args.breakpoints) ? args.breakpoints : [];
    const attached = this.#attached;
    const definition = attached === null ? undefined : this.#system.inspect(attached.actor).definitionID;
    this.#audited({ type: 'breakpoints', kind: 'function',
                    breakpoints: specs.map(({ name, condition, hitCondition }) => ({ name, condition, hitCondition })) });
    const answered = this.#breakpoints.setFunctions(specs, (breakpoint) => this.#checkBreakpoint(breakpoint, definition));
    this.#updateGate();
    return { body: { breakpoints: answered } };
  },

  setExceptionBreakpoints(args) {
    const filters = [...(args.filters ?? []), ...(args.filterOptions ?? []).map((option) => option.filterId)];
    const unique = [...new Set(filters)];
    this.#breakpoints.setFilters(unique);
    this.#updateGate();
    return { body: { breakpoints: unique.map(() => ({ verified: true })) } };
  },

  dataBreakpointInfo(args) {
    this.#requireAttached();
    const name = String(args.name ?? '');
    let target;
    if (args.variablesReference !== undefined) {
      target = this.#variables.location(args.variablesReference, name);
    } else {
      const path = expressions.pathOf(name);
      target = path === undefined ? undefined : { thread: this.#frameThread(args.frameId), path };
    }
    if (target === undefined) {
      return { body: { dataId: null, description: `${name} is not a data-model location` } };
    }
    return { body: {
      dataId: Breakpoints.dataID(target.thread.label, target.path),
      description: `${target.thread.label} ${expressions.locationText(target.path)}`,
      accessTypes: ['write'],
      canPersist: true,
    } };
  },

  setDataBreakpoints(args) {
    const specs = Array.isArray(args.breakpoints) ? args.breakpoints : [];
    this.#audited({ type: 'breakpoints', kind: 'data',
                    breakpoints: specs.map(({ dataId, condition, hitCondition }) => ({ dataId, condition, hitCondition })) });
    const answered = this.#breakpoints.setData(specs, (breakpoint) => {
      if (![...this.#threads.values()].some((thread) => thread.label === breakpoint.label)) {
        return `no thread is ${breakpoint.label}`;
      }
      return this.#checkCondition(breakpoint);
    });
    this.#updateGate();
    return { body: { breakpoints: answered } };
  },

  modules(args) {
    if (this.#attached !== null) this.#refreshCatalog();
    const all = [...this.#modules.values()];
    const start = Number(args.startModule ?? 0);
    const count = Number(args.moduleCount ?? 0);
    return { body: { modules: all.slice(start, count > 0 ? start + count : undefined), totalModules: all.length } };
  },

  loadedSources() {
    if (this.#attached !== null) this.#refreshCatalog();
    return { body: { sources: [...this.#sources.values()].map((entry) => entry.source) } };
  },

  source(args) {
    const reference = Number(args.sourceReference ?? args.source?.sourceReference ?? 0);
    const entry = this.#sources.get(reference);
    if (entry === undefined) throw refuse('no such source');
    const spec = core.definition_export(this.#native, entry.definition);
    return { body: { content: exportText(spec), mimeType: 'application/json' } };
  },
    };
  }
}

module.exports = { DebugSession, createDebugSession, DAPReader, encodeDAPMessage, CAPABILITIES };

function createDebugSession(system, options) {
  return new DebugSession(system, options);
}

// { family, name } from an address in one of its spellings, or undefined.
function parseAddress(address) {
  if (address !== null && typeof address === 'object') {
    return typeof address.family === 'string' && typeof address.name === 'string' && address.family !== '' && address.name !== ''
      ? { family: address.family, name: address.name } : undefined;
  }
  if (typeof address !== 'string') return undefined;
  const text = address.startsWith('#_actor_') ? address.slice('#_actor_'.length) : address;
  const slash = text.indexOf('/');
  return slash > 0 && slash < text.length - 1 ? { family: text.slice(0, slash), name: text.slice(slash + 1) } : undefined;
}

/*
 * The resident actor that holds session `wanted` (a bigint), or undefined.
 * A session ID is the runtime ID its session was spawned with (core ADR
 * 0066), so the actor that still has that ID is tried first and checked;
 * one reloaded since has another ID, and `walk` looks through every
 * resident actor for it.
 */
function actorOfSession(system, wanted, walk = true) {
  const guess = new ActorID(system, Number(wanted & 0xffffffffn), Number(wanted >> 32n));
  try {
    if (system.inspect(guess).sessionID === wanted) return guess;
  } catch (error) {
    if (!(error instanceof TinyactorsError && (error.code === 'not-found' || error.code === 'invalid'))) throw error;
  }
  if (!walk) return undefined;
  for (const view of system.actors()) {
    if (view.sessionID === wanted) return view.actor;
  }
  return undefined;
}

// A definition as the loadedSource the client can open.
function sourceOf(definition, revision, reference) {
  return { name: `${definition.identity.family} ${revision}.json`, sourceReference: reference, origin: 'definition export',
           presentationHint: definition.retired ? 'deemphasize' : 'normal' };
}

// A definition's export as text: JavaScript functions as their source.
function exportText(spec) {
  return JSON.stringify(spec, (key, value) => {
    if (typeof value === 'function') return String(value);
    if (typeof value === 'bigint') return String(value);
    if (value instanceof Uint8Array) {
      try {
        return new TextDecoder('utf-8', { fatal: true }).decode(value);
      } catch {
        return [...value];
      }
    }
    return value;
  }, 2);
}
