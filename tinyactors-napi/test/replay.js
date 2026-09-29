'use strict';

// Persistence by replay, end to end (core ADRs 0088 and 0089, napi ADR 0032
// sections 6 to 8): named actors whose input log an in-memory store keeps,
// evicted, and brought back by a loader that answers { replay }; an
// upgrade by replay into a new revision; the timelines of core ADR 0089
// section 2, a crash between two commits included. Runs on Node and Bun.

const assert = require('node:assert/strict');
const { createSystem, statechart } = require('..');

// An input log in memory: each committed record under its address, and the
// latest committed horizon. `frozen` drops every later commit, as a crash
// loses what was not committed yet.
function memoryStore() {
  const logs = new Map();
  const store = {
    horizon: 0,
    frozen: false,
    sink: {
      commit(batch) {
        if (store.frozen) {
          return;
        }
        store.horizon = batch.horizon;
        for (const input of batch.inputs) {
          const key = `${input.address.family}/${input.address.name}`;
          if (!logs.has(key)) {
            logs.set(key, []);
          }
          logs.get(key).push(input);
        }
      },
    },
    of(family, name) {
      return logs.get(`${family}/${name}`) ?? [];
    },
    // What a crash left queued: `queued` records with no `event` of their seq.
    undelivered() {
      const records = [...logs.values()].flat();
      const delivered = new Set(records.filter((r) => r.kind === 'event' && r.seq !== undefined).map((r) => r.seq));
      return records.filter((r) => r.kind === 'queued' && !delivered.has(r.seq));
    },
    // A loader answering from the log: nothing logged is not-found.
    loader(definitions) {
      return (address) => {
        const log = store.of(address.family, address.name);
        if (log.length === 0) {
          return 'not-found';
        }
        const evicted = log.filter((r) => r.kind === 'evicted').at(-1);
        return {
          replay: {
            spawn: definitions[address.family],
            binding: log[0].binding,
            inputs: log,
            stoppedAt: evicted !== undefined ? evicted.time : store.horizon,
            onReport: (report) => store.reports.push({ address, report }),
          },
        };
      };
    },
    reports: [],
  };
  return store;
}

const ledger = () => statechart({ family: 'ledger', revision: 'v1' })
  .data('pings', 0)
  .data('hellos', 0)
  .state('open', (s) => s
    .on('ping', (t) => t.assign('pings', ({ data }) => data.pings + 1))
    .on('hello', (t) => t.assign('hellos', ({ data }) => data.hellos + 1)));

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

function snapshot(inspection) {
  return {
    activeStates: inspection.activeStates,
    data: inspection.data,
    delayedSends: inspection.delayedSends.map((send) => ({ id: send.id, due: send.due, event: send.event })),
  };
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

  const stopped = system.evict(live);
  await system.flush();
  const kinds = store.of('order', 'o1').map((r) => r.kind);
  assert.equal(kinds[0], 'spawned');
  assert.equal(kinds.at(-1), 'evicted');
  assert.ok(kinds.includes('withdrawn'));
  const logged = store.of('order', 'o1').length;

  await system.advanceTo(1500); // before the reminder armed at 1200 is due
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
  assert.equal(stopped, 1200);
  const newer = store.of('order', 'o1').slice(logged).map((r) => r.kind);
  assert.deepEqual(newer.filter((kind) => kind !== 'queued' && kind !== 'event'), [], 'the replay logged nothing');
  assert.ok(newer.includes('event'), 'the mail that summoned it is logged');
  assert.equal(store.reports.length, 1);
  assert.equal(store.reports[0].report.ignored.length, 0);

  await system.advanceTo(2200); // the reminder armed at 1200
  await system.flush();
  assert.equal(back.inspect().data.reminded, 2);
  system.close();
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

  const log = store.of('counter', 'c');
  const next = system.prepare(v2, { incumbent: live, binding: log[0].binding, at: log[0].time });
  const report = await next.replay(log);
  assert.equal(report.inputs, log.length);
  assert.deepEqual(report.ignored.map((r) => r.name), ['bump']);
  const published = system.publish(next, { at: system.time });
  assert.equal(published.inspect().data.n, 4, 'v2 reads the history: two incs, the bump ignored');
  assert.ok(system.findActor({ family: 'counter', name: 'c' }).equals(published));
  system.close();
}

// A timeout to itself (core ADR 0089 section 2, first timeline): it fired
// live, and its delivery is in the log.
async function timeoutToSelf() {
  const store = memoryStore();
  const system = createSystem({ clock: 'manual', incarnation: 1 });
  system.setInputLog(store.sink, { families: ['timer'] });
  const definition = system.define(statechart({ family: 'timer', revision: 'v1' })
    .state('idle', (s) => s.on('e1', (t) => t.target('waiting').send('timeout', (send) => send.after(5000))))
    .state('waiting', (s) => s.on('timeout', (t) => t.target('timedout')))
    .state('timedout', (s) => s.on('peek', (t) => t.target('timedout'))));
  system.setLoader('timer', store.loader({ timer: definition }));
  const live = system.spawn(definition, { name: 't' });
  await system.flush();
  await system.advanceTo(1000);
  await live.send('e1');
  await system.advanceTo(6000);
  await system.flush();
  system.evict(live);
  await system.flush();
  await system.advanceTo(6000 + 7 * 24 * 3600 * 1000);
  await system.send({ family: 'timer', name: 't' }, 'peek');
  const back = system.findActor({ family: 'timer', name: 't' });
  assert.deepEqual(back.inspect().activeStates, ['timedout']);
  assert.equal(back.inspect().delayedSends.length, 0, 'nothing fires twice');
  system.close();
}

// Sends to another actor (second and third timelines): one fired before the
// actor stopped and is not sent again; one still pending when it stopped
// fires once, late, at publish.
async function sendsToAnother() {
  const store = memoryStore();
  const system = createSystem({ clock: 'manual', incarnation: 1 });
  system.setInputLog(store.sink, { families: ['sender', 'ledger'] });
  const definitions = {
    ledger: system.define(ledger()),
    sender: system.define(statechart({ family: 'sender', revision: 'v1' })
      .state('s', (s) => s
        .on('arm', (t) => t.send('ping', (send) => send.to('#_actor_ledger/main').after(2000)))
        .on('peek', (t) => t.target('s')))),
  };
  system.setLoader('sender', store.loader(definitions));
  const book = system.spawn(definitions.ledger, { name: 'main' });
  const fired = system.spawn(definitions.sender, { name: 'fired' });
  const pending = system.spawn(definitions.sender, { name: 'pending' });
  await system.flush();
  await system.advanceTo(1000);
  await fired.send('arm');
  await pending.send('arm');
  await system.advanceTo(1500);
  await system.flush();
  system.evict(pending); // stopped at 1500, its ping due at 3000
  await system.advanceTo(4000);
  await system.flush();
  system.evict(fired); // stopped at 4000, its ping fired at 3000
  await system.flush();
  assert.equal(book.inspect().data.pings, 1);

  await system.advanceTo(10000);
  await system.send({ family: 'sender', name: 'fired' }, 'peek');
  await system.flush();
  assert.equal(book.inspect().data.pings, 1, 'the ping that fired is not sent again');
  await system.send({ family: 'sender', name: 'pending' }, 'peek');
  await system.flush();
  assert.equal(book.inspect().data.pings, 2, 'the pending ping fires once, late');
  system.close();
}

// A crash between two commits (fourth timeline), with mail left queued: a
// new system, another incarnation, requeues what was never delivered and
// replays both actors from what was committed. The ledger takes the hello
// and the ping once each.
async function crash() {
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
  assert.ok(store.undelivered().some((r) => r.name === 'hello'), 'the hello is committed as queued');
  store.frozen = true; // the crash: nothing after this point is committed
  // advanceTo() waits for a quiet system, which the hanging load never
  // allows: the ping is dispatched meanwhile, and lost with the process.
  const moved = first.advanceTo(3000).catch(() => {});
  await new Promise((resolve) => setTimeout(resolve, 20));
  first.close();
  await moved;

  const second = createSystem({ clock: 'manual', incarnation: 2 });
  await second.advanceTo(5000);
  store.frozen = false;
  second.setInputLog(store.sink, { families: ['sender', 'ledger'] });
  const definitions = { ledger: second.define(ledger()), sender: second.define(sender()) };
  second.setLoader('ledger', store.loader(definitions));
  second.setLoader('sender', store.loader(definitions));
  second.requeue(store.undelivered());
  await second.send({ family: 'sender', name: 'a' }, 'peek'); // replayed; its ping, due at 3000, fires late
  await second.flush();
  const main = second.findActor({ family: 'ledger', name: 'main' });
  assert.equal(main.inspect().data.hellos, 1, 'the queued hello is delivered once');
  assert.equal(main.inspect().data.pings, 1, 'the ping lost in the crash is sent again, once');
  second.close();
}

// replay() refuses what it cannot replay.
async function refusals() {
  const system = createSystem({ clock: 'manual' });
  const definition = system.define(ledger());
  const plain = system.prepare(definition, { address: { family: 'ledger', name: 'x' } });
  await assert.rejects(plain.replay([]), (error) => error.code === 'invalid');
  const replaying = system.prepare(definition, { address: { family: 'ledger', name: 'y' }, at: 0 });
  await assert.rejects(replaying.replay('abc'), (error) => error.code === 'invalid');
  await assert.rejects(replaying.replay([{ kind: 'event' }]), (error) => error.code === 'invalid');
  await assert.rejects(replaying.replay([{ kind: 'mystery', time: 0 }]), (error) => error.code === 'invalid');
  const unnamed = system.spawn(definition);
  await system.flush();
  assert.throws(() => system.evict(unnamed), (error) => error.code === 'invalid');
  system.close();
}

(async () => {
  await endToEnd();
  await upgrade();
  await timeoutToSelf();
  await sendsToAnother();
  await crash();
  await refusals();
  console.log('replay.js passed');
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
