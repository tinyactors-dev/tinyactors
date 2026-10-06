'use strict';

// Bench (not part of `mise run test`): what core holds per actor for the
// events delivered to it (core ADRs 0069 and 0099), and delivery
// throughput. Runs on Node and Bun.
//
//   node test/bench/delivered_events.js [population]
//
// - idle: core bytes per actor once every actor is stable. An idle actor
//   holds no event, so this is the actor alone.
// - delivered: core bytes per actor, beyond `idle`, when every actor has a
//   delivered event it has not taken yet: the event waits in an envelope of
//   core's pool, its record in the name area. The chart takes `go` and then
//   runs a chain of eventless transitions. With resumeBudget 1,
//   resumesPerTurn 1 and a messageBudget of the whole population, one pump
//   turn delivers `go` to every actor and resumes only one, so every other
//   actor's event waits from delivery until its macrostep ends.
// - throughput: `ping` posted to every actor and flushed, default options,
//   the median and best of five rounds.

const { createSystem, statechart } = require('../..');

const population = Number(process.argv[2] ?? 200000);
const turn = () => new Promise((resolve) => setImmediate(resolve));

function chainChart() {
  let chart = statechart({ family: 'bench-chain', revision: 'v1' })
    .state('idle', (s) => { s.on('go', (t) => t.target('s1')); });
  for (let at = 1; at <= 8; at += 1) {
    const next = at === 8 ? 'idle' : `s${at + 1}`;
    chart = chart.state(`s${at}`, (s) => { s.always((t) => t.target(next)); });
  }
  return chart;
}

function pingChart() {
  return statechart({ family: 'bench-ping', revision: 'v1' })
    .state('idle', (s) => { s.on('ping', (t) => t.target('idle')); });
}

async function memory() {
  const system = createSystem({
    parameters: { resumeBudget: 1, resumesPerTurn: 1, messageBudget: population },
  });
  const definition = system.define(chainChart());
  const actors = [];
  for (let at = 0; at < population; at += 1) actors.push(system.spawn(definition));
  await system.flush();
  const idle = system.stats().memory.core.bytesInUse;
  for (const actor of actors) system.post(actor, 'go');
  await turn();
  const stats = system.stats();
  const delivered = stats.memory.core.bytesInUse;
  const messages = stats.messages.delivered;
  await system.flush();
  system.close();
  return {
    idle: idle / population,
    delivered: (delivered - idle) / population,
    messages,
  };
}

async function throughput() {
  const system = createSystem();
  const definition = system.define(pingChart());
  const actors = [];
  for (let at = 0; at < population; at += 1) actors.push(system.spawn(definition));
  await system.flush();
  const rates = [];
  for (let round = 0; round < 5; round += 1) {
    const started = process.hrtime.bigint();
    for (const actor of actors) system.post(actor, 'ping');
    await system.flush();
    const took = Number(process.hrtime.bigint() - started) / 1e9;
    rates.push(population / took);
  }
  system.close();
  rates.sort((a, b) => a - b);
  return { median: rates[Math.floor(rates.length / 2)], best: rates[rates.length - 1] };
}

(async () => {
  const m = await memory();
  console.log(`delivered_events population ${population}: core bytes per actor idle ${m.idle.toFixed(1)}, ` +
    `+${m.delivered.toFixed(1)} with a delivered event not yet taken (${m.messages} delivered)`);
  const t = await throughput();
  console.log(`delivered_events throughput ${population}: median ${(t.median / 1e6).toFixed(2)} M events/s, ` +
    `best ${(t.best / 1e6).toFixed(2)} M events/s`);
})().catch((error) => { console.error(error); process.exit(1); });
