// The lab in Node.js: every scenario on 2,000 actors and both datamodels,
// five units of work each (fifty for the timers: five simulated seconds),
// seeded, so every run prints the same counts.
// The website runs one scenario at a time in a Worker, as many actors as
// the reader asks for, and charts its rates. Run: node main.mjs
import { readFileSync } from 'node:fs';
import * as tinyactors from '@tinyactors/wasm';
import { createLab, SCENARIOS } from './lab.mjs';

const chart = (name) => readFileSync(new URL(`./${name}.scxml`, import.meta.url), 'utf8');
const charts = { counter: chart('counter'), player: chart('player'), timer: chart('timer') };

for (const datamodel of ['native', 'quickjs']) {
  for (const scenario of SCENARIOS) {
    const lab = createLab(tinyactors, { scenario, actors: 2000, datamodel, charts, resident: 100, seed: 7 });
    await lab.populate();
    for (let unit = 0; unit < (scenario === 'timers' ? 50 : 5); unit++) await lab.work();
    const s = lab.stats();
    let line = `${datamodel} ${scenario}: ${s.actors} actors, ${s.messages} messages`;
    if (scenario === 'events') {
      // Every add was counted by its counter.
      let sum = 0;
      for (let i = 0; i < s.actors; i++) sum += (await lab.sample(i)).variables.count;
      line += `, counted ${sum}`;
    }
    if (scenario === 'rally') line += `, ${s.points} rallies finished`;
    if (scenario === 'timers') {
      // Every tick the lab counted, a timer counted too.
      let sum = 0;
      for (let i = 0; i < s.actors; i++) sum += (await lab.sample(i)).variables.ticks;
      line = `${datamodel} ${scenario}: ${s.actors} actors, ${s.messages} ticks in ${s.simulated} simulated seconds, counted ${sum}`;
    }
    if (scenario === 'sleep') line += `, ${s.stored} asleep in the store, ${s.wakes} woken`;
    console.log(line);
    lab.close();
  }
}
