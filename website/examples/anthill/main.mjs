// The anthill in Node.js: two colonies of 1,500 ants and a queen each for
// five simulated minutes, seeded, so every run prints the same colonies.
// The website runs each colony in a Worker of its own; here they take
// turns in one process and trade the same messages. Run: node main.mjs
import { readFileSync } from 'node:fs';
import * as tinyactors from '@tinyactors/wasm';
import { createColony, FRAME } from './world.mjs';

const chart = (name) => readFileSync(new URL(`./${name}.scxml`, import.meta.url), 'utf8');
const charts = { ant: chart('ant'), queen: chart('queen') };
const colonies = await Promise.all([0, 1].map((colony) => createColony(tinyactors, { charts, population: 1500, colony, seed: 7 })));
for (const colony of colonies) await colony.populate();

let messages = [null, null];
for (let minute = 1; minute <= 5; minute++) {
  for (let frame = 0; frame < 60_000 / FRAME; frame++) {
    messages = await Promise.all(colonies.map((colony, i) => colony.step(messages[1 - i])));
  }
  const stats = colonies.map((colony) => colony.stats());
  const shown = stats.map(({ colony: c }, i) => `hill ${i + 1}: ${c.workers} workers, ${c.brood} brood, ${c.stored} food home, ${c.born} born, ${c.deaths} died`);
  console.log(`minute ${minute}: ${shown.join('; ')}; ${stats[0].fights + stats[1].fights} fights`);
}
colonies.forEach((colony) => colony.close());
