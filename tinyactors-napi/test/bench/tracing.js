'use strict';

// Bench (not part of `mise run test`): what tracing costs (core ADR 0067,
// napi ADRs 0016 and 0017). Runs on Node and Bun.
//
//   node test/bench/tracing.js [population] [--check] [--variants=a,b,...]
//
// --check fails (exit 1) when a budget of napi ADR 0017 section 2 is
// exceeded: a traceparent on an untraced system, and a sampled-out
// macrostep with a sink installed, each cost at most 40 ns per event more
// than untraced. `mise run bench-check` runs it.
//
// Posts `ping` to every actor and flushes, five rounds, and reports the
// median and best events per second, and core's bytes per idle actor:
// - untraced: no sink, so core runs with its null sink and builds nothing;
// - traceparent: untraced, but every post carries a sampled traceparent,
//   which core parses (core ADR 0073);
// - sampled out: a sink that wants every kind, and every post carries an
//   unsampled traceparent, so core builds nothing;
// - macrosteps: a sink that wants macrostepBegin and macrostepEnd only;
// - every record: a sink that wants every kind; the addon encodes the
//   spans as OTLP protobuf and hands over the bytes, which are dropped:
//   what an exporter pays before it posts them;
// - decoded: every record, and decodeTraces() of each batch, which is what
//   a user who wants span objects in JavaScript pays.
// The configurations a deployment would choose (napi ADR 0017 section 3),
// with no traceparent, so the system starts every trace itself:
// - summary: the default detail (a span per macrostep, lifecycle, failures);
// - decisions: adds microsteps, conditions and raised events;
// - summary 10% and summary 1%: the default detail, sampled at that ratio;
// - full + values: every record, with event data and values as attributes;
// - watch N: nothing sampled, and N actors watched in full (by session;
//   watchTraces takes at most 1024), which is what turning tracing on for a
//   few actors at 3am costs. WATCH (default 1000) sets N;
// - summary, no logs: the default detail with the log output switched off.
// For comparison, napi ADR 0016 measured before it (every record): plain
// record objects 0.19M events/s, objects mapped to spans 0.13M, and the
// prototype encoder 0.83M.
// The chart's transition assigns, so every macrostep writes BEGIN,
// MICROSTEP, ASSIGN and END.

const { createSystem, statechart, decodeTraces } = require('../..');

const args = process.argv.slice(2);
const check = args.includes('--check');
const population = Number(args.find((arg) => !arg.startsWith('--')) ?? 100000);
const WATCH = Number(process.env.WATCH ?? 1000);
const only = args.find((arg) => arg.startsWith('--variants='))?.slice('--variants='.length).split(',');
const BUDGET_NS = 40;
const SAMPLED = '00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01';
const UNSAMPLED = '00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-00';
const rounds = 5;
let bytesOut = 0;

function chart() {
  return statechart({ family: 'bench-traced', revision: 'v1' })
    .data('count', 0)
    .state('idle', (s) => { s.on('ping', (t) => t.assign('count', function () { return this.count + 1; })); });
}

async function measure({ install, traceparent, watch }) {
  const system = createSystem();
  install?.(system);
  const definition = system.define(chart());
  const actors = [];
  for (let at = 0; at < population; at += 1) actors.push(system.spawn(definition));
  await system.flush();
  watch?.(system, actors);
  const idle = system.stats().memory.core.bytesInUse / population;
  const rates = [];
  for (let round = 0; round < rounds; round += 1) {
    const start = process.hrtime.bigint();
    const options = traceparent === undefined ? undefined : { traceparent };
    for (const actor of actors) system.post(actor, 'ping', undefined, options);
    await system.flush();
    const seconds = Number(process.hrtime.bigint() - start) / 1e9;
    rates.push(population / seconds);
  }
  system.close();
  rates.sort((a, b) => a - b);
  return { idle, median: rates[Math.floor(rounds / 2)], best: rates.at(-1) };
}

const variants = {
  untraced: {},
  traceparent: { traceparent: SAMPLED },
  'sampled out': { install: (system) => system.setTraceSink(() => {}), traceparent: UNSAMPLED },
  macrosteps: { install: (system) => system.setTraceSink(() => {}, { kinds: ['macrostepBegin', 'macrostepEnd'] }) },
  'every record': { install: (system) => system.setTraceSink((bytes) => { bytesOut += bytes.length; }, { detail: 'full' }) },
  decoded: { install: (system) => system.setTraceSink((bytes) => { decodeTraces(bytes); }, { detail: 'full' }) },
  summary: { install: (system) => system.setTraceSink(count) },
  decisions: { install: (system) => system.setTraceSink(count, { detail: 'decisions' }) },
  'summary 10%': { install: (system) => system.setTraceSink(count, { sample: 0.1 }) },
  'summary 1%': { install: (system) => system.setTraceSink(count, { sample: 0.01 }) },
  'full + values': { install: (system) => system.setTraceSink(count, { detail: 'full', values: true }) },
  [`watch ${WATCH}`]: {
    install: (system) => system.setTraceSink(count, { sample: 0 }),
    watch: (system, actors) =>
      system.watchTraces({ sessions: actors.filter((_, at) => at % Math.ceil(actors.length / WATCH) === 0).slice(0, WATCH) }),
  },
  'summary, no logs': { install: (system) => system.setTraceSink(count, { logs: false }) },
};
function count(traces, logs) {
  bytesOut += traces.length + (logs?.length ?? 0);
}
const budgeted = ['traceparent', 'sampled out'];

(async () => {
  const runtime = typeof Bun === 'undefined' ? `node ${process.version}` : `bun ${Bun.version}`;
  console.log(`${runtime}, ${population} actors, ${rounds} rounds`);
  console.log('variant          events/s (median)   events/s (best)   ns/event   core bytes/actor');
  const ns = {};
  for (const [name, variant] of Object.entries(variants)) {
    if (check && name !== 'untraced' && !budgeted.includes(name)) {
      continue;
    }
    if (only && name !== 'untraced' && !only.includes(name)) {
      continue;
    }
    bytesOut = 0;
    const { idle, median, best } = await measure(variant);
    ns[name] = 1e9 / best;
    if (bytesOut !== 0) {
      console.log(`  OTLP bytes per event: ${(bytesOut / (population * (rounds + 1))).toFixed(0)}`);
    }
    console.log(`${name.padEnd(16)} ${(median / 1e6).toFixed(2).padStart(10)} M ${(best / 1e6).toFixed(2).padStart(15)} M ` +
                `${ns[name].toFixed(0).padStart(10)} ${idle.toFixed(0).padStart(18)}`);
  }
  // Budgets compare the best rounds: noise only ever adds time.
  let over = false;
  for (const name of budgeted.filter((name) => ns[name] !== undefined)) {
    const extra = ns[name] - ns.untraced;
    const verdict = extra <= BUDGET_NS ? 'within' : 'OVER';
    over ||= extra > BUDGET_NS;
    console.log(`budget: ${name} costs ${extra.toFixed(0)} ns/event over untraced (${verdict} ${BUDGET_NS} ns)`);
  }
  if (check && over) {
    process.exitCode = 1;
  }
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
