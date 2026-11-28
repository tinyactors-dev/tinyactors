import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { compileCharts, Host, SimulatedClock } from '@tinyactors/node';

const compiled = compileCharts([{ name: 'payment.scxml', text: readFileSync('payment.scxml', 'utf8') }]);
let paidLate = 0;
let answeredAfterGivingUp = 0;

for (let seed = 1; seed <= 1000; seed++) {
  const random = mulberry32(seed);
  const clock = new SimulatedClock();
  const requests = [];
  const late = []; // answers the provider sends 45 seconds after the charge
  let outcome;
  const host = new Host(compiled, {
    system: 'simulation',
    clock,
    ioProcessors: {
      'urn:payments': {
        receive(send) {
          requests.push(send.data.key);
          const roll = random();
          if (roll < 0.3) send.answer('charged'); // at once
          else if (roll < 0.6) late.push({ at: clock.now + 45_000, actor: send.sender }); // after the next charge
        }, // otherwise it never answers
      },
    },
    onDone: (actor, donedata) => { outcome = donedata.outcome; },
  });
  host.spawn('payment');
  await host.idle();
  while (outcome === undefined || late.length > 0) {
    const nextLate = late.length > 0 ? Math.min(...late.map((answer) => answer.at)) - clock.now : Infinity;
    clock.increment(Math.min(host.nextDeadline ?? Infinity, nextLate)); // jump to what comes next: no waiting
    for (const answer of late.filter((answer) => answer.at <= clock.now)) {
      late.splice(late.indexOf(answer), 1);
      if (outcome === undefined) { answer.actor.send('charged'); paidLate++; }
      else if (outcome === 'failed') answeredAfterGivingUp++;
    }
    await host.idle();
  }
  assert.ok(requests.length <= 3, `seed ${seed}: charged ${requests.length} times`);
  assert.equal(new Set(requests).size, 1, `seed ${seed}: the attempts carry different keys`);
  assert.ok(outcome === 'paid' || requests.length === 3, `seed ${seed}: gave up early`);
  host.close();
}
console.log('1000 seeds: never more than three charges, one key for all of them, never giving up early');
console.log(`${paidLate} paid by an answer that came after the next charge`);
console.log(`${answeredAfterGivingUp} answered after the chart gave up: reconcile those with the provider`);

function mulberry32(seed) {
  return () => {
    seed = (seed + 0x6d2b79f5) | 0;
    let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}
