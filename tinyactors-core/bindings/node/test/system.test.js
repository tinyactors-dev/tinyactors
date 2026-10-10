'use strict';
// Systems and actors (ta_host): spawn, send, snapshots, subscriptions,
// toPromise, stop, names, many sends, simulated time, <log>, and exceptions
// in expressions — under both "ecmascript" datamodels.
const test = require('node:test');
const assert = require('node:assert/strict');
const { scxml, fromSCXML, createSystem, createActor, toPromise, Actor } = require('./binding');

const c = scxml.chart();
/* text expressions: both datamodels run them */
const light = c.scxml('light', { initial: 'green', datamodel: 'ecmascript' },
  c.datamodel({ cycles: 0, last: null }),
  c.state('green',
    c.onentry(c.send('timer', { delay: '30s', id: 'timer' })),
    c.transition('timer', 'yellow'),
    c.transition('emergency', 'red', c.assign('last', '_event.data.reason'))),
  c.state('yellow', c.transition('timer', 'red'), c.onentry(c.send('timer', { delay: '5s' }))),
  c.state('red',
    c.onentry(c.log('red', "'stop ' + cycles")),
    c.transition('timer', 'green', c.assign('cycles', 'cycles + 1')),
    c.transition('off', 'off')),
  c.final('off', c.donedata(c.param('cycles', 'cycles'))));

const nested = c.scxml('nested', { initial: 'p', datamodel: 'ecmascript' },
  c.parallel('p',
    c.state('left', { initial: 'l1' }, c.state('l1', c.transition('go', 'l2')), c.state('l2')),
    c.state('right', { initial: 'r1' }, c.state('r1'))));

for (const ecmascript of ['native', 'quickjs']) {
  test(`${ecmascript}: spawn, send, snapshots with nested values; delays on a simulated clock; donedata`, async () => {
    let now = 0;
    const logs = [];
    const system = createSystem({ ecmascript, clock: () => now, logger: (label, text) => logs.push([label, text]) });
    const a = createActor(light, { system, systemID: 'main' }).start();
    assert.ok(a instanceof Actor);
    system.flush();
    assert.deepEqual(a.getSnapshot(), { value: 'green', context: { cycles: 0, last: null }, status: 'active', output: undefined, error: undefined });
    now += 30000;
    system.flush();
    assert.equal(a.getSnapshot().value, 'yellow');
    now += 5000;
    system.flush();
    assert.equal(a.getSnapshot().value, 'red');
    assert.deepEqual(logs, [['red', 'stop 0']]);
    a.send({ type: 'timer' });
    assert.equal(a.getSnapshot().value, 'green');
    assert.equal(a.getSnapshot().context.cycles, 1);
    a.send({ type: 'emergency', reason: 'fire' });
    assert.deepEqual(a.getSnapshot().context, { cycles: 1, last: 'fire' });
    assert.equal(system.get('main'), a);
    assert.equal(a.address, `tinyactors://${system.id}/#_scxml_${a.sessionID}`);
    const done = toPromise(a);
    a.send({ type: 'off' });
    assert.deepEqual(await done, { cycles: 1 });
    assert.deepEqual(a.getSnapshot(), { value: 'off', context: { cycles: 1, last: 'fire' }, status: 'done', output: { cycles: 1 }, error: undefined });
    assert.equal(system.get('main'), undefined);

    const n = createActor(nested, { system }).start();
    assert.deepEqual(n.getSnapshot().value, { p: { left: 'l1', right: 'r1' } });
    n.send({ type: 'go' });
    assert.deepEqual(n.getSnapshot().value, { p: { left: 'l2', right: 'r1' } });
    system.stop();
  });

  test(`${ecmascript}: input overrides <data>; subscribers hear of events their actor took; stop`, () => {
    const system = createSystem({ ecmascript });
    const a = createActor(light, { system, input: { cycles: 41 } }).start();
    const b = createActor(light, { system }).start();
    assert.equal(a.getSnapshot().context.cycles, 41);
    const seen = [];
    let completed = 0;
    const sub = a.subscribe({ next: (s) => seen.push(s.value), complete: () => completed++ });
    a.send({ type: 'emergency' });
    b.send({ type: 'emergency' }); /* not a's: no call */
    a.send({ type: 'nothing' }); /* taken (no transition): a call */
    assert.deepEqual(seen, ['red', 'red']);
    sub.unsubscribe();
    a.send({ type: 'timer' });
    assert.equal(seen.length, 2);
    a.subscribe({ complete: () => completed++ });
    a.stop();
    assert.equal(completed, 1);
    assert.equal(a.getSnapshot().status, 'stopped');
    assert.equal(a.getSnapshot().value, 'green', 'the snapshot it stopped with');
    a.send({ type: 'timer' }); /* ignored */
    assert.equal(system.actors().length, 1);
    system.stop();
  });

  test(`${ecmascript}: sendMany in one call; system.send with ta_event_in fields; invoked children are actors`, () => {
    const system = createSystem({ systemID: 'many', ecmascript });
    const actors = Array.from({ length: 5 }, () => createActor(light, { system }).start());
    const n = system.sendMany(actors.map((a, i) => [a, { type: i % 2 ? 'emergency' : 'timer' }]));
    assert.equal(n, 5);
    assert.deepEqual(actors.map((a) => a.getSnapshot().value), ['yellow', 'red', 'yellow', 'red', 'yellow']);
    const echo = fromSCXML(`<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="fields" initial="s">
      <datamodel><data id="got" expr="null"/></datamodel>
      <state id="s"><transition event="hello"><assign location="got" expr="[_event.sendid, _event.origin, _event.origintype, _event.data.x]"/></transition></state></scxml>`);
    const e = createActor(echo, { system, systemID: 'fields' }).start();
    system.send('tinyactors://many/@fields', { type: 'hello', x: 1 }, { sendid: 's1', origin: 'tinyactors://far/#_scxml_9', origintype: 'urn:me' });
    assert.deepEqual(e.getSnapshot().context.got, ['s1', 'tinyactors://far/#_scxml_9', 'urn:me', 1]);
    system.stop();
  });

  test(`${ecmascript}: an expression that throws raises error.execution; the logger hears of it`, () => {
    const logs = [];
    const system = createSystem({ ecmascript, logger: (...a) => logs.push(a) });
    const doc = fromSCXML(`<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="oops" initial="s">
      <state id="s"><transition event="go" cond="nope.x" target="never"/><transition event="error.execution" target="caught"/></state>
      <state id="never"/><state id="caught"/></scxml>`);
    const a = createActor(doc, { system }).start();
    a.send({ type: 'go' });
    assert.equal(a.getSnapshot().value, 'caught');
    if (ecmascript === 'native') assert.ok(logs.some(([label, e]) => label === 'error.execution' && /nope/.test(String(e))));
    system.stop();
  });
}

test('native: c.expr functions, closures and calls into the system from expressions', () => {
  const system = createSystem();
  const calls = [];
  const h = scxml.chart();
  const recorder = h.scxml('recorder', { initial: 's', datamodel: 'ecmascript' },
    h.datamodel({ items: h.expr(() => []) }),
    h.state('s', h.transition('add', undefined, h.script(h.expr(({ items, _event }) => {
      items.push(_event.data.item);
      calls.push(_event.data.item);
      if (_event.data.item === 'again') a.send({ type: 'add', item: 'later' }); /* queued: sent after the run */
    })))));
  const a = createActor(recorder, { system, systemID: 'rec' }).start();
  a.send({ type: 'add', item: 'a' });
  a.send({ type: 'add', item: 'again' });
  assert.deepEqual(calls, ['a', 'again', 'later']);
  assert.deepEqual(a.getSnapshot().context.items, ['a', 'again', 'later']);
  system.stop();
});

test('real time: delays fire on their own timer', async () => {
  const system = createSystem();
  const h = scxml.chart();
  const quick = h.scxml('quick', { initial: 'a', datamodel: 'ecmascript' },
    h.state('a', h.onentry(h.send('t', { delay: '20ms' })), h.transition('t', 'z')),
    h.final('z'));
  const a = createActor(quick, { system }).start();
  await toPromise(a);
  assert.equal(a.getSnapshot().value, 'z');
  system.stop();
});

test('boot: a <system> manifest spawns its actors by name', () => {
  const system = createSystem();
  const actors = system.boot(
    `<system><chart name="light" src="light.scxml"/><actor name="corner" chart="light"/></system>`,
    { documents: { 'light.scxml': light.xml } },
  );
  assert.deepEqual(Object.keys(actors), ['corner']);
  assert.equal(actors.corner.getSnapshot().value, 'green');
  assert.equal(system.get('corner'), actors.corner);
  system.stop();
});

test('stats: the host counts and each chart', () => {
  const system = createSystem();
  createActor(light, { system }).start();
  const b = createActor(light, { system }).start();
  b.send({ type: 'emergency' });
  b.send({ type: 'off' });
  const stats = system.stats();
  assert.equal(stats.actors, 1);
  const chart = stats.charts.find((x) => x.name === 'light');
  assert.equal(chart.spawned, 2);
  assert.equal(chart.ended, 1);
  system.stop();
});
