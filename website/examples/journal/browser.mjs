import { compileCharts, Host, SimulatedClock } from '@tinyactors/wasm';

const chart = await fetch(new URL('order.scxml', import.meta.url)).then((response) => response.text());
const compiled = compileCharts([{ name: 'order.scxml', text: chart }]);

// Every input goes through record(): the host's time, then the input.
const journal = [];
function apply(host, entry) {
  if (entry.spawn) host.spawn(entry.spawn, { name: entry.name });
  else host.send(entry.send);
}
function record(host, entry) {
  const line = JSON.stringify({ at: host.now(), ...entry });
  journal.push(line); // in production: append to a file or a table first
  apply(host, JSON.parse(line));
}

const order = { target: 'tinyactors://shop/@order-1' };
const clock = new SimulatedClock();
const charged = [];
const live = new Host(compiled, {
  system: 'shop',
  clock,
  ioProcessors: { payments: { receive: (send) => charged.push(send.data.amount) } },
});

record(live, { spawn: 'order', name: 'order-1' });
record(live, { send: { ...order, event: 'add', data: { sku: 'pen' } } });
record(live, { send: { ...order, event: 'add', data: { sku: 'notebook' } } });
record(live, { send: { ...order, event: 'checkout' } });
await live.idle();
clock.increment(30_000); // no answer: the chart charges again
await live.idle();
clock.increment(5_000);
record(live, { send: { ...order, event: 'paid' } }); // the provider's webhook
await live.idle();

const replayClock = new SimulatedClock();
const replay = new Host(compiled, {
  system: 'shop',
  clock: replayClock,
  ioProcessors: { payments: { receive: () => {} } }, // charges nobody
});
for (const line of journal) {
  const entry = JSON.parse(line);
  replayClock.increment(entry.at - replay.now());
  await replay.idle();
  apply(replay, entry);
  await replay.idle();
}

// Go on from the replay on the runtime's clock, with the real processor.
const restored = Host.restore(replay.snapshot(), compiled, {
  system: 'shop',
  ioProcessors: { payments: { receive: (send) => charged.push(send.data.amount) } },
});

for (const [name, host] of [['live', live], ['replay', replay], ['restored', restored]]) {
  const actor = host.actor('order-1');
  const { value } = await actor.state();
  const { variables } = await actor.context();
  console.log(`${name}: ${JSON.stringify(value)} after ${variables.attempts} attempts, ${variables.items.join(', ')}`);
}
console.log(`${journal.length} inputs journaled; charges: ${charged.join(', ')}`);
for (const host of [live, replay, restored]) host.close();
compiled.close();
