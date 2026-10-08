'use strict';
// Tracing (docs/adr/0006 in the repository root): one trace across actors,
// systems and links; a sink and an OTLP/HTTP exporter get core's records as
// OTLP protobuf; the trace values are C's (stepSpanID, parseTraceparent,
// formatTraceparent, decodeTraces).
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const t = require('./binding');
const { createMachine, createActor, createSystem, createLink, serveLinks, sendTo, assign, decodeTraces } = t;

const until = async (cond, ms = 3000) => {
  const t0 = Date.now();
  while (!cond()) {
    if (Date.now() - t0 > ms) throw new Error('timed out');
    await new Promise((r) => setTimeout(r, 5));
  }
};
const settle = () => new Promise((r) => setTimeout(r, 20));

const PARENT = '00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01';
const TRACE = '0af7651916cd43dd8448eb211c80319c';

/* the spans a sink got */
function collector() {
  const batches = [];
  const logs = [];
  return {
    sink: (traces, l) => {
      batches.push(traces);
      if (l) logs.push(l);
    },
    spans: () => batches.flatMap((b) => decodeTraces(b)),
    logs,
  };
}

const pong = createMachine({
  id: 'pong',
  initial: 'idle',
  context: { n: 0 },
  states: { idle: { on: { ping: { actions: assign({ n: ({ context }) => context.n + 1 }) } } } },
});
const ping = createMachine({
  id: 'ping',
  initial: 'idle',
  context: ({ input }) => ({ peer: input.peer }),
  states: { idle: { on: { go: { actions: sendTo(({ context }) => context.peer, { type: 'ping' }) } } } },
});

test('one trace: a traceparent from the host runs through every step it causes', async () => {
  const c = collector();
  const system = createSystem({ traces: { sink: c.sink, seed: 7n } });
  const b = createActor(pong, { system }).start();
  const a = createActor(ping, { system, input: { peer: b } }).start();
  a.send({ type: 'go' }, { traceparent: PARENT });
  await settle();
  system.stop();
  const spans = c.spans();
  const go = spans.find((s) => s.name === 'scxml.macrostep go');
  assert.ok(go, 'the macrostep span of go');
  assert.equal(go.traceID, TRACE);
  assert.equal(go.parentSpanID, 'b7ad6b7169203331', "the host's span is its parent");
  const pinged = spans.find((s) => s.name === 'scxml.macrostep ping');
  assert.equal(pinged.traceID, TRACE, 'the event it sent continues the trace');
  const sentFrom = spans.find((s) => s.parentSpanID === go.spanID && s.name.startsWith('scxml.microstep'));
  assert.ok(sentFrom, 'a microstep of go');
  assert.equal(pinged.parentSpanID, sentFrom.spanID, 'the step that sent it is its parent');
  // span IDs are the step keys' (C's mix)
  const session = go.attributes['scxml.session_id'];
  assert.equal(t.stepSpanID(session, go.attributes['scxml.macrostep']), go.spanID);
  for (const s of spans) assert.ok(s.endTimeUnixNano > s.startTimeUnixNano, `${s.name} has a duration`);
});

test('detail, kinds and sampling change while traces flow; lifecycle spans stay', async () => {
  const c = collector();
  const system = createSystem({ traces: { sink: c.sink, detail: 'summary' } });
  const b = createActor(pong, { system }).start();
  b.send({ type: 'ping' });
  await settle();
  assert.ok(c.spans().some((s) => s.name === 'scxml.macrostep ping'));
  assert.ok(!c.spans().some((s) => s.name.startsWith('scxml.microstep')), 'summary: no microsteps');
  system.configureTraces({ detail: 'full', sample: 0 });
  const before = c.spans().length;
  b.send({ type: 'ping' });
  createActor(pong, { system }).start();
  await settle();
  const later = c.spans().slice(before);
  assert.ok(!later.some((s) => s.name.startsWith('scxml.macrostep')), 'sample 0: no step spans');
  assert.ok(later.some((s) => s.name === 'scxml.spawned'), 'lifecycle spans whatever the sampling');
  system.configureTraces({ kinds: ['macrostepBegin', 'macrostepEnd'], sample: 1 });
  b.send({ type: 'ping' });
  await settle();
  assert.ok(c.spans().some((s) => s.name === 'scxml.macrostep ping' && s.startTimeUnixNano > later.at(-1)?.startTimeUnixNano));
  assert.throws(() => system.configureTraces({ kinds: ['nonsense'] }), TypeError);
  system.stop();
});

test('without a sink or an exporter no record is written, and traceparents still flow', async () => {
  const system = createSystem({ telemetry: false });
  const b = createActor(pong, { system }).start();
  b.send({ type: 'ping' }, { traceparent: PARENT });
  await settle();
  const c = collector();
  system.setTraceSink(c.sink);
  b.send({ type: 'ping' }, { traceparent: PARENT });
  await settle();
  system.stop();
  const spans = c.spans().filter((s) => s.name === 'scxml.macrostep ping');
  assert.equal(spans.length, 1, 'only what came after the sink');
  assert.equal(spans[0].traceID, TRACE);
});

test('a span still open when the system stops ends, marked incomplete', async () => {
  const c = collector();
  const system = createSystem({ traces: { sink: c.sink } });
  const slow = createMachine({ id: 'slow', initial: 'a', states: { a: { after: { 100000: 'b' } }, b: {} } });
  createActor(slow, { system }).start();
  await settle();
  system.stop();
  assert.ok(c.spans().every((s) => s.endTimeUnixNano >= s.startTimeUnixNano));
});

test('a trace crosses a link: the envelope carries the traceparent', async () => {
  const c = collector();
  const server = createSystem({ id: 'trace-server', traces: { sink: c.sink } });
  createActor(pong, { system: server, systemID: 'pong' }).start();
  const served = serveLinks(server, { port: 0, exports: ['pong'] });
  await until(() => served.port);
  const client = createSystem({ id: 'trace-client', telemetry: false });
  const link = createLink(client, { url: `ws://127.0.0.1:${served.port}/link`, name: 'trace-server' });
  const a = createActor(ping, { system: client, input: { peer: link.get('pong') } }).start();
  a.send({ type: 'go' }, { traceparent: PARENT });
  await until(() => c.spans().some((s) => s.name === 'scxml.macrostep ping'));
  const pinged = c.spans().find((s) => s.name === 'scxml.macrostep ping');
  assert.equal(pinged.traceID, TRACE, 'the step on the other system continues the trace');
  client.stop();
  await served.close();
  server.stop();
});

test('host sends: deliver(), ActorRef.send() and sendMany() take a traceparent', async () => {
  const c = collector();
  const system = createSystem({ id: 'trace-host', traces: { sink: c.sink } });
  const b = createActor(pong, { system, systemID: 'pong' }).start();
  const traces = ['1', '2', '3'].map((d) => `00-${d.repeat(32)}-${'a'.repeat(16)}-01`);
  system.deliver({ target: 'tinyactors://trace-host/@pong', name: 'ping', traceparent: traces[0] });
  system.ref('tinyactors://trace-host/@pong').send({ type: 'ping' }, { traceparent: traces[1] });
  system.sendMany([[b, { type: 'ping' }, { traceparent: traces[2] }]]);
  await settle();
  system.stop();
  const seen = new Set(c.spans().filter((s) => s.name === 'scxml.macrostep ping').map((s) => s.traceID));
  for (const d of ['1', '2', '3']) assert.ok(seen.has(d.repeat(32)), `trace ${d}`);
});

/* an OTLP/HTTP receiver on port 0 */
async function receiver() {
  const requests = [];
  const server = http.createServer((request, response) => {
    const chunks = [];
    request.on('data', (chunk) => chunks.push(chunk));
    request.on('end', () => {
      requests.push({ url: request.url, headers: request.headers, body: new Uint8Array(Buffer.concat(chunks)) });
      response.end();
    });
  });
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  return { requests, endpoint: `http://127.0.0.1:${server.address().port}`, close: () => new Promise((r) => server.close(r)) };
}

test('an exporter posts OTLP/HTTP protobuf to the endpoint it is given', async () => {
  const r = await receiver();
  const system = createSystem({ telemetry: { endpoint: r.endpoint, headers: { 'x-token': 'abc' }, resource: { 'service.name': 'exported' } } });
  const b = createActor(pong, { system }).start();
  b.send({ type: 'ping' });
  await settle();
  await system.flushTraces();
  system.stop();
  await system.flushTraces();
  await r.close();
  const traces = r.requests.filter((q) => q.url === '/v1/traces');
  assert.ok(traces.length >= 1);
  assert.equal(traces[0].headers['content-type'], 'application/x-protobuf');
  assert.equal(traces[0].headers['x-token'], 'abc');
  const spans = traces.flatMap((q) => decodeTraces(q.body));
  assert.ok(spans.some((s) => s.name === 'scxml.macrostep ping'));
  assert.equal(spans[0].resource.attributes['service.name'], 'exported');
});

test('posting fails quietly', async () => {
  const logged = [];
  const original = console.log;
  console.log = (...args) => logged.push(args.join(' '));
  try {
    const system = createSystem({ telemetry: { endpoint: 'http://127.0.0.1:9' } });
    createActor(pong, { system }).start().send({ type: 'ping' });
    await settle();
    await system.flushTraces();
    system.stop();
    await system.flushTraces();
  } finally {
    console.log = original;
  }
  assert.ok(logged.length <= 1, 'at most one line in ten seconds');
});

test('Node.js: OTEL_* variables export with no code', { skip: process.env.TA_BINDING === 'web' && 'the web build reads no environment' }, async () => {
  const r = await receiver();
  const script = `
    const { createMachine, createActor, createSystem } = require(${JSON.stringify(path.join(__dirname, '..'))});
    const m = createMachine({ id: 'env', initial: 'a', states: { a: { on: { go: 'b' } }, b: {} } });
    const system = createSystem();
    createActor(m, { system }).start().send({ type: 'go' });
    setTimeout(() => { system.stop(); system.flushTraces(); }, 20);`;
  const child = require('node:child_process').spawn(process.execPath, ['-e', script], {
    env: {
      ...process.env,
      OTEL_EXPORTER_OTLP_ENDPOINT: r.endpoint,
      OTEL_EXPORTER_OTLP_HEADERS: 'x-api-key=s%20ecret',
      OTEL_SERVICE_NAME: 'from-env',
      OTEL_RESOURCE_ATTRIBUTES: 'deployment.environment=test',
    },
    stdio: 'inherit',
  });
  await new Promise((resolve) => child.on('exit', resolve));
  await r.close();
  const traces = r.requests.filter((q) => q.url === '/v1/traces');
  assert.ok(traces.length >= 1, 'posted');
  assert.equal(traces[0].headers['x-api-key'], 's ecret');
  const spans = traces.flatMap((q) => decodeTraces(q.body));
  assert.ok(spans.some((s) => s.name === 'scxml.macrostep go'));
  assert.equal(spans[0].resource.attributes['service.name'], 'from-env');
  assert.equal(spans[0].resource.attributes['deployment.environment'], 'test');
});

test('Node.js: OTEL_SDK_DISABLED turns the export off', { skip: process.env.TA_BINDING === 'web' && 'the web build reads no environment' }, () => {
  const { telemetryFromEnvironment } = require('../telemetry');
  assert.deepEqual(telemetryFromEnvironment({ OTEL_SDK_DISABLED: 'true', OTEL_EXPORTER_OTLP_TRACES_ENDPOINT: 'http://x/t', OTEL_TRACES_SAMPLER_ARG: '0.25' }), {
    disabled: true,
    tracesEndpoint: 'http://x/t',
    logsEndpoint: '',
    sample: 0.25,
  });
});

test('nothing in bindings/shared reads the environment', () => {
  const shared = path.join(__dirname, '../../shared');
  for (const file of fs.readdirSync(shared)) {
    const text = fs.readFileSync(path.join(shared, file), 'utf8');
    assert.ok(!/process\.env|\bgetenv\s*\(|Deno\.env|Bun\.env/.test(text), `${file} reads the environment`);
  }
});

test('trace values are C\'s: stepSpanID, parseTraceparent, formatTraceparent', () => {
  for (const k of JSON.parse(fs.readFileSync(path.join(__dirname, '../../../tests/conformance/span_ids.json'), 'utf8')).keys)
    assert.equal(t.stepSpanID(k.session, BigInt(k.macrostep), k.microstep), k.span);
  assert.deepEqual(t.parseTraceparent(PARENT), { traceID: TRACE, parentSpanID: 'b7ad6b7169203331', flags: 1, sampled: true });
  assert.equal(t.parseTraceparent('00-00000000000000000000000000000000-b7ad6b7169203331-01'), undefined);
  assert.equal(t.parseTraceparent(undefined), undefined);
  assert.equal(t.formatTraceparent({ traceID: TRACE, spanID: 'b7ad6b7169203331' }), PARENT);
  assert.equal(t.formatTraceparent({ traceID: TRACE, spanID: 'b7ad6b7169203331', sampled: false }), PARENT.slice(0, -2) + '00');
  assert.throws(() => t.formatTraceparent({ traceID: 'x', spanID: 'y' }), TypeError);
  assert.equal(t.stepSpanID('1', 1n, 0), t.stepSpanID(1, 1));
  assert.ok(t.TRACE_KINDS.includes('macrostepBegin'));
  assert.throws(() => decodeTraces(new Uint8Array([0x0a, 0xff])), /malformed/);
});
