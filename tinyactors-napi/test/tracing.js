'use strict';

// Tracing (core ADR 0067, ADR 0015): the hook, the records it hands
// JavaScript, traceparent in and out, and the span-shaped data TraceSpans
// makes of them. Runs on Node and Bun.

const assert = require('node:assert/strict');
const {
  createSystem,
  statechart,
  TraceSpans,
  TRACE_KINDS,
  parseTraceparent,
  formatTraceparent,
  stepSpanId,
  TinyactorsError,
} = require('..');
const { core } = require('../lib/native');
const { nativeOf } = require('../lib/system');

const TRACE_ID = '4bf92f3577b34da6a3ce929d0e0e4736';
const traceparent = (span, flags = '01') => `00-${TRACE_ID}-${span}-${flags}`;

// A counter that goes to `done` on `go`, assigning on the way.
function counter(system) {
  return system.define(
    statechart({ family: 'counter', revision: 'v1' })
      .data('count', 0)
      .state('idle', (s) => s.on('go', (t) => t.assign('count', function () { return this.count + 1; }).target('done')))
      .state('done'),
  );
}

function recorder(system, options) {
  const batches = [];
  system.setTraceSink((records) => batches.push(records), options);
  return {
    batches,
    get records() {
      return batches.flat();
    },
    kinds() {
      return batches.flat().map((record) => record.kind);
    },
  };
}

// Without a sink core builds nothing, and a sink that wants no kind is none.
async function untraced() {
  const plain = createSystem();
  const traced = createSystem();
  const sink = recorder(traced, { kinds: [] });
  for (const system of [plain, traced]) {
    const actor = system.spawn(counter(system));
    await system.flush();
    system.post(actor, 'go');
    await system.flush();
  }
  assert.equal(sink.batches.length, 0, 'a sink that wants no kind is called with nothing');
  assert.equal(traced.stats().allocatorCalls, plain.stats().allocatorCalls, 'and costs nothing');
  plain.close();
  traced.close();
}

// One array per pump turn, in order; SPAWNED waits for the next turn.
async function batches() {
  const system = createSystem();
  const sink = recorder(system);
  const actor = system.spawn(counter(system));
  await system.flush();
  system.post(actor, 'go');
  await system.flush();
  assert.equal(sink.batches.length, 2, 'one batch per turn that wrote records');
  assert.deepEqual(sink.kinds(), [
    'spawned', 'macrostepBegin', 'dataInit', 'microstep', 'macrostepEnd',
    'macrostepBegin', 'microstep', 'assign', 'macrostepEnd',
  ]);
  const [spawned, boot, data, initial, bootEnd, begin, micro, assign, end] = sink.records;
  assert.equal(spawned.macrostep, 0, 'lifecycle records are outside macrosteps');
  assert.equal(typeof spawned.session, 'bigint');
  assert.equal(boot.macrostep, 1, 'the bootstrap is macrostep 1');
  assert.equal(data.name, 'count');
  assert.equal(data.value, 0);
  assert.equal(data.at, 'bootstrap');
  assert.deepEqual(initial.entered, ['idle']);
  assert.deepEqual(bootEnd.states, ['idle']);
  assert.equal(begin.event.name, 'go');
  assert.equal(begin.macrostep, 2);
  assert.deepEqual([micro.exited, micro.entered], [['idle'], ['done']]);
  assert.deepEqual([assign.at, assign.state, assign.value], ['transition', 'idle', 1]);
  assert.deepEqual(end.states, ['done']);
  assert.ok(end.actor.equals(actor), 'records name the actor');
  assert.ok(typeof end.wallTime === 'number' && end.wallTime > 1e12, 'records carry wall-clock time');
  system.close();
}

// The mask decides what is built; names are checked.
async function kinds() {
  const system = createSystem();
  const sink = recorder(system, { kinds: ['microstep'] });
  const actor = system.spawn(counter(system));
  await system.flush();
  system.post(actor, 'go');
  await system.flush();
  assert.deepEqual(sink.kinds(), ['microstep', 'microstep']);
  assert.throws(() => system.setTraceSink(() => {}, { kinds: ['nope'] }),
                (error) => error instanceof TinyactorsError && error.code === 'invalid');
  assert.equal(TRACE_KINDS.length, 22);
  system.setTraceSink(undefined);
  system.post(actor, 'go');
  await system.flush();
  assert.equal(sink.records.length, 2, 'removing the sink stops the records');
  system.close();
}

// traceparent in: the macrostep carries the trace, and its cause is the
// host's span; an unsampled one builds nothing unless asked.
async function traceparentIn() {
  const system = createSystem();
  const sink = recorder(system, { kinds: ['macrostepBegin', 'spawned'] });
  const actor = system.spawn(counter(system), { traceparent: traceparent('00f067aa0ba902b7') });
  await system.flush();
  const [spawned, boot] = sink.records;
  assert.equal(spawned.traceId, TRACE_ID);
  assert.deepEqual(spawned.cause, { session: 0n, macrostep: 0x00f067aa0ba902b7n, microstep: 0 });
  assert.equal(boot.traceId, TRACE_ID, 'the bootstrap runs under the spawn\'s trace');
  assert.equal(boot.sampled, true);

  system.post(actor, 'go', undefined, { traceparent: traceparent('b7ad6b7169203331', '00') });
  await system.flush();
  assert.equal(sink.records.length, 2, 'an unsampled macrostep builds no record');
  system.setTraceSink((records) => sink.batches.push(records), { kinds: ['macrostepBegin'], unsampled: true });
  const other = system.spawn(counter(system));
  await system.flush();
  system.post(other, 'go', undefined, { traceparent: traceparent('b7ad6b7169203331', '00') });
  await system.flush();
  const unsampled = sink.records.at(-1);
  assert.equal(unsampled.event.name, 'go');
  assert.equal(unsampled.sampled, false, 'a sink that asks for unsampled macrosteps gets them');
  assert.throws(() => system.post(actor, 'go', undefined, { traceparent: 'nonsense' }),
                (error) => error instanceof TinyactorsError && error.code === 'invalid');
  system.close();
}

// W3C traceparent helpers, and span IDs the same as core derives them.
function helpers() {
  assert.deepEqual(parseTraceparent(traceparent('00f067aa0ba902b7')),
                   { traceId: TRACE_ID, parentSpanId: '00f067aa0ba902b7', flags: 1, sampled: true });
  assert.equal(parseTraceparent(`00-${'0'.repeat(32)}-00f067aa0ba902b7-01`), undefined, 'an all-zero trace ID');
  assert.equal(parseTraceparent('ff-' + TRACE_ID + '-00f067aa0ba902b7-01'), undefined, 'version ff');
  assert.equal(parseTraceparent(undefined), undefined);
  assert.equal(formatTraceparent({ traceId: TRACE_ID, spanId: '00f067aa0ba902b7' }), traceparent('00f067aa0ba902b7'));
  assert.equal(formatTraceparent({ traceId: TRACE_ID, spanId: '00f067aa0ba902b7', sampled: false }),
               traceparent('00f067aa0ba902b7', '00'));
  // Core's golden values (tinyactors-core/tests/trace_test.c).
  assert.equal(stepSpanId(1n, 1n, 0), '049e7f1d71e53363');
  assert.equal(stepSpanId(1n, 1n, 1), '304917c348f35bf4');
  assert.equal(stepSpanId(1n, 2n, 0), '79368ebf22c90f5b');
  assert.equal(stepSpanId(2n, 1n, 0), '6412a7d0f1c7345a');
  assert.equal(stepSpanId(0n, 0x1234n, 0), '0000000000001234', 'a host cause is its own span ID');
}

// Two actors: the first sends to the second by name; the second's
// macrostep names the send as its cause, and inherits the trace. As spans,
// the receiver's macrostep sits under the sender's step that sent.
async function causality() {
  const system = createSystem();
  const sink = recorder(system);
  const receiver = system.define(
    statechart({ family: 'receiver', revision: 'v1' })
      .state('waiting', (s) => s.on('ping', (t) => t.target('pinged')))
      .state('pinged'),
  );
  const sender = system.define(
    statechart({ family: 'sender', revision: 'v1' })
      .state('idle', (s) => s.on('go', (t) => t.target('sent').send('ping', (m) => m.to('#_actor_receiver/r'))))
      .state('sent'),
  );
  system.spawn(receiver, { name: 'r' });
  const actor = system.spawn(sender, { name: 's' });
  await system.flush();
  system.post(actor, 'go', undefined, { traceparent: traceparent('00f067aa0ba902b7') });
  await system.flush();
  const sent = sink.records.find((record) => record.kind === 'sent');
  const ping = sink.records.find((record) => record.kind === 'macrostepBegin' && record.event?.name === 'ping');
  assert.ok(sent && ping, `a send and the macrostep it caused: ${sink.kinds()}`);
  assert.equal(ping.traceId, TRACE_ID, 'the receiver runs in the sender\'s trace');
  assert.deepEqual(ping.cause, { session: sent.session, macrostep: BigInt(sent.macrostep), microstep: sent.microstep });

  const all = [];
  new TraceSpans({ onSpan: (span) => all.push(span), system }).write(sink.records);
  const go = all.find((span) => span.name === 'scxml.macrostep go');
  const goStep = all.find((span) => span.name === 'scxml.microstep go');
  const pinged = all.find((span) => span.name === 'scxml.macrostep ping');
  assert.equal(go.parentSpanId, '00f067aa0ba902b7', 'a delivery with a traceparent sits under the host\'s span');
  assert.equal(goStep.parentSpanId, go.spanId);
  assert.equal(pinged.parentSpanId, goStep.spanId, 'the receiver\'s macrostep sits under the step that sent');
  assert.equal(pinged.traceId, TRACE_ID);
  assert.ok(goStep.events.some((event) => event.name === 'scxml.send'), 'the send is an event of its step');
  system.close();
}

// traceparent out: an I/O processor gets the sending step's traceparent.
async function traceparentOut() {
  const requests = [];
  const system = createSystem({ io: { webhook: { send: (request) => requests.push(request) } } });
  const sink = recorder(system, { kinds: ['sent'] });
  const definition = system.define(
    statechart({ family: 'hook', revision: 'v1' })
      .state('idle', (s) => s.on('go', (t) => t.send('call', (m) => m.via('webhook').to('client-7').data(() => 1)))),
  );
  const actor = system.spawn(definition);
  await system.flush();
  system.post(actor, 'go', undefined, { traceparent: traceparent('00f067aa0ba902b7') });
  await system.flush();
  assert.equal(requests.length, 1);
  const sent = sink.records.find((record) => record.kind === 'sent');
  const parsed = parseTraceparent(requests[0].traceparent);
  assert.equal(parsed.traceId, TRACE_ID);
  assert.equal(parsed.parentSpanId, stepSpanId(sent.session, sent.macrostep, sent.microstep),
               'the processor\'s parent span is the step that sent');
  assert.equal(requests[0].trace, undefined, 'the raw trace parent does not leak into the request');
  system.close();
}

// Spans: a macrostep with its microsteps, events on the step they
// happened in, values only when asked for.
async function spans() {
  const system = createSystem();
  const sink = recorder(system);
  const actor = system.spawn(counter(system));
  await system.flush();
  system.post(actor, 'go', undefined, { traceparent: traceparent('00f067aa0ba902b7') });
  await system.flush();
  const plain = [];
  new TraceSpans({ onSpan: (span) => plain.push(span), system }).write(sink.records);
  const macrostep = plain.find((span) => span.name === 'scxml.macrostep go');
  const microstep = plain.find((span) => span.name === 'scxml.microstep go');
  assert.equal(macrostep.traceId, TRACE_ID);
  assert.deepEqual(macrostep.attributes['scxml.state'], ['done']);
  assert.equal(macrostep.attributes['scxml.event.name'], 'go');
  assert.equal(microstep.parentSpanId, macrostep.spanId);
  assert.deepEqual(microstep.attributes['scxml.state.entered'], ['done']);
  const assign = microstep.events.find((event) => event.name === 'scxml.assign');
  assert.ok(assign, 'the assignment is an event of its microstep');
  assert.equal(assign.attributes['scxml.value'], undefined, 'values are left out by default');
  assert.ok(microstep.startTime <= microstep.endTime && macrostep.endTime >= microstep.endTime);
  const withValues = [];
  new TraceSpans({ onSpan: (span) => withValues.push(span), system, values: true }).write(sink.records);
  const valued = withValues.find((span) => span.name === 'scxml.microstep go').events.find((event) => event.name === 'scxml.assign');
  assert.equal(valued.attributes['scxml.value'], '1');
  // "link" makes the macrostep a root, linked to its cause.
  const linked = [];
  new TraceSpans({ onSpan: (span) => linked.push(span), system, parent: 'link' }).write(sink.records);
  const root = linked.find((span) => span.name === 'scxml.macrostep go');
  assert.equal(root.parentSpanId, undefined);
  assert.deepEqual(root.links, [{ traceId: TRACE_ID, spanId: '00f067aa0ba902b7' }]);
  system.close();
}

// Spans name transitions and actions through the definition (core ADR
// 0071), and keep the compiled IDs beside the labels; a record's
// expression IDs resolve to the function or the text the chart gave.
async function labels() {
  const system = createSystem();
  const sink = recorder(system);
  const definition = system.define(
    statechart({ family: 'labelled', revision: 'v1' })
      .data('n', 0)
      .data('item', null)
      .state('idle', (s) => s.on('order.placed', (t) => t.when(() => true).target('busy')))
      .state('busy', (s) => {
        s.entry((a) =>
          a.choose(
            () => false,
            (then) => then.assign('n', () => 1),
            (otherwise) => otherwise.forEach(() => [1, 2], 'item', (body) =>
              body.assign('n', function () { return this.n + this.item; })),
          ));
        s.on('poke', (t) => t.internal().assign('n', () => 0));
      }),
  );
  const actor = system.spawn(definition);
  await system.flush();
  system.post(actor, 'order.placed');
  system.post(actor, 'poke');
  await system.flush();
  assert.throws(() => new TraceSpans({ onSpan: () => {} }), /options\.system/);
  const all = [];
  new TraceSpans({ onSpan: (span) => all.push(span), system }).write(sink.records);

  const placed = all.find((span) => span.name === 'scxml.microstep order.placed');
  assert.deepEqual(placed.attributes['scxml.transition'], ['idle → busy on order.placed [guarded]']);
  assert.deepEqual(placed.attributes['scxml.transition.id'], [0]);
  const guard = all.find((span) => span.name === 'scxml.macrostep order.placed').events
    .find((event) => event.name === 'scxml.condition');
  assert.equal(guard.attributes['scxml.transition'], 'idle → busy on order.placed [guarded]');
  assert.equal(guard.attributes['scxml.transition.id'], 0);
  const assigns = placed.events.filter((event) => event.name === 'scxml.assign');
  assert.equal(assigns.length, 2, 'the <foreach> body assigned twice');
  assert.equal(assigns[0].attributes['scxml.action'],
               '<assign> in the body of <foreach> in else of <if> in onentry of busy');
  assert.equal(typeof assigns[0].attributes['scxml.action.id'], 'number');

  const poke = all.find((span) => span.name === 'scxml.microstep poke');
  assert.deepEqual(poke.attributes['scxml.transition'], ['busy on poke (internal)']);
  const reset = poke.events.find((event) => event.name === 'scxml.assign');
  assert.equal(reset.attributes['scxml.action'], '<assign> in busy on poke (internal)');

  // Expressions come back as the chart gave them: a function, or text.
  const native = nativeOf(system);
  const condition = sink.records.find((record) => record.kind === 'condition');
  assert.equal(typeof core.definition_expression(native, condition.definition, condition.expr), 'function');
  const assigned = sink.records.find((record) => record.kind === 'assign');
  assert.equal(core.definition_expression(native, assigned.definition, assigned.expr), 'n',
               'an assignment\'s location is the data name the chart gave');
  system.close();
}

(async () => {
  await untraced();
  await batches();
  await kinds();
  await traceparentIn();
  helpers();
  await causality();
  await traceparentOut();
  await spans();
  await labels();
  console.log('tracing.js passed');
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
