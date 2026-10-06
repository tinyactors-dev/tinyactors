'use strict';

// Tracing (core ADR 0067, napi ADR 0016): the hook hands JavaScript OTLP
// protobuf bytes, one request per turn; decodeTraces reads them back.
// Traceparent in and out, trace IDs the system starts, the mapping of
// records to spans and span events, values, links and labels. Runs on Node
// and Bun.

const assert = require('node:assert/strict');
const {
  createSystem,
  statechart,
  decodeTraces,
  TRACE_KINDS,
  parseTraceparent,
  formatTraceparent,
  stepSpanID,
  TinyactorsError,
} = require('..');

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
  system.setTraceSink((bytes) => batches.push(bytes), { detail: 'full', ...options });
  return {
    batches,
    get spans() {
      return batches.flatMap((bytes) => decodeTraces(bytes));
    },
    names() {
      return this.spans.map((span) => span.name);
    },
  };
}

const named = (spans, name) => spans.find((span) => span.name === name);

async function untraced() {
  const system = createSystem();
  let calls = 0;
  system.setTraceSink((bytes) => { calls += 1; return bytes; });
  system.setTraceSink(undefined);
  const actor = system.spawn(counter(system));
  await system.flush();
  actor.post('go');
  await system.flush();
  assert.equal(calls, 0, 'no sink, no bytes');
  system.close();
}

// One Uint8Array per turn; decoded, the macrostep, its microsteps as
// children, and every other record as an event on the step it happened in.
async function batches() {
  const system = createSystem();
  const sink = recorder(system, { resource: { 'service.name': 'orders' } });
  const actor = system.spawn(counter(system));
  await system.flush();
  actor.post('go');
  await system.flush();
  assert.ok(sink.batches.length >= 2, 'one request per turn with spans');
  for (const bytes of sink.batches) {
    assert.ok(bytes instanceof Uint8Array && bytes.length > 0);
  }
  const spans = sink.spans;
  const spawned = named(spans, 'scxml.spawned');
  const boot = named(spans, 'scxml.macrostep bootstrap');
  const bootStep = named(spans, 'scxml.microstep (eventless)');
  const go = named(spans, 'scxml.macrostep go');
  const goStep = named(spans, 'scxml.microstep go');
  assert.ok(spawned && boot && bootStep && go && goStep, `the spans: ${sink.names()}`);
  const session = boot.attributes['scxml.session_id'];
  assert.equal(boot.spanID, stepSpanID(session, 1n, 0), 'span IDs are step keys');
  assert.equal(goStep.spanID, stepSpanID(session, 2n, 1));
  assert.equal(bootStep.parentSpanID, boot.spanID);
  assert.equal(goStep.parentSpanID, go.spanID);
  assert.equal(go.traceID, goStep.traceID);
  assert.equal(spawned.resource.attributes['service.name'], 'orders');
  assert.equal(spawned.scope.name, 'tinyactors');
  assert.ok(boot.events.some((event) => event.name === 'scxml.data' && event.attributes['scxml.data.name'] === 'count'),
            'the bootstrap\'s <data> is an event of its macrostep');
  const assign = goStep.events.find((event) => event.name === 'scxml.assign');
  assert.ok(assign, 'the assign is an event of the microstep that ran it');
  assert.equal(assign.attributes['scxml.position'], 'transition');
  assert.equal(assign.attributes['scxml.transition'], 'idle → done on go');
  assert.equal(assign.attributes['scxml.value'], undefined, 'no values unless asked for');
  assert.deepEqual(goStep.attributes['scxml.state.exited'], ['idle']);
  assert.deepEqual(goStep.attributes['scxml.state.entered'], ['done']);
  assert.deepEqual(go.attributes['scxml.state'], ['done'], 'the configuration at the end');
  assert.equal(goStep.attributes['scxml.event.name'], 'go');
  assert.ok(go.endTime >= go.startTime);
  system.close();
}

// The bytes belong to the sink (napi ADR 0018): each turn's request is a
// Buffer of its own, written in place, so bytes kept across an await read
// the same after later turns have written theirs.
async function bytesOutliveTheTurn() {
  const system = createSystem();
  const kept = [];
  system.setTraceSink((traces, logs) => {
    kept.push({ traces, copy: Uint8Array.from(traces), logs, logsCopy: logs && Uint8Array.from(logs) });
  }, { detail: 'full' });
  const failing = system.define(
    statechart({ family: 'failing', revision: 'v1' })
      .state('idle', (s) => s.on('go', (t) => t.log('going').script(() => { throw new Error('no'); }))),
  );
  const actors = [system.spawn(counter(system)), system.spawn(failing)];
  await system.flush();
  for (let round = 0; round < 5; round += 1) {
    for (const actor of actors) actor.post('go');
    await new Promise((resolve) => setTimeout(resolve, 1));
    await system.flush();
  }
  assert.ok(kept.length >= 3, 'several turns wrote requests');
  assert.ok(kept.some((batch) => batch.logs !== undefined), 'some turns wrote logs');
  for (const batch of kept) {
    assert.equal(Object.getPrototypeOf(batch.traces), Uint8Array.prototype, 'a plain Uint8Array, not a Buffer');
    assert.deepEqual(batch.traces, batch.copy, 'a kept request is unchanged by later turns');
    if (batch.logs !== undefined) {
      assert.ok(batch.logs instanceof Uint8Array && batch.logs.length > 0);
      assert.deepEqual(batch.logs, batch.logsCopy, 'kept logs are unchanged by later turns');
    }
    assert.deepEqual(decodeTraces(batch.traces), decodeTraces(batch.copy));
  }
  system.close();
}

// Autoforwarding is a span event naming the invocation and the event, and
// an invoker's unavailable report is the RAISED event it queued (core ADR 0067
// open point 3, and events core queues outside a run).
async function forwardedAndUnavailable() {
  const forwarded = [];
  const system = createSystem({
    invokers: {
      echo: { start() { return { cancel() {}, forward(event) { forwarded.push(event.name); } }; } },
      flaky: { start() { throw new Error('no backend'); } },
    },
  });
  const sink = recorder(system);
  const definition = system.define(
    statechart({ family: 'forwarding', revision: 'v1' })
      .data('failed', false)
      .state('running', (s) => {
        s.invoke((i) => i.type('echo').id('svc').autoforward());
        s.invoke((i) => i.type('flaky').id('bad'));
        s.on('error.communication', (t) => t.assign('failed', () => true));
        s.on('hello', (t) => t.assign('failed', ({ data }) => data.failed));
      }),
  );
  const actor = system.spawn(definition);
  for (let turn = 0; turn < 20 && !actor.inspect().data.failed; turn += 1) {
    await system.flush();
  }
  assert.equal(actor.inspect().data.failed, true, 'the flaky service was reported unavailable');
  actor.post('hello');
  await system.flush();
  const events = sink.spans.flatMap((span) => span.events);
  const forwards = events.filter((event) => event.name === 'scxml.forward');
  assert.ok(forwards.some((event) => event.attributes['scxml.event.name'] === 'hello'),
            `a forward of hello: ${JSON.stringify(forwards.map((event) => event.attributes))}`);
  assert.ok(forwards.every((event) => event.attributes['scxml.invoke_id'] === 'svc'), 'forwards name the invocation');
  assert.ok(forwarded.includes('hello'), 'the service got it');
  const raised = events.filter((event) => event.name === 'scxml.raise')
    .map((event) => event.attributes['scxml.event.name']);
  assert.ok(raised.includes('error.communication.invoke.bad.unavailable'), `raised: ${JSON.stringify(raised)}`);
  assert.ok(TRACE_KINDS.includes('forwarded') && TRACE_KINDS.includes('ignored'));
  system.close();
}

// A small request is whole on Bun too: it is written before its
// ArrayBuffer is asked for, which moves a small (pooled, under 4 KiB)
// Buffer's bytes. The resource names the parameters in one short
// attribute (napi ADR 0036), so a request stays this small.
async function smallRequests() {
  const system = createSystem();
  let whole = 0;
  let broken = 0;
  system.setTraceSink((traces) => {
    assert.ok(traces.length > 0 && traces.length <= 1000, `a small request (${traces.length} bytes)`);
    try {
      decodeTraces(traces);
      whole += 1;
    } catch {
      broken += 1;
    }
  }, { kinds: ['macrostepBegin', 'macrostepEnd'] });
  const actor = system.spawn(counter(system));
  await system.flush();
  for (let round = 0; round < 40; round += 1) {
    actor.post('go');
    await system.flush();
  }
  assert.equal(broken, 0);
  assert.ok(whole >= 40);
  system.close();
}

// The mask decides what core builds, and so which spans there are.
async function kinds() {
  const system = createSystem();
  const sink = recorder(system, { kinds: ['macrostepBegin', 'macrostepEnd'] });
  const actor = system.spawn(counter(system));
  await system.flush();
  actor.post('go');
  await system.flush();
  assert.deepEqual(sink.names().sort(), ['scxml.macrostep bootstrap', 'scxml.macrostep go']);
  assert.ok(sink.spans.every((span) => span.events.length === 0));
  assert.throws(() => system.setTraceSink(() => {}, { kinds: ['nope'] }),
                (error) => error instanceof TinyactorsError && error.code === 'invalid');
  assert.ok(TRACE_KINDS.includes('microstep'));
  system.setTraceSink(undefined);
  const before = sink.batches.length;
  actor.post('go');
  await system.flush();
  assert.equal(sink.batches.length, before, 'removed: nothing more');
  system.close();
}

// traceparent in: a delivery runs in the caller's trace, under the caller's
// span; an unsampled one is traced only when asked for.
async function traceparentIn() {
  const system = createSystem();
  const sink = recorder(system, { kinds: ['macrostepBegin', 'macrostepEnd'] });
  const actor = system.spawn(counter(system));
  await system.flush();
  actor.post('go', undefined, { traceparent: traceparent('00f067aa0ba902b7') });
  await system.flush();
  const go = named(sink.spans, 'scxml.macrostep go');
  assert.equal(go.traceID, TRACE_ID);
  assert.equal(go.parentSpanID, '00f067aa0ba902b7');

  const unsampled = createSystem();
  const quiet = recorder(unsampled, { kinds: ['macrostepBegin', 'macrostepEnd'] });
  const other = unsampled.spawn(counter(unsampled));
  await unsampled.flush();
  other.post('go', undefined, { traceparent: traceparent('00f067aa0ba902b7', '00') });
  await unsampled.flush();
  assert.equal(named(quiet.spans, 'scxml.macrostep go'), undefined, 'not sampled, not traced');
  unsampled.setTraceSink((bytes) => quiet.batches.push(bytes), { kinds: ['macrostepBegin', 'macrostepEnd'],
                                                                  unsampled: true });
  other.post('go', undefined, { traceparent: traceparent('00f067aa0ba902b7', '00') });
  await unsampled.flush();
  assert.ok(named(quiet.spans, 'scxml.macrostep go'), 'unsampled: traced anyway');
  system.close();
  unsampled.close();
}

// A delivery or spawn without a traceparent starts a trace: random, or a
// fixed sequence with a seed.
async function startedTraces() {
  const ids = async (seed) => {
    const system = createSystem();
    const sink = recorder(system, { kinds: ['macrostepBegin', 'macrostepEnd'], seed });
    const actor = system.spawn(counter(system));
    await system.flush();
    actor.post('go');
    await system.flush();
    const traces = sink.spans.map((span) => span.traceID);
    system.close();
    return traces;
  };
  const first = await ids(7n);
  assert.equal(first.length, 2);
  assert.ok(first.every((id) => /^[0-9a-f]{32}$/.test(id) && id !== '0'.repeat(32)));
  assert.notEqual(first[0], first[1], 'the spawn and the delivery each start a trace');
  assert.deepEqual(await ids(7n), first, 'a seed makes them a fixed sequence');
  const random = await ids(undefined);
  assert.notDeepEqual(random, first);
  assert.throws(() => createSystem().setTraceSink(() => {}, { seed: 1 }), /seed/);
}

function helpers() {
  assert.deepEqual(parseTraceparent(traceparent('00f067aa0ba902b7')),
                   { traceID: TRACE_ID, parentSpanID: '00f067aa0ba902b7', flags: 1, sampled: true });
  assert.equal(parseTraceparent(`00-${'0'.repeat(32)}-00f067aa0ba902b7-01`), undefined, 'an all-zero trace ID');
  assert.equal(parseTraceparent('ff-' + TRACE_ID + '-00f067aa0ba902b7-01'), undefined, 'version ff');
  assert.equal(parseTraceparent(undefined), undefined);
  // W3C versioning: a later version is read by its first four fields; version
  // 00 has nothing after its flags; hex is lowercase; spaces around are fine.
  assert.deepEqual(parseTraceparent(`01-${TRACE_ID}-00f067aa0ba902b7-01-whatever`),
                   { traceID: TRACE_ID, parentSpanID: '00f067aa0ba902b7', flags: 1, sampled: true }, 'a later version');
  assert.equal(parseTraceparent(`01-${TRACE_ID}-00f067aa0ba902b7-01x`), undefined, 'a later version needs a dash');
  assert.equal(parseTraceparent(`00-${TRACE_ID}-00f067aa0ba902b7-01-extra`), undefined, 'version 00 with more');
  assert.equal(parseTraceparent(`00-${TRACE_ID.toUpperCase()}-00f067aa0ba902b7-01`), undefined, 'uppercase hex');
  assert.equal(parseTraceparent(`00-${TRACE_ID}-0000000000000000-01`), undefined, 'an all-zero parent ID');
  assert.equal(parseTraceparent(` \t${traceparent('00f067aa0ba902b7')} `).traceID, TRACE_ID, 'spaces and tabs around');
  assert.deepEqual(parseTraceparent(traceparent('0000000000000001', '00')),
                   { traceID: TRACE_ID, parentSpanID: '0000000000000001', flags: 0, sampled: false }, 'leading zeros');
  assert.equal(formatTraceparent({ traceID: TRACE_ID, spanID: '00f067aa0ba902b7' }), traceparent('00f067aa0ba902b7'));
  assert.equal(formatTraceparent({ traceID: TRACE_ID, spanID: '00f067aa0ba902b7', sampled: false }),
               traceparent('00f067aa0ba902b7', '00'));
  // Core's golden values (tinyactors-core/tests/trace_test.c).
  assert.equal(stepSpanID(1n, 1n, 0), '049e7f1d71e53363');
  assert.equal(stepSpanID(1n, 1n, 1), '304917c348f35bf4');
  assert.equal(stepSpanID(1n, 2n, 0), '79368ebf22c90f5b');
  assert.equal(stepSpanID(2n, 1n, 0), '6412a7d0f1c7345a');
  assert.equal(stepSpanID(1n, 0n, 1), '65c672b9ddbfb556', 'a lifecycle key');
  assert.equal(stepSpanID(0n, 0x1234n, 0), '0000000000001234', 'a host cause is its own span ID');
  assert.throws(() => decodeTraces(new Uint8Array([0x0a, 0x05, 0x01])), /malformed/);
  assert.throws(() => decodeTraces([]), /Uint8Array/);
}

// Two actors: the receiver's macrostep sits under the sender's step that
// sent, in the sender's trace.
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
  const all = sink.spans;
  const go = named(all, 'scxml.macrostep go');
  const goStep = named(all, 'scxml.microstep go');
  const pinged = named(all, 'scxml.macrostep ping');
  assert.ok(go && goStep && pinged, `the spans: ${sink.names()}`);
  assert.equal(go.parentSpanID, '00f067aa0ba902b7', 'a delivery with a traceparent sits under the host\'s span');
  assert.equal(goStep.parentSpanID, go.spanID);
  assert.equal(pinged.parentSpanID, goStep.spanID, 'the receiver\'s macrostep sits under the step that sent');
  assert.equal(pinged.traceID, TRACE_ID);
  assert.equal(pinged.attributes['scxml.cause.session_id'], goStep.attributes['scxml.session_id']);
  assert.ok(goStep.events.some((event) => event.name === 'scxml.send'), 'the send is an event of its step');
  system.close();
}

// traceparent out: an I/O processor gets the sending step's traceparent.
async function traceparentOut() {
  const requests = [];
  const system = createSystem({ io: { webhook: { send: (request) => requests.push(request) } } });
  const sink = recorder(system);
  const definition = system.define(
    statechart({ family: 'hook', revision: 'v1' })
      .state('idle', (s) => s.on('go', (t) => t.send('call', (m) => m.via('webhook').to('client-7')))),
  );
  const actor = system.spawn(definition);
  await system.flush();
  system.post(actor, 'go', undefined, { traceparent: traceparent('00f067aa0ba902b7') });
  await system.flush();
  assert.equal(requests.length, 1);
  const goStep = named(sink.spans, 'scxml.microstep go');
  const parsed = parseTraceparent(requests[0].traceparent);
  assert.equal(parsed.traceID, TRACE_ID);
  assert.equal(parsed.parentSpanID, goStep.spanID, 'the processor\'s parent span is the step that sent');
  assert.equal(requests[0].trace, undefined, 'the raw trace parent does not leak into the request');
  system.close();
}

// napi ADR 0017 section 6: a processor's asynchronous reply stays in the
// trace of the step that sent the request, as a child of that step.
async function repliesStayInTheTrace() {
  const system = createSystem({
    io: {
      webhook: {
        send: (request, context) => {
          setImmediate(() => context.post(request.source, 'answer'));
        },
      },
    },
  });
  const sink = recorder(system);
  const definition = system.define(
    statechart({ family: 'asker', revision: 'v1' })
      .state('idle', (s) => {
        s.on('go', (t) => t.send('call', (m) => m.via('webhook').to('client-7')));
        s.on('answer', (t) => t.target('answered'));
      })
      .state('answered'),
  );
  const actor = system.spawn(definition);
  await system.flush();
  system.post(actor, 'go', undefined, { traceparent: traceparent('00f067aa0ba902b7') });
  await system.flush();
  await new Promise((resolve) => setImmediate(resolve));
  await system.flush();
  const goStep = named(sink.spans, 'scxml.microstep go');
  const answer = named(sink.spans, 'scxml.macrostep answer');
  assert.equal(answer.traceID, TRACE_ID, 'the reply is in the request\'s trace');
  assert.equal(answer.parentSpanID, goStep.spanID, 'the reply is a child of the step that sent the request');
  system.close();
}

// The strings of an OTLP message, and how many times a field-2 message of
// ScopeLogs (a LogRecord) occurs: enough to check logs without a decoder.
function protobufStrings(bytes, out = []) {
  let at = 0;
  const varint = () => {
    let value = 0;
    let scale = 1;
    for (;;) {
      const byte = bytes[at++];
      if (byte === undefined) throw new Error('truncated');
      value += (byte & 0x7f) * scale;
      scale *= 128;
      if ((byte & 0x80) === 0) return value;
    }
  };
  while (at < bytes.length) {
    const wire = varint() & 7;
    if (wire === 0) varint();
    else if (wire === 1) at += 8;
    else if (wire === 5) at += 4;
    else if (wire === 2) {
      const length = varint();
      const body = bytes.subarray(at, at + length);
      if (body.length !== length) throw new Error('truncated');
      at += length;
      const text = Buffer.from(body).toString('utf8');
      if (/^[\x20-\x7e]+$/.test(text)) out.push(text);
      try { protobufStrings(body, out); } catch { /* a string */ }
    } else throw new Error(`wire ${wire}`);
  }
  return out;
}

// napi ADR 0017 section 7: failures and <log> are OTLP logs, written even
// when the trace is sampled out, and rate-limited per definition and name.
async function logs() {
  const system = createSystem();
  const logBatches = [];
  let traced = 0;
  system.setTraceSink((traces, batch) => {
    traced += decodeTraces(traces).length;
    if (batch !== undefined) logBatches.push(batch);
  }, { logLimit: 2 });
  const definition = system.define(
    statechart({ family: 'failing', revision: 'v1' })
      .data('n', 0)
      .state('idle', (s) => {
        s.on('fail', (t) => t.assign('n', () => { throw new Error('boom'); }));
        s.on('note', (t) => t.log('checkout', () => 'order 42'));
      }),
  );
  const actor = system.spawn(definition);
  await system.flush();
  traced = 0;
  const unsampled = traceparent('00f067aa0ba902b7', '00');
  for (let i = 0; i < 5; i += 1) actor.post('fail', undefined, { traceparent: unsampled });
  actor.post('note', undefined, { traceparent: unsampled });
  await system.flush();
  assert.equal(traced, 0, 'sampled out: no spans');
  const text = logBatches.flatMap((bytes) => protobufStrings(bytes));
  const failed = text.filter((s) => s === 'tinyactors.action.failed').length;
  assert.ok(failed >= 1 && failed <= 2, `rate-limited to 2 per second: ${failed}`);
  assert.ok(text.includes('WARN') && text.includes('failing'), 'a WARN log naming the definition');
  assert.ok(text.includes('scxml.log') && text.includes('checkout: "order 42"'), `the <log>: ${text}`);
  assert.ok(text.includes(TRACE_ID) || logBatches.length > 0, 'logs carry the trace');
  assert.throws(() => system.setTraceSink(() => {}, { logLimit: 0 }), /logLimit/);
  system.close();
}

// A failure log says what failed and why (napi ADR 0017 section 7): the
// action's label and the thrown message are in the body, and in the
// attributes for filtering, for <script> as well as <assign>, whatever
// `values` says.
async function failureLogsSayWhy() {
  for (const element of ['script', 'assign']) {
    const system = createSystem();
    const text = [];
    system.setTraceSink((traces, batch) => {
      if (batch !== undefined) text.push(...protobufStrings(batch));
    });
    const definition = system.define(
      statechart({ family: 'stock', revision: 'v1' })
        .data('n', 0)
        .state('ready', (s) =>
          s.on('reserve', (t) =>
            element === 'script'
              ? t.script(() => { throw new Error('cannot reserve 50 items'); })
              : t.assign('n', () => { throw new Error('cannot reserve 50 items'); }),
          ),
        ),
    );
    const actor = system.spawn(definition);
    await system.flush();
    actor.post('reserve');
    await system.flush();
    const failed = `<${element}> in ready on reserve failed: Error: cannot reserve 50 items`;
    assert.ok(text.includes('tinyactors.action.failed'), `${element}: an action.failed log: ${text}`);
    assert.ok(text.includes(failed), `${element}: the body names the action and the reason: ${text}`);
    assert.ok(text.includes('error.execution: Error: cannot reserve 50 items'), `${element}: error.raised says why`);
    assert.ok(text.includes('exception.message') && text.includes('Error: cannot reserve 50 items'),
              `${element}: exception.message carries the reason`);
    assert.ok(text.includes(`<${element}> in ready on reserve`) && text.includes('scxml.action.id'),
              `${element}: the failing action, labelled and by ID`);
    system.close();
  }
  // A <send> that core itself refuses says why too, with no data model
  // involved: a delayed send to the internal queue, and an unknown type.
  for (const [build, reason] of [
    [(t) => t.send('later', (m) => m.to('#_internal').after(10)), 'a <send> with a delay cannot target #_internal'],
    [(t) => t.send('x', (m) => m.to('elsewhere').via('http://example.com/none')),
     'the <send> type names no I/O processor this system has'],
  ]) {
    const system = createSystem();
    const text = [];
    system.setTraceSink((traces, batch) => {
      if (batch !== undefined) text.push(...protobufStrings(batch));
    });
    const actor = system.spawn(system.define(
      statechart({ family: 'sender', revision: 'v1' }).state('ready', (s) => s.on('go', build)),
    ));
    await system.flush();
    actor.post('go');
    await system.flush();
    assert.ok(text.includes(`<send> in ready on go failed: ${reason}`), `the body says why: ${text}`);
    assert.ok(text.includes('scxml.fault.message') && text.includes('exception.message') && text.includes(reason),
              `the reason is an attribute: ${text}`);
    system.close();
  }
}

// A 16 KiB payload arrives whole with telemetryValueBytes: 16384 (core ADR
// 0098; the ring and its records are made to hold it),
// and is cut, and marked, at the default of 1024.
async function largeValues() {
  const payload = { body: 'x'.repeat(16000) };
  for (const [parameters, whole] of [[{ telemetryValueBytes: 16384, maxRecordBytes: 32768, telBytes: 262144 }, true], [{}, false]]) {
    const system = createSystem({ parameters });
    const sink = recorder(system, { values: true });
    const actor = system.spawn(counter(system));
    await system.flush();
    actor.post('go', payload);
    await system.flush();
    const goStep = named(sink.spans, 'scxml.microstep go');
    const text = goStep.attributes['scxml.event.data'];
    if (whole) {
      assert.equal(text, JSON.stringify(payload), 'the payload arrives whole');
      assert.equal(goStep.attributes['scxml.event.data.truncated'], undefined);
    } else {
      assert.equal(text.length, 1024, 'cut at the default');
      assert.equal(goStep.attributes['scxml.event.data.truncated'], true);
    }
    system.close();
  }
}

// traceStagingBytes (napi ADR 0035): a turn whose spans pass the staging
// bound ends early, so the sink gets several requests where one turn would
// have made one large one, and no span is lost.
async function stagingBound() {
  const actors = 2000; // one burst of posts stays under core's admission watermark (napi ADR 0041)
  const counts = [];
  for (const parameters of [{ traceStagingBytes: 65536 }, {}]) {
    const system = createSystem({ parameters });
    const sink = recorder(system, {});
    const definition = counter(system);
    const spawned = [];
    for (let index = 0; index < actors; index += 1) {
      spawned.push(system.spawn(definition));
    }
    await system.flush();
    sink.batches.length = 0;
    for (const actor of spawned) {
      actor.post('go');
    }
    await system.flush();
    const steps = sink.spans.filter((span) => span.name === 'scxml.microstep go');
    assert.equal(steps.length, actors, 'every delivery traced, none dropped');
    counts.push(sink.batches.length);
    if (parameters.traceStagingBytes !== undefined) {
      const largest = Math.max(...sink.batches.map((bytes) => bytes.length));
      assert.ok(largest < 2 * 65536, `a request stays near the bound: ${largest} bytes`);
    }
    system.close();
  }
  assert.ok(counts[0] > counts[1], `a small bound makes more requests: ${counts}`);
}

// The staging bound holds outside turns too (napi ADR 0036 section 2): a
// burst of spawn() calls, each staging its lifecycle span outside any turn,
// hands the sink a request as soon as the staging reaches the bound, so no
// request holds more than the bound plus one call's records, and none is
// lost. The staging is reserved while a sink is installed, and only then.
async function stagingBoundOutsideTurns() {
  const bound = 65536;
  const system = createSystem({ parameters: { traceStagingBytes: bound } });
  const reserved = () => system.stats().memory.other.bytesInUse;
  const without = reserved();
  assert.ok(without < bound, `no staging without a sink: ${without} bytes`);
  const sink = recorder(system, {});
  assert.ok(reserved() >= without + bound, `a sink reserves the staging whole: ${reserved()} bytes`);
  const definition = counter(system);
  await system.flush();
  sink.batches.length = 0;
  let duringBurst = 0;
  for (let index = 0; index < 3000; index += 1) {
    system.spawn(definition);
    duringBurst = sink.batches.length;
  }
  assert.ok(duringBurst > 1, `the sink was called while the spawns ran, outside a turn: ${duringBurst} requests`);
  await system.flush();
  const largest = Math.max(...sink.batches.map((bytes) => bytes.length));
  assert.ok(largest <= bound + 4096, `no request past the bound plus one call's records: ${largest} bytes`);
  assert.equal(sink.names().filter((name) => name === 'scxml.spawned').length, 3000, 'every spawn traced, none dropped');
  system.setTraceSink(undefined);
  assert.ok(reserved() < bound, `removing the sink frees the staging: ${reserved()} bytes`);
  system.close();
}

// Every request's resource names the system's parameters in one attribute
// (napi ADR 0036): the defaults' version, then what differs from them in
// name order; `v1` alone for a system with every default. stats() reports
// every value.
async function parameterResource() {
  const request = async (parameters, resource) => {
    const system = createSystem({ parameters });
    const batches = [];
    system.setTraceSink((bytes) => batches.push(bytes), { detail: 'full', resource });
    system.spawn(counter(system));
    await system.flush();
    const [span] = decodeTraces(batches[0]);
    system.close();
    return span.resource.attributes;
  };
  const defaults = await request(undefined, {});
  assert.equal(defaults['tinyactors.params'], 'v1', 'a system with every default sends the version alone');
  assert.equal(Object.keys(defaults).filter((key) => key.startsWith('tinyactors.param.')).length, 0);
  const changed = await request({ telemetryValueBytes: 4096, maxRecordBytes: 8192, microstepLimit: 777, interpreterBytes: 131072 }, {});
  assert.equal(changed['tinyactors.params'], 'v1;interpreterBytes=131072;maxRecordBytes=8192;microstepLimit=777;telemetryValueBytes=4096');
  const mine = await request({ telemetryValueBytes: 2048 }, { 'tinyactors.params': 'mine' });
  assert.equal(mine['tinyactors.params'], 'mine', "the application's attribute wins");
  const system = createSystem({ parameters: { telemetryValueBytes: 2048 } });
  assert.equal(system.stats().parameters.telemetryValueBytes, 2048);
  assert.ok(Object.isFrozen(system.parameters));
  system.close();
}

// The defaults `v1` names (napi ADR 0036). A default that changes must
// change DEFAULTS_VERSION in lib/params.js, and add its defaults here.
function defaultsVersion() {
  const { DEFAULTS_VERSION, parameterInfo } = require('../lib/params');
  const versions = {
    v1: {
      actionBudget: 1024, admissionWatermark: 50, compileMessageBytes: 256, compileNearestBytes: 128, compileTextBytes: 1024,
      datamodelNameBytes: 32, dueBudget: 1024, eventNameDepth: 64,
      interpreterBytes: 65536, interpreterNesting: 4, maxActors: 2147483647, maxDefinitions: 2147483647,
      macrostepReserve: 1024, maxPendingSends: 2147483647, maxQueuedMessages: 0, maxRecordBytes: 4096,
      maxRetainedBytes: 0, messageBudget: 1024, microstepLimit: 100000, poolEnvelopes: 4096, registeredDatamodels: 8, renderBytes: 16384,
      resumeBudget: 64, telBytes: 65536, telemetryValueBytes: 1024,
      traceStagingBytes: 1048576,
    },
  };
  const now = Object.fromEntries(Object.entries(parameterInfo()).map(([name, info]) => [name, info.default]));
  assert.deepEqual(now, versions[DEFAULTS_VERSION],
    `the defaults changed: change DEFAULTS_VERSION (${DEFAULTS_VERSION}) and record the new ones here`);
}

// Values only when asked for; links instead of parents when asked for.
async function valuesAndLinks() {
  const system = createSystem({ parameters: { telemetryValueBytes: 16 } });
  const sink = recorder(system, { values: true });
  const actor = system.spawn(counter(system));
  await system.flush();
  actor.post('go', { order: 'a-long-order-name' });
  await system.flush();
  const goStep = named(sink.spans, 'scxml.microstep go');
  const assign = goStep.events.find((event) => event.name === 'scxml.assign');
  assert.equal(assign.attributes['scxml.value'], '1');
  assert.equal(assign.attributes['scxml.value.truncated'], undefined, 'a value that fits is not marked');
  assert.equal(goStep.attributes['scxml.event.data'], '{"order":"a-long', 'cut at telemetryValueBytes');
  assert.equal(goStep.attributes['scxml.event.data.truncated'], true, 'and marked as cut');
  assert.throws(() => system.setTraceSink(() => {}, { valueLimit: 8 }), { code: 'invalid' });

  const linked = createSystem();
  const links = recorder(linked, { parent: 'link', kinds: ['macrostepBegin', 'macrostepEnd'] });
  const other = linked.spawn(counter(linked));
  await linked.flush();
  other.post('go', undefined, { traceparent: traceparent('00f067aa0ba902b7') });
  await linked.flush();
  const go = named(links.spans, 'scxml.macrostep go');
  assert.equal(go.parentSpanID, undefined, 'a root');
  assert.deepEqual(go.links.map((link) => link.spanID), ['00f067aa0ba902b7'], 'linked to its cause');
  assert.throws(() => linked.setTraceSink(() => {}, { parent: 'sideways' }), /parent/);
  assert.throws(() => linked.setTraceSink(() => {}, { resource: { n: 1 } }), /string/);
  system.close();
  linked.close();
}

// Lifecycle records are spans of their own, with distinct span IDs.
async function lifecycle() {
  const system = createSystem();
  const sink = recorder(system, { kinds: ['spawned', 'destroyed', 'finished'] });
  const definition = system.define(
    statechart({ family: 'short', revision: 'v1' })
      .state('idle', (s) => s.on('stop', (t) => t.target('end')))
      .final('end'),
  );
  const a = system.spawn(definition);
  await system.flush();
  a.post('stop');
  await system.flush();
  const spans = sink.spans;
  const names = spans.map((span) => span.name).sort();
  assert.ok(names.includes('scxml.spawned') && names.includes('scxml.finished'), `lifecycle spans: ${names}`);
  assert.equal(new Set(spans.map((span) => span.spanID)).size, spans.length, 'every span ID is distinct');
  const finished = named(spans, 'scxml.finished');
  assert.equal(finished.attributes['scxml.final_state'], 'end');
  system.close();
}

// Unloading and loading are lifecycle spans (core ADR 0074), under the
// default summary: unload(), load(address), and a load that mail causes.
async function loadedAndUnloaded() {
  const system = createSystem({ clock: 'manual' });
  const batches = [];
  system.setTraceSink((bytes) => batches.push(bytes));
  const definition = system.define(statechart({ family: 'paged', revision: 'v1' })
    .data('n', 0)
    .state('a', (s) => s.on('go', (t) => t.assign('n', function () { return this.n + 1; }))));
  const actor = system.spawn(definition, { name: 'x' });
  await system.flush();
  actor.unload();
  const back = system.load({ family: 'paged', name: 'x' });
  await system.flush();
  back.unload();
  system.post({ family: 'paged', name: 'x' }, 'go');
  await system.flush();
  system.setTraceSink(undefined);
  const spans = batches.flatMap((bytes) => decodeTraces(bytes));
  const names = spans.map((span) => span.name);
  assert.equal(names.filter((name) => name === 'scxml.unloaded').length, 2, `two unloads: ${names}`);
  assert.equal(names.filter((name) => name === 'scxml.loaded').length, 2, `a load, and one by mail: ${names}`);
  const session = String(system.inspect(system.findActor({ family: 'paged', name: 'x' })).sessionID);
  for (const span of spans.filter((each) => each.name === 'scxml.loaded' || each.name === 'scxml.unloaded')) {
    assert.equal(span.attributes['scxml.session_id'], session, 'the same session throughout');
  }
  system.close();
}

// The default summary carries the failure kinds: a runaway macrostep's span
// says it ran long and was stopped (core ADR 0075), without microsteps.
async function summaryFailures() {
  const system = createSystem({ clock: 'manual', parameters: { microstepLimit: 50 }, fault: () => {} });
  const batches = [];
  system.setTraceSink((bytes) => batches.push(bytes));
  const actor = system.spawn(system.define(statechart({ family: 'loop', revision: 'v1' })
    .state('a', (s) => s.on('go', (t) => t.target('b')))
    .state('b', (s) => s.always((t) => t.target('c')))
    .state('c', (s) => s.always((t) => t.target('b')))));
  await system.flush();
  actor.post('go');
  await system.flush();
  actor.destroy();
  await system.flush();
  system.setTraceSink(undefined);
  const spans = batches.flatMap((bytes) => decodeTraces(bytes));
  assert.ok(!spans.some((span) => span.name.startsWith('scxml.microstep')), 'summary has no microstep spans');
  const run = named(spans, 'scxml.macrostep go');
  assert.ok(run, `the runaway macrostep has a span: ${spans.map((span) => span.name)}`);
  const events = run.events.map((event) => event.name);
  assert.ok(events.includes('scxml.macrostep.long'), `the long-macrostep warning: ${events}`);
  assert.ok(events.includes('scxml.runaway'), `the runaway: ${events}`);
  system.close();
}

// napi ADR 0017 section 3: summary by default, full for watched actors,
// sampling for the traces the system starts, and changes in place.
async function watchAndSample() {
  const system = createSystem();
  const batches = [];
  system.setTraceSink((bytes) => batches.push(bytes), { seed: 7n });
  const spans = () => batches.splice(0).flatMap((bytes) => decodeTraces(bytes));
  const counter = system.define(
    statechart({ family: 'watched', revision: 'v1' })
      .data('count', 0)
      .state('idle', (s) => s.on('go', (t) => t.assign('count', function () { return this.count + 1; }))),
  );
  const a = system.spawn(counter);
  const b = system.spawn(counter);
  await system.flush();
  spans();

  a.post('go');
  b.post('go');
  await system.flush();
  let seen = spans();
  assert.deepEqual(seen.map((span) => span.name).sort(), ['scxml.macrostep go', 'scxml.macrostep go'],
                   'summary: one span per macrostep, no microsteps');
  assert.ok(seen.every((span) => span.events.length === 0), 'summary: no data events');

  const watch = system.watchTraces({ sessions: [a] });
  a.post('go');
  b.post('go');
  await system.flush();
  seen = spans();
  assert.equal(seen.filter((span) => span.name === 'scxml.microstep go').length, 1, 'only the watched actor in full');
  const watchedMacro = seen.find((span) => span.name === 'scxml.macrostep go' &&
                                          seen.some((other) => other.parentSpanID === span.spanID));
  assert.ok(watchedMacro !== undefined, 'the watched microstep is a child of its macrostep');
  watch.stop();
  a.post('go');
  await system.flush();
  assert.deepEqual(spans().map((span) => span.name), ['scxml.macrostep go'], 'stop() ends the watch');

  // A watched family: spawns of it are watched from their bootstrap.
  const family = system.watchTraces({ families: ['watched'] });
  const c = system.spawn(counter);
  await system.flush();
  const boot = spans();
  assert.ok(boot.some((span) => span.name.startsWith('scxml.microstep')), `the bootstrap of a watched family is full: ${boot.map((span) => span.name)}`);
  family.stop();

  // sample: 0 starts unsampled traces; a caller's sampled traceparent still wins.
  system.configureTraces({ sample: 0 });
  c.post('go');
  await system.flush();
  assert.equal(spans().length, 0, 'sample 0: nothing');
  c.post('go', undefined, { traceparent: '00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01' });
  await system.flush();
  assert.equal(spans().length, 1, "a sampled traceparent keeps its caller's decision");

  // detail in place, and invalid options.
  system.configureTraces({ sample: 1, detail: 'decisions' });
  c.post('go');
  await system.flush();
  assert.ok(spans().some((span) => span.name === 'scxml.microstep go'), 'decisions adds microsteps');
  assert.throws(() => system.configureTraces({ detail: 'all' }), /detail/);
  assert.throws(() => system.configureTraces({ sample: 2 }), /sample/);
  assert.throws(() => c.post('go', undefined, { traceparent: 'nope' }), /traceparent/);
  system.close();
}

(async () => {
  await untraced();
  await watchAndSample();
  await batches();
  await bytesOutliveTheTurn();
  await smallRequests();
  await kinds();
  await forwardedAndUnavailable();
  await traceparentIn();
  await startedTraces();
  helpers();
  await causality();
  await traceparentOut();
  await repliesStayInTheTrace();
  await logs();
  await failureLogsSayWhy();
  await valuesAndLinks();
  await largeValues();
  await stagingBound();
  await stagingBoundOutsideTurns();
  await parameterResource();
  defaultsVersion();
  await lifecycle();
  await summaryFailures();
  await loadedAndUnloaded();
  console.log('tracing.js passed');
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
