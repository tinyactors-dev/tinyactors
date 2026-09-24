'use strict';

// Bench (not part of `mise run test`): what tracing costs (core ADR 0067,
// ADR 0015). Runs on Node and Bun.
//
//   node test/bench/tracing.js [population]
//
// Posts `ping` to every actor and flushes, five rounds, and reports the
// median and best events per second, and core's bytes per idle actor:
// - untraced: no sink, so core runs with its null sink and builds nothing;
// - macrosteps: a sink that wants macrostepBegin and macrostepEnd only;
// - every record: a sink that wants every kind and drops the batches;
// - spans: every record, mapped by TraceSpans into span objects that are
//   dropped, which is what an exporter pays before it exports.
// The chart's transition assigns, so every macrostep writes BEGIN,
// MICROSTEP, ASSIGN and END.

const { createSystem, statechart, TraceSpans } = require('../..');

const population = Number(process.argv[2] ?? 100000);
const rounds = 5;

function chart() {
  return statechart({ family: 'bench-traced', revision: 'v1' })
    .data('count', 0)
    .state('idle', (s) => { s.on('ping', (t) => t.assign('count', function () { return this.count + 1; })); });
}

async function measure(install) {
  const system = createSystem();
  install?.(system);
  const definition = system.define(chart());
  const actors = [];
  for (let at = 0; at < population; at += 1) actors.push(system.spawn(definition));
  await system.flush();
  const idle = system.stats().memory.core.bytesInUse / population;
  const rates = [];
  for (let round = 0; round < rounds; round += 1) {
    const start = process.hrtime.bigint();
    for (const actor of actors) system.post(actor, 'ping');
    await system.flush();
    const seconds = Number(process.hrtime.bigint() - start) / 1e9;
    rates.push(population / seconds);
  }
  system.close();
  rates.sort((a, b) => a - b);
  return { idle, median: rates[Math.floor(rounds / 2)], best: rates.at(-1) };
}

const variants = {
  untraced: undefined,
  macrosteps: (system) => system.setTraceSink(() => {}, { kinds: ['macrostepBegin', 'macrostepEnd'] }),
  'every record': (system) => system.setTraceSink(() => {}),
  spans: (system) => {
    const spans = new TraceSpans({ onSpan: () => {} });
    system.setTraceSink((records) => spans.write(records));
  },
};

(async () => {
  const runtime = typeof Bun === 'undefined' ? `node ${process.version}` : `bun ${Bun.version}`;
  console.log(`${runtime}, ${population} actors, ${rounds} rounds`);
  console.log('variant          events/s (median)   events/s (best)   core bytes/actor');
  for (const [name, install] of Object.entries(variants)) {
    const { idle, median, best } = await measure(install);
    console.log(`${name.padEnd(16)} ${(median / 1e6).toFixed(2).padStart(10)} M ${(best / 1e6).toFixed(2).padStart(15)} M ${idle.toFixed(0).padStart(18)}`);
  }
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
