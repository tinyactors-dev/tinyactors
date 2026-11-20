// Property testing a chart: a run, properties, a generator, a search and a
// shrinker. The page imports this module in the browser, main.mjs in
// Node.js; both run it on @tinyactors/wasm.
import { compileCharts, decodeTraces, Host, SimulatedClock } from '@tinyactors/wasm';

export function compile(text) {
  return compileCharts([{ name: 'payment.scxml', text }]);
}

// A run: the chart against a world. The world is data: answers[n] says
// whether the payment service answers the chart's charge number n. Nothing
// else decides what happens, so the same answers make the same run.
export async function run(compiled, answers, { trace = false } = {}) {
  const clock = new SimulatedClock();
  const charges = [];
  const requests = [];
  let outcome;
  const host = new Host(compiled, {
    system: 'property',
    clock,
    config: { system: { ticksPerSecond: 1000 }, ...(trace ? { telemetry: { resource: { 'service.name': 'payment' } } } : {}) },
    ...(trace ? { telemetry: (signal, data) => signal === 'traces' && requests.push(data) } : {}),
    ioProcessors: {
      'urn:payments': {
        receive(send) {
          const n = charges.push(clock.now) - 1;
          if (answers[n]) send.answer('charged');
        },
      },
    },
    onDone: (actor, donedata) => { outcome = donedata.outcome; },
  });
  host.spawn('payment');
  await host.idle();
  while (outcome === undefined) {
    clock.increment(host.nextDeadline); // the next timeout, at once
    await host.idle();
  }
  host.close(); // the last traces go to the telemetry handler here
  return { answers, charges, outcome, spans: requests.flatMap((data) => decodeTraces(data)) };
}

// Properties: what must hold for every run, whatever the world did.
export const properties = {
  'never more than three charges': (r) => r.charges.length <= 3,
  'gives up only after three charges': (r) => r.outcome === 'paid' || r.charges.length === 3,
  'paid only when the service answered': (r) => r.outcome === 'failed' || r.answers.slice(0, r.charges.length).some(Boolean),
};

export const broken = (r) => Object.keys(properties).filter((name) => !properties[name](r));

// A generator: a world from a seed, three to eight answers, each yes with
// probability 0.4. The seed reproduces it.
export function generate(seed) {
  const random = mulberry32(seed);
  return Array.from({ length: 3 + Math.floor(random() * 6) }, () => random() < 0.4);
}

// The search: worlds from seeds 1 to `seeds`, until one breaks a property.
export async function search(compiled, seeds) {
  for (let seed = 1; seed <= seeds; seed++) {
    const r = await run(compiled, generate(seed));
    if (broken(r).length) return { seed, run: r };
  }
  return null;
}

// The shrinker: a smaller world that still breaks a property, until no
// smaller one does. Smaller is one answer fewer, or a yes turned into
// silence.
export async function shrink(compiled, answers) {
  let smallest = answers;
  for (let changed = true; changed; ) {
    changed = false;
    for (const candidate of smaller(smallest)) {
      if (broken(await run(compiled, candidate)).length) {
        smallest = candidate;
        changed = true;
        break;
      }
    }
  }
  return smallest;
}

function* smaller(answers) {
  for (let n = 0; n < answers.length; n++) yield answers.toSpliced(n, 1);
  for (let n = 0; n < answers.length; n++) if (answers[n]) yield answers.with(n, false);
}

function mulberry32(seed) {
  return () => {
    seed = (seed + 0x6d2b79f5) | 0;
    let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}
