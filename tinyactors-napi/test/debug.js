'use strict';

// The debug adapter (lib/debug.js; napi ADR 0027), driven by a scripted
// DAP client: every request it implements and every one it refuses, the
// modes and permissions, the audit hook, holding a live actor with its
// timeout, the webhook case, breakpoints, stepping, the console, scopes,
// modules and sources, and the base protocol's framing. Runs on Node and
// Bun.

const assert = require('node:assert/strict');
const {
  createSystem, statechart, createDebugSession, DAPReader, encodeDAPMessage, TinyactorsError,
} = require('..');

const watchdog = setTimeout(() => {
  console.error('debug.js: timed out');
  process.exit(1);
}, 60000);

const turn = () => new Promise((resolve) => setImmediate(resolve));
const sleep = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));
const ALL = ['send', 'set', 'goto', 'hold', 'evaluate'];

// A scripted client: requests in order, and every message the session sent.
class Client {
  constructor(system, options) {
    this.messages = [];
    this.audit = [];
    this.seq = 0;
    this.session = createDebugSession(system, {
      audit: (entry) => this.audit.push(entry),
      ...options,
      send: (message) => this.messages.push(message),
    });
  }

  async request(command, args = {}) {
    this.seq += 1;
    const seq = this.seq;
    await this.session.receive({ seq, type: 'request', command, arguments: args });
    const response = this.messages.find((message) => message.type === 'response' && message.request_seq === seq);
    assert.ok(response !== undefined, `${command} was answered`);
    assert.equal(response.command, command);
    return response;
  }

  async ok(command, args) {
    const response = await this.request(command, args);
    assert.ok(response.success, `${command} failed: ${response.message}`);
    return response.body;
  }

  async refused(command, args, pattern) {
    const response = await this.request(command, args);
    assert.equal(response.success, false, `${command} should be refused`);
    assert.equal(response.body.error.format, response.message);
    if (pattern !== undefined) assert.match(response.message, pattern);
    return response.message;
  }

  events(name) {
    return this.messages.filter((message) => message.type === 'event' && message.event === name).map((message) => message.body);
  }

  output() {
    return this.events('output').map((body) => body.output).join('');
  }

  mark() {
    return this.messages.length;
  }

  since(mark, name) {
    return this.messages.slice(mark).filter((message) => message.type === 'event' && message.event === name)
      .map((message) => message.body);
  }

  async until(predicate, what) {
    for (let tries = 0; tries < 500; tries += 1) {
      if (predicate()) return;
      await turn();
    }
    assert.fail(`timed out waiting for ${what}; the session wrote:\n${this.output()}`);
  }

  async stoppedAfter(mark, reason) {
    await this.until(() => this.since(mark, 'stopped').length > 0, `a stop (${reason})`);
    const [stop] = this.since(mark, 'stopped');
    assert.equal(stop.reason, reason, JSON.stringify(stop));
    return stop;
  }

  async repl(text) {
    return this.ok('evaluate', { expression: text, context: 'repl' });
  }

  async start(attach) {
    const capabilities = await this.ok('initialize', { clientID: 'test', adapterID: 'tinyactors', supportsInvalidatedEvent: true });
    await this.ok('attach', attach);
    await this.ok('configurationDone');
    return capabilities;
  }
}

// A subscription: active --payment.failed--> past_due (counting failures,
// arming a retry and answering the sender), past_due --paid--> active;
// `boom` fails a script; `spin` never ends; `remind` arms a delayed send.
function subscription(revision = 'v1') {
  return statechart({ family: 'subscription', revision })
    .data('failures', 0)
    .data('plan', { name: 'basic', seats: 2 })
    .data('log', [])
    .state('active', (s) =>
      s
        .on('payment.failed', (t) =>
          t.target('past_due').assign('failures', function () { return this.failures + 1; }))
        .on('boom', (t) => t.script(function () { throw new Error('boom'); }))
        .on('spin', (t) => t.target('loop1'))
        .on('remind', (t) => t.send('reminder', (send) => send.id('nudge').after('60s')))
        .on('ping', (t) => t.send('pong', (send) => send.via(({ event }) => event.originType).to(({ event }) => event.origin))))
    .state('past_due', (s) =>
      s
        .entry((e) => e.log('overdue', function () { return this.failures; }))
        .on('paid', (t) => t.target('active'))
        .on('payment.failed', (t) => t.assign('failures', function () { return this.failures + 1; })))
    .state('loop1', (s) =>
      s.entry((e) => e.script(function () { this.log.push('spin'); }).script(function () {}))
        .always((t) => t.target('loop2')))
    .state('loop2', (s) => s.always((t) => t.target('loop1')));
}

async function world(options = {}) {
  const system = createSystem({ clock: 'manual', ...options });
  const definition = system.define(subscription());
  const actor = system.spawn(definition, { name: 'acct_1' });
  await system.flush();
  return { system, definition, actor };
}

// ----------------------------------------------------------------------

async function framing() {
  const messages = [];
  const reader = new DAPReader((message) => messages.push(message));
  const bytes = encodeDAPMessage({ seq: 1, type: 'request', command: 'initialize', arguments: { text: 'grüße' } });
  const header = new TextDecoder().decode(bytes.subarray(0, 20));
  assert.match(header, /^Content-Length: \d+\r\n/);
  const twice = new Uint8Array(bytes.length * 2);
  twice.set(bytes, 0);
  twice.set(bytes, bytes.length);
  reader.push(twice.subarray(0, 7));
  reader.push(twice.subarray(7, bytes.length + 3));
  assert.equal(messages.length, 1);
  reader.push(twice.subarray(bytes.length + 3));
  assert.equal(messages.length, 2);
  assert.equal(messages[1].arguments.text, 'grüße');
  assert.throws(() => new DAPReader((m) => m).push(new TextEncoder().encode('X: 1\r\n\r\n{}')), TinyactorsError);
  assert.throws(() => new DAPReader(), TinyactorsError);
}

async function options() {
  const system = createSystem({ clock: 'manual' });
  const send = () => {};
  assert.throws(() => createDebugSession(system, { mode: 'fly', send }), /mode/);
  assert.throws(() => createDebugSession(system, { mode: 'live', allow: ['everything'], send }), /allow/);
  assert.throws(() => createDebugSession(system, { mode: 'live' }), /send/);
  assert.throws(() => createDebugSession(system, { mode: 'live', send, audit: 1 }), /audit/);
  assert.throws(() => createDebugSession(system, { mode: 'live', send, holdTimeout: 0 }), /holdTimeout/);
  assert.throws(() => createDebugSession({}, { mode: 'live', send }), /System/);
  const session = createDebugSession(system, { mode: 'watch', allow: ['evaluate'], send });
  assert.equal(session.mode, 'watch');
  assert.deepEqual(session.allow, ['evaluate']);
  assert.equal(session.attached, false);
  system.close();
}

// initialize, the capabilities, attach and its refusals, and the requests
// every client sends first.
async function session() {
  const { system, actor } = await world();
  const client = new Client(system, { mode: 'live', allow: ALL });
  const capabilities = await client.ok('initialize', { clientID: 'test' });
  assert.deepEqual(client.events('initialized'), [undefined]);
  assert.equal(capabilities.supportsConfigurationDoneRequest, true);
  assert.equal(capabilities.supportsFunctionBreakpoints, true);
  assert.equal(capabilities.supportsDataBreakpoints, true);
  assert.equal(capabilities.supportTerminateDebuggee, false);
  assert.equal(capabilities.supportsStepBack, undefined, 'not declared: stepping back needs a fork');
  assert.equal(capabilities.supportsLogPoints, undefined, 'not declared: logpoints are line breakpoints');
  assert.deepEqual(capabilities.exceptionBreakpointFilters.map((filter) => filter.filter),
                   ['error.execution', 'error.communication', 'error.platform', 'ignored', 'stopped']);

  await client.refused('launch', {}, /attach/);
  await client.refused('threads', {}, undefined).catch(() => {});
  await client.refused('stackTrace', { threadId: 1 }, /attach/);
  await client.refused('attach', {}, /address/);
  await client.refused('attach', { address: 'nope' }, /address/);
  await client.refused('attach', { address: 'subscription/nobody' }, /no resident actor/);
  await client.refused('attach', { address: 'subscription/acct_1', mode: 'fork' }, /ADR 0085/);
  await client.refused('attach', { address: 'subscription/acct_1', mode: 'sideways' }, /mode/);
  await client.refused('attach', { actor: { slot: 999, generation: 1 } }, /no resident actor/);
  await client.refused('attach', { sessionID: 'x' }, /session ID/);

  await client.ok('attach', { address: '#_actor_subscription/acct_1' });
  await client.refused('attach', { address: 'subscription/acct_1' }, /already attached/);
  await client.ok('configurationDone');
  const [process] = client.events('process');
  assert.deepEqual(process, { name: 'subscription/acct_1', startMethod: 'attach', isLocalProcess: true });
  assert.match(client.output(), /attached to subscription\/acct_1 .* in live mode; allowed: send, set, goto, hold, evaluate/);
  const { threads } = await client.ok('threads');
  assert.deepEqual(threads, [{ id: 1, name: 'subscription/acct_1 · active' }]);
  assert.equal(client.audit[0].type, 'attach');
  assert.deepEqual(client.audit[0].allow, ALL);
  assert.equal(client.audit[0].actor, 'subscription/acct_1');

  // Requests a chart has no use for, and ones that need what is pending.
  for (const [command, pattern] of [
    ['stepBack', /ADR 0085/], ['reverseContinue', /ADR 0085/], ['restartFrame', /ADR 0085/],
    ['breakpointLocations', /ADR 0086/], ['cancel', /nothing to cancel/], ['stepInTargets', /one way in/],
    ['terminateThreads', /destroy an actor/], ['setInstructionBreakpoints', /no instructions/],
    ['locations', /source/], ['readMemory', /memory/], ['writeMemory', /memory/], ['disassemble', /instructions/],
    ['runInTerminal', /not supported/],
  ]) {
    await client.refused(command, {}, pattern);
  }
  const lines = await client.ok('setBreakpoints', { source: { path: 'chart.js' }, breakpoints: [{ line: 3 }] });
  assert.deepEqual(lines.breakpoints, [{ verified: false,
    message: 'line breakpoints need source spans (core ADR 0086); use function breakpoints' }]);

  // Attach by actor ID and by session ID, and restart.
  const other = new Client(system, { mode: 'watch' });
  await other.start({ actor: { slot: actor.slot, generation: actor.generation } });
  assert.match(other.output(), /attached to actor \d+:\d+/);
  await other.ok('disconnect');
  const third = new Client(system, { mode: 'watch' });
  await third.start({ sessionID: String(actor.inspect().sessionID), mode: 'watch' });
  await third.ok('restart', {});
  assert.match(third.output(), /reattached/);
  assert.deepEqual(third.audit.map((entry) => entry.type), ['attach', 'detach', 'attach']);
  await third.ok('terminate');
  assert.equal(third.events('terminated').length, 1);
  assert.equal(third.session.attached, false);

  await client.ok('disconnect', { terminateDebuggee: true });
  assert.equal(system.exists(actor), true, 'the actor is never destroyed');
  assert.equal(client.audit.at(-1).type, 'detach');
  client.session.close();
  system.close();
}

// Watch mode: nothing stops and nothing changes; breakpoints say where
// they would have stopped; every macrostep is summarized from the traces,
// and the application's own sink still gets every turn.
async function watchMode() {
  const { system, actor } = await world();
  const applicationTurns = [];
  system.setTraceSink((traces) => applicationTurns.push(traces.length), { detail: 'summary', values: true });
  const client = new Client(system, { mode: 'watch', allow: ['evaluate'] });
  await client.start({ address: { family: 'subscription', name: 'acct_1' } });
  await client.ok('setFunctionBreakpoints', { breakpoints: [{ name: 'past_due' }, { name: 'on:payment.failed' }] });
  await client.ok('setExceptionBreakpoints', { filters: ['error.execution'] });
  await client.refused('pause', { threadId: 1 }, /watch mode/);
  await client.refused('continue', { threadId: 1 }, /watch mode/);
  await client.refused('next', { threadId: 1 }, /watch mode/);
  await client.refused('evaluate', { expression: 'send paid', context: 'repl' }, /watch mode/);
  await client.refused('evaluate', { expression: 'goto active', context: 'repl' }, /watch mode/);
  await client.refused('evaluate', { expression: 'abandon', context: 'repl' }, /watch mode/);
  await client.refused('evaluate', { expression: 'cancel nudge', context: 'repl' }, /watch mode/);

  await actor.send('boom');
  await actor.send('payment.failed', { attempt: 3 });
  await system.flush();
  assert.equal(client.events('stopped').length, 0, 'watch mode never stops');
  const text = client.output();
  assert.match(text, /subscription\/acct_1 macrostep \d+: payment\.failed \{"attempt":3\}/);
  assert.match(text, /active → past_due on payment\.failed/);
  assert.match(text, /assign failures = 1/);
  assert.match(text, /now in past_due/);
  assert.match(text, /would have stopped after macrostep \d+: error\.execution/);
  assert.match(text, /would have stopped after macrostep \d+: entered past_due, took payment\.failed/);
  assert.match(text, /overdue: 1/, '<log> records are stdout');
  assert.ok(client.events('invalidated').length >= 2);
  assert.ok(applicationTurns.length > 0, 'the application\'s sink still gets its turns');
  // Expressions still evaluate with the permission.
  assert.equal((await client.repl('failures * 10')).result, '10');
  await client.ok('disconnect');
  system.close();
}

// Holding a live actor: pause between macrosteps, the webhook case, step
// one message at a time, continue, and the hold's timeout.
async function holding() {
  const { system, actor } = await world();
  const client = new Client(system, { mode: 'live', allow: ALL, holdTimeout: 150 });
  await client.start({ address: 'subscription/acct_1' });
  let mark = client.mark();
  await client.ok('pause', { threadId: 1 });
  await client.stoppedAfter(mark, 'pause');
  assert.equal(actor.inspect().scheduling, 'held');
  assert.ok(client.audit.some((entry) => entry.type === 'mutation' && entry.operation === 'hold'));
  await client.ok('pause', { threadId: 1 }); // already stopped

  // The webhook case: a handler waiting with a timeout has its message
  // withdrawn, never taken later, so Stripe's retry is not a duplicate.
  await assert.rejects(actor.send('payment.failed', { attempt: 1 }, { timeout: 20 }), (error) => error.code === 'timeout');
  assert.deepEqual(system.mailbox(actor), []);
  actor.post('payment.failed', { attempt: 2 });
  actor.post('payment.failed', { attempt: 3 });
  await system.flush();
  assert.equal(actor.inspect().data.failures, 0, 'held mail waits');
  const { stackFrames } = await client.ok('stackTrace', { threadId: 1 });
  assert.equal(stackFrames[0].name, 'idle in active (held; 2 messages waiting)');

  // next takes exactly one message and stops after its macrostep.
  mark = client.mark();
  await client.ok('next', { threadId: 1, granularity: 'statement' });
  const step = await client.stoppedAfter(mark, 'step');
  assert.equal(step.threadId, 1);
  assert.equal(actor.inspect().data.failures, 1);
  mark = client.mark();
  await client.ok('stepIn', { threadId: 1 });
  await client.stoppedAfter(mark, 'step');
  assert.equal(actor.inspect().data.failures, 2);
  // An empty mailbox: the step waits for the next message.
  mark = client.mark();
  await client.ok('stepOut', { threadId: 1 });
  await system.flush();
  assert.match(client.output(), /waits for its next message/);
  assert.equal(client.since(mark, 'stopped').length, 0);
  actor.post('paid');
  await client.stoppedAfter(mark, 'step');
  assert.deepEqual(actor.inspect().activeStates, ['active']);

  // continue lets it run; the retry is taken once.
  const retry = actor.send('payment.failed', { attempt: 1 });
  await client.ok('continue', { threadId: 1 });
  assert.equal((await retry).status, 'stable');
  assert.equal(actor.inspect().data.failures, 3);
  assert.equal(actor.inspect().scheduling, 'idle');
  await client.refused('next', { threadId: 1 }, /not stopped/);

  // The hold's timeout lets the actor go by itself.
  mark = client.mark();
  await client.ok('pause', { threadId: 1 });
  await client.stoppedAfter(mark, 'pause');
  actor.post('paid');
  await sleep(250);
  await system.flush();
  assert.equal(client.since(mark, 'continued').length, 1);
  assert.match(client.output(), /was held for 150 ms, the most this session may hold it/);
  assert.deepEqual(actor.inspect().activeStates, ['active'], 'the mail was taken once the hold ended');
  assert.ok(client.audit.some((entry) => entry.operation === 'release' && entry.reason === 'hold timeout'));

  // A pause while a macrostep runs stops when it ends.
  const slow = createSystem({ clock: 'manual', scheduling: { resumeBudget: 1 } });
  const slowActor = slow.spawn(slow.define(subscription()), { name: 'acct_2' });
  await slow.flush();
  const slowClient = new Client(slow, { mode: 'live', allow: ['hold'] });
  await slowClient.start({ address: 'subscription/acct_2' });
  const completed = slowActor.send('payment.failed');
  await turn();
  assert.equal(slowActor.inspect().scheduling, 'runnable');
  mark = slowClient.mark();
  await slowClient.ok('pause', { threadId: 1 });
  assert.match(slowClient.output(), /is inside a macrostep; it stops when the macrostep ends/);
  assert.equal((await completed).status, 'stable', 'the send() of the running macrostep completes');
  await slowClient.stoppedAfter(mark, 'pause');
  // Disconnecting releases the hold.
  slowActor.post('paid');
  await slowClient.ok('disconnect');
  await slow.flush();
  assert.deepEqual(slowActor.inspect().activeStates, ['active']);
  slow.close();

  await client.ok('disconnect');
  system.close();
}

// Breakpoints on a live actor stop it between macrosteps.
async function breakpoints() {
  const { system, actor } = await world();
  const client = new Client(system, { mode: 'live', allow: ALL });
  await client.start({ address: 'subscription/acct_1' });

  // Entry, with a condition and a hit condition.
  const set = await client.ok('setFunctionBreakpoints', { breakpoints: [
    { name: 'past_due', condition: 'failures >= 1', hitCondition: '1' },
    { name: 'nowhere' },
  ] });
  assert.equal(set.breakpoints[0].verified, true);
  assert.deepEqual(set.breakpoints[1], { id: set.breakpoints[1].id, verified: false, message: 'nowhere is not a state of the chart' });
  let mark = client.mark();
  const first = actor.send('payment.failed', { attempt: 1 });
  let stop = await client.stoppedAfter(mark, 'function breakpoint');
  assert.equal((await first).status, 'stable', 'the macrostep ended before the stop');
  assert.deepEqual(stop.hitBreakpointIds, [set.breakpoints[0].id]);
  assert.match(stop.description, /entered past_due/);
  await client.ok('continue', { threadId: 1 });

  // on: stops before the macrostep that takes the event, and lets it
  // through on continue.
  const on = await client.ok('setFunctionBreakpoints', { breakpoints: [{ name: 'on:paid' }] });
  actor.post('payment.failed');
  mark = client.mark();
  actor.post('paid');
  stop = await client.stoppedAfter(mark, 'function breakpoint');
  assert.match(stop.description, /about to take paid/);
  assert.deepEqual(stop.hitBreakpointIds, [on.breakpoints[0].id]);
  assert.deepEqual(actor.inspect().activeStates, ['past_due']);
  assert.equal(actor.inspect().data.failures, 2, 'the mail before it was taken');
  mark = client.mark();
  await client.ok('continue', { threadId: 1 });
  await system.flush();
  assert.deepEqual(actor.inspect().activeStates, ['active']);
  assert.equal(client.since(mark, 'stopped').length, 0, 'the same message does not stop twice');

  // A transition by its name, and exit.
  await client.ok('setFunctionBreakpoints', { breakpoints: [{ name: 'active -> past_due' }] });
  mark = client.mark();
  actor.post('payment.failed');
  stop = await client.stoppedAfter(mark, 'function breakpoint');
  assert.match(stop.description, /active → past_due/);
  await client.ok('setFunctionBreakpoints', { breakpoints: [{ name: 'exit:past_due' }] });
  mark = client.mark();
  await client.ok('continue', { threadId: 1 });
  actor.post('paid');
  stop = await client.stoppedAfter(mark, 'function breakpoint');
  assert.match(stop.description, /exited past_due/);
  await client.ok('setFunctionBreakpoints', { breakpoints: [] });
  await client.ok('continue', { threadId: 1 });

  // An exception, and exceptionInfo.
  await client.refused('setExceptionBreakpoints', { filters: ['bogus'] }, /unknown exception filter/);
  await client.ok('setExceptionBreakpoints', { filters: ['error.execution'] });
  mark = client.mark();
  actor.post('boom');
  stop = await client.stoppedAfter(mark, 'exception');
  assert.match(stop.text, /error\.execution/);
  const info = await client.ok('exceptionInfo', { threadId: 1 });
  assert.equal(info.exceptionId, 'error.execution');
  assert.equal(info.details.typeName, 'error.execution');
  assert.equal(info.breakMode, 'always');
  await client.ok('setExceptionBreakpoints', { filters: [] });
  await client.ok('continue', { threadId: 1 });
  await client.refused('exceptionInfo', { threadId: 1 }, /did not stop for an exception/);

  // A data breakpoint, from the Variables pane.
  mark = client.mark();
  await client.ok('pause', { threadId: 1 });
  await client.stoppedAfter(mark, 'pause');
  const { stackFrames } = await client.ok('stackTrace', { threadId: 1 });
  const { scopes } = await client.ok('scopes', { frameId: stackFrames[0].id });
  const data = scopes.find((scope) => scope.name === 'Data model');
  const described = await client.ok('dataBreakpointInfo', { variablesReference: data.variablesReference, name: 'failures' });
  assert.equal(described.dataId, 'subscription/acct_1 ["failures"]');
  assert.deepEqual(described.accessTypes, ['write']);
  const byName = await client.ok('dataBreakpointInfo', { name: '.plan.seats' });
  assert.equal(byName.dataId, 'subscription/acct_1 ["plan","seats"]');
  assert.equal((await client.ok('dataBreakpointInfo', { name: 'a + b' })).dataId, null);
  const watched = await client.ok('setDataBreakpoints', { breakpoints: [
    { dataId: described.dataId }, { dataId: 'nobody ["x"]' }, { dataId: described.dataId, accessType: 'read' }, { dataId: 'x' },
  ] });
  assert.deepEqual(watched.breakpoints.map((entry) => entry.verified), [true, false, false, false]);
  mark = client.mark();
  await client.ok('continue', { threadId: 1 });
  actor.post('payment.failed');
  stop = await client.stoppedAfter(mark, 'data breakpoint');
  assert.match(stop.description, /failures/);
  await client.ok('setDataBreakpoints', { breakpoints: [] });
  await client.ok('continue', { threadId: 1 });
  await system.flush();
  assert.equal(actor.inspect().scheduling, 'idle', 'no breakpoints: nothing holds it');
  await client.ok('disconnect');
  system.close();
}

// The console: send and replies, goto, abandon, cancel, replace, and what
// needs a fork.
async function console() {
  const { system, actor } = await world();
  const v2 = system.define(subscription('v2'));
  const client = new Client(system, { mode: 'live', allow: ALL });
  await client.start({ address: 'subscription/acct_1' });

  assert.match((await client.repl('help')).result, /send <event> \[json\]/);
  assert.match((await client.repl('send ping')).result, /sent ping to subscription\/acct_1/);
  await system.flush();
  assert.match(client.output(), /subscription\/acct_1 replied pong/);
  await client.refused('evaluate', { expression: 'send', context: 'repl' }, /send <event>/);
  await client.refused('evaluate', { expression: 'send x {nope', context: 'repl' }, /not JSON/);

  // A message sent while stopped waits in the mailbox, marked as the console's.
  let mark = client.mark();
  await client.ok('pause', { threadId: 1 });
  await client.stoppedAfter(mark, 'pause');
  assert.match((await client.repl('send payment.failed {"attempt": 9}')).result, /waits in the mailbox \(1 message\)/);
  const { stackFrames } = await client.ok('stackTrace', { threadId: 1 });
  const { scopes } = await client.ok('scopes', { frameId: stackFrames[0].id });
  const queues = scopes.find((scope) => scope.name === 'Queues');
  const queued = await client.ok('variables', { variablesReference: queues.variablesReference });
  const mailbox = queued.variables.find((variable) => variable.name === 'mailbox');
  const [message] = (await client.ok('variables', { variablesReference: mailbox.variablesReference })).variables;
  const fields = await client.ok('variables', { variablesReference: message.variablesReference });
  assert.deepEqual(Object.fromEntries(fields.variables.map((variable) => [variable.name, variable.value])),
                   { event: '"payment.failed"', data: '{"attempt":9}', from: '"this console"' });
  mark = client.mark();
  await client.ok('continue', { threadId: 1 });
  await system.flush();
  assert.equal(actor.inspect().data.failures, 1);

  // goto, while running: queued behind real mail; and when held with an
  // empty mailbox: taken at once, stopping with reason goto.
  assert.match((await client.repl('goto active from past_due')).result, /goto active from past_due queued/);
  await system.flush();
  assert.deepEqual(actor.inspect().activeStates, ['active']);
  await client.refused('evaluate', { expression: 'goto nowhere', context: 'repl' }, /not a state/);
  await client.refused('evaluate', { expression: 'goto', context: 'repl' }, /goto <state/);
  mark = client.mark();
  await client.ok('pause', { threadId: 1 });
  await client.stoppedAfter(mark, 'pause');
  mark = client.mark();
  await client.repl('goto past_due');
  await client.stoppedAfter(mark, 'goto');
  assert.deepEqual(actor.inspect().activeStates, ['past_due']);
  assert.match(client.output(), /goto: exited active, entered past_due/);
  // The DAP goto request, from gotoTargets.
  const { targets } = await client.ok('gotoTargets', { source: { path: 'x' }, line: 1 });
  assert.deepEqual(targets.map((target) => target.label), ['active', 'past_due', 'loop1', 'loop2']);
  mark = client.mark();
  await client.ok('goto', { threadId: 1, targetId: targets[0].id });
  await client.stoppedAfter(mark, 'goto');
  assert.deepEqual(actor.inspect().activeStates, ['active']);
  await client.refused('goto', { threadId: 1, targetId: 999 }, /gotoTargets/);
  await client.ok('continue', { threadId: 1 });

  // cancel withdraws a delayed send by the ID the Timers scope shows.
  actor.post('remind');
  await system.flush();
  assert.equal(actor.inspect().delayedSendCount, 1);
  assert.match((await client.repl('cancel nudge')).result, /withdrew send nudge/);
  assert.equal(actor.inspect().delayedSendCount, 0);
  await client.refused('evaluate', { expression: 'cancel nudge', context: 'repl' }, /no pending send/);

  // abandon: a macrostep that never ends, and a pause that waits for it.
  const spinning = createSystem({ clock: 'manual', scheduling: { actionBudget: 1 } });
  const spinner = spinning.spawn(spinning.define(subscription()), { name: 'acct_3' });
  await spinning.flush();
  const spinClient = new Client(spinning, { mode: 'live', allow: ALL });
  await spinClient.start({ address: 'subscription/acct_3' });
  await spinClient.refused('evaluate', { expression: 'abandon', context: 'repl' }, /no macrostep to abandon/);
  const runaway = spinner.send('spin');
  for (let index = 0; index < 5; index += 1) await turn();
  mark = spinClient.mark();
  await spinClient.ok('pause', { threadId: 1 });
  const frames = await spinClient.ok('stackTrace', { threadId: 1 });
  assert.ok(frames.stackFrames.some((frame) => /^macrostep \d+ on spin$/.test(frame.name)), JSON.stringify(frames));
  assert.match((await spinClient.repl('abandon')).result, /abandoned macrostep \d+/);
  await assert.rejects(runaway, (error) => error.code === 'abandoned');
  await spinClient.stoppedAfter(mark, 'pause');
  assert.ok(spinClient.audit.some((entry) => entry.operation === 'abandon'));
  await spinClient.ok('disconnect');
  spinning.close();

  // replace follows the address to the new revision.
  assert.match((await client.repl('replace v2')).result, /now runs v2 from its initial state, with its data/);
  const replaced = system.findActor({ family: 'subscription', name: 'acct_1' });
  assert.equal(replaced.inspect().definition.revision, 'v2');
  assert.equal(replaced.inspect().data.failures, 1);
  assert.match((await client.ok('threads')).threads[0].name, /subscription\/acct_1/);
  await client.refused('evaluate', { expression: 'replace v9', context: 'repl' }, /no registered/);
  void v2;

  // What a fork would do.
  await client.refused('evaluate', { expression: 'clock +3d', context: 'repl' }, /ADR 0085/);
  await client.refused('evaluate', { expression: 'fork', context: 'repl' }, /ADR 0085/);
  await client.ok('disconnect');
  system.close();
}

// Permissions: each operation needs its own; expressions need `evaluate`,
// and without it the Variables pane and the console commands still work.
async function permissions() {
  const { system, actor } = await world();
  const client = new Client(system, { mode: 'live', allow: ['send'] });
  await client.start({ address: 'subscription/acct_1' });
  await client.refused('pause', { threadId: 1 }, /"hold" permission/);
  await client.refused('evaluate', { expression: 'goto past_due', context: 'repl' }, /"goto" permission/);
  await client.refused('evaluate', { expression: 'abandon', context: 'repl' }, /"goto" permission/);
  await client.refused('evaluate', { expression: 'failures', context: 'repl' }, /"evaluate" permission/);
  await client.refused('evaluate', { expression: 'failures', context: 'hover' }, /"evaluate" permission/);
  await client.refused('evaluate', { expression: 'failures', context: 'watch' }, /runs code in the process/);
  const { stackFrames } = await client.ok('stackTrace', { threadId: 1 });
  const { scopes } = await client.ok('scopes', { frameId: stackFrames[0].id });
  const data = scopes.find((scope) => scope.name === 'Data model');
  const { variables } = await client.ok('variables', { variablesReference: data.variablesReference });
  assert.deepEqual(variables.map((variable) => variable.name), ['failures', 'plan', 'log']);
  await client.refused('setVariable', { variablesReference: data.variablesReference, name: 'failures', value: '5' }, /"set"/);
  const conditioned = await client.ok('setFunctionBreakpoints', { breakpoints: [{ name: 'past_due', condition: 'true' }] });
  assert.match(conditioned.breakpoints[0].message, /"evaluate" permission/);
  assert.match((await client.repl('send paid')).result, /sent paid/);
  await client.ok('disconnect');

  // A session may ask for less than it was given, never more.
  const narrow = new Client(system, { mode: 'live', allow: ALL });
  await narrow.start({ address: 'subscription/acct_1', allow: ['hold'] });
  await narrow.refused('evaluate', { expression: 'send paid', context: 'repl' }, /"send" permission/);
  await narrow.ok('disconnect');
  const watcher = new Client(system, { mode: 'watch', allow: ALL });
  await watcher.ok('initialize');
  await watcher.refused('attach', { address: 'subscription/acct_1', mode: 'live' }, /watch mode at most/);
  void actor;
  system.close();
}

// The audit hook hears everything first; one that throws refuses it.
async function audit() {
  const { system, actor } = await world();
  let refuse = false;
  const entries = [];
  const client = new Client(system, {
    mode: 'live',
    allow: ALL,
    audit: (entry) => {
      entries.push(entry);
      if (refuse && entry.type === 'mutation') throw new Error('not during a freeze');
    },
  });
  await client.start({ address: 'subscription/acct_1' });
  await client.repl('failures + 1');
  await client.ok('evaluate', { expression: 'plan.seats', context: 'hover' });
  refuse = true;
  await client.refused('evaluate', { expression: 'send paid', context: 'repl' }, /audit hook refused it: not during a freeze/);
  await system.flush();
  assert.equal(system.mailbox(actor).length, 0);
  refuse = false;
  const types = entries.map((entry) => `${entry.type}${entry.operation ? `:${entry.operation}` : ''}`);
  assert.deepEqual(types, ['attach', 'command', 'evaluate', 'command', 'mutation:send']);
  assert.deepEqual(entries[2], { type: 'evaluate', context: 'hover', expression: 'plan.seats', time: entries[2].time,
                                  session: client.session.id, actor: 'subscription/acct_1',
                                  sessionID: String(actor.inspect().sessionID), mode: 'live' });
  // A refusing attach hook refuses the attach.
  const shut = new Client(system, { mode: 'live', allow: ALL, audit: () => { throw new Error('closed for maintenance'); } });
  await shut.ok('initialize');
  await shut.refused('attach', { address: 'subscription/acct_1' }, /closed for maintenance/);
  assert.equal(shut.session.attached, false);
  await client.ok('disconnect');
  assert.equal(entries.at(-1).type, 'detach');
  system.close();
}

// Threads with an invoked child, stack frames inside a macrostep, the
// scopes, evaluate, setVariable and setExpression, completions.
async function state() {
  const system = createSystem({ clock: 'manual', scheduling: { actionBudget: 1 } });
  const child = system.define(statechart({ family: 'worker', revision: 'v1' }).data('jobs', 3).state('working'));
  const parent = system.define(subscription().state('supervising', (s) =>
    s.invoke((i) => i.type('scxml').id('kid').content(child))));
  const actor = system.spawn(parent, { name: 'acct_4' });
  await system.flush();
  const client = new Client(system, { mode: 'live', allow: ALL });
  await client.start({ address: 'subscription/acct_4' });

  // Scopes of the idle actor.
  let { stackFrames } = await client.ok('stackTrace', { threadId: 1 });
  assert.equal(stackFrames[0].name, 'idle in active');
  let { scopes } = await client.ok('scopes', { frameId: stackFrames[0].id });
  assert.deepEqual(scopes.map((scope) => scope.name),
                   ['Data model', 'Event', 'Configuration', 'Queues', 'Timers', 'Invocations', 'System']);
  const read = async (name) => {
    const scope = scopes.find((entry) => entry.name === name);
    return (await client.ok('variables', { variablesReference: scope.variablesReference })).variables;
  };
  const dataModel = await read('Data model');
  const plan = dataModel.find((variable) => variable.name === 'plan');
  assert.equal(plan.evaluateName, '.plan');
  assert.equal(plan.namedVariables, 2);
  const planned = await client.ok('variables', { variablesReference: plan.variablesReference });
  assert.deepEqual(planned.variables.map((variable) => [variable.name, variable.value]), [['name', '"basic"'], ['seats', '2']]);
  assert.deepEqual((await read('Configuration')).map((variable) => variable.name), ['active']);
  const system_ = Object.fromEntries((await read('System')).map((variable) => [variable.name, variable.value]));
  assert.equal(system_._sessionid, JSON.stringify(String(actor.inspect().sessionID)));
  assert.equal(system_.scheduling, '"idle"');
  await client.refused('setVariable', { variablesReference: scopes[6].variablesReference, name: 'now', value: '1' },
                       /only the Data model/);

  // setVariable writes the live data; a non-JSON value is an expression.
  const written = await client.ok('setVariable', { variablesReference: plan.variablesReference, name: 'seats', value: '5' });
  assert.equal(written.value, '5');
  assert.equal(actor.inspect().data.plan.seats, 5);
  await client.ok('setVariable', { variablesReference: plan.variablesReference, name: 'name', value: 'plan.name.toUpperCase()' });
  assert.equal(actor.inspect().data.plan.name, 'BASIC');
  const expression = await client.ok('setExpression', { expression: '.plan.seats', value: 'plan.seats * 2' });
  assert.equal(expression.value, '10');
  assert.equal(actor.inspect().data.plan.seats, 10);

  // evaluate in its contexts.
  assert.equal((await client.ok('evaluate', { expression: '.plan.seats + 1', context: 'watch' })).result, '11');
  assert.equal((await client.ok('evaluate', { expression: 'inState("active")', context: 'hover' })).result, 'true');
  assert.equal((await client.ok('evaluate', { expression: 'plan', context: 'clipboard' })).result,
               JSON.stringify({ name: 'BASIC', seats: 10 }, null, 2));
  const presented = await client.ok('evaluate', { expression: 'plan', context: 'variables' });
  assert.equal(presented.namedVariables, 2);
  await client.refused('evaluate', { expression: 'nope(', context: 'watch' }, /does not parse/);
  await client.refused('evaluate', { expression: 'undefinedThing.x', context: 'watch' }, /threw/);

  // completions: event names, states, send IDs, commands, data locations.
  const complete = async (text) => (await client.ok('completions', { text, column: text.length + 1 })).targets.map((target) => target.label);
  assert.deepEqual(await complete('send pa'), ['paid', 'payment.failed']);
  assert.deepEqual(await complete('goto loop1 lo'), ['loop1', 'loop2']);
  assert.deepEqual(await complete('ab'), ['abandon']);
  assert.deepEqual(await complete('.pl'), ['plan']);
  assert.deepEqual(await complete('plan.s'), ['seats']);
  const [target] = (await client.ok('completions', { text: 'send pai', column: 9 })).targets;
  assert.equal(target.start, 6);
  assert.equal(target.length, 3);

  // An invoked child is a thread of its own.
  const { definitionID } = actor.inspect();
  void definitionID;
  await client.repl('goto supervising');
  await system.flush();
  let { threads } = await client.ok('threads');
  assert.equal(threads.length, 2);
  assert.match(threads[1].name, /subscription\/acct_4 invocation \d+ · working/);
  assert.equal(client.events('thread').filter((event) => event.reason === 'started').length, 1);
  const invocations = await (async () => {
    ({ stackFrames } = await client.ok('stackTrace', { threadId: 1 }));
    ({ scopes } = await client.ok('scopes', { frameId: stackFrames[0].id }));
    return read('Invocations');
  })();
  const invocation = await client.ok('variables', { variablesReference: invocations[0].variablesReference });
  const thread = invocation.variables.find((variable) => variable.name === 'thread');
  assert.match(thread.value, /^"2 subscription\/acct_4 invocation \d+"$/);
  const kid = await client.ok('stackTrace', { threadId: threads[1].id });
  assert.equal(kid.stackFrames[0].name, 'idle in working');
  await client.repl('goto active');
  await system.flush();
  ({ threads } = await client.ok('threads'));
  assert.equal(threads.length, 1);
  assert.equal(client.events('thread').filter((event) => event.reason === 'exited').length, 1);

  // Frames inside a paused macrostep: the unit it runs next.
  actor.post('spin');
  for (let index = 0; index < 4; index += 1) await turn();
  ({ stackFrames } = await client.ok('stackTrace', { threadId: 1 }));
  const names = stackFrames.map((frame) => frame.name);
  assert.match(names.join(' | '), /(script \d|selecting transitions).* \| macrostep \d+ on spin/);
  const partial = await client.ok('stackTrace', { threadId: 1, startFrame: 1, levels: 1 });
  assert.equal(partial.stackFrames.length, 1);
  assert.equal(partial.totalFrames, stackFrames.length);
  ({ scopes } = await client.ok('scopes', { frameId: stackFrames[0].id }));
  const event = Object.fromEntries((await read('Event')).map((variable) => [variable.name, variable.value]));
  assert.equal(event.name, '"spin"');
  await client.refused('scopes', { frameId: 9999 }, /not valid/);
  await client.refused('variables', { variablesReference: 9999 }, /not valid/);
  await client.refused('stackTrace', { threadId: 42 }, /no thread 42/);
  await client.ok('disconnect');
  system.close();
}

// Modules and sources: every registered revision, retired ones included.
async function catalog() {
  const { system, definition } = await world();
  const client = new Client(system, { mode: 'watch' });
  await client.start({ address: 'subscription/acct_1' });
  let { modules, totalModules } = await client.ok('modules', { startModule: 0, moduleCount: 10 });
  assert.equal(totalModules, 1);
  assert.deepEqual(modules[0], { id: String(definition.id), name: 'subscription', version: 'v1' });
  const next = system.define(subscription('v2'));
  const { sources } = await client.ok('loadedSources');
  assert.deepEqual(sources.map((source) => source.name), ['subscription v1.json', 'subscription v2.json']);
  assert.ok(client.events('module').some((event) => event.reason === 'new' && event.module.version === 'v2'));
  assert.ok(client.events('loadedSource').length >= 2);
  const text = await client.ok('source', { sourceReference: sources[0].sourceReference });
  assert.equal(text.mimeType, 'application/json');
  const exported = JSON.parse(text.content);
  assert.deepEqual(exported.states.map((state) => state.id), ['active', 'past_due', 'loop1', 'loop2']);
  assert.match(text.content, /this\.failures \+ 1/, 'functions are shown as their source');
  await client.refused('source', { sourceReference: 99 }, /no such source/);
  next.retire();
  ({ modules } = await client.ok('modules'));
  assert.equal(modules.length, 1, 'a retired revision no actor runs is freed');
  assert.ok(client.events('module').some((event) => event.reason === 'removed'));
  await client.ok('disconnect');
  system.close();
}

// The session ends when the attached actor goes; closing the transport
// detaches and releases what the session held.
async function ending() {
  const { system, actor } = await world();
  const client = new Client(system, { mode: 'live', allow: ALL });
  await client.start({ address: 'subscription/acct_1' });
  actor.destroy();
  await client.ok('threads');
  assert.equal(client.events('terminated').length, 1);
  assert.match(client.output(), /is gone; the debug session ended/);
  assert.equal(client.session.attached, false);
  await client.refused('stackTrace', { threadId: 1 }, /attach/);

  const again = system.spawn(system.findDefinition({ family: 'subscription', revision: 'v1' }), { name: 'acct_1' });
  await system.flush();
  const second = new Client(system, { mode: 'live', allow: ALL });
  await second.start({ address: 'subscription/acct_1' });
  const mark = second.mark();
  await second.ok('pause', { threadId: 1 });
  await second.stoppedAfter(mark, 'pause');
  again.post('payment.failed');
  second.session.close();
  second.session.close();
  await system.flush();
  assert.equal(again.inspect().data.failures, 1, 'closing the transport released the hold');
  assert.equal(second.audit.at(-1).reason, 'the connection closed');
  const count = second.messages.length;
  await second.session.receive({ seq: 99, type: 'request', command: 'threads' });
  assert.equal(second.messages.length, count, 'a closed session answers nothing');
  system.close();
}

(async () => {
  await framing();
  await options();
  await session();
  await watchMode();
  await holding();
  await breakpoints();
  await console();
  await permissions();
  await audit();
  await state();
  await catalog();
  await ending();
  clearTimeout(watchdog);
  process.stdout.write('debug.js passed\n');
})().catch((error) => {
  process.stderr.write(`${error?.stack ?? error}\n`);
  process.exit(1);
});
