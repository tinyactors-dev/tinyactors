// The anthill in Node.js: 3,000 ants and two queens for five simulated
// minutes, seeded, so every run prints the same colonies. Run: node main.mjs
import { readFileSync } from 'node:fs';
import * as tinyactors from '@tinyactors/wasm';
import { createAnthill, FRAME } from './world.mjs';

const chart = (name) => readFileSync(new URL(`./${name}.scxml`, import.meta.url), 'utf8');
const anthill = await createAnthill(tinyactors, { charts: { ant: chart('ant'), queen: chart('queen') }, population: 3000, seed: 7 });
await anthill.populate();

for (let minute = 1; minute <= 5; minute++) {
  for (let frame = 0; frame < 60_000 / FRAME; frame++) await anthill.step();
  const { colonies, fights } = anthill.stats();
  const shown = colonies.map((c, i) => `hill ${i + 1}: ${c.workers} workers, ${c.brood} brood, ${c.stored} food home, ${c.born} born, ${c.deaths} died`);
  console.log(`minute ${minute}: ${shown.join('; ')}; ${fights} fights`);
}
anthill.close();
