'use strict';

// Persistence by replay, end to end (core ADRs 0088 to 0091, napi ADRs 0032
// to 0034): named actors whose write-ahead log an in-memory store keeps in
// two tables, a log of opaque bytes and a wake per address; evicted, woken,
// and brought back by a loader that answers { replay: { spawn, inputs } };
// crashes between commits; checkpoints and compaction; an upgrade by
// replay; a long log streamed from an async generator. Runs on Node and Bun.

const assert = require('node:assert/strict');
const { createSystem, statechart, readInputLog } = require('..');

// The store an application writes: an append-only log of bytes per address
// and a wake per address, stored as the batch says, never parsed. `frozen`
// drops every later commit, as a crash loses what was not committed yet.
// records() decodes a log with readInputLog(), as an audit trail would.
function memoryStore() {
  const logs = new Map();
  const wakes = new Map();
  const key = (address) => `${address.family}/${address.name}`;
  const store = {
    frozen: false,
    wakes,
    sink: {
      commit({ entries, wakes: changed }) {
        if (store.frozen) {
          return;
        }
        for (const { address, bytes } of entries) {
          if (!logs.has(key(address))) {
            logs.set(key(address), []);
          }
          logs.get(key(address)).push(bytes);
        }
        for (const { address, at } of changed) {
          if (at === null) {
            wakes.delete(key(address));
          } else {
            wakes.set(key(address), { address, at });
          }
        }
      },
    },
    read(address) {
      return logs.get(key(address)) ?? [];
    },
    async records(address) {
      const records = [];
      for await (const record of readInputLog(store.read(address))) {
        records.push(record);
      }
      return records;
    },
    // Compaction: the log becomes the checkpoint and what follows it.
    compact(address, checkpoint) {
      logs.set(key(address), [...checkpoint]);
    },
    // The whole loader: the address's records, handed back.
    loader(definitions) {
      return (address) => ({ replay: { spawn: definitions[address.family], inputs: store.read(address) } });
    },
    // At startup: every stored wake, armed again.
    wakeAll(system) {
      for (const { address, at } of wakes.values()) {
        system.wakeAt(address, at);
      }
    },
  };
  return store;
}

const ledger = () => statechart({ family: 'ledger', revision: 'v1' })
  .data('pings', 0)
  .data('hellos', 0)
  .data('sendID', null)
  .state('open', (s) => s
    .on('ping', (t) => t.assign('pings', ({ data }) => data.pings + 1))
    .on('hello', (t) => t
      .assign('hellos', ({ data }) => data.hellos + 1)
      .assign('sendID', ({ event }) => event.sendID ?? null)));

// An order: `add` counts, logs, pings the ledger, arms a reminder to itself
// and an expiry an operator may withdraw; a service's `result` is taken
// through <finalize>.
const order = () => statechart({ family: 'order', revision: 'v1' })
  .data('items', 0)
  .data('reminded', 0)
  .data('result', 0)
  .state('open', (s) => {
    s.invoke((i) => i.type('job').id('work')
      .finalize((f) => f.assign('result', ({ event }) => event.data.v)));
    s.on('add', (t) => t.assign('items', ({ data }) => data.items + 1)
      .log('added')
      .send('ping', (send) => send.to('#_actor_ledger/main'))
      .send('remind', (send) => send.id('reminder').after(1000))
      .send('expire', (send) => send.id('expiry').after(60000)));
    s.on('remind', (t) => t.assign('reminded', ({ data }) => data.reminded + 1));
    s.on('result', (t) => t.assign('items', ({ data }) => data.items));
  });

// A timeout of 5s to itself on `e1`.
const timer = () => statechart({ family: 'timer', revision: 'v1' })
  .state('idle', (s) => s.on('e1', (t) => t.target('waiting').send('timeout', (send) => send.after(5000))))
  .state('waiting', (s) => s.on('timeout', (t) => t.target('timedout')))
  .state('timedout', (s) => s.on('peek', (t) => t.target('timedout')));

function snapshot(inspection) {
  return {
    activeStates: inspection.activeStates,
    data: inspection.data,
    delayedSends: inspection.delayedSends.map((send) => ({ id: send.id, due: send.due, event: send.event })),
  };
}

function joined(chunks) {
  return Buffer.concat(chunks.map((chunk) => Buffer.from(chunk.buffer, chunk.byteOffset, chunk.length)));
}

const collect = typeof Bun !== 'undefined'
  ? () => Bun.gc(true)
  : (() => {
    require('node:v8').setFlagsFromString('--expose-gc');
    return require('node:vm').runInNewContext('gc');
  })();
function heap() {
  collect();
  return process.memoryUsage().heapUsed;
}

// Records in core's portable layout (core ADR 0090 section 3), written by
// hand: a 32-byte header, 8-byte fields (a number, or a text's offset and
// length), the text part, and value streams of chunks.
function record({ kind, seq, time, fields, value }) {
  const texts = [];
  let textLength = 0;
  const refs = fields.map((field) => {
    if (typeof field === 'bigint') {
      return { number: field };
    }
    if (field === undefined) {
      return { absent: true };
    }
    const bytes = Buffer.from(field, 'utf8');
    const ref = { offset: textLength, length: bytes.length };
    texts.push(bytes);
    textLength += bytes.length;
    return ref;
  });
  const pad = (n) => (8 - (n % 8)) % 8;
  const fixed = 32 + 8 * fields.length;
  const valueBytes = value === undefined ? undefined : Buffer.from(value, 'utf8');
  const valueLength = valueBytes === undefined ? 0 : 4 + valueBytes.length + pad(4 + valueBytes.length) + 8;
  const out = Buffer.alloc(fixed + textLength + pad(textLength) + valueLength);
  out.writeUInt16LE(1, 0);
  out.writeUInt16LE(kind, 2);
  out.writeUInt32LE(fixed, 4);
  out.writeBigUInt64LE(BigInt(seq), 8);
  out.writeBigUInt64LE(BigInt(time), 16);
  out.writeUInt32LE(textLength, 24);
  out.writeUInt16LE(valueBytes === undefined ? 0 : 1, 28);
  refs.forEach((ref, index) => {
    const at = 32 + 8 * index;
    if (ref.number !== undefined) {
      out.writeBigUInt64LE(ref.number, at);
    } else if (ref.absent) {
      out.writeUInt32LE(0xffffffff, at);
    } else {
      out.writeUInt32LE(ref.offset, at);
      out.writeUInt32LE(ref.length, at + 4);
    }
  });
  Buffer.concat(texts).copy(out, fixed);
  if (valueBytes !== undefined) {
    const at = fixed + textLength + pad(textLength);
    out.writeUInt32LE(valueBytes.length, at);
    valueBytes.copy(out, at + 4);
  }
  return out;
}

const spawnedRecord = (seq, time, family, name, revision) =>
  record({ kind: 1, seq, time, fields: [family, name, revision] });
const eventRecord = (seq, time, name, data) =>
  record({ kind: 2, seq, time, fields: [0n, 0n, name, undefined, undefined, undefined, undefined], value: data });

// Lets a loader's promise settle and the turns after it run.
async function settle(system) {
  for (let turn = 0; turn < 5; turn += 1) {
    await new Promise((resolve) => setTimeout(resolve, 2));
    await system.flush();
  }
}

// Live actors, their log committed, the order evicted, and mail for it
// summoning it through { replay }: the same state, the waiting mail after,
// and nothing the replay did left the actor or reached the log.
async function endToEnd() {
  const store = memoryStore();
  let reporter;
  let starts = 0;
  const logs = [];
  const system = createSystem({
    clock: 'manual',
    incarnation: 1,
    invokers: { job: { start(request, report) { starts += 1; reporter = report; } } },
  });
  system.setLogSink((record) => logs.push(record.label));
  system.setInputLog(store.sink, { families: ['order', 'ledger'] });
  const definitions = { order: system.define(order()), ledger: system.define(ledger()) };
  system.setLoader('order', store.loader(definitions));
  system.setLoader('ledger', store.loader(definitions));

  const book = system.spawn(definitions.ledger, { name: 'main' });
  const live = system.spawn(definitions.order, { name: 'o1' });
  await system.flush();
  await system.advanceTo(100);
  await live.send('add');
  await system.flush();
  reporter.event('result', { v: 7 });
  await system.flush();
  await system.advanceTo(1200); // the reminder fires
  await system.flush();
  system.withdrawSend(live, 'expiry');
  await live.send('add');
  await system.flush();
  const before = snapshot(live.inspect());
  assert.equal(before.data.items, 2);
  assert.equal(before.data.reminded, 1);
  assert.equal(before.data.result, 7);
  assert.deepEqual(before.delayedSends.map((send) => send.id).sort(), ['expiry', 'reminder']);
  assert.equal(book.inspect().data.pings, 2);
  assert.deepEqual(logs, ['added', 'added']);
  assert.equal(starts, 1);

  assert.equal(system.evict(live), 1200);
  await system.flush();
  const kinds = (await store.records({ family: 'order', name: 'o1' })).map((r) => r.kind);
  assert.equal(kinds[0], 'spawned');
  assert.equal(kinds.at(-1), 'evicted');
  assert.ok(kinds.includes('withdrawn'));
  assert.ok(kinds.includes('fired'), 'the reminder that fired');
  assert.equal(store.wakes.get('order/o1').at, 2200, 'woken for the reminder armed at 1200');
  const logged = (await store.records({ family: 'order', name: 'o1' })).length;

  await system.advanceTo(1500); // before the reminder is due
  const reply = await system.send({ family: 'order', name: 'o1' }, 'peek'); // summons it
  assert.equal(reply.status, 'stable');
  const back = system.findActor({ family: 'order', name: 'o1' });
  const after = snapshot(back.inspect());
  assert.deepEqual(after.activeStates, before.activeStates);
  assert.deepEqual(after.data, before.data);
  assert.deepEqual(after.delayedSends, before.delayedSends, 'the same pending sends, with their due times');
  assert.equal(book.inspect().data.pings, 2, 'the replay pinged nobody');
  assert.deepEqual(logs, ['added', 'added'], 'the replay wrote no log');
  assert.equal(starts, 1, 'the replay started no service');
  const newer = (await store.records({ family: 'order', name: 'o1' })).slice(logged).map((r) => r.kind);
  assert.deepEqual(newer, ['queued', 'event'], 'only the mail that summoned it is logged, once');

  await system.advanceTo(2200); // the reminder armed at 1200
  await system.flush();
  assert.equal(back.inspect().data.reminded, 2);
  system.close();
}

// An evicted actor's timeout fires on time: its wake brings it back through
// the loader when the timer is due, with no mail.
async function evictedTimeoutFiresOnTime() {
  const store = memoryStore();
  const system = createSystem({ clock: 'manual', incarnation: 1 });
  system.setInputLog(store.sink, { families: ['timer'] });
  const definitions = { timer: system.define(timer()) };
  system.setLoader('timer', store.loader(definitions));
  const live = system.spawn(definitions.timer, { name: 't' });
  await system.flush();
  await system.advanceTo(1000);
  await live.send('e1'); // the timeout is due at 6000
  await system.advanceTo(2000);
  system.evict(live);
  await system.flush();
  assert.equal(store.wakes.get('timer/t').at, 6000);
  assert.equal(system.findActor({ family: 'timer', name: 't' }), undefined);
  await system.advanceTo(6000);
  await settle(system);
  const back = system.findActor({ family: 'timer', name: 't' });
  assert.ok(back !== undefined, 'the wake brought it back');
  assert.deepEqual(back.inspect().activeStates, ['timedout'], 'and its timeout fired');
  system.close();
}

// The same after a restart: the first system dies with the actor resident;
// a new system, of another incarnation, arms every stored wake at startup.
async function timeoutAfterARestart() {
  const store = memoryStore();
  const first = createSystem({ clock: 'manual', incarnation: 1 });
  first.setInputLog(store.sink, { families: ['timer'] });
  const live = first.spawn(first.define(timer()), { name: 't' });
  await first.flush();
  await first.advanceTo(1000);
  await live.send('e1');
  await first.advanceTo(2000);
  first.close(); // the crash, the actor resident
  assert.equal(store.wakes.get('timer/t').at, 6000, 'its wake was committed while it lived');

  const second = createSystem({ clock: 'manual', incarnation: 2, initialTime: 2000 });
  second.setInputLog(store.sink, { families: ['timer'] });
  const definitions = { timer: second.define(timer()) };
  second.setLoader('timer', store.loader(definitions));
  store.wakeAll(second);
  await second.advanceTo(5999);
  await settle(second);
  assert.equal(second.findActor({ family: 'timer', name: 't' }), undefined, 'not before its wake');
  await second.advanceTo(6000);
  await settle(second);
  assert.deepEqual(second.findActor({ family: 'timer', name: 't' }).inspect().activeStates, ['timedout']);
  second.close();
}

// A crash between two commits, with mail left queued: the ledger's load
// never finished, and the sender's ping was dispatched after the last
// commit. A new system arms the stored wakes; each actor replays what was
// committed, the hello owed to the ledger is delivered once, and the lost
// ping is sent again, once.
async function crashWithMailInFlight() {
  const store = memoryStore();
  const first = createSystem({ clock: 'manual', incarnation: 1 });
  first.setInputLog(store.sink, { families: ['sender', 'ledger'] });
  const sender = () => statechart({ family: 'sender', revision: 'v1' })
    .state('s', (s) => s
      .on('arm', (t) => t.send('ping', (send) => send.to('#_actor_ledger/main').after(2000)))
      .on('peek', (t) => t.target('s')));
  const book = first.spawn(first.define(ledger()), { name: 'main' });
  const a = first.spawn(first.define(sender()), { name: 'a' });
  await first.flush();
  await first.advanceTo(1000);
  await a.send('arm'); // the ping is due at 3000
  first.evict(book);
  await first.flush();
  first.setLoader('ledger', () => new Promise(() => {})); // the ledger's load never finishes
  first.post({ family: 'ledger', name: 'main' }, 'hello'); // queued, waiting for the load
  await new Promise((resolve) => setTimeout(resolve, 20)); // a turn commits it
  assert.ok((await store.records({ family: 'ledger', name: 'main' })).some((r) => r.kind === 'queued'), 'the hello is queued');
  assert.ok(store.wakes.has('ledger/main'), 'and the ledger owes it');
  store.frozen = true; // the crash: nothing after this point is committed
  const moved = first.advanceTo(3000).catch(() => {});
  await new Promise((resolve) => setTimeout(resolve, 20));
  first.close();
  await moved;

  store.frozen = false;
  const second = createSystem({ clock: 'manual', incarnation: 2, initialTime: 5000 });
  second.setInputLog(store.sink, { families: ['sender', 'ledger'] });
  const definitions = { ledger: second.define(ledger()), sender: second.define(sender()) };
  second.setLoader('ledger', store.loader(definitions));
  second.setLoader('sender', store.loader(definitions));
  store.wakeAll(second);
  await second.advanceTo(5001);
  await settle(second);
  const main = second.findActor({ family: 'ledger', name: 'main' });
  assert.equal(main.inspect().data.hellos, 1, 'the queued hello is delivered once');
  assert.equal(main.inspect().data.pings, 1, 'the ping lost in the crash is sent again, once');
  second.close();
}

// A queued message that was withdrawn is `dropped`, and a replay after a
// crash does not deliver it; one that was not is delivered, once, with its
// literal send ID, which the new system has never seen.
async function droppedAndLiteralSendIDs() {
  const store = memoryStore();
  const first = createSystem({ clock: 'manual', incarnation: 1 });
  first.setInputLog(store.sink, { families: ['ledger', 'greeter'] });
  const greeter = () => statechart({ family: 'greeter', revision: 'v1' })
    .state('s', (s) => s.on('go', (t) => t.send('hello', (send) => send.to('#_actor_ledger/main').id('greeting'))));
  const book = first.spawn(first.define(ledger()), { name: 'main' });
  const g = first.spawn(first.define(greeter()), { name: 'g' });
  await first.flush();
  first.evict(book);
  await first.flush();
  first.setLoader('ledger', () => new Promise(() => {}));
  await g.send('go'); // queued for the ledger, with its send ID
  const alive = setInterval(() => {}, 1000); // a send()'s timeout does not keep the process alive
  await assert.rejects(first.send({ family: 'ledger', name: 'main' }, 'ping', undefined, { timeout: 5 }),
    (error) => error.code === 'timeout'); // queued, then withdrawn
  clearInterval(alive);
  await new Promise((resolve) => setTimeout(resolve, 20));
  const kinds = (await store.records({ family: 'ledger', name: 'main' })).map((r) => r.kind);
  assert.deepEqual(kinds.filter((kind) => kind === 'queued' || kind === 'dropped'), ['queued', 'queued', 'dropped']);
  first.close();

  const second = createSystem({ clock: 'manual', incarnation: 2 });
  second.setInputLog(store.sink, { families: ['ledger', 'greeter'] });
  const definitions = { ledger: second.define(ledger()) }; // not the greeter: 'greeting' is new here
  second.setLoader('ledger', store.loader(definitions));
  store.wakeAll(second);
  await second.advanceTo(1);
  await settle(second);
  const main = second.findActor({ family: 'ledger', name: 'main' });
  assert.equal(main.inspect().data.hellos, 1, 'the hello, once');
  assert.equal(main.inspect().data.sendID, 'greeting', 'with its literal send ID');
  assert.equal(main.inspect().data.pings, 0, 'the withdrawn ping is not delivered');
  second.close();
}

// An operator transition is replayed as the transition it was.
async function jumpReplayed() {
  const store = memoryStore();
  const system = createSystem({ clock: 'manual', incarnation: 1 });
  system.setInputLog(store.sink, { families: ['jumper'] });
  const definitions = {
    jumper: system.define(statechart({ family: 'jumper', revision: 'v1' })
      .state('a', (s) => s.on('peek', (t) => t.target('a')))
      .state('b', (s) => s.on('peek', (t) => t.target('b')))),
  };
  system.setLoader('jumper', store.loader(definitions));
  const live = system.spawn(definitions.jumper, { name: 'j' });
  await system.flush();
  system.post(live, 'operator.goto', undefined, { transition: { targets: ['b'] } });
  await system.flush();
  assert.deepEqual(live.inspect().activeStates, ['b']);
  system.evict(live);
  await system.send({ family: 'jumper', name: 'j' }, 'peek');
  assert.deepEqual(system.findActor({ family: 'jumper', name: 'j' }).inspect().activeStates, ['b']);
  system.close();
}

// A history that ends in a final state: the load ends `done`, and its mail
// becomes a dead letter with reason done.
async function finishedIsDone() {
  const store = memoryStore();
  const first = createSystem({ clock: 'manual', incarnation: 1 });
  first.setInputLog(store.sink, { families: ['task'] });
  const task = () => statechart({ family: 'task', revision: 'v1' })
    .state('open', (s) => s.on('close', (t) => t.target('closed')))
    .final('closed');
  const live = first.spawn(first.define(task()), { name: 't' });
  await live.send('close');
  await first.flush();
  first.close();

  const letters = [];
  const second = createSystem({ clock: 'manual', incarnation: 2, deadLetter: (letter) => letters.push(letter) });
  second.setLoader('task', store.loader({ task: second.define(task()) }));
  second.post({ family: 'task', name: 't' }, 'close');
  await settle(second);
  assert.equal(second.findActor({ family: 'task', name: 't' }), undefined);
  assert.deepEqual(letters.map((letter) => letter.reason), ['done'], JSON.stringify(letters.map((l) => l.detail)));
  second.close();
}

// destroy() of a logged actor records `destroyed`: its wake goes, from the
// store and from the process, so its old timeout never brings it back, and
// the next mail for its name starts a fresh actor, since the log's replay
// begins after the last `destroyed`.
async function destroyedFreesTheAddress() {
  const store = memoryStore();
  const system = createSystem({ clock: 'manual', incarnation: 1 });
  system.setInputLog(store.sink, { families: ['timer'] });
  const definitions = { timer: system.define(timer()) };
  system.setLoader('timer', store.loader(definitions));
  const address = { family: 'timer', name: 't' };
  const live = system.spawn(definitions.timer, { name: 't' });
  await system.flush();
  await system.advanceTo(1000);
  await live.send('e1'); // the timeout is due at 6000
  await system.advanceTo(2000);
  system.evict(live); // arms a wake at 6000 in the process
  await system.flush();
  await system.advanceTo(3000);
  system.post(address, 'nudge'); // brings it back early; the wake stays armed
  await settle(system);
  const back = system.findActor(address);
  assert.ok(back !== undefined);
  await system.advanceTo(4000);
  system.destroy(back);
  await system.flush();
  assert.equal(store.wakes.has('timer/t'), false, 'the stored wake went');
  assert.equal((await store.records(address)).at(-1).kind, 'destroyed');
  await system.advanceTo(6000);
  await settle(system);
  assert.equal(system.findActor(address), undefined, 'its old timeout did not bring it back');

  await system.advanceTo(7000);
  system.post(address, 'e1');
  await settle(system);
  const fresh = system.findActor(address);
  assert.ok(fresh !== undefined, 'the next mail started an actor');
  const view = fresh.inspect();
  assert.deepEqual(view.activeStates, ['waiting'], 'a fresh one: only the new e1 moved it');
  assert.deepEqual(view.delayedSends.map((send) => send.due), [12000]);
  const lives = await store.records(address);
  assert.ok(lives.findIndex((record) => record.kind === 'destroyed') < lives.length - 1,
    'the new life is logged after `destroyed`');
  system.close();

  // After a restart nothing wakes the old actor, and its log replays to the new one.
  const second = createSystem({ clock: 'manual', incarnation: 2, initialTime: 7000 });
  second.setInputLog(store.sink, { families: ['timer'] });
  second.setLoader('timer', store.loader({ timer: second.define(timer()) }));
  store.wakeAll(second);
  await second.advanceTo(12000);
  await settle(second);
  assert.deepEqual(second.findActor(address).inspect().activeStates, ['timedout'],
    'the fresh actor\'s own timeout, replayed from the records after `destroyed`');
  second.close();
}

// Outward effects are at most once across a crash: a send released only
// after its batch committed is lost if the process dies in between, and a
// replay does not repeat it, since its cause is committed.
async function effectsAreAtMostOnce() {
  const store = memoryStore();
  const sent = [];
  const io = { out: { send(request) { sent.push(request.event); } } };
  const emitter = () => statechart({ family: 'emitter', revision: 'v1' })
    .data('emitted', 0)
    .state('s', (s) => s
      .on('emit', (t) => t.assign('emitted', ({ data }) => data.emitted + 1).send('out-event', (send) => send.via('out').to('x')))
      .on('peek', (t) => t.target('s')));
  const first = createSystem({ clock: 'manual', incarnation: 1, io });
  first.setInputLog({ commit(batch) { store.sink.commit(batch); return new Promise(() => {}); } }, { families: ['emitter'] });
  const live = first.spawn(first.define(emitter()), { name: 'e' });
  live.post('emit');
  await first.flush();
  first.close(); // committed, and the send never left
  assert.deepEqual(sent, []);

  const second = createSystem({ clock: 'manual', incarnation: 2, io });
  second.setInputLog(store.sink, { families: ['emitter'] });
  second.setLoader('emitter', store.loader({ emitter: second.define(emitter()) }));
  await second.send({ family: 'emitter', name: 'e' }, 'peek');
  assert.equal(second.findActor({ family: 'emitter', name: 'e' }).inspect().data.emitted, 1, 'the emit is replayed');
  await second.flush();
  assert.deepEqual(sent, [], 'and its send is not repeated: at most once');
  second.close();
}

// An upgrade by replay: the live counter's log replayed into v2, which no
// longer takes `bump`, beside the incumbent, then published in its place.
async function upgrade() {
  const store = memoryStore();
  const system = createSystem({ clock: 'manual', incarnation: 1 });
  system.setInputLog(store.sink, { families: ['counter'] });
  const v1 = system.define(statechart({ family: 'counter', revision: 'v1' })
    .data('n', 0)
    .state('s', (s) => s
      .on('inc', (t) => t.assign('n', ({ data }) => data.n + 1))
      .on('bump', (t) => t.assign('n', ({ data }) => data.n + 10))));
  const v2 = system.define(statechart({ family: 'counter', revision: 'v2' })
    .data('n', 0)
    .state('s', (s) => s.on('inc', (t) => t.assign('n', ({ data }) => data.n + 2))));
  const live = system.spawn(v1, { name: 'c' });
  await system.flush();
  await system.advanceTo(10);
  await live.send('inc');
  await live.send('bump');
  await live.send('inc');
  await system.flush();
  assert.equal(live.inspect().data.n, 12);

  const records = await store.records({ family: 'counter', name: 'c' });
  // The whole log as the start: prepare takes its `spawned`, replay the rest.
  const next = system.prepare(v2, { incumbent: live, log: joined(store.read({ family: 'counter', name: 'c' })) });
  const report = await next.replay([]);
  assert.equal(report.inputs, records.length - 1);
  assert.deepEqual(report.ignored.map((r) => r.name), ['bump']);
  const published = system.publish(next); // at the last record's time
  assert.equal(published.inspect().data.n, 4, 'v2 reads the history: two incs, the bump ignored');
  assert.ok(system.findActor({ family: 'counter', name: 'c' }).equals(published));
  system.close();
}

// A report and an operator's actions are replayed too: an invocation that
// reported unavailable, and a runaway the operator abandoned and released.
async function reportsAndOperations() {
  const store = memoryStore();
  let reporter;
  const system = createSystem({
    clock: 'manual',
    incarnation: 1,
    parameters: { microstepLimit: 50 },
    invokers: { job: { start(request, report) { reporter = report; } } },
  });
  system.setInputLog(store.sink, { families: ['worker', 'spinner'] });
  const definitions = {
    worker: system.define(statechart({ family: 'worker', revision: 'v1' })
      .state('running', (s) => {
        s.invoke((i) => i.type('job').id('work'));
        s.on('error.communication.invoke.work.unavailable', (t) => t.target('lost'));
      })
      .state('lost', (s) => s.on('peek', (t) => t.target('lost')))),
    spinner: system.define(statechart({ family: 'spinner', revision: 'v1' })
      .data('n', 0)
      .state('idle', (s) => s.on('go', (t) => t.target('spin')))
      .state('spin', (s) => s
        .always((t) => t.when(({ data }) => data.n < 1000).assign('n', ({ data }) => data.n + 1).target('spin')))),
  };
  system.setLoader('worker', store.loader(definitions));
  system.setLoader('spinner', store.loader(definitions));

  const worker = system.spawn(definitions.worker, { name: 'w' });
  await system.flush();
  reporter.unavailable({ why: 'gone' });
  await system.flush();
  assert.deepEqual(worker.inspect().activeStates, ['lost']);
  system.evict(worker);

  const spinner = system.spawn(definitions.spinner, { name: 's' });
  await system.flush();
  spinner.post('go');
  await system.flush();
  assert.equal(system.inspect(spinner).scheduling, 'quarantined', 'a runaway');
  system.abandon(spinner);
  system.release(spinner);
  await system.flush();
  const before = spinner.inspect();
  system.evict(spinner);
  await system.flush();

  await system.send({ family: 'worker', name: 'w' }, 'peek');
  const back = system.findActor({ family: 'worker', name: 'w' });
  assert.deepEqual(back.inspect().activeStates, ['lost'], 'the report is replayed');
  // Any mail would start the spinner's eventless loop again, so it is
  // replayed by hand and inspected before it is published.
  const records = await store.records({ family: 'spinner', name: 's' });
  const chunks = store.read({ family: 'spinner', name: 's' });
  // The first chunk holds `spawned`; the rest streams in.
  const again = system.prepare(definitions.spinner, { address: { family: 'spinner', name: 's' }, log: chunks[0] });
  const report = await again.replay(chunks.slice(1));
  assert.equal(report.inputs, records.length - 1);
  assert.deepEqual(again.inspect().activeStates, before.activeStates);
  assert.equal(again.inspect().data.n, before.data.n, 'the runaway stops where the abandon stopped it');
  system.close();
}

// A long log streamed from an async generator, as a database cursor would
// hand it: replay holds one record and one chunk, so memory does not grow
// with the log. The log is written by hand in core's layout (core ADR 0090),
// which is what makes it portable.
async function streamsALongLog() {
  const count = 100000;
  const payload = 'x'.repeat(2048); // 100,000 records would be over 200 MB if held
  const system = createSystem({ clock: 'manual', incarnation: 1, initialTime: count });
  const definition = system.define(statechart({ family: 'counter', revision: 'v1' })
    .data('n', 0)
    .state('s', (s) => s.on('inc', (t) => t.assign('n', ({ data }) => data.n + 1))));
  const baseline = heap();
  let peak = 0;
  async function* cursor() {
    for (let index = 1; index <= count; index += 1) {
      if (index % 10000 === 0) {
        peak = Math.max(peak, heap() - baseline);
      }
      yield eventRecord(index + 1, index, 'inc', `{"payload":"${payload}${index}"}`);
    }
  }
  const prepared = system.prepare(definition, { address: { family: 'counter', name: 'long' },
                                                log: spawnedRecord(1, 0, 'counter', 'long', 'v1') });
  const report = await prepared.replay(cursor());
  assert.equal(report.inputs, count);
  assert.equal(prepared.inspect().data.n, count);
  assert.ok(peak < 40 * 1024 * 1024, `the log is not held: the heap grew by at most ${Math.round(peak / 1048576)} MB`);
  system.close();
}

// A 10 MB value is logged and replayed in chunks: the store gets it as bytes,
// and the replay's feed holds that one record, not a copy per chunk.
async function aLargeValue() {
  const store = memoryStore();
  const system = createSystem({ clock: 'manual', incarnation: 1 });
  system.setInputLog(store.sink, { families: ['box'] });
  const definition = system.define(statechart({ family: 'box', revision: 'v1' })
    .data('size', 0)
    .state('s', (s) => s.on('put', (t) => t.assign('size', ({ event }) => event.data.length))));
  system.setLoader('box', store.loader({ box: definition }));
  const live = system.spawn(definition, { name: 'b' });
  await system.flush();
  const value = 'v'.repeat(10 * 1024 * 1024);
  await live.send('put', value);
  await system.flush();
  system.evict(live);
  await system.flush();
  const bytes = store.read({ family: 'box', name: 'b' }).reduce((total, chunk) => total + chunk.length, 0);
  assert.ok(bytes > 2 * value.length, `logged as \`queued\` and \`event\`: ${bytes} bytes`);
  // Hand the log back in 64 KiB pieces.
  const pieces = function* () {
    for (const chunk of store.read({ family: 'box', name: 'b' })) {
      for (let at = 0; at < chunk.length; at += 65536) {
        yield chunk.subarray(at, at + 65536);
      }
    }
  };
  const before = heap();
  const rssBefore = process.memoryUsage().rss;
  const again = system.prepare(definition, { address: { family: 'box', name: 'c' }, log: pieces().next().value });
  // (The first piece holds `spawned`; replaying the rest into another address.)
  const rest = pieces();
  rest.next();
  const report = await again.replay(rest);
  // The durable reader scans each piece once (napi ADR 0034's notes): the
  // process grows by about the value, not by copies of it. Before, Bun kept
  // 339 MB and Node peaked at 200 MB.
  const rssGrew = process.memoryUsage().rss - rssBefore;
  assert.ok(rssGrew < 120 * 1024 * 1024, `replaying a 10 MB value grew RSS by ${Math.round(rssGrew / 1048576)} MB`);
  const grew = heap() - before;
  assert.equal(again.inspect().data.size, value.length);
  assert.ok(report.inputs >= 2);
  // What stays is the value the replayed actor holds, about 10 MB.
  assert.ok(grew < 40 * 1024 * 1024, `replaying a 10 MB value grew the heap by ${Math.round(grew / 1048576)} MB`);
  system.close();
}

// A checkpoint rebuilds the actor, a log compacted to it keeps the owed
// mail, and a checkpoint loads only under its own revision.
async function checkpointsAndCompaction() {
  const store = memoryStore();
  const first = createSystem({ clock: 'manual', incarnation: 1 });
  first.setInputLog(store.sink, { families: ['counter'] });
  const v1 = () => statechart({ family: 'counter', revision: 'v1' })
    .data('n', 0)
    .state('s', (s) => s
      .on('inc', (t) => t.assign('n', ({ data }) => data.n + 1).send('tick', (send) => send.id('tick').after(1000)))
      .on('tick', (t) => t.assign('n', ({ data }) => data.n + 100)));
  const definition = first.define(v1());
  const address = { family: 'counter', name: 'c' };
  const live = first.spawn(definition, { name: 'c' });
  await first.flush();
  for (let index = 0; index < 5; index += 1) {
    await live.send('inc');
  }
  await first.flush();
  const before = snapshot(live.inspect());
  assert.equal(before.data.n, 5);
  const checkpoint = [];
  first.checkpoint(live, (chunk) => checkpoint.push(Buffer.from(chunk)));
  assert.ok(checkpoint.length > 0);
  const [head] = await (async () => {
    const records = [];
    for await (const record of readInputLog(checkpoint)) {
      records.push(record);
    }
    return records;
  })();
  assert.equal(head.kind, 'checkpoint');
  assert.equal(head.data.n, 5, 'the data root travels in it');
  // A message queued and not yet delivered travels with the checkpoint.
  first.evict(live);
  await first.flush();
  first.setLoader('counter', () => new Promise(() => {}));
  first.post(address, 'inc'); // waits for the load, logged `queued`
  await new Promise((resolve) => setTimeout(resolve, 20));
  store.compact(address, [...checkpoint, ...store.read(address).slice(-1)]);
  first.close();

  const second = createSystem({ clock: 'manual', incarnation: 2 });
  second.setInputLog(store.sink, { families: ['counter'] });
  const again = second.define(v1());
  second.setLoader('counter', store.loader({ counter: again }));
  store.wakeAll(second);
  await second.advanceTo(1);
  await settle(second);
  const back = second.findActor(address);
  assert.equal(back.inspect().data.n, 6, 'the checkpoint\'s five, and the owed inc');
  assert.deepEqual(back.inspect().delayedSends.map((send) => send.id), new Array(6).fill('tick'),
    'the checkpoint\'s five pending ticks came back, and the owed inc armed a sixth');
  // Another revision cannot start from the checkpoint.
  const v2 = second.define(statechart({ family: 'counter', revision: 'v2' }).state('s'));
  assert.throws(() => second.prepare(v2, { address: { family: 'counter', name: 'd' }, log: joined(checkpoint) }),
    (error) => error.code === 'definition-mismatch');
  // Only an idle, named, logged actor has one.
  const unnamed = second.spawn(again);
  await second.flush();
  assert.throws(() => second.checkpoint(unnamed, () => {}), (error) => error.code === 'invalid');
  second.close();
}

// An input to a logged actor whose data has no durable form is refused.
async function nonDurableRefused() {
  const system = createSystem({ clock: 'manual' });
  system.setInputLog({ commit() {} }, { families: ['ledger'] });
  const book = system.spawn(system.define(ledger()), { name: 'main' });
  await system.flush();
  assert.throws(() => book.post('ping', { f() {} }), (error) => error.code === 'unsupported');
  assert.throws(() => book.post('ping', { self: null, get loop() { return this; } }), (error) => error.code === 'unsupported');
  book.post('ping', { ok: [1, 2, new Date(0)] });
  await system.flush();
  assert.equal(book.inspect().data.pings, 1);
  system.close();
}

// replay() and evict() refuse what they cannot do.
async function refusals() {
  const system = createSystem({ clock: 'manual' });
  const definition = system.define(ledger());
  const plain = system.prepare(definition, { address: { family: 'ledger', name: 'x' } });
  await assert.rejects(plain.replay([]), (error) => error.code === 'invalid');
  const replaying = system.prepare(definition, { address: { family: 'ledger', name: 'y' }, at: 0 });
  await assert.rejects(replaying.replay('abc'), (error) => error.code === 'invalid');
  await assert.rejects(replaying.replay([{ kind: 'event' }]), (error) => error.code === 'invalid');
  await assert.rejects(replaying.replay([new Uint8Array(40)]), (error) => error.code === 'corrupt');
  assert.throws(() => system.prepare(definition, { address: { family: 'ledger', name: 'z' }, log: [1] }),
    (error) => error.code === 'invalid');
  assert.throws(() => system.prepare(definition, { address: { family: 'ledger', name: 'z' }, log: new Uint8Array(8) }),
    (error) => error.code === 'invalid');
  const unnamed = system.spawn(definition);
  await system.flush();
  assert.throws(() => system.evict(unnamed), (error) => error.code === 'invalid');
  system.close();
}

(async () => {
  await endToEnd();
  await evictedTimeoutFiresOnTime();
  await timeoutAfterARestart();
  await crashWithMailInFlight();
  await droppedAndLiteralSendIDs();
  await jumpReplayed();
  await finishedIsDone();
  await destroyedFreesTheAddress();
  await effectsAreAtMostOnce();
  await upgrade();
  await reportsAndOperations();
  await streamsALongLog();
  await aLargeValue();
  await checkpointsAndCompaction();
  await nonDurableRefused();
  await refusals();
  console.log('replay.js passed');
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
