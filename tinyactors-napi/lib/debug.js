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
// Modes:
// - `watch` never stops the actor and changes nothing; breakpoints report
//   where they would have stopped.
// - `fork` copies the actor into a debug System of its own (core ADR 0085,
//   napi ADR 0026) and debugs the copy: it stops anywhere, steps one unit
//   at a time, moves its own clock, and steps back to images captured at
//   its stops. Nothing it does reaches production.
// - `live` works on the actor itself, as the permissions allow: `send`,
//   `set`, `goto` (with abandon, cancel and replace), `hold` (pause,
//   stepping and breakpoints that stop), and `evaluate`. A live actor stops
//   only between macrosteps, so a send() waiting for a macrostep is never
//   left waiting by a debugger, and a hold ends by itself after
//   `holdTimeout`.
//
// Evaluating an expression runs arbitrary code in the process that hosts
// the actor, a fork's copy included. It is the `evaluate` permission, off
// unless granted: without it the Variables pane and the console commands
// still work.
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
const { frames, leaves, depthOf, blockOf } = require('./debug/frames');
const { Sources } = require('./debug/sources');
const { Fork } = require('./debug/fork');
const expressions = require('./debug/expressions');
const commands = require('./debug/console');
const { DAPReader, encodeDAPMessage } = require('./debug/protocol');

const MODES = ['watch', 'fork', 'live'];
const PERMISSIONS = ['send', 'set', 'goto', 'hold', 'evaluate'];
// On a fork's copy everything is allowed but evaluating, which runs code in
// the process whatever it evaluates against.
const FORK_PERMISSIONS = ['send', 'set', 'goto', 'hold'];
const DEFAULT_HOLD_TIMEOUT = 30000;
const SCXML_INVOKER = 'http://www.w3.org/TR/scxml/';
const ATTACHED = 1; // the attached actor's thread ID
const OPERATOR_EVENT = 'operator.goto';
// A fork's copy runs one unit per resume while the debugger drives it.
const SINGLE_STEP = Object.freeze({ resumeBudget: 1, actionBudget: 1, microstepStart: true });
// Units a fork's copy runs toward one goal before it stops anyway.
const MOST_UNITS = 100000;
// Stops a fork remembers for stepping back.
const MOST_POINTS = 256;
const DURATION = /^\+(\d+(?:\.\d+)?)(ms|s|m|h|d)?$/;
const SCALE = { ms: 1, s: 1000, m: 60000, h: 3600000, d: 86400000 };

const CAPABILITIES = Object.freeze({
  supportsConfigurationDoneRequest: true,
  supportsFunctionBreakpoints: true,
  supportsConditionalBreakpoints: true,
  supportsHitConditionalBreakpoints: true,
  supportsLogPoints: true,
  supportsEvaluateForHovers: true,
  exceptionBreakpointFilters: EXCEPTION_FILTERS.map(({ filter, label, description }) => ({ filter, label, description })),
  supportsStepBack: true,
  supportsSetVariable: true,
  supportsRestartFrame: true,
  supportsGotoTargetsRequest: true,
  supportsCompletionsRequest: true,
  completionTriggerCharacters: ['.', ' '],
  supportsModulesRequest: true,
  supportsRestartRequest: true,
  supportsExceptionInfoRequest: true,
  supportsDelayedStackTraceLoading: true,
  supportsLoadedSourcesRequest: true,
  supportsSetExpression: true,
  supportsTerminateRequest: true,
  supportsDataBreakpoints: true,
  supportsBreakpointLocationsRequest: true,
  supportsClipboardContext: true,
  supportsSteppingGranularity: true,
  supportTerminateDebuggee: false,
  supportSuspendDebuggee: false,
});

// Requests the adapter answers with an error, and why. Their capabilities
// are not declared.
const REFUSED = Object.freeze({
  launch: 'napi\'s adapter attaches to a running actor: use "attach" with an address (the test host launches workbenches)',
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
const plural = (count, word) => `${count} ${word}${count === 1 ? '' : 's'}`;

let sessionCount = 0;
// The request handlers, by command; set in the class's static block, where
// they can reach its private members.
let HANDLERS;

class DebugSession {
  #system;
  #mode;
  #allow;
  #audit;
  #send;
  #holdTimeout;
  #seq = 0;
  #queue = Promise.resolve();
  #client = {};
  #closed = false;

  // While attached: { mode, allow, system, actor, label, address,
  // sessionID, labels, sources, fork, observer, watch, console, origin }.
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
  #sourceRefs = new Map();
  #files = new Set();

  // Holding: where the actor stopped, what the next hold means (live), the
  // goal a fork's copy runs toward, and whether it is held after each
  // macrostep (live) or unit (fork) for the breakpoints.
  #stop = null;
  #pending = null;
  #run = null;
  #gate = false;
  #hits = [];
  #positionHits = new Set();
  #skipTaking = false;
  #holdTimer;
  #points = [];
  // A new copy at rest settles in the next turn; that stop is not a step's.
  #settling = false;

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
      throw new TinyactorsError('createDebugSession(): options.send must be a function that takes each outgoing message',
                                'invalid');
    }
    if (audit !== undefined && typeof audit !== 'function') {
      throw new TinyactorsError('createDebugSession(): options.audit must be a function', 'invalid');
    }
    if (!(typeof holdTimeout === 'number' && holdTimeout > 0 && holdTimeout <= 2147483647)) {
      throw new TinyactorsError('createDebugSession(): options.holdTimeout must be a positive number of milliseconds',
                                'invalid');
    }
    this.#system = system;
    this.#mode = mode;
    this.#allow = new Set(allow);
    this.#audit = audit;
    this.#send = send;
    this.#holdTimeout = holdTimeout;
    sessionCount += 1;
    this.id = sessionCount;
    this.#macrosteps = new Macrosteps((session) => this.#threadOfSession(session) !== undefined);
    const session = this;
    this.#variables = new Variables({
      get system() { return session.#attached?.system ?? session.#system; },
      get native() { return nativeOf(this.system); },
      get labels() { return session.#attached.labels; },
      thread: (id) => this.#thread(id),
      eventOf: (thread, view) => this.#eventOf(thread, view),
      mailboxNote: (message) => this.#mailboxNote(message),
      threadOfSession: (sessionID) => this.#threadOfSession(sessionID),
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

  /** The transport closed: detach, releasing a hold and closing a fork, without answering. */
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
    try {
      after?.();
    } catch (error) {
      this.#output('stderr', `${command}: ${error?.message ?? error}`);
    }
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
      ...(attached === null ? {}
        : { actor: attached.origin.label, sessionID: attached.origin.sessionID, mode: attached.mode }),
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

  // An operation that changes or stops the actor: live with the permission,
  // or on a fork's copy.
  #requireChange(permission, what) {
    const attached = this.#requireAttached();
    if (attached.mode === 'watch') {
      throw refuse(`${what} is not offered in watch mode, which never changes or stops the actor`);
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

  #requireFork(what) {
    const attached = this.#requireAttached();
    if (attached.mode !== 'fork') {
      throw refuse(`${what} restores images of a fork's copy: attach in fork mode, or use the \`fork\` console command`);
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
    const asked = Array.isArray(args.allow) ? new Set(args.allow) : this.#allow;
    const granted = new Set([...this.#allow].filter((permission) => asked.has(permission)));
    const { actor, address } = this.#resolve(args);
    const view = this.#system.inspect(actor);
    const label = address !== undefined ? `${address.family}/${address.name}` : `actor ${actor.slot}:${actor.generation}`;
    const origin = { system: this.#system, actor, address, label, sessionID: String(view.sessionID), args };
    const allow = mode === 'fork' ? new Set([...FORK_PERMISSIONS, ...[...granted].filter((entry) => entry === 'evaluate')])
      : granted;
    const attached = { mode, allow, origin, system: this.#system, actor, label, address, sessionID: origin.sessionID };
    this.#attached = attached;
    try {
      this.#audited({ type: 'attach', allow: [...allow] });
      if (mode === 'fork') {
        attached.fork = new Fork(this.#system, actor, {
          record: (text) => this.#output('console', `fork: ${text}`),
          log: (record) => this.#onLog(record),
        });
        attached.system = attached.fork.system;
        attached.actor = attached.fork.copy;
        attached.label = `${label} (fork)`;
        attached.address = undefined;
        attached.sessionID = String(attached.system.inspect(attached.actor).sessionID);
      }
    } catch (error) {
      this.#attached = null;
      throw error instanceof TinyactorsError ? refuse(error.message) : error;
    }
    const native = nativeOf(attached.system);
    attached.labels = new Labels(native);
    attached.sources = new Sources(attached.system, native);
    this.#threads.set(ATTACHED, { id: ATTACHED, actor: attached.actor, label: attached.label, session: attached.sessionID });
    attached.observer = observe(attached.system, {
      traces: (traces) => this.#onTraces(traces),
      log: mode === 'fork' ? undefined : (record) => this.#onLog(record),
      held: (held) => this.#onHeld(held),
      defined: (id) => this.#onDefined(id),
    });
    attached.watch = attached.system.watchTraces({ sessions: [attached.actor] });
    if (mode === 'fork' || (mode === 'live' && (allow.has('send') || allow.has('goto')))) {
      attached.console = commands.openConsole(attached.system, `debug://session/${this.id}`,
        (request) => this.#onReply(request));
    }
    if (mode === 'fork') {
      this.#holdCopy(attached.actor);
    }
    return attached;
  }

  // A new copy stands still where it was captured. One captured at rest is
  // runnable only to be scheduled: it settles in the next turn, before any
  // step, so a step takes its next message.
  #holdCopy(actor) {
    const { system } = this.#attached;
    system.setBudgets(actor, SINGLE_STEP);
    system.hold(actor, { anywhere: true });
    if (!system.inspect(actor).macrostepInProgress) {
      system.step(actor);
      this.#settling = true;
    }
  }

  // What attaching says, and what it sets going, once its response is out.
  #announce(attached, verb) {
    this.#event('process', { name: attached.origin.label, startMethod: 'attach', isLocalProcess: true });
    this.#refreshCatalog();
    this.#refreshThreads();
    const permissions = attached.allow.size === 0 ? 'no permissions' : `allowed: ${[...attached.allow].join(', ')}`;
    this.#output('console', `${verb} ${attached.origin.label} (session ${attached.origin.sessionID}) in ${attached.mode} ` +
                            `mode; ${permissions}`);
    if (attached.fork !== undefined) {
      for (const note of attached.fork.notes) this.#output('console', `fork: ${note}`);
      this.#output('console', `fork: the copy is ${attached.label}, on a clock of its own at t=${attached.system.time}ms`);
    }
    this.#relocateBreakpoints();
    this.#updateGate();
    if (attached.fork !== undefined) {
      this.#halt('entry', { description: 'the copy stands where the live actor stood' });
    }
  }

  // Ends the attachment: releases what this session holds, stops watching,
  // closes a fork.
  #detach(reason) {
    const attached = this.#attached;
    if (attached === null) return;
    this.#clearHoldTimer();
    const system = attached.system;
    if (attached.fork === undefined && !system.closed && system.exists(attached.actor) &&
        (this.#gate || this.#pending !== null || this.#stop !== null)) {
      system.unhold(attached.actor);
    }
    this.#gate = false;
    this.#pending = null;
    this.#run = null;
    this.#stop = null;
    this.#hits = [];
    attached.observer?.stop();
    if (!system.closed) {
      attached.watch?.stop();
      attached.console?.close();
    }
    for (const point of this.#points) point.image.destroy();
    this.#points = [];
    attached.fork?.close();
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
    const system = this.#attached?.system ?? this.#system;
    try {
      return system.inspect(thread.actor);
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
      const child = actorOfSession(attached.system, invocation.serviceHandle, false);
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
      : leaves(view, this.#attached.labels).join(' ') || 'starting';
    return `${thread.label} · ${where}`;
  }

  // The attached actor changed identity (a fork restored an image): its
  // thread follows it, held where it stands.
  #follow(actor) {
    const attached = this.#attached;
    attached.actor = actor;
    const thread = this.#threads.get(ATTACHED);
    thread.actor = actor;
    thread.session = String(attached.system.inspect(actor).sessionID);
    attached.sessionID = thread.session;
    this.#holdCopy(actor);
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
    const attached = this.#attached;
    if (gone === 'finished' && attached !== null && attached.system.exists(attached.actor)) {
      this.#output('important', `${attached.label} finished`);
    } else if (gone !== undefined && attached?.fork === undefined) {
      this.#ended(gone === 'unloaded' ? 'was unloaded' : gone === 'finished' ? 'finished' : 'was destroyed');
      return;
    }
    this.#refreshThreads();
    this.#refreshCatalog();
  }

  #onMacrostep(macrostep) {
    const attached = this.#attached;
    const thread = this.#threadOfSession(macrostep.session);
    if (thread === undefined) return;
    if (macrostep.event !== null) this.#lastEvent.set(macrostep.session, macrostep.event);
    const last = macrostep.microsteps.at(-1);
    if (last !== undefined) this.#lastMicrostep.set(macrostep.session, { macrostep: macrostep.number, microstep: last });
    const location = (definition, id) => attached.labels.expression(BigInt(definition), id);
    for (const line of summarize(macrostep, thread.label, location)) {
      this.#output('console', line);
    }
    if (thread.id !== ATTACHED) return;
    const skipTaking = this.#skipTaking;
    this.#skipTaking = false;
    const skip = this.#positionHits;
    this.#positionHits = new Set();
    const hits = this.#breakpoints.hits(macrostep, {
      label: thread.label,
      location: (id) => {
        const text = location(macrostep.definition, id);
        return text === undefined ? undefined : expressions.pathOf(text);
      },
      fires: (breakpoint) => this.#condition(breakpoint, thread),
      log: (breakpoint) => this.#logpoint(breakpoint, thread),
      skipTaking,
      skip,
    });
    if (hits.length === 0) return;
    if (this.#gate) {
      this.#hits.push(...hits.map((hit) => ({ ...hit, macrostep: macrostep.number })));
    } else {
      const why = attached.mode === 'watch' ? '' : ' (stopping needs the "hold" permission)';
      for (const hit of hits) {
        this.#output('console', `would have stopped after macrostep ${macrostep.number}: ${hit.description}${why}`);
      }
    }
  }

  // A breakpoint's condition, in the data model where it hit. One that
  // throws stops, so its author sees it fail.
  #condition(breakpoint, thread) {
    try {
      return Boolean(expressions.evaluate(breakpoint.condition, this.#scope(thread)));
    } catch (error) {
      this.#output('stderr', `the condition of breakpoint ${breakpoint.id} failed: ${error.message}`);
      return true;
    }
  }

  // A logpoint writes its message: {expression} parts are evaluated with
  // the `evaluate` permission, and left as written without it.
  #logpoint(breakpoint, thread) {
    const evaluate = this.#attached.allow.has('evaluate');
    const text = String(breakpoint.logMessage).replace(/\{([^{}]+)\}/g, (whole, expression) => {
      if (!evaluate) return whole;
      try {
        const value = expressions.evaluate(expression, this.#scope(thread));
        return typeof value === 'string' ? value : jsonText(value);
      } catch (error) {
        return `<${error.message}>`;
      }
    });
    this.#output('console', text);
  }

  #onLog(record) {
    const attached = this.#attached;
    if (attached === null) return;
    const source = record.source;
    // A fork's copy is reconstructed from an image of the live actor, so
    // its records name that actor's address: no other actor of the fork's
    // system has it.
    const address = attached.fork === undefined ? attached.address : attached.origin.address;
    const ours = source instanceof ActorID ? source.equals(attached.actor)
      : address !== undefined && source?.family === address.family && source?.name === address.name;
    if (!ours) return;
    const value = record.hasValue ? ` ${typeof record.value === 'string' ? record.value : jsonText(record.value)}` : '';
    this.#output('stdout', `${record.label ?? 'log'}:${value}`);
  }

  #onReply(request) {
    const data = request.data === undefined ? '' : ` ${jsonText(request.data)}`;
    this.#output('console', `${this.#attached?.label ?? 'an actor'} replied ${request.event}${data}`);
  }

  // ------------------------------------------------------------------
  // Holding (napi ADR 0027)

  // A hold stopped the attached actor; the traces of the turn were read
  // already.
  #onHeld(actor) {
    const attached = this.#attached;
    if (attached === null || !actor.equals(attached.actor)) return;
    if (this.#settling) {
      // A new copy settled: a step asked for meanwhile runs now.
      this.#settling = false;
      if (this.#stop === null && (this.#run !== null || this.#gate)) this.#stepCopy();
      return;
    }
    if (this.#stop !== null) return;
    if (attached.fork !== undefined) {
      this.#heldCopy();
      return;
    }
    const hits = this.#hits;
    this.#hits = [];
    if (this.#pending !== null) {
      const reason = this.#pending;
      this.#pending = null;
      const description = reason === 'goto' ? 'the jump\'s macrostep ended'
        : reason === 'step' ? 'the macrostep ended' : 'paused';
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

  // A live actor held for the breakpoints, and nothing hit: stop before the
  // next message when an `on:` breakpoint names it, and let it take the
  // message otherwise.
  #advance() {
    const { system, actor } = this.#attached;
    const [next] = system.mailbox(actor, { limit: 1 });
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
    system.step(actor);
  }

  // A fork's copy stood still after a unit, a delivery, or its macrostep's
  // end: stop there, or run on toward the goal and the breakpoints.
  #heldCopy() {
    const attached = this.#attached;
    const view = attached.system.inspect(attached.actor);
    const thread = this.#threads.get(ATTACHED);
    const hits = this.#hits;
    this.#hits = [];
    if (this.#gate || this.#run !== null) {
      hits.push(...this.#breakpoints.atPosition(view, {
        transition: (id) => attached.labels.transition(view.definitionID, id),
        fires: (breakpoint) => this.#condition(breakpoint, thread),
        log: (breakpoint) => this.#logpoint(breakpoint, thread),
        passed: (id) => this.#positionHits.add(id),
      }));
    }
    const run = this.#run;
    if (hits.length > 0) {
      this.#run = null;
      this.#halt(undefined, { hits });
      return;
    }
    if (run !== null) {
      run.units += 1;
      const done = run.done(view);
      if (done || run.units >= MOST_UNITS) {
        this.#run = null;
        this.#halt(run.reason, { description: done ? run.description
          : `${MOST_UNITS} units ran without reaching the step's end` });
        return;
      }
    } else if (!this.#gate) {
      this.#halt('pause', { description: 'paused' });
      return;
    }
    this.#stepCopy();
  }

  // The copy runs one more unit, or takes its next message. A copy that is
  // still settling steps once it has.
  #stepCopy() {
    const { system, actor, label } = this.#attached;
    if (system.inspect(actor).scheduling !== 'held') return;
    if (system.step(actor) === 'waiting') {
      const view = system.inspect(actor);
      const [next] = view.delayedSends;
      const hint = next === undefined ? 'nothing is due'
        : `its next delayed send is due at t=${next.due}ms: \`clock +${Math.max(next.due - system.time, 0)}ms\``;
      this.#output('console', `${label} waits for its next message; ${hint}, or \`send\` it one`);
    }
  }

  // The actor is held: tell the client, keep the place (fork), and arm the
  // hold's timeout (live).
  #halt(reason, { description, hits = [] }) {
    const attached = this.#attached;
    const hit = hits[0];
    const kind = reason ?? (hit.kind === 'exception' ? 'exception' : hit.kind === 'data' ? 'data breakpoint'
      : hit.kind === 'line' ? 'breakpoint' : 'function breakpoint');
    const text = [description, ...hits.map((entry) => entry.description)].filter((part) => part !== undefined).join('; ');
    this.#stop = { reason: kind, description: text, exception: hits.find((entry) => entry.kind === 'exception')?.exception,
                   breakpoints: hits.flatMap((entry) => entry.breakpoints), taking: text.includes('about to take') };
    this.#resetViews();
    if (attached.fork !== undefined) {
      this.#keepPoint(hits.length > 0);
    }
    const body = { reason: kind, threadId: ATTACHED, allThreadsStopped: false, preserveFocusHint: false };
    if (text !== '') body.description = text;
    if (this.#stop.exception !== undefined) body.text = hit?.description;
    if (this.#stop.breakpoints.length > 0) body.hitBreakpointIds = this.#stop.breakpoints;
    this.#event('stopped', body);
    this.#clearHoldTimer();
    if (attached.fork === undefined) {
      this.#holdTimer = setTimeout(() => this.#holdExpired(), this.#holdTimeout);
      this.#holdTimer.unref?.();
    }
  }

  // A fork remembers each stop as an image, to step back to it.
  #keepPoint(breakpoint) {
    const { fork, system, actor } = this.#attached;
    let point;
    try {
      point = fork.capture();
    } catch (error) {
      this.#output('stderr', `fork: this stop cannot be kept for stepping back: ${error.message}`);
      return;
    }
    const view = system.inspect(actor);
    this.#points.push({ ...point, macrostep: view.macrostepInProgress ? view.step.macrostep : 0, breakpoint });
    while (this.#points.length > MOST_POINTS) this.#points.shift().image.destroy();
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
    this.#run = null;
    this.#clearHoldTimer();
    this.#resetViews();
    if (attached === null || attached.system.closed || !attached.system.exists(attached.actor)) return;
    const { system, actor } = attached;
    if (attached.fork !== undefined) {
      if (this.#gate) {
        this.#stepCopy();
      } else {
        system.setBudgets(actor, undefined);
        system.unhold(actor);
      }
      return;
    }
    if (this.#gate) {
      // A stop before an `on:` breakpoint's message lets that message through.
      this.#skipTaking = stopped?.taking === true;
      if (system.inspect(actor).scheduling === 'held') this.#advance();
    } else {
      system.unhold(actor);
    }
  }

  // Holds after each macrostep (live, with `hold`) or unit (fork) while
  // breakpoints are set, so they can stop the actor.
  #updateGate() {
    const attached = this.#attached;
    const wanted = attached !== null && attached.mode !== 'watch' && attached.allow.has('hold') && this.#breakpoints.any();
    if (wanted === this.#gate) return;
    this.#gate = wanted;
    if (this.#stop !== null || attached === null || !attached.system.exists(attached.actor)) return;
    const { system, actor } = attached;
    if (attached.fork !== undefined) {
      if (wanted) {
        system.setBudgets(actor, SINGLE_STEP);
        if (system.hold(actor, { anywhere: true })) this.#stepCopy();
      }
      return;
    }
    if (wanted) {
      if (system.hold(actor)) this.#advance();
    } else if (this.#pending === null) {
      system.unhold(actor);
    }
  }

  // After an operation outside a turn (abandon): a pending hold may have
  // taken effect at once.
  #checkHeld() {
    const attached = this.#attached;
    if (attached !== null && this.#stop === null && attached.system.exists(attached.actor) &&
        attached.system.inspect(attached.actor).scheduling === 'held') {
      this.#onHeld(attached.actor);
    }
  }

  // ------------------------------------------------------------------
  // Stepping a fork's copy

  // Runs the copy toward a goal: `done(view)` says when it is there.
  #runCopy(reason, done, description) {
    this.#stop = null;
    this.#clearHoldTimer();
    this.#resetViews();
    // Between two microsteps is no place to stop a step: the next
    // microstep's start (core ADR 0087), or the macrostep's end, comes right
    // after it.
    const goal = reason === 'step'
      ? (view) => !(view.macrostepInProgress && view.position.at === 'none' && view.step.microstep > 0) && done(view)
      : done;
    this.#run = { reason, done: goal, description, units: 0 };
    this.#stepCopy();
  }

  // The goal of a step on a fork's copy, from where it stands (core ADR
  // 0083), as the proposal's stepping table has it. At `statement`
  // granularity, stepIn and next pass over a stop that names the unit they
  // started at: a <foreach> that has not run and the same loop before its
  // first iteration have one position (ADR 0083, open point 1), and a
  // debugger that stops twice at one place looks stuck. `instruction`
  // stops there anyway.
  #stepGoal(kind, granularity, start) {
    const labels = this.#attached.labels;
    const macro = start.step.macrostep;
    const ended = (view) => !view.macrostepInProgress || view.step.macrostep !== macro;
    const otherStep = (view) => ended(view) || view.step.microstep !== start.step.microstep;
    if (!start.macrostepInProgress || macro === 0) {
      // At rest, or holding an event not taken yet: the next stop is the step.
      return () => true;
    }
    if (granularity === 'line') {
      return kind === 'out' ? ended : otherStep;
    }
    if (kind !== 'out' && granularity === 'instruction') {
      return () => true;
    }
    const same = (view) => !otherStep(view) && samePosition(view.position, start.position);
    if (kind === 'in') {
      return (view) => !same(view);
    }
    const block = blockOf(start);
    const depth = depthOf(start, labels);
    if (kind === 'over') {
      return (view) => otherStep(view) || blockOf(view) !== block || (!same(view) && depthOf(view, labels) <= depth);
    }
    if (start.position.at === 'none') return ended;
    return (view) => otherStep(view) || blockOf(view) !== block || depthOf(view, labels) < depth;
  }

  // Puts the copy back at the kept stop `index`, which becomes the latest.
  #restore(index, reason, description) {
    const attached = this.#attached;
    const point = this.#points[index];
    for (const later of this.#points.splice(index + 1)) later.image.destroy();
    this.#points.pop();
    this.#stop = null;
    this.#run = null;
    this.#follow(attached.fork.restore(point));
    point.image.destroy();
    this.#halt(reason, { description });
  }

  // ------------------------------------------------------------------
  // Views of the actor

  #resetViews() {
    this.#variables.reset();
    this.#frames.clear();
  }

  #documentName(view) {
    const system = this.#attached?.system ?? this.#system;
    try {
      return core.definition_inspect(nativeOf(system), view.definitionID).document_name || view.definition.family;
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
    const console = scope !== undefined && message.scope?.index === scope.index &&
      message.scope?.generation === scope.generation;
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
  // Modules, sources and line breakpoints (core ADR 0086)

  #refreshCatalog() {
    const attached = this.#attached;
    const seen = new Set();
    for (const definition of attached.system.definitions()) {
      const key = String(definition.id);
      seen.add(key);
      const revision = typeof definition.identity.revision === 'string' ? definition.identity.revision
        : [...definition.identity.revision].map((byte) => byte.toString(16).padStart(2, '0')).join('');
      const module = { id: key, name: `${definition.identity.family}${definition.retired ? ' (retired)' : ''}`,
                       version: revision };
      const known = this.#modules.get(key);
      if (known === undefined) {
        this.#modules.set(key, module);
        const reference = this.#sourceRefs.size + 1;
        this.#sourceRefs.set(reference, { definition: definition.id,
                                          source: exportSource(definition, revision, reference) });
        this.#event('module', { reason: 'new', module });
        this.#event('loadedSource', { reason: 'new', source: this.#sourceRefs.get(reference).source });
        for (const path of attached.sources.files(definition.id)) {
          if (!this.#files.has(path)) {
            this.#files.add(path);
            this.#event('loadedSource', { reason: 'new', source: Sources.source({ path }) });
          }
        }
      } else if (known.name !== module.name) {
        this.#modules.set(key, module);
        this.#event('module', { reason: 'changed', module });
      }
    }
    for (const [key, module] of [...this.#modules]) {
      if (!seen.has(key)) {
        this.#modules.delete(key);
        this.#event('module', { reason: 'removed', module });
        for (const [reference, entry] of [...this.#sourceRefs]) {
          if (String(entry.definition) === key) {
            this.#sourceRefs.delete(reference);
            this.#event('loadedSource', { reason: 'removed', source: entry.source });
          }
        }
      }
    }
  }

  // The element a line of `path` stands for, in the attached actor's
  // chart, or why there is none.
  #resolveLine(path, line) {
    const attached = this.#attached;
    if (attached === null) return 'attach first: lines are resolved against the attached actor\'s chart';
    const definition = attached.system.inspect(attached.actor).definitionID;
    const found = attached.sources.locate(definition, path, line);
    if (found === undefined) {
      return attached.sources.files(definition).length === 0
        ? 'the chart keeps no source spans (core ADR 0086): statechart() records them'
        : `no state, transition or action of ${attached.label}'s chart at or after line ${line} of this file`;
    }
    const name = found.kind === 'state' ? attached.labels.state(definition, found.id)
      : found.kind === 'transition' ? attached.labels.transition(definition, found.id)
        : attached.labels.action(definition, found.id);
    return { element: { kind: found.kind, id: found.id, name }, line: found.line, column: found.column,
             source: Sources.source(found) };
  }

  // A chart defined again: its spans may have moved (core ADRs 0077 and
  // 0086), so its lines are read again and the breakpoints placed again.
  #onDefined(id) {
    if (this.#attached === null) return;
    this.#attached.sources.forget(id);
    this.#relocateBreakpoints();
  }

  // Line breakpoints set before attaching are resolved against the
  // attached actor's chart, and the client hears where they went.
  #relocateBreakpoints() {
    for (const { path } of this.#breakpoints.lineRequests) {
      for (const breakpoint of this.#breakpoints.relocate(path, (line) => this.#resolveLine(path, line))) {
        this.#event('breakpoint', { reason: 'changed', breakpoint });
      }
    }
  }

  // ------------------------------------------------------------------
  // Console operations

  #post(event, data, options) {
    const { system, actor, console } = this.#attached;
    const origin = console === undefined ? undefined : { type: commands.TYPE, scope: console.scope };
    system.post(actor, event, data, { ...options, origin });
    const waiting = system.mailbox(actor).length;
    return this.#stop !== null
      ? `; it waits in the mailbox (${plural(waiting, 'message')}): continue or step to deliver it` : '';
  }

  #consoleSend({ event, data }) {
    const attached = this.#requireChange('send', 'send');
    this.#audited({ type: 'mutation', operation: 'send', event, data });
    const note = this.#post(event, data);
    return `sent ${event} to ${attached.label}${note}`;
  }

  #jump(targets, from) {
    const attached = this.#requireChange('goto', 'goto');
    const { system, actor, labels } = attached;
    const definition = system.inspect(actor).definitionID;
    for (const state of [...targets, ...(from === undefined ? [] : [from])]) {
      if (labels.stateID(definition, state) === undefined) throw refuse(`${state} is not a state of the chart`);
    }
    this.#audited({ type: 'mutation', operation: 'goto', targets, from });
    const data = { targets, ...(from === undefined ? {} : { from }) };
    const alone = this.#stop !== null && system.mailbox(actor).length === 0 && !system.inspect(actor).macrostepInProgress;
    const note = this.#post(OPERATOR_EVENT, data, { transition: { targets, source: from } });
    const text = `${attached.label}: goto ${targets.join(' ')}${from === undefined ? '' : ` from ${from}`}`;
    if (alone && attached.allow.has('hold')) {
      // Nothing waits before it: take the jump now and stop after it.
      const then = attached.fork !== undefined
        ? () => this.#runCopy('goto', (view) => !view.macrostepInProgress, 'the jump\'s macrostep ended')
        : () => this.#stepNow('goto');
      return { text, then };
    }
    return { text: `${text} queued${note}` };
  }

  async #abandon() {
    const attached = this.#requireChange('goto', 'abandon');
    const { system, actor } = attached;
    const view = system.inspect(actor);
    if (!view.macrostepInProgress) throw refuse(`${attached.label} has no macrostep to abandon`);
    this.#audited({ type: 'mutation', operation: 'abandon', macrostep: view.step.macrostep });
    if (view.microstepInProgress) {
      // Core abandons only between microsteps: let this one finish first.
      system.setBudgets(actor, { resumeBudget: 1, actionBudget: 0 });
      try {
        if (system.inspect(actor).scheduling === 'held') system.step(actor);
        for (let tries = 0; tries < 100 && system.exists(actor) && system.inspect(actor).microstepInProgress; tries += 1) {
          await nextTurn();
        }
      } finally {
        if (system.exists(actor)) system.setBudgets(actor, attached.fork === undefined ? undefined : SINGLE_STEP);
      }
      if (!system.exists(actor) || !system.inspect(actor).macrostepInProgress) {
        return `${attached.label} finished its macrostep while finishing its microstep`;
      }
    }
    try {
      system.abandon(actor);
    } catch (error) {
      if (error instanceof TinyactorsError && error.code === 'busy') {
        throw refuse(`${attached.label} has not finished its bootstrap, or is still inside a microstep; try again`);
      }
      throw error;
    }
    const text = `${attached.label}: abandoned macrostep ${view.step.macrostep}; it keeps the configuration after its ` +
                 'last completed microstep';
    if (attached.fork !== undefined && this.#stop !== null) {
      this.#stop = null;
      this.#halt('pause', { description: 'its macrostep was abandoned' });
    } else {
      this.#checkHeld();
    }
    return text;
  }

  #cancel({ id }) {
    const attached = this.#requireChange('goto', 'cancel');
    this.#audited({ type: 'mutation', operation: 'cancel', sendID: id });
    attached.system.withdrawSend(attached.actor, id);
    return `${attached.label}: withdrew send ${id}`;
  }

  async #replace({ revision }) {
    const attached = this.#requireChange('goto', 'replace');
    if (attached.fork !== undefined) {
      throw refuse('a fork\'s copy has no address to replace it at; restart makes a new copy');
    }
    if (attached.address === undefined) throw refuse('only a named actor can be replaced in place');
    const { system } = attached;
    const definition = system.findDefinition({ family: attached.address.family, revision });
    if (definition === undefined) throw refuse(`no registered, unretired ${attached.address.family} revision ${revision}`);
    const view = system.inspect(attached.actor);
    if (view.macrostepInProgress) throw refuse(`${attached.label} is inside a macrostep: \`abandon\` it first`);
    this.#audited({ type: 'mutation', operation: 'replace', revision });
    const dropped = view.delayedSends.filter((send) => !send.submitted).map((send) => send.id ?? send.event);
    const prepared = system.prepareReplacement(attached.actor, definition);
    let replacement;
    try {
      // The replacement bootstraps in a pump turn before it can be activated.
      for (let tries = 0; tries < 100 && system.inspect(prepared).scheduling === 'runnable'; tries += 1) {
        await nextTurn();
      }
      replacement = system.activate(prepared);
    } catch (error) {
      if (!system.closed && system.exists(prepared)) system.destroy(prepared);
      throw error;
    }
    // Follow the address to the replacement.
    const args = { address: attached.address, mode: attached.mode, allow: [...attached.allow] };
    this.#detach('replaced');
    const next = this.#attach(args);
    this.#refreshThreads();
    this.#updateGate();
    const lost = dropped.length === 0 ? '' : `; its delayed sends ${dropped.join(', ')} were dropped`;
    return `${next.label} now runs ${revision} from its initial state, with its data${lost} (actor ` +
           `${replacement.slot}:${replacement.generation})`;
  }

  async #clock({ duration }) {
    const attached = this.#requireAttached();
    if (attached.fork === undefined) {
      throw refuse('clock moves a fork\'s own clock; on a live actor it would move time for every actor of the system: ' +
                   'use the `fork` command first');
    }
    const match = DURATION.exec(String(duration).trim());
    if (match === null) throw refuse('clock +<duration>, such as +500ms, +5s, +2m, +1h or +3d');
    const milliseconds = Math.round(Number(match[1]) * SCALE[match[2] ?? 'ms']);
    this.#audited({ type: 'mutation', operation: 'clock', milliseconds });
    await attached.system.advanceBy(milliseconds);
    const waiting = attached.system.mailbox(attached.actor).length;
    return `t=${attached.system.time}ms${waiting === 0 ? '' : `; ${plural(waiting, 'message')} waiting for the copy`}`;
  }

  // Switches a live or watch session to a fork of the same actor.
  #forkNow() {
    const attached = this.#requireAttached();
    if (attached.fork !== undefined) throw refuse('this is a fork already; restart makes a new copy of the live actor');
    if (MODES.indexOf(this.#mode) < MODES.indexOf('fork')) throw refuse(`this session allows ${this.#mode} mode at most`);
    const args = { ...attached.origin.args, mode: 'fork' };
    this.#detach('fork');
    const next = this.#attach(args);
    return { text: `now debugging ${next.label}, a copy of ${next.origin.label}`,
             then: () => this.#announce(next, 'forked') };
  }

  // A held live actor takes its next message now, and stops after it.
  #stepNow(reason) {
    const { system, actor, label } = this.#attached;
    this.#stop = null;
    this.#clearHoldTimer();
    this.#resetViews();
    this.#pending = reason;
    if (system.step(actor) === 'waiting') {
      this.#output('console', `${label} waits for its next message, and stops after it`);
    }
  }

  // next, stepIn and stepOut. A live actor stops only between macrosteps,
  // so a step there is one macrostep, the next message's, whatever the
  // granularity. A fork's copy steps as the proposal's stepping table says.
  #stepRequest(args, kind) {
    const attached = this.#requireChange('hold', 'stepping');
    const thread = this.#threadArgument(args);
    if (thread.id !== ATTACHED) throw refuse('only the attached actor steps; its invoked children run');
    if (this.#stop === null) throw refuse(`${attached.label} is not stopped: pause it first`);
    this.#audited({ type: 'mutation', operation: 'step', kind, granularity: args.granularity });
    if (attached.fork !== undefined) {
      const start = attached.system.inspect(attached.actor);
      const done = this.#stepGoal(kind, args.granularity ?? 'statement', start);
      return { after: () => this.#runCopy('step', done, undefined) };
    }
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
        return text(await this.#clock(parsed));
      case 'fork': {
        const forked = this.#forkNow();
        return text(forked.text, forked.then);
      }
      default: {
        this.#requireEvaluate('an expression');
        return { body: this.#variables.present(expressions.evaluate(parsed.text, this.#scope(thread))) };
      }
    }
  }

  // Why a function breakpoint cannot be verified, or undefined.
  #checkBreakpoint(breakpoint, definition) {
    if ((breakpoint.kind === 'entry' || breakpoint.kind === 'exit') && definition !== undefined &&
        this.#attached.labels.stateID(definition, breakpoint.target) === undefined) {
      return `${breakpoint.target} is not a state of the chart`;
    }
    return this.#checkCondition(breakpoint);
  }

  #checkCondition(breakpoint) {
    const condition = breakpoint.condition;
    const allowed = this.#attached === null ? this.#allow.has('evaluate') : this.#attached.allow.has('evaluate');
    if (condition !== undefined && condition !== null && String(condition).trim() !== '' && !allowed) {
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
        return { after: () => this.#announce(attached, 'attached to') };
      },

      restart(args) {
        const attached = this.#requireAttached();
        if (attached.fork !== undefined && args.arguments === undefined) {
          // A fork: a new copy of the live actor.
          this.#audited({ type: 'mutation', operation: 'restart' });
          for (const point of this.#points) point.image.destroy();
          this.#points = [];
          this.#stop = null;
          this.#run = null;
          this.#follow(attached.fork.renew());
          return { after: () => {
            this.#output('console', `fork: a new copy of ${attached.origin.label}`);
            this.#halt('entry', { description: 'a new copy stands where the live actor stands' });
          } };
        }
        const config = args.arguments ?? { ...attached.origin.args, mode: attached.mode };
        this.#detach('restart');
        const next = this.#attach(config);
        return { after: () => this.#announce(next, 'reattached to') };
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
        const threads = [...this.#threads.values()].map((thread) => ({ id: thread.id, name: this.#threadName(thread) }));
        return { body: { threads } };
      },

      stackTrace(args) {
        const thread = this.#threadArgument(args);
        const attached = this.#attached;
        const view = this.#inspect(thread);
        let note;
        if (thread.id === ATTACHED && view !== undefined && view.scheduling === 'held') {
          note = `held; ${plural(attached.system.mailbox(thread.actor).length, 'message')} waiting`;
        }
        const list = frames({ view, labels: attached.labels, sources: attached.sources,
                              seen: this.#lastMicrostep.get(thread.session), note });
        const start = Number(args.startFrame ?? 0);
        const levels = Number(args.levels ?? 0);
        const shown = list.slice(start, levels > 0 ? start + levels : undefined).map((frame) => {
          this.#nextFrame += 1;
          this.#frames.set(this.#nextFrame, { thread: thread.id, macrostep: frame.macrostep });
          const located = frame.place === undefined ? { line: 0, column: 0 }
            : { source: Sources.source(frame.place), line: frame.place.line, column: frame.place.column };
          return { id: this.#nextFrame, name: frame.name, ...located, presentationHint: frame.hint };
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
        this.#requireChange('set', 'setVariable');
        const text = String(args.value);
        const where = this.#variables.location(args.variablesReference, String(args.name));
        if (where === undefined) {
          throw refuse('only the Data model can be written; the other scopes are views of core state');
        }
        let value;
        try {
          value = JSON.parse(text);
        } catch {
          this.#requireEvaluate('a value that is not JSON');
          value = expressions.evaluate(text, this.#scope(where.thread));
        }
        this.#audited({ type: 'mutation', operation: 'set', location: expressions.locationText(where.path),
                        value: jsonText(value) });
        return { body: this.#variables.set(args.variablesReference, String(args.name), value) };
      },

      setExpression(args) {
        this.#requireChange('set', 'setExpression');
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
        const attached = this.#requireAttached();
        const thread = this.#frameThread(args.frameId);
        const columnsFrom = this.#client.columnsStartAt1 === false ? 0 : 1;
        const column = Number(args.column) - columnsFrom;
        const view = this.#inspect(thread);
        const targets = commands.completions(String(args.text ?? ''), column, {
          events: () => (view === undefined ? [] : attached.labels.events(view.definitionID)),
          states: () => (view === undefined ? [] : attached.labels.states(view.definitionID).map((state) => state.name)),
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
        const attached = this.#requireChange('hold', 'pause');
        this.#threadArgument(args);
        if (this.#stop !== null) return {};
        this.#audited({ type: 'mutation', operation: 'hold' });
        const { system, actor } = attached;
        if (attached.fork !== undefined) {
          system.setBudgets(actor, SINGLE_STEP);
          if (system.hold(actor, { anywhere: true })) {
            this.#run = null;
            return { after: () => this.#halt('pause', { description: 'paused' }) };
          }
          this.#run = { reason: 'pause', done: () => true, description: 'paused', units: 0 };
          return {};
        }
        if (system.hold(actor)) {
          return { after: () => this.#halt('pause', { description: 'paused between macrosteps' }) };
        }
        this.#pending = 'pause';
        return { after: () => this.#output('console', `${attached.label} is inside a macrostep; it stops when the ` +
                                                      'macrostep ends (a macrostep that never ends needs `abandon`)') };
      },

      continue() {
        this.#requireChange('hold', 'continue');
        if (this.#stop === null && this.#pending === null && this.#run === null) {
          return { body: { allThreadsContinued: false } };
        }
        this.#audited({ type: 'mutation', operation: 'release' });
        this.#release();
        return { body: { allThreadsContinued: false } };
      },

      next(args) {
        return this.#stepRequest(args, 'over');
      },

      stepIn(args) {
        return this.#stepRequest(args, 'in');
      },

      stepOut(args) {
        return this.#stepRequest(args, 'out');
      },

      stepBack() {
        this.#requireFork('stepBack');
        if (this.#stop === null) throw refuse('stop the copy first');
        if (this.#points.length < 2) throw refuse('no earlier stop to step back to');
        this.#audited({ type: 'mutation', operation: 'stepBack' });
        this.#points.pop().image.destroy();
        return { after: () => this.#restore(this.#points.length - 1, 'step', 'stepped back to the previous stop') };
      },

      reverseContinue() {
        this.#requireFork('reverseContinue');
        if (this.#stop === null) throw refuse('stop the copy first');
        if (this.#points.length < 2) throw refuse('no earlier stop to go back to');
        this.#audited({ type: 'mutation', operation: 'reverseContinue' });
        this.#points.pop().image.destroy();
        let index = this.#points.length - 1;
        while (index > 0 && !this.#points[index].breakpoint) index -= 1;
        const found = this.#points[index].breakpoint;
        return { after: () => this.#restore(index, found ? 'breakpoint' : 'step',
                                             found ? 'back at the previous breakpoint' : 'back at the first stop kept') };
      },

      restartFrame(args) {
        this.#requireFork('restartFrame');
        const frame = this.#frames.get(Number(args.frameId));
        if (frame === undefined) throw refuse(`frame ${args.frameId} is not valid (any more)`);
        if (frame.macrostep === undefined || frame.macrostep === 0) throw refuse('only a frame of a macrostep restarts');
        const index = this.#points.findIndex((point) => point.macrostep === frame.macrostep);
        if (index < 0) throw refuse(`no stop was kept from the start of macrostep ${frame.macrostep}`);
        this.#audited({ type: 'mutation', operation: 'restartFrame', macrostep: frame.macrostep });
        const description = `back at the first stop kept in macrostep ${frame.macrostep}`;
        return { after: () => this.#restore(index, 'restart', description) };
      },

      gotoTargets(args) {
        const attached = this.#requireAttached();
        const definition = attached.system.inspect(attached.actor).definitionID;
        let states = attached.labels.states(definition);
        const path = args.source?.path;
        if (path !== undefined && args.line !== undefined) {
          const located = attached.sources.lines(definition, path, Number(args.line))
            .filter((element) => element.kind === 'state').map((element) => element.id);
          if (located.length > 0) states = states.filter((state) => located.includes(state.id));
        }
        const targets = states.map((state) => {
          const id = this.#gotoTargets.size + 1;
          this.#gotoTargets.set(id, { definition: String(definition), state: state.name });
          const place = attached.sources.place(definition, 'state', state.id);
          return { id, label: state.name, line: place?.line ?? 0, ...(place === undefined ? {} : { column: place.column }) };
        });
        return { body: { targets } };
      },

      goto(args) {
        const attached = this.#requireChange('goto', 'goto');
        const target = this.#gotoTargets.get(Number(args.targetId));
        if (target === undefined) throw refuse(`no goto target ${args.targetId}; ask gotoTargets first`);
        if (target.definition !== String(attached.system.inspect(attached.actor).definitionID)) {
          throw refuse(`${attached.label} runs another revision than the target's`);
        }
        const { text, then } = this.#jump([target.state]);
        return { after: () => { this.#output('console', text); then?.(); } };
      },

      setBreakpoints(args) {
        const path = args.source?.path ?? args.source?.name;
        if (path === undefined) throw refuse('setBreakpoints needs a source with a path');
        const specs = Array.isArray(args.breakpoints) ? args.breakpoints : (args.lines ?? []).map((line) => ({ line }));
        this.#audited({ type: 'breakpoints', kind: 'line', path,
                        breakpoints: specs.map(({ line, condition, hitCondition, logMessage }) =>
                          ({ line, condition, hitCondition, logMessage })) });
        const answered = this.#breakpoints.setLines(path, specs, (line) => {
          const problem = this.#checkCondition(specs.find((spec) => Number(spec.line) === line) ?? {});
          return problem ?? this.#resolveLine(path, line);
        });
        this.#updateGate();
        return { body: { breakpoints: answered } };
      },

      breakpointLocations(args) {
        const attached = this.#requireAttached();
        const path = args.source?.path;
        if (path === undefined) return { body: { breakpoints: [] } };
        const definition = attached.system.inspect(attached.actor).definitionID;
        const found = attached.sources.lines(definition, path, Number(args.line), Number(args.endLine ?? args.line));
        const unique = new Map(found.map((element) => [`${element.line}:${element.column}`, element]));
        const breakpoints = [...unique.values()].map((element) => ({ line: element.line, column: element.column }));
        return { body: { breakpoints } };
      },

      setFunctionBreakpoints(args) {
        const specs = Array.isArray(args.breakpoints) ? args.breakpoints : [];
        const attached = this.#attached;
        const definition = attached === null ? undefined : attached.system.inspect(attached.actor).definitionID;
        this.#audited({ type: 'breakpoints', kind: 'function',
                        breakpoints: specs.map(({ name, condition, hitCondition }) =>
                          ({ name, condition, hitCondition })) });
        const answered = this.#breakpoints.setFunctions(specs, (breakpoint) =>
          (attached === null ? this.#checkCondition(breakpoint) : this.#checkBreakpoint(breakpoint, definition)));
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
                        breakpoints: specs.map(({ dataId, condition, hitCondition }) =>
                          ({ dataId, condition, hitCondition })) });
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
        return { body: { sources: [...[...this.#sourceRefs.values()].map((entry) => entry.source),
                                   ...[...this.#files].map((path) => Sources.source({ path }))] } };
      },

      source(args) {
        const reference = Number(args.sourceReference ?? args.source?.sourceReference ?? 0);
        const entry = this.#sourceRefs.get(reference);
        if (entry === undefined) {
          throw refuse(args.source?.path !== undefined && this.#files.has(args.source.path)
            ? `${args.source.path} is a file the client reads itself` : 'no such source');
        }
        const system = this.#attached?.system ?? this.#system;
        const spec = core.definition_export(nativeOf(system), entry.definition);
        return { body: { content: exportText(spec), mimeType: 'application/json' } };
      },
    };
  }
}

/** A debug session over `system` (see the top of this module). */
function createDebugSession(system, options) {
  return new DebugSession(system, options);
}

// Whether two positions (core ADR 0083) name the same unit, in the same
// iteration of its <foreach>.
function samePosition(one, other) {
  return one.at === other.at && one.state === other.state && one.transition === other.transition &&
    one.action === other.action && one.iteration === other.iteration;
}

// { family, name } from an address in one of its spellings, or undefined.
function parseAddress(address) {
  if (address !== null && typeof address === 'object') {
    const { family, name } = address;
    return typeof family === 'string' && typeof name === 'string' && family !== '' && name !== ''
      ? { family, name } : undefined;
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

// A definition's export as a loaded source the client asks for by reference.
function exportSource(definition, revision, reference) {
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

module.exports = { DebugSession, createDebugSession, DAPReader, encodeDAPMessage, CAPABILITIES };
