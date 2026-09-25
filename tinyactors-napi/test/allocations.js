'use strict';

// After warm-up, delivering an event makes no allocator call in core, for
// the chart shapes that once did (napi roadmap "Core findings"): an executed
// <send>, to another actor, to #_internal or delayed with a <cancel>, and a
// chart with a top-level <final>. Each chart must also really send and raise
// no error, so an error path that happens to allocate nothing cannot pass.
// Runs on Node and Bun.

const assert = require('node:assert/strict');
const { createSystem, statechart } = require('..');

const ACTORS = 1000;
const ROUNDS = 5;

const charts = {
  'an assign': () =>
    statechart({ family: 'assign', revision: 'v1' })
      .data('n', 0)
      .state('idle', (s) => s.on('ping', (t) => t.assign('n', function () { return this.n + 1; }))),
  'a top-level <final>': () =>
    statechart({ family: 'final', revision: 'v1' })
      .data('n', 0)
      .state('idle', (s) => {
        s.on('ping', (t) => t.target('idle').assign('n', function () { return this.n + 1; }));
        s.on('stop', (t) => t.target('done'));
      })
      .final('done'),
  'a <send> to another actor': () =>
    statechart({ family: 'send', revision: 'v1' })
      .state('idle', (s) => s.on('ping', (t) => t.send('pong', (m) => m.to('#_actor_sink/main')))),
  'a <send> to #_internal': () =>
    statechart({ family: 'internal', revision: 'v1' })
      .state('idle', (s) => {
        s.on('ping', (t) => t.send('pong', (m) => m.to('#_internal')));
        s.on('pong', (t) => t);
      }),
  'a <raise>': () =>
    statechart({ family: 'raise', revision: 'v1' })
      .state('idle', (s) => {
        s.on('ping', (t) => t.raise('pong'));
        s.on('pong', (t) => t);
      }),
  'a delayed <send> re-armed with <cancel>': () =>
    statechart({ family: 'timeout', revision: 'v1' })
      .state('idle', (s) =>
        s.on('ping', (t) => t.cancel('idle').send('timeout', (m) => m.to('#_actor_sink/main').after(60_000).id('idle'))),
      ),
  'a timeout re-armed on entry and cancelled on exit': () =>
    statechart({ family: 'rearm', revision: 'v1' })
      .state('idle', (s) => {
        s.entry((a) => a.send('timeout', (send) => send.id('idle').after(30_000)));
        s.exit((a) => a.cancel('idle'));
        s.on('ping', (t) => t.target('idle'));
        s.on('timeout', (t) => t.target('done'));
      })
      .final('done'),
};

// The sum of a counter's points for one definition family.
function counted(system, name, family) {
  const metric = system.metrics().metrics.find((candidate) => candidate.name === name);
  return (metric?.points ?? [])
    .filter((point) => point.attributes['tinyactors.definition.family'] === family)
    .reduce((sum, point) => sum + (point.value ?? 0), 0);
}

async function steadyState(name, make) {
  const system = createSystem();
  const sink = system.define(statechart({ family: 'sink', revision: 'v1' }).state('idle', (s) => s.on('pong', (t) => t)));
  system.spawn(sink, { name: 'main' });
  const chart = make();
  const definition = system.define(chart);
  const family = definition.inspect().identity.family;
  const actors = [];
  for (let at = 0; at < ACTORS; at += 1) {
    actors.push(system.spawn(definition));
  }
  await system.flush();
  const round = async () => {
    for (const actor of actors) {
      system.post(actor, 'ping');
    }
    await system.flush();
  };
  await round(); // warm-up: queues, caches and timer storage reach their size
  await round();
  const before = system.stats().memory.core;
  for (let at = 0; at < ROUNDS; at += 1) {
    await round();
  }
  const after = system.stats().memory.core;
  assert.equal(after.allocations - before.allocations, 0, `${name}: core allocated after warm-up`);
  assert.equal(after.frees - before.frees, 0, `${name}: core freed after warm-up`);
  assert.equal(counted(system, 'tinyactors.errors', family), 0, `${name}: the chart raised errors`);
  if (/<send>|timeout/.test(name)) {
    const sends = counted(system, 'tinyactors.sends', family) + counted(system, 'tinyactors.sends.delayed', family);
    assert.ok(sends >= ACTORS * ROUNDS, `${name}: expected at least ${ACTORS * ROUNDS} sends, counted ${sends}`);
  }
  system.close();
}

(async () => {
  for (const [name, make] of Object.entries(charts)) {
    await steadyState(name, make);
  }
  console.log('allocations.js passed');
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
