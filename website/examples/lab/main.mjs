// The lab in Node.js, seeded, so every run prints the same counts. First a
// colony whose light feeds 2,000 cells, for two simulated minutes: the
// cells alive every tenth second, then who was born and how they died.
// Then every other scenario on 2,000 actors on both datamodels, five units
// of work each (fifty for the timers: five simulated seconds), checked
// against what the actors counted themselves. The website runs one
// scenario at a time in a Worker and charts it. Run: node main.mjs
import { readFileSync } from 'node:fs';
import * as tinyactors from '@tinyactors/wasm';
import { createLab, SCENARIOS } from './lab.mjs';

const chart = (name) => readFileSync(new URL(`./${name}.scxml`, import.meta.url), 'utf8');
const charts = Object.fromEntries(['cell', 'counter', 'player', 'timer'].map((name) => [name, chart(name)]));
const populate = async (lab) => { while ((await lab.populate()) !== 0); };

for (const datamodel of ['native', 'quickjs']) {
  const lab = createLab(tinyactors, { scenario: 'cells', actors: 2000, datamodel, charts, seed: 7 });
  await populate(lab);
  const alive = [];
  for (let second = 1; second <= 120; second++) {
    for (let unit = 0; unit < 10; unit++) await lab.work(); // a simulated second
    if (second % 10 === 0) alive.push(lab.stats().actors);
  }
  await lab.refresh();
  const s = lab.stats();
  console.log(`${datamodel} cells, alive every 10 s: ${alive.join(' ')}`);
  console.log(`  ${s.spawned} born, ${s.starved} starved, ${s.aged} died of age, ${s.actors} alive; ${s.messages} events in ${s.simulated} simulated seconds`);
  lab.close();
}

for (const datamodel of ['native', 'quickjs']) {
  for (const scenario of SCENARIOS.filter((name) => name !== 'cells')) {
    const lab = createLab(tinyactors, { scenario, actors: 2000, datamodel, charts, resident: 100, seed: 7 });
    await populate(lab);
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
