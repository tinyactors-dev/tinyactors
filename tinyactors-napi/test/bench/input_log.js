'use strict';

// Bench (not part of `mise run test`): what a logged input costs (napi ADR
// 0034, kernel ADR 0002). Runs on Node and Bun.
//
//   node --expose-gc test/bench/input_log.js [actors] [--variants=a,b,...]
//        [--exclusive] [--json=<file>] [--list]
//
// The options are test/bench/tracing.js's, and `mise run bench` and `mise
// run bench-compare` run both scripts, each variant in a process of its own.
// The first variant, `unlogged`, is the baseline and always runs unless
// --exclusive is given.
//
// ADR 0034's shape: named actors (default 1,000) of one family, and each
// round posts 50 `inc` messages to every actor and flushes once; five
// rounds, reporting the median and best events per second:
// - unlogged: no input log, each message with data `{ by: 1 }`;
// - logged: an input log for the family, whose sink counts the bytes of each
//   batch and drops them, each message with data `{ by: 1 }`; so each
//   message writes an `accepted` record with its value and a `delivered`
//   record (layout version 2);
// - logged without data: the same, with no data, so no value is written.
// Memory is measured after the actors are spawned and a GC ran, as in
// tracing.js, and `memory.logBytesPerEvent` is the bytes the sink received
// per message over the timed rounds.

const { createSystem, statechart } = require('../..');

const args = process.argv.slice(2);
const population = Number(args.find((arg) => !arg.startsWith('--')) ?? 1000);
const exclusive = args.includes('--exclusive');
const jsonPath = args.find((arg) => arg.startsWith('--json='))?.slice('--json='.length);
const only = args.find((arg) => arg.startsWith('--variants='))?.slice('--variants='.length).split(',');
const rounds = 5;
const MESSAGES = 50;
const FAMILY = 'bench-logged';
let bytesOut = 0;

const chart = () => statechart({ family: FAMILY, revision: 'v1' })
  .data('count', 0)
  .state('idle', (s) => { s.on('inc', (t) => t.assign('count', function () { return this.count + 1; })); });

const collect = globalThis.gc ?? (typeof Bun !== 'undefined' ? () => Bun.gc(true) : undefined);

function settle() {
  collect?.();
  const { rss, heapUsed } = process.memoryUsage();
  return { rss, heapUsed };
}

const sink = { commit(batch) { for (const entry of batch.entries) bytesOut += entry.bytes.length; } };

async function measure({ logged, data }) {
  const before = settle();
  const system = createSystem();
  if (logged) system.setInputLog(sink, { families: [FAMILY] });
  const definition = system.define(chart());
  const actors = [];
  for (let at = 0; at < population; at += 1) actors.push(system.spawn(definition, { name: `a${at}` }));
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
  bytesOut = 0;
  const events = population * MESSAGES;
  const rates = [];
  for (let round = 0; round < rounds; round += 1) {
    const start = process.hrtime.bigint();
    for (let message = 0; message < MESSAGES; message += 1) {
      for (const actor of actors) system.post(actor, 'inc', data === undefined ? undefined : { ...data });
    }
    await system.flush();
    const seconds = Number(process.hrtime.bigint() - start) / 1e9;
    rates.push(events / seconds);
  }
  memory.allocatorCallsPerEvent = (system.stats().allocatorCalls - callsBefore) / (events * rounds);
  memory.logBytesPerEvent = logged ? bytesOut / (events * rounds) : null;
  system.close();
  memory.peakRSSBytes = process.resourceUsage().maxRSS * 1024;
  rates.sort((a, b) => a - b);
  return { memory, median: rates[Math.floor(rounds / 2)], best: rates.at(-1) };
}

const variants = {
  unlogged: { logged: false, data: { by: 1 } },
  logged: { logged: true, data: { by: 1 } },
  'logged without data': { logged: true },
};

if (args.includes('--list')) {
  console.log(JSON.stringify(Object.keys(variants)));
  process.exit(0);
}

(async () => {
  const runtime = typeof Bun === 'undefined' ? `node ${process.version}` : `bun ${Bun.version}`;
  console.log(`${runtime}, ${population} actors x ${MESSAGES} messages, ${rounds} rounds`);
  console.log('variant               events/s (median)   events/s (best)   ns/event   log bytes/event');
  const records = [];
  for (const [name, variant] of Object.entries(variants)) {
    if (only && (exclusive || name !== 'unlogged') && !only.includes(name)) {
      continue;
    }
    let result;
    try {
      result = await measure(variant);
    } catch (error) {
      // An older build (bench-compare) may lack what a variant uses.
      records.push({ variant: name, population, rounds, error: String(error?.message ?? error) });
      console.log(`${name.padEnd(21)} failed: ${error?.message ?? error}`);
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
    console.log(`${name.padEnd(21)} ${(median / 1e6).toFixed(2).padStart(10)} M ${(best / 1e6).toFixed(2).padStart(15)} M ` +
                `${(1e9 / best).toFixed(0).padStart(10)} ${(memory.logBytesPerEvent ?? 0).toFixed(0).padStart(17)}`);
  }
  if (jsonPath !== undefined) {
    require('node:fs').writeFileSync(jsonPath, `${JSON.stringify({ runtime, records })}\n`);
  }
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
