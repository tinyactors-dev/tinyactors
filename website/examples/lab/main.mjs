// The lab in Node.js, seeded, so every run prints the same counts. First
// every scenario on 2,000 actors, steady, on both datamodels, five units of
// work each (fifty for the timers: five simulated seconds), checked against
// what the actors counted themselves. Then each pattern for two minutes of
// pattern time, with timers falling due, printing the population at every
// tenth second. The website runs one scenario at a time in a Worker, on
// the wall clock, and charts it. Run: node main.mjs
import { readFileSync } from 'node:fs';
import * as tinyactors from '@tinyactors/wasm';
import { createLab, SCENARIOS, PATTERNS, target } from './lab.mjs';

const chart = (name) => readFileSync(new URL(`./${name}.scxml`, import.meta.url), 'utf8');
const charts = { counter: chart('counter'), player: chart('player'), timer: chart('timer') };
const fill = async (lab, wanted) => { while ((await lab.adjust(wanted)) !== 0); };

for (const datamodel of ['native', 'quickjs']) {
  for (const scenario of SCENARIOS) {
    const lab = createLab(tinyactors, { scenario, actors: 2000, datamodel, charts, resident: 100, seed: 7 });
    await fill(lab, 2000);
    for (let unit = 0; unit < (scenario === 'timers' ? 50 : 5); unit++) await lab.work();
    const s = lab.stats();
    const sum = async (name) => {
      let total = 0;
      for (let i = 0; i < s.actors; i++) total += (await lab.sample(i)).variables[name];
      return total;
    };
    let line = `${datamodel} ${scenario}: ${s.actors} actors, ${s.messages} messages`;
    if (scenario === 'events') line += `, counted ${await sum('count')}`; // every add was counted by its counter
    if (scenario === 'rally') line += `, ${s.points} rallies finished`;
    if (scenario === 'sleep') line += `, ${s.asleep} asleep in the store, ${s.wakes} woken`;
    if (scenario === 'timers') line += ` in ${s.simulated} simulated seconds, counted ${await sum('ticks')}`; // every tick, by its timer
    console.log(line);
    lab.close();
  }
}

for (const pattern of Object.keys(PATTERNS)) {
  const lab = createLab(tinyactors, { scenario: 'timers', actors: 2000, charts, seed: 7 });
  const shown = [];
  for (let t = 0; t < 120; t++) {
    await fill(lab, target(pattern, 2000, t));
    for (let unit = 0; unit < 10; unit++) await lab.work(); // a simulated second of timers
    if (t % 10 === 0) shown.push(lab.stats().actors);
  }
  const s = lab.stats();
  console.log(`${pattern}: ${shown.join(' ')}; ${s.spawned} spawned, ${s.ended} ended, ${s.messages} ticks`);
  lab.close();
}
