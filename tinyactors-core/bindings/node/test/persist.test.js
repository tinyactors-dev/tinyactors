'use strict';
// Actors leave memory and come back (ta_host_sweep / _hibernate / _wake),
// move between systems (ta_host_export_actor / ta_host_adopt), survive a
// restart (ta_host_snapshot / ta_host_restore) and move to new versions of
// their chart (ta_host_migrate, error.platform.upgrade) — under both
// "ecmascript" datamodels.
const test = require('node:test');
const assert = require('node:assert/strict');
const { scxml, fromSCXML, createSystem, createActor, toPromise } = require('./binding');
const { until } = require('./charts');

const counter = (version = 1, extra = '') =>
  fromSCXML(`<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="counter" initial="on">
  <datamodel><data id="n" expr="0"/><data id="version" expr="${version}"/>${extra}</datamodel>
  <state id="on">
    <transition event="add"><assign location="n" expr="n + (_event.data.by || 1)"/></transition>
    <transition event="later"><send event="add" delay="1000ms"><param name="by" expr="100"/></send></transition>
    <transition event="error.platform.upgrade"><assign location="n" expr="_event.data.n * 10"/></transition>
    <transition event="end" target="done"/>
  </state>
  <final id="done"><donedata><param name="n" expr="n"/></donedata></final>
</scxml>`);

/* a store whose writes and reads resolve later */
function slowStore() {
  const map = new Map();
  const later = (v) => new Promise((r) => setTimeout(() => r(v), 2));
  return { map, get: (k) => later(map.get(k)), set: (k, v) => later(map.set(k, v)), delete: (k) => later(map.delete(k)) };
}

for (const ecmascript of ['native', 'quickjs']) {
  test(`${ecmascript}: hibernation: idle actors go to the store and wake for the next event`, async () => {
    const store = slowStore();
    const system = createSystem({ ecmascript, hibernation: { store } });
    const chart = counter();
    const a = createActor(chart, { system }).start();
    const b = createActor(chart, { system }).start();
    a.send({ type: 'add', by: 2 });
    assert.equal(system.sweep(), 0, 'the first sweep only marks');
    b.send({ type: 'add' });
    assert.equal(system.sweep(), 1, 'a did not run since: hibernated');
    assert.equal(a.hibernated, true);
    assert.equal(b.hibernated, false);
    assert.deepEqual(a.getSnapshot().context, { n: 2, version: 1 }, 'the snapshot it went to sleep with');
    await until(() => store.map.size === 1);
    a.send({ type: 'add', by: 3 });
    await until(() => !a.hibernated);
    assert.equal(a.getSnapshot().context.n, 5);
    await until(() => store.map.size === 0);
    assert.equal(system.stats().hibernated, 0);
    system.stop();
  });

  test(`${ecmascript}: export and adopt: the same session in another system, its timers keep their time left`, async () => {
    let now = 0;
    let later = 5000;
    const chart = counter();
    const one = createSystem({ systemID: 'one', ecmascript, clock: () => now });
    const two = createSystem({ systemID: 'two', ecmascript, clock: () => later });
    two.load([chart]);
    const a = createActor(chart, { system: one }).start();
    a.send({ type: 'add', by: 4 });
    a.send({ type: 'later' });
    now = 400;
    one.flush();
    const blob = one.export(a);
    assert.ok(blob instanceof Uint8Array);
    assert.equal(a.getSnapshot().status, 'stopped');
    const b = two.adopt(blob);
    assert.equal(b.sessionID, a.sessionID);
    assert.deepEqual(b.getSnapshot().context, { n: 4, version: 1 });
    later += 599;
    two.flush();
    assert.equal(b.getSnapshot().context.n, 4, 'not yet: 600 ms were left');
    later += 1;
    two.flush();
    assert.equal(b.getSnapshot().context.n, 104);
    assert.match(b.address, /^tinyactors:\/\/two\/#_scxml_/);
    one.stop();
    two.stop();
  });

  test(`${ecmascript}: snapshot and restore: the whole system, queued timers included`, () => {
    let now = 0;
    const chart = counter();
    const system = createSystem({ systemID: 'snap', ecmascript, clock: () => now });
    const a = createActor(chart, { system, systemID: 'a' }).start();
    a.send({ type: 'add', by: 7 });
    a.send({ type: 'later' });
    const bytes = system.snapshot();
    system.stop();
    const again = createSystem({ ecmascript, clock: () => now, restore: bytes, charts: [chart] });
    assert.equal(again.id, 'snap');
    const r = again.get('a');
    assert.deepEqual(r.getSnapshot().context, { n: 7, version: 1 });
    now = 1000;
    again.flush();
    assert.equal(r.getSnapshot().context.n, 107);
    again.stop();
  });

  test(`${ecmascript}: migration: running actors move to the current version and get error.platform.upgrade`, async () => {
    const system = createSystem({ ecmascript });
    const v1 = counter(1);
    const a = createActor(v1, { system }).start();
    a.send({ type: 'add', by: 3 });
    system.load([counter(2, '<data id="added" expr="true"/>')]);
    const fresh = createActor(v1, { system }).start();
    assert.equal(fresh.getSnapshot().context.version, 2, 'new actors start in the current version');
    const { done } = system.migrate('counter');
    assert.deepEqual(await done, { migrated: 1, failed: 0, pending: [] });
    assert.deepEqual(a.getSnapshot().context, { n: 30, version: 1, added: true }, 'kept its data; new data initialized; upgraded');
    a.send({ type: 'end' });
    assert.deepEqual(await toPromise(a), { n: 30 });
    system.stop();
  });
}

test('native: a c.expr chart is restored with its functions from `charts`', () => {
  const c = scxml.chart();
  const factor = 3;
  const chart = c.scxml('scaled', { initial: 's', datamodel: 'ecmascript' },
    c.datamodel({ n: 1 }),
    c.state('s', c.transition('scale', undefined, c.assign('n', c.expr(({ n }) => n * factor)))));
  const system = createSystem({ systemID: 'scaled' });
  createActor(chart, { system, systemID: 's' }).start().send({ type: 'scale' });
  const bytes = system.snapshot();
  system.stop();
  const again = createSystem({ restore: bytes, charts: [chart] });
  again.get('s').send({ type: 'scale' });
  assert.equal(again.get('s').getSnapshot().context.n, 9);
  again.stop();
});

test('native: what cannot be stored stays resident, and says why', () => {
  const c = scxml.chart();
  const chart = c.scxml('holder', { initial: 's', datamodel: 'ecmascript' }, c.datamodel({ f: c.expr(() => () => 1) }), c.state('s'));
  const system = createSystem({ hibernation: { store: new Map() } });
  const a = createActor(chart, { system }).start();
  assert.throws(() => system.hibernate(a), /variable 'f' cannot be stored: it holds a function/);
  assert.throws(() => system.snapshot(), /variable 'f' cannot be stored/);
  assert.equal(a.hibernated, false);
  system.stop();
});
