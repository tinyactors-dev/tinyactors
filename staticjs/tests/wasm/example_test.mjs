// examples/wasm/staticjs.mjs's default limits against the header's
// SJS_LIMITS_DEFAULT (mise run wasm-test): node tests/wasm/example_test.mjs <staticjs.h>
import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';
import { DEFAULT_LIMITS } from '../../examples/wasm/staticjs.mjs';

const header = readFileSync(process.argv[2], 'utf8');
const defaults = header.match(/#define SJS_LIMITS_DEFAULT[\s\S]*?\}\)/)[0];
const limits = Object.fromEntries([...defaults.matchAll(/\.(\w+) = ([\du *]+?)\s*[,}]/g)]
  .map(([, name, value]) => [name, value.replace(/u/g, '').split('*').reduce((a, b) => a * Number(b), 1)]));
assert.deepEqual({ ...DEFAULT_LIMITS }, limits, 'examples/wasm/staticjs.mjs gives the header\'s default limits');
console.log(`examples/wasm/staticjs.mjs: the header's ${Object.keys(limits).length} default limits`);
