'use strict';

// Event data leaving a session is a copy (core ADR 0097, napi ADR 0039,
// SCXML C.1): a <send> to another actor, to the sender itself or to an I/O
// processor, an invocation's params, and a child's #_parent send. What
// stays in the session (raise(), #_internal) and mail from JavaScript is not
// copied. Runs on Node and Bun.

const assert = require('node:assert/strict');
const { createSystem, statechart, actorURI, serialize } = require('..');
const { ecmascript } = require('../tools/ecmascript-datamodel');

const NS = 'xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript"';

// A sender keeps `box` and sends it to a receiver, then changes it; the
// receiver keeps what it got and changes that. Neither sees the other's change.
async function betweenActors() {
  const system = createSystem();
  const receiver = system.define(statechart({ family: 'copy-receiver', revision: 'v1' })
    .data('got', null)
    .state('idle', (s) => s.on('box', (t) => t
      .assign('got', ({ event }) => event.data)
      .script(function () { this.got.n = 3; this.got.list.push('receiver'); }))));
  const sender = system.define(statechart({ family: 'copy-sender', revision: 'v1' })
    .data('box', null)
    .state('idle', (s) => s.on('go', (t) => t
      .assign('box', () => ({ n: 1, list: ['sender'], when: new Date(5), tags: new Map([['a', { x: 1 }]]) }))
      .send('box', (m) => m.to(actorURI({ family: 'copy-receiver', name: 'r' })).data(function () { return this.box; }))
      .script(function () { this.box.n = 2; this.box.tags.get('a').x = 2; }))));
  const r = system.spawn(receiver, { name: 'r' });
  const s = system.spawn(sender, { name: 's' });
  s.send('go');
  await system.flush();
  const got = r.inspect().data.got;
  const box = s.inspect().data.box;
  assert.equal(got.n, 3);
  assert.deepEqual(got.list, ['sender', 'receiver']);
  assert.equal(got.when.getTime(), 5);
  assert.notEqual(got.when, box.when, 'a Date is copied');
  assert.equal(got.tags.get('a').x, 1, 'a Map and what it holds are copied');
  assert.equal(box.n, 2, 'the receiver changing its copy leaves the sender as it was');
  assert.deepEqual(box.list, ['sender']);
  system.close();
}

// A send to self is a copy taken when the send runs (SCXML 6.2); raise()
// stays in the session and shares.
async function toSelfAndRaise() {
  const system = createSystem();
  const chart = statechart({ family: 'copy-self', revision: 'v1' })
    .data('box', null)
    .data('sent', null)
    .data('raised', null)
    .state('idle', (s) => s
      .on('go', (t) => t
        .assign('box', () => ({ n: 1 }))
        .send('sent')
        .send('sent', (m) => m.data(function () { return this.box; }))
        .raise('raised', function () { return this.box; })
        .script(function () { this.box.n = 2; }))
      .on('raised', (t) => t.assign('raised', ({ event }) => event.data))
      .on('sent', (t) => t.assign('sent', ({ event }) => event.data ?? null)));
  const actor = system.spawn(system.define(chart));
  actor.send('go');
  await system.flush();
  const { box, sent, raised } = actor.inspect().data;
  assert.deepEqual(sent, { n: 1 });
  assert.equal(raised, box, 'a raised event shares its data');
  assert.equal(box.n, 2);
  system.close();
}

// What cannot be copied is shared: functions, class instances that do not
// describe themselves, and those whose class is not registered. A value that
// describes itself is rebuilt with its registered class; a copy that throws
// (a getter) is error.execution, and nothing is sent.
async function whatIsCopied() {
  class Money {
    constructor(amount, currency) { this.amount = amount; this.currency = currency; }
    [serialize]() { return { name: 'Money', arguments: [this.amount, this.currency] }; }
  }
  class Opaque { constructor() { this.n = 1; } }
  const reply = () => 'reply';
  const stranger = new (class Stranger {
    [serialize]() { return { name: 'Stranger', arguments: [] }; }
  })();
  const opaque = new Opaque();
  const received = [];
  const system = createSystem({ io: { effects: { send(request) { received.push(request.data); } } } });
  system.registerClass('Money', Money);
  const chart = statechart({ family: 'copy-kinds', revision: 'v1' })
    .data('price', null)
    .data('failed', null)
    .state('idle', (s) => s
      .on('go', (t) => t
        .assign('price', () => new Money(5, 'EUR'))
        .send('charge', (m) => m.via('effects').data(function () {
          return { price: this.price, reply, opaque, cyclic: this.cyclic, bytes: Buffer.from('hi') };
        }))
        .script(function () { this.price.amount = 6; }))
      .on('unregistered', (t) => t
        .send('charge', (m) => m.via('effects').data(() => ({ stranger }))))
      .on('throwing', (t) => t
        .send('charge', (m) => m.via('effects').data(() => ({ get bad() { throw new Error('no copy'); } }))))
      .on('error.execution', (t) => t.assign('failed', ({ event }) => String(event.data?.message ?? event.data))));
  const actor = system.spawn(system.define(chart), { binding: { cyclic: undefined } });
  actor.send('go');
  await system.flush();
  assert.equal(received.length, 1);
  const [data] = received;
  assert.ok(data.price instanceof Money, 'a described value is rebuilt with its class');
  assert.equal(data.price.amount, 5);
  assert.equal(actor.inspect().data.price.amount, 6);
  assert.equal(data.reply, reply, 'a function is shared');
  assert.equal(data.opaque, opaque, 'a class instance without [serialize]() is shared');
  assert.ok(Buffer.isBuffer(data.bytes) && data.bytes.toString() === 'hi');
  actor.send('unregistered');
  await system.flush();
  assert.equal(received.length, 2);
  assert.equal(received[1].stranger, stranger, 'a value whose class is not registered is shared');
  actor.send('throwing');
  await system.flush();
  assert.equal(received.length, 2, 'data whose copy throws is not sent');
  assert.match(actor.inspect().data.failed, /no copy/);
  system.close();
}

// Shared references and cycles survive the copy.
async function sharedAndCyclic() {
  const received = [];
  const system = createSystem({ io: { effects: { send(request) { received.push(request.data); } } } });
  const chart = statechart({ family: 'copy-cycles', revision: 'v1' })
    .state('idle', (s) => s.on('go', (t) => t.send('graph', (m) => m.via('effects').data(() => {
      const leaf = { n: 1 };
      const root = { a: leaf, b: leaf, list: [leaf] };
      root.self = root;
      return root;
    }))));
  const actor = system.spawn(system.define(chart));
  actor.send('go');
  await system.flush();
  const [root] = received;
  assert.equal(root.self, root);
  assert.equal(root.a, root.b);
  assert.equal(root.list[0], root.a);
  system.close();
}

// Mail from JavaScript is handed over as it is: the application owns it.
async function mailIsNotCopied() {
  const system = createSystem();
  const chart = statechart({ family: 'copy-mail', revision: 'v1' })
    .data('got', null)
    .state('idle', (s) => s.on('mail', (t) => t.assign('got', ({ event }) => event.data)));
  const actor = system.spawn(system.define(chart));
  const mail = { n: 1 };
  actor.send('mail', mail);
  await system.flush();
  assert.equal(actor.inspect().data.got, mail);
  system.close();
}

// An invoked child gets copies of its params, and its #_parent sends are
// copies too (the ECMAScript example data model, napi ADR 0019).
async function parentAndChild() {
  const system = createSystem();
  system.registerDatamodel('ecmascript', ecmascript);
  const parent = system.define(`<scxml ${NS} name="copy-parent" initial="s0">
    <datamodel>
      <data id="sent" expr="({n: 1})"/>
      <data id="report" expr="null"/>
    </datamodel>
    <state id="s0">
      <invoke id="kid" type="scxml">
        <param name="shared" expr="sent"/>
        <content>
          <scxml ${NS} name="copy-child" initial="k0">
            <datamodel><data id="shared"/></datamodel>
            <state id="k0">
              <onentry>
                <assign location="shared.n" expr="shared.n + 10"/>
                <send event="report" target="#_parent"><param name="shared" expr="shared"/></send>
                <assign location="shared.n" expr="100"/>
              </onentry>
            </state>
          </scxml>
        </content>
      </invoke>
      <transition event="report" target="reported"><assign location="report" expr="_event.data.shared"/></transition>
    </state>
    <state id="reported"/>
  </scxml>`, { format: 'scxml' });
  const actor = system.spawn(parent);
  await system.flush();
  const { sent, report } = actor.inspect().data;
  assert.equal(sent.n, 1, 'the child changing its params leaves the parent as it was');
  assert.equal(report.n, 11, 'the child changing what it sent leaves the copy the parent got');
  system.close();
}

(async () => {
  await betweenActors();
  await toSelfAndRaise();
  await whatIsCopied();
  await sharedAndCyclic();
  await mailIsNotCopied();
  await parentAndChild();
  console.log('copy: ok');
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
