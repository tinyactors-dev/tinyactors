// The element charts, driven directly (the elements only render snapshots
// and forward DOM events). Runs on the web (wasm) binding, like the page.
import test from 'node:test';
import assert from 'node:assert/strict';
import * as ta from '../../../bindings/web/node.mjs';
import { uiCharts } from '../charts.mjs';

const T = { repeatDelay: 30, repeatEvery: 10, debounce: 20, latency: 30, dialog: 15, submit: 20 };
const m = uiCharts(ta, T);
const system = ta.createSystem();
test.after(() => system.stop());
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const until = async (cond, what, limit = 2000) => {
  const t0 = Date.now();
  while (!cond()) {
    if (Date.now() - t0 > limit) throw new Error(`timed out: ${what}`);
    await sleep(2);
  }
};

test('toggle: on/off, disabled', () => {
  const a = ta.createActor(m.toggle, { system }).start();
  a.send({ type: 'TOGGLE' });
  assert.equal(a.getSnapshot().value, 'on');
  a.send({ type: 'DISABLE' });
  a.send({ type: 'TOGGLE' });
  assert.equal(a.getSnapshot().value, 'on');
  a.send({ type: 'ENABLE' });
  a.send({ type: 'TOGGLE' });
  assert.equal(a.getSnapshot().value, 'off');
});

test('counter: bounds, and auto-repeat while held', async () => {
  const a = ta.createActor(m.counter, { system, input: { value: 5, min: 0, max: 10 } }).start();
  a.send({ type: 'INC' });
  assert.equal(a.getSnapshot().context.value, 6);
  a.send({ type: 'PRESS', dir: 1 }); /* one step at once, then repeats */
  assert.equal(a.getSnapshot().context.value, 7);
  await until(() => a.getSnapshot().context.value === 10, 'repeating up to the bound');
  await until(() => a.getSnapshot().value === 'idle', 'stops at the bound');
  a.send({ type: 'RELEASE' });
  a.send({ type: 'PRESS', dir: -1 });
  await until(() => a.getSnapshot().value === 'repeating', 'repeat delay');
  a.send({ type: 'RELEASE' });
  const v = a.getSnapshot().context.value;
  await sleep(40);
  assert.equal(a.getSnapshot().context.value, v, 'no more steps after release');
  assert.ok(v <= 9);
});

test('search: debounce, stale answers dropped, keyboard', async () => {
  const words = ['apple', 'apricot', 'avocado', 'banana', 'blueberry', 'cherry', 'grape', 'grapefruit'];
  const a = ta.createActor(m.search, { system, input: { words } }).start();
  a.send({ type: 'TYPE', value: 'a' });
  a.send({ type: 'TYPE', value: 'ap' }); /* restarts the debounce */
  assert.equal(a.getSnapshot().value, 'debouncing');
  await until(() => a.getSnapshot().value === 'searching', 'the search');
  a.send({ type: 'TYPE', value: 'grape' }); /* while "ap" is in flight */
  await until(() => a.getSnapshot().value === 'open', 'results');
  assert.deepEqual(a.getSnapshot().context.results, ['grape', 'grapefruit'], 'only the latest answer counts');
  a.send({ type: 'DOWN' });
  a.send({ type: 'DOWN' });
  a.send({ type: 'DOWN' });
  assert.equal(a.getSnapshot().context.active, 1);
  a.send({ type: 'PICK' });
  assert.equal(a.getSnapshot().context.picked, 'grapefruit');
  assert.equal(a.getSnapshot().value, 'idle');
});

test('form: fields report to the form actor; submit only when valid', async () => {
  const form = ta.createActor(m.form, { system, input: { names: ['name', 'email'] } }).start();
  const name = ta.createActor(m.field, { system, input: { name: 'name', rule: 'name', form } }).start();
  const email = ta.createActor(m.field, { system, input: { name: 'email', rule: 'email', form } }).start();
  name.send({ type: 'INPUT', value: 'Ada' });
  email.send({ type: 'INPUT', value: 'ada@' });
  assert.equal(email.getSnapshot().context.error, 'not an email address');
  form.send({ type: 'SUBMIT' });
  assert.equal(form.getSnapshot().value, 'editing');
  email.send({ type: 'INPUT', value: 'ada@example.org' });
  form.send({ type: 'SUBMIT' });
  assert.equal(form.getSnapshot().value, 'submitting');
  await until(() => form.getSnapshot().value === 'done', 'submitted');
  assert.deepEqual(form.getSnapshot().context.sent, { name: 'Ada', email: 'ada@example.org' });
});

test('dialog: timed opening and closing', async () => {
  const a = ta.createActor(m.dialog, { system }).start();
  a.send({ type: 'OPEN' });
  assert.equal(a.getSnapshot().value, 'opening');
  await until(() => a.getSnapshot().value === 'open', 'open');
  a.send({ type: 'ESCAPE' });
  await until(() => a.getSnapshot().value === 'closed', 'closed');
});

test('charts read well as SCXML', () => {
  const xml = m.search.xml;
  assert.match(xml, /<state id="debouncing">/);
  assert.doesNotMatch(xml, /expr="[a-z]{1,3}:\d+"/);
});
