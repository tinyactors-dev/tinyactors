// Evaluate staticjs programs through staticjs.wasm in Node.
//
//   node node.mjs staticjs.wasm staticjs.api.json
import { readFileSync } from 'node:fs';
import { load, StaticJSError } from './staticjs.mjs';

const [wasmPath = 'staticjs.wasm', apiPath = 'staticjs.api.json'] = process.argv.slice(2);
const staticjs = await load(readFileSync(wasmPath), JSON.parse(readFileSync(apiPath, 'utf8')));

console.log(staticjs.evaluate('[3, 1, 2].toSorted((a, b) => a - b)'));
console.log(staticjs.evaluate('export const total = [1, 2, 3].reduce((a, b) => a + b, 0);', { module: true }));
console.log(staticjs.evaluate('new Date().toISOString()', { now: Date.UTC(2024, 0, 31, 10) }));
for (const source of ['({ a: 1 }).b.c', '[1, 2', 'xs.map((x) => x * 2)', 'Array.from({ length: 1e6 }, (_, i) => i)']) {
  try {
    staticjs.evaluate(source, { fuel: 10_000 });
  } catch (error) {
    if (!(error instanceof StaticJSError)) throw error;
    console.log(error.message);
  }
}
staticjs.close();
