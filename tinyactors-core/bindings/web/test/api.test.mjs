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

test('machines compile and run on the wasm runtime', () => {
  const m = web.createMachine({ id: 'w', initial: 'a', context: { n: 0 }, states: { a: { on: { GO: { target: 'b', actions: web.assign({ n: 1 }) } } }, b: {} } });
  const a = web.createActor(m).start();
  a.send({ type: 'GO' });
  assert.equal(a.getSnapshot().value, 'b');
  assert.equal(a.getSnapshot().context.n, 1);
  assert.match(m.toSCXML(), /<scxml/);
});

test('actors move between the Node addon and wasm (export/adopt), delayed data as values', async (t) => {
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
    const a = from.createSystem({ clock: () => t });
    const actor = from.createActor(from.fromSCXML(xml, { id: 'm' }), { system: a }).start();
    a.flush();
    t = 40;
    const b = to.createSystem({ clock: () => u });
    const moved = b.adopt(a.export(actor));
    u += 59;
    b.flush();
    assert.equal(moved.getSnapshot().value, 'w', 'not before its time');
    u += 1;
    b.flush();
    assert.equal(moved.getSnapshot().value, 'done');
    const { got, hits, hit } = moved.getSnapshot().context;
    assert.deepEqual({ got, hits }, { got: { list: [1, 2] }, hits: 1 });
    assert.equal(typeof hit, 'function', 're-created');
    a.stop();
    b.stop();
  }
});
