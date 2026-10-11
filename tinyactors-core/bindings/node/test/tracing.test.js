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
const { scxml, createActor, createSystem, createLink, serveLinks, decodeTraces } = t;
const h = scxml.chart();

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

const pong = h.scxml('pong', { initial: 'idle', datamodel: 'ecmascript' },
  h.datamodel({ n: 0 }),
  h.state('idle', h.transition('ping', undefined, h.assign('n', h.expr(({ n }) => n + 1)))));
const ping = h.scxml('ping', { initial: 'idle', datamodel: 'ecmascript' },
  h.datamodel({ peer: null }),
  h.state('idle', h.transition('go', undefined, h.send('ping', { targetexpr: 'String(peer)' }))));

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

test('watchTraces: one actor in full, whatever the ratio, and what it causes; unwatchTraces(id) ends it', async () => {
  const c = collector();
  const system = createSystem({ traces: { sink: c.sink, sample: 0, detail: 'summary' } });
  const b = createActor(pong, { system }).start();
  const a = createActor(ping, { system, input: { peer: b } }).start();
  const other = createActor(pong, { system }).start();
  await settle();
  const watch = system.watchTraces({ actors: [a] });
  assert.equal(typeof watch, 'number');
  a.send({ type: 'go' });
  other.send({ type: 'ping' });
  await settle();
  const spans = c.spans();
  const go = spans.find((s) => s.name === 'scxml.macrostep go');
  assert.ok(go, 'the watched actor is traced although nothing is sampled');
  assert.ok(spans.some((s) => s.name.startsWith('scxml.microstep') && s.traceID === go.traceID), 'in full: its microsteps');
  const caused = spans.filter((s) => s.name === 'scxml.macrostep ping');
  assert.equal(caused.length, 1, 'what it caused is traced, the other actor is not');
  assert.equal(caused[0].traceID, go.traceID);
  system.unwatchTraces(watch);
  system.unwatchTraces(watch); /* once is enough */
  const before = c.spans().length;
  a.send({ type: 'go' });
  await settle();
  assert.ok(!c.spans().slice(before).some((s) => s.name.startsWith('scxml.macrostep')), 'after unwatchTraces(): sampled as before');
  system.stop();
});

test('watchTraces by chart, with values, ends by itself', async () => {
  const c = collector();
  const system = createSystem({ traces: { sink: c.sink, sample: 0 } });
  const b = createActor(pong, { system }).start();
  await settle();
  system.watchTraces({ charts: ['pong'], values: true, for: 50 });
  b.send({ type: 'ping', n: 1 });
  await settle();
  const pinged = c.spans().find((s) => s.name === 'scxml.macrostep ping');
  assert.ok(pinged);
  assert.match(String(pinged.attributes['scxml.event.data']), /"n":1/, 'values for the watched class');
  await new Promise((r) => setTimeout(r, 120));
  const before = c.spans().length;
  b.send({ type: 'ping' });
  await settle();
  assert.ok(!c.spans().slice(before).some((s) => s.name.startsWith('scxml.macrostep')), 'the watch ended');
  assert.throws(() => system.watchTraces({}), TypeError);
  system.stop();
});

test('rules: a chart never traced, another at a ratio; malformed rules change nothing', async () => {
  const c = collector();
  const system = createSystem({ traces: { sink: c.sink } });
  system.configureTraces({ rules: [{ chart: 'pong', at: 'take', detail: 'off' }] });
  const b = createActor(pong, { system }).start();
  const a = createActor(ping, { system, input: { peer: b } }).start();
  a.send({ type: 'go' });
  await settle();
  const spans = c.spans();
  assert.ok(spans.some((s) => s.name === 'scxml.macrostep go'));
  assert.ok(!spans.some((s) => s.name === 'scxml.macrostep ping'), 'pong writes nothing');
  assert.throws(() => system.configureTraces({ rules: [{ from: 'nowhere' }] }), TypeError);
  assert.throws(() => system.configureTraces({ rules: [{ detail: 'loud' }] }), TypeError);
  assert.throws(() => system.configureTraces({ rules: Array.from({ length: 70 }, () => ({ sample: true })) }), /more rules/);
  const before = c.spans().length;
  a.send({ type: 'go' });
  await settle();
  assert.ok(!c.spans().slice(before).some((s) => s.name === 'scxml.macrostep ping'), 'the earlier rules still hold');
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
  const slow = h.scxml('slow', { initial: 'a', datamodel: 'ecmascript' },
    h.state('a', h.onentry(h.send('later', { delay: '100s' })), h.transition('later', 'b')),
    h.state('b'));
  createActor(slow, { system }).start();
  await settle();
  system.stop();
  assert.ok(c.spans().every((s) => s.endTimeUnixNano >= s.startTimeUnixNano));
});

test('a trace crosses a link: the envelope carries the traceparent', async () => {
  const c = collector();
  const server = createSystem({ systemID: 'trace-server', traces: { sink: c.sink } });
  createActor(pong, { system: server, systemID: 'pong' }).start();
  const served = serveLinks(server, { port: 0, exports: ['pong'] });
  await until(() => served.port);
  const client = createSystem({ systemID: 'trace-client', telemetry: false });
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
  const system = createSystem({ systemID: 'trace-host', traces: { sink: c.sink } });
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
    const { scxml, createActor, createSystem } = require(${JSON.stringify(path.join(__dirname, '..'))});
    const c = scxml.chart();
    const m = c.scxml('env', { initial: 'a' }, c.state('a', c.transition('go', 'b')), c.state('b'));
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

test('only the Node.js entry reads the environment (telemetry.js): not the API, the addon or the web binding', () => {
  const root = path.join(__dirname, '../..');
  for (const file of ['node/core.mjs', 'node/src/system.c', 'node/src/trace.c', 'node/src/metrics.c', 'web/core.mjs', 'web/native.mjs', 'web/index.mjs', 'web/node.mjs']) {
    const text = fs.readFileSync(path.join(root, file), 'utf8');
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
