// The presence ants on a simulated clock: four tabs from three countries
// and one unknown join the field, roam for a minute, one greets another,
// one hides its tab (its actor hibernates) and comes back, and one leaves.
// What the world tells the tabs is counted and the story printed, so
// `mise run test` checks the world (website/presence/world.mjs) and the
// chart (visitor.scxml) against output.txt.
import { readFileSync } from 'node:fs';
import * as tinyactors from '@tinyactors/node';
import { createWorld } from '../../presence/world.mjs';

const chart = readFileSync(new URL('../../presence/visitor.scxml', import.meta.url), 'utf8');
const random = mulberry32(7);
const clock = new tinyactors.SimulatedClock();
const counts = {};
const lines = [];
const world = createWorld(tinyactors, {
  chart, clock, random,
  emit(to, message) {
    counts[message.t] = (counts[message.t] ?? 0) + 1;
    if (message.t === 'greeted') lines.push(`  tab of ant ${to} reads: an ant from ${message.country} greets you`);
    if (message.t === 'greet') lines.push(`  ants ${message.a} and ${message.b} touch antennae`);
    if (message.t === 'leave') lines.push(`  ant ${message.h} walked off`);
  },
});

async function run(seconds) {
  for (let i = 0; i < seconds * 10; i++) { clock.increment(100); await world.tick(); }
}

const ee = world.join('tab-ee', 'EE');
const de = world.join('tab-de', 'DE');
const us = world.join('tab-us', 'US');
const xx = world.join('tab-xx', 'XX');
await run(1);
console.log(`joined: ${world.stats().ants} ants, handles ${[ee, de, us, xx].join(', ')}`);

await run(30);
const s = world.stats();
console.log(`after 30 s: ${s.countries} countries have a trail, the food went from 200 to ${s.food}`);

console.log(`ant ${ee} is tapped to greet ant ${de}:`);
world.greet(ee, de);
await run(25);
for (const line of lines.splice(0)) console.log(line);

world.hide(us);
await run(2);
console.log(`tab of ant ${us} hidden: ${world.stats().asleep} asleep, hibernated actors: ${await hibernated()}`);
world.show(us);
await run(2);
console.log(`tab of ant ${us} shown: ${world.stats().asleep} asleep, hibernated actors: ${await hibernated()}, roaming again: ${await isRoaming('tab-us')}`);

console.log(`a reload finds tab-de's ant again: ${world.find('tab-de') === de}`);
world.leave(xx);
await run(1);
for (const line of lines.splice(0)) console.log(line);
console.log(`left: ${world.stats().ants} ants`);

const trips = counts.trip ?? 0;
console.log(`messages to the tabs: ${trips} trips, ${counts.join} joins, ${counts.greet} greeting, ${counts.greeted} greeted line, ${counts.sleep} sleep changes, ${counts.leave} leave`);
world.close();

async function hibernated() {
  return (await world.host.hostStats()).host.hibernated;
}
async function isRoaming(tab) {
  const state = await world.host.actor(tab).state(); // its active states
  return JSON.stringify(state).includes('"roaming"');
}
function mulberry32(seed) {
  return () => {
    seed = (seed + 0x6d2b79f5) | 0;
    let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}
