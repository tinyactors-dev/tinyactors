// AGENTS.md's "Rules the suite cannot see", each with a test of its own,
// on the native datamodel (the one whose code they constrain) unless the
// rule is the API's. Run under node and bun (mise run test).
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import v8 from 'node:v8';
import vm from 'node:vm';
import process from 'node:process';
import { test, run } from './harness.mjs';
import { Host, SimulatedClock, compile, encode, decode, TinyactorsError, ERROR_INVALID } from '@tinyactors/node';

// A full collection, in both runtimes.
const gc = globalThis.Bun ? () => globalThis.Bun.gc(true) : (v8.setFlagsFromString('--expose-gc'), vm.runInNewContext('gc'));
const settle = () => new Promise((resolve) => setTimeout(resolve, 20));

function chart(name, body, data = '') {
  return compile([
    {
      name: `${name}.scxml`,
      text: `<scxml xmlns="http://www.w3.org/2005/07/scxml" datamodel="ecmascript" name="${name}" initial="s">
        <datamodel>${data}</datamodel>${body}</scxml>`,
    },
  ]);
}

function native(on = {}) {
  const clock = new SimulatedClock();
  return { clock, host: new Host({ config: { systemID: 'h', system: { ticksPerSecond: 1000 } }, clock, on }) };
}

// ---- Dropping a payload's last reference is deferred; reference counts in the slot table ----

test('payloads the host releases are dropped: after a run, and in close (ta_host_free)', async () => {
  const finalized = new Set();
  const registry = new FinalizationRegistry((held) => finalized.add(held));
  globalThis.keep = (value, name) => registry.register(value, name);
  const { host } = native();
  host.addImage(
    chart('keeper', `<state id="s"><transition event="v"><script>keep(_event.data, _event.data.name)</script></transition></state>`),
  );
  const session = host.spawn('keeper');
  host.send(session, 'v', { name: 'first' });
  host.send(session, 'v', { name: 'second' }); // the first event's data is released by this run
  for (let i = 0; i < 5 && !finalized.has('first'); i++) {
    gc();
    await settle();
  }
  assert.ok(finalized.has('first'), 'the payload of an event no session holds is collected');
  host.close(); // ta_host_free releases the second event's data, outside a run
  for (let i = 0; i < 5 && !finalized.has('second'); i++) {
    gc();
    await settle();
  }
  assert.ok(finalized.has('second'), 'close() drops what the host released');
  delete globalThis.keep;
});

test('a Host the application never closed is freed by its finalizer, payloads and all', async () => {
  for (let i = 0; i < 20; i++) {
    const { host } = native();
    host.addImage(chart('idle', '<state id="s"/>', '<data id="d" expr="({a: [1, 2, 3]})"/>'));
    host.spawn('idle', { params: { d: { big: 'x'.repeat(1000) } } });
  }
  for (let i = 0; i < 3; i++) {
    gc();
    await settle();
  }
});

// ---- Handles are slots in two tables, each with a generation ----------------------------

test('sessions that end leave their slots to new ones, which see only their own values', () => {
  const done = new Map();
  const { host } = native({ done: (record) => done.set(record.name, record.data) });
  host.addImage(
    chart(
      'once',
      `<state id="s"><transition event="end" target="f"/></state>
       <final id="f"><donedata><param name="mine" expr="mine"/><param name="seen" expr="_event.data"/></donedata></final>`,
      '<data id="mine"/>',
    ),
  );
  for (let i = 0; i < 200; i++) {
    const session = host.spawn('once', { name: `a${i}`, params: { mine: i } });
    host.send(session, 'end', { i });
  }
  assert.equal(done.size, 200);
  for (let i = 0; i < 200; i++) assert.deepEqual(done.get(`a${i}`), { mine: i, seen: { i } });
  host.close();
});

// ---- Text is valid UTF-8 ------------------------------------------------------------------

test('a lone surrogate is refused: in the API\'s data, and in what a chart sends', () => {
  const errors = [];
  const { host } = native({ log: (record) => errors.push(record) });
  host.addImage(
    chart(
      'texts',
      `<state id="s">
         <transition event="go"><send event="x" target="#_internal"><content expr="'\\ud800'"/></send></transition>
         <transition event="error.execution"><log label="refused" expr="_event.name"/></transition>
       </state>`,
    ),
  );
  const session = host.spawn('texts');
  assert.throws(() => host.send(session, 'e', { text: '\ud800' }), (error) => error.status === ERROR_INVALID);
  assert.throws(() => host.send(session, 'e\udc00'), (error) => error.status === ERROR_INVALID);
  host.send(session, 'go');
  assert.deepEqual(errors.map((record) => record.label), ['refused']);
  host.close();
});

// ---- Expressions are strict functions inside with (scope); assign checks its location ------

test('an undeclared assignment is error.execution and leaves the realm\'s globals alone', () => {
  const logs = [];
  const { host } = native({ log: (record) => logs.push(record.text) });
  host.addImage(
    chart(
      'strict',
      `<state id="s">
         <transition event="script"><script>undeclaredByScript = 1</script></transition>
         <transition event="assign"><assign location="undeclaredByAssign" expr="1"/></transition>
         <transition event="system"><assign location="_sessionid" expr="'x'"/></transition>
         <transition event="global"><assign location="process" expr="1"/></transition>
         <transition event="declared"><assign location="known.a" expr="2"/><log expr="known.a"/></transition>
         <transition event="error.execution"><log expr="'error.execution'"/></transition>
       </state>`,
      '<data id="known" expr="({a: 1})"/>',
    ),
  );
  const session = host.spawn('strict');
  for (const event of ['script', 'assign', 'system', 'global', 'declared']) host.send(session, event);
  assert.deepEqual(logs, ['error.execution', 'error.execution', 'error.execution', 'error.execution', '2']);
  assert.equal('undeclaredByScript' in globalThis, false);
  assert.equal('undeclaredByAssign' in globalThis, false);
  assert.equal(typeof process, 'object');
  host.close();
});

// ---- One handle scope per vtable entry -----------------------------------------------------

// 400,000 evaluations in one run: with a handle scope per entry, memory
// grows 17 MB under node and 23 MB under bun by the time the run's last
// script runs; without them, 78 and 118 MB (a spike, 2026-10-12: 1,000,000
// evaluations gave 26 and 48 MB with them, 192 and 308 MB without).
test('handles opened by the entries of one run do not pile up until it returns', () => {
  let during = 0;
  globalThis.probe = () => {
    during = process.memoryUsage().rss;
  };
  const { host } = native();
  host.addImage(
    chart(
      'many',
      `<state id="s"><transition event="go">
         <foreach array="items" item="x"><assign location="sum" expr="sum + x"/></foreach>
         <script>probe()</script>
       </transition></state>`,
      '<data id="items" expr="Array.from({length: 400000}, (_, i) => i)"/><data id="sum" expr="0"/>',
    ),
  );
  const session = host.spawn('many');
  gc();
  const before = process.memoryUsage().rss;
  host.send(session, 'go');
  const grown = (during - before) / 1e6;
  assert.ok(grown < 50, `memory grew ${grown.toFixed(1)} MB during the run`);
  assert.equal(host.request('actor.context', { session }).variables.sum, 79999800000);
  host.close();
  delete globalThis.probe;
});

// ---- napi_get_value_int64 saturates silently ------------------------------------------------

test('ids are BigInts; a number past 2^53 in data stays exact as a float, never saturated', () => {
  const { host } = native();
  host.addImage(chart('idle', '<state id="s"/>'));
  const session = host.spawn('idle');
  assert.equal(typeof session, 'bigint');
  assert.throws(() => host.send(2 ** 64, 'e'), TinyactorsError);
  assert.throws(() => host.send(1.5, 'e'), TinyactorsError);
  assert.throws(() => host.send(2n ** 64n, 'e'), TinyactorsError);
  host.send(Number(session), 'e'); // a safe integer from a request's reply is taken
  assert.deepEqual(decode(encode(2 ** 63)), 2 ** 63);
  assert.equal(encode(2 ** 63)[0], 0xfa); // a float, not a saturated integer
  host.close();
});

// ---- napi_type_tag_object on every wrapped object ------------------------------------------

test('a wrapped object of another kind is refused, not misread', () => {
  const { clock, host } = native();
  assert.throws(() => new Host({ config: { systemID: 'h' }, clock: host }), (error) => error.status === ERROR_INVALID);
  assert.throws(() => SimulatedClock.prototype.increment.call(host, 1));
  assert.throws(() => Host.prototype.send.call(clock, 1n, 'e'));
  host.close();
});

// ---- The timer is the global setTimeout, ref'd while a deadline is pending -------------------

// A child process runs a host on the runtime's clock: it stays alive while
// a delayed send is pending and exits when nothing is (bindings.md,
// section 11: Node is alive while a deadline is pending).
test('a pending deadline keeps the process alive; none lets it exit', () => {
  const script = (delay) => `
    import { Host, compile } from '@tinyactors/node';
    const host = new Host({ config: { systemID: 'h' }, on: { log: ({ label }) => console.log(label) } });
    host.addImage(compile([{ name: 't.scxml', text: '<scxml xmlns="http://www.w3.org/2005/07/scxml" datamodel="ecmascript" name="t" initial="s"><state id="s"><onentry>${delay}</onentry><transition event="tick"><log label="tick" expr="1"/></transition></state></scxml>' }]));
    host.spawn('t');
    console.log('spawned');`;
  const runtime = process.execPath;
  const options = { cwd: new URL('.', import.meta.url), encoding: 'utf8', timeout: 10000 };
  const started = Date.now();
  const pending = spawnSync(runtime, ['--input-type=module', '-e', script('<send event="tick" delay="200ms"/>')], options);
  const pendingTook = Date.now() - started;
  assert.equal(pending.status, 0, pending.stderr);
  assert.deepEqual(pending.stdout.trim().split('\n'), ['spawned', 'tick']);
  assert.ok(pendingTook >= 200, `exited after ${pendingTook} ms, before the deadline`);
  const idle = spawnSync(runtime, ['--input-type=module', '-e', script('')], options);
  assert.equal(idle.status, 0, idle.stderr);
  assert.deepEqual(idle.stdout.trim().split('\n'), ['spawned']);
});

// ---- The README says chart code is trusted, and that nesting stops at 64 ----------------------

test('the README says chart code is trusted, runs in the application\'s realm, and nests 64 deep', () => {
  const readme = readFileSync(new URL('../README.md', import.meta.url), 'utf8');
  assert.match(readme, /Chart code is trusted/);
  assert.match(readme, /application's realm/);
  assert.match(readme, /64/);
  assert.match(readme, /shares subtrees/);
});

// ---- An actor reference is a string ----------------------------------------------------------

test('an actor reference (tag 32) is read as its address text and never written', () => {
  const reference = decode(Uint8Array.from([0xd8, 0x20, 0x62, 0x23, 0x61])); // 32("#a")
  assert.equal(reference, '#a');
  assert.deepEqual([...encode(reference)], [0x62, 0x23, 0x61]);
  // a chart reads a reference that arrived in event data as its text
  const logs = [];
  const { host } = native({ log: (record) => logs.push(record.text) });
  host.addImage(chart('reader', '<state id="s"><transition event="v"><log expr="typeof _event.data.to + _event.data.to"/></transition></state>'));
  const session = host.spawn('reader');
  host.send(session, 'v', decode(Uint8Array.from([0xa1, 0x62, 0x74, 0x6f, 0xd8, 0x20, 0x62, 0x23, 0x61]))); // {"to": 32("#a")}
  assert.deepEqual(logs, ['string#a']);
  host.close();
});

await run();
