// The addon's memory (AGENTS.md, "The binding's memory"; ADR 0010): what
// the suite cannot see. Nested calls share nothing, so JavaScript a call
// runs (a getter, a toJSON, a resolve) may call the Host again.
import assert from 'node:assert/strict';
import { test, run } from './harness.mjs';
import { Host, SimulatedClock, compile, encode, decode, TinyactorsError, ERROR_STATE } from '@tinyactors/node';

const ECHO = `<scxml xmlns="http://www.w3.org/2005/07/scxml" datamodel="ecmascript" name="echo" initial="s">
  <state id="s"><transition event="*"><log label="got" expr="_event.name + ' ' + JSON.stringify(_event.data)"/></transition></state>
</scxml>`;

const DATAMODELS = ['native', 'quickjs'];
let datamodel = DATAMODELS[0];

function host(on = {}) {
  const made = new Host({ config: { systemID: 'h', system: { ticksPerSecond: 1000 } }, datamodel, clock: new SimulatedClock(), on });
  made.addImage(compile([{ name: 'echo.scxml', text: ECHO }]));
  return made;
}

test('a getter of send()\'s data may send: both are taken', () => {
  const logs = [];
  const h = host({ log: (record) => logs.push(record.text) });
  const session = h.spawn('echo');
  h.send(session, 'outer', { get v() { h.send(session, 'inner', { w: 2 }); return 1; } });
  assert.deepEqual(logs.sort(), ['inner {"w":2}', 'outer {"v":1}']);
  h.close();
});

test('a getter of sendMany()\'s data may sendMany: both are taken', () => {
  const logs = [];
  const h = host({ log: (record) => logs.push(record.text) });
  const session = h.spawn('echo');
  const taken = h.sendMany([
    { session, event: 'a', data: { get v() { return h.sendMany([{ session, event: 'inner', data: 3 }]); } } },
    { session, event: 'b', data: 2 },
  ]);
  assert.equal(taken, 2);
  assert.deepEqual(logs.sort(), ['a {"v":1}', 'b 2', 'inner 3']);
  h.close();
});

test('sendMany past one batch takes every event, in order', () => {
  const logs = [];
  const h = host({ log: (record) => logs.push(record.text) });
  const session = h.spawn('echo');
  const events = Array.from({ length: 150 }, (_, i) => ({ session, event: 'e', data: i }));
  assert.equal(h.sendMany(events), 150);
  assert.deepEqual(logs, events.map((_, i) => `e ${i}`));
  h.close();
});

test('a malformed item of sendMany sends nothing', () => {
  const logs = [];
  const h = host({ log: (record) => logs.push(record.text) });
  const session = h.spawn('echo');
  const events = Array.from({ length: 100 }, (_, i) => ({ session, event: 'e', data: i }));
  events[90] = { session, event: 7 };
  assert.throws(() => h.sendMany(events), TinyactorsError);
  assert.deepEqual(logs, []);
  h.close();
});

test('a getter of request()\'s params may request: each gets its own reply', () => {
  const h = host();
  let inner;
  const outer = h.request('actor.list', { get limit() { inner = h.request('host.config'); return 10; } });
  assert.deepEqual(outer, { sessions: [], cursor: null });
  assert.equal(inner.systemID, 'h');
  h.close();
});

test('a toJSON that calls request() during actor.context: core refuses it, the outer reply stands', () => {
  if (datamodel !== 'native') return; // a quickjs chart cannot reach the Host
  const chart = `<scxml xmlns="http://www.w3.org/2005/07/scxml" datamodel="ecmascript" name="c" initial="s">
    <datamodel><data id="v" expr="globalThis.makeValue()"/><data id="w" expr="'after'"/></datamodel>
    <state id="s"/></scxml>`;
  const h = host();
  h.addImage(compile([{ name: 'c.scxml', text: chart }]));
  let inner;
  globalThis.makeValue = () => ({ toJSON() {
    try { h.request('actor.list', { pad: 'x'.repeat(100000) }); inner = 'answered'; } catch (error) { inner = error; }
    return { inner: true };
  } });
  const session = h.spawn('c');
  const context = h.request('actor.context', { session });
  delete globalThis.makeValue;
  assert.deepEqual(context, { variables: { v: { inner: true }, w: 'after' } });
  assert.ok(inner instanceof TinyactorsError && inner.status === ERROR_STATE, String(inner));
  h.close();
});

test('a toJSON that encodes, inside encode', () => {
  const value = { a: { toJSON() { return decode(encode({ b: [1, 2] })); } }, c: 'x'.repeat(5000) };
  assert.deepEqual(decode(encode(value)), { a: { b: [1, 2] }, c: 'x'.repeat(5000) });
});

test('a resolve that compiles, inside compile', () => {
  const main = `<scxml xmlns="http://www.w3.org/2005/07/scxml" datamodel="ecmascript" name="main" initial="s">
    <datamodel><data id="d" src="part.json"/></datamodel>
    <state id="s"><onentry><log label="d" expr="JSON.stringify(d)"/></onentry></state></scxml>`;
  let nested;
  const image = compile([{ name: 'main.scxml', text: main }], {
    resolve: ({ source }) => {
      nested = compile([{ name: 'echo.scxml', text: ECHO }]);
      return source === 'part.json' ? { name: 'part.json', text: '{"a": 1}' } : undefined;
    },
  });
  assert.ok(nested instanceof Uint8Array && nested.length > 0);
  const logs = [];
  const h = host({ log: (record) => logs.push(record.text) });
  h.addImage(image);
  h.spawn('main');
  assert.deepEqual(logs, ['{"a":1}']);
  h.close();
});

for (const each of DATAMODELS) {
  datamodel = each;
  console.log(`-- ${each}`);
  await run();
}
