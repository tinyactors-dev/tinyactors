// The web binding has the same API as the Node.js binding: the same
// declarations (index.d.ts differs only in its first line) and every declared
// value exported at runtime.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import * as web from '../node.mjs';

const read = (p) => readFileSync(new URL(p, import.meta.url), 'utf8');
const body = (dts) => dts.split('\n').slice(1).join('\n');

test('index.d.ts is the Node binding\'s declarations', () => {
  assert.equal(body(read('../index.d.ts')), body(read('../../node/index.d.ts')));
});

test('every declared function and class is exported', () => {
  const declared = [...read('../index.d.ts').matchAll(/export declare (?:function|class) (\w+)/g)].map((m) => m[1]);
  assert.ok(declared.length > 20);
  for (const name of declared) assert.equal(typeof web[name], 'function', name);
});

test('charts compile and run on the wasm runtime', () => {
  const c = web.scxml.chart();
  const chart = c.scxml('w', { initial: 'a', datamodel: 'ecmascript' }, c.datamodel({ n: 0 }),
    c.state('a', c.transition('GO', 'b', c.assign('n', c.expr(() => 1)))), c.state('b'));
  const system = web.createSystem();
  const a = web.createActor(chart, { system }).start();
  a.send({ type: 'GO' });
  assert.equal(a.getSnapshot().value, 'b');
  assert.equal(a.getSnapshot().context.n, 1);
  assert.match(chart.xml, /<scxml/);
  system.stop();
});

test('actors move between the Node addon and wasm (export/adopt), delayed data as values, the same image', async (t) => {
  const { createRequire } = await import('node:module');
  let node;
  try {
    node = createRequire(import.meta.url)('../../node');
  } catch {
    return t.skip('the Node addon is not built (mise run node:test)');
  }
  const xml = `<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" initial="w">
  <datamodel><data id="got" expr="null"/></datamodel>
  <script>var hits = 0; function hit(v) { hits += 1; return v; }</script>
  <state id="w"><onentry><send event="late" delay="100ms"><param name="v" expr="({ list: [1, 2] })"/></send></onentry>
    <transition event="late" target="done"><assign location="got" expr="hit(_event.data.v)"/></transition></state>
  <state id="done"/></scxml>`;
  for (const [from, to] of [[node, web], [web, node]]) {
    let t = 0, u = 500;
    const a = from.createSystem({ clock: () => t, ecmascript: 'quickjs' });
    const actor = from.createActor(from.fromSCXML(xml, { name: 'm' }), { system: a }).start();
    a.flush();
    t = 40;
    const b = to.createSystem({ clock: () => u, ecmascript: 'quickjs' });
    b.load([to.fromSCXML(xml, { name: 'm' })]);
    const moved = b.adopt(a.export(actor));
    u += 59;
    b.flush();
    assert.equal(moved.getSnapshot().value, 'w', 'not before its time');
    u += 1;
    b.flush();
    assert.equal(moved.getSnapshot().value, 'done');
    assert.deepEqual(moved.getSnapshot().context, { got: { list: [1, 2] } });
    a.stop();
    b.stop();
  }
});
