// Atlas in Node.js: reads atlas.json (the core's types and call graph,
// written from the debug information of a build) and prints what the page
// draws first: the counts per layer, ta_actor's layout on cache lines, and
// what ta_host_run calls. Run: node main.mjs [atlas.json]
import { readFileSync, existsSync } from 'node:fs';
import { load, layerCounts, layoutRows, paddingBytes, neighborhood, crossLayerCalls, costOfType } from './atlas.mjs';

const path = process.argv[2] ?? new URL('./atlas.json', import.meta.url).pathname;
if (!existsSync(path)) {
  console.log(`no atlas at ${path}`);
  process.exit(0);
}
const atlas = load(JSON.parse(readFileSync(path, 'utf8')));

console.log(`${atlas.types.length} types, ${atlas.functions.length} functions`);
for (const { layer, types, functions } of layerCounts(atlas)) console.log(`  ${layer}: ${types} types, ${functions} functions`);

const actor = atlas.typeByName.get('ta_actor');
if (actor) {
  console.log(`ta_actor: ${actor.size} bytes, ${paddingBytes(atlas, actor)} of padding`);
  for (const row of layoutRows(atlas, actor)) {
    const pieces = row.pieces.map(({ span, start, end }) => `${span.kind === 'padding' ? '(padding)' : span.name}@${row.offset + start}+${end - start}`);
    console.log(`  line ${row.line}: ${pieces.join(' ')}`);
  }
  const cost = costOfType(atlas, actor);
  if (cost) console.log(`  an ${cost.name}: ${cost.total} bytes (${cost.parts.map(({ part, bytes }) => `${part} ${bytes}`).join(', ')})`);
}

const run = atlas.functionByName.get('ta_host_run');
if (run) {
  const { callers, callees } = neighborhood(atlas, run);
  console.log(`ta_host_run: ${run.size ?? 'no'} bytes, ${callers.length} callers, ${callees.length} callees`);
  for (const { fn, inlined, direction } of callees) console.log(`  → ${fn.name} (${fn.layer}${inlined ? ', inlined' : ''}${direction === 'up' ? ', up' : ''})`);
}

const up = crossLayerCalls(atlas).filter((edge) => edge.direction === 'up');
console.log(`calls up a layer: ${up.length ? up.map(({ from, to, count }) => `${from} → ${to} ${count}`).join(', ') : 'none'}`);
