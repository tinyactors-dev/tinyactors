// The Host on quickjs with a simulated clock: what the suite's scenarios
// do through the adapter, seen from an application's test.
import assert from 'node:assert/strict';
import { test, run } from './harness.mjs';
import { Host, SimulatedClock, compile, TinyactorsError, ERROR_INVALID } from '@tinyactors/node';

const COUNTER = `<scxml xmlns="http://www.w3.org/2005/07/scxml" datamodel="ecmascript" name="counter" initial="idle">
  <datamodel><data id="n" expr="0"/></datamodel>
  <state id="idle">
    <transition event="inc"><assign location="n" expr="n + _event.data.by"/><log label="n" expr="n"/></transition>
    <transition event="later"><send event="tick" delay="50ms"/></transition>
    <transition event="tick"><log label="tick" expr="n"/></transition>
    <transition event="stop" target="end"/>
  </state>
  <final id="end"><donedata><param name="total" expr="n"/></donedata></final>
</scxml>`;

// Each case runs on both datamodels: the native one (this package's) and
// quickjs.
const DATAMODELS = ['native', 'quickjs'];
let datamodel = DATAMODELS[0];

function quickjs(clock, on) {
  return new Host({
    config: { systemID: 'h', system: { ticksPerSecond: 1000 } },
    datamodel,
    clock,
    on,
  });
}

test('spawn, send, log and done, each before the input returns', () => {
  const logs = [];
  const done = [];
  const host = quickjs(new SimulatedClock(), { log: (record) => logs.push(record), done: (record) => done.push(record) });
  const segment = host.addImage(compile([{ name: 'counter.scxml', text: COUNTER }]));
  assert.equal(typeof segment, 'bigint');
  const session = host.spawn('counter', { name: 'a', params: { n: 10 } });
  assert.equal(typeof session, 'bigint');
  host.send(session, 'inc', { by: 2 });
  assert.deepEqual(logs, [{ session, label: 'n', text: '12' }]);
  host.send(session, 'stop');
  assert.equal(done.length, 1);
  assert.equal(done[0].session, session);
  assert.equal(done[0].name, 'a');
  assert.deepEqual(done[0].data, { total: 12 });
  assert.equal(done[0].final, 'end');
  host.close();
});

test('a delayed send fires when the simulated clock reaches it', () => {
  const logs = [];
  const clock = new SimulatedClock();
  const host = quickjs(clock, { log: (record) => logs.push(record.label) });
  host.addImage(compile([{ name: 'counter.scxml', text: COUNTER }]));
  const session = host.spawn('counter');
  host.send(session, 'later');
  clock.increment(49);
  assert.deepEqual(logs, []);
  clock.increment(1);
  assert.deepEqual(logs, ['tick']);
  assert.equal(clock.now(), 50);
  host.close();
});

test('a delayed send fires on the runtime clock, from one setTimeout', async () => {
  let resolve;
  const fired = new Promise((done) => (resolve = done));
  const host = quickjs(undefined, { log: (record) => record.label === 'tick' && resolve(performance.now()) });
  host.addImage(compile([{ name: 'counter.scxml', text: COUNTER }]));
  const session = host.spawn('counter');
  const started = performance.now();
  host.send(session, 'later');
  const at = await fired;
  assert.ok(at - started >= 49, `fired after ${at - started} ms`);
  host.close();
});

test('a config without ticksPerSecond runs on the host\'s own rate (host.config)', () => {
  const logs = [];
  const clock = new SimulatedClock();
  const host = new Host({ config: { systemID: 'h' }, datamodel, clock, on: { log: (record) => logs.push(record.label) } });
  assert.equal(host.request('host.config').system.ticksPerSecond, 1000000000);
  host.addImage(compile([{ name: 'counter.scxml', text: COUNTER }]));
  host.send(host.spawn('counter'), 'later');
  clock.increment(49);
  assert.deepEqual(logs, []);
  clock.increment(1);
  assert.deepEqual(logs, ['tick']);
  host.close();
});

test('a refused config is a TinyactorsError with core\'s code, reason and path', () => {
  assert.throws(
    () => new Host({ config: { systemID: 'h', x: 1 }, datamodel }),
    (error) => error instanceof TinyactorsError && error.code === -32602 && error.reason === 'unknown' && error.path === 'x',
  );
});

test('request gives the result, or throws core\'s error', () => {
  const host = quickjs(new SimulatedClock(), {});
  assert.deepEqual(host.request('actor.list'), { sessions: [], cursor: null });
  assert.throws(
    () => host.request('actor.list', { x: 1 }),
    (error) => error instanceof TinyactorsError && error.code === -32602 && error.reason === 'unknown' && error.path === 'params.x',
  );
  host.close();
});

test('a chart that does not compile: TinyactorsError with core\'s diagnostics', () => {
  assert.throws(
    () => compile([{ name: 'bad.scxml', text: '<scxml xmlns="http://www.w3.org/2005/07/scxml" initial="nowhere"/>' }]),
    (error) => error instanceof TinyactorsError && error.status === ERROR_INVALID && error.diagnostics.length > 0,
  );
});

test('a method on another object, or on a closed host, is refused', () => {
  const host = quickjs(new SimulatedClock(), {});
  // V8 refuses a foreign `this` itself (TypeError); Bun leaves it to the
  // addon's type tag (TinyactorsError).
  assert.throws(() => Host.prototype.spawn.call({}, 'x'));
  assert.throws(() => quickjs({}, {}), TinyactorsError);
  assert.throws(() => quickjs(Object.create(SimulatedClock.prototype), {}), TinyactorsError);
  host.close();
  assert.throws(() => host.spawn('counter'), TinyactorsError);
});

for (const each of DATAMODELS) {
  datamodel = each;
  console.log(`-- ${each}`);
  await run();
}
