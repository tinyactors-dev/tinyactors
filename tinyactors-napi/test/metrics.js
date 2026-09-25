'use strict';

// Metrics as plain data (napi ADR 0017 section 7, core ADR 0072): the
// snapshot system.metrics() returns, and its OTLP bytes. Runs on Node and Bun.

const assert = require('node:assert/strict');
const { createSystem, statechart, metricsToOTLP, TinyactorsError } = require('..');

const point = (snapshot, name, match = {}) => {
  const metric = snapshot.metrics.find((entry) => entry.name === name);
  assert.ok(metric !== undefined, `metric ${name}`);
  return metric.points.find((p) => Object.entries(match).every(([key, value]) => p.attributes[key] === value));
};

// A minimal protobuf walk: every length-delimited field is nested well, and
// the strings it holds are collected.
function strings(bytes, out = []) {
  let at = 0;
  const varint = () => {
    let value = 0;
    let scale = 1;
    for (;;) {
      const byte = bytes[at++];
      assert.ok(byte !== undefined, 'a varint runs past its message');
      value += (byte & 0x7f) * scale;
      scale *= 128;
      if ((byte & 0x80) === 0) return value;
    }
  };
  while (at < bytes.length) {
    const wire = varint() & 7;
    if (wire === 0) varint();
    else if (wire === 1) at += 8;
    else if (wire === 2) {
      const length = varint();
      const body = bytes.subarray(at, at + length);
      assert.equal(body.length, length, 'a field runs past its message');
      at += length;
      const text = Buffer.from(body).toString('utf8');
      if (/^[\x20-\x7e]+$/.test(text)) out.push(text);
      try {
        strings(body, out);
      } catch {
        // not a message: a string or bytes
      }
    } else if (wire === 5) at += 4;
    else throw new Error(`wire type ${wire}`);
  }
  return out;
}

async function snapshot() {
  const system = createSystem();
  const definition = system.define(
    statechart({ family: 'orders', revision: 'v2' })
      .state('open', (s) => s.on('close', (t) => t.target('closed')))
      .state('closed'),
  );
  const a = system.spawn(definition);
  const b = system.spawn(definition);
  system.spawn(definition);
  await system.flush();
  a.post('close');
  b.post('unknown');
  await system.flush();

  const metrics = system.metrics();
  const family = { 'tinyactors.definition.family': 'orders', 'tinyactors.definition.revision': 'v2' };
  assert.equal(point(metrics, 'tinyactors.actors.spawned', family).value, 3);
  assert.equal(point(metrics, 'tinyactors.events.taken', family).value, 2);
  assert.equal(point(metrics, 'tinyactors.events.ignored', { ...family, 'scxml.event.type': 'external' }).value, 1);
  assert.equal(point(metrics, 'tinyactors.actors.occupancy', { ...family, 'scxml.state': 'open' }).value, 2);
  assert.equal(point(metrics, 'tinyactors.actors.occupancy', { ...family, 'scxml.state': 'closed' }).value, 1);
  assert.equal(metrics.metrics.find((m) => m.name === 'tinyactors.actors.occupancy').kind, 'gauge');
  assert.equal(metrics.metrics.find((m) => m.name === 'tinyactors.macrosteps').kind, 'sum');
  assert.ok(point(metrics, 'tinyactors.messages.delivered').value >= 2);
  assert.ok(point(metrics, 'tinyactors.memory.usage', { 'tinyactors.memory.category': 'core' }).value > 0);
  for (const metric of metrics.metrics) {
    for (const p of metric.points) {
      assert.ok(!('scxml.session_id' in p.attributes), 'no metric names one actor');
    }
  }
  assert.equal(metrics.system.spawned, 3);

  a.destroy();
  const after = system.metrics();
  assert.equal(point(after, 'tinyactors.actors.occupancy', { ...family, 'scxml.state': 'closed' }), undefined,
               'a destroyed actor leaves the occupancy');

  const bytes = metricsToOTLP(after, { resource: { 'service.name': 'orders-service' } });
  assert.ok(bytes instanceof Uint8Array && bytes.length > 0);
  const text = strings(bytes);
  for (const expected of ['service.name', 'orders-service', 'tinyactors', 'tinyactors.actors.occupancy',
                          'tinyactors.definition.family', 'orders', '{actor}']) {
    assert.ok(text.includes(expected), `the OTLP bytes carry ${expected}`);
  }
  assert.throws(() => metricsToOTLP({}), (error) => error instanceof TinyactorsError && error.code === 'invalid');
  system.close();
}

(async () => {
  await snapshot();
  console.log('metrics.js passed');
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
