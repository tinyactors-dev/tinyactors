'use strict';
// Metrics (docs/adr/0007 in the repository root): core's per-chart counts
// and the system's stats as plain data (system.metrics()) and as OTLP
// protobuf (metricsToOTLP, the exporter's /v1/metrics), one C writer.
const test = require('node:test');
const assert = require('node:assert/strict');
const http = require('node:http');
const t = require('./binding');
const { scxml, createActor, createSystem, metricsToOTLP } = t;

const settle = () => new Promise((r) => setTimeout(r, 20));

const h = scxml.chart();
const counter = h.scxml('counter', { initial: 'idle', datamodel: 'ecmascript' },
  h.datamodel({ n: 0 }),
  h.state('idle', h.transition('add', undefined, h.assign('n', 'n + 1')), h.transition('stop', 'done')),
  h.final('done'));

const point = (snapshot, name, attributes = {}) => {
  const metric = snapshot.metrics.find((m) => m.name === name);
  assert.ok(metric, `${name} is there`);
  return metric.points.find((p) => Object.entries(attributes).every(([k, v]) => p.attributes[k] === v));
};

async function receiver() {
  const requests = [];
  const server = http.createServer((request, response) => {
    const chunks = [];
    request.on('data', (chunk) => chunks.push(chunk));
    request.on('end', () => {
      requests.push({ url: request.url, headers: request.headers, body: Buffer.concat(chunks) });
      response.end();
    });
  });
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  return { requests, endpoint: `http://127.0.0.1:${server.address().port}`, close: () => new Promise((r) => server.close(r)) };
}

test('metrics(): per chart its actors, macrosteps, fuel and ends; the system its pools', async () => {
  const system = createSystem();
  const a = createActor(counter, { system }).start();
  const b = createActor(counter, { system }).start();
  for (let i = 0; i < 3; i++) a.send({ type: 'add' });
  b.send({ type: 'stop' });
  await settle();
  const snapshot = system.metrics();
  assert.ok(snapshot.startTime <= snapshot.time);
  const chart = { 'tinyactors.chart.name': 'counter' };
  assert.equal(point(snapshot, 'tinyactors.actors', { ...chart, 'tinyactors.actor.state': 'resident' }).value, 1);
  assert.equal(point(snapshot, 'tinyactors.actors.spawned', chart).value, 2);
  assert.equal(point(snapshot, 'tinyactors.actors.ended', chart).value, 1);
  assert.equal(point(snapshot, 'tinyactors.macrosteps', chart).value, 6); /* two bootstraps, four events */
  assert.ok(point(snapshot, 'tinyactors.fuel', chart).value > 0);
  assert.equal(point(snapshot, 'tinyactors.errors', { ...chart, 'scxml.error.type': 'execution' }).value, 0);
  assert.ok(point(snapshot, 'tinyactors.pool.limit', { 'tinyactors.pool': 'events' }).value > 0);
  assert.ok(point(snapshot, 'tinyactors.events.delivered').value >= 4);
  assert.equal(snapshot.metrics.find((m) => m.name === 'tinyactors.macrosteps').kind, 'sum');
  const runs = point(snapshot, 'tinyactors.run.duration');
  assert.ok(runs.count >= 1 && runs.buckets.length === runs.bounds.length + 1);
  assert.equal(runs.buckets.reduce((x, y) => x + y, 0), runs.count);
  system.stop();
  assert.throws(() => system.metrics(), /stopped/);
});

test('metricsToOTLP: an application snapshot, written by the C writer', () => {
  const snapshot = {
    startTime: 1000,
    time: 2000,
    metrics: [
      { name: 'ops_backup_age_seconds', unit: '', kind: 'gauge', points: [{ attributes: { plan: 'nightly' }, value: 12.5 }] },
      { name: 'ops.uploads', unit: '{upload}', kind: 'sum', points: [{ attributes: {}, value: 3 }] },
      {
        name: 'ops.latency',
        unit: 's',
        kind: 'histogram',
        points: [{ attributes: {}, count: 2, sum: 0.3, min: 0.1, max: 0.2, bounds: [0.15], buckets: [1, 1], exemplars: [{ traceID: '0af7651916cd43dd8448eb211c80319c', spanID: 'b7ad6b7169203331', time: 1500, value: 0.2 }] }],
      },
    ],
  };
  const bytes = metricsToOTLP(snapshot, { resource: { 'service.name': 'granary-ops' } });
  assert.ok(bytes instanceof Uint8Array && bytes.length > 0);
  const text = Buffer.from(bytes);
  for (const name of ['ops_backup_age_seconds', 'ops.uploads', 'ops.latency', 'granary-ops', 'tinyactors']) assert.ok(text.includes(name), name);
  assert.ok(text.includes(Buffer.from('0af7651916cd43dd8448eb211c80319c', 'hex')), 'the exemplar\'s trace ID');
  assert.throws(() => metricsToOTLP({}), TypeError);
  assert.throws(() => metricsToOTLP({ metrics: [{ name: 'x', kind: 'nope', points: [] }] }), TypeError);
  /* the same snapshot, the same bytes */
  assert.deepEqual(metricsToOTLP(snapshot, { resource: { 'service.name': 'granary-ops' } }), bytes);
});

test('the exporter posts /v1/metrics at its interval and when the system stops', async () => {
  const r = await receiver();
  const system = createSystem({
    telemetry: { endpoint: r.endpoint, metricsInterval: 30, resource: { 'service.name': 'metered' }, metrics: () => [{ name: 'app.items', unit: '{item}', kind: 'gauge', points: [{ attributes: {}, value: 7 }] }] },
  });
  createActor(counter, { system }).start().send({ type: 'add' });
  const t0 = Date.now();
  while (!r.requests.some((q) => q.url === '/v1/metrics') && Date.now() - t0 < 3000) await new Promise((res) => setTimeout(res, 10));
  system.stop();
  await system.flushTraces();
  await r.close();
  const posted = r.requests.filter((q) => q.url === '/v1/metrics');
  assert.ok(posted.length >= 2, 'one at an interval, one at stop');
  assert.equal(posted[0].headers['content-type'], 'application/x-protobuf');
  for (const name of ['tinyactors.macrosteps', 'tinyactors.run.duration', 'tinyactors.telemetry.bytes', 'app.items', 'metered', 'counter'])
    assert.ok(posted[posted.length - 1].body.includes(name), name);
});

test('a metrics endpoint alone exports metrics and records no traces', async () => {
  const r = await receiver();
  const system = createSystem({ telemetry: { metricsEndpoint: `${r.endpoint}/m`, metricsInterval: 0 } });
  createActor(counter, { system }).start().send({ type: 'add' });
  await settle();
  system.stop();
  await system.flushTraces();
  await r.close();
  assert.deepEqual(
    r.requests.map((q) => q.url),
    ['/m'],
  );
});

test('spanMetrics: macrostep durations per chart, with exemplars that are spans', async () => {
  const traces = [];
  const system = createSystem({ telemetry: { spanMetrics: true }, traces: { sink: (b) => traces.push(b) } });
  const a = createActor(counter, { system }).start();
  for (let i = 0; i < 3; i++) a.send({ type: 'add' });
  await settle();
  const durations = point(system.metrics(), 'tinyactors.macrostep.duration', { 'tinyactors.chart.name': 'counter' });
  assert.equal(durations.count, 4);
  assert.ok(durations.exemplars.length >= 1);
  const spans = traces.flatMap((b) => t.decodeTraces(b));
  assert.ok(spans.some((s) => s.spanID === durations.exemplars[0].spanID), 'the exemplar is a span');
  system.stop();
  const plain = createSystem();
  assert.ok(!plain.metrics().metrics.some((m) => m.name === 'tinyactors.macrostep.duration'), 'off by default');
  plain.stop();
});

test('Node.js: OTEL_* metrics variables', { skip: process.env.TA_BINDING === 'web' && 'the web build reads no environment' }, () => {
  const { telemetryFromEnvironment } = require('../telemetry');
  assert.deepEqual(telemetryFromEnvironment({ OTEL_EXPORTER_OTLP_METRICS_ENDPOINT: 'http://x/m', OTEL_METRIC_EXPORT_INTERVAL: '5000' }), {
    metricsEndpoint: 'http://x/m',
    metricsInterval: 5000,
  });
  assert.deepEqual(telemetryFromEnvironment({ OTEL_EXPORTER_OTLP_ENDPOINT: 'http://x', OTEL_METRICS_EXPORTER: 'none' }), {
    endpoint: 'http://x',
    metricsEndpoint: '',
  });
});
