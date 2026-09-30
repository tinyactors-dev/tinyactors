'use strict';

// Bench (not part of `mise run test`): what an actor-to-actor send costs
// with event data, which leaves the sending session as a copy (core ADR
// 0097, napi ADR 0039). Runs on Node and Bun.
//
//   node --expose-gc test/bench/send_data.js [actors] [--variants=a,b,...]
//        [--exclusive] [--json=<file>] [--list]
//
// The options are test/bench/tracing.js's, and `mise run bench` and `mise
// run bench-compare` run it, each variant in a process of its own. The
// first variant, `no data`, is the baseline and always runs unless
// --exclusive is given.
//
// Named actors (default 10,000) in a ring: each round posts 10 `go` to
// every actor and flushes once; each `go` sends one `hit` to the next actor
// by its address, with:
// - no data: none;
// - number: the number 1, which rides inline and is shared;
// - object: a new `{ by: 1 }`, copied once as it leaves;
// - nested object: a new `{ by: 1, meta: { tags: ['a', 'b'], at: 5 } }`,
//   three containers copied.
// An event is one send: its `go` post, the sender's macrostep, the send and
// the receiver's macrostep. Memory is measured after the actors are spawned
// and a GC ran, as in tracing.js.

const { createSystem, statechart } = require('../..');

const args = process.argv.slice(2);
const population = Number(args.find((arg) => !arg.startsWith('--')) ?? 10000);
const exclusive = args.includes('--exclusive');
const jsonPath = args.find((arg) => arg.startsWith('--json='))?.slice('--json='.length);
const only = args.find((arg) => arg.startsWith('--variants='))?.slice('--variants='.length).split(',');
const rounds = 5;
const MESSAGES = 10;
const FAMILY = 'bench-send';

const chart = (data) => statechart({ family: FAMILY, revision: 'v1' })
  .data('count', 0)
  .data('next', '')
  .state('idle', (s) => s
    .on('go', (t) => t.send('hit', (m) => {
      m.to(function () { return this.next; });
      if (data !== undefined) m.data(data);
    }))
    .on('hit', (t) => t.assign('count', function () { return this.count + 1; })));

const collect = globalThis.gc ?? (typeof Bun !== 'undefined' ? () => Bun.gc(true) : undefined);

function settle() {
  collect?.();
  const { rss, heapUsed } = process.memoryUsage();
  return { rss, heapUsed };
}

async function measure({ data }) {
  const before = settle();
  const system = createSystem();
  const definition = system.define(chart(data));
  const actors = [];
  for (let at = 0; at < population; at += 1) {
    actors.push(system.spawn(definition, {
      name: `a${at}`,
      binding: { count: 0, next: `#_actor_${FAMILY}/a${(at + 1) % population}` },
    }));
  }
  await system.flush();
  const after = settle();
  const stats = system.stats();
  const native = {};
  for (const [category, counters] of Object.entries(stats.memory)) {
    native[category] = counters.bytesInUse / population;
  }
  const memory = {
    coreBytesPerActor: stats.memory.core.bytesInUse / population,
    nativeBytesPerActor: native,
    rssBytesPerActor: (after.rss - before.rss) / population,
    heapBytesPerActor: (after.heapUsed - before.heapUsed) / population,
  };
  const callsBefore = stats.allocatorCalls;
  const events = population * MESSAGES;
  const rates = [];
  for (let round = 0; round < rounds; round += 1) {
    const start = process.hrtime.bigint();
    for (let message = 0; message < MESSAGES; message += 1) {
      for (const actor of actors) system.post(actor, 'go');
    }
    await system.flush();
    const seconds = Number(process.hrtime.bigint() - start) / 1e9;
    rates.push(events / seconds);
  }
  memory.allocatorCallsPerEvent = (system.stats().allocatorCalls - callsBefore) / (events * rounds);
  const hits = actors.reduce((sum, actor) => sum + actor.inspect().data.count, 0);
  if (hits !== events * rounds) {
    throw new Error(`expected ${events * rounds} hits, counted ${hits}`);
  }
  system.close();
  memory.peakRSSBytes = process.resourceUsage().maxRSS * 1024;
  rates.sort((a, b) => a - b);
  return { memory, median: rates[Math.floor(rounds / 2)], best: rates.at(-1) };
}

const variants = {
  'no data': { data: undefined },
  number: { data: () => 1 },
  object: { data: () => ({ by: 1 }) },
  'nested object': { data: () => ({ by: 1, meta: { tags: ['a', 'b'], at: 5 } }) },
};

if (args.includes('--list')) {
  console.log(JSON.stringify(Object.keys(variants)));
  process.exit(0);
}

(async () => {
  const runtime = typeof Bun === 'undefined' ? `node ${process.version}` : `bun ${Bun.version}`;
  console.log(`${runtime}, ${population} actors x ${MESSAGES} sends, ${rounds} rounds`);
  console.log('variant          events/s (median)   events/s (best)   ns/event');
  const records = [];
  for (const [name, variant] of Object.entries(variants)) {
    if (only && (exclusive || name !== 'no data') && !only.includes(name)) {
      continue;
    }
    let result;
    try {
      result = await measure(variant);
    } catch (error) {
      // An older build (bench-compare) may lack what a variant uses.
      records.push({ variant: name, population, rounds, error: String(error?.message ?? error) });
      console.log(`${name.padEnd(16)} failed: ${error?.message ?? error}`);
      continue;
    }
    const { memory, median, best } = result;
    records.push({
      variant: name,
      population,
      messagesPerActor: MESSAGES,
      rounds,
      throughput: { eventsPerSecondMedian: median, eventsPerSecondBest: best, nsPerEvent: 1e9 / best },
      memory,
    });
    console.log(`${name.padEnd(16)} ${(median / 1e6).toFixed(2).padStart(10)} M ${(best / 1e6).toFixed(2).padStart(15)} M ` +
                `${(1e9 / best).toFixed(0).padStart(10)}`);
  }
  if (jsonPath !== undefined) {
    require('node:fs').writeFileSync(jsonPath, `${JSON.stringify({ runtime, records })}\n`);
  }
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
