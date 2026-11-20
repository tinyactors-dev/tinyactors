import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { compileCharts, Host, SimulatedClock } from '@tinyactors/node';

const compiled = compileCharts([{ name: 'payment.scxml', text: readFileSync('payment.scxml', 'utf8') }]);

for (let seed = 1; seed <= 1000; seed++) {
  const random = mulberry32(seed);
  const clock = new SimulatedClock();
  const charges = [];
  let outcome;
  const host = new Host(compiled, {
    system: 'simulation',
    clock,
    ioProcessors: {
      'urn:payments': {
        receive(send) {
          charges.push(clock.now);
          if (random() < 0.4) send.answer('charged'); // the service answers, or it never does
        },
      },
    },
    onDone: (actor, donedata) => { outcome = donedata.outcome; },
  });
  host.spawn('payment');
  await host.idle();
  while (outcome === undefined) {
    clock.increment(host.nextDeadline); // jump to the next timeout: no waiting
    await host.idle();
  }
  assert.ok(charges.length <= 3, `seed ${seed}: charged ${charges.length} times`);
  assert.ok(outcome === 'paid' || charges.length === 3, `seed ${seed}: gave up early`);
  host.close();
}
console.log('1000 seeds: never more than three charges, never giving up early');

function mulberry32(seed) {
  return () => {
    seed = (seed + 0x6d2b79f5) | 0;
    let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}
